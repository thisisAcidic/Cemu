"""Samples USB input and battery power five times a second. The thermal log only gets a sample every few seconds,
too few to average the power of a scene where the load changes with every camera move.

Usage: power_log.py OUT_CSV   (runs until it is terminated)
"""
import subprocess
import sys
import time

LOOP = ('while true; do read a < /sys/class/power_supply/usb/current_now; read b < /sys/class/power_supply/usb/voltage_now; '
        'read c < /sys/class/power_supply/battery/current_now; read d < /sys/class/power_supply/battery/voltage_now; '
        'echo "$a,$b,$c,$d"; sleep 0.2; done')


def main():
    with open(sys.argv[1], 'w') as out:
        out.write('time,usb_ua,usb_uv,batt_ua,batt_uv\n')
        proc = subprocess.Popen(['adb', 'shell', LOOP], stdout=subprocess.PIPE, text=True)
        for line in proc.stdout:
            out.write(f'{time.time():.2f},{line.strip()}\n')
            out.flush()


if __name__ == '__main__':
    main()
