#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Host-JDK tests of the app's Native CPU code (NativeCpu.java: libbbaot.json, the import checks, the state, the trigger rule; AotBuild.java: the
# phone build's command line, progress lines, bar, time left, jobs and needs).
#   tests/android-native-cpu/run.sh
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
root=$PWD
jdk="$root/.local-deps/android/jdk/usr/lib/jvm/java-27-openjdk"
[[ -x $jdk/bin/javac ]] || jdk=/home/cid/BloodbornePC/.local-deps/android/jdk/usr/lib/jvm/java-27-openjdk
[[ -x $jdk/bin/javac ]] || jdk=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")
out=${BB_TEST_OUT:-$root/out/android-native-cpu-test}
rm -rf "$out"; mkdir -p "$out/classes"
src=tools/android/frontend/src/ro/cobrabm/fexdroid
"$jdk/bin/javac" --release 8 -Xlint:-options -d "$out/classes" $src/FsUtil.java $src/SafeWrite.java $src/MiniJson.java $src/Sfo.java $src/GameCheck.java $src/NativeCpu.java $src/AotBuild.java
"$jdk/bin/javac" --release 17 -cp "$out/classes" -d "$out/classes" tests/android-native-cpu/NativeCpuTest.java tests/android-native-cpu/AotBuildTest.java
"$jdk/bin/java" -cp "$out/classes" ro.cobrabm.fexdroid.NativeCpuTest
"$jdk/bin/java" -cp "$out/classes" ro.cobrabm.fexdroid.AotBuildTest
