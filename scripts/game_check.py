#!/usr/bin/env python3
"""The game files bbport runs: Bloodborne with the 1.09 update merged in. The retail releases (the
US CUSA00900, the European Game of the Year edition CUSA03173, the Asian The Old Hunters Edition
CUSA03023, ...) share the same 1.09 executable.

Other versions start and then fail inside the game's code (the base game 1.00 faults at guest
offset 0x20348b8): hooks and patches use the addresses of this one executable. The check compares
the loaded executable image, whose hash is the same whatever tool dumped it (the SELF headers
around it differ). The shaders must also unpack: an extraction tool that leaves
parts of compressed sectors stale (LibOrbisPkg PkgTool on .NET 6 or newer, issue #81) makes the
game hang while it loads them, with no error. BB_SKIP_GAME_CHECK=1 skips both.

Run as a script, it checks every .dcx file of a game folder: game_check.py GAME_DIR
"""
import hashlib
import os
import struct
import sys
import zlib
from pathlib import Path

# The retail releases (the list of content IDs from PR #117): Bloodborne, then the editions with
# The Old Hunters. The executable decides in the end: SUPPORTED_IMAGE.
SUPPORTED_TITLES = ('CUSA00900', 'CUSA00207', 'CUSA00208', 'CUSA00299', 'CUSA01363',  # US EU UK JP AS
                    'CUSA03179', 'CUSA03173', 'CUSA03014', 'CUSA03023')  # US EU JP AS
SUPPORTED_VERSION = '01.09'
SUPPORTED_IMAGE = '071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a'
# The 1.09 executable with Lance McDonald's 60 fps patch applied to it, as some dumps ship it
# (seen on US CUSA00900): one of its edits inverts a branch in the menu code (+0xf6d90d) and the
# gestures menu crashes the game. bbport has its own 60 fps; the clean 1.09 eboot.bin is needed.
LANCE_60FPS_IMAGE = 'bbc91f4dff6bc3118b039464289e39878a8fdfc802a07fab9d5fe84d935a676f'
# The game loads these first; a broken extraction damages most files, these among them.
CHECKED_FOLDER = 'dvdroot_ps4/shader'


def image_sha256(game):
    """The SHA-256 of eboot.bin's loaded image (its loadable segments at their addresses)."""
    from prepare import parse_self, span  # same directory
    elf, header, ph, _segments, _missing = parse_self((Path(game) / 'eboot.bin').read_bytes())
    loads = [p for p in ph if p['type'] in (1, 0x61000010)]
    image = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
    for p in loads:
        image[p['vaddr']:p['vaddr'] + p['filesz']] = span(elf, p['offset'], p['filesz'])
    return hashlib.sha256(image).hexdigest()


def dcx_broken(path):
    """True when a DFLT .dcx file does not unpack to its stated size or fails zlib's checksum.
    Other compressions are not checked."""
    data = Path(path).read_bytes()
    if data[:4] != b'DCX\0' or data[0x18:0x1C] != b'DCS\0' or data[0x28:0x2C] != b'DFLT':
        return False
    size, packed = struct.unpack_from('>II', data, 0x1C)
    try:
        return len(zlib.decompress(data[0x4C:0x4C + packed])) != size
    except zlib.error:
        return True


def broken_files(game, folder=CHECKED_FOLDER):
    """The .dcx files under the game's folder that do not unpack (paths relative to the game)."""
    root = Path(game)
    return [str(f.relative_to(root)) for f in sorted((root / folder).rglob('*.dcx')) if dcx_broken(f)]


def problem(game, image_hash=None):
    """None for the supported game, else (kind, title, version): kind is 'missing_update' (base
    game or an older update), 'wrong_eboot' (param.sfo says 1.09, eboot.bin is another version),
    'patched_eboot' (the 1.09 eboot.bin with a 60 fps patch baked in), 'other_title' (another
    edition), 'unreadable' or 'damaged_files' (the right game, but its shaders do not unpack: a
    broken extraction)."""
    if os.environ.get('BB_SKIP_GAME_CHECK') == '1':
        return None
    from prepare import sfo
    try:
        info = sfo((Path(game) / 'sce_sys/param.sfo').read_bytes())
    except (OSError, ValueError, IndexError):
        info = {}
    title, version = info.get('TITLE_ID', '?'), info.get('APP_VER', '?')
    try:
        image_hash = image_hash or image_sha256(game)
        if image_hash == SUPPORTED_IMAGE:
            return ('damaged_files', title, version) if broken_files(game) else None
    except (OSError, ValueError, IndexError, StopIteration, KeyError):
        return 'unreadable', title, version
    if image_hash == LANCE_60FPS_IMAGE:
        return 'patched_eboot', title, version
    if title not in SUPPORTED_TITLES:
        return 'other_title', title, version
    if version != SUPPORTED_VERSION:
        return 'missing_update', title, version
    return 'wrong_eboot', title, version


def explain(kind, title, version):
    """What is wrong and what to do, for the log (English)."""
    found = f'Found {title} version {version} (sce_sys/param.sfo).'
    return {
        'missing_update': f'{found} bbport needs the 1.09 update merged into the game folder: copy '
                          'everything from the dumped 1.09 update (e.g. CUSA03173-patch) into the '
                          'game folder, replacing files (eboot.bin and sce_sys too).',
        'wrong_eboot': f'{found} param.sfo is from 1.09 but eboot.bin is not: copy eboot.bin from '
                       'the dumped 1.09 update into the game folder, replacing the old one.',
        'patched_eboot': f'{found} This eboot.bin has the 60 fps patch by Lance McDonald applied '
                         'to it, which crashes the game when the gestures menu opens (bbport has its '
                         'own 60 fps): copy eboot.bin from a clean dump of the 1.09 update into the '
                         'game folder, replacing this one.',
        'other_title': f'{found} This is not a retail release of Bloodborne: bbport runs Bloodborne '
                       '(any region, The Old Hunters editions too) with update 1.09.',
        'unreadable': f'{found} eboot.bin could not be read as a decrypted PS4 executable: dump '
                      'the game and the 1.09 update again.',
        'damaged_files': f'{found} The game files are damaged: the shaders in {CHECKED_FOLDER} do '
                         'not unpack, and the game would hang while loading them. The extraction '
                         'tool left parts of compressed sectors stale (LibOrbisPkg PkgTool on '
                         '.NET 6 or newer, issue #81): extract the game and the 1.09 update again '
                         'with a fixed tool. scripts/game_check.py GAME_DIR checks every file.',
    }[kind]


def main(argv):
    """game_check.py GAME_DIR: every .dcx file of the game, those that do not unpack listed."""
    if len(argv) != 2:
        print('Usage: game_check.py GAME_DIR', file=sys.stderr)
        return 2
    game = Path(argv[1])
    files = sorted((game / 'dvdroot_ps4').rglob('*.dcx'))
    broken = []
    for number, path in enumerate(files, 1):
        if dcx_broken(path):
            broken.append(path)
            print(f'damaged: {path.relative_to(game)}')
        if number % 1000 == 0:
            print(f'{number} of {len(files)} checked', file=sys.stderr)
    print(f'{len(broken)} of {len(files)} .dcx files do not unpack')
    return 1 if broken else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
