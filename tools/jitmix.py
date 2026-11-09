"""Instruction mix of the sampled recompiled code.

Usage: jitmix.py RUN_DIR [--comm NAME]
RUN_DIR needs perf.data, jitmap.txt and the recompiler/ dump written with --jitmap.
Prints which host instructions the samples hit and the static mix of the sampled functions, weighted by samples.
"""
import argparse
import bisect
import collections
import os

import capstone

import jitprof

# fixed registers of the AArch64 backend
BASES = {'x29': 'ppc context', 'x28': 'guest memory', 'x27': 'jump table', 'sp': 'host stack'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run_dir')
    ap.add_argument('--comm', default='OSSched[core=1]')
    args = ap.parse_args()

    starts, funcs = jitprof.load_jitmap(os.path.join(args.run_dir, 'jitmap.txt'))
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    code_cache = {}

    def instructions(name):
        if name not in code_cache:
            path = os.path.join(args.run_dir, 'recompiler', f'{name}.bin')
            code = open(path, 'rb').read() if os.path.exists(path) else b''
            code_cache[name] = {i.address: (i.mnemonic, i.op_str) for i in md.disasm(code, 0)}
        return code_cache[name]

    hit = collections.Counter()
    memory = collections.Counter()
    func_weight = collections.Counter()
    total = 0
    for comms, s in jitprof.samples(os.path.join(args.run_dir, 'perf.data')):
        if comms.get(s.get('tid')) != args.comm:
            continue
        i = bisect.bisect_right(starts, s['ip']) - 1
        if i < 0:
            continue
        size, name = funcs[i]
        offset = s['ip'] - starts[i]
        if offset >= size:
            continue
        ins = instructions(name).get(offset)
        if ins is None:
            continue
        mnemonic, operands = ins
        hit[mnemonic] += s['period']
        if mnemonic.startswith(('ldr', 'str', 'ldur', 'stur')):
            memory[(mnemonic.rstrip('bh'), BASES.get(operands.split('[')[-1].split(',')[0].rstrip(']'), 'other'))] += s['period']
        func_weight[name] += s['period']
        total += s['period']

    print(f'samples on instructions ({total} cycles):')
    for m, p in hit.most_common(25):
        print(f'  {100 * p / total:5.1f} %  {m}')

    print('loads and stores by base register:')
    for (m, base), p in memory.most_common(12):
        print(f'  {100 * p / total:5.1f} %  {m} {base}')

    # static mix of the sampled functions, each weighted by its share of samples
    mix = collections.Counter()
    for name, weight in func_weight.items():
        ins = instructions(name)
        if not ins:
            continue
        per_ins = weight / len(ins)
        for m, _ in ins.values():
            if m != 'udf':  # code buffers are padded
                mix[m] += per_ins
    mix_total = sum(mix.values())
    print('static mix of the sampled functions:')
    for m, p in mix.most_common(25):
        print(f'  {100 * p / mix_total:5.1f} %  {m}')


if __name__ == '__main__':
    main()
