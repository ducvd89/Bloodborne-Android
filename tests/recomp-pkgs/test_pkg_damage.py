#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The damage check of bb-recomp (--check-pkg, and the check that runs before anything is unpacked), on copies of a real PKG:
a good copy passes; a truncated copy, a flipped byte in the header, in the body and in the PFS image are caught and named; and a damaged PKG in
a folder stops the build before anything is unpacked. The original is never touched.
   test_pkg_damage.py <bb-recomp binary> <real .pkg> <work dir>"""
import os, shutil, struct, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_fake_pkgs import pkg, sfo

exe, real, work = sys.argv[1], sys.argv[2], os.path.abspath(sys.argv[3])
shutil.rmtree(work, ignore_errors=True); os.makedirs(work)
data = open(real, 'rb').read()
body_off, pfs_off = struct.unpack('>Q', data[0x20:0x28])[0], struct.unpack('>Q', data[0x410:0x418])[0]
failed = 0

def check(name, blob, expect_ok, needles=()):
    global failed
    path = os.path.join(work, name + '.pkg'); open(path, 'wb').write(blob)
    p = subprocess.run([exe, '--check-pkg', path], capture_output=True, text=True, timeout=300)
    text = p.stdout + p.stderr
    ok = (p.returncode == 0) == expect_ok and all(n in text for n in needles)
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else '\n  rc=%d: %s' % (p.returncode, text[-500:])))
    failed += not ok

def flip(at):
    b = bytearray(data); b[at] ^= 0x01; return bytes(b)

check('good_copy', data, True, ['no damage found', 'matched: header', 'matched: body'])
check('truncated_half', data[:len(data) // 2], False, ['incomplete', 'should be %d bytes and has %d' % (len(data), len(data) // 2)])
check('truncated_tail', data[:-4096], False, ['should be %d bytes and has %d' % (len(data), len(data) - 4096)])
check('flipped_header_byte', flip(0x300), False, ['DAMAGED', 'header digest mismatch'])
check('flipped_body_byte', flip(body_off + 16), False, ['DAMAGED', 'body digest mismatch'])
check('flipped_image_byte', flip(pfs_off + 100), False, ['DAMAGED', 'image start digest mismatch'])
check('not_a_pkg', b'this is not a pkg' * 1000, False, ['not a PS4 PKG'])

# in a folder: the build stops before unpacking, names the file and what to do
base = pkg('EP9000-CUSA03173_00-BLOODBORNE0000EU', sfo({'TITLE_ID': 'CUSA03173', 'APP_VER': '01.00', 'CATEGORY': 'gd'}), b'\nPLAN base x\n')
for name, blob in (('bad_body', flip(body_off + 16)), ('truncated', data[:len(data) // 2])):
    d = os.path.join(work, 'folder_' + name); os.makedirs(os.path.join(d, 'pkgs'))
    open(os.path.join(d, 'pkgs', 'base.pkg'), 'wb').write(base); open(os.path.join(d, 'pkgs', 'update.pkg'), 'wb').write(blob)
    tool = os.path.join(d, 'must-not-run'); open(tool, 'w').write('#!/bin/sh\ntouch "%s/RAN"\n' % d); os.chmod(tool, 0o755)
    p = subprocess.run([exe, '--pkgs', os.path.join(d, 'pkgs'), '--out', os.path.join(d, 'out'), '--pkg-tool', tool], capture_output=True, text=True, timeout=300)
    text = p.stdout + p.stderr
    want = ['update.pkg is damaged', 'body digest mismatch', 'Download or dump that file again'] if name == 'bad_body' else ['update.pkg', 'should be', 'cut short']
    ok = p.returncode != 0 and all(w in text for w in want) and not os.path.exists(os.path.join(d, 'RAN')) and not os.path.exists(os.path.join(d, 'out', 'CUSA03173'))
    print(('ok   ' if ok else 'FAIL ') + 'folder_' + name + ('' if ok else '\n  rc=%d: %s' % (p.returncode, text[-600:])))
    failed += not ok

# the update must not be older than the base
d = os.path.join(work, 'folder_old_update'); os.makedirs(os.path.join(d, 'pkgs'))
b2 = pkg('EP9000-CUSA03173_00-BLOODBORNE0000EU', sfo({'TITLE_ID': 'CUSA03173', 'APP_VER': '01.09', 'CATEGORY': 'gd'}), b'\nPLAN base x\n')
u2 = pkg('EP9000-CUSA03173_00-BLOODBORNE0000EU', sfo({'TITLE_ID': 'CUSA03173', 'APP_VER': '01.06', 'CATEGORY': 'gp'}), b'\nPLAN update\n')
open(os.path.join(d, 'pkgs', 'base.pkg'), 'wb').write(b2); open(os.path.join(d, 'pkgs', 'update.pkg'), 'wb').write(u2)
p = subprocess.run([exe, '--pkgs', os.path.join(d, 'pkgs'), '--out', os.path.join(d, 'out')], capture_output=True, text=True, timeout=60)
ok = p.returncode != 0 and 'older than the base game' in p.stdout + p.stderr
print(('ok   ' if ok else 'FAIL ') + 'update_older_than_base'); failed += not ok
sys.exit(1 if failed else 0)
