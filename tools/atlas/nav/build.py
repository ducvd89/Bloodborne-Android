"""Build the navigation graph of one area: grid nodes, walk / drop links, ladders, lifts, gates."""
import json
import math
import time

import numpy as np

import grid as gridmod
import links as linkmod
import mechanisms as mechmod
import objinfo
from graph import NavGraph, WALK, DROP, LADDER, LIFT, PORTAL, GATE_WALK, LEDGE
from links import seg_cross
from navconf import PARAMS, OUT


def _yaw_dir(rot_y_deg):
    """World (x, z) of a model's local +Z axis after a Y rotation (generator convention)."""
    t = math.radians(rot_y_deg)
    return math.sin(t), math.cos(t)


def pick_nodes(grid, cx, cz, cy, radius, dy_lo, dy_hi, k=4):
    """Nodes within `radius` of (cx, cz) whose y is in [cy+dy_lo, cy+dy_hi], nearest first (plan distance + 0.3*dy)."""
    out = []
    for n in grid.nodes_in_radius(cx, cz, radius):
        dy = float(grid.y[n]) - cy
        if dy_lo <= dy <= dy_hi:
            out.append((math.hypot(float(grid.x[n]) - cx, float(grid.z[n]) - cz) + 0.3 * abs(dy), n))
    out.sort()
    return [n for _, n in out[:k]]


def ladder_ends(grid, m, bounds=None):
    """Bottom / top landing node lists of a ladder marker (None if a landing is not found).

    Height comes from the object's collision (or FLVER) box.  Models without any geometry (o256000...) fall back to
    "the nearest floor 2-30 m above, within 3 m in plan, in front of the ladder"."""
    b = bounds if bounds is not None else objinfo.best_bounds(m['model'])
    x, y, z = m['pos']
    fx, fz = _yaw_dir(m['rotation'][1])
    bot = pick_nodes(grid, x + fx * 0.7, z + fz * 0.7, y, 1.6, -1.2, 1.2)
    if not bot:
        bot = pick_nodes(grid, x + fx * 0.7, z + fz * 0.7, y, 3.0, -3.0, 1.5)
    if b is not None and abs(float(b[1][1])) < 1e6:
        lo, hi = b
        H = float(hi[1]); zmax = float(hi[2])
        tz = max(0.55 * zmax, 0.5)
        top = pick_nodes(grid, x + fx * tz, z + fz * tz, y + H, 2.4, -2.0, 1.6)
        if not top:
            top = pick_nodes(grid, x + fx * tz, z + fz * tz, y + H, 4.0, -3.5, 3.0)
        return bot, top, H
    best = []
    for n in grid.nodes_in_radius(x + fx * 1.5, z + fz * 1.5, 3.0):
        dy = float(grid.y[n]) - y
        if 2.0 <= dy <= 30.0:
            best.append((math.hypot(float(grid.x[n]) - x - fx * 1.5, float(grid.z[n]) - z - fz * 1.5) + 0.15 * dy, n))
    best.sort()
    top = [n for _, n in best[:4]]
    H = float(grid.y[top[0]]) - y if top else None
    return bot, top, H


def add_virtual(extra, grid, nodes, x, y, z):
    """Create a virtual node connected (cost ~0) to `nodes`; returns its index."""
    vid = grid.n + len(extra['x'])
    extra['x'].append(x); extra['y'].append(y); extra['z'].append(z)
    for n in nodes:
        extra['edges'].append((vid, int(n), 0.05, PORTAL, -1, 0, 0))
    return vid


def _stops_for_lift(inv, lift):
    """Stop positions (x, y, z) of a lift: region pairs named ...上/下 near the lift, else the lever objects around it."""
    lx, _, lz = lift['pos']
    pts = [tuple(r['pos']) for r in inv.get('lift_stops', []) if math.hypot(r['pos'][0] - lx, r['pos'][2] - lz) < 14]
    if len(pts) < 2:
        pts = [tuple(a['pos']) for a in inv['mechanisms'] if a['kind'] == 'lever' and math.hypot(a['pos'][0] - lx, a['pos'][2] - lz) < 16]
    # merge stops closer than 1.5 m vertically
    pts.sort(key=lambda q: q[1]); out = []
    for q in pts:
        if not out or abs(q[1] - out[-1][1]) > 1.5:
            out.append(q)
    return out


def build_area(mid, mech=None, params=None, verbose=False):
    """-> NavGraph.  `mech` (optional) is the area's mechanism inventory (see mechanisms.py) used for gates / lifts."""
    t0 = time.time()
    p = dict(PARAMS); p.update(params or {})
    area = gridmod.load_area(mid)
    g = gridmod.Grid(area, p)
    inv = mech if mech is not None else mechmod.run(mid)
    fog_frames = [m['frame'] for m in inv['mechanisms'] if m['kind'] == 'fog' and 'frame' in m]
    walls = linkmod.WallIndex(area, g, fog_frames) if not p.get('no_walls') else linkmod.WallIndex({'layers': []}, g)
    wu, wv, wc, wled = linkmod.walk_links(g, walls, p)
    du, dv, dc = linkmod.drop_links(g, walls, p)
    if verbose:
        print(f'{mid}: nodes {g.n} walk {len(wu)} drop {len(du)} ({time.time() - t0:.1f}s)')
    edges_u = [wu, du]; edges_v = [wv, dv]; edges_c = [wc, dc]
    edges_t = [np.where(wled, LEDGE, WALK).astype(np.uint8), np.full(len(du), DROP, np.uint8)]
    edges_d = [np.zeros(len(wu), np.int8), np.full(len(du), 1, np.int8)]          # drops are forward-only
    extra = dict(x=[], y=[], z=[], edges=[])
    stats = dict(ladders=[], gates=[], lifts=[])

    mech_by_entity = {m['entity']: m for m in inv['mechanisms'] if m['entity'] > 0}
    mechs_out = []                                   # index = mechanism id used by NavGraph.eg

    def register(m, **extra_fields):
        rec = {k: m[k] for k in ('id', 'kind', 'entity', 'model', 'desc', 'pos') if k in m}
        rec['behavior'] = m.get('behavior'); rec['flags'] = m.get('flags', [])
        if m.get('operable_points'):                 # [x, z] of the points the door can be opened from (the side of `eside` +1)
            rec['operable_points'] = [[round(float(a), 2), round(float(b), 2)] for a, b in m['operable_points']]
        for k in ('side', 'side_from', 'regions', 'key', 'levers'):      # interact.py: which side the action button works from
            if m.get(k):
                rec[k] = m[k]
        rec.update(extra_fields)
        mechs_out.append(rec)
        return len(mechs_out) - 1

    # ---- gates: doors / grates (links crossing the aperture segment are tagged with the mechanism index)
    nw = len(wu)
    all_u = np.concatenate([wu, du]); all_v = np.concatenate([wv, dv])
    all_g = np.full(len(all_u), -1, np.int32); all_s = np.zeros(len(all_u), np.int8)
    for m in inv['mechanisms']:
        if m['kind'] not in ('door', 'fog') or 'frame' not in m:
            continue
        fr = m['frame']; ax, az, bx, bz = fr['segment']
        py = m['pos'][1]
        # candidate links: both ends near the door in plan and within the door's height range
        cx, cz = fr['center']; rad = fr['width'] / 2 + 1.5
        near = np.nonzero(((g.x[all_u] - cx) ** 2 + (g.z[all_u] - cz) ** 2 < rad * rad) &
                          (g.y[all_u] >= fr['y_lo'] - 0.5) & (g.y[all_u] <= fr['y_hi']) & (all_g < 0))[0]
        if len(near) == 0:
            stats['gates'].append(dict(entity=m['entity'], desc=m['desc'], links=0)); continue
        u = all_u[near]; v = all_v[near]
        hit = seg_cross(g.x[u].astype(float), g.z[u].astype(float), g.x[v].astype(float), g.z[v].astype(float), ax, az, bx, bz)
        idx = near[hit]
        mi = register(m, frame=fr)
        all_g[idx] = mi
        nx, nz = fr['normal']
        side_u = ((g.x[all_u[idx]] - cx) * nx + (g.z[all_u[idx]] - cz) * nz)
        # operable side: sign of the reference point relative to the door plane (+1 front = same side as +normal)
        pts = m.get('operable_points') or []
        if pts:
            px, pz = pts[0]
            op = np.sign((px - cx) * nx + (pz - cz) * nz) or 1.0
            all_s[idx] = np.where(np.sign(side_u) == op, 1, -1)
        stats['gates'].append(dict(entity=m['entity'], desc=m['desc'], behavior=m.get('behavior'), links=int(len(idx))))
    edges_g = [all_g[:nw], all_g[nw:]]; edges_s = [all_s[:nw], all_s[nw:]]

    # ---- ladders (RegisterLadder markers; vertical extent = object collision height)
    for mk in area.get('markers', []):
        if mk['kind'] != 'ladder':
            continue
        bot, top, H = ladder_ends(g, mk)
        rec = dict(kind='ladder', entity=mk['entity_id'], model=mk['model'], pos=mk['pos'], H=H, bottom=len(bot or []), top=len(top or []))
        stats['ladders'].append(rec)
        if not bot or not top:
            continue
        im = mech_by_entity.get(mk['entity_id'])
        gate = -1
        if im is not None and im.get('flags'):
            gate = register(im)                      # conditional ladder (kick-down etc.): state decided by scenario
        vb = add_virtual(extra, g, bot, float(g.x[bot[0]]), float(g.y[bot[0]]), float(g.z[bot[0]]))
        vt = add_virtual(extra, g, top, float(g.x[top[0]]), float(g.y[top[0]]), float(g.z[top[0]]))
        L = float(g.y[top[0]]) - float(g.y[bot[0]])
        side = 0                                     # +1: the lever that kicks the ladder down is at the bottom end (u -> v starts at it)
        if gate >= 0 and im.get('levers'):
            lx, ly, lz = im['levers'][0]['pos']
            db = math.dist((lx, ly, lz), (float(g.x[bot[0]]), float(g.y[bot[0]]), float(g.z[bot[0]])))
            dt = math.dist((lx, ly, lz), (float(g.x[top[0]]), float(g.y[top[0]]), float(g.z[top[0]])))
            side = 1 if db < dt else -1
        extra['edges'].append((vb, vt, max(abs(L), 1.0) * p['ladder_cost'], LADDER, gate, 0, side))

    # ---- lifts (stop pairs from the elevator-switch regions / lever names; see mechanisms._build_lifts)
    for L in inv.get('lifts', []):
        stops = [tuple(s['pos']) for s in L['stops']]
        stops.sort(key=lambda q: q[1])
        landings = []; lstop = []
        for si, (sx, sy, sz) in enumerate(stops):
            nodes = pick_nodes(g, sx, sz, sy, 5.0, -0.9, 0.9, k=6)
            if not nodes:
                nodes = pick_nodes(g, sx, sz, sy, 8.0, -1.5, 1.5, k=6)
            if nodes:
                landings.append((nodes, sy)); lstop.append(si)
        stats['lifts'].append(dict(id=L['id'], desc=L.get('desc', ''), stops=len(stops), landed=len(landings),
                                   pos=list(stops[0]) if stops else None, gate_flags=L['gate_flags']))
        if len(landings) < 2:
            continue
        rec = dict(id=f"{mid}:{L['id']}", kind='lift', entity=L.get('entity', -1), model=L.get('model'), desc=L.get('desc', ''), pos=list(stops[0]),
                   behavior=None, flags=[dict(flag=f, kind='unlock', enabled_means='open', evidence='checked by every event of this lift', conf='medium') for f in L['gate_flags']],
                   stops=[list(q) for q in stops])
        if L.get('levers'):                          # levers near the stops (their own action regions): a lift with no flag set can be
            rec['levers'] = [dict(entity=lv['entity'], pos=lv['pos'], stop=lv['stop'], regions=lv['regions']) for lv in L['levers']]   # used from a stop that has one
        mechs_out.append(rec); mi = len(mechs_out) - 1
        vids = [add_virtual(extra, g, nodes, float(g.x[nodes[0]]), float(g.y[nodes[0]]), float(g.z[nodes[0]])) for nodes, _ in landings]
        lever_stops = {lv['stop'] for lv in L.get('levers', [])}
        for i in range(len(vids)):
            for j in range(i + 1, len(vids)):
                cost = 12.0 + abs(landings[i][1] - landings[j][1]) * 0.6
                # one directed edge per direction; side +1 = the ride starts at a stop that has a lever (usable while the lift is not unlocked)
                extra['edges'].append((vids[i], vids[j], cost, LIFT, mi, 1, 1 if lstop[i] in lever_stops else 0))
                extra['edges'].append((vids[j], vids[i], cost, LIFT, mi, 1, 1 if lstop[j] in lever_stops else 0))

    # ---- assemble
    ex = extra['edges']
    if ex:
        a = np.array([(e[0], e[1]) for e in ex], np.int32)
        edges_u.append(a[:, 0]); edges_v.append(a[:, 1])
        edges_c.append(np.array([e[2] for e in ex], np.float32))
        edges_t.append(np.array([e[3] for e in ex], np.uint8))
        edges_g.append(np.array([e[4] for e in ex], np.int32))
        edges_d.append(np.array([e[5] for e in ex], np.int8))
        edges_s.append(np.array([e[6] for e in ex], np.int8))
    x = np.concatenate([g.x, np.array(extra['x'], np.float32)])
    y = np.concatenate([g.y, np.array(extra['y'], np.float32)])
    z = np.concatenate([g.z, np.array(extra['z'], np.float32)])
    ng = NavGraph(x, y, z, g.n, np.concatenate(edges_u).astype(np.int32), np.concatenate(edges_v).astype(np.int32),
                  np.concatenate(edges_c).astype(np.float32), np.concatenate(edges_t), np.concatenate(edges_g),
                  np.concatenate(edges_d), np.concatenate(edges_s), mechs_out,
                  meta=dict(area=mid, params=p, ox=g.ox, oz=g.oz, cell=g.c, W=g.W, H=g.H, stats=stats,
                            build_seconds=round(time.time() - t0, 1)))
    ng.grid = g
    ng.area = area
    return ng
