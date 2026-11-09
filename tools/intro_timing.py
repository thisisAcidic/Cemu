"""Compare game speed of two recordings of the unskipped LCU intro.

  intro_timing.py <baseline.mp4> <test.mp4>

Both recordings start right after the autosave notice. Frames of the baseline at fixed offsets after the
loading screen are searched in the test recording; equal offsets mean equal game speed.
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageChops, ImageStat

FPS = 10


def frames(video):
    tmp = tempfile.mkdtemp()
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', video, '-vf', f'fps={FPS},scale=160:90', os.path.join(tmp, '%05d.png')], check=True)
    return [Image.open(os.path.join(tmp, f)).convert('RGB') for f in sorted(os.listdir(tmp))]


def is_loading(im):
    r, g, b = ImageStat.Stat(im).mean
    return b > 120 and b > r * 1.8


def intro_start(fs):
    seen = False
    for i, im in enumerate(fs):
        if is_loading(im):
            seen = True
        elif seen:
            return i
    raise ValueError('no loading screen found')


def diff(a, b):
    return ImageStat.Stat(ImageChops.difference(a.convert('L'), b.convert('L'))).mean[0]


def main():
    base, test = frames(sys.argv[1]), frames(sys.argv[2])
    b0, t0 = intro_start(base), intro_start(test)
    print(f'intro starts: baseline {b0 / FPS:.1f}s, test {t0 / FPS:.1f}s')
    for offset in range(5, (len(base) - b0) // FPS, 5):
        ref = base[b0 + offset * FPS]
        lo, hi = t0, min(len(test), t0 + offset * FPS * 2 + 5 * FPS)
        best = min(range(lo, hi), key=lambda i: diff(ref, test[i]))
        found = (best - t0) / FPS
        print(f'baseline +{offset:3d}s -> test +{found:6.1f}s  ratio {found / offset:5.2f}  diff {diff(ref, test[best]):5.1f}')


if __name__ == '__main__':
    main()
