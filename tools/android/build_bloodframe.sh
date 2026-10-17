#!/usr/bin/env bash
# Bloodframe, this port's Lossless Scaling frame generation engine for the Bionic game process:
# libbloodframe.so. It lives in its own repository, https://github.com/ducvd89/bloodframe (MIT; it carries
# the history of GameNative's MIT lsfg-vk Android fork it is based on, lsfg-vk 1.0 by PancakeTAS), and is
# built here at a pinned commit with that repository's build-android.sh. Not a Vulkan layer: a single shared
# library with the small C API of bb_lsfg.h; libbbgpu dlopen()s it only when frame generation is on
# (gpu/shadps4/video_core/renderer_vulkan/vk_lsfg.cpp).
# Lossless Scaling's shaders come from the player's own Lossless.dll at run time, never the APK.
# Output: out/bionic/lib/libbloodframe.so and out/bionic/licenses/bloodframe-*.LICENSE*, bloodframe-source.txt;
# build_bionic.sh runs this, build_release_runtime.py packages them.
# tools/android/lsfg/bb_lsfg.h is the game's copy of the C API header (libbbgpu is built before this runs); this script fails when it differs from the
# header of the pinned commit, so a pin bump must copy android/bb_lsfg.h over it.
# Prerequisite: tools/android/build_bionic_deps.sh (the NDK).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
BLOODFRAME_REPO=https://github.com/ducvd89/bloodframe.git
BLOODFRAME_COMMIT=c29027555cfa8c9fc73c20f03816230becb9f469
root=$PWD/.local-deps/android
src=$root/src/bloodframe
ndk=${BB_NDK:-$root/sdk/ndk/29.0.14206865}
api=${BB_ANDROID_API:-30}
J=${BB_JOBS:-22}
out=$PWD/out/bionic
build=$out/bloodframe-build
[[ -f $ndk/build/cmake/android.toolchain.cmake ]] || { echo "No NDK: $ndk (tools/android/build_bionic_deps.sh)" >&2; exit 1; }
if [[ ! -d $src/.git ]]; then
    mkdir -p "$root/src"
    git clone -q "$BLOODFRAME_REPO" "$src" ||
        { echo "Cannot clone $BLOODFRAME_REPO (network?)" >&2; exit 1; }
fi
git -C "$src" cat-file -e "$BLOODFRAME_COMMIT^{commit}" 2>/dev/null ||
    git -C "$src" fetch -q origin main
git -C "$src" -c advice.detachedHead=false checkout -q "$BLOODFRAME_COMMIT"
[[ $(git -C "$src" rev-parse HEAD) == "$BLOODFRAME_COMMIT" ]] ||
    { echo "$src is not at $BLOODFRAME_COMMIT" >&2; exit 1; }
cmp -s "$src/android/bb_lsfg.h" tools/android/lsfg/bb_lsfg.h ||
    { echo "tools/android/lsfg/bb_lsfg.h differs from $src/android/bb_lsfg.h (the pinned commit)" >&2; exit 1; }
# A build tree made from another source (an older layout) is not reused.
if [[ -f $build/CMakeCache.txt ]] && ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$src" "$build/CMakeCache.txt"; then
    rm -rf "$build"
fi
taskset -c 2-23 nice -n 19 "$src/build-android.sh" --ndk "$ndk" --api "$api" --out "$out" --build-dir "$build" --jobs "$J"
