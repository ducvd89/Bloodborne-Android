#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Linux release of the Bloodborne Android Rebuild Tool: out/dist/Bloodborne-Android-Rebuild-Tool-x86_64.AppImage
#   tools/recomp/package.sh [--test <folder with the game's PKGs> [--compare <libbbaot.so of a dev build>]]
#
# The AppImage holds the program (window and command line in one binary: Dear ImGui + SDL3, LLVM 23 with the X86 and AArch64 code generators
# and LLD linked in statically), ps4-pkg-tool, the AArch64 x87 helper object and the link stubs. It is built against an Ubuntu 22.04 sysroot,
# so it needs glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36 ...) and nothing else from the system but what a desktop has (X11 or
# Wayland, OpenGL, which SDL loads when it starts): no LLVM, clang, NDK or lld. The build runs at nice 19 with 22 jobs.
# Needs: cmake, ninja, git, curl, zstd, tar, python3 (with Pillow for the icon), ar; the Android NDK r29 (BB_NDK, or .local-deps/android/sdk/ndk/
# 29.0.14206865 of this or the main checkout) for the AArch64 helper object, the stubs and the builtins.
# FEX_SOURCE_DIR (a FEX 2609.1 tree with its submodules) is optional: fex_project's default (fetch-fex.sh) makes one under out/.
# --test runs the AppImage in a clean environment (env -i; no LLVM or NDK on PATH) on the PKGs and checks the result; --compare also requires
# the .so to match that file (byte-identical, or identical in everything the phone loads: see the end of this script).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
root=$PWD
test_pkgs=; compare=
while (( $# )); do
    case $1 in
        --test) test_pkgs=$2; shift 2 ;;
        --compare) compare=$2; shift 2 ;;
        *) echo "usage: $0 [--test <PKG folder> [--compare <dev libbbaot.so>]]" >&2; exit 2 ;;
    esac
done
J=${BB_JOBS:-22}
run() { nice -n 19 "$@"; }
VERSION=${BB_RECOMP_VERSION:-0.1.0}
APPNAME=Bloodborne-Android-Rebuild-Tool-x86_64.AppImage

# The NDK: this worktree's .local-deps, else the main checkout's (git worktrees share the common dir).
ndk=${BB_NDK:-}
if [[ -z $ndk ]]; then
    common=$(cd "$(git rev-parse --git-common-dir)/.." && pwd)
    for c in "$root/.local-deps/android/sdk/ndk/29.0.14206865" "$common/.local-deps/android/sdk/ndk/29.0.14206865"; do
        [[ -d $c ]] && { ndk=$c; break; }
    done
fi
[[ -d $ndk ]] || { echo "No Android NDK (set BB_NDK, or run tools/android/build_bionic_deps.sh)" >&2; exit 1; }
nbin=$ndk/toolchains/llvm/prebuilt/linux-x86_64
builtins=$(ls "$nbin"/lib/clang/*/lib/linux/libclang_rt.builtins-aarch64-android.a | head -n 1)

tools/recomp/fetch-deps.sh
deps=$root/.local-deps
llvm=$deps/llvm/x/LLVM-${BB_LLVM_VERSION:-23.1.3}-Linux-X64
sysroot=$deps/sysroot-jammy
prefix=$deps/deps/prefix-jammy
imgui=$(ls -d "$deps"/imgui/imgui-* | head -n 1)

# 1. bb-recomp: window + command line, against the sysroot
fexargs=()
[[ -n ${FEX_SOURCE_DIR:-} ]] && fexargs+=(-DFEX_SOURCE_DIR="$FEX_SOURCE_DIR")
run cmake -S tools/recomp -B out/recomp-linux -G Ninja "${fexargs[@]}" -DBB_RECOMP_BUNDLED=ON -DBB_RECOMP_VERSION="$VERSION" \
    -DBB_RECOMP_TOOLCHAIN_FILE="$root/tools/recomp/linux-jammy-toolchain.cmake" -DBB_SYSROOT="$sysroot" -DBB_LLVM_ROOT="$llvm" \
    -DBB_RECOMP_LLVM_DIR="$llvm/lib/cmake/llvm" -DBB_RECOMP_DEPS_PREFIX="$prefix" -DBB_RECOMP_SDL_PREFIX="$deps/sdl/prefix-linux" -DBB_RECOMP_IMGUI_DIR="$imgui" \
    -DBB_RECOMP_JOBS="$J" > out/recomp-linux-cfg.log 2>&1 || { tail -20 out/recomp-linux-cfg.log; exit 1; }
run cmake --build out/recomp-linux -j "$J"

# 2. ps4-pkg-tool (the app's PKG unpacker), against the same sysroot
BB_SYSROOT=$sysroot BB_LLVM_ROOT=$llvm BB_ZLIB_PREFIX=$prefix tools/android/build_pkg_tool.sh linux

# 3. the AppDir
dist=out/dist/AppDir
rm -rf "$dist"; mkdir -p "$dist/usr/bin" "$dist/usr/share/bb-recomp" "$dist/usr/share/licenses/bb-recomp" "$dist/usr/share/applications" "$dist/usr/share/icons/hicolor/256x256/apps"
share=$dist/usr/share/bb-recomp
# the files the .so link needs, from the NDK: the same commands compile.cpp runs for a developer build
run "$nbin/bin/clang" --target=aarch64-linux-android30 -O2 -fPIC -fvisibility=hidden -fno-stack-protector -ffp-contract=off -c \
    tools/recomp/helpers/bbr_x87_all.c -o "$share/helpers-aarch64.o"
# libbbcpu.so: a DT_NEEDED entry only (see compile.cpp: the real libbbcpu.so is already loaded when the game starts)
run "$nbin/bin/clang" --target=aarch64-linux-android30 -shared -nostdlib -Wl,-soname,libbbcpu.so -x c /dev/null -o "$share/libbbcpu-stub.so"
# libm.so: the NDK's own link stub (no code), so that the .so gets libm's DT_NEEDED and symbol versions exactly as a developer build
cp "$nbin/sysroot/usr/lib/aarch64-linux-android/30/libm.so" "$share/libm.so"
cp "$builtins" "$share/"
[[ -d tools/recomp/variants ]] && cp -r tools/recomp/variants "$share/variants"
strip -o "$dist/usr/bin/bb-recomp" out/recomp-linux/bb-recomp
strip -o "$dist/usr/bin/ps4-pkg-tool" out/linux/pkg-tool/ps4-pkg-tool
chmod 755 "$dist/usr/bin/"*
lic=$dist/usr/share/licenses/bb-recomp
fex=$(sed -n 's/^FEX_SOURCE_DIR:PATH=//p' out/recomp-linux/CMakeCache.txt)
[[ -f $fex/LICENSE ]] || fex=$(sed -n 's/^FEX_GIT_DIR:PATH=//p' out/recomp-linux/CMakeCache.txt)
cp "$fex/LICENSE" "$lic/FEX-MIT.txt"
cp "$deps/llvm/LICENSE.TXT" "$lic/LLVM-Apache-2.0-with-LLVM-exception.txt"
cp tools/recomp/helpers/NOTICE "$lic/x87-helpers-NOTICE.txt"
cp tools/recomp/LICENSE "$lic/GPL-3.0.txt"
cp tools/recomp/NOTICES "$lic/NOTICES.txt"
cp docs/NATIVE_CPU.md "$dist/usr/share/bb-recomp/NATIVE_CPU.md" 2>/dev/null || true
echo "$VERSION" > "$dist/usr/share/bb-recomp/VERSION"
cat > "$dist/AppRun" <<'APPRUN'
#!/bin/sh
# Double click: the window. With arguments (--pkgs <dir> --out <dir>, or --cli ...): the command line.
HERE=$(dirname "$(readlink -f "$0")")
exec "$HERE/usr/bin/bb-recomp" "$@"
APPRUN
chmod 755 "$dist/AppRun"
cat > "$dist/bb-recomp.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=Bloodborne Android Rebuild Tool
Comment=Builds the game folder and the native CPU files for the Bloodborne Android app from your own PKG files
Exec=bb-recomp
Icon=bb-recomp
Categories=Utility;
Terminal=false
DESKTOP
cp "$dist/bb-recomp.desktop" "$dist/usr/share/applications/"
# A simple generated icon (a chip: gold on dark); no game or console artwork.
python3 - "$dist/bb-recomp.png" <<'PY'
import sys
from PIL import Image, ImageDraw
S = 256; im = Image.new('RGBA', (S, S), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
gold, dark = (198, 173, 118, 255), (11, 12, 16, 255)
d.rounded_rectangle((8, 8, S - 8, S - 8), 40, fill=dark, outline=gold, width=6)
d.rectangle((68, 68, S - 68, S - 68), fill=(30, 26, 18, 255), outline=gold, width=8)
for i in range(5):
    t = 84 + i * 22
    for (a, b, c, e) in ((t, 36, t + 10, 68), (t, S - 68, t + 10, S - 36), (36, t, 68, t + 10), (S - 68, t, S - 36, t + 10)): d.rectangle((a, b, c, e), fill=gold)
d.rectangle((100, 100, S - 100, S - 100), fill=gold)
im.save(sys.argv[1])
PY
cp "$dist/bb-recomp.png" "$dist/usr/share/icons/hicolor/256x256/apps/bb-recomp.png"

# 4. the AppImage
appimagetool=${BB_APPIMAGETOOL:-$deps/appimage/appimagetool-x86_64.AppImage}
if [[ ! -x $appimagetool ]]; then
    mkdir -p "$deps/appimage"
    curl -fL -o "$deps/appimage/appimagetool-x86_64.AppImage" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$deps/appimage/appimagetool-x86_64.AppImage"; appimagetool=$deps/appimage/appimagetool-x86_64.AppImage
fi
mkdir -p out/dist
rm -f "out/dist/$APPNAME"
ARCH=x86_64 run "$appimagetool" --appimage-extract-and-run --no-appstream "$dist" "out/dist/$APPNAME" > out/appimagetool.log 2>&1 || { tail -20 out/appimagetool.log; exit 1; }
ls -l "out/dist/$APPNAME"
echo "needs glibc: $(objdump -T "$dist/usr/bin/bb-recomp" "$dist/usr/bin/ps4-pkg-tool" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n 1) (or newer)"

# 5. test in a clean environment (XAUTHORITY is passed on: every AppImage runtime, this one included, fails on a host where DISPLAY is set but X
# authorisation is missing from the environment)
if [[ -n $test_pkgs ]]; then
    t=$(mktemp -d "$root/out/bb-recomp-clean.XXXXXX")
    echo "clean-environment test in $t (env -i, PATH=/usr/bin:/bin, no LLVM or NDK)"
    ( cd "$t" && TIMEFORMAT='wall %R s, user %U s' && time env -i HOME="$t" PATH=/usr/bin:/bin DISPLAY="${DISPLAY:-}" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}" \
        XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-}" XAUTHORITY="${XAUTHORITY:-}" BB_PKG_TOOL="${BB_PKG_TOOL:-}" "$root/out/dist/$APPNAME" --appimage-extract-and-run --pkgs "$test_pkgs" --out "$t/out" ) 2>&1 | tail -n 30
    so=$(ls "$t"/out/*/aot/libbbaot.so)
    [[ -f $so && -f ${so%.so}.json ]] || { echo "TEST FAILED: no libbbaot.so" >&2; exit 1; }
    if [[ -n $compare ]]; then
        if cmp -s "$so" "$compare"; then echo "TEST OK: byte-identical to $compare"
        else
            # A developer build links with the NDK's LLD 21, the AppImage with LLD 23: the .comment string differs, which moves the
            # (unloaded) .symtab and e_shoff. Everything that is loaded must be identical.
            loadend=0
            while read -r off filesz; do e=$(( off + filesz )); (( e > loadend )) && loadend=$e; done < <(readelf -lW "$so" | awk '$1=="LOAD"{print $2, $5}')
            if [[ -z $(cmp -l -n "$loadend" "$so" "$compare" | awk '$1<41||$1>48') ]]; then echo "TEST OK: the loaded part ($loadend bytes) is identical to $compare (only e_shoff and the .comment/.symtab tail differ: LLD version)"
            else echo "TEST FAILED: differs from $compare in the loaded part" >&2; exit 1; fi
        fi
    fi
    echo "kept: $t"
fi
