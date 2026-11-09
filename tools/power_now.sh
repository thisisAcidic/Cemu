#!/bin/bash
# usage: power_now.sh <seconds> [label]
# Averages the battery power of the whole device over the given time. Only valid while discharging.
SECONDS_TOTAL=${1:-30}
LABEL=${2:-}
adb shell "
sum=0; n=0; charging=0
end=\$((\$(date +%s) + $SECONDS_TOTAL))
while [ \$(date +%s) -lt \$end ]; do
  ua=\$(cat /sys/class/power_supply/battery/current_now)
  uv=\$(cat /sys/class/power_supply/battery/voltage_now)
  [ \$ua -gt 0 ] && charging=1
  sum=\$((sum + (-ua / 1000) * (uv / 1000)))
  n=\$((n + 1))
  sleep 1
done
if [ \$charging -eq 1 ]; then echo '$LABEL charging, no valid reading'; else echo \"$LABEL \$((sum / n / 1000)) mW average over \$n samples\"; fi
"
