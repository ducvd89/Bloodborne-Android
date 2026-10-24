#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds an Ubuntu 22.04 (glibc 2.35) sysroot in .local-deps/sysroot-jammy without root: downloads the .deb files of a few -dev
packages and everything they depend on, and unpacks them. The Linux build of the Rebuild Tool is compiled and linked against it, so
that the AppImage runs on any distribution with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36, ...), not only on the
build machine's own.   tools/recomp/fetch-sysroot.py [dest]
"""
import gzip, io, os, re, subprocess, sys, tarfile, urllib.request

DEST = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '../../.local-deps/sysroot-jammy'))
MIRROR = 'https://archive.ubuntu.com/ubuntu'
SUITES = ['jammy-security', 'jammy-updates', 'jammy']
WANT = ['libc6-dev', 'libstdc++-12-dev', 'libgcc-12-dev', 'linux-libc-dev', 'libx11-dev', 'libxext-dev', 'libxcursor-dev', 'libxi-dev', 'libxfixes-dev',
        'libxrandr-dev', 'libxss-dev', 'libxtst-dev', 'libxkbcommon-dev', 'libwayland-dev', 'libdbus-1-dev', 'libxrender-dev', 'x11proto-dev', 'libgl-dev', 'libegl-dev', 'libgles-dev', 'libglx-dev', 'libopengl-dev', 'libfribidi-dev', 'libdecor-0-dev']
SKIP = {'debconf', 'dpkg', 'perl-base', 'libc-dev-bin', 'libc-bin', 'gcc-12', 'cpp-12'}

def fetch(url):
    with urllib.request.urlopen(url, timeout=60) as r: return r.read()

pk = {}
for suite in SUITES:
    for comp in ('main', 'universe'):
        data = gzip.decompress(fetch(f'{MIRROR}/dists/{suite}/{comp}/binary-amd64/Packages.gz')).decode()
        for block in data.split('\n\n'):
            f = dict(re.findall(r'^([A-Za-z-]+): (.*)$', block, re.M))
            if 'Package' in f and f['Package'] not in pk: pk[f['Package']] = f
        for block in []: pass
provides = {}
for name, f in pk.items():
    for p in re.split(r',\s*', f.get('Provides', '')):
        if p: provides.setdefault(p.split(' ')[0], name)

need, todo = {}, list(WANT)
while todo:
    n = todo.pop()
    if n in need or n in SKIP: continue
    n = n if n in pk else provides.get(n)
    if not n or n in need or n in SKIP: continue
    need[n] = pk[n]
    for dep in re.split(r',\s*', pk[n].get('Depends', '')):
        alt = dep.split('|')[0].strip().split(' ')[0].split(':')[0]
        if alt: todo.append(alt)
print(len(need), 'packages')
os.makedirs(DEST, exist_ok=True)
dl = os.path.join(DEST, '.debs'); os.makedirs(dl, exist_ok=True)
for n, f in sorted(need.items()):
    path = os.path.join(dl, os.path.basename(f['Filename']))
    if not os.path.exists(path):
        open(path, 'wb').write(fetch(f"{MIRROR}/{f['Filename']}"))
    # a .deb is an ar archive: debian-binary, control.tar.*, data.tar.*
    subprocess.check_call(f'cd "{DEST}" && ar p "{path}" $(ar t "{path}" | grep "^data.tar") | tar -x --zstd -f - 2>/dev/null || ar p "{path}" $(ar t "{path}" | grep "^data.tar") | tar -xJf - 2>/dev/null || ar p "{path}" $(ar t "{path}" | grep "^data.tar") | tar -xzf -', shell=True)
# The packages have absolute symlinks (/lib64/ld-linux-x86-64.so.2 -> /lib/x86_64-linux-gnu/...): make them relative inside the sysroot.
for root, dirs, files in os.walk(DEST):
    if '.debs' in root: continue
    for name in dirs + files:
        path = os.path.join(root, name)
        if os.path.islink(path):
            target = os.readlink(path)
            if target.startswith('/'):
                os.unlink(path)
                os.symlink(os.path.relpath(DEST + target, root), path)
print('sysroot:', DEST)
