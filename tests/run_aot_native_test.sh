#!/usr/bin/env bash
# Builds and runs tests/test_aot_native.cpp (the atomic emulator and the CPUID model of the FEX-free runtime) and tests/test_aot_strict.cpp (granular
# dispatch, the misses log, strict mode, the import-trap peephole of cpu/guest_cpu_aot.cpp) on an x86-64 PC. No game files, no .so.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
mkdir -p out
for t in native strict; do
    nice -n 19 "${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -Icpu -Isrc tests/test_aot_$t.cpp -ldl -o out/aot-$t-test
    out/aot-$t-test
done
