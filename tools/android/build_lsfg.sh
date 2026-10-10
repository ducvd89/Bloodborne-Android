#!/usr/bin/env bash
# Lossless Scaling frame generation for Android: lsfg-vk (https://lsfg-vk.dev, CC BY-NC-ND 4.0),
# a Vulkan layer, cross-compiled for the Thor's Debian trixie arm64 rootfs from its UNMODIFIED
# source (the licence allows no derivatives: only build options here, never patches).
# Output: out/arm64/lsfg/{liblsfg-vk-layer.so,VkLayer_LSFGVK_frame_generation.json,LICENSE.txt};
# the release runtime ships it as bbport/arm64/lsfg and run-thor.sh enables it (lsfg_multiplier).
# Lossless Scaling's shaders come from the player's own Lossless.dll at run time, never the APK.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
LSFG_TAG=${LSFG_TAG:-2.0.0}
src=$PWD/.local-deps/android/src/lsfg-vk-$LSFG_TAG
build=$PWD/out/arm64/lsfg-build
out=$PWD/out/arm64/lsfg
if [[ ! -d $src/.git ]]; then
    git clone -q -c advice.detachedHead=false --depth 1 --branch "$LSFG_TAG" --recurse-submodules \
        --shallow-submodules https://git.lsfg-vk.dev/lsfg-vk.git "$src"
fi
[[ $(git -C "$src" describe --tags --exact-match 2>/dev/null) == "$LSFG_TAG" ]] ||
    { echo "$src is not at $LSFG_TAG" >&2; exit 1; }
[[ -z $(git -C "$src" status --porcelain) ]] || { echo "$src has local changes" >&2; exit 1; }
rm -rf "$build" "$out"
# The layer library beside its manifest (a path with a separator is relative to the manifest).
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/tools/android/aarch64-toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DLSFGVK_BUILD_CLI=OFF -DLSFGVK_BUILD_UI=OFF \
    -DLSFGVK_LAYER_LIBRARY_PATH=./liblsfg-vk-layer.so > "$build.log" 2>&1 ||
    { tail -30 "$build.log" >&2; exit 1; }
ninja -C "$build" >> "$build.log" 2>&1 || { grep -v '^\[' "$build.log" | tail -30 >&2; exit 1; }
mkdir -p "$out"
llvm-strip --strip-debug -o "$out/liblsfg-vk-layer.so" "$build/lsfg-vk-layer/liblsfg-vk-layer.so"
cp "$build/lsfg-vk-layer/VkLayer_LSFGVK_frame_generation.json" "$src/LICENSE.txt" "$out/"
grep -q '"./liblsfg-vk-layer.so"' "$out/VkLayer_LSFGVK_frame_generation.json" ||
    { echo 'Unexpected layer manifest' >&2; exit 1; }
echo "lsfg-vk $LSFG_TAG ($(git -C "$src" rev-parse --short HEAD)): $out"
