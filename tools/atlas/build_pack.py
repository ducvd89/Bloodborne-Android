#!/usr/bin/env python3
"""Hunter's Atlas map pack for the Android app (downloaded from the atlas-maps-N release).

Input: the output directory of Hunter's Atlas's generator (generate_atlas.py --output-dir, run on
the CUSA03173 1.09 files; see tools/atlas/README.md) and the HuntersAtlas checkout for its art and
license. Output: a zip whose single root is atlas/:

  pack.json                 schema, versions, sha256 of every other file
  maps/atlas.json           generator schema, unchanged
  maps/features.json        generator schema, unchanged
  maps/catalog.json         generator schema, only what the map reads (no evidence blobs); a
                            region's triangles are [byteOffset,count] into catalog.bin (count×6 floats)
  maps/catalog.bin          little-endian float32
  maps/<id>.json            version 3: the area's metadata; its geometry is in <id>.bin
  maps/<id>.bin             little-endian float32 arrays, offsets in the layers of <id>.json
  maps/nav/<id>.bnv         pack version 2: the area's route graph (BNV1, tools/atlas/nav/pack.py)
  maps/nav/<id>.gates.json  pack version 2: the gates / mechanisms of that graph (see gate_table)
  art/                      Hunter's Atlas art (PNG) and the DejaVu font
  NOTICE.txt, LICENSE-HuntersAtlas.txt

Layer entry of <id>.json: {height, span, anchor, triangles:[byteOffset,count] (count×6 floats
x0,z0,x1,z1,x2,z2), heights:[byteOffset,count] (count×3 floats y0,y1,y2), walls:[byteOffset,count]
(count×4 floats x0,z0,x1,z1), outline:[byteOffset,floatCount] (x,z pairs), rings:[points,...]}.
The game-extracted font and the generator's build manifest are left out.

Pack version 2 adds maps/nav/ (route finding); everything of version 1 is unchanged and schema stays 1, so an older
app ignores the folder. Usage:
  build_pack.py --generated DIR --hunters-atlas DIR --output ZIP [--nav DIR] [--pack-version N]
--nav is the output of nav/pack.py (<id>.bnv per area); with it the pack version defaults to 2, without to 1.

Gate table (maps/nav/<id>.gates.json, {"version":1,"area":id,"gates":[...]}), gate i = mechanism i of the .bnv (the
index the graph's gated links carry): {i, type, entity, model, name (designer text, Japanese), pos [x,y,z],
default ("open" | "closed" | "oneway": the state when no candidate flag is set), flags [{id, kind, means, conf}],
seg [x0,z0,x1,z1] and y [lo,hi] (door / fog wall aperture, for drawing), operable [[x,z]..] (one-way and lever doors:
the points the door is opened from), stops [[x,y,z]..] (lifts)}. type: door (ordinary), key (key item), oneway,
lever (opened by a lever elsewhere), scripted (opened by story events), special, static (never opens), fog (boss or
multiplayer fog wall, passable), lift, ladder (kick-down ladder). A gate is open when ANY of its candidate flags is set.
"""
import argparse, hashlib, json, shutil, stat, struct, sys, tempfile, zipfile, zlib
from pathlib import Path

EBOOT = 'd65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9'
ART = ('background.png', 'parchment.png', 'icons.png', 'fallback.ttf', 'LICENSE-DejaVu.txt')
MARKER_KINDS = ('ladder', 'npc', 'gate')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def dump(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, separators=(',', ':')), encoding='utf-8')


def floats(values):
    return struct.pack('<%df' % len(values), *values)


def area(source, maps):
    a = json.loads(source.read_text(encoding='utf-8'))
    if a.get('version') != 2:
        sys.exit(f'{source.name}: expected generator version 2, got {a.get("version")}')
    blob = bytearray()

    def put(values, per):
        offset = len(blob)
        flat = [float(v) for item in values for v in item]
        if any(len(item) != per for item in values):
            sys.exit(f'{source.name}: an entry is not {per} values')
        blob.extend(floats(flat))
        return [offset, len(values)]

    layers = []
    for layer in a['layers']:
        tris = layer['triangles']
        heights = layer.get('heights') or []
        if heights and len(heights) != len(tris):
            sys.exit(f'{source.name}: heights do not match the triangles')
        rings = [ring for ring in layer['outlines'] if len(ring) > 1]
        entry = {'height': layer['height'], 'span': layer['span'], 'anchor': layer['anchor'],
                 'triangles': put(tris, 6), 'heights': put(heights, 3), 'walls': put(layer['walls'], 4)}
        offset = len(blob)
        points = [p for ring in rings for p in ring]
        blob.extend(floats([float(v) for p in points for v in p]))
        entry['outline'] = [offset, len(points) * 2]
        entry['rings'] = [len(ring) for ring in rings]
        layers.append(entry)
    markers = [{'kind': m['kind'], 'name': m['name'], 'pos': m['pos'], 'entity_id': m.get('entity_id', 0)}
               for m in a.get('markers', []) if m.get('kind') in MARKER_KINDS and 'name' in m]
    lamps = [{'name': l['name'], 'pos': l['pos']} for l in a.get('lamps', [])]
    dump(maps / source.name, {'version': 3, 'id': a['id'], 'name': a['name'], 'bounds': a['bounds'],
                              'lamps': lamps, 'markers': markers, 'geometry': a['id'] + '.bin',
                              'layers': layers})
    (maps / (a['id'] + '.bin')).write_bytes(bytes(blob))


GATE_TYPE = {'normal': 'door', 'key': 'key', 'oneway': 'oneway', 'lever': 'lever', 'scripted': 'scripted',
             'unknown': 'scripted', 'special': 'special', 'static': 'static'}


def bnv_extra(path):
    """The JSON block (virtual nodes, extra edges, mechanism table) of a BNV1 file; checks the container on the way."""
    buf = Path(path).read_bytes()
    if buf[:4] != b'BNV1':
        sys.exit(f'{path}: not a BNV1 file')
    version, count = struct.unpack_from('<BI', buf, 4)
    if version != 1:
        sys.exit(f'{path}: BNV version {version}')
    heads, off = [], 9
    for _ in range(count):
        heads.append(struct.unpack_from('<BII', buf, off)); off += 9
    sections = {}
    for sid, comp, raw in heads:
        data = zlib.decompress(buf[off:off + comp]); off += comp
        if len(data) != raw:
            sys.exit(f'{path}: section {sid} has {len(data)} bytes, expected {raw}')
        sections[sid] = data
    if off != len(buf) or set(sections) != set(range(1, 9)):
        sys.exit(f'{path}: unexpected container layout')
    return json.loads(sections[7].decode('utf-8'))


def _region(r):
    out = {'kind': r['kind'], 'x': r['x'], 'y': r['y'], 'z': r['z'], 'fx': r['fx'], 'fz': r['fz'], 'y0': r['y0'], 'y1': r['y1'],
           'src': r.get('source', 'button')}
    if r['kind'] == 'box':
        out.update(depth=r['depth'], width=r['width'])
    else:
        out.update(radius=r['radius'], angle=r['angle'])
    return out


def gate_table(mechs):
    out = []
    for i, m in enumerate(mechs):
        kind, behavior = m['kind'], m.get('behavior')
        gtype = 'fog' if kind == 'fog' else GATE_TYPE.get(behavior, 'scripted') if kind == 'door' else kind
        operable = [[round(a, 2), round(b, 2)] for a, b in m.get('operable_points') or []]
        side = m.get('side', 'unknown')
        known = side in ('front', 'back') or bool(operable)
        if gtype == 'fog':
            default = 'open'
        elif gtype in ('door', 'oneway'):
            default = 'oneway' if known else 'open'
        elif gtype == 'lever':
            default = 'oneway' if known else ('open' if side == 'both' else 'closed')
        elif gtype in ('lift', 'ladder'):
            default = 'oneway' if m.get('levers') else 'closed'
        else:
            default = 'closed'     # key, scripted, special, static
        flags = [{'id': f['flag'], 'kind': f['kind'], 'means': f['enabled_means'], 'conf': f['conf']} for f in m.get('flags', [])]
        gate = {'i': i, 'type': gtype, 'entity': m.get('entity', -1), 'model': m.get('model', ''), 'name': m.get('desc', ''),
                'pos': [round(v, 2) for v in m['pos']], 'default': default, 'side': side, 'flags': flags}
        frame = m.get('frame')
        if frame:
            ax, az, bx, bz = frame['segment']
            length = ((bx - ax) ** 2 + (bz - az) ** 2) ** .5
            cut = min(1.0, length * .25) / length if length > 0 else 0   # the aperture reaches 1 m into the wall frame on each side
            gate['seg'] = [round(ax + (bx - ax) * cut, 2), round(az + (bz - az) * cut, 2),
                           round(bx - (bx - ax) * cut, 2), round(bz - (bz - az) * cut, 2)]
            gate['y'] = [round(frame['y_lo'], 2), round(frame['y_hi'], 2)]
            gate['center'] = [round(v, 3) for v in frame['center']]
            gate['normal'] = [round(v, 4) for v in frame['normal']]
        if operable:
            gate['operable'] = operable
        if m.get('regions'):
            gate['regions'] = [_region(r) for r in m['regions']]
        if m.get('key'):
            gate['key'] = {'id': m['key']['id'], 'name': m['key'].get('name', '')}
        if m.get('levers'):
            gate['levers'] = [{'entity': lv['entity'], 'pos': [round(v, 2) for v in lv['pos']], 'stop': lv.get('stop', -1),
                               'regions': [_region(r) for r in lv.get('regions', [])]} for lv in m['levers']]
        if m.get('stops'):
            gate['stops'] = [[round(v, 2) for v in p] for p in m['stops']]
        out.append(gate)
    return out


def nav_area(source, nav):
    """Copies <id>.bnv into maps/nav/ and writes its gate table. -> (gates, flag count)."""
    extra = bnv_extra(source)
    gates = gate_table(extra['mechs'])
    if len(gates) > 254:
        sys.exit(f'{source.name}: {len(gates)} gates do not fit the one-byte gate index')
    area_id = source.stem
    shutil.copyfile(source, nav / source.name)
    dump(nav / (area_id + '.gates.json'), {'version': 1, 'area': area_id, 'gates': gates})
    return gates, len({f['id'] for g in gates for f in g['flags']})


def catalog(source, maps):
    c = json.loads(source.read_text(encoding='utf-8'))
    out, blob = {}, bytearray()
    for area_id, entry in c.items():
        if not isinstance(entry, dict):
            continue
        landmarks = [{k: l[k] for k in ('name', 'pos', 'max_horizontal_distance', 'max_vertical_distance',
                                        'subtitle_prefix') if k in l} for l in entry.get('landmarks', [])]
        regions = []
        for r in entry.get('regions', []):
            region = {k: r[k] for k in ('name', 'shape', 'min_y', 'max_y', 'main_location') if k in r}
            tris = [t for t in r.get('triangles', []) if len(t) == 6]
            region['triangles'] = [len(blob), len(tris)]
            blob.extend(floats([float(v) for t in tris for v in t]))
            regions.append(region)
        out[area_id] = {'landmarks': landmarks, 'regions': regions}
    dump(maps / 'catalog.json', out)
    (maps / 'catalog.bin').write_bytes(bytes(blob))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--generated', type=Path, required=True)
    parser.add_argument('--hunters-atlas', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--nav', type=Path, help='directory with <id>.bnv per area (nav/pack.py output): makes a version 2 pack')
    parser.add_argument('--pack-version', type=int)
    args = parser.parse_args()
    pack_version = args.pack_version or (2 if args.nav else 1)
    if pack_version >= 2 and not args.nav:
        sys.exit('pack version 2 needs --nav')
    generated = args.generated / 'maps'
    manifest = json.loads((generated / 'generated-assets.json').read_text(encoding='utf-8'))
    if manifest.get('game_eboot_sha256') != EBOOT:
        sys.exit('the maps were not generated from the supported 1.09 eboot')
    features = json.loads((generated / 'features.json').read_text(encoding='utf-8'))
    if features.get('schema') != 1 or features.get('game_eboot_sha256') != EBOOT:
        sys.exit('features.json does not match the supported eboot')
    with tempfile.TemporaryDirectory() as temp:
        pack = Path(temp)
        root = pack / 'atlas'
        maps, art = root / 'maps', root / 'art'
        maps.mkdir(parents=True)
        art.mkdir()
        shutil.copyfile(generated / 'atlas.json', maps / 'atlas.json')
        shutil.copyfile(generated / 'features.json', maps / 'features.json')
        catalog(generated / 'catalog.json', maps)
        ids = [m['id'] for m in json.loads((generated / 'atlas.json').read_text(encoding='utf-8'))['maps']]
        for area_id in ids:
            area(generated / (area_id + '.json'), maps)
        nav_info = None
        if args.nav:
            nav = maps / 'nav'
            nav.mkdir()
            listed, flag_ids = [], set()
            for area_id in ids:
                source = args.nav / (area_id + '.bnv')
                if not source.is_file():
                    sys.exit(f'no route graph for {area_id}: {source}')
                gates, _ = nav_area(source, nav)
                listed.append(area_id)
                flag_ids |= {f['id'] for g in gates for f in g['flags']}
            nav_info = {'version': 1, 'areas': listed, 'gate_flags': len(flag_ids)}
        for name in ART:
            shutil.copyfile(args.hunters_atlas / 'source/art' / name, art / name)
        shutil.copyfile(args.hunters_atlas / 'LICENSE', root / 'LICENSE-HuntersAtlas.txt')
        (root / 'NOTICE.txt').write_text(
            "Map data generated by Hunter's Atlas (https://github.com/MikeTat1988/HuntersAtlas, GPL-2.0) "
            "from the CUSA03173 1.09 game files. Art by Hunter's Atlas; DejaVu font license in art/.\n",
            encoding='utf-8')
        files = {p.relative_to(root).as_posix(): sha(p) for p in sorted(root.rglob('*')) if p.is_file()}
        info = {'schema': 1, 'pack_version': pack_version, 'generator': manifest.get('generator_version'),
                'eboot_sha256': EBOOT, 'areas': ids}
        if nav_info:
            info['nav'] = nav_info
        info['files'] = files
        dump(root / 'pack.json', info)
        names = sorted(p.relative_to(pack).as_posix() for p in root.rglob('*') if p.is_file())
        args.output.unlink(missing_ok=True)
        with zipfile.ZipFile(args.output, 'w') as z:
            for name in names:
                info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = (stat.S_IFREG | 0o644) << 16
                z.writestr(info, (pack / name).read_bytes(), compresslevel=9)
        total = sum((pack / n).stat().st_size for n in names)
    print(f'{args.output}: {args.output.stat().st_size} bytes ({total} unpacked), sha256 {sha(args.output)}')


if __name__ == '__main__':
    main()
