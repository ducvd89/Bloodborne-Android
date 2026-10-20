"""Reference route costs for the Java route finder's test (tests/android-nav): lamp pairs of one area through the decoded .bnv.

    python javaref.py PACK_DIR AREA OUT.json [max_pairs]

PACK_DIR holds <area>.bnv (nav/pack.py output).  Writes, for two gate scenarios (`default`: no flag known, `open`: every
candidate flag set), the gate states and, per ordered lamp pair, the snapped nodes, the exact (Dijkstra) route cost and the
Python A* cost / expansions / time.  The nodes are those of the decoded file, the numbering the Java loader uses.
"""
import json
import sys
import time
from pathlib import Path

import numpy as np
from scipy.sparse.csgraph import dijkstra
from scipy.spatial import cKDTree

import pack
import scenario as scen
from graph import astar
from navconf import MAPS


def main():
    pack_dir, area, out = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
    limit = int(sys.argv[4]) if len(sys.argv) > 4 else 12
    g = pack.decode((pack_dir / f'{area}.bnv').read_bytes())
    feats = json.loads((MAPS / 'features.json').read_text())['areas'][area]
    lamps = [(l['name'], l['pos']) for l in feats['lamps']]
    tree = cKDTree(np.c_[g.x[:g.n_grid], g.y[:g.n_grid], g.z[:g.n_grid]])

    def snap(p):
        d, i = tree.query(p, distance_upper_bound=3.0)
        if not np.isfinite(d):
            d, i = tree.query(p, distance_upper_bound=6.0)
        return int(i) if np.isfinite(d) else None

    flags_open = {f['flag']: True for m in g.mechs for f in m.get('flags', [])}
    result = dict(area=area, nodes=int(g.n), scenarios={})
    for name, flags in (('default', {}), ('open', flags_open), ('keyretry', {})):
        state = scen.scenario_state(g, 'live', flags)
        if name == 'keyretry':       # the app's second search when no route exists: key doors not open count as crossable from their side
            for i, m in enumerate(g.mechs):
                if m.get('behavior') == 'key' and state[i] == 'closed':
                    state[i] = 'oneway' if m.get('side') in ('front', 'back') or m.get('operable_points') else 'open'
        M = g.csr(state)
        code = {'open': 0, 'closed': 1, 'oneway': 2}
        sc = dict(states=[code[state[i]] for i in range(len(g.mechs))], pairs=[])
        pairs = [(a, b) for a in lamps for b in lamps if a is not b]
        for (an, ap), (bn, bp) in pairs[:limit]:
            s, t = snap(ap), snap(bp)
            if s is None or t is None:
                continue
            exact = dijkstra(M, directed=True, indices=s)[t]
            t0 = time.perf_counter(); cost, path, exp = astar(g, M, s, t, 1.0); ms = (time.perf_counter() - t0) * 1000
            sc['pairs'].append(dict(a=an, b=bn, ap=ap, bp=bp, src=s, dst=t, exact=None if not np.isfinite(exact) else float(exact),
                                    astar=cost if cost is None else float(cost), expansions=exp, py_ms=round(ms, 1)))
        result['scenarios'][name] = sc
    out.write_text(json.dumps(result))
    for name, sc in result['scenarios'].items():
        ok = sum(1 for p in sc['pairs'] if p['exact'] is not None)
        print(name, len(sc['pairs']), 'pairs', ok, 'connected')


if __name__ == '__main__':
    main()
