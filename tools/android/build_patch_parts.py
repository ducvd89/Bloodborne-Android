#!/usr/bin/env python3
"""Pre-compile the game patches the Thor's graphics settings can choose, one file per choice.

The Android runtime has no Python: scripts/patches.py cannot run at each start as in run.sh. Its
patches are compiled here instead, a BBPATCH2 file per effect switch, model LOD and scene resolution
(build_release_runtime.py puts them in the runtime archive as bbport/patch-parts), and run-bionic.sh
passes the loader the files bbport.ini selects (the loader applies them in order).
fps=uncap.bin: the frame rate patches for any limit above the game's 30.
"""
import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import patches  # noqa: E402

# The scene size is any size (any aspect ratio: bbport.ini aspect_ratio / output_res): res-scene.bin is the
# resolution patch with the 1280x720 scene of its template and the UI's 1920x1080, and the runtime writes
# the scene's two immediates itself from BB_RENDER_RES (gpu/shim/bbport_display.cpp, PatchSceneSize).
# graphics-heap.bin: the larger graphics heaps a scene above 1080p needs (with BB_DMEM_MB raised).
SCENE_TEMPLATE = (1280, 720)


def write(path, writes):
    blob = struct.pack('<8sQQ', b'BBPATCH2', patches.EBOOT_BASE, len(writes))
    for offset, data in writes:
        blob += struct.pack('<QQ', offset, len(data)) + data
    path.write_bytes(blob)


# Cheats (the app's Cheats menu), as code hooks of 1.09 always in place and switched live: each
# hook's code tests its flag byte, which the loader keeps as BB_CHEATS_FILE says (probe.c), and does
# the game's own instructions when it is 0. The code goes in the unused end of the code segment's
# last page (past the segment's 0x50d96dc bytes, mapped executable with it), the flags in the
# unused end of the data segment's (past 0x56d30f4, mapped writable). The same hooks as
# Shiningami's GoldHEN cheats for CUSA03173 1.09, re-written to stay inside the image.
CAVE = 0x50d9700         # code segment end 0x50d96dc; its page ends at 0x50da000
CHEAT_FLAGS = 0x56d3f00  # data segment end 0x56d30f4; its page ends at 0x56d4000 (probe.c too)


def jump(at, to):
    return b'\xe9' + struct.pack('<i', to - (at + 5))


def flag_test(at, flag):
    """cmp byte [rip + flag], 0 at `at` (7 bytes)."""
    return b'\x80\x3d' + struct.pack('<i', flag - (at + 7)) + b'\x00'


def cheat_writes(text):
    """The hooks and their code, checked against this eboot's 1.09 code."""
    writes = []
    # Health: the player's HP (+0xf8) read each frame for the HUD; flag on: the max HP (+0xfc) first.
    site, original, back, cave = 0x18f78b1, bytes.fromhex('8b88f8000000'), 0x18f78b7, CAVE
    code = flag_test(cave, CHEAT_FLAGS) + bytes.fromhex(
        '740c'                        # je +12 (off)
        '8b88fc000000' '8988f8000000' # mov ecx,[rax+0xfc]; mov [rax+0xf8],ecx
        '8b88f8000000')               # mov ecx,[rax+0xf8] (the game's own)
    writes.append((site, original, back, cave, code, b''))
    # Items: the count change (count += amount, at 0x14d94a0); flag on: a negative amount (a blood
    # vial or bullet used, items given away) leaves the count as it was.
    site, original, back, cave = 0x14d9540, bytes.fromhex('448b63084501f44139c4'), 0x14d954a, CAVE + 0x40
    code = bytes.fromhex('448b6308') + flag_test(cave + 4, CHEAT_FLAGS + 1) + bytes.fromhex(
        '7405'                        # je +5 (off: add)
        '4585f6' '7803'               # test r14d,r14d; js +3 (skip the add)
        '4501f4' '4139c4')            # add r12d,r14d; cmp r12d,eax (the game's own)
    writes.append((site, original, back, cave, code, b''))
    out = []
    for site, original, back, cave, code, _ in writes:
        if text[site:site + len(original)] != original:
            raise SystemExit(f'cheats: unexpected code at {site:#x}: not the 1.09 eboot')
        code += jump(cave + len(code), back)
        hook = jump(site, cave) + b'\x90' * (len(original) - 5)
        assert cave + len(code) <= 0x50da000
        out += [(cave, code), (site, hook)]
    return out


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
    # The delta-time patch with the sprint fix (patches.FPS_PRESETS['uncap']), the frame rate before the
    # hooks (gpu/shim/bbport_frame_rate.cpp): run-bionic.sh gives it to bb-probe only as --fallback-patches
    # (the hooks did not install) or with BB_FRAME_HOOKS=0; the present thread limits the rate
    # (BB_FPS_LIMIT).
    write(args.out / 'fps=uncap.bin',
          patches.compile_patches(args.xml, patches.FPS_PRESETS['uncap'], args.app_version, segments))
    count += 1
    # The title screen straight to the main menu, offline (on unless skip_network_choice=0).
    write(args.out / 'skip_network_choice=1.bin',
          patches.compile_patches(args.xml, [patches.SKIP_NETWORK_CHOICE], args.app_version, segments))
    count += 1
    text = args.eboot.read_bytes()[0x4000:0x4000 + 0x50d96dc]  # the code segment (offset 0x4000, vaddr 0)
    write(args.out / 'cheats.bin', cheat_writes(text))
    count += 1
    # The GX per-frame command heaps 256 KiB -> 1 MiB (upstream bloodborne_pc #162, PR #224): a full
    # heap returned NULL to code that writes through it unchecked (a guest fault at 0x2199310).
    write(args.out / 'gx-heap.bin',
          patches.compile_patches(args.xml, ['Larger GX Per-Frame Command Heap'], args.app_version, segments))
    count += 1
    write(args.out / 'res-scene.bin',
          patches.resolution_writes(args.xml, SCENE_TEMPLATE, args.app_version, segments))
    write(args.out / 'graphics-heap.bin',
          patches.compile_patches(args.xml, ['Increased Graphics Heap Sizes'], args.app_version, segments))
    count += 2
    print(f'{count} patch files in {args.out}')


if __name__ == '__main__':
    main()
