#!/usr/bin/env bash
# Builds and runs tests/test_aot_patchers.cpp: the native hook slot callbacks of the port's code patchers against the semantics of the stubs they replace.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
mkdir -p out
nice -n 19 "${CXX:-c++}" -std=c++20 -O1 -g -w -Igpu/shim -Igpu/shadps4 -Isrc tests/test_aot_patchers.cpp -o out/aot-patchers-test
out/aot-patchers-test
