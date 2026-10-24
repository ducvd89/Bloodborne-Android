#!/usr/bin/env bash
# Builds and runs tests/test_aot_variants.cpp on an x86-64 PC: cpu/guest_cpu_aot.cpp's patch variants (classification of a function from its
# bytes, redirect of the dirty entry, un-dirtying) and native hook slots on synthetic tables. No game files, no .so.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
mkdir -p out
nice -n 19 "${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -Icpu -Isrc tests/test_aot_variants.cpp -ldl -o out/aot-variants-test
out/aot-variants-test
