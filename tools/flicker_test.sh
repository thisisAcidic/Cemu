#!/bin/bash
# usage: flicker_test.sh <apk or -> <run name> [driver]
# Boots LCU into the harbor, stands still, records the TV screen for 15 s and counts flicker frames.
# driver is stock, turnip or a directory in customDrivers like for measure.py, default: keep the current one
set -e
cd "$(dirname "$0")"
PY=/Users/acidic/Documents/git/CEMU-gamedata/venv/bin/python
OUT=/Users/acidic/Documents/git/CEMU-gamedata/runs/$2
mkdir -p "$OUT"
if [ -n "$3" ]; then
  "$PY" -c "import measure; measure.configure('$OUT', '$3', 'on', 'multi')"
fi
"$PY" lcu_repro.py "$1" "$OUT" --idle 40 --cooldown 55 > "$OUT/repro.log" 2>&1 &
repro=$!
while [ ! -f "$OUT/ingame.png" ]; do
  kill -0 $repro 2>/dev/null || { echo "repro ended early"; cat "$OUT/repro.log"; exit 1; }
  sleep 2
done
sleep 5
adb shell screenrecord --time-limit 15 --bit-rate 20000000 /sdcard/flicker.mp4
adb pull -q /sdcard/flicker.mp4 "$OUT/flicker.mp4"
adb shell rm /sdcard/flicker.mp4
wait $repro || true
"$PY" flicker_check.py "$OUT/flicker.mp4"
