"""Coverage test: lamp<->lamp, lamp->pickup, lamp->NPC reachability per area and scenario."""
import json
import sys
import time

import numpy as np
from scipy.sparse.csgraph import dijkstra, connected_components
from scipy.spatial import cKDTree

import build
import grid as gridmod
import scenario as scen
from graph import NavGraph
from navconf import MAPS, OUT, AREAS

SCENARIOS = ['all_open', 'mech', 'fresh', 'all_closed']


def load_targets(mid, area=None):
    feats = json.loads((MAPS / 'features.json').read_text())['areas'][mid]
    area = area or gridmod.load_area(mid)
    lamps = [dict(name=l['name'], pos=l['pos'], arrival=l.get('arrival_pos'), kind='lamp') for l in feats['lamps']]
    pickups = [dict(name='/'.join(c['name'] for c in p['contents']) or p['id'], pos=p['pos'], kind='pickup', id=p['id']) for p in feats['pickups']]
    npcs = [dict(name=m['name'], pos=m['pos'], kind='npc', id=m.get('entity_id')) for m in area.get('markers', []) if m['kind'] == 'npc']
    return lamps, pickups, npcs


def snap_all(g, tree, items, radius=3.0):
    for it in items:
        p = it['pos']
        d, i = tree.query(p, distance_upper_bound=radius)
        if not np.isfinite(d):
            d, i = tree.query(p, distance_upper_bound=6.0)
        it['node'] = int(i) if np.isfinite(d) else None
        it['snap'] = float(d) if np.isfinite(d) else None


def path_length(g, pred, src, dst):
    n = dst; L = 0.0
    while n != src and n >= 0:
        p = pred[n]
        if p < 0:
            return None
        L += float(np.sqrt((g.x[n] - g.x[p]) ** 2 + (g.y[n] - g.y[p]) ** 2 + (g.z[n] - g.z[p]) ** 2))
        n = p
    return L


def evaluate(g, lamps, pickups, npcs, scenario, flags=None):
    state = scen.scenario_state(g, scenario, flags)
    M = g.csr(state)
    srcs = [l['node'] for l in lamps if l['node'] is not None]
    res = dict(scenario=scenario, lamp_pairs=0, lamp_pairs_ok=0, lamp_pairs_sym=0, pickups=0, pickups_ok=0, npcs=0, npcs_ok=0,
               unsnapped=sum(1 for it in lamps + pickups + npcs if it['node'] is None), ratios=[], failures=[], lamp_fail=[])
    if not srcs:
        return res
    D, P = dijkstra(M, directed=True, indices=srcs, return_predecessors=True)
    row = {}
    k = 0
    for l in lamps:
        if l['node'] is not None:
            row[l['name']] = k; k += 1
    reach_from = {}
    for l in lamps:
        if l['node'] is None:
            continue
        r = row[l['name']]
        reach_from[l['name']] = r
    for a in lamps:
        for b in lamps:
            if a is b or a['node'] is None or b['node'] is None:
                continue
            res['lamp_pairs'] += 1
            ra, rb = row[a['name']], row[b['name']]
            ok = np.isfinite(D[ra, b['node']])
            res['lamp_pairs_ok'] += int(ok)
            res['lamp_pairs_sym'] += int(ok and np.isfinite(D[rb, a['node']]))
            if ok:
                L = path_length(g, P[ra], a['node'], b['node'])
                st = float(np.sqrt(sum((a['pos'][i] - b['pos'][i]) ** 2 for i in range(3))))
                if L and st > 5:
                    res['ratios'].append(L / st)
            else:
                res['lamp_fail'].append((a['name'], b['name'], a['pos'], b['pos']))
    for kind, items, key in (('pickups', pickups, 'pickups'), ('npcs', npcs, 'npcs')):
        for it in items:
            if it['node'] is None:
                res[key] += len(lamps); continue
            for l in lamps:
                if l['node'] is None:
                    continue
                res[key] += 1
                ok = np.isfinite(D[row[l['name']], it['node']])
                res[key + '_ok'] += int(ok)
                if not ok:
                    res['failures'].append((l['name'], it['name'], it['kind'], it['pos']))
    return res


def run_area(mid, scenarios=SCENARIOS, params=None, flags=None, keep_graph=False):
    t = time.time()
    g = build.build_area(mid, params=params)
    area = g.area
    lamps, pickups, npcs = load_targets(mid, area)
    tree = cKDTree(np.c_[g.x[:g.n_grid], g.y[:g.n_grid], g.z[:g.n_grid]])
    snap_all(g, tree, lamps + pickups + npcs)
    out = dict(area=mid, name=area['name'], nodes=g.n, grid_nodes=g.n_grid, edges=int(len(g.eu)), seconds_build=round(time.time() - t, 1),
               lamps=len(lamps), pickups=len(pickups), npcs=len(npcs), scenarios={}, stats=g.meta['stats'])
    for s in scenarios:
        r = evaluate(g, lamps, pickups, npcs, s, flags)
        rat = r.pop('ratios')
        r['ratio_median'] = float(np.median(rat)) if rat else None
        r['ratio_p90'] = float(np.percentile(rat, 90)) if rat else None
        r['ratio_max'] = float(max(rat)) if rat else None
        out['scenarios'][s] = r
    out['seconds'] = round(time.time() - t, 1)
    return (out, g, (lamps, pickups, npcs)) if keep_graph else out


def _job(a):
    try:
        return run_area(a)
    except Exception as e:
        import traceback
        return dict(area=a, error=traceback.format_exc())


if __name__ == '__main__':
    from multiprocessing import Pool
    areas = [a for a in sys.argv[1:] if a.startswith('m2') or a.startswith('m3')] or AREAS
    workers = min(len(areas), 12)
    with Pool(workers) as p:
        results = p.map(_job, areas, chunksize=1)
    (OUT).mkdir(parents=True, exist_ok=True)
    (OUT / 'coverage.json').write_text(json.dumps(results, indent=1))
    for r in results:
        if 'error' in r:
            print(r['area'], 'ERROR', r['error'][-500:]); continue
        print(r['area'], r['name'], 'nodes', r['nodes'], 'edges', r['edges'], 'build', r['seconds_build'], 's')
        for s, v in r['scenarios'].items():
            lp = v['lamp_pairs']; pk = v['pickups']; nn = v['npcs']
            print(f"   {s:10s} lamps {v['lamp_pairs_ok']}/{lp}  pickups {v['pickups_ok']}/{pk}  npcs {v['npcs_ok']}/{nn}  unsnapped {v['unsnapped']}  ratio med {v['ratio_median']} max {v['ratio_max']}")


def gap_between(g, M, src, dst, limit=400.0):
    """Closest approach between the nodes reachable from `src` and the nodes that can reach `dst`.
    -> dict(dist, dy, plan, a=(x,y,z), b=(x,y,z)) describing where a link would have to be added."""
    N = g.n_grid
    ds = dijkstra(M, directed=True, indices=src, limit=1e9)
    dt = dijkstra(M.T.tocsr(), directed=True, indices=dst, limit=1e9)
    S = np.nonzero(np.isfinite(ds[:N]))[0]; T = np.nonzero(np.isfinite(dt[:N]))[0]
    if len(S) == 0 or len(T) == 0:
        return None
    PS = np.c_[g.x[S], g.y[S], g.z[S]]; PT = np.c_[g.x[T], g.y[T], g.z[T]]
    tree = cKDTree(PT)
    d, j = tree.query(PS)
    k = int(d.argmin())
    a = PS[k]; b = PT[j[k]]
    return dict(dist=float(d[k]), dy=float(b[1] - a[1]), plan=float(np.hypot(a[0] - b[0], a[2] - b[2])),
                a=[round(float(v), 1) for v in a], b=[round(float(v), 1) for v in b], reach_src=int(len(S)), reach_dst=int(len(T)))
