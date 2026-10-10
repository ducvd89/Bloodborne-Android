#!/usr/bin/env bash
# Lossless Scaling frame generation for Android: lsfg-vk 1.0.0 (https://github.com/PancakeTAS/lsfg-vk,
# MIT), a Vulkan layer, cross-compiled for the Thor's Debian trixie arm64 rootfs with
# lsfg-patches/*.patch applied: Turnip on KGSL has no OPAQUE_FD semaphores, so the layer and the
# framegen device hand frames over with sync files, and both import the shared images with the
# same create info (Turnip picks tiling and UBWC from it). lsfg-vk 2.0 is CC BY-NC-ND (no modified
# builds) and needs timeline semaphores shared between devices, which KGSL cannot do.
# Output: out/arm64/lsfg/{liblsfg-vk.so,VkLayer_LS_frame_generation.json,LICENSE.md}; the release
# runtime ships it as bbport/arm64/lsfg and run-thor.sh enables it (lsfg_multiplier).
# Lossless Scaling's shaders come from the player's own Lossless.dll at run time, never the APK.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
LSFG_TAG=v1.0.0
LSFG_COMMIT=7113d7d
src=$PWD/.local-deps/android/src/lsfg-vk-build-$LSFG_TAG
build=$PWD/out/arm64/lsfg-build
out=$PWD/out/arm64/lsfg
if [[ ! -d $src/.git ]]; then
    git clone -q -c advice.detachedHead=false --depth 1 --branch "$LSFG_TAG" --recurse-submodules \
        --shallow-submodules https://github.com/PancakeTAS/lsfg-vk.git "$src"
fi
[[ $(git -C "$src" rev-parse --short=7 HEAD) == "$LSFG_COMMIT" ]] ||
    { echo "$src is not at $LSFG_TAG ($LSFG_COMMIT)" >&2; exit 1; }
# Always the pinned source plus our patches, nothing else.
git -C "$src" checkout -q -- . && git -C "$src" clean -qfd
for patch in tools/android/lsfg-patches/*.patch; do
    git -C "$src" apply --whitespace=nowarn "$PWD/$patch"
done
rm -rf "$build" "$out"
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/tools/android/aarch64-toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release > "$build.log" 2>&1 || { tail -30 "$build.log" >&2; exit 1; }
ninja -C "$build" lsfg-vk >> "$build.log" 2>&1 || { grep -v '^\[' "$build.log" | tail -40 >&2; exit 1; }
mkdir -p "$out"
llvm-strip --strip-debug -o "$out/liblsfg-vk.so" "$build/liblsfg-vk.so"
# An implicit layer: the framegen's own Vulkan instance must skip it (DISABLE_LSFG, set by the
# layer around its initialization), which a forced (VK_INSTANCE_LAYERS) layer would not.
sed 's|"library_path": "liblsfg-vk.so"|"library_path": "./liblsfg-vk.so"|' \
    "$src/VkLayer_LS_frame_generation.json" > "$out/VkLayer_LS_frame_generation.json"
grep -q '"./liblsfg-vk.so"' "$out/VkLayer_LS_frame_generation.json" ||
    { echo 'Unexpected layer manifest' >&2; exit 1; }
cp "$src/LICENSE.md" "$out/"
cp tools/android/lsfg-patches/*.patch "$out/"
echo "lsfg-vk $LSFG_TAG ($LSFG_COMMIT) + Turnip patches: $out"
