"""Size / speed measurements: graph sizes, Python A* per query, estimate for Java on a phone.

    python bench.py [area ...]   ->  OUT/bench.json
"""
import json
import sys
import time

import numpy as np
from scipy.sparse.csgraph import connected_components, dijkstra
from scipy.spatial import cKDTree

import coverage
import scenario as scen
from graph import astar
from navconf import OUT, AREAS


def bench_area(mid):
    out, g, (lamps, pickups, npcs) = coverage.run_area(mid, scenarios=['mech'], keep_graph=True)
    state = scen.scenario_state(g, 'mech')
    M = g.csr(state)
    p = OUT / 'graphs'; p.mkdir(parents=True, exist_ok=True)
    g.save(p / f'{mid}.npz')
    res = dict(area=mid, name=g.area['name'], nodes=int(g.n), edges_undirected=int(len(g.eu)), directed_arcs=int(M.nnz),
               npz_bytes=(p / f'{mid}.npz').stat().st_size, csr_bytes=int(M.indptr.nbytes + M.indices.nbytes + M.data.nbytes), build_s=out['seconds_build'])
    # region graph (strongly connected components, mech scenario)
    nc, lab = connected_components(M, directed=True, connection='strong')
    sz = np.bincount(lab)
    big = sz >= 50
    coo = M.tocoo(); cu = lab[coo.row]; cv = lab[coo.col]
    cross = cu != cv
    pairs = set(zip(cu[cross].tolist(), cv[cross].tolist()))
    res.update(regions=int(nc), regions_ge50=int(big.sum()), nodes_in_regions_ge50=int(sz[big].sum()), region_links=len(pairs))
    # queries: lamp-to-lamp and lamp-to-far-pickup pairs that are connected
    srcs = [l for l in lamps if l['node'] is not None]
    pairs_q = []
    for a in srcs:
        d = dijkstra(M, directed=True, indices=a['node'])
        tg = [b for b in srcs + [x for x in pickups + npcs if x['node'] is not None] if b is not a and np.isfinite(d[b['node']])]
        tg.sort(key=lambda b: -d[b['node']])
        for b in tg[:2]:
            pairs_q.append((a, b))
    pairs_q = pairs_q[:8]
    times, exps, costs = [], [], []
    for a, b in pairs_q:
        t = time.perf_counter(); c, path, e = astar(g, M, a['node'], b['node'], 1.0); dt = time.perf_counter() - t
        if c is not None:
            times.append(dt); exps.append(e); costs.append(c)
    if times:
        res.update(astar_py_ms_median=round(1000 * float(np.median(times)), 1), astar_py_ms_max=round(1000 * max(times), 1),
                   expansions_median=int(np.median(exps)), expansions_max=int(max(exps)), queries=len(times),
                   py_us_per_expansion=round(1e6 * float(np.sum(times)) / float(np.sum(exps)), 2))
    t = time.perf_counter()
    for a, b in pairs_q[:3]:
        dijkstra(M, directed=True, indices=a['node'])
    res['scipy_dijkstra_all_ms'] = round(1000 * (time.perf_counter() - t) / max(1, min(3, len(pairs_q))), 1)
    return res


def _job(a):
    try:
        return bench_area(a)
    except Exception:
        import traceback
        return dict(area=a, error=traceback.format_exc())


if __name__ == '__main__':
    from multiprocessing import Pool
    areas = sys.argv[1:] or AREAS
    with Pool(min(len(areas), 8)) as pool:
        rs = pool.map(_job, areas, chunksize=1)
    (OUT / 'bench.json').write_text(json.dumps(rs, indent=1))
    for r in rs:
        print(r if 'error' in r else {k: r[k] for k in r if k != 'name'})
