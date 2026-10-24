#!/usr/bin/env bash
# Downloads and builds what the self-contained bb-recomp needs, into .local-deps (ignored by git). Needs network the first time.
#   tools/recomp/fetch-deps.sh
# - LLVM 23.1.3: the official release tarball (Linux x86-64, static libraries for every target, headers, clang, lld). Used as the
#   compiler for the build, and its libraries (X86 + AArch64 backends, LLD's ELF linker) are linked into bb-recomp.
# - zlib 1.3.1 and zstd 1.5.7 as static libraries (LLVM's Support library wants both), built into .local-deps/deps/prefix.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
LLVM_VER=${BB_LLVM_VERSION:-23.1.3}
deps=${BB_RECOMP_DEPS:-$PWD/.local-deps}
llvm=$deps/llvm/x/LLVM-$LLVM_VER-Linux-X64
J=${BB_JOBS:-22}
mkdir -p "$deps/llvm/dl" "$deps/llvm/x" "$deps/deps/dl"

if [[ ! -x $llvm/bin/clang ]]; then
    f=$deps/llvm/dl/LLVM-$LLVM_VER-Linux-X64.tar.zst
    [[ -f $f ]] || curl -fL -C - -o "$f" "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/LLVM-$LLVM_VER-Linux-X64.tar.zst"
    # the archive uses a 1 GiB window: zstd needs --long=31 (tar --zstd refuses)
    (cd "$deps/llvm/x" && zstd -d --long=31 -c "$f" | nice -n 19 tar -x)
fi
[[ -f $deps/llvm/LICENSE.TXT ]] || curl -fL -o "$deps/llvm/LICENSE.TXT" "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-$LLVM_VER/llvm/LICENSE.TXT"

prefix=$deps/deps/prefix
if [[ ! -f $prefix/lib/libz.a ]]; then
    [[ -f $deps/deps/dl/zlib-1.3.1.tar.xz ]] || curl -fL -o "$deps/deps/dl/zlib-1.3.1.tar.xz" https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.xz
    mkdir -p "$deps/deps/src1" && tar -xf "$deps/deps/dl/zlib-1.3.1.tar.xz" -C "$deps/deps/src1"
    nice -n 19 cmake -S "$deps/deps/src1/zlib-1.3.1" -B "$deps/deps/b1" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_C_COMPILER="$llvm/bin/clang" >/dev/null
    nice -n 19 cmake --build "$deps/deps/b1" -j "$J" >/dev/null && cmake --install "$deps/deps/b1" >/dev/null
    rm -f "$prefix"/lib/libz.so*   # only the static library: the executable is linked statically
fi
if [[ ! -f $prefix/lib/libzstd.a ]]; then
    [[ -f $deps/deps/dl/zstd-1.5.7.tar.gz ]] || curl -fL -o "$deps/deps/dl/zstd-1.5.7.tar.gz" https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    mkdir -p "$deps/deps/src2" && tar -xf "$deps/deps/dl/zstd-1.5.7.tar.gz" -C "$deps/deps/src2"
    nice -n 19 cmake -S "$deps/deps/src2/zstd-1.5.7/build/cmake" -B "$deps/deps/b2" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_C_COMPILER="$llvm/bin/clang" -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON >/dev/null
    nice -n 19 cmake --build "$deps/deps/b2" -j "$J" >/dev/null && cmake --install "$deps/deps/b2" >/dev/null
fi
echo "LLVM: $llvm"
echo "deps prefix: $prefix"
