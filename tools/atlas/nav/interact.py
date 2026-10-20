"""Interaction regions of doors, levers, lifts and ladders: where the player may press the action button.

An ObjAct event (MSB) attaches an ObjActParam row to an object; the row names an ActionButtonParam row (`button`, e.g. 7000
"open a door"), whose region is a wedge of a cylinder (regionType 0: radius, angle, height) or a box (regionType 1: depth,
width, height) placed at a dummy polygon of the object's FLVER model (dummyPoly1; the row's own `dummy_id` when that is -1)
and opening along that dummy's forward vector; baseHeightOffset moves its floor relative to the dummy.  Doors have one
dummy per side (100 / 101, 120 / 121, ...), so an ObjAct that names one of them makes the door operable from that side only.

Convention checked against the game's own data (see report in the README, "Interaction regions"): a dummy's forward vector
points to the side the player stands on (a dummy at the standing spot facing the object has forward pointing at the object,
which is the opposite; the action-button dummies are the former kind).

Everything here is read from the game's files (FLVER dummy / bone tables, ActionButtonParam, ObjActParam).
"""
import json
import math
import struct
import sys
from functools import lru_cache

import numpy as np
from soulstruct.containers import Binder

import objinfo
from navconf import GAME_DATA, GENSRC


@lru_cache(maxsize=None)
def flver_tables(model):
    """-> (bones, dummies) of the model's FLVER: bones [dict(t, rot, parent, scale)], dummies [dict(ref, parent, attach, pos, fwd, up)] (local)."""
    path = GAME_DATA / 'obj' / f'{model}.objbnd.dcx'
    if not path.exists():
        return [], []
    for e in Binder.from_path(path).entries:
        if not e.name.lower().endswith('.flver'):
            continue
        d = e.get_uncompressed_data()
        if d[:5] != b'FLVER':
            return [], []
        nd, nm, nb = struct.unpack_from('<iii', d, 0x14)
        dummies = []
        for i in range(nd):
            o = 0x80 + 64 * i
            ref, parent = struct.unpack_from('<hh', d, o + 28)
            attach = struct.unpack_from('<h', d, o + 44)[0]
            dummies.append(dict(ref=ref, parent=parent, attach=attach, pos=np.array(struct.unpack_from('<3f', d, o)),
                                fwd=np.array(struct.unpack_from('<3f', d, o + 16)), up=np.array(struct.unpack_from('<3f', d, o + 32))))
        bones = []
        bo = 0x80 + 64 * nd + 32 * nm
        for i in range(nb):
            o = bo + 128 * i
            bones.append(dict(t=np.array(struct.unpack_from('<3f', d, o)), rot=np.array(struct.unpack_from('<3f', d, o + 16)),
                              parent=struct.unpack_from('<h', d, o + 28)[0], scale=np.array(struct.unpack_from('<3f', d, o + 32))))
        return bones, dummies
    return [], []


def _bone_local(b):
    """Column-vector 4x4 of SoulsFormats' row-vector S * Rx * Rz * Ry * T."""
    rx, ry, rz = b['rot']
    cx, sx, cy, sy, cz, sz = math.cos(rx), math.sin(rx), math.cos(ry), math.sin(ry), math.cos(rz), math.sin(rz)
    RX = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]]); RY = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]); RZ = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    M = np.eye(4)
    M[:3, :3] = RY @ RZ @ RX @ np.diag(b['scale'])
    M[:3, 3] = b['t']
    return M


def bone_world(bones, idx):
    if idx < 0 or idx >= len(bones):
        return np.eye(4)
    M = _bone_local(bones[idx])
    return bone_world(bones, bones[idx]['parent']) @ M if bones[idx]['parent'] >= 0 else M


def dummy_model_space(model, ref):
    """-> [(pos, fwd)] of every dummy with that reference id in the model's space (bone-attached dummies through their parent bone)."""
    bones, dummies = flver_tables(model)
    out = []
    for d in dummies:
        if d['ref'] != ref:
            continue
        M = bone_world(bones, d['parent']) if d['parent'] >= 0 else np.eye(4)
        p = (M @ np.append(d['pos'], 1.0))[:3]
        f = M[:3, :3] @ d['fwd']
        n = np.linalg.norm(f)
        out.append((p, f / n if n > 1e-9 else f))
    return out


@lru_cache(maxsize=None)
def action_buttons():
    path = GAME_DATA / 'param/gameparam/gameparam.parambnd.dcx'
    entry = next(e for e in Binder.from_path(path).entries if e.name.replace('\\', '/').endswith('/ActionButtonParam.param'))
    b = entry.get_uncompressed_data()
    count = struct.unpack_from('<H', b, 10)[0]
    rows = {}
    for i in range(count):
        rid, _, offset, nameoff = struct.unpack_from('<iiqq', b, 64 + 24 * i)
        end = nameoff
        while b[end:end + 2] != b'\0\0':
            end += 2
        (rtype, d1, d2, radius, angle, depth, width, height, base, check, allow) = struct.unpack_from('<B3xiififfffB3xi', b, offset)
        rows[rid] = dict(name=b[nameoff:end].decode('utf-16le'), type=rtype, dummy1=d1, dummy2=d2, radius=radius, angle=angle,
                         depth=depth, width=width, height=height, base=base, angle_check=check, allow_angle=allow)
    return rows


@lru_cache(maxsize=None)
def goods_names():
    """Goods (item) names, English: {goods id: name}, from msg/engus/item.msgbnd.dcx (FMG 10)."""
    if GENSRC and GENSRC not in sys.path:
        sys.path.insert(0, GENSRC)
    try:
        from extract_atlas_features import binder, fmg
        texts = binder(GAME_DATA / 'msg/engus/item.msgbnd.dcx')
        for _, (eid, data) in texts.items():
            if eid == 10 and data[:1] == b'\0':
                return fmg(data)
    except Exception as e:                      # names are cosmetic
        print('goods names unavailable:', e, file=sys.stderr)
    return {}


def _yaw_dirs(rot_deg):
    return objinfo.local_to_world(np.zeros(3), rot_deg, [1, 0, 0]), objinfo.local_to_world(np.zeros(3), rot_deg, [0, 0, 1])


DEFAULT_BUTTON = dict(name='(default)', type=0, dummy1=-1, dummy2=-1, radius=1.2, angle=60, depth=0.0, width=0.0, height=2.0, base=-1.0,
                      angle_check=0, allow_angle=90)


def regions_for(part, acts, rows=None):
    """Interaction regions of one object.  part: dict(model, pos, rot, scale); acts: ObjAct rows [dict(entity, row)].
    -> [dict(kind 'wedge'|'box', x, y, z (the dummy), fx, fz (forward, unit, plan view), radius | depth + width, angle, y0, y1,
             objact, button, dummy, source)]
    source 'button': the ActionButtonParam region at its dummyPoly1; 'snap': the ObjAct's own dummy (objDummyId, where the
    character is placed; its forward points at the object, so the region is mirrored) when the row names no ActionButtonParam
    row (a few ids such as 3500000 are not in this game's table) or its dummy is not in the model."""
    ab = action_buttons()
    out = []
    sc = np.array(part.get('scale') or [1, 1, 1], float)
    for a in acts:
        r = a.get('row')
        if not r:
            continue
        button = ab.get(r.get('button'))
        did, source, found = None, 'button', []
        if button:
            for cand in [i for i in (button['dummy1'], button['dummy2']) if i >= 0][:1]:
                found = dummy_model_space(part['model'], cand); did = cand
        if not found and r.get('dummy_id', -1) >= 0:      # no usable button region: the ObjAct's own dummy
            found = dummy_model_space(part['model'], r['dummy_id']); did = r['dummy_id']; source = 'snap'
            button = button or DEFAULT_BUTTON
        if not found:
            continue
        for p, f in found:
            wp = objinfo.local_to_world(part['pos'], part['rot'], p * sc)
            wf = objinfo.local_to_world(np.zeros(3), part['rot'], f)
            n = math.hypot(wf[0], wf[2])
            if n < 1e-6:
                continue
            sign = -1.0 if source == 'snap' else 1.0
            fx, fz = float(sign * wf[0] / n), float(sign * wf[2] / n)
            y0 = float(wp[1] + button['base']); y1 = y0 + float(button['height'])
            reg = dict(x=round(float(wp[0]), 3), y=round(float(wp[1]), 3), z=round(float(wp[2]), 3), fx=round(fx, 4), fz=round(fz, 4),
                       y0=round(y0, 2), y1=round(y1, 2), objact=a['entity'], button=r['button'], dummy=did, source=source)
            if button['type'] == 1:
                reg.update(kind='box', depth=round(button['depth'], 2), width=round(button['width'], 2))
            else:
                reg.update(kind='wedge', radius=round(button['radius'], 2), angle=button['angle'])
            out.append(reg)
    return out


def region_centre(reg):
    """The middle of a region in plan view (where a player stands to use it)."""
    d = reg.get('radius', reg.get('depth', 1.0)) * 0.5
    return reg['x'] + reg['fx'] * d, reg['z'] + reg['fz'] * d


# ---------------------------------------------------------------------------------------------------- per mechanism

def _side_of(frame, point):
    """+1 / -1: which side of the door plane (its +normal = 'front') a plan-view point is on; 0 on the plane."""
    cx, cz = frame['center']; nx, nz = frame['normal']
    v = (point[0] - cx) * nx + (point[1] - cz) * nz
    return 1 if v > 0.05 else -1 if v < -0.05 else 0


def _sides_name(signs):
    s = set(signs) - {0}
    return 'both' if s == {1, -1} else 'front' if s == {1} else 'back' if s == {-1} else 'unknown'


def annotate(mechs, goods=None):
    """Adds to every mechanism of an area: `regions` (interaction regions of its own ObjActs), `side` ('front' | 'back' | 'both' |
    'unknown': which side of the door plane the action button works from; front = the +normal side of `frame`), `key`
    (key doors: goods id and English name), and for doors whose side is a single one `operable_points` = [the middle of the
    region].  The prototype's event-derived `operable_points` (SpEffect 4150 grant regions, lever positions) are kept as
    `event_operable_points`; lever doors keep theirs (the door is opened by a lever, whose side is what matters)."""
    goods = goods if goods is not None else goods_names()
    for m in mechs:
        m['regions'] = regions_for(m, m.get('objacts', []))
        m['side'] = 'unknown'; m['side_from'] = None
        if m['kind'] != 'door':
            continue
        fr = m.get('frame')
        ev = m.get('operable_points') or []
        m['event_operable_points'] = ev
        beh = m.get('behavior')
        if beh == 'lever':
            if ev and fr:
                m['side'] = _sides_name([_side_of(fr, p) for p in ev]); m['side_from'] = 'lever'
                if m['side'] == 'both':
                    m['operable_points'] = []
                elif m['side'] != 'unknown':
                    want = 1 if m['side'] == 'front' else -1
                    m['operable_points'] = [p for p in ev if _side_of(fr, p) == want][:1]
            continue
        if m['regions'] and fr:
            cents = [region_centre(r) for r in m['regions']]
            m['side'] = _sides_name([_side_of(fr, c) for c in cents]); m['side_from'] = 'action button'
            m['operable_points'] = [[round(cents[0][0], 3), round(cents[0][1], 3)]] if m['side'] in ('front', 'back') else []
            if m['side'] == 'front' or m['side'] == 'back':
                want = 1 if m['side'] == 'front' else -1
                m['operable_points'] = [[round(c[0], 3), round(c[1], 3)] for c in cents if _side_of(fr, c) == want][:1]
        else:
            m['operable_points'] = []
        if beh == 'key' and m.get('sp'):
            m['key'] = dict(id=m['sp']['id'], name=goods.get(m['sp']['id'], ''))
    # levers that operate a door or a kick-down ladder from elsewhere (their own regions)
    by_entity = {m['entity']: m for m in mechs if m['kind'] == 'lever' and m['entity'] > 0}
    for m in mechs:
        lv = []
        for a in m.get('activators', []):
            lever = by_entity.get(a['entity'])
            if lever:
                lv.append(dict(entity=lever['entity'], pos=lever['pos'], regions=lever['regions']))
        if lv and m['kind'] in ('door', 'ladder'):
            m['levers'] = lv


def annotate_lifts(out):
    """Levers near each stop of every lift: L['levers'] = [dict(entity, pos, stop, regions)] (stops are sorted by height)."""
    levers = [m for m in out['mechanisms'] if m['kind'] == 'lever']
    for L in out['lifts']:
        stops = sorted(L['stops'], key=lambda s: s['pos'][1])
        L['levers'] = []
        for lv in levers:
            best = None
            for i, s in enumerate(stops):
                d = math.hypot(lv['pos'][0] - s['pos'][0], lv['pos'][2] - s['pos'][2])
                if d < 9 and abs(lv['pos'][1] - s['pos'][1]) < 4 and (best is None or d < best[0]):
                    best = (d, i)
            if best:
                L['levers'].append(dict(entity=lv['entity'], pos=lv['pos'], stop=best[1], regions=lv['regions']))
