#!/usr/bin/env python3
"""Variant and hook catalogs of the online module (BBSC, gpu/bbnet20), generated from its sources.

    tools/recomp/variants/gen_bbsc.py [--eboot out/eboot.elf] [--sites-tool PATH]   # writes bbsc-patches.txt and hooks-bbsc.txt here
    tools/recomp/variants/gen_bbsc.py --check                                      # exit 1 when a file is out of date

bbsc-patches.txt: the persistence tables (bbsc_patch_tables.h: compiled and run by this script: retargeted calls, entry returns, byte edits,
the fog wall jump: one group per table group, toggled at run time by SetPersistentSessionPatches / SetHostLampPatches) and the thirteen
seamless-summon patches (bloodborne_re.cpp, applied once: group bbsc:seamless, constants read from the source text).
hooks-bbsc.txt: every code hook BBSC installs on the pristine 1.09 eboot, as `hook <vaddr> <len> snap [skip]`. They come from the existing
dev tool tools/android/dev/bbsc_sites.sh (BBSC's own install code run against the eboot with the real hook installer), so they are exactly
the sites InstallGuestCodeHook is called for; that needs the eboot and the tool's build environment, or a prebuilt binary (--sites-tool).
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
RE_CPP = ROOT / 'gpu/bbnet20/bbsc/core/bloodborne_re.cpp'

# name of the replaced bytes -> (offset constant, replacement constant), from InstallSeamlessCoopPatches (bloodborne_re.cpp)
SEAMLESS = [
    ('BeckoningAreaComparison', 'BeckoningAreaComparisonOffset', 'BeckoningSignedAreaComparison'),
    ('BeckoningAreaFlagResult', 'BeckoningAreaFlagResultOffset', 'BeckoningAllowedAreaFlagResult'),
    ('ResponderBellAreaResult', 'ResponderBellAreaResultOffset', 'ResponderBellAllowedAreaResult'),
    ('ResponderBellCommonResult', 'ResponderBellCommonResultOffset', 'ResponderBellAllowedCommonResult'),
    ('ActiveBellAreaComparison', 'ActiveBellAreaComparisonOffset', 'ActiveBellSignedAreaComparison'),
    ('ActiveBellAreaFlagResult', 'ActiveBellAreaFlagResultOffset', 'ActiveBellAllowedAreaFlagResult'),
    ('ResponderSearchAreaRange', 'ResponderSearchAreaRangeResultOffset', 'ResponderSearchSignedAreaRangeResult'),
    ('SosStatusAreaRestriction', 'SosStatusAreaRestrictionOffset', 'SosStatusAllowedArea'),
    ('SummonCandidateArea', 'SummonCandidateAreaRestrictionOffset', 'SummonCandidateAllowedCrossMap'),
    ('SummonBuildArea', 'SummonBuildAreaRestrictionOffset', 'SummonBuildAllowedCrossMap'),
    ('SummonBuildWorldState', 'SummonBuildWorldStateRestrictionOffset', 'SummonBuildAllowedWorldState'),
    ('SummonBuildNegativeEvent', 'SummonBuildNegativeEventRestrictionOffset', 'SummonBuildAllowedNegativeEvent'),
    ('StageTransitionKeepMatchingCall', 'StageTransitionStopMatchingCallOffset', 'StageTransitionKeepMatchingCall'),
]


def seamless_group():
    text = RE_CPP.read_text()
    offsets = {m.group(1): int(m.group(2), 16) for m in re.finditer(r'constexpr u64 (\w+) = (0x[0-9A-Fa-f]+);', text)}
    arrays = {}
    for m in re.finditer(r'constexpr std::array<u8, (\d+)> (\w+)\s*\{([^}]*)\};', text):
        data = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{1,2})', m.group(3))]
        assert len(data) <= int(m.group(1)), m.group(2)
        arrays[m.group(2)] = bytes(data)
    out = ['', 'group bbsc:seamless']
    for name, off, rep in SEAMLESS:
        out.append(f'w {offsets[off]:x} {arrays[rep].hex()}')
    return out


def tables_groups():
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / 'gen'
        subprocess.run(['c++', '-std=c++20', '-O0', '-I', str(ROOT / 'gpu/bbnet20/bbsc'), str(HERE / 'gen_bbsc_tables.cpp'), '-o', str(exe)], check=True)
        return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.rstrip('\n').split('\n')


def hook_lines(eboot, tool):
    if tool:
        run = [tool, str(eboot), '-']
    else:
        run = ['bash', str(ROOT / 'tools/android/dev/bbsc_sites.sh'), str(eboot)]
    res = subprocess.run(run, capture_output=True, text=True, cwd=ROOT)
    # "hook    0x01874710  6 bytes tag 0 (skip flag)": the first listing is the pristine code
    sites = {}
    for line in res.stdout.split('\n'):
        if line.startswith('=== With the port'):
            break
        m = re.match(r'^hook\s+0x([0-9a-f]+)\s+(\d+) bytes tag \d+( \(skip flag\))?', line)
        if m:
            sites[int(m.group(1), 16)] = (int(m.group(2)), bool(m.group(3)))
    if not sites:
        sys.exit('no hook sites found: ' + res.stdout[-400:] + res.stderr[-400:])
    out = []
    for vaddr in sorted(sites):
        n, skip = sites[vaddr]
        out.append(f'hook {vaddr:x} {n} snap' + (' skip' if skip else ''))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--eboot', type=Path, default=ROOT / 'out/eboot.elf')
    ap.add_argument('--sites-tool', help='a built bbsc-sites binary (default: tools/android/dev/bbsc_sites.sh builds one)')
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    head = ['# Generated by tools/recomp/variants/gen_bbsc.py: do not edit.']
    patches = '\n'.join(head + ['# BBSC byte patches: the persistence tables (toggled at run time by group) and the seamless-summon patches (applied once).'] +
                        tables_groups() + seamless_group()) + '\n'
    hooks = '\n'.join(head + ['# Every InstallGuestCodeHook site of BBSC on the pristine 1.09 eboot (snap: the callback runs before the instruction, skip: it may return from the function).'] +
                      hook_lines(args.eboot, args.sites_tool)) + '\n'
    for name, text in (('bbsc-patches.txt', patches), ('hooks-bbsc.txt', hooks)):
        path = HERE / name
        if args.check:
            if not path.exists() or path.read_text() != text:
                print(f'{path} is out of date: rerun tools/recomp/variants/gen_bbsc.py', file=sys.stderr)
                sys.exit(1)
        else:
            path.write_text(text)
            print(f'{path}: {len(text.splitlines())} lines')


if __name__ == '__main__':
    main()
