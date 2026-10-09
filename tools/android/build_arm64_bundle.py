#!/usr/bin/env python3
"""Stage the native ARM64 build (tools/android/build_arm64.sh) for the Thor.

bin/bb-probe runs natively in fexdroid's Debian trixie arm64 rootfs; lib/ holds libbbgpu, libbbcpu
(FEXCore) and the libraries that rootfs lacks, from the same Debian release (the sysroot).
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'out/arm64'
OURS = {'libbbgpu.so': OUT / 'gpu/libbbgpu.so', 'libbbcpu.so': OUT / 'fex/libbbcpu.so'}
# FFmpeg's libraries share internal symbols: always the same build of all of them.
FFMPEG = ('libavformat.so.61', 'libavcodec.so.61', 'libavutil.so.59', 'libswscale.so.8',
          'libswresample.so.5')


def needed(path):
    output = subprocess.check_output(['llvm-readelf', '-d', str(path)], text=True)
    return [line.split('[', 1)[1].split(']', 1)[0] for line in output.splitlines() if '(NEEDED)' in line]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sysroot', type=Path, default=ROOT / '.local-deps/android/arm64-sysroot')
    parser.add_argument('--device-libs', type=Path, required=True,
                        help='names of the shared libraries in the Thor rootfs (one per line)')
    parser.add_argument('--out', type=Path, default=OUT / 'bundle')
    args = parser.parse_args()
    have = set(args.device_libs.read_text().split())
    libdirs = [args.sysroot / 'usr/lib/aarch64-linux-gnu', args.sysroot / 'usr/lib']

    def find(name):
        if name in OURS:
            return OURS[name]
        for directory in libdirs:
            if (directory / name).exists():
                return directory / name
        raise SystemExit(f'not in the sysroot: {name}')

    ship, queue, seen = {}, [OUT / 'bb-probe', *(find(n) for n in FFMPEG)], set()
    for name in FFMPEG:
        ship[name] = find(name)
    while queue:
        for name in needed(queue.pop()):
            if name in seen:
                continue
            seen.add(name)
            if name in have and name not in OURS and name not in FFMPEG:
                continue
            ship[name] = find(name)
            queue.append(ship[name])
    stage = args.out
    if stage.exists():
        shutil.rmtree(stage)
    (stage / 'bin').mkdir(parents=True)
    (stage / 'lib').mkdir()
    shutil.copy2(OUT / 'bb-probe', stage / 'bin/bb-probe')
    # The game patches the settings screen chooses from (run-thor.sh), compiled here.
    subprocess.run([sys.executable, str(ROOT / 'tools/android/build_thor_patch_parts.py'),
                    '--out', str(stage / 'patch-parts')], check=True)
    # FSR 4 model assets (tools/fetch_fsr4_assets.sh, not in the repository), when fetched.
    if (ROOT / 'fsr4_shaders').is_dir():
        shutil.copytree(ROOT / 'fsr4_shaders', stage / 'fsr4_shaders')
    for name, source in sorted(ship.items()):
        shutil.copy2(source.resolve(), stage / 'lib' / name)
    for path in [stage / 'bin/bb-probe', *(stage / 'lib').iterdir()]:
        subprocess.run(['llvm-strip', '--strip-debug', str(path)], check=True)
        if path.parent.name == 'lib':
            subprocess.run(['patchelf', '--set-rpath', '$ORIGIN', str(path)], check=True)
    records = []
    for path in sorted(stage.rglob('*')):
        if path.is_file():
            records.append({'path': str(path.relative_to(stage)), 'bytes': path.stat().st_size,
                            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    (stage / 'manifest.json').write_text(json.dumps(records, indent=2) + '\n')
    total = sum(r['bytes'] for r in records) / 2**20
    print(f'Staged {len(records)} files, {total:.1f} MiB: {stage}')


if __name__ == '__main__':
    main()
