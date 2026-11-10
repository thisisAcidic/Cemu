"""Records when SurfaceFlinger presented the frames of the TV surface of an emulator app, until killed.

  sf_frames.py <package> <out.csv>

Works for builds without the frame time log (e.g. the Cemu Android port). The TV surface is the SurfaceView of the
emulation activity on the main display. SurfaceFlinger keeps the last 128 frames, so polling every second is enough
at 60 FPS.
"""
import subprocess
import sys
import time


def adb(*args):
    return subprocess.run(['adb', 'shell', *args], capture_output=True, text=True).stdout


def find_layer(package):
    for line in adb('dumpsys', 'SurfaceFlinger', '--list').splitlines():
        line = line.strip()
        if line.startswith(f'SurfaceView[{package}/') and 'EmulationActivity' in line and 'Background' not in line:
            return line
    return None


def main():
    package, out = sys.argv[1], sys.argv[2]
    layer = None
    while layer is None:
        layer = find_layer(package)
        if layer is None:
            time.sleep(2)
    seen = set()
    with open(out, 'w') as f:
        f.write('present_ns\n')
        while True:
            lines = adb('dumpsys', 'SurfaceFlinger', '--latency', layer).splitlines()[1:]
            for line in lines:
                parts = line.split()
                # actual present time, 0 or INT64_MAX while a frame is still pending
                if len(parts) == 3 and parts[1] not in ('0', '9223372036854775807') and parts[1] not in seen:
                    seen.add(parts[1])
                    f.write(parts[1] + '\n')
            f.flush()
            time.sleep(1)


if __name__ == '__main__':
    main()
