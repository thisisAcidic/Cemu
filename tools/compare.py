"""Print the summaries of measurement runs side by side.

Usage: compare.py RUN...   (names of folders in CEMU-gamedata/runs)
"""
import json
import os
import sys

ROOT = '/Users/acidic/Documents/git/CEMU-gamedata/runs'


def power(thermal):
    if thermal.get('charging'):
        return 'charging'
    return f'{thermal.get("power_scene_avg_w") or 0:6.2f}W'


def main():
    print(f'{"run":22} {"fps":>5} {"p99":>6} {"<=16.7":>7} {">50ms":>5} {"power":>7} {"cpu max":>7} {"gpu max":>7} {"prime":>6} {"gpu":>5}')
    for name in sys.argv[1:]:
        path = os.path.join(ROOT, name, 'summary.json')
        if not os.path.exists(path):
            print(f'{name:22} missing')
            continue
        s = json.load(open(path))
        f, t = s['frames'], s['thermal']
        print(f'{name:22} {f["avg_fps"]:5.1f} {f["p99_ms"]:5.1f}ms {f["pct_within_16_7"]:6.1f}% {f["frames_over_50ms"]:5d} '
              f'{power(t)} {t["cpu_temp_max_c"]:6.1f}C {t["gpu_temp_max_c"]:6.1f}C '
              f'{t["prime_core_mhz_avg"]:6.0f} {t["gpu_mhz_avg"]:5.0f}')


if __name__ == '__main__':
    main()
