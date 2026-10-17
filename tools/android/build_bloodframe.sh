#!/usr/bin/env bash
# Bloodframe, this port's Lossless Scaling frame generation engine for the Bionic game process:
# libbloodframe.so. It is based on lsfg-vk 1.0 by PancakeTAS (MIT) via GameNative's Android fork of it
# (https://github.com/Ragnarok93/lsfg-vk-android, MIT, branch release), built at a pinned commit with
# bloodframe-patches/*.patch applied. Not the fork's Vulkan layer: its framegen backend (framegen/), the
# Lossless.dll shader extraction (src/extract/) and dxbc/pe-parse/volk are linked into ONE shared library
# with the small C API of tools/android/lsfg/bb_lsfg.h; libbbgpu dlopen()s it only when frame generation
# is on (gpu/shadps4/video_core/renderer_vulkan/vk_lsfg.cpp).
# The patches make the framegen's own Vulkan instance come from the game's vkGetInstanceProcAddr (the
# driver the process loaded itself, e.g. a Turnip zip) and the DLL path a call, not a folder search. The
# fork's toml11 (config file) and its layer are not built.
# Lossless Scaling's shaders come from the player's own Lossless.dll at run time, never the APK.
# Output: out/bionic/lib/libbloodframe.so and out/bionic/licenses/bloodframe-*.LICENSE; build_bionic.sh runs
# this, build_release_runtime.py packages both.
# Prerequisite: tools/android/build_bionic_deps.sh (the NDK).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
LSFG_REPO=https://github.com/Ragnarok93/lsfg-vk-android.git
LSFG_BRANCH=release
LSFG_COMMIT=0802cd33319c101e4c41f6828153d3331d8a3258
root=$PWD/.local-deps/android
src=$root/src/lsfg-vk-android
ndk=${BB_NDK:-$root/sdk/ndk/29.0.14206865}
api=${BB_ANDROID_API:-30}
llvm=$ndk/toolchains/llvm/prebuilt/linux-x86_64
J=${BB_JOBS:-22}
build=$PWD/out/bionic/bloodframe-build
out=$PWD/out/bionic
run() { taskset -c 2-23 nice -n 19 "$@"; }
[[ -f $ndk/build/cmake/android.toolchain.cmake ]] || { echo "No NDK: $ndk (tools/android/build_bionic_deps.sh)" >&2; exit 1; }
if [[ ! -d $src/.git ]]; then
    mkdir -p "$root/src"
    git clone -q -c advice.detachedHead=false --branch "$LSFG_BRANCH" "$LSFG_REPO" "$src"
fi
git -C "$src" cat-file -e "$LSFG_COMMIT^{commit}" 2>/dev/null ||
    git -C "$src" fetch -q origin "$LSFG_BRANCH"
git -C "$src" -c advice.detachedHead=false checkout -q "$LSFG_COMMIT"
git -C "$src" submodule update -q --init --recursive
[[ $(git -C "$src" rev-parse HEAD) == "$LSFG_COMMIT" ]] ||
    { echo "$src is not at $LSFG_COMMIT" >&2; exit 1; }
# Always the pinned source plus our patches (the submodules untouched), and the pinned source again
# when this script ends, so the checkout stays what the commit says.
reset_source() { git -C "$src" checkout -q -- . && git -C "$src" clean -qfd; }
reset_source
trap reset_source EXIT
for patch in tools/android/bloodframe-patches/*.patch; do
    git -C "$src" apply --whitespace=nowarn "$PWD/$patch"
done
mkdir -p "$out/lib" "$out/licenses"
if [[ ! -f $build/build.ninja ]]; then
    rm -rf "$build"
    cmake -S tools/android/lsfg -B "$build" -G Ninja -DLSFG_SRC="$src" \
        -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-$api -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release \
        > "$build.log" 2>&1 || { tail -30 "$build.log" >&2; rm -rf "$build"; exit 1; }
fi
run cmake --build "$build" -j "$J" --target bloodframe >> "$build.log" 2>&1 ||
    { grep -E "error|FAILED" "$build.log" | tail -40 >&2; exit 1; }
"$llvm/bin/llvm-strip" --strip-debug -o "$out/lib/libbloodframe.so" "$build/libbloodframe.so"
# The licences of what is linked in (the fork's toml11 is not).
cp "$src/LICENSE.md" "$out/licenses/bloodframe-lsfg-vk.LICENSE.md"
cp "$src/thirdparty/dxbc/LICENSE.md" "$out/licenses/bloodframe-dxbc.LICENSE"
cp "$src/thirdparty/pe-parse/LICENSE" "$out/licenses/bloodframe-pe-parse.LICENSE"
cp "$src/thirdparty/volk/LICENSE.md" "$out/licenses/bloodframe-volk.LICENSE.md"
printf 'Bloodframe: lsfg-vk 1.0 by PancakeTAS (MIT) via lsfg-vk-android %s (%s) + tools/android/bloodframe-patches\n' "$LSFG_COMMIT" "$LSFG_BRANCH" \
    > "$out/licenses/bloodframe-source.txt"
exported=$("$llvm/bin/llvm-nm" -D --defined-only "$out/lib/libbloodframe.so" | awk '{print $3}' | grep -v '^_' | sort | tr '\n' ' ')
echo "Bloodframe (lsfg-vk-android ${LSFG_COMMIT:0:7} + patches): $out/lib/libbloodframe.so (exports: $exported)"
