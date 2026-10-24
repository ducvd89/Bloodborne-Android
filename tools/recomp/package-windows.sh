#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Windows release of the Bloodborne Android Rebuild Tool on Linux, cross-compiled with llvm-mingw: out/dist/bb-recomp-windows-x86_64.zip
#   tools/recomp/package-windows.sh [--test <PKG folder>]
#
# bb-recomp.exe is one static program (window and command line: Dear ImGui + SDL3, FEXCore's front end, LLVM 23 with the X86 and AArch64
# code generators and LLD built from source for mingw), next to ps4-pkg-tool.exe and share/bb-recomp (the AArch64 x87 helper object and the
# link stubs). It needs Windows 10 (1903 or newer) or 11, 64-bit, and nothing else: no LLVM, no Visual C++ runtime. Everything is built at nice 19
# with 22 jobs. Needs: cmake, ninja, curl, tar, xz, zstd, python3; the LLVM tarball of fetch-deps.sh (its llvm-tblgen builds the cross LLVM);
# the Android NDK r29 for the helper object (see package.sh). Needs network the first time (llvm-mingw, the LLVM sources, SDL3, ...).
# --test runs bb-recomp.exe under Wine (if installed) on the PKG folder: slow, see docs/NATIVE_CPU.md.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
root=$PWD
test_pkgs=
while (( $# )); do
    case $1 in
        --test) test_pkgs=$2; shift 2 ;;
        *) echo "usage: $0 [--test <PKG folder>]" >&2; exit 2 ;;
    esac
done
J=${BB_JOBS:-22}
run() { nice -n 19 "$@"; }
VERSION=${BB_RECOMP_VERSION:-0.1.0}
LLVM_VER=${BB_LLVM_VERSION:-23.1.3}
MINGW_TAG=${BB_LLVM_MINGW_TAG:-20261020}
deps=$root/.local-deps
tc=$root/tools/recomp/windows/toolchain-mingw-x86_64.cmake

tools/recomp/fetch-deps.sh >/dev/null    # the LLVM tarball (native llvm-tblgen), the SDL3 sources and ImGui
llvm_native=$deps/llvm/x/LLVM-$LLVM_VER-Linux-X64
imgui=$(ls -d "$deps"/imgui/imgui-* | head -n 1)

# 1. llvm-mingw (clang, lld, mingw-w64 headers and libraries for x86_64-w64-mingw32, UCRT)
mingw=$deps/mingw/x/llvm-mingw-$MINGW_TAG-ucrt-ubuntu-22.04-x86_64
if [[ ! -x $mingw/bin/x86_64-w64-mingw32-clang ]]; then
    mkdir -p "$deps/mingw/dl" "$deps/mingw/x"
    curl -fL -o "$deps/mingw/dl/llvm-mingw.tar.xz" "https://github.com/mstorsjo/llvm-mingw/releases/download/$MINGW_TAG/llvm-mingw-$MINGW_TAG-ucrt-ubuntu-22.04-x86_64.tar.xz"
    (cd "$deps/mingw/x" && run tar xf ../dl/llvm-mingw.tar.xz)
fi
tcargs=(-DCMAKE_TOOLCHAIN_FILE="$tc" -DBB_MINGW_ROOT="$mingw")

# 2. LLVM + LLD for Windows (X86 and AArch64 only, no tools): the libraries bb-recomp links
src=$deps/llvm/src/llvm-project-$LLVM_VER.src
if [[ ! -d $src ]]; then
    [[ -f $deps/llvm/dl/llvm-project-$LLVM_VER.src.tar.xz ]] || curl -fL -o "$deps/llvm/dl/llvm-project-$LLVM_VER.src.tar.xz" "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz"
    mkdir -p "$deps/llvm/src" && (cd "$deps/llvm/src" && run tar xf "../dl/llvm-project-$LLVM_VER.src.tar.xz")
fi
lb=$deps/llvm/build-mingw
if [[ ! -f $lb/lib/liblldELF.a ]]; then
    run cmake -S "$src/llvm" -B "$lb" -G Ninja "${tcargs[@]}" -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS=lld -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
        -DLLVM_NATIVE_TOOL_DIR="$llvm_native/bin" -DLLVM_HOST_TRIPLE=x86_64-w64-mingw32 -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
        -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_BUILD_TOOLS=OFF -DLLVM_INCLUDE_UTILS=OFF \
        -DLLVM_INCLUDE_DOCS=OFF -DLLD_BUILD_TOOLS=OFF -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_PLUGINS=OFF -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_Z3_SOLVER=OFF >/dev/null
    run cmake --build "$lb" -j "$J" --target lldELF lldCommon LLVMX86CodeGen LLVMX86AsmParser LLVMAArch64CodeGen LLVMAArch64AsmParser LLVMOrcJIT LLVMPasses \
        LLVMIRReader LLVMLTO LLVMDTLTO LLVMObject LLVMTargetParser LLVMOption LLVMBinaryFormat LLVMProfileData LLVMDebugInfoDWARF LLVMExecutionEngine LLVMX86Desc \
        LLVMX86Info LLVMAArch64Desc LLVMAArch64Info LLVMAArch64Utils LLVMMCJIT LLVMRuntimeDyld LLVMJITLink LLVMOrcTargetProcess LLVMOrcShared > "$root/out/llvm-mingw-build.log" 2>&1
fi

# 3. zlib and SDL3 for Windows, static
zp=$deps/deps/prefix-mingw
if [[ ! -f $zp/lib/libzlibstatic.a ]]; then
    run cmake -S "$deps/deps/src1/zlib-1.3.1" -B "$deps/deps/b1-mingw" -G Ninja "${tcargs[@]}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$zp" >/dev/null
    run cmake --build "$deps/deps/b1-mingw" -j "$J" >/dev/null && cmake --install "$deps/deps/b1-mingw" >/dev/null
    rm -f "$zp"/lib/libzlib.dll.a "$zp"/bin/*.dll
fi
sp=$deps/sdl/prefix-mingw
if [[ ! -f $sp/lib/libSDL3.a ]]; then
    run cmake -S "$deps/sdl/src/SDL3-${BB_SDL_VERSION:-3.4.18}" -B "$deps/sdl/build-mingw" -G Ninja "${tcargs[@]}" -DCMAKE_BUILD_TYPE=Release -DSDL_STATIC=ON -DSDL_SHARED=OFF \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="$sp" >/dev/null
    run cmake --build "$deps/sdl/build-mingw" -j "$J" >/dev/null && cmake --install "$deps/sdl/build-mingw" >/dev/null
fi

# mimalloc (replaces operator new/delete: 22 threads allocating scale badly on the Windows C runtime's heap)
mi=$deps/mimalloc/mimalloc-3.6.0
if [[ ! -f $mi/src/static.c ]]; then
    mkdir -p "$deps/mimalloc"
    curl -fL -o "$deps/mimalloc/mimalloc.tar.gz" https://github.com/microsoft/mimalloc/archive/refs/tags/v3.6.0.tar.gz
    tar -xf "$deps/mimalloc/mimalloc.tar.gz" -C "$deps/mimalloc"
fi

# 4. bb-recomp.exe
fexargs=()
[[ -n ${FEX_SOURCE_DIR:-} ]] && fexargs+=(-DFEX_SOURCE_DIR="$FEX_SOURCE_DIR")
run cmake -S tools/recomp -B out/recomp-win -G Ninja "${fexargs[@]}" -DBB_RECOMP_BUNDLED=ON -DBB_RECOMP_VERSION="$VERSION" -DBB_RECOMP_TOOLCHAIN_FILE="$tc" -DBB_MINGW_ROOT="$mingw" \
    -DBB_RECOMP_LLVM_DIR="$lb/lib/cmake/llvm" -DBB_RECOMP_DEPS_PREFIX="$zp" -DBB_RECOMP_SDL_PREFIX="$sp" -DBB_RECOMP_IMGUI_DIR="$imgui" -DBB_RECOMP_MIMALLOC_DIR="$mi" \
    "-DBB_RECOMP_LLD_INCLUDE=$src/lld/include;$lb/tools/lld/include" -DBB_RECOMP_JOBS="$J" > out/recomp-win-cfg.log 2>&1 || { tail -20 out/recomp-win-cfg.log; exit 1; }
run cmake --build out/recomp-win -j "$J"
BB_MINGW_ROOT=$mingw BB_MINGW_ZLIB_PREFIX=$zp tools/android/build_pkg_tool.sh windows

# 5. the folder and the zip
ndk=${BB_NDK:-}
if [[ -z $ndk ]]; then
    common=$(cd "$(git rev-parse --git-common-dir)/.." && pwd)
    for c in "$root/.local-deps/android/sdk/ndk/29.0.14206865" "$common/.local-deps/android/sdk/ndk/29.0.14206865"; do [[ -d $c ]] && { ndk=$c; break; }; done
fi
[[ -d $ndk ]] || { echo "No Android NDK (set BB_NDK)" >&2; exit 1; }
nbin=$ndk/toolchains/llvm/prebuilt/linux-x86_64
builtins=$(ls "$nbin"/lib/clang/*/lib/linux/libclang_rt.builtins-aarch64-android.a | head -n 1)
name=Bloodborne-Android-Rebuild-Tool
d=out/dist/$name-windows-x86_64
rm -rf "$d"; mkdir -p "$d/share/bb-recomp" "$d/licenses"
run "$nbin/bin/clang" --target=aarch64-linux-android30 -O2 -fPIC -fvisibility=hidden -fno-stack-protector -ffp-contract=off -c tools/recomp/helpers/bbr_x87_all.c -o "$d/share/bb-recomp/helpers-aarch64.o"
run "$nbin/bin/clang" --target=aarch64-linux-android30 -shared -nostdlib -Wl,-soname,libbbcpu.so -x c /dev/null -o "$d/share/bb-recomp/libbbcpu-stub.so"
cp "$nbin/sysroot/usr/lib/aarch64-linux-android/30/libm.so" "$d/share/bb-recomp/libm.so"
cp "$builtins" "$d/share/bb-recomp/"
[[ -d tools/recomp/variants ]] && cp -r tools/recomp/variants "$d/share/bb-recomp/variants"
"$mingw/bin/llvm-strip" -o "$d/bb-recomp.exe" out/recomp-win/bb-recomp.exe
cp out/windows/pkg-tool/ps4-pkg-tool.exe "$d/"
fex=$(sed -n 's/^FEX_SOURCE_DIR:PATH=//p' out/recomp-win/CMakeCache.txt)
[[ -f $fex/LICENSE ]] || fex=$(sed -n 's/^FEX_GIT_DIR:PATH=//p' out/recomp-win/CMakeCache.txt)
cp "$fex/LICENSE" "$d/licenses/FEX-MIT.txt"
cp "$deps/llvm/LICENSE.TXT" "$d/licenses/LLVM-Apache-2.0-with-LLVM-exception.txt"
cp tools/recomp/helpers/NOTICE "$d/licenses/x87-helpers-NOTICE.txt"
cp tools/recomp/LICENSE "$d/licenses/GPL-3.0.txt"
cp tools/recomp/NOTICES "$d/licenses/NOTICES.txt"
cp docs/NATIVE_CPU.md "$d/NATIVE_CPU.md" 2>/dev/null || true
cat > "$d/README.txt" <<'README'
Bloodborne Android Rebuild Tool
===============================
Double-click bb-recomp.exe. Choose the folder with your Bloodborne PKG files (the base game and the 1.09 update), choose an output folder,
press Build. When it says Done, copy the game folder it made to your phone and choose it as the game folder in the Bloodborne app.
Full guide: NATIVE_CPU.md.

Windows 10 (1903 or newer) or 11, 64-bit. About 8 GB of RAM and enough free disk space for the unpacked game. Windows may warn that the program
is unknown (it is not signed): More info, then Run anyway. Command line: bb-recomp.exe --pkgs <folder> --out <folder>.
Free software (GPL-3.0-or-later): if you paid for this, you were scammed. The files it makes come from your own game and are for you only.
README
python3 - "$d" "out/dist/bb-recomp-windows-x86_64.zip" <<'PY'
import os, sys, zipfile
d, z = sys.argv[1], sys.argv[2]
base = os.path.basename(d)
with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as f:
    for root, _, files in sorted(os.walk(d)):
        for n in sorted(files):
            p = os.path.join(root, n); f.write(p, os.path.join(base, os.path.relpath(p, d)))
PY
ls -l out/dist/bb-recomp-windows-x86_64.zip

if [[ -n $test_pkgs ]] && command -v wine >/dev/null; then
    t=$(mktemp -d /tmp/bb-recomp-win.XXXXXX)
    echo "Wine test in $t"
    ( cd "$t" && unzip -q "$root/out/dist/bb-recomp-windows-x86_64.zip" && WINEPREFIX=$t/prefix WINEDEBUG=-all nice -n 19 wine "$name-windows-x86_64/bb-recomp.exe" \
        --pkgs "Z:$test_pkgs" --out "Z:$t/out" ) 2>&1 | grep -v "MESA\|pci id" | tail -n 25
    ls -l "$t"/out/*/aot/ 2>/dev/null || true
fi
