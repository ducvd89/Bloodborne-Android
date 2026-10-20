#!/usr/bin/env bash
# Host-JDK test of the app's route finder (NavData, NavGate) against the Python prototype's route costs.
#   tests/android-nav/run.sh PACK_NAV_DIR AREA REF.json
# PACK_NAV_DIR = atlas/maps/nav of an unpacked pack 2 (or nav/pack.py's output plus the gates files); REF.json from
# tools/atlas/nav/javaref.py.  Compiles the classes as plain Java 8, like the frontend's javac --release 8.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
root=$PWD
jdk="$root/.local-deps/android/jdk/usr/lib/jvm/java-27-openjdk"
[[ -x $jdk/bin/javac ]] || jdk=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")
out=${BB_TEST_OUT:-$root/out/android-nav-test}
rm -rf "$out"; mkdir -p "$out/classes"
src=tools/android/frontend/src/ro/cobrabm/fexdroid
"$jdk/bin/javac" --release 8 -Xlint:-options -d "$out/classes" $src/MiniJson.java $src/NavGate.java $src/NavData.java
"$jdk/bin/javac" --release 17 -cp "$out/classes" -d "$out/classes" tests/android-nav/NavTest.java
exec "$jdk/bin/java" -cp "$out/classes" ro.cobrabm.fexdroid.NavTest "$@"
