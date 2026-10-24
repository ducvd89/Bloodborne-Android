#!/usr/bin/env bash
# Lowering regressions found on the AYN Thor (docs/AOT_DEVICE_STATUS.md), checked on the game's own functions (needs the
# CUSA03173 1.09 image: out/eboot.elf, out/boot-linked.bin and its sce_module):
#  1. +0x221b124 `movabs rax, 0x200000002` (two packed counts) must stay that constant, not be relocated as an image address
#     (the old lift base 0x200000000 made it image + 2).
#  2. +0x21b8420 `L: cmp eax, [r13 + 0x18]; jg L` (a spin-wait with no store) must load each iteration: a volatile load.
# usage: tests/run_recomp_device_regressions.sh [<sce_module dir>]
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
tool=out/recomp-build/bb-recomp
prx=${1:-../BloodbornePC/game/CUSA03173/sce_module}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
args=(--elf out/eboot.elf --boot out/boot-linked.bin --prx-dir "$prx" --target aarch64)
fail=0
nice -n 19 "$tool" "${args[@]}" --emit-ll 0x221b124 "$tmp/const.ll" >/dev/null 2>&1
if grep -q 'i64 8589934594' "$tmp/const.ll" && ! grep -q 'add nuw nsw i64 %imgbase, 2$' "$tmp/const.ll"; then
    echo "PASS packed constant 0x200000002 kept"
else echo "FAIL packed constant 0x200000002 relocated (+0x221b124)"; fail=1; fi
nice -n 19 "$tool" "${args[@]}" --emit-ll 0x21b8420 "$tmp/spin.ll" >/dev/null 2>&1
if grep -q 'load volatile i32' "$tmp/spin.ll"; then
    echo "PASS spin-wait load volatile (+0x21b8420)"
else echo "FAIL spin-wait load not volatile (+0x21b8420)"; fail=1; fi
exit $fail
