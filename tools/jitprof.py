"""Attribute simpleperf samples in recompiled code to guest functions.

Usage: jitprof.py PERF_DATA JITMAP [--comm NAME] [--top N]
JITMAP is the jitmap.txt Cemu writes when jitmap.enable exists in its user data folder.
Prints the hottest guest functions of one thread and, per function, the hottest host code offsets.
"""
import argparse
import bisect
import collections
import os
import re
import subprocess

SIMPLEPERF = os.path.expanduser('~/Library/Android/sdk/ndk/28.2.13676358/simpleperf/bin/darwin/x86_64/simpleperf')


def load_jitmap(path):
    # functions can be recompiled into memory that held another function before, later entries win
    entries = {}
    for line in open(path):
        start, size, name = line.split()
        entries[int(start, 16)] = (int(size, 16), name)
    starts = sorted(entries)
    return starts, [entries[s] for s in starts]


def samples(perf_data):
    proc = subprocess.Popen([SIMPLEPERF, 'dump', perf_data], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    comms = {}
    sample = None
    for line in proc.stdout:
        if line.startswith('record comm'):
            sample = None
            continue
        if line.startswith('record sample'):
            sample = {}
            continue
        if line.startswith('record '):
            sample = None
            continue
        m = re.match(r'\s+pid (\d+), tid (\d+)', line)
        if m and sample is not None:
            sample['tid'] = int(m.group(2))
            continue
        m = re.match(r'\s+ip (0x[0-9a-f]+)', line)
        if m and sample is not None:
            sample['ip'] = int(m.group(1), 16)
            continue
        m = re.match(r'\s+period (\d+)', line)
        if m and sample is not None:
            sample['period'] = int(m.group(1))
            yield comms, sample
            sample = None
            continue
        m = re.match(r'\s+pid (\d+), tid (\d+), comm (.+)', line)
        if m:
            comms[int(m.group(2))] = m.group(3).strip()
    proc.wait()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('perf_data')
    ap.add_argument('jitmap')
    ap.add_argument('--comm', default='OSSched[core=1]')
    ap.add_argument('--top', type=int, default=40)
    args = ap.parse_args()

    starts, funcs = load_jitmap(args.jitmap)
    total = 0
    in_jit = 0
    per_func = collections.Counter()
    per_offset = collections.defaultdict(collections.Counter)
    for comms, s in samples(args.perf_data):
        if comms.get(s.get('tid')) != args.comm:
            continue
        period = s['period']
        total += period
        i = bisect.bisect_right(starts, s['ip']) - 1
        if i < 0:
            continue
        size, name = funcs[i]
        offset = s['ip'] - starts[i]
        if offset >= size:
            continue
        in_jit += period
        per_func[name] += period
        per_offset[name][offset] += period

    print(f'{args.comm}: {total} cycles sampled, {100 * in_jit / max(total, 1):.1f} % in recompiled code')
    acc = 0
    for name, period in per_func.most_common(args.top):
        acc += period
        hot = ', '.join(f'+{o:x} {100 * p / period:.0f}%' for o, p in per_offset[name].most_common(4))
        print(f'{100 * period / total:5.2f} %  (cum {100 * acc / total:5.1f} %)  {name}  hot: {hot}')


if __name__ == '__main__':
    main()
