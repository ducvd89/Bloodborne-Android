"""Parity check: the Python mods_android.py (list / validate / apply) and its Java port (ParityCli) on the same fixture
mod and game folders must agree: the JSON of list and validate, the overlay trees (each entry: kind, link target
relative to the game or the mods folder) and the "Mods: ..." log lines.

Run by run.sh: parity.py JAVA CLASSES_DIR. Exits 1 on any difference."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent.parent
PY = ROOT / 'tools/android/mods_android.py'
JAVA, CLASSES = sys.argv[1], sys.argv[2]
failures = []


def put(base, relative, data=b'x'):
    path = Path(base) / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def run_py(*args):
    r = subprocess.run([sys.executable, '-I', str(PY), *map(str, args)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return r.stdout, r.stderr


def run_java(*args):
    r = subprocess.run([JAVA, '-cp', CLASSES, 'com.ducvd89.bloodborne.ParityCli', *map(str, args)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return r.stdout, r.stderr


def normalize(row_or_list, problems=True):
    return row_or_list


def tree(overlay, game, mods):
    """{relative path: description} of the overlay: directories, and links with their target relative to the game or
    the mods folder (so two overlays in different folders compare)."""
    overlay = Path(overlay)
    game, mods = Path(game).resolve(), Path(mods).resolve()
    out = {}
    for directory, dirs, files in os.walk(overlay, followlinks=False):
        for name in [*dirs, *files]:
            path = Path(directory) / name
            relative = path.relative_to(overlay).as_posix()
            if path.is_symlink():
                target = Path(os.readlink(path))
                if target.is_relative_to(game):
                    out[relative] = 'link -> game/' + target.relative_to(game).as_posix()
                elif target.is_relative_to(mods):
                    out[relative] = 'link -> mods/' + target.relative_to(mods).as_posix()
                else:
                    out[relative] = 'link -> ' + str(target)
            elif path.is_dir():
                out[relative] = 'dir'
            else:
                out[relative] = 'file'
    return out


def check(name, expected, actual):
    if expected != actual:
        failures.append(name)
        print(f'DIFF {name}')
        print('  python:', json.dumps(expected, ensure_ascii=False, sort_keys=True)[:2000])
        print('  java:  ', json.dumps(actual, ensure_ascii=False, sort_keys=True)[:2000])
    else:
        print(f'same {name}')


def mods_lines(stderr):
    """The "Mods:" lines, with the order of the per-file conflict lines kept."""
    return [line for line in stderr.splitlines() if line.startswith('Mods:')]


def fixture(root):
    """A game and a rich set of mods (valid, invalid, overlapping, odd layouts and cases)."""
    root = Path(root)
    game, mods = root / 'game', root / 'mods'
    put(game, 'eboot.bin', b'eboot')
    put(game, 'sce_sys/param.sfo', b'sfo')
    put(game, 'dvdroot_ps4/chr/a.dcx', b'orig')
    put(game, 'dvdroot_ps4/chr/b.dcx', b'orig-b')
    put(game, 'dvdroot_ps4/msg/menu.fmg', b'orig-msg')
    put(game, 'dvdroot_ps4/map/m10/m10_00.mapbnd.dcx', b'map')
    mods.mkdir()
    put(mods, 'Alpha/dvdroot_ps4/chr/a.dcx', b'alpha-a')
    put(mods, 'Alpha/dvdroot_ps4/chr/new_alpha.dcx', b'new')
    put(mods, 'Alpha/dvdroot_ps4/Msg/Menu.FMG', b'alpha-menu')
    put(mods, 'beta/CUSA03173/dvdroot_ps4/chr/a.dcx', b'beta-a')
    put(mods, 'beta/CUSA03173/dvdroot_ps4/parts/p.dcx', b'p')
    put(mods, 'Gamma/chr/b.dcx', b'gamma-b')
    put(mods, 'Gamma/map/m10/new.dcx', b'new-map')
    put(mods, 'Delta Wrap/inner/dvdroot_ps4/msg/extra.fmg', b'extra')
    put(mods, 'Eps/app0/dvdroot_ps4/chr/a.dcx', b'eps-a')
    put(mods, 'bad-eboot/dvdroot_ps4/chr/a.dcx')
    put(mods, 'bad-eboot/eboot.bin')
    put(mods, 'bad-sys/dvdroot_ps4/chr/a.dcx')
    put(mods, 'bad-sys/sce_sys/param.sfo')
    put(mods, 'bad-prx/dvdroot_ps4/chr/a.dcx')
    put(mods, 'bad-prx/x.sprx')
    put(mods, 'docs/readme.txt')
    (mods / 'empty').mkdir()
    put(mods, '.staging-x/dvdroot_ps4/chr/a.dcx')
    put(mods, 'loose.zip', b'zip')
    (mods / 'linked').mkdir()
    put(mods, 'linked/dvdroot_ps4/chr/a.dcx')
    os.symlink(game / 'eboot.bin', mods / 'linked/dvdroot_ps4/chr/link.dcx')
    put(mods, 'Zeta/dvdroot_ps4/chr/b.dcx', b'zeta-b')
    return game, mods


def digests(folder):
    """{relative path: sha256 of the contents} of the files under `folder` (links not followed)."""
    import hashlib
    out = {}
    for directory, _, files in os.walk(folder, followlinks=False):
        for name in files:
            path = Path(directory) / name
            out[path.relative_to(folder).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return out


def last_json(text):
    lines = text.strip().splitlines()
    return [line for line in lines[:-1] if line.startswith('progress ')], json.loads(lines[-1])


def make_sfo(values):
    import struct
    keys, data, entries = b'', b'', b''
    for key, value in values.items():
        raw = value.encode() + b'\0'
        entries += struct.pack('<HHIII', len(keys), 0x0204, len(raw), len(raw), len(data))
        keys += key.encode() + b'\0'
        data += raw
    key_start = 20 + len(entries)
    data_start = (key_start + len(keys) + 3) & ~3
    return (b'\0PSF' + struct.pack('<IIII', 0x101, key_start, data_start, len(values)) + entries + keys +
            b'\0' * (data_start - key_start - len(keys)) + data)


def real(root):
    """A real 1.09 data-mod PKG with the host ps4-pkg-tool and the real game folder (read only), when given:
    BB_TEST_MOD_PKG, BB_TEST_GAME, BB_HOST_PKG_TOOL (default out/host/pkg-tool/ps4-pkg-tool)."""
    pkg, game = os.environ.get('BB_TEST_MOD_PKG'), os.environ.get('BB_TEST_GAME')
    tool = Path(os.environ.get('BB_HOST_PKG_TOOL', ROOT / 'out/host/pkg-tool/ps4-pkg-tool'))
    if not (pkg and game and Path(pkg).is_file() and Path(game).is_dir() and tool.is_file()):
        print('skip real PKG parity (needs BB_TEST_MOD_PKG, BB_TEST_GAME and the host ps4-pkg-tool)')
        return
    mods_py, mods_java = root / 'real-mods-py', root / 'real-mods-java'
    py_out, _ = run_py('install-pkg', '--pkg', pkg, '--name', 'Vietnamese', '--mods-dir', mods_py, '--game', game, '--tool', tool)
    java_out, _ = run_java('install-pkg', '--pkg', pkg, '--name', 'Vietnamese', '--mods-dir', mods_java, '--game', game, '--tool', tool)
    (py_progress, py_json), (java_progress, java_json) = last_json(py_out), last_json(java_out)
    check('real install-pkg json', py_json, java_json)
    check('real install-pkg progress', py_progress, java_progress)
    check('real install-pkg mod files', digests(mods_py), digests(mods_java))
    config = root / 'missing.json'
    py_out, _ = run_py('list', '--mods-dir', mods_py, '--config', config)
    java_out, _ = run_java('list', '--mods-dir', mods_java, '--config', config)
    expected, actual = json.loads(py_out), json.loads(java_out)
    expected['modsDir'] = actual['modsDir'] = ''
    check('real list', expected, actual)
    py_run, _ = run_py('apply', '--game', game, '--out', root / 'real-py', '--mods-dir', mods_py, '--config', config)
    java_run, _ = run_java('apply', '--game', game, '--out', root / 'real-java', '--mods-dir', mods_java, '--config', config)
    check('real apply tree', tree(py_run.strip().splitlines()[-1], game, mods_py), tree(java_run.strip().splitlines()[-1], game, mods_java))
    # The game check on the real game folder (its executable image and every shader).
    sys.path.insert(0, str(ROOT / 'scripts'))
    import game_check
    found = game_check.problem(Path(game))
    java_out, _ = run_java('check', '--game', game)
    check('real game check', 'ok' if found is None else ' '.join(found), java_out.strip())
    # install-game: the PKG (an update) merged into a small fake base with the real executable (the game check
    # recognises it), once with the Python and once with the Java.
    import shutil
    results = []
    for runner, label in ((run_py, 'py'), (run_java, 'java')):
        dest = root / f'games-{label}'
        base = dest / 'CUSA03173'
        (base / 'sce_sys').mkdir(parents=True)
        (base / 'dvdroot_ps4/msg').mkdir(parents=True)
        shutil.copy(Path(game) / 'eboot.bin', base / 'eboot.bin')
        (base / 'sce_sys/param.sfo').write_bytes(make_sfo({'TITLE_ID': 'CUSA03173', 'APP_VER': '01.00'}))
        (base / 'dvdroot_ps4/msg/old.fmg').write_bytes(b'old')
        out, _ = runner('install-game', '--pkg', pkg, '--dest', dest, '--tool', tool)
        progress, data = last_json(out)
        data['gameDir'] = Path(data['gameDir']).relative_to(dest).as_posix() if data['gameDir'] else None
        results.append((progress, data, digests(dest)))
    check('real install-game json', results[0][1], results[1][1])
    check('real install-game progress', results[0][0], results[1][0])
    check('real install-game files', results[0][2], results[1][2])
    print('real install-game:', results[1][1])


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        game, mods = fixture(root)
        configs = {
            'no-config': None,
            'order+disabled': {'order': ['Zeta', 'Gamma', 'Alpha', 'gone', 'beta'], 'disabled': ['Gamma', 'bad-eboot']},
            'only-disabled': {'disabled': ['Alpha']},
            'empty-object': {},
        }
        raw_configs = {'bad-json': '{not json', 'array': '[]', 'order-not-list': '{"order": "A"}', 'disabled-not-strings': '{"disabled": [1]}'}
        files = {}
        for key, value in configs.items():
            if value is not None:
                (root / f'{key}.json').write_text(json.dumps(value))
                files[key] = root / f'{key}.json'
            else:
                files[key] = root / 'missing.json'
        for key, text in raw_configs.items():
            (root / f'{key}.json').write_text(text)
            files[key] = root / f'{key}.json'
        for key, config in files.items():
            py_out, _ = run_py('list', '--mods-dir', mods, '--config', config)
            java_out, _ = run_java('list', '--mods-dir', mods, '--config', config)
            expected, actual = json.loads(py_out), json.loads(java_out)
            check(f'list {key}', expected, actual)
            # apply
            out_py, out_java = root / 'out-py', root / 'out-java'
            py_run, py_err = run_py('apply', '--game', game, '--out', out_py, '--mods-dir', mods, '--config', config)
            java_run, java_err = run_java('apply', '--game', game, '--out', out_java, '--mods-dir', mods, '--config', config)
            py_folder, java_folder = Path(py_run.strip().splitlines()[-1]), Path(java_run.strip().splitlines()[-1])
            if py_folder == game:
                check(f'apply {key} (game itself)', str(game), str(java_folder))
            else:
                check(f'apply {key} tree', tree(py_folder, game, mods), tree(java_folder, game, mods))
                check(f'apply {key} folder under out', [py_folder.parent == out_py.resolve(), py_folder.name.startswith('mod-game-')],
                      [java_folder.parent == out_java.resolve(), java_folder.name.startswith('mod-game-')])
            # Log: the same lines (the per-file conflict lines come in directory order, so compare as sets of lines)
            check(f'apply {key} log', sorted(mods_lines(py_err)), sorted(mods_lines(java_err)))
        # validate, every folder of the fixture and some bad names
        names = sorted(p.name for p in mods.iterdir()) + ['Nope', '..', 'a/b', '']
        for name in names:
            if not name:
                continue
            py_out, _ = run_py('validate', '--mods-dir', mods, '--name', name)
            java_out, _ = run_java('validate', '--mods-dir', mods, '--name', name)
            check(f'validate {name}', json.loads(py_out), json.loads(java_out))
        # missing mods dir and the refusal to clear the game
        nope = root / 'nope'
        py_out, _ = run_py('list', '--mods-dir', nope, '--config', root / 'missing.json')
        java_out, _ = run_java('list', '--mods-dir', nope, '--config', root / 'missing.json')
        check('list missing dir', json.loads(py_out), json.loads(java_out))
        py_run, py_err = run_py('apply', '--game', game, '--out', game, '--mods-dir', mods, '--config', root / 'missing.json')
        java_run, java_err = run_java('apply', '--game', game, '--out', game, '--mods-dir', mods, '--config', root / 'missing.json')
        check('apply refuses to clear the game', [py_run.strip(), mods_lines(py_err)], [java_run.strip(), mods_lines(java_err)])
        real(root)
    print(f'parity: {len(failures)} difference(s)')
    return 1 if failures else 0


sys.exit(main())
