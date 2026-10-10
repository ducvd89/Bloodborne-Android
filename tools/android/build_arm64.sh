#!/usr/bin/env bash
# Native ARM64 build for the Thor's Linux userland (Debian trixie arm64), cross-compiled with Clang:
# the runtime and the GPU library run natively, the game's x86-64 code in FEXCore (libbbcpu.so).
# Fetch the sysroot first: python3 tools/android/fetch_arm64_sysroot.py
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
out=out/arm64
sysroot=${BB_ARM64_SYSROOT:-$PWD/.local-deps/android/arm64-sysroot}
prefix=$PWD/.local-deps/android/arm64-prefix
# Header-only packages (magic_enum, robin-map, VMA, Boost) from the host build's dependencies.
headers=${BB_ARM64_HEADERS:-$PWD/.local-deps/root/usr}
toolchain=$PWD/tools/android/aarch64-toolchain.cmake
[[ -f $sysroot/usr/lib/aarch64-linux-gnu/libc.so.6 ]] || { echo "No sysroot: $sysroot" >&2; exit 1; }
[[ -f $prefix/lib/libminiz.a ]] || { echo "No ARM64 miniz in $prefix (see docs/ANDROID_THOR.md)" >&2; exit 1; }
[[ -d $headers/share/cmake/magic_enum ]] || { echo "No header-only packages in $headers" >&2; exit 1; }
mkdir -p "$out"
# This port's changes to FSR-Vulkan (gpu/patches/fsr-vulkan), applied once, as build.sh does.
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
    fi
done

if [[ ! -f $out/fex/build.ninja ]]; then
    cmake -S third_party/FEX -B "$out/fex" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_PROJECT_FEX_INCLUDE="$PWD/cpu/fex_project.cmake" \
        -DBUILD_TESTING=OFF -DBUILD_THUNKS=OFF -DBUILD_FEXCONFIG=OFF -DBUILD_STEAM_SUPPORT=OFF \
        -DBUILD_FEX_LINUX_TESTS=OFF -DENABLE_ZYDIS=OFF -DTUNE_ARCH=armv8.2-a -DTUNE_CPU=generic >/dev/null
fi
ninja -C "$out/fex" bbcpu > "$out/fex-build.log" 2>&1 || { tail -40 "$out/fex-build.log" >&2; exit 1; }

if [[ ! -f $out/gpu/build.ninja ]]; then
    lib=$sysroot/usr/lib/aarch64-linux-gnu/cmake
    cmake -S gpu -B "$out/gpu" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO=off "-DBB_ARM64_HEADER_PREFIX=$prefix;$headers" \
        -Dminiz_DIR="$prefix/lib/cmake/miniz" -DZydis_DIR="$lib/zydis" -DZycore_DIR="$lib/zycore" \
        -Dfmt_DIR="$lib/fmt" -Dmagic_enum_DIR="$headers/share/cmake/magic_enum" \
        -Dtsl-robin-map_DIR="$headers/share/cmake/tsl-robin-map" \
        -DVulkanMemoryAllocator_DIR="$headers/share/cmake/VulkanMemoryAllocator" \
        -DBoost_DIR="$headers/lib/cmake/Boost-1.92.0" \
        -Dboost_headers_DIR="$headers/lib/cmake/boost_headers-1.92.0" >/dev/null
fi
ninja -C "$out/gpu" bbgpu > "$out/gpu-build.log" 2>&1 || { grep -v '^\[' "$out/gpu-build.log" | tail -40 >&2; exit 1; }

cc=(clang --target=aarch64-linux-gnu --sysroot="$sysroot" -fuse-ld=lld)
export PKG_CONFIG_SYSROOT_DIR=$sysroot
export PKG_CONFIG_LIBDIR=$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig:$sysroot/usr/share/pkgconfig
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
mkdir -p "$out/atrac9"
for source in third_party/LibAtrac9/C/src/*.c; do
    "${cc[@]}" -std=c99 -O2 -g -w -c "$source" -o "$out/atrac9/$(basename "${source%.c}").o"
done
ar rcs "$out/libatrac9.a" "$out"/atrac9/*.o
# Next to bin/: lib/ holds libbbgpu, libbbcpu and the libraries the Thor's rootfs lacks.
# Android has no /lib: the interpreter is the fexdroid rootfs's, by absolute path (as its own programs).
rootfs=${BB_THOR_ROOTFS:-/data/data/com.ducvd89.bloodborne/files/rootfs}
links=(-L"$out/gpu" -lbbgpu -L"$out/fex" -lbbcpu -Wl,-rpath,'$ORIGIN/../lib'
       -Wl,--dynamic-linker="$rootfs/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1" -rdynamic)
"${cc[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -no-pie "${includes[@]}" -I. -Isrc \
    src/probe.c src/runtime*.c src/vulkan_smoke.c "$out/libatrac9.a" -lm "${links[@]}" "${libraries[@]}" \
    -o "$out/bb-probe"
# run-thor.sh asks it for the GPU's chip id to pick the Turnip variant (Adreno 8xx: gen8).
"${cc[@]}" -std=c11 -O2 -Wall -Wextra -Werror tools/android/kgsl_chip_id.c \
    -Wl,--dynamic-linker="$rootfs/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1" \
    -o "$out/kgsl-chip-id"
echo "Built $out/bb-probe (aarch64)"
