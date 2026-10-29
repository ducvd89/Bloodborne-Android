#!/usr/bin/env python3
"""Build the game-free Android runtime archive embedded in the release APK.

Bionic only: the Android NDK build of the game process (build_bionic.sh: bb-probe, libbbgpu, libbbcpu (the native units engine, no FEXCore),
bb-prepare, kgsl-chip-id), the Bionic PKG tool (build_pkg_tool.sh android), the pre-compiled game patches
(build_patch_parts.py) and the launch script. No Linux rootfs, no glibc libraries, no Python.

Archive layout (everything under bbport/, extracted into the app's files directory):
  bbport/bionic/bin/   bb-probe bb-prepare bb-texpack kgsl-chip-id ps4-pkg-tool   (stripped, executable)
  bbport/bionic/lib/   libbbgpu.so libbbcpu.so libbloodframe.so libbbnet20.so libc++_shared.so   (libbloodframe.so: Lossless
                       Scaling frame generation, loaded by libbbgpu only when it is on; needs the player's own Lossless.dll.
                       libbbnet20.so: online play, loaded by bb-probe only with BB_ONLINE=1)
  bbport/bionic/share/ cacert.pem   (Mozilla's CA bundle: the online module's HTTPS trust, BB_CA_BUNDLE overrides)
  bbport/patch-parts/  the settings' BBPATCH2 files (effects, model LOD, resolutions, cheats, ...)
  bbport/run-bionic.sh bbport/bbport.default.ini
  bbport/licenses/     ps4-pkg-tool.NOTICE, bloodframe-* (the licences of what libbloodframe.so contains), cacert-source.txt and the
                       licences of what libbbnet20.so contains (OpenSSL, protobuf, miniupnpc, nlohmann/json, cpp-httplib)
  bbport/data bbport/logs bbport/user   (empty folders)
"""
import hashlib
import os
import posixpath
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/android'
BIONIC = ROOT / 'out/bionic'  # build_bionic.sh (+ build_pkg_tool.sh android)
EBOOT = ROOT / 'out/eboot.elf'  # the patch compiler reads the 1.09 code from it; never packaged
NDK_STRIP = next(iter(sorted((ROOT / '.local-deps/android/sdk/ndk').glob(
    '*/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip'))), None)
OUT = ROOT / 'out/release'
PROGRAMS = ('bb-probe', 'bb-prepare', 'bb-texpack', 'kgsl-chip-id', 'ps4-pkg-tool')
LIBRARIES = ('libbbgpu.so', 'libbbcpu.so', 'libbloodframe.so', 'libbbnet20.so', 'libc++_shared.so')
# build_bionic.sh (+ build_bionic_deps.sh): the online module's CA bundle and the licences of its static libraries.
CA_BUNDLE = 'cacert.pem'
ONLINE_LICENSES = ('cacert-source.txt', 'openssl.LICENSE.txt', 'protobuf.LICENSE', 'miniupnpc.LICENSE',
                   'nlohmann-json.LICENSE.MIT', 'httplib.LICENSE')
# build_bloodframe.sh (run by build_bionic.sh): the licences of libbloodframe.so's contents, shipped with it.
LSFG_LICENSES = ('bloodframe-lsfg-vk.LICENSE.md', 'bloodframe-dxbc.LICENSE', 'bloodframe-pe-parse.LICENSE', 'bloodframe-volk.LICENSE.md',
                 'bloodframe-source.txt')
FILES = (
    ('tools/android/run-bionic.sh', 'run-bionic.sh', 0o755),
    ('tools/android/thor.ini', 'bbport.default.ini', 0o644),
    ('tools/android/ps4-pkg-tool.NOTICE', 'licenses/ps4-pkg-tool.NOTICE', 0o644),
)
EMPTY_DIRS = ('bbport/data', 'bbport/logs', 'bbport/user')
# Words that must not appear in a member name: game-derived files, and what the Linux runtime shipped.
BANNED = ('boot-linked.bin', 'eboot.bin', 'eboot.elf', 'game/', 'save/', 'Lossless.dll',
          'rootfs', 'python', 'Xvfb', 'fexdroid', 'arm64/', '.py')


def strip(path):
    if NDK_STRIP:
        subprocess.run([str(NDK_STRIP), '--strip-debug', str(path)], check=True)


def stage_runtime(stage):
    """bin/ and lib/ without debug info, executable (the app execs them from its data), and the patches."""
    shutil.rmtree(stage, ignore_errors=True)
    for directory, names in (('bin', PROGRAMS), ('lib', LIBRARIES)):
        (stage / 'bionic' / directory).mkdir(parents=True)
        for name in names:
            source = BIONIC / directory / name
            if not source.is_file():
                raise SystemExit(f'Missing {source}: build_bionic.sh builds it'
                                 + (' (build_pkg_tool.sh android for ps4-pkg-tool)' if name == 'ps4-pkg-tool' else ''))
            target = stage / 'bionic' / directory / name
            shutil.copy2(source, target)
            strip(target)
            target.chmod(0o755)
    source = BIONIC / 'share' / CA_BUNDLE
    if not source.is_file():
        raise SystemExit(f'Missing {source}: build_bionic.sh copies it from the deps')
    (stage / 'bionic' / 'share').mkdir(parents=True)
    shutil.copy2(source, stage / 'bionic' / 'share' / CA_BUNDLE)
    (stage / 'bionic' / 'share' / CA_BUNDLE).chmod(0o644)
    # Anything else in out/bionic/lib would be a library the runtime does not know: refuse a surprise.
    extra = sorted(p.name for p in (BIONIC / 'lib').iterdir() if p.name not in LIBRARIES)
    if extra:
        raise SystemExit(f'Unexpected libraries in {BIONIC / "lib"}: {", ".join(extra)} (add them to LIBRARIES if needed)')
    if not EBOOT.is_file():
        raise SystemExit(f'The game patches are compiled from the 1.09 eboot: {EBOOT} is missing')
    subprocess.run([sys.executable, str(TOOLS / 'build_patch_parts.py'), '--out', str(stage / 'patch-parts'),
                    '--eboot', str(EBOOT)], check=True)


def owned_by_root(member):
    member.uid = member.gid = 0
    member.uname = member.gname = ''
    return member


def check_links(tar):
    """Refuses symbolic and hard links that leave the tree (absolute, or climbing out of bbport/), devices and fifos."""
    for member in tar.getmembers():
        if member.isdev() or member.isfifo():
            raise SystemExit(f'Refusing to archive a device or fifo: {member.name}')
        if member.issym() or member.islnk():
            target = member.linkname
            if member.islnk():
                resolved = posixpath.normpath(target)
            elif target.startswith('/'):
                raise SystemExit(f'Refusing a symlink with an absolute target: {member.name} -> {target}')
            else:
                resolved = posixpath.normpath(posixpath.join(posixpath.dirname(member.name), target))
            if resolved.startswith('/') or resolved == '..' or resolved.startswith('../') \
                    or not (resolved == 'bbport' or resolved.startswith('bbport/')):
                raise SystemExit(f'Refusing a link that leaves the tree: {member.name} -> {target}')
        if member.name.startswith('/') or '..' in member.name.split('/'):
            raise SystemExit(f'Refusing an unsafe member name: {member.name}')


def main():
    OUT.mkdir(exist_ok=True)
    stage = OUT / 'runtime-stage'
    stage_runtime(stage)
    archive = OUT / 'runtime.tar.gz'
    with tarfile.open(archive, 'w:gz', compresslevel=6, format=tarfile.PAX_FORMAT) as tar:
        tar.add(stage / 'bionic', arcname='bbport/bionic', filter=owned_by_root)
        tar.add(stage / 'patch-parts', arcname='bbport/patch-parts', filter=owned_by_root)
        for source, name, mode in FILES:
            info = tar.gettarinfo(str(ROOT / source), arcname='bbport/' + name)
            info.mode = mode
            owned_by_root(info)
            with open(ROOT / source, 'rb') as handle:
                tar.addfile(info, handle)
        for name in LSFG_LICENSES + ONLINE_LICENSES:
            source = BIONIC / 'licenses' / name
            if not source.is_file():
                raise SystemExit(f'Missing {source}: tools/android/build_bloodframe.sh writes it')
            info = tar.gettarinfo(str(source), arcname='bbport/licenses/' + name)
            info.mode = 0o644
            owned_by_root(info)
            with open(source, 'rb') as handle:
                tar.addfile(info, handle)
        for name in EMPTY_DIRS:
            info = tarfile.TarInfo(name)
            info.type = tarfile.DIRTYPE
            info.mode = 0o755
            tar.addfile(info)
    with tarfile.open(archive) as tar:
        check_links(tar)
        names = tar.getnames()
    bad = [name for name in names if any(word in name for word in BANNED)]
    if bad:
        archive.unlink()
        raise SystemExit('Refusing to package a game-derived or Linux-runtime file: ' + ', '.join(bad[:5]))
    digest = hashlib.file_digest(archive.open('rb'), 'sha256').hexdigest()
    (OUT / 'runtime.sha256').write_text(digest + '\n')
    print(f'Runtime: {archive} ({archive.stat().st_size:,} bytes, {len(names)} entries), sha256 {digest}')


if __name__ == '__main__':
    main()
