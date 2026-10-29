#!/usr/bin/env bash
# bb-prepare: the native game-preparation step (tools/android/prepare/, a port of prepare_game.py).
#   build_prepare.sh [android|host|all]   (default: all)
# android: out/bionic/bin/bb-prepare (arm64, PIE, NEEDED: Bionic libc/libm/libdl only; libc++ and
#          zlib are linked statically; needs the NDK of build_bionic_deps.sh only).
# host:    out/host/bb-prepare, for tests/test_prepare_native.py (system clang++ and zlib).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
what=${1:-all}
src=tools/android/prepare
root=${BB_DEPS_ROOT:-$PWD/.local-deps/android}
ndk=${BB_NDK:-$root/sdk/ndk/29.0.14206865}
api=${BB_ANDROID_API:-30}
llvm=$ndk/toolchains/llvm/prebuilt/linux-x86_64
J=${BB_JOBS:-22}
run() { taskset -c 2-23 nice -n 19 "$@"; }
flags=(-std=c++17 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter)

# build OUT CXX LINK_ARGS...: one object per source file, built in parallel, then linked.
build() {
    local out=$1 cxx=$2 objs=$1.obj
    shift 2
    mkdir -p "$objs" "$(dirname "$out")"
    local f
    for f in "$src"/*.cpp; do
        run "$cxx" "${flags[@]}" "${cflags[@]}" -c "$f" -o "$objs/$(basename "$f" .cpp).o" &
        while (( $(jobs -r | wc -l) >= J )); do wait -n || exit 1; done
    done
    wait || exit 1
    run "$cxx" "${cflags[@]}" "$objs"/*.o "$@" -o "$out"
}

if [[ $what == android || $what == all ]]; then
    [[ -d $llvm ]] || { echo "No NDK: $ndk (tools/android/build_bionic_deps.sh)" >&2; exit 1; }
    cflags=(-fPIE)
    build out/bionic/bin/bb-prepare "$llvm/bin/aarch64-linux-android$api-clang++" \
        -fuse-ld=lld -pie -static-libstdc++ -Wl,-z,max-page-size=16384 -Wl,--gc-sections -Wl,-s \
        "$llvm/sysroot/usr/lib/aarch64-linux-android/libz.a"
    echo "Built out/bionic/bin/bb-prepare (Bionic, API $api)"
    # bb-texpack: finds a texture pack in the game folder (run-bionic.sh: the larger graphics heaps).
    run "$llvm/bin/aarch64-linux-android$api-clang++" "${flags[@]}" -fPIE tools/android/texpack/texpack.cpp \
        -fuse-ld=lld -pie -static-libstdc++ -Wl,-z,max-page-size=16384 -Wl,--gc-sections -Wl,-s \
        "$llvm/sysroot/usr/lib/aarch64-linux-android/libz.a" -o out/bionic/bin/bb-texpack
    echo "Built out/bionic/bin/bb-texpack (Bionic, API $api)"
fi
if [[ $what == host || $what == all ]]; then
    cflags=()
    build out/host/bb-prepare "${CXX:-clang++}" -lz
    echo "Built out/host/bb-prepare (host)"
fi
