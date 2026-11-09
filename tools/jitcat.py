"""Group the samples in recompiled code by what the instructions are for.

Usage: jitcat.py RUN_DIR [--comm NAME]
RUN_DIR needs perf.data, jitmap.txt and the recompiler/ dump written with --jitmap.
Fixed registers of the AArch64 backend: x29 PPC context, x28 guest memory base, x27 jump table,
x25/x26 temporaries, x24 remaining cycles (since the cycle counter moved into a register).
"""
import argparse
import bisect
import collections
import os
import re

import capstone

import jitprof


def category(mnemonic, operands, previous):
    if '#0x2b0]' in operands or 'w24' in operands:
        return 'cycle counter'
    if mnemonic in ('br', 'blr', 'ret'):
        return 'indirect branch'
    if '[x29' in operands:
        return 'ppc context load/store'
    if '[x28' in operands:
        return 'guest memory load/store'
    if '[x27' in operands:
        return 'jump table load'
    if mnemonic in ('mov', 'movk', 'movz', 'movn') and re.match(r'[wx]2[56],', operands) and '#' in operands:
        return 'constants for branches and calls'
    if mnemonic in ('rev', 'rev16', 'rev32', 'rev64'):
        # usually waiting for the guest memory load right before
        return 'byte swap (incl. waiting for loads)'
    if mnemonic in ('fmov', 'fcvt'):
        return 'float conversion'
    if mnemonic.startswith('f'):
        return 'float arithmetic'
    if mnemonic in ('cmp', 'cset', 'tbnz', 'tbz', 'cbz', 'cbnz', 'b') or mnemonic.startswith('b.'):
        return 'compare and branch'
    if mnemonic in ('mov',) and '#' not in operands:
        return 'register moves'
    if mnemonic == 'nop':
        return 'nop'
    return 'integer arithmetic'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run_dir')
    ap.add_argument('--comm', default='OSSched[core=1]')
    args = ap.parse_args()

    starts, funcs = jitprof.load_jitmap(os.path.join(args.run_dir, 'jitmap.txt'))
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    cache = {}

    def instructions(name):
        if name not in cache:
            path = os.path.join(args.run_dir, 'recompiler', f'{name}.bin')
            code = open(path, 'rb').read() if os.path.exists(path) else b''
            cache[name] = {i.address: (i.mnemonic, i.op_str) for i in md.disasm(code, 0)}
        return cache[name]

    per_category = collections.Counter()
    total_thread = 0
    total_jit = 0
    for comms, s in jitprof.samples(os.path.join(args.run_dir, 'perf.data')):
        if comms.get(s.get('tid')) != args.comm:
            continue
        total_thread += s['period']
        i = bisect.bisect_right(starts, s['ip']) - 1
        if i < 0:
            continue
        size, name = funcs[i]
        offset = s['ip'] - starts[i]
        if offset >= size:
            continue
        ins = instructions(name)
        current = ins.get(offset)
        if current is None:
            continue
        total_jit += s['period']
        per_category[category(*current, ins.get(offset - 4, ('', '')))] += s['period']

    print(f'{args.comm}: {100 * total_jit / max(total_thread, 1):.1f} % of the thread in recompiled code')
    for name, period in per_category.most_common():
        print(f'{100 * period / total_jit:5.1f} %  {name}')


if __name__ == '__main__':
    main()
