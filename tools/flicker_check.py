"""Count flicker frames in a screen recording of a mostly still scene.

Usage: flicker_check.py VIDEO
A flicker frame differs clearly from both its neighbours while the neighbours are close to each other,
which a still or slowly moving scene doesn't produce.
"""
import statistics
import subprocess
import sys

W, H = 192, 108


def frames(video):
    proc = subprocess.Popen(['ffmpeg', '-v', 'error', '-i', video, '-vf', f'scale={W}:{H}', '-f', 'rawvideo', '-pix_fmt', 'gray', '-'],
                            stdout=subprocess.PIPE)
    size = W * H
    while True:
        data = proc.stdout.read(size)
        if len(data) < size:
            break
        yield data
    proc.wait()


def diff(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def corner(frame):
    # the FPS counter sits in the top left corner
    return bytes(frame[y * W + x] for y in range(H // 10) for x in range(W // 6))


def count_flicker(all_frames, label):
    diffs = [diff(all_frames[i], all_frames[i + 1]) for i in range(len(all_frames) - 1)]
    median = statistics.median(diffs) if diffs else 0
    threshold = max(3 * median, 1.5)
    flicker = []
    for i in range(1, len(all_frames) - 1):
        before, after = diffs[i - 1], diffs[i]
        across = diff(all_frames[i - 1], all_frames[i + 1])
        if before > threshold and after > threshold and across < min(before, after) / 2:
            flicker.append((i, before, after, across))
    print(f'{label}: median difference {median:.2f}, flicker frames: {len(flicker)}')
    for i, before, after, across in flicker[:10]:
        print(f'  frame {i}: {before:.1f} to previous, {after:.1f} to next, {across:.1f} between them')


def main():
    all_frames = list(frames(sys.argv[1]))
    print(f'{len(all_frames)} frames')
    count_flicker(all_frames, 'whole image')
    count_flicker([corner(f) for f in all_frames], 'top left corner')


if __name__ == '__main__':
    main()
