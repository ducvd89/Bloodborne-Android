#!/bin/sh
# Copies the Thor's current game log to $1 every 0.5 s (follows log rotation; keeps the last good copy).
while true; do
  adb -s 70538e86 exec-out run-as com.ducvd89.bloodborne cat /data/data/com.ducvd89.bloodborne/files/bbport/logs/android-game.log 2>/dev/null | tr -d '\000' > "$1.tmp"
  [ -s "$1.tmp" ] && mv "$1.tmp" "$1"
  sleep 0.5
done
