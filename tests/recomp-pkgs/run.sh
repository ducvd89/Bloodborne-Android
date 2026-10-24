#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests of bb-recomp's PKG step.
#   tests/recomp-pkgs/run.sh <bb-recomp binary> [<game folder to serve as the base game: runs the whole pipeline on synthetic PKGs and a stand-in tool>]
# BB_TEST_MOD_PKG=<a real data-mod .pkg> BB_TEST_PKG_TOOL=<ps4-pkg-tool> also unpacks that PKG with the real tool through the same code path.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
exe=$(realpath "$1"); game=${2:-}
out=${BB_TEST_OUT:-$PWD/out/recomp-pkgs-test}
rm -rf "$out"; mkdir -p "$out"
python3 -I tests/recomp-pkgs/test_pkg_step.py "$exe" "$out/cases"
if [[ -n $game ]]; then
    python3 -I tests/recomp-pkgs/make_fake_pkgs.py "$out/fake" "$game" >/dev/null
    "$exe" --pkgs "$out/fake/pkgs" --out "$out/result" --pkg-tool "$out/fake/fake-pkg-tool" --keep-work | tail -n 12
    test -f "$out/result/CUSA03173/aot/libbbaot.so" -a -f "$out/result/CUSA03173/aot/libbbaot.json" -a -f "$out/result/README.txt"
    test -f "$out/result/CUSA03173/sce_sys/changeinfo/bb_test_marker.txt"   # the update was merged over the base
    echo "pipeline ok: $out/result"
fi
if [[ -n ${BB_TEST_MOD_PKG:-} && -n ${BB_TEST_PKG_TOOL:-} && -n $game ]]; then
    # the real ps4-pkg-tool through the same code path: a real 1.09 data-mod PKG (category gp) as the update over the synthetic base; the run
    # may end at the game check afterwards (the mod changes files), what is tested is the unpacking and merging of a real PKG
    mkdir -p "$out/real/pkgs"
    cp "$out/fake/pkgs/"*A0100* "$out/real/pkgs/"; cp "$BB_TEST_MOD_PKG" "$out/real/pkgs/mod.pkg"
    BB_REAL_TOOL=$(realpath "$BB_TEST_PKG_TOOL") "$exe" --pkgs "$out/real/pkgs" --out "$out/real/out" --pkg-tool "$out/fake/fake-pkg-tool" --keep-work 2>&1 | tee "$out/real/log.txt" | head -n 14 || true
    grep -q "Extracting mod.pkg" "$out/real/log.txt" && grep -q "Merging mod.pkg" "$out/real/log.txt" && echo "real PKG unpacked and merged ok"
fi
