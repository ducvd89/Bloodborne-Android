#!/usr/bin/env python3
"""Pre-compile the game patches the Thor's graphics settings can choose, one file per choice.

The Thor has no Python: scripts/patches.py cannot run at each start as in run.sh. Its patches are
compiled here instead, a BBPATCH2 file per effect switch, model LOD and scene resolution, and
run-thor.sh passes the loader the files bbport.ini selects (the loader applies them in order).
fps=uncap.bin: the frame rate patches for any limit above the game's 30.
"""
import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import patches  # noqa: E402

# Outputs the Thor offers (its screen is 1920x1080): the upscaler's output, the game's UI size.
OUTPUTS = [(1280, 720), (1920, 1080)]


def render_sizes():
    """Scene sizes of every preset at every output (patches.scaled_sizes), the game's own excluded."""
    sizes = set()
    for out in OUTPUTS:
        for scale in patches.PRESET_SCALES:
            size = tuple(max(2, round(v / scale / 2) * 2) for v in out)
            if size == patches.OUTPUT_SIZE:
                continue  # 1920x1080: the game's own size, no patch
            sizes.add(size)
    return sorted(sizes)


def write(path, writes):
    blob = struct.pack('<8sQQ', b'BBPATCH2', patches.EBOOT_BASE, len(writes))
    for offset, data in writes:
        blob += struct.pack('<QQ', offset, len(data)) + data
    path.write_bytes(blob)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True, help='directory for the patch files')
    parser.add_argument('--eboot', type=Path, default=ROOT / 'out/eboot.elf')
    parser.add_argument('--xml', type=Path, default=ROOT / 'patches/Bloodborne.xml')
    parser.add_argument('--app-version', default='01.09')
    args = parser.parse_args()
    segments = patches.eboot_segments(args.eboot.read_bytes())
    args.out.mkdir(parents=True, exist_ok=True)
    count = 0
    for key, choices in patches.EFFECTS.items():
        for value, name in (('0', choices[0]), ('1', choices[1])):
            write(args.out / f'{key}={value}.bin',
                  patches.compile_patches(args.xml, [name], args.app_version, segments) if name else [])
            count += 1
    for value, name in patches.MODEL_LOD.items():
        write(args.out / f'model_lod={value}.bin',
              patches.compile_patches(args.xml, [name], args.app_version, segments))
        count += 1
    # Above 30 FPS: the delta-time patch with the sprint fix (patches.FPS_PRESETS['uncap']); the
    # present thread then limits the rate (BB_FPS_LIMIT, run-thor.sh).
    write(args.out / 'fps=uncap.bin',
          patches.compile_patches(args.xml, patches.FPS_PRESETS['uncap'], args.app_version, segments))
    count += 1
    for size in render_sizes():
        writes = patches.resolution_writes(args.xml, size, args.app_version, segments)
        write(args.out / f'res-{size[0]}x{size[1]}.bin', writes)
        count += 1
    print(f'{count} patch files in {args.out}')


if __name__ == '__main__':
    main()
