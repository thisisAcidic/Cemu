#!/bin/bash
# usage: measure_batch.sh "<run name> <measure.py args>"...  runs each measurement in order
cd "$(dirname "$0")"
PY=/Users/acidic/Documents/git/CEMU-gamedata/venv/bin/python
for run in "$@"; do
  read -r -a argv <<< "$run"
  "$PY" measure.py "${argv[@]}" > "/Users/acidic/Documents/git/CEMU-gamedata/runs/${argv[0]}.out" 2>&1
  echo "${argv[0]} exit=$?"
done
