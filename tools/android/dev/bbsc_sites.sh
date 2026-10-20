#!/usr/bin/env bash
# Which of BBSC's hooks and byte patches does the game's code accept, and which do the port's own patches keep from
# installing? Runs BBSC's install code (gpu/bbnet20/bbsc) on an x86-64 PC against the decrypted 1.09 eboot
# (out/eboot.elf), with the real hook installer (gpu/shim/bbport_guest_code_hooks.cpp), first on the pristine code,
# then with the port's patches applied (the patch-parts the launcher loads, and the C++ patchers' sites).
#   tools/android/dev/bbsc_sites.sh [ELF] [PATCH-PARTS-DIR]
# Needs: clang++, the Android build's compile_commands (tools/android/build_bionic.sh ran once), Zydis (system, or built
# from .local-deps/android/bionic-src by tests/run_guest_code_hooks_test.sh into out/host/zydis).
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
elf=${1:-out/eboot.elf}
parts=${2:-out/release/runtime-stage/patch-parts}
out=out/host/bbsc-sites
mkdir -p "$out"
run() { taskset -c 2-23 nice -n 19 "$@"; }
[[ -f out/host/zydis/libZydis.a ]] || tests/run_guest_code_hooks_test.sh >/dev/null
z=.local-deps/android/bionic-src/zydis-4.1.1
flags=(-std=gnu++23 -O1 -w -fPIC -pthread -DNDEBUG -DBBNET_BBSC=1 -DBOOST_ASIO_STANDALONE
       -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD
       -Igpu/bbnet20/include -Igpu/bbnet20/bbsc -Igpu/bbnet20/compat -Igpu/bbnet20 -Igpu -Igpu/shim -Igpu/shadps4
       -Igpu/bbnet20/vendor/shadps4 -Isrc -I$z/include -I$z/dependencies/zycore/include -Iout/host/zydis -Iout/host/zydis/zycore
       -isystem .local-deps/android/bionic-prefix/include -isystem .local-deps/root/usr/include)
objs=()
compile() { # source object [extra flags]
    local source=$1 object=$2; shift 2
    if [[ ! -f $object || $source -nt $object || tools/android/dev/bbsc_sites.cpp -nt $object ]]; then
        run clang++ "${flags[@]}" "$@" -c "$source" -o "$object"
    fi
    objs+=("$object")
}
for f in bloodborne_re bbsc_persistence bbsc_memscan bbsc_cinematic; do compile gpu/bbnet20/bbsc/core/$f.cpp "$out/$f.o" -D__ANDROID__=1; done
# The real installer under another name: the tool's own InstallGuestCodeHook wraps it to list every site.
compile gpu/shim/bbport_guest_code_hooks.cpp "$out/hooks.o" -DInstallGuestCodeHook=RealInstallGuestCodeHook -DBB_GUEST_HOOK_THUNK=0
compile gpu/shim/common/logging/log.cpp "$out/log.o"
# fmt's own sources (the headers in the Android prefix are fmt 10.1.1; its header-only mode does not build with a
# current clang in C++23, its library does in C++17).
if [[ ! -f $out/fmt.o ]]; then
    run clang++ -std=c++17 -O1 -w -fPIC -I.local-deps/android/bionic-prefix/include -c \
        .local-deps/android/bionic-src/fmt-10.1.1/src/format.cc -o "$out/fmt.o"
fi
objs+=("$out/fmt.o")
compile tools/android/dev/bbsc_sites.cpp "$out/tool.o"
run clang++ "${objs[@]}" out/host/zydis/libZydis.a out/host/zydis/zycore/libZycore.a -pthread -o "$out/bbsc-sites"
echo "=== The pristine 1.09 code ==="
BB_GPU_LOG=${BB_GPU_LOG:-warning} "$out/bbsc-sites" "$elf" - || true
if [[ -d $parts ]]; then
    echo; echo "=== With the port's patches (patch parts and the C++ patchers' sites) ==="
    BB_GPU_LOG=${BB_GPU_LOG:-warning} "$out/bbsc-sites" "$elf" "$parts" --ports || true
fi
