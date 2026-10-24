#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Downloads and builds what the Linux build of the Rebuild Tool needs, into .local-deps (ignored by git). Needs network the first time.
#   tools/recomp/fetch-deps.sh
# - LLVM 23.1.3: the official release tarball (Linux x86-64: static libraries for every target, headers, clang, lld). clang is the compiler;
#   the X86 + AArch64 code generators and LLD's ELF linker are linked into bb-recomp.
# - an Ubuntu 22.04 sysroot (glibc 2.35): everything is built against it, so the AppImage runs on glibc 2.35 and newer (fetch-sysroot.py).
# - zlib 1.3.1, zstd 1.5.7 and SDL3 3.4.18 as static libraries built against that sysroot; Dear ImGui (the commit gpu/third_party/imgui pins).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
LLVM_VER=${BB_LLVM_VERSION:-23.1.3}
SDL_VER=${BB_SDL_VERSION:-3.4.18}
IMGUI_COMMIT=${BB_IMGUI_COMMIT:-f1cc2ae15e53a861a874c3034aae6798fde194ab}
deps=${BB_RECOMP_DEPS:-$PWD/.local-deps}
llvm=$deps/llvm/x/LLVM-$LLVM_VER-Linux-X64
sysroot=$deps/sysroot-jammy
prefix=$deps/deps/prefix-jammy
J=${BB_JOBS:-22}
mkdir -p "$deps/llvm/dl" "$deps/llvm/x" "$deps/deps/dl" "$deps/sdl/dl" "$deps/sdl/src" "$deps/imgui/dl"
toolchain=(-DCMAKE_TOOLCHAIN_FILE="$PWD/tools/recomp/linux-jammy-toolchain.cmake" -DBB_SYSROOT="$sysroot" -DBB_LLVM_ROOT="$llvm")

if [[ ! -x $llvm/bin/clang ]]; then
    f=$deps/llvm/dl/LLVM-$LLVM_VER-Linux-X64.tar.zst
    [[ -f $f ]] || curl -fL -C - -o "$f" "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/LLVM-$LLVM_VER-Linux-X64.tar.zst"
    # the archive uses a 1 GiB window: zstd needs --long=31 (tar --zstd refuses)
    (cd "$deps/llvm/x" && zstd -d --long=31 -c "$f" | nice -n 19 tar -x)
fi
[[ -f $deps/llvm/LICENSE.TXT ]] || curl -fL -o "$deps/llvm/LICENSE.TXT" "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-$LLVM_VER/llvm/LICENSE.TXT"
[[ -f $sysroot/usr/include/GL/glx.h && -d $sysroot/usr/include/fribidi ]] || nice -n 19 python3 tools/recomp/fetch-sysroot.py "$sysroot"

if [[ ! -f $prefix/lib/libz.a ]]; then
    [[ -f $deps/deps/dl/zlib-1.3.1.tar.xz ]] || curl -fL -o "$deps/deps/dl/zlib-1.3.1.tar.xz" https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.xz
    mkdir -p "$deps/deps/src1" && tar -xf "$deps/deps/dl/zlib-1.3.1.tar.xz" -C "$deps/deps/src1"
    rm -rf "$deps/deps/b1j"
    nice -n 19 cmake -S "$deps/deps/src1/zlib-1.3.1" -B "$deps/deps/b1j" -G Ninja "${toolchain[@]}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" >/dev/null
    nice -n 19 cmake --build "$deps/deps/b1j" -j "$J" >/dev/null && cmake --install "$deps/deps/b1j" >/dev/null
    rm -f "$prefix"/lib/libz.so*   # only the static library
fi
if [[ ! -f $prefix/lib/libzstd.a ]]; then
    [[ -f $deps/deps/dl/zstd-1.5.7.tar.gz ]] || curl -fL -o "$deps/deps/dl/zstd-1.5.7.tar.gz" https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    mkdir -p "$deps/deps/src2" && tar -xf "$deps/deps/dl/zstd-1.5.7.tar.gz" -C "$deps/deps/src2"
    rm -rf "$deps/deps/b2j"
    nice -n 19 cmake -S "$deps/deps/src2/zstd-1.5.7/build/cmake" -B "$deps/deps/b2j" -G Ninja "${toolchain[@]}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON >/dev/null
    nice -n 19 cmake --build "$deps/deps/b2j" -j "$J" >/dev/null && cmake --install "$deps/deps/b2j" >/dev/null
fi

# SDL3, static, X11 and Wayland (both loaded with dlopen at run time, so the AppImage needs neither installed at build or at run time
# beyond what the desktop has). jammy's libxkbcommon (1.4) lacks the XKB_VMOD_NAME_* names SDL's Wayland code wants: defined here.
sdlsrc=$deps/sdl/src/SDL3-$SDL_VER
if [[ ! -f $deps/sdl/prefix-linux/lib/libSDL3.a ]]; then
    [[ -f $deps/sdl/dl/SDL3-$SDL_VER.tar.gz ]] || curl -fL -o "$deps/sdl/dl/SDL3-$SDL_VER.tar.gz" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL3-$SDL_VER.tar.gz"
    [[ -d $sdlsrc ]] || tar -xf "$deps/sdl/dl/SDL3-$SDL_VER.tar.gz" -C "$deps/sdl/src"
    rm -rf "$deps/sdl/build-linux"
    cat > "$deps/sdl/xkb_compat.h" <<'H'
#define XKB_VMOD_NAME_ALT "Alt"
#define XKB_VMOD_NAME_SUPER "Super"
#define XKB_VMOD_NAME_LEVEL3 "LevelThree"
#define XKB_VMOD_NAME_LEVEL5 "LevelFive"
#define XKB_VMOD_NAME_NUM "NumLock"
H
    nice -n 19 cmake -S "$sdlsrc" -B "$deps/sdl/build-linux" -G Ninja "${toolchain[@]}" -DCMAKE_BUILD_TYPE=Release -DSDL_STATIC=ON -DSDL_SHARED=OFF \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_WAYLAND=ON -DSDL_X11=ON -DSDL_LIBTHAI=OFF -DSDL_KMSDRM=OFF -DSDL_PIPEWIRE=OFF -DSDL_PULSEAUDIO=OFF \
        -DSDL_ALSA=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_AUDIO=OFF -DSDL_CAMERA=OFF -DSDL_JOYSTICK=OFF -DSDL_HIDAPI=OFF -DSDL_HAPTIC=OFF \
        -DSDL_SENSOR=OFF -DSDL_LIBUDEV=OFF -DSDL_IBUS=OFF -DSDL_DBUS=ON -DSDL_DEPS_SHARED=ON \
        -DLIBC_HAS_STRLCPY=0 -DLIBC_HAS_STRLCAT=0 -DLIBC_HAS_WCSLCPY=0 -DLIBC_HAS_WCSLCAT=0 \
        -DHAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCHDIR=0 -DHAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCHDIR_NP=1 \
        "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG -include $deps/sdl/xkb_compat.h" \
        -DCMAKE_INSTALL_PREFIX="$deps/sdl/prefix-linux" >/dev/null
    nice -n 19 cmake --build "$deps/sdl/build-linux" -j "$J" >"$deps/sdl/build-linux.log" 2>&1 || { tail -20 "$deps/sdl/build-linux.log" >&2; exit 1; }
    cmake --install "$deps/sdl/build-linux" >/dev/null
fi
if ! ls "$deps"/imgui/imgui-*/imgui.cpp >/dev/null 2>&1; then
    [[ -f $deps/imgui/dl/imgui.tar.gz ]] || curl -fL -o "$deps/imgui/dl/imgui.tar.gz" "https://github.com/ocornut/imgui/archive/$IMGUI_COMMIT.tar.gz"
    tar -xf "$deps/imgui/dl/imgui.tar.gz" -C "$deps/imgui"
fi
echo "LLVM: $llvm"; echo "sysroot: $sysroot"; echo "deps prefix: $prefix"; echo "SDL3: $deps/sdl/prefix-linux"; ls -d "$deps"/imgui/imgui-*
