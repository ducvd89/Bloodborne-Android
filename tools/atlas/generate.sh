#!/bin/sh
# Hunter's Atlas map pack: runs Hunter's Atlas's generator on the 1.09 game files and builds the
# pack the Android app downloads (build_pack.py). PC only, no game data is committed.
# Usage: tools/atlas/generate.sh WORK_DIR GAME_DIR OUTPUT_ZIP
#   WORK_DIR    scratch directory: the HuntersAtlas checkout, a Python 3.12 venv, the generator output
#   GAME_DIR    the CUSA03173 1.09 game folder (dvdroot_ps4/, eboot.bin); only read
set -e
work=$(realpath -m "$1"); game=$(realpath "$2"); output=$(realpath -m "$3")
here=$(cd "$(dirname "$0")" && pwd)
commit=4ee4a0edf00ca87797e40f7ad73c27c9b2198625   # HuntersAtlas 1.3 (main, 2026-10-17)
mkdir -p "$work"
[ -d "$work/ha/.git" ] || git clone -q https://github.com/MikeTat1988/HuntersAtlas.git "$work/ha"
git -C "$work/ha" fetch -q --depth 1 origin $commit 2>/dev/null || true
git -C "$work/ha" checkout -q $commit
export UV_PYTHON_INSTALL_DIR="$work/uvpy"
if [ ! -x "$work/venv/bin/python" ]; then
    python3 -m venv "$work/uvenv" && "$work/uvenv/bin/pip" install -q uv
    "$work/uvenv/bin/uv" python install 3.12
    "$work/uvenv/bin/uv" venv --python 3.12 "$work/venv"
    grep -v '^soulstruct' "$work/ha/source/generator/requirements-runtime.txt" > "$work/requirements.txt"
    "$work/uvenv/bin/uv" pip install --python "$work/venv/bin/python" -r "$work/requirements.txt" lxml==6.1.1 soulstruct==2.3.2
fi
# The generator runs from a patched copy: binder entry paths are Windows paths, which pathlib on
# Linux does not split (linux-binderentry.patch).
rm -rf "$work/generator" "$work/generated"
cp -r "$work/ha/source/generator/src" "$work/generator"
# generate_atlas.py has CRLF line endings.
sed "s/$/\r/" "$here/linux-binderentry.patch" | patch -s --binary -d "$work/generator" generate_atlas.py
nice -n 19 "$work/venv/bin/python" "$work/generator/generate_atlas.py" --game-dir "$game" --output-dir "$work/generated" > "$work/generate.log"
python3 -I "$here/build_pack.py" --generated "$work/generated" --hunters-atlas "$work/ha" --output "$output"
