#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The PKG step of bb-recomp: what it accepts and the messages it gives (nothing is unpacked or compiled in these cases).
   test_pkg_step.py <bb-recomp binary> <work dir>"""
import os, shutil, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_fake_pkgs import pkg, sfo

exe, work = sys.argv[1], os.path.abspath(sys.argv[2])
CID = 'EP9000-CUSA03173_00-BLOODBORNE0000EU'
failed = 0

def case(name, files, expect, tool=None):
    global failed
    d = os.path.join(work, name); shutil.rmtree(d, ignore_errors=True); os.makedirs(os.path.join(d, 'pkgs'))
    for fname, data in files.items(): open(os.path.join(d, 'pkgs', fname), 'wb').write(data)
    p = subprocess.run([exe, '--pkgs', os.path.join(d, 'pkgs'), '--out', os.path.join(d, 'out')] + (['--pkg-tool', tool] if tool else []),
                       capture_output=True, text=True, timeout=120)
    text = p.stdout + p.stderr
    ok = p.returncode != 0 and expect in text
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else '\n  expected %r\n  got rc=%d: %s' % (expect, p.returncode, text[-600:])))
    failed += not ok

def mk(cid=CID, title='CUSA03173', ver='01.09', cat='gp'): return pkg(cid, sfo({'TITLE_ID': title, 'APP_VER': ver, 'CATEGORY': cat}))
def mkbase(ver='01.00', title='CUSA03173'): return mk(ver=ver, cat='gd', title=title)

case('empty_folder', {}, 'There are no .pkg files')
case('not_a_pkg', {'a.pkg': b'hello world' * 100}, 'This is not a PS4 PKG file')
case('truncated', {'a.pkg': mk()[:0x300]}, 'incomplete')
case('other_game', {'a.pkg': mk(title='CUSA12345')}, "isn't a Bloodborne release")
case('dlc', {'a.pkg': mk(cat='ac')}, 'only the game (gd) and its update (gp)')
case('update_only', {'u.pkg': mk()}, 'base game PKG is missing')
case('two_bases', {'a.pkg': mkbase(), 'b.pkg': mkbase()}, 'Two base game PKGs')
case('no_109', {'a.pkg': mkbase(), 'u.pkg': mk(ver='01.06')}, 'The newest PKG is version 01.06')
case('base_only_old', {'a.pkg': mkbase()}, 'The newest PKG is version 01.00')
case('same_version_twice', {'a.pkg': mkbase(), 'u1.pkg': mk(), 'u2.pkg': mk()}, 'same version 01.09')
case('mixed_titles', {'a.pkg': mkbase(), 'u.pkg': mk(title='CUSA00207')}, 'is an update for CUSA00207')
case('no_tool', {'a.pkg': mkbase(), 'u.pkg': mk()}, 'PKG tool', tool=os.path.join(work, 'no-such-tool'))
sys.exit(1 if failed else 0)
