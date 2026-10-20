#!/usr/bin/env bash
# Builds and runs tests/test_guest_code_hooks.cpp on an x86-64 PC: the guest-code hook trampoline
# (gpu/shim/bbport_guest_code_hooks.cpp) executed against fake game functions, with the hook called
# directly and through the host-function thunk the FEXCore build uses. Zydis: the system's when installed,
# else built from the Android dependency sources (build_bionic_deps.sh fetches them) into out/host/zydis.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
out=out/host/guest-code-hooks
mkdir -p "$out"
run() { taskset -c 2-23 nice -n 19 "$@"; }
include=() libs=()
if pkg-config --exists zydis 2>/dev/null; then
    read -r -a include <<<"$(pkg-config --cflags zydis)"
    read -r -a libs <<<"$(pkg-config --libs zydis)"
else
    src=.local-deps/android/bionic-src/zydis-4.1.1
    [[ -f $src/CMakeLists.txt ]] || { echo "No Zydis (install it, or run tools/android/build_bionic_deps.sh)" >&2; exit 1; }
    if [[ ! -f out/host/zydis/libZydis.a ]]; then
        cmake -S "$src" -B out/host/zydis -G Ninja -DCMAKE_BUILD_TYPE=Release -DZYDIS_BUILD_EXAMPLES=OFF \
            -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_SHARED_LIB=OFF >/dev/null
        run ninja -C out/host/zydis -j22 >/dev/null
    fi
    include=(-I "$src/include" -I "$src/dependencies/zycore/include" -I out/host/zydis -I out/host/zydis/zycore
             -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD)
    libs=(out/host/zydis/libZydis.a out/host/zydis/zycore/libZycore.a)
fi
for thunk in 0 1; do
    run clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror -DBB_GUEST_HOOK_THUNK=$thunk -I src -I gpu/shim "${include[@]}" \
        tests/test_guest_code_hooks.cpp gpu/shim/bbport_guest_code_hooks.cpp "${libs[@]}" -o "$out/test-$thunk"
    "$out/test-$thunk"
done
