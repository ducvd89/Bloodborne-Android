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
  art/                      Hunter's Atlas art (PNG) and the DejaVu font
  NOTICE.txt, LICENSE-HuntersAtlas.txt

Layer entry of <id>.json: {height, span, anchor, triangles:[byteOffset,count] (count×6 floats
x0,z0,x1,z1,x2,z2), heights:[byteOffset,count] (count×3 floats y0,y1,y2), walls:[byteOffset,count]
(count×4 floats x0,z0,x1,z1), outline:[byteOffset,floatCount] (x,z pairs), rings:[points,...]}.
The game-extracted font and the generator's build manifest are left out.

Usage: build_pack.py --generated DIR --hunters-atlas DIR --output ZIP [--pack-version N]
"""
import argparse, hashlib, json, shutil, stat, struct, sys, tempfile, zipfile
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
    parser.add_argument('--pack-version', type=int, default=1)
    args = parser.parse_args()
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
        for name in ART:
            shutil.copyfile(args.hunters_atlas / 'source/art' / name, art / name)
        shutil.copyfile(args.hunters_atlas / 'LICENSE', root / 'LICENSE-HuntersAtlas.txt')
        (root / 'NOTICE.txt').write_text(
            "Map data generated by Hunter's Atlas (https://github.com/MikeTat1988/HuntersAtlas, GPL-2.0) "
            "from the CUSA03173 1.09 game files. Art by Hunter's Atlas; DejaVu font license in art/.\n",
            encoding='utf-8')
        files = {p.relative_to(root).as_posix(): sha(p) for p in sorted(root.rglob('*')) if p.is_file()}
        dump(root / 'pack.json', {'schema': 1, 'pack_version': args.pack_version,
                                  'generator': manifest.get('generator_version'), 'eboot_sha256': EBOOT,
                                  'areas': ids, 'files': files})
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
