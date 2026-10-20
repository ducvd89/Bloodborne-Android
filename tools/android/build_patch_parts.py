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
    out = []
    for site, original, back, cave, code, _ in writes:
        if text[site:site + len(original)] != original:
            raise SystemExit(f'cheats: unexpected code at {site:#x}: not the 1.09 eboot')
        code += jump(cave + len(code), back)
        hook = jump(site, cave) + b'\x90' * (len(original) - 5)
        assert cave + len(code) <= 0x50da000
        out += [(cave, code), (site, hook)]
    out += item_use_writes(text)
    return out


# Items. The game's whole inventory goes through ChangeCount(container, slot, delta, &over) at
# 0x14d94a0 (count += delta, clamped to the max), and every removal of goods through
# Remove(container, slot, -n, force, ..) at 0x14d0be0 (it checks the goods' consumable flag, calls
# ChangeCount at 0x14d0e48, and frees the slot when the count reaches 0). An earlier version skipped
# every negative ChangeCount: that also skipped what is NOT a use - items moved to the storage box
# (0x14da750 / the menu's send-to-storage 0x14fcd40: add to the box, then Remove from the
# inventory), the over-the-cap moves, the max-count clamp when the cap changes (0x1a573f0), the
# restock that takes vials back out of the storage (0x1b63bc0), discard (0x1743590), shop costs and
# sales (0x1736140, 0x1b5cde0 via 0x1c14a00) - and left the inventory structures out of sync (no
# Blood Vials in the inventory, the HUD slot still showing 20).
# The player's USES are four Remove calls, found from the callers of 0x14d94a0 / 0x14d0be0
# (all with force = 0, the consumable-flag path), and only these are made free now:
#   0x18f983c  PlayerIns::ConsumeGoods(goods id), the virtual at vtable +0x400 (relocated slots
#              0x538f718 / 0x538fe88). It is reached from the use-item animation event (id 0xfa1:
#              the player event switch 0x18cef90 calls vtable +0x3e8 = 0x18c6db0, which calls +0x400
#              for a player target). It looks the goods up in the quick-item list 0x14deff0 / the
#              inventory 0x14d9f60 and calls Remove(inv + 0x1d0, slot, -1, 0, 0, 0), then
#              `test eax,eax; setle cl` (the "last one used" flag). A flag word of the player's
#              behaviour data ([[chr+0x3b0]+0x20]+0x200 & 0x200) already skips it for the game's
#              own free uses. Blood Vials (goods 1000), the throwables, the other consumables.
#   0x18fa58b  consume the firearm's bullet (0x18fa3f0 (player, slot, 1), from the shot event
#              0x1a267a0: weapon type [param+0xe2] = 0xa / 0xb, the ammo slot read from the
#              player's equip table [gamedata+0x1f4 + 4 * n]).
#   0x18fa0b3, 0x18fa0ce   0x18f9f00 (player, n): pay n bullets of a weapon's cost (an event
#              0x1a249d0 / a SpEffect 0x18b61c0): goods 901 first, then goods 900 (Quicksilver
#              Bullets), both removed once the whole n is there (it returns 0, not paying, when
#              they are not: nothing is removed then, with or without the cheat).
# Each call's rel32 is moved to a 20-byte stub: flag off, it jumps to 0x14d0be0 (the same call);
# flag on, it returns 1 (more than 0 left: nothing used up, the slot kept, no count change, no
# quick-slot reset) without calling. The stub is also what the callers' return value checks see:
# the 0x18f9841 `test eax, eax; setle cl` (a use is "not the last"), the others ignore it.
# Nothing else is touched: with the flag off the game's code runs as it is.
ITEM_USE_SITES = (0x18f983c, 0x18fa58b, 0x18fa0b3, 0x18fa0ce)
ITEM_REMOVE = 0x14d0be0


def item_use_writes(text):
    stub = CAVE + 0x40
    code = flag_test(stub, CHEAT_FLAGS + 1) + bytes.fromhex(
        '7406'                        # je +6 (off: the game's own call)
        'b801000000' 'c3')            # mov eax,1; ret (on: one left, nothing removed)
    code += jump(stub + len(code), ITEM_REMOVE)    # off: the remove, with the caller's return address
    assert stub + len(code) <= 0x50da000 and stub >= CAVE + 0x20
    out = [(stub, code)]
    for site in ITEM_USE_SITES:
        if text[site:site + 5] != b'\xe8' + struct.pack('<i', ITEM_REMOVE - (site + 5)):
            raise SystemExit(f'cheats: unexpected code at {site:#x}: not the 1.09 eboot')
        out.append((site + 1, struct.pack('<i', stub - (site + 5))))    # call rel32 -> the stub
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
    # The title screen straight to the main menu, offline (only with skip_network_choice=1; off by default since 1.2.0).
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
