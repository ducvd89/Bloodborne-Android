#!/usr/bin/env bash
set -euo pipefail
serial=${1:?usage: launch_thor.sh ADB_SERIAL [cpu|vulkan|renderer|window|gamepads|controller|game]}
mode=${2:-game}
case $mode in cpu|vulkan|renderer|window|gamepads|controller|game) ;; *) echo 'Unknown mode' >&2; exit 2 ;; esac
app=ro.cobrabm.fexdroid
files=/data/data/$app/files
if [[ $mode == game || $mode == window ]]; then
  if ! adb -s "$serial" shell run-as "$app" test -S "$files/rootfs/tmp/.X11-unix/X0"; then
    # Recreate the activity so its developer-screen action is applied. Do not
    # force-stop: that also kills other app-owned processes and file transfers.
    adb -s "$serial" shell am start -f 0x10008000 -n "$app/.MainActivity" --es action x
    sleep 2
    for ((attempt=0; attempt<30; attempt++)); do
        if adb -s "$serial" shell run-as "$app" test -S "$files/rootfs/tmp/.X11-unix/X0"; then
            break
        fi
        sleep 1
    done
    adb -s "$serial" shell run-as "$app" test -S "$files/rootfs/tmp/.X11-unix/X0"
  fi
fi
exec adb -s "$serial" shell run-as "$app" "$files/rootfs/bin/sh" "$files/bbport/run-thor.sh" "$mode"
