"""Dual-screen lifecycle check on the Thor: Home and return, runtime swaps, Home while swapped.

Usage: dual_check.py APK OUT
Boots LCU to the title screen and keeps the process alive throughout. Prints one line per step
and screenshots both displays after each one.
"""
import os
import sys
import time

import lcu_repro as r

TOP = '4630946441858561667'
BOTTOM = '4630946482288158084'
LOG = f'{r.FILES}/log.txt'


def snap(out, label):
    for name, display in (('top', TOP), ('bottom', BOTTOM)):
        r.adb('shell', 'screencap', '-d', display, '-p', '/data/local/tmp/d.png')
        r.adb('pull', '-q', '/data/local/tmp/d.png', os.path.join(out, f'{label}_{name}.png'))


def presentations():
    windows = r.adb('shell', 'dumpsys', 'window', 'windows')
    # presentations have no title, so their windows are only named after the package
    return sum(1 for line in windows.splitlines()
               if line.strip().startswith('Window #') and line.rstrip().endswith(f'{r.PKG}}}:'))


def home_and_back(out, label, pid0):
    r.adb('shell', 'input', '-d', '0', 'keyevent', '3')
    time.sleep(3)
    snap(out, f'{label}_home')
    left = presentations()
    r.adb('shell', 'monkey', '-p', r.PKG, '-c', 'android.intent.category.LAUNCHER', '1')
    time.sleep(6)
    snap(out, f'{label}_back')
    pid = r.adb('shell', 'pidof', r.PKG, check=False).strip()
    print(f'{label}: {"OK" if pid == pid0 else "RESTARTED"}, presentations while in background: {left}')


def swap(out, label, pid0):
    r.adb('shell', 'input', '-d', '0', 'keyevent', '4')
    time.sleep(1.5)
    r.adb('shell', 'input', '-d', '0', 'tap', '320', '514')
    time.sleep(5)
    snap(out, label)
    pid = r.adb('shell', 'pidof', r.PKG, check=False).strip()
    print(f'{label}: {"OK" if pid == pid0 else "RESTARTED"}')


def main():
    apk, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    r.adb('shell', 'am', 'force-stop', r.PKG)
    print(r.adb('install', '-r', apk).strip().splitlines()[-1])
    r.adb('shell', 'monkey', '-p', r.PKG, '-c', 'android.intent.category.LAUNCHER', '1')
    time.sleep(6)
    r.adb('shell', 'input', '-d', '0', 'tap', '533', '307')
    r.wait_for(out, 'title', lambda im: r.matches(im, 'title'), 300)
    pid0 = r.adb('shell', 'pidof', r.PKG).strip()
    time.sleep(3)

    for i in range(1, 4):
        home_and_back(out, f'home{i}', pid0)
    for i in range(1, 5):
        swap(out, f'swap{i}', pid0)
    swap(out, 'swapped', pid0)
    home_and_back(out, 'home_swapped', pid0)
    swap(out, 'unswapped', pid0)

    errors = r.adb('shell', f'grep -E "Cannot create|Failed to create|SURFACE_LOST|signal" {LOG}', check=False).strip()
    print('log errors:', errors or 'none')
    r.adb('shell', 'am', 'force-stop', r.PKG)


if __name__ == '__main__':
    main()
