#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic PKG files for testing bb-recomp's PKG step without the real (tens of GB) PKGs, and a stand-in for ps4-pkg-tool.

  make_fake_pkgs.py <out dir> <game folder to serve as the base game>

Writes <out dir>/pkgs/: a base game PKG (gd, 01.00 header) and an update PKG (gp, 01.09), both with a real PKG header and param.sfo (the
parts bb-recomp reads itself), and <out dir>/fake-pkg-tool (a Python script with the ps4-pkg-tool command line: PKG STAGING). The "base"
unpacks to a hard-linked copy of <game folder>; the "update" to a few files, one of which replaces a file of the game, so that the
merge is exercised. The real ps4-pkg-tool is exercised by tests/recomp-pkgs/run.sh on a real data-mod PKG when BB_TEST_MOD_PKG is set.
"""
import os, struct, sys

def sfo(values):
    keys, data, entries = b'', b'', b''
    for k, v in values.items():
        raw = v.encode() + b'\0'
        entries += struct.pack('<HHIII', len(keys), 0x0204, len(raw), len(raw), len(data))
        keys += k.encode() + b'\0'
        data += raw
    key_start = 20 + len(entries)
    data_start = (key_start + len(keys) + 3) & ~3
    return b'\0PSF' + struct.pack('<IIII', 0x101, key_start, data_start, len(values)) + entries + keys + b'\0' * (data_start - key_start - len(keys)) + data

def pkg(content_id, sfo_bytes, tail=b''):
    head = bytearray(0x440)
    head[0:4] = b'\x7fCNT'
    head[0x40:0x40 + len(content_id)] = content_id.encode()
    struct.pack_into('>I', head, 0x74, 0x1a)
    struct.pack_into('>I', head, 0x10, 1)
    struct.pack_into('>I', head, 0x18, 0x440)
    entry = struct.pack('>IIIIIIII', 0x1000, 0, 0, 0, 0x440 + 32, len(sfo_bytes), 0, 0)
    body = bytes(head) + entry + sfo_bytes + tail + b'\0' * 4096
    body = bytearray(body)
    struct.pack_into('>Q', body, 0x430, len(body))
    return bytes(body)

def main():
    out, game = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    os.makedirs(os.path.join(out, 'pkgs'), exist_ok=True)
    base = pkg('EP9000-CUSA03173_00-BLOODBORNE0000EU', sfo({'TITLE_ID': 'CUSA03173', 'APP_VER': '01.00', 'CATEGORY': 'gd'}), b'\nPLAN base ' + game.encode() + b'\n')
    upd = pkg('EP9000-CUSA03173_00-BLOODBORNE0000EU', sfo({'TITLE_ID': 'CUSA03173', 'APP_VER': '01.09', 'CATEGORY': 'gp'}), b'\nPLAN update\n')
    open(os.path.join(out, 'pkgs', 'EP9000-CUSA03173_00-BLOODBORNE0000EU-A0100-V0100.pkg'), 'wb').write(base)
    open(os.path.join(out, 'pkgs', 'EP9000-CUSA03173-PATCH_00-BLOODBORNE0000EU-A0109-V0100.pkg'), 'wb').write(upd)
    tool = os.path.join(out, 'fake-pkg-tool')
    open(tool, 'w').write('''#!/usr/bin/env python3
import os, shutil, subprocess, sys
pkg, staging = sys.argv[1], sys.argv[2]
data = open(pkg, 'rb').read()
plan = [l for l in data.split(b'\\n') if l.startswith(b'PLAN ')][0].split(b' ', 2)
root = os.path.join(staging, 'CUSA03173')
print('Progress-bytes: 0 1000', flush=True)
if plan[1] == b'base':
    subprocess.check_call(['cp', '-al', plan[2].decode(), root])   # hard links: the "extraction" costs nothing
else:
    os.makedirs(os.path.join(root, 'sce_sys', 'changeinfo'))
    open(os.path.join(root, 'sce_sys', 'changeinfo', 'bb_test_marker.txt'), 'w').write('update 1.09\\n')
    shutil.copyfile(os.path.join(os.path.dirname(os.path.abspath(pkg)), '..', 'game-sfo.bin'), os.path.join(root, 'sce_sys', 'param.sfo'))
print('Progress-bytes: 1000 1000', flush=True)
''')
    os.chmod(tool, 0o755)
    # the update's param.sfo (a copy of the game's own, so the result stays the 1.09 game)
    open(os.path.join(out, 'game-sfo.bin'), 'wb').write(open(os.path.join(game, 'sce_sys', 'param.sfo'), 'rb').read())
    print(os.path.join(out, 'pkgs'), tool)

main()
