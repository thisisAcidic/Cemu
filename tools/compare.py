"""Print the summaries of measurement runs side by side.

Usage: compare.py RUN...   (names of folders in CEMU-gamedata/runs)
"""
import csv
import json
import os
import sys

ROOT = '/Users/acidic/Documents/git/CEMU-gamedata/runs'


def power(thermal):
    if thermal.get('charging'):
        return 'charging'
    return f'{thermal.get("power_scene_avg_w") or 0:6.2f}W'


def thread_load(run, window_s=70):
    """share of one core used by the emulated main core and the GPU thread while the scene was measured"""
    path = os.path.join(ROOT, run, 'thermal.csv')
    rows = [r for r in csv.DictReader(l for l in open(path) if not l.startswith('#')) if r.get('core1_ticks') and int(r['core1_ticks']) > 0]
    if len(rows) < 2:
        return '    -      -      -     -     -'
    end = int(rows[-1]['time']) - 5
    scene = [r for r in rows if end - window_s <= int(r['time']) <= end]
    if len(scene) < 2:
        return '    -      -      -     -     -'
    seconds = int(scene[-1]['time']) - int(scene[0]['time'])
    load = lambda col: (int(scene[-1].get(col) or 0) - int(scene[0].get(col) or 0)) / seconds
    others = f' {load("core2_ticks"):5.0f}% {load("recompiler_ticks"):4.0f}%' if scene[0].get('core2_ticks') else '      -     -'
    others += f' {load("llvm_ticks"):4.0f}%' if scene[0].get('llvm_ticks') else '     -'
    return f'{load("core1_ticks"):4.0f}% {load("gpu_thread_ticks"):5.0f}%' + others


def ddr(run, window_s=70):
    path = os.path.join(ROOT, run, 'thermal.csv')
    rows = [r for r in csv.DictReader(l for l in open(path) if not l.startswith('#')) if r.get('ddr_khz')]
    if not rows:
        return '    -'
    end = int(rows[-1]['time']) - 5
    scene = [int(r['ddr_khz']) for r in rows if end - window_s <= int(r['time']) <= end]
    return f'{sum(scene) / len(scene) / 1000:5.0f}' if scene else '    -'


def window_stats(run, window_s=70):
    """averages over the measured scene only: cpu temperature, prime/gold core clock, gpu clock"""
    path = os.path.join(ROOT, run, 'thermal.csv')
    rows = list(csv.DictReader(l for l in open(path) if not l.startswith('#')))
    if not rows:
        return '-'
    end = int(rows[-1]['time']) - 5
    scene = [r for r in rows if end - window_s <= int(r['time']) <= end]
    cpu_cols = [c for c in rows[0] if c.startswith('cpu-')]
    def avg(values):
        values = [v for v in values if v is not None]
        return sum(values) / len(values) if values else 0
    temp = avg([max(int(r[c]) for c in cpu_cols) / 1000 if all(r.get(c) for c in cpu_cols) else None for r in scene])
    prime = avg([int(r['cpu7_khz']) / 1000 if r.get('cpu7_khz') else None for r in scene])
    gold = avg([int(r['cpu3_khz']) / 1000 if r.get('cpu3_khz') else None for r in scene])
    return f'{temp:5.1f}C {prime:5.0f} {gold:5.0f}'


def main():
    print(f'{"run":22} {"fps":>5} {"p99":>6} {"<=16.7":>7} {">50ms":>5} {"power":>7} {"cpu max":>7} {"gpu max":>7} {"prime":>6} {"gpu":>5} {"core1":>5} {"gputh":>6} {"core2":>6} {"rec":>5} {"llvm":>5} {"ddr":>5}   scene: {"cpu":>6} {"prime":>5} {"gold":>5}')
    for name in sys.argv[1:]:
        path = os.path.join(ROOT, name, 'summary.json')
        if not os.path.exists(path):
            print(f'{name:22} missing')
            continue
        s = json.load(open(path))
        f, t = s['frames'], s['thermal']
        print(f'{name:22} {f["avg_fps"]:5.1f} {f["p99_ms"]:5.1f}ms {f["pct_within_16_7"]:6.1f}% {f["frames_over_50ms"]:5d} '
              f'{power(t)} {t["cpu_temp_max_c"]:6.1f}C {t["gpu_temp_max_c"]:6.1f}C '
              f'{t["prime_core_mhz_avg"]:6.0f} {t["gpu_mhz_avg"]:5.0f} {thread_load(name)} {ddr(name)}          {window_stats(name)}')


if __name__ == '__main__':
    main()
