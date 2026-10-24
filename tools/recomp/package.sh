#!/usr/bin/env bash
# Builds the self-contained Linux x86-64 bb-recomp for players: out/dist/bb-recomp-linux-x86_64.tar.gz
#   tools/recomp/package.sh [--test <game dump folder> [--compare <libbbaot.so of a dev build>]]
#
# The tool inside needs no LLVM, clang, NDK or lld on the player's machine: LLVM (X86 + AArch64) and LLD are linked in statically
# (official LLVM release tarball, tools/recomp/fetch-deps.sh), the executable is fully static (no glibc version requirement), the x87
# helpers are an AArch64 object compiled here, and the two soname stubs and the builtins archive the .so link needs are shipped in
# share/bb-recomp (bundle.h). The build runs at nice 19 with 22 jobs.
#
# Needs: cmake, ninja, git, curl, zstd, tar; a static glibc (libc.a) and libstdc++.a; the Android NDK r29 (BB_NDK or
# .local-deps/android/sdk/ndk/29.0.14206865 of this or the main checkout) for the AArch64 helper object, the stubs and the builtins.
# FEX_SOURCE_DIR (a FEX 2609.1 tree with its submodules) is optional: fex_project's default (fetch-fex.sh) makes one under out/.
# --test runs the package in a clean environment (env -i, no LLVM or NDK on PATH) on the dump and checks the .so; --compare also
# requires it to be byte-identical to that file.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
root=$PWD
test_game=; compare=
while (( $# )); do
    case $1 in
        --test) test_game=$2; shift 2 ;;
        --compare) compare=$2; shift 2 ;;
        *) echo "usage: $0 [--test <game dump> [--compare <dev libbbaot.so>]]" >&2; exit 2 ;;
    esac
done
J=${BB_JOBS:-22}
run() { nice -n 19 "$@"; }
VERSION=${BB_RECOMP_VERSION:-0.1.0}
name=bb-recomp-linux-x86_64

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
llvm=$root/.local-deps/llvm/x/LLVM-${BB_LLVM_VERSION:-23.1.3}-Linux-X64
prefix=$root/.local-deps/deps/prefix

# 1. bb-recomp: static, LLVM 23 from the tarball as the compiler too
fexargs=()
[[ -n ${FEX_SOURCE_DIR:-} ]] && fexargs+=(-DFEX_SOURCE_DIR="$FEX_SOURCE_DIR")
run cmake -S tools/recomp -B out/recomp-bundled -G Ninja "${fexargs[@]}" -DBB_RECOMP_BUNDLED=ON -DBB_RECOMP_VERSION="$VERSION" \
    -DBB_RECOMP_LLVM_DIR="$llvm/lib/cmake/llvm" -DBB_RECOMP_DEPS_PREFIX="$prefix" -DBB_RECOMP_CC="$llvm/bin/clang" -DBB_RECOMP_CXX="$llvm/bin/clang++" \
    -DBB_RECOMP_JOBS="$J" > out/recomp-bundled-cfg.log 2>&1 || { tail -20 out/recomp-bundled-cfg.log; exit 1; }
run cmake --build out/recomp-bundled -j "$J"

# 2. the files the .so link needs, from the NDK: the same commands compile.cpp runs for a developer build
dist=out/dist/$name
rm -rf "$dist"; mkdir -p "$dist/bin" "$dist/share/bb-recomp" "$dist/licenses"
share=$dist/share/bb-recomp
run "$nbin/bin/clang" --target=aarch64-linux-android30 -O2 -fPIC -fvisibility=hidden -fno-stack-protector -ffp-contract=off -c \
    tools/recomp/helpers/bbr_x87_all.c -o "$share/helpers-aarch64.o"
# libbbcpu.so: a DT_NEEDED entry only (see compile.cpp: the real libbbcpu.so is already loaded when the game starts)
run "$nbin/bin/clang" --target=aarch64-linux-android30 -shared -nostdlib -Wl,-soname,libbbcpu.so -x c /dev/null -o "$share/libbbcpu-stub.so"
# libm.so: the NDK's own link stub (no code), so that the .so gets libm's DT_NEEDED and symbol versions exactly as a developer build
cp "$nbin/sysroot/usr/lib/aarch64-linux-android/30/libm.so" "$share/libm.so"
cp "$builtins" "$share/"
[[ -d tools/recomp/variants ]] && cp -r tools/recomp/variants "$share/variants"

# 3. the tool, stripped, and the notices
strip -o "$dist/bin/bb-recomp" out/recomp-bundled/bb-recomp
fex=$(sed -n 's/^FEX_SOURCE_DIR:PATH=//p' out/recomp-bundled/CMakeCache.txt)
[[ -f $fex/LICENSE ]] || fex=$(sed -n 's/^FEX_GIT_DIR:PATH=//p' out/recomp-bundled/CMakeCache.txt)
cp "$fex/LICENSE" "$dist/licenses/FEX-MIT.txt"
cp .local-deps/llvm/LICENSE.TXT "$dist/licenses/LLVM-Apache-2.0-with-LLVM-exception.txt"
cp tools/recomp/helpers/NOTICE "$dist/licenses/x87-helpers-NOTICE.txt"
cp LICENSE "$dist/licenses/bbport-GPL-2.0.txt"
cat > "$dist/licenses/THIRD_PARTY.txt" <<'NOTICE'
bb-recomp contains:
- FEXCore (the x86-64 front end of FEX-Emu 2609.1), MIT license: FEX-MIT.txt
- LLVM 23.1.3 and LLD (the AArch64 code generator and the linker), Apache License 2.0 with LLVM exceptions: LLVM-Apache-2.0-with-LLVM-exception.txt
- compiler-rt builtins and the libm.so link stub for AArch64 Android (share/bb-recomp), from the Android NDK r29,
  Apache License 2.0 with LLVM exceptions / Android Open Source Project licenses
- the x87 helpers (FEX's fallback handlers over Berkeley SoftFloat 3e, BSD-3-Clause): x87-helpers-NOTICE.txt
- zlib 1.3.1 (zlib license) and zstd 1.5.7 (BSD-3-Clause, dual licensed with GPL-2.0), linked statically
- bb-prepare and the rest of the bbport sources, GPL-2.0-or-later: bbport-GPL-2.0.txt
bb-recomp itself is part of bbport. The libbbaot.so it writes is derived from the player's own game: it is for that player only.
NOTICE
cp docs/NATIVE_CPU.md "$dist/NATIVE_CPU.md" 2>/dev/null || true
cat > "$dist/README.txt" <<'README'
bb-recomp: compiles Bloodborne's code for your Android phone, from your own game dump.

  ./bin/bb-recomp --game /path/to/CUSA03173 --out ./out

CUSA03173 is the folder with eboot.bin, sce_module and sce_sys (Bloodborne with the 1.09 update merged in). It takes a few minutes and
about 3 GB of free disk space. It writes out/libbbaot.so and out/libbbaot.json: copy both to the phone and import them in the app
(Menu -> Native CPU (recompiled)). Full guide: NATIVE_CPU.md.

Nothing here needs LLVM, clang or the Android NDK. Linux x86-64 with AVX2 (Intel Haswell 2013 / AMD Zen or newer) and 8 GB RAM or more.
On Windows, use WSL2 (NATIVE_CPU.md). The result is derived from your game: do not share it.
README
echo "$VERSION" > "$dist/VERSION"

# 4. the tarball (reproducible: sorted, fixed owner and times)
mkdir -p out/dist
run tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='2026-01-01 00:00:00' -C out/dist -cf - "$name" | gzip -9n > "out/dist/$name.tar.gz"
ls -l "out/dist/$name.tar.gz"
du -sh "$dist"

# 5. test in a clean environment
if [[ -n $test_game ]]; then
    t=$(mktemp -d /tmp/bb-recomp-clean.XXXXXX)
    tar -xzf "out/dist/$name.tar.gz" -C "$t"
    echo "clean-environment test in $t (env -i, PATH=/nonexistent-llvm, HOME=$t)"
    ( cd "$t" && TIMEFORMAT='wall %R s, user %U s' && time env -i HOME="$t" PATH=/nonexistent-llvm "$t/$name/bin/bb-recomp" --game "$test_game" --out "$t/out" ) 2>&1 | tail -n 45
    so=$t/out/libbbaot.so
    [[ -f $so && -f $t/out/libbbaot.json ]] || { echo "TEST FAILED: no libbbaot.so" >&2; exit 1; }
    if [[ -n $compare ]]; then
        if cmp -s "$so" "$compare"; then echo "TEST OK: byte-identical to $compare"
        else
            # A developer build links with the NDK's LLD 21, the package with LLD 23: the .comment string differs, which moves the
            # (unloaded) .symtab. Everything that is loaded must be identical.
            loadend=0
            while read -r off filesz; do e=$(( off + filesz )); (( e > loadend )) && loadend=$e; done < <(readelf -lW "$so" | awk '$1=="LOAD"{print $2, $5}')
            # (the ELF header's e_shoff, bytes 41-48 counted from 1 by cmp, moves with the tail)
            if [[ -z $(cmp -l -n "$loadend" "$so" "$compare" | awk '$1<41||$1>48') ]]; then echo "TEST OK: the loaded part ($loadend bytes) is identical to $compare (only e_shoff and the .comment/.symtab tail differ: LLD version)"
            else echo "TEST FAILED: differs from $compare in the loaded part" >&2; exit 1; fi
        fi
    fi
    echo "kept: $t"
fi
