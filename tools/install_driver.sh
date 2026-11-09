#!/bin/bash
# usage: install_driver.sh <driver.zip> <name>
# Unpacks an adrenotools driver package and copies it to customDrivers/<name> of the profile app,
# where measure.py --driver <name> picks it up.
set -e
zip=$1
name=$2
dir=$(mktemp -d)
unzip -q "$zip" -d "$dir"
meta=$(find "$dir" -name meta.json | head -1)
[ -n "$meta" ] || { echo "no meta.json in $zip"; exit 1; }
src=$(dirname "$meta")
cat "$meta"; echo
dest=/sdcard/Android/data/info.cemu.cemu.profile/files/customDrivers/$name
# adb push may not create directories below Android/data, the shell may
adb shell "rm -rf $dest && mkdir -p $dest"
for f in "$src"/*; do
  adb push -q "$f" "$dest/"
done
adb shell "ls -l $dest"
rm -rf "$dir"
echo "installed as $name"
