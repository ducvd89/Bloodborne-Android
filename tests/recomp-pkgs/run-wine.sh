#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The PKG step of the Windows bb-recomp.exe under Wine, with a batch file as the PKG tool: the PKGs are found, "unpacked", the update is merged
# over the base, and the run then stops at the game check (the tiny tree is not a game). Also unpacks a real PKG with the real ps4-pkg-tool.exe.
#   tests/recomp-pkgs/run-wine.sh <bb-recomp.exe> [<ps4-pkg-tool.exe> <a real .pkg>]
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
exe=$(realpath "$1"); out=$PWD/out/recomp-pkgs-wine
rm -rf "$out"; mkdir -p "$out"
export WINEPREFIX=$out/prefix WINEDEBUG=-all
python3 -I - "$out" <<'PY'
import os, sys
sys.path.insert(0, 'tests/recomp-pkgs')
from make_fake_pkgs import pkg, sfo
out = sys.argv[1]; os.makedirs(out + '/pkgs')
cid = 'EP9000-CUSA03173_00-BLOODBORNE0000EU'
open(out + '/pkgs/base.pkg', 'wb').write(pkg(cid, sfo({'TITLE_ID': 'CUSA03173', 'APP_VER': '01.00', 'CATEGORY': 'gd'}), b'\nPLAN base x\n'))
open(out + '/pkgs/update.pkg', 'wb').write(pkg(cid, sfo({'TITLE_ID': 'CUSA03173', 'APP_VER': '01.09', 'CATEGORY': 'gp'}), b'\nPLAN update\n'))
PY
cp tests/recomp-pkgs/fake-pkg-tool.cmd "$out/fake-pkg-tool.cmd"
set +e
wine "$exe" --cli --pkgs "Z:$out/pkgs" --out "Z:$out/o" --pkg-tool "Z:$out/fake-pkg-tool.cmd" --keep-work 2>&1 | grep -v "MESA\|pci id" | tee "$out/log.txt" | tail -n 14
set -e
grep -q "Merging update.pkg" "$out/log.txt"
test "$(tr -d '\r\n ' < "$out/o/CUSA03173/file.txt")" = new           # the update's file replaced the base's
test -f "$out/o/CUSA03173/sce_sys/base.txt" -a -f "$out/o/CUSA03173/sce_sys/changeinfo/bb_test_marker.txt"
echo "Windows PKG step ok"
if [[ $# -ge 3 ]]; then
    mkdir -p "$out/real" && wine "$2" "Z:$(realpath "$3")" "Z:$out/real" 2>&1 | grep -v "MESA\|pci id" | tail -n 2
fi
