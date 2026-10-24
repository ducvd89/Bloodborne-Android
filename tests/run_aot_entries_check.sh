#!/usr/bin/env bash
# PC-side check of phase 6 on the real image (needs out/eboot.elf, out/boot-linked.bin and the game's sce_module directory; no code is generated, ~2 s):
# the three guest addresses the Thor logged as misses in a 13-minute session must now be covered:
#   +0x20       the static-initialiser runner at the start (no FDE)           -> a unit entry
#   +0x210ebc0  CSChrThread's loop head (85 k misses)                         -> a case of a resolved jump table
#   +0x23a9c9c  EzWorkPool's switch case (746 misses)                         -> a case of a resolved jump table
# plus a list file in the misses-log format through --extra-entries (an address that is a unit entry because the log said so), and the table dump.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
tool=${BB_RECOMP:-out/recomp-build/bb-recomp}
sce=${BB_SCE_MODULE:-/home/cid/BloodbornePC/game/CUSA03173/sce_module}
[[ -x $tool && -f out/boot-linked.bin && -d $sce ]] || { echo "aot entries check: skipped (needs $tool, out/boot-linked.bin, $sce)"; exit 0; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
args=(--elf out/eboot.elf --boot out/boot-linked.bin --prx-dir "$sce" --units-only --no-nice)
out=$("$tool" "${args[@]}" --check-entries 20,210ebc0,23a9c9c --dump-units "$tmp/units.txt")
echo "$out" | grep -E '^\+0x|not covered'
echo "$out" | grep -q '^+0x20: unit entry'
echo "$out" | grep -q '^+0x210ebc0: case of the jump table'
echo "$out" | grep -q '^+0x23a9c9c: case of the jump table'
echo "$out" | grep -q '^0 of 3 addresses are not covered'
grep -q '^unit 0x20 ' "$tmp/units.txt"
# A misses log: an address in the middle of a function becomes a unit entry; one in no function a pseudo-function.
printf '# bb aot misses\nmiss +0x23a9d00 kind=jump thread=T tid=1 rsp=0x0 backtrace=+0x1\nmiss +0x10 kind=call thread=T tid=1 rsp=0x0 backtrace=\n' > "$tmp/misses.log"
out=$("$tool" "${args[@]}" --extra-entries "$tmp/misses.log" --check-entries 23a9d00 2>&1)
echo "$out" | grep -E 'extra entries|^\+0x'
echo "$out" | grep -q '^+0x23a9d00: unit entry'
echo "aot entries check: ok"
