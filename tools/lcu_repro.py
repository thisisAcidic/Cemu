"""Drive the LEGO City Undercover repro on the Thor: boot, load save, skip intro, wait for shaders, sweep camera.

  lcu_repro.py <apk or -> <out dir> [--rounds N]

Needs /data/local/tmp/pad.sh on the device and reference crops in CEMU-gamedata/repro_ref.
"""
import argparse
import os
import subprocess
import time

from PIL import Image, ImageChops, ImageStat

# the release build runs the emulation in its own process, see EMU_PROCESS
PKG = os.environ.get('CEMU_PKG', 'info.cemu.cemu.profile')
EMU_PROCESS = PKG + ':EmulationProcess' if not PKG.endswith(('.profile', '.debug')) else PKG
FILES = f'/sdcard/Android/data/{PKG}/files'
REF_DIR = '/Users/acidic/Documents/git/CEMU-gamedata/repro_ref'
BOXES = {'title': (40, 325, 330, 360), 'menu': (40, 225, 320, 300), 'notice': (130, 200, 570, 265)}
THRESHOLDS = {'title': 8, 'menu': 12, 'notice': 12}
BTN_A, BTN_X, BTN_START = 304, 307, 315


def adb(*args, check=True):
    return subprocess.run(['adb', *args], capture_output=True, text=True, check=check).stdout


def pad(*args):
    adb('shell', '/data/local/tmp/pad.sh', *map(str, args))


def screenshot(path):
    adb('shell', 'screencap -p /data/local/tmp/s.png')
    adb('pull', '-q', '/data/local/tmp/s.png', path)
    im = Image.open(path).convert('RGB').resize((700, 393))
    im.save(path)
    return im


def matches(im, name):
    ref = Image.open(os.path.join(REF_DIR, f'{name}.png'))
    diff = ImageChops.difference(im.convert('L').crop(BOXES[name]), ref)
    return ImageStat.Stat(diff).mean[0] < THRESHOLDS[name]


def is_loading(im):
    r, g, b = ImageStat.Stat(im.resize((70, 39))).mean
    return b > 120 and b > r * 1.8


def max_cpu_temp():
    temps = adb('shell', 'for z in /sys/class/thermal/thermal_zone*; do case $(cat $z/type) in cpu-*) cat $z/temp;; esac; done')
    return max(int(t) for t in temps.split()) / 1000


def cool_down(limit):
    t = max_cpu_temp()
    while t > limit:
        print(f'cooling down: {t:.1f} C')
        time.sleep(20)
        t = max_cpu_temp()


def wait_for(out, label, cond, timeout):
    end = time.time() + timeout
    n = 0
    while time.time() < end:
        im = screenshot(os.path.join(out, f'wait_{label}.png'))
        if cond(im):
            print(f'{label}: ok after {n} polls')
            return im
        n += 1
        time.sleep(2)
    raise TimeoutError(label)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('apk')
    ap.add_argument('out')
    ap.add_argument('--rounds', type=int, default=8)
    ap.add_argument('--from-notice', action='store_true', help='game is already waiting at the autosave notice')
    ap.add_argument('--record-intro', type=int, metavar='SECONDS', help='record the unskipped intro instead of playing')
    ap.add_argument('--intro-only', type=int, metavar='SECONDS', help='let the unskipped intro play for this long, no recording')
    ap.add_argument('--idle', type=int, metavar='SECONDS', help='stand still in game for this long instead of sweeping the camera')
    ap.add_argument('--cooldown', type=float, metavar='CELSIUS', help='wait until the hottest CPU zone is below this before starting')
    ap.add_argument('--settle', type=int, metavar='SECONDS', help='stand still in game this long before measuring, e.g. until background translation is done')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    if not args.from_notice:
        adb('shell', 'am', 'force-stop', PKG)
        if args.cooldown:
            cool_down(args.cooldown)
        if args.apk != '-':
            print(adb('install', '-r', args.apk).strip().splitlines()[-1])
        adb('shell', 'monkey', '-p', PKG, '-c', 'android.intent.category.LAUNCHER', '1')
        time.sleep(6)
        adb('shell', 'input', '-d', '0', 'tap', '533', '307')

        wait_for(args.out, 'title', lambda im: matches(im, 'title'), 600)
        pad('press', BTN_START)
        wait_for(args.out, 'menu', lambda im: matches(im, 'menu'), 30)
        for _ in range(3):  # Spiel laden, slot 1, confirm
            pad('press', BTN_A)
            time.sleep(3)
    wait_for(args.out, 'notice', lambda im: matches(im, 'notice'), 60)
    pad('press', BTN_A)
    if args.intro_only:
        time.sleep(args.intro_only)
        screenshot(os.path.join(args.out, 'intro_end.png'))
        adb('shell', 'am', 'force-stop', PKG)
        print('intro done')
        return
    if args.record_intro:
        adb('shell', f'screenrecord --time-limit {args.record_intro} /sdcard/intro.mp4')
        adb('pull', '-q', '/sdcard/intro.mp4', os.path.join(args.out, 'intro.mp4'))
        adb('shell', 'rm', '/sdcard/intro.mp4')
        adb('shell', 'am', 'force-stop', PKG)
        print('recorded')
        return
    wait_for(args.out, 'loading', is_loading, 30)
    wait_for(args.out, 'intro', lambda im: not is_loading(im), 180)
    time.sleep(4)
    pad('press', BTN_START)
    time.sleep(0.4)
    pad('press', BTN_X)
    time.sleep(5)
    wait_for(args.out, 'ingame', lambda im: not is_loading(im), 120)
    time.sleep(20)
    if args.settle:
        time.sleep(args.settle)
    screenshot(os.path.join(args.out, 'ingame.png'))
    if args.idle:
        end = time.time() + args.idle
        i = 1
        while time.time() < end:
            time.sleep(min(60, max(0, end - time.time())))
            screenshot(os.path.join(args.out, f'idle_{i}.png'))
            i += 1
    else:
        for i in range(1, args.rounds + 1):
            pad('look', 12)
            screenshot(os.path.join(args.out, f'look_{i}.png'))
    adb('shell', 'am', 'force-stop', PKG)
    adb('pull', '-q', f'{FILES}/log.txt', os.path.join(args.out, 'log.txt'))
    print('done')


if __name__ == '__main__':
    main()
