"""Measurement runs for LCU on the Thor: configure, run a scene, summarize frame times and thermals.

  measure.py <run name> --scene harbor|intro --driver stock|turnip|<dir in customDrivers> --patch on|off --cpu multi|single [--idle S] [--perf S]

Results go to CEMU-gamedata/runs/<run name>/ (summary.json plus raw logs).
"""
import argparse
import csv
import json
import os
import signal
import subprocess
import sys
import time

import frametimes
import lcu_repro as r

ROOT = '/Users/acidic/Documents/git/CEMU-gamedata/runs'
TOOLS = os.path.dirname(os.path.abspath(__file__))
DRIVERS = '/storage/emulated/0/Android/data/info.cemu.cemu.profile/files/customDrivers'
TURNIP = 'fe1e2b85-a742-46d9-8884-23abd7ea946f'
PACK = 'graphicPacks/LEGOCityUndercover_60fps/rules.txt'
PROFILE = 'gameProfiles/0005000010101b00.ini'
SCENE_WINDOW_S = {'harbor': 70, 'intro': 60}


def configure(out, driver, patch, cpu):
    local = os.path.join(out, 'settings.xml')
    r.adb('shell', 'am', 'force-stop', r.PKG)
    r.adb('pull', '-q', f'{r.FILES}/settings.xml', local)
    s = open(local, encoding='utf-8').read()
    start = s.index('<custom_driver_path>') + len('<custom_driver_path>')
    end = s.index('</custom_driver_path>')
    if driver == 'stock':
        path = ''
    else:
        path = f'{DRIVERS}/{TURNIP if driver == "turnip" else driver}'
    s = s[:start] + path + s[end:]
    enabled = f'<Entry filename="{PACK}"/>'
    disabled = f'<Entry filename="{PACK}" disabled="true"/>'
    s = s.replace(enabled, disabled) if patch == 'off' else s.replace(disabled, enabled)
    open(local, 'w', encoding='utf-8').write(s)
    r.adb('push', '-q', local, f'{r.FILES}/settings.xml')
    if cpu == 'single':
        r.adb('shell', f"printf '[CPU]\\ncpuMode = 1\\n' > {r.FILES}/{PROFILE}")
    else:
        r.adb('shell', 'rm', '-f', f'{r.FILES}/{PROFILE}')
    r.adb('shell', f'touch {r.FILES}/frametimes.enable; rm -f {r.FILES}/frametimes.csv')


def thermal_summary(path, window_s):
    rows = list(csv.DictReader(line for line in open(path) if not line.startswith('#')))
    if not rows:
        return {}
    cpu_cols = [c for c in rows[0] if c.startswith('cpu-')]
    gpu_cols = [c for c in rows[0] if c.startswith('gpuss-')]
    maxc = [max(int(row[c]) for c in cpu_cols) / 1000 for row in rows if all(row[c] for c in cpu_cols)]
    maxg = [max(int(row[c]) for c in gpu_cols) / 1000 for row in rows if all(row[c] for c in gpu_cols)]
    prime = [int(row['cpu7_khz']) / 1000 for row in rows if row.get('cpu7_khz')]
    gpu_mhz = [int(row['gpu_hz']) / 1e6 for row in rows if row.get('gpu_hz')]
    busy = [int(row['gpu_busy']) for row in rows if row.get('gpu_busy')]
    # battery power while the scene is measured: the log keeps running for a few seconds after the app was closed
    end = int(rows[-1]['time']) - 5
    scene = [row for row in rows if end - window_s <= int(row['time']) <= end and row.get('batt_ua') and row.get('batt_uv')]
    # while charging the battery current is the charge current, not what the device uses
    charging = any(int(row['batt_ua']) > 0 for row in scene)
    power = [-int(row['batt_ua']) * int(row['batt_uv']) / 1e12 for row in scene]
    return {
        'power_scene_avg_w': None if charging or not power else sum(power) / len(power),
        'charging': charging,
        'cpu_temp_max_c': max(maxc, default=0), 'cpu_temp_end_c': maxc[-1] if maxc else 0,
        'gpu_temp_max_c': max(maxg, default=0),
        'prime_core_mhz_min': min(prime, default=0), 'prime_core_mhz_avg': sum(prime) / len(prime) if prime else 0,
        'gpu_mhz_avg': sum(gpu_mhz) / len(gpu_mhz) if gpu_mhz else 0, 'gpu_busy_avg': sum(busy) / len(busy) if busy else 0,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('--scene', choices=['harbor', 'intro'], required=True)
    ap.add_argument('--driver', default='turnip', help='stock, turnip or the name of a directory in customDrivers')
    ap.add_argument('--patch', choices=['on', 'off'], default='on')
    ap.add_argument('--cpu', choices=['multi', 'single'], default='multi')
    ap.add_argument('--idle', type=int, help='harbor: stand still this long instead of sweeping')
    ap.add_argument('--perf', type=int, metavar='SECONDS', help='harbor: simpleperf record for this long during the sweep')
    ap.add_argument('--jitmap', action='store_true', help='let Cemu write jitmap.txt, use together with --perf')
    ap.add_argument('--cooldown', type=float, default=50)
    args = ap.parse_args()
    out = os.path.join(ROOT, args.name)
    os.makedirs(out, exist_ok=True)

    configure(out, args.driver, args.patch, args.cpu)
    r.adb('shell', f"{'touch' if args.jitmap else 'rm -f'} {r.FILES}/jitmap.enable; rm -rf {r.FILES}/jitmap.txt {r.FILES}/dump/recompiler")
    thermal = subprocess.Popen([os.path.join(TOOLS, 'thermal_log.sh'), os.path.join(out, 'thermal.csv'), '5'], start_new_session=True)
    cmd = [sys.executable, os.path.join(TOOLS, 'lcu_repro.py'), '-', out, '--cooldown', str(args.cooldown)]
    if args.scene == 'intro':
        cmd += ['--intro-only', '150']
    elif args.idle:
        cmd += ['--idle', str(args.idle)]
    else:
        cmd += ['--rounds', '5']
    perf = None
    repro = subprocess.Popen(cmd)
    if args.perf:
        # start sampling once the sweep is running: wait for the in-game screenshot
        while repro.poll() is None and not os.path.exists(os.path.join(out, 'ingame.png')):
            time.sleep(2)
        perf = subprocess.run(['adb', 'shell', f'simpleperf record --app {r.PKG} -g --duration {args.perf} -o /data/local/tmp/perf.data'],
                              capture_output=True, text=True)
        r.adb('pull', '-q', '/data/local/tmp/perf.data', os.path.join(out, 'perf.data'))
    repro.wait()
    os.killpg(thermal.pid, signal.SIGTERM)
    r.adb('pull', '-q', f'{r.FILES}/frametimes.csv', os.path.join(out, 'frametimes.csv'))
    if args.jitmap:
        r.adb('pull', '-q', f'{r.FILES}/jitmap.txt', os.path.join(out, 'jitmap.txt'))
        r.adb('pull', '-q', f'{r.FILES}/dump/recompiler', os.path.join(out, 'recompiler'))

    rows = frametimes.load(os.path.join(out, 'frametimes.csv'))
    window = args.idle if args.idle else SCENE_WINDOW_S[args.scene]
    last = rows[-1]['timestamp_us']
    scene_rows = [row for row in rows if row['timestamp_us'] >= last - window * 1e6]
    summary = {'config': vars(args), 'frames': frametimes.summarize(scene_rows), 'thermal': thermal_summary(os.path.join(out, 'thermal.csv'), window)}
    if perf is not None:
        summary['perf_stderr'] = perf.stderr[-500:]
    json.dump(summary, open(os.path.join(out, 'summary.json'), 'w'), indent=1)
    print(json.dumps(summary, indent=1))


if __name__ == '__main__':
    main()
