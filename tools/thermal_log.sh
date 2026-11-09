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
echo ',gpu_hz,gpu_busy'
while true; do
  printf '%s' \$(date +%s)
  for z in \$zones; do printf ',%s' \$(cat \$z/temp 2>/dev/null); done
  for c in /sys/devices/system/cpu/cpu[0-9]*; do printf ',%s' \$(cat \$c/cpufreq/scaling_cur_freq 2>/dev/null); done
  printf ',%s,%s\n' \$(cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null) \$(cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage 2>/dev/null | tr -d ' %')
  sleep $INTERVAL
done
" > "$OUT"
