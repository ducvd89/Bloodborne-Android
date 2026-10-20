#!/bin/sh
# Full prototype run: mechanism inventory -> coverage/report -> size+speed -> pack -> visualisations.
# Usage: PY=/path/to/venv/bin/python tools/atlas/nav/run_all.sh   (venv: numpy scipy shapely soulstruct==2.3.2 pillow)
set -e
cd "$(dirname "$0")"
PY=${PY:-python3}
nice -n 19 $PY mechanisms.py        # OUT/mech/<area>.json
nice -n 19 $PY bench.py             # OUT/bench.json, OUT/graphs/*.npz
nice -n 19 $PY pack.py              # OUT/pack/*.bnv (+ round-trip check)
nice -n 19 $PY report.py            # OUT/report.md, OUT/coverage.json, OUT/inventory.csv
nice -n 19 $PY viz.py m24_01_00_00 m24_00_00_00   # OUT/<area>.html
