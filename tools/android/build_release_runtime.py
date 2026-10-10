#!/usr/bin/env python3
"""Build the game-free Android runtime archive embedded in the release APK.

Inputs are the migrated Debian/FEX rootfs, Turnip from build_turnip.sh (in place of the
rootfs's own) and the local native ARM64 build.
Python packages are fetched from Debian trixie with Packages-index SHA-256 checks.
"""
import hashlib
import lzma
import os
import posixpath
import subprocess
import tarfile
from pathlib import Path

from fetch_arm64_sysroot import MIRROR, SUITE, extract_deb, fetch, parse_packages

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / '.local-deps/android/migration/x/rootfs'
NATIVE = ROOT / 'out/arm64/bundle'
TURNIP = ROOT / 'out/arm64/turnip'
TURNIP_GEN8 = ROOT / 'out/arm64/turnip-gen8'
LSFG = ROOT / 'out/arm64/lsfg'  # build_lsfg.sh: the lsfg-vk layer, unmodified
# The rootfs's Turnip, replaced by TURNIP's (rootfs-relative path: file there).
TURNIP_FILES = {
    'usr/lib/aarch64-linux-gnu/libvulkan_freedreno.so': 'libvulkan_freedreno.so',
    'usr/share/vulkan/icd.d/freedreno_icd.aarch64.json': 'freedreno_icd.aarch64.json',
    'usr/share/drirc.d/00-mesa-defaults.conf': 'drirc.d/00-mesa-defaults.conf',
    'usr/share/drirc.d/00-turnip-defaults.conf': 'drirc.d/00-turnip-defaults.conf',
}
# The Adreno 8xx variant (build_turnip.sh gen8), beside it; run-thor.sh picks one.
TURNIP_GEN8_FILES = {
    'usr/lib/aarch64-linux-gnu/libvulkan_freedreno_gen8.so': 'libvulkan_freedreno_gen8.so',
    'usr/share/vulkan/icd.d/freedreno_gen8_icd.aarch64.json': 'freedreno_gen8_icd.aarch64.json',
}
OUT = ROOT / 'out/release'
PYTHON = ('python3.13-minimal', 'libpython3.13-minimal', 'libpython3.13-stdlib')
SCRIPTS = ('prepare_game.py', 'prepare.py', 'link_modules.py', 'link_libc.py',
           'content_profile.py', 'game_check.py')
EXCLUDE_ROOTFS = ('dev', 'home', 'root', 'run', 'tmp', 'var', 'opt',
                  'usr/include', 'usr/share/man',
                  'etc/shadow', 'etc/shadow-', 'etc/gshadow', 'etc/gshadow-',
                  'etc/passwd', 'etc/passwd-', 'etc/group', 'etc/group-',
                  'etc/hostname', 'etc/hosts', 'etc/resolv.conf', 'etc/machine-id',
                  'etc/ssh')


def checked_packages(dest):
    index = parse_packages(lzma.decompress(fetch(
        f'{MIRROR}/dists/{SUITE}/main/binary-arm64/Packages.xz')).decode())
    records = []
    for name in PYTHON:
        record = index[name]
        data = fetch(f"{MIRROR}/{record['Filename']}")
        if hashlib.sha256(data).hexdigest() != record['SHA256']:
            raise ValueError(f'Debian SHA-256 mismatch: {name}')
        extract_deb(data, dest)
        records.append(f"{name} {record['Version']} {record['SHA256']}")
    return records


def scrub(member):
    relative = member.name.removeprefix('rootfs/').rstrip('/')
    if relative and any(relative == item or relative.startswith(item + '/')
                        for item in EXCLUDE_ROOTFS):
        return None
    if member.name.startswith('rootfs/') and relative in TURNIP_FILES:
        return None
    if member.isdev() or member.isfifo():
        return None
    if member.issym() and member.linkname.startswith('/'):
        target = member.linkname
        own_root = '/data/data/com.ducvd89.bloodborne/files/rootfs/'
        if target.startswith(own_root):
            target = target[len(own_root):]
        elif target.startswith(('/system/', '/dev/', '/proc/', '/sys/', '/mnt/', '/storage/')):
            return None
        else:
            target = target.lstrip('/')
        member.linkname = posixpath.relpath('rootfs/' + target,
                                            posixpath.dirname(member.name))
    member.uid = member.gid = 0
    member.uname = member.gname = ''
    return member


def owned_by_root(member):
    member.uid = member.gid = 0
    member.uname = member.gname = ''
    return member


def main():
    if not (SOURCE / 'usr/bin/FEX').is_file() or not (SOURCE / 'usr/lib/aarch64-linux-gnu/libvulkan_freedreno.so').is_file():
        raise SystemExit('Missing migrated FEX/Turnip rootfs')
    if not (NATIVE / 'bin/bb-probe').is_file() or not (NATIVE / 'lib/libbbcpu.so').is_file():
        raise SystemExit('Build the native ARM64 bundle first')
    if not all((TURNIP / name).is_file() for name in TURNIP_FILES.values()):
        raise SystemExit('Build Turnip first: tools/android/build_turnip.sh')
    if not all((TURNIP_GEN8 / name).is_file() for name in TURNIP_GEN8_FILES.values()):
        raise SystemExit('Build the Adreno 8xx Turnip first: tools/android/build_turnip.sh gen8')
    if not (LSFG / 'liblsfg-vk-layer.so').is_file():
        raise SystemExit('Build the Lossless Scaling layer first: tools/android/build_lsfg.sh')
    OUT.mkdir(exist_ok=True)
    overlay = OUT / 'python-overlay'
    overlay.mkdir(exist_ok=True)
    records = checked_packages(overlay)
    subprocess.run(['patchelf', '--set-interpreter',
                    '/data/data/com.ducvd89.bloodborne/files/rootfs/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1',
                    str(overlay / 'usr/bin/python3.13')], check=True)
    archive = OUT / 'runtime.tar.gz'
    with tarfile.open(archive, 'w:gz', compresslevel=6, format=tarfile.PAX_FORMAT) as tar:
        tar.add(SOURCE, arcname='rootfs', filter=scrub)
        # Overlay Debian Python after the base rootfs. Its standard library is needed to
        # link the user's game files at first launch, never shipped in the APK.
        for child in sorted(overlay.iterdir()):
            tar.add(child, arcname='rootfs/' + child.name, filter=scrub)
        for relative, name in TURNIP_FILES.items():
            tar.add(TURNIP / name, arcname='rootfs/' + relative, filter=owned_by_root)
        for relative, name in TURNIP_GEN8_FILES.items():
            tar.add(TURNIP_GEN8 / name, arcname='rootfs/' + relative, filter=owned_by_root)
        for name, mode in (('rootfs/tmp', 0o1777), ('rootfs/tmp/.X11-unix', 0o1777),
                           ('rootfs/run', 0o755), ('rootfs/home', 0o755),
                           ('rootfs/var', 0o755),
                           ('rootfs/var/cache', 0o755), ('rootfs/var/log', 0o755)):
            info = tarfile.TarInfo(name)
            info.type = tarfile.DIRTYPE
            info.mode = mode
            tar.addfile(info)
        for directory in ('bin', 'lib', 'patch-parts'):
            tar.add(NATIVE / directory, arcname='bbport/arm64/' + directory)
        # FSR 4 v07 INT8 assets (MIT: built from AMD's FSR 4 source), fetched with
        # tools/fetch_fsr4_assets.sh: the upscaler=fsr4 setting needs them.
        if (NATIVE / 'fsr4_shaders').is_dir():
            tar.add(NATIVE / 'fsr4_shaders', arcname='bbport/arm64/fsr4_shaders')
        tar.add(LSFG, arcname='bbport/arm64/lsfg', filter=owned_by_root)
        for script in SCRIPTS:
            origin = (ROOT / 'tools/android' if script == 'prepare_game.py' else ROOT / 'scripts') / script
            tar.add(origin, arcname='bbport/scripts/' + script)
        for source, name in (
            ('tools/android/run-thor.sh', 'run-thor.sh'),
            ('tools/android/fex-compatible.json', 'fex-compatible.json'),
            ('tools/android/thor-gamecontrollerdb.txt', 'thor-gamecontrollerdb.txt'),
            ('tools/android/thor.ini', 'bbport.default.ini'),
        ):
            tar.add(ROOT / source, arcname='bbport/' + name)
        for name in ('bbport/data', 'bbport/logs', 'bbport/user'):
            info = tarfile.TarInfo(name)
            info.type = tarfile.DIRTYPE
            info.mode = 0o755
            tar.addfile(info)
    digest = hashlib.file_digest(archive.open('rb'), 'sha256').hexdigest()
    (OUT / 'runtime.sha256').write_text(digest + '\n')
    (OUT / 'runtime.python-packages.txt').write_text('\n'.join(records) + '\n')
    with tarfile.open(archive) as tar:
        names = tar.getnames()
    banned = ('boot-linked.bin', 'eboot.bin', 'eboot.elf', 'game/', 'save/', 'Lossless.dll')
    if any(any(word in name for word in banned) for name in names):
        archive.unlink()
        raise SystemExit('Refusing to package a game-derived file')
    print(f'Runtime: {archive} ({archive.stat().st_size:,} bytes), sha256 {digest}')


if __name__ == '__main__':
    main()
