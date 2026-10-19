#!/usr/bin/env bash
# Host-JDK tests of the app's Java mod/PKG code (ModLoader, ModManager, PkgInstaller, GameCheck, Sfo, MiniJson), of RuntimeCleanup
# (the removal of the former Linux runtime on an update) and the
# parity check against the Python they replace (tools/android/mods_android.py).
#   tests/android-mods/run.sh            unit tests, then the parity check
# Optional: BB_TEST_MOD_PKG=<1.09 data-mod .pkg> BB_TEST_GAME=<game folder, read only> for the real-PKG test
# (needs out/host/pkg-tool/ps4-pkg-tool: tools/android/build_pkg_tool.sh host).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
root=$PWD
jdk="$root/.local-deps/android/jdk/usr/lib/jvm/java-27-openjdk"
[[ -x $jdk/bin/javac ]] || jdk=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")
out=${BB_TEST_OUT:-$root/out/android-mods-test}
rm -rf "$out"; mkdir -p "$out/classes"
src=tools/android/frontend/src/ro/cobrabm/fexdroid
# The core classes are plain Java 8, as the frontend's javac --release 8 builds them.
"$jdk/bin/javac" --release 8 -Xlint:-options -d "$out/classes" \
    $src/FsUtil.java $src/SafeWrite.java $src/MiniJson.java $src/ModLoader.java $src/ModManager.java $src/Sfo.java \
    $src/GameCheck.java $src/PkgInstaller.java $src/PkgVerifier.java $src/PkgProgress.java $src/RuntimeCleanup.java
"$jdk/bin/javac" --release 17 -cp "$out/classes" -d "$out/classes" tests/android-mods/T.java tests/android-mods/ModsTest.java \
    tests/android-mods/ParityCli.java tests/android-mods/CleanupTest.java tests/android-mods/SafeWriteTest.java
"$jdk/bin/java" -cp "$out/classes" ro.cobrabm.fexdroid.ModsTest
"$jdk/bin/java" -cp "$out/classes" ro.cobrabm.fexdroid.CleanupTest
"$jdk/bin/java" -cp "$out/classes" ro.cobrabm.fexdroid.SafeWriteTest
python3 -I tests/android-mods/parity.py "$jdk/bin/java" "$out/classes"
