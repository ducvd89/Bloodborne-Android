#!/usr/bin/env bash
# Bionic (Android NDK) build of the native game process, the Android app's only runtime: libbbcpu.so
# (the ahead-of-time unit engine: the game runs as native code from libbbaot.so, no FEXCore, no JIT), libbbgpu.so (GPU/HLE), libbbnet20.so (online play: shadNet / The Hunter's Dream, loaded only with
# BB_ONLINE=1), libbloodframe.so (Lossless Scaling frame generation, build_bloodframe.sh),
# bb-probe (runtime executable), kgsl-chip-id, bb-prepare (game preparation) and ps4-pkg-tool (PKG
# installs). Output: out/bionic/{bin,lib,licenses}; build_release_runtime.py packages them.
# Prerequisite: tools/android/build_bionic_deps.sh (NDK + third-party libraries).
# STL: c++_shared for all components (libbbcpu/libbbgpu/bb-probe share one libc++_shared.so; static
# libc++ in several DSOs would give each its own exception/RTTI/new state).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
out=out/bionic
root=$PWD/.local-deps/android
ndk=${BB_NDK:-$root/sdk/ndk/29.0.14206865}
api=${BB_ANDROID_API:-30}
prefix=$root/bionic-prefix
# Header-only packages (magic_enum, robin-map, VMA, Boost) from the host build's dependencies.
headers=${BB_ARM64_HEADERS:-$PWD/.local-deps/root/usr}
toolchain=$ndk/build/cmake/android.toolchain.cmake
llvm=$ndk/toolchains/llvm/prebuilt/linux-x86_64
J=${BB_JOBS:-22}
run() { taskset -c 2-23 nice -n 19 "$@"; }
[[ -f $toolchain ]] || { echo "No NDK: $ndk (tools/android/build_bionic_deps.sh)" >&2; exit 1; }
[[ -f $prefix/.done-ffmpeg ]] || { echo "No Bionic deps in $prefix (tools/android/build_bionic_deps.sh)" >&2; exit 1; }
[[ -f $prefix/.done-openssl && -x $root/host-tools/bin/protoc && -f $prefix/share/cacert.pem ]] \
    || { echo "No online-module deps (OpenSSL, protobuf, CA bundle) in $prefix (tools/android/build_bionic_deps.sh)" >&2; exit 1; }
[[ -d $headers/share/cmake/magic_enum ]] || { echo "No header-only packages in $headers" >&2; exit 1; }
mkdir -p "$out"
ndk_cmake=(-DCMAKE_TOOLCHAIN_FILE="$toolchain" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$api -DANDROID_STL=c++_shared)
# This port's changes to FSR-Vulkan (gpu/patches/fsr-vulkan), applied once, as build.sh does.
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
    fi
done

# Guest CPU: libbbcpu.so is the ahead-of-time unit engine and its platform services (cpu/CMakeLists.txt): no FEXCore, nothing but libc/libdl.
# Built first: libbbnet20 is linked against it.
if [[ ! -f $out/cpu/build.ninja ]]; then
    cmake -S cpu -B "$out/cpu" -G Ninja "${ndk_cmake[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
fi
run ninja -C "$out/cpu" -j "$J" bbcpu > "$out/cpu-build.log" 2>&1 || { grep -E "error|FAILED" "$out/cpu-build.log" | head -40 >&2; exit 1; }

# GPU library: no libvulkan (gpu/shim/vk_driver_loader.cpp dlopens the driver), no X11. glslang
# (host program) compiles the host shaders.
export PKG_CONFIG_PATH=$prefix/lib/pkgconfig:$prefix/share/pkgconfig
export PKG_CONFIG_LIBDIR=$PKG_CONFIG_PATH
if [[ ! -f $out/gpu/build.ninja ]]; then
    cmake -S gpu -B "$out/gpu" -G Ninja "${ndk_cmake[@]}" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO=off -DBB_LTO=OFF \
        "-DCMAKE_FIND_ROOT_PATH=$prefix;$headers" -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
        -DCMAKE_PREFIX_PATH="$prefix;$headers" \
        -Dmagic_enum_DIR="$headers/share/cmake/magic_enum" \
        -Dtsl-robin-map_DIR="$headers/share/cmake/tsl-robin-map" \
        -DVulkanMemoryAllocator_DIR="$headers/share/cmake/VulkanMemoryAllocator" \
        -DBoost_DIR="$headers/lib/cmake/Boost-1.92.0" \
        -Dboost_headers_DIR="$headers/lib/cmake/boost_headers-1.92.0" >/dev/null
fi
# The online module's options (an existing build directory picks them up here): its static dependencies are in
# the prefix, and the protoc that generates the shadNet protocol runs on this PC (the same version as the library).
# BB_NET_BBSC: BBSC (seamless co-op, BB_BBSC=1 at run time) is part of the module; an old build directory has the
# option cached as OFF.
cmake -S gpu -B "$out/gpu" -DBB_NET=ON -DBB_NET_BBSC=ON "-DBB_HOST_PROTOC=$root/host-tools/bin/protoc" "-DBB_CPU_LIB=$PWD/$out/cpu/libbbcpu.so" >/dev/null
run ninja -C "$out/gpu" -j "$J" bbgpu bbnet20 > "$out/gpu-build.log" 2>&1 || { grep -v '^\[' "$out/gpu-build.log" | tail -40 >&2; exit 1; }

# bin/ and lib/ side by side, executables find lib/ through $ORIGIN.
mkdir -p "$out/bin" "$out/lib"
cp -f "$out/cpu/libbbcpu.so" "$out/gpu/libbbgpu.so" "$out/gpu/libbbnet20.so" "$out/lib/"
# Mozilla's CA bundle (build_bionic_deps.sh): the online module's TLS trust (BB_CA_BUNDLE overrides the default
# path, <runtime>/share/cacert.pem beside lib/); MPL-2.0, which the bundle's own header names.
mkdir -p "$out/share" "$out/licenses"
cp -f "$prefix/share/cacert.pem" "$out/share/cacert.pem"
# Licences of what libbbnet20.so contains besides this project's own (OpenSSL, protobuf, miniupnpc, nlohmann/json,
# cpp-httplib), shipped with it.
src=$root/bionic-src
cp -f "$src/openssl-3.5.4/LICENSE.txt" "$out/licenses/openssl.LICENSE.txt"
cp -f "$src/protobuf-3.21.12/LICENSE" "$out/licenses/protobuf.LICENSE"
cp -f "$src/miniupnpc-2.3.3/LICENSE" "$out/licenses/miniupnpc.LICENSE"
cp -f "$src/nlohmann-json-3.12.0/LICENSE.MIT" "$out/licenses/nlohmann-json.LICENSE.MIT"
cp -f gpu/third_party/httplib/LICENSE "$out/licenses/httplib.LICENSE"
printf 'cacert.pem: Mozilla CA Certificate Store as prepared by curl (https://curl.se/docs/caextract.html),\nsnapshot 2026-09-25, sha256 a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505.\nLicense: MPL-2.0 (https://www.mozilla.org/MPL/2.0/).\n' > "$out/licenses/cacert-source.txt"
# libSDL3.so only while libbbgpu still links it (the AHardwareBuffer present path removes that);
# bb-probe does not: src/ is built with BB_NO_SDL on Android (audio to the app's FIFO, pad from BB_PAD_FILE).
rm -f "$out/lib/libSDL3.so"
if "$llvm/bin/llvm-readelf" -d "$out/lib/libbbgpu.so" | grep -q 'libSDL3'; then
    cp -f "$prefix/lib/libSDL3.so" "$out/lib/"
fi
cp -f "$llvm/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$out/lib/"

cc=("$llvm/bin/aarch64-linux-android$api-clang" -fuse-ld=lld)
mkdir -p "$out/atrac9"
for source in third_party/LibAtrac9/C/src/*.c; do
    "${cc[@]}" -std=c99 -O2 -g -w -fPIC -c "$source" -o "$out/atrac9/$(basename "${source%.c}").o"
done
"$llvm/bin/llvm-ar" rcs "$out/libatrac9.a" "$out"/atrac9/*.o
# PIE (Android requires it; the PC build is -no-pie for its brk heap, see src/probe.c main),
# -rdynamic: libbbgpu resolves runtime_* symbols against the executable. Linked against the NDK
# sysroot only: no libvulkan, no loader of ours.
links=(-L"$out/lib" -lbbgpu -lbbcpu -Wl,-rpath,'$ORIGIN/../lib' -rdynamic -pie)
"${cc[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I"$prefix/include" -I. -Isrc \
    src/probe.c src/runtime*.c src/vulkan_smoke.c "$out/libatrac9.a" -lm "${links[@]}" \
    -o "$out/bin/bb-probe"
"${cc[@]}" -std=c11 -O2 -Wall -Wextra -Werror tools/android/kgsl_chip_id.c -pie -o "$out/bin/kgsl-chip-id"
# bb-prepare: the game-preparation step (port of prepare_game.py), no Python or rootfs on the device.
tools/android/build_prepare.sh android
# libbloodframe.so: Lossless Scaling frame generation (GameNative's lsfg-vk fork's framegen), which libbbgpu
# loads with dlopen only when it is on. The ducvd89/bloodframe repository, cloned at a pinned commit into
# .local-deps/android/src.
tools/android/build_bloodframe.sh
# ps4-pkg-tool: unpacks PKGs for the app's game and mod installers (the app runs it from the runtime).
tools/android/build_pkg_tool.sh android
echo "Built $out/bin/bb-probe, $out/lib/libbbgpu.so, $out/lib/libbbcpu.so, $out/lib/libbbnet20.so (Bionic, API $api)"
tools/android/check_bionic_symbols.sh
