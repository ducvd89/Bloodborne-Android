#!/usr/bin/env python3
"""Fetch a Debian trixie arm64 sysroot for cross-building the native ARM64 runtime.

The Thor's Linux userland (fexdroid's rootfs) is Debian 13 (trixie) arm64: building against the
same release keeps glibc and library ABIs identical. Packages come from deb.debian.org over HTTPS
and every .deb is checked against the SHA-256 in the Packages index. The host is not modified.
"""
import argparse
import hashlib
import io
import json
import lzma
import os
import re
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MIRROR = 'https://deb.debian.org/debian'
SUITE = 'trixie'
# What the runtime, the GPU library and FEXCore link against (dependencies follow).
WANTED = [
    'libc6-dev', 'libstdc++-14-dev', 'libgcc-14-dev', 'linux-libc-dev',
    'libvulkan-dev', 'libfmt-dev', 'libxxhash-dev', 'libsdl3-dev', 'libx11-dev', 'libzydis-dev',
    'libavformat-dev', 'libavcodec-dev', 'libavutil-dev', 'libswscale-dev', 'libswresample-dev',
    'spirv-headers', 'zlib1g-dev',
]
# Not needed to compile or link (package management, scripts, documentation, data).
SKIP = re.compile(r'^(debconf|perl.*|dpkg|adduser|tzdata|sensible-utils|libc-bin|libc-dev-bin|'
                  r'fonts-.*|.*-data|.*-doc|ucf|lsb-base|init-system-helpers|'
                  r'python3.*|libpython.*|dbus.*|systemd.*|libsystemd-shared|'
                  r'readline-common|mailcap|media-types|netbase|x11-common|coreutils|bash|'
                  r'base-files|login|util-linux|mount)$')


def fetch(url):
    with urllib.request.urlopen(url, timeout=120) as response:
        return response.read()


def parse_packages(text):
    packages = {}
    for block in text.split('\n\n'):
        fields, key = {}, None
        for line in block.splitlines():
            if line.startswith(' ') and key:
                fields[key] += '\n' + line
            elif ':' in line:
                key, value = line.split(':', 1)
                fields[key] = value.strip()
        if 'Package' in fields:
            packages.setdefault(fields['Package'], fields)
            for provided in fields.get('Provides', '').split(','):
                name = provided.strip().split(' ')[0]
                if name:
                    packages.setdefault('virtual:' + name, fields)
    return packages


def dependencies(fields):
    names = []
    for key in ('Pre-Depends', 'Depends'):
        for group in fields.get(key, '').split(','):
            group = group.strip()
            if group:
                # First alternative, without version or architecture qualifiers.
                names.append(re.split(r'[ (:]', group.split('|')[0].strip())[0])
    return names


def sysroot_member(member, path):
    """Absolute symlinks point into the target system: made relative, inside the sysroot."""
    if member.issym() and member.linkname.startswith('/'):
        base = os.path.dirname(os.path.normpath(member.name))
        member = member.replace(linkname=os.path.relpath(member.linkname.lstrip('/'), base or '.'))
    return tarfile.tar_filter(member, path)


def extract_deb(data, dest):
    """A .deb is an ar archive; its data.tar.* holds the files."""
    assert data[:8] == b'!<arch>\n', 'not a .deb'
    offset = 8
    while offset < len(data):
        name = data[offset:offset + 16].decode().strip().rstrip('/')
        size = int(data[offset + 48:offset + 58].decode().strip())
        body = data[offset + 60:offset + 60 + size]
        offset += 60 + size + (size & 1)
        if name.startswith('data.tar'):
            with tarfile.open(fileobj=io.BytesIO(body)) as tar:
                tar.extractall(dest, filter=sysroot_member)
            return
    raise RuntimeError('no data.tar in .deb')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT / '.local-deps/android/arm64-sysroot')
    parser.add_argument('--packages', nargs='+', default=WANTED,
                        help='packages to fetch instead of the runtime build set (build_turnip.sh)')
    args = parser.parse_args()
    out = args.out
    cache = out.parent / 'arm64-debs'
    cache.mkdir(parents=True, exist_ok=True)
    index = lzma.decompress(fetch(f'{MIRROR}/dists/{SUITE}/main/binary-arm64/Packages.xz')).decode()
    packages = parse_packages(index)
    chosen, queue = {}, list(args.packages)
    while queue:
        name = queue.pop()
        if name in chosen or SKIP.match(name):
            continue
        fields = packages.get(name) or packages.get('virtual:' + name)
        if not fields:
            raise SystemExit(f'package not found: {name}')
        if fields['Package'] in chosen or SKIP.match(fields['Package']):
            chosen.setdefault(name, None)
            continue
        chosen[fields['Package']] = fields
        queue.extend(dependencies(fields))
    records = []
    for name, fields in sorted((k, v) for k, v in chosen.items() if v):
        path = cache / Path(fields['Filename']).name
        if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != fields['SHA256']:
            data = fetch(f"{MIRROR}/{fields['Filename']}")
            if hashlib.sha256(data).hexdigest() != fields['SHA256']:
                raise SystemExit(f'SHA-256 mismatch: {name}')
            path.write_bytes(data)
        extract_deb(path.read_bytes(), out)
        records.append({'package': name, 'version': fields['Version'], 'sha256': fields['SHA256']})
    # Debian is merged-/usr: absolute paths in linker scripts name /lib and /usr/lib.
    for link in ('lib', 'bin', 'sbin'):
        target = out / link
        if not target.exists() and not target.is_symlink():
            target.symlink_to(f'usr/{link}')
    (out / 'packages.json').write_text(json.dumps(records, indent=2) + '\n')
    print(f'{len(records)} packages in {out}')


if __name__ == '__main__':
    main()
