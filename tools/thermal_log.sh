#!/bin/sh
# usage: thermal_log.sh <out.csv> [interval_s]
# Samples Thor temperatures and clocks over adb until killed. Unreadable files are reported once and skipped.
OUT=$1
INTERVAL=${2:-5}
adb shell "
zones=''
for z in /sys/class/thermal/thermal_zone*; do
  case \$(cat \$z/type 2>/dev/null) in cpu-*|cpuss-*|gpuss-*|ddr) zones=\"\$zones \$z\";; esac
done
for f in /sys/class/kgsl/kgsl-3d0/gpuclk /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage; do
  [ -r \$f ] || echo \"# not readable: \$f\" >&2
done
printf 'time'
for z in \$zones; do printf ',%s' \$(cat \$z/type); done
for c in /sys/devices/system/cpu/cpu[0-9]*; do printf ',%s_khz' \${c##*/}; done
echo ',gpu_hz,gpu_busy,batt_ua,batt_uv,core1_ticks,gpu_thread_ticks'
while true; do
  printf '%s' \$(date +%s)
  for z in \$zones; do printf ',%s' \$(cat \$z/temp 2>/dev/null); done
  for c in /sys/devices/system/cpu/cpu[0-9]*; do printf ',%s' \$(cat \$c/cpufreq/scaling_cur_freq 2>/dev/null); done
  printf ',%s,%s' \$(cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null) \$(cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage 2>/dev/null | tr -d ' %')
  printf ',%s,%s' \$(cat /sys/class/power_supply/battery/current_now 2>/dev/null) \$(cat /sys/class/power_supply/battery/voltage_now 2>/dev/null)
  # cpu time (utime + stime, clock ticks) of the emulated main core and the GPU thread
  core1=''; gpu=''
  p=\$(pidof info.cemu.cemu.profile)
  for t in /proc/\$p/task/*; do
    case \$(cat \$t/comm 2>/dev/null) in
      'OSSched[core=1]') core1=\$(cut -d' ' -f14,15 \$t/stat | tr ' ' '+');;
      LatteThread) gpu=\$(cut -d' ' -f14,15 \$t/stat | tr ' ' '+');;
    esac
  done
  printf ',%s,%s\n' \$((\${core1:-0})) \$((\${gpu:-0}))
  sleep $INTERVAL
done
" > "$OUT"
