"""Coverage of the app's live rules on the decoded .bnv files: lamp pairs / pickups connected with every flag set, with no flag known
(default gate states: ordinary doors crossed from their operable side), and with doors treated as the prototype did (side ignored).

    python livecov.py PACK_DIR      -> console table
"""
import json
import sys
from pathlib import Path

import numpy as np
from scipy.sparse.csgraph import dijkstra
from scipy.spatial import cKDTree

import pack
import scenario as scen
from navconf import MAPS, AREAS


def main():
    pack_dir = Path(sys.argv[1])
    feats = json.loads((MAPS / 'features.json').read_text())['areas']
    tot = dict(open=[0, 0], default=[0, 0], nosides=[0, 0])
    for a in AREAS:
        g = pack.decode((pack_dir / f'{a}.bnv').read_bytes())
        tree = cKDTree(np.c_[g.x[:g.n_grid], g.y[:g.n_grid], g.z[:g.n_grid]])
        def snap(p):
            d, i = tree.query(p, distance_upper_bound=3.0)
            if not np.isfinite(d):
                d, i = tree.query(p, distance_upper_bound=6.0)
            return int(i) if np.isfinite(d) else None
        lamps = [snap(l['pos']) for l in feats[a]['lamps']]; lamps = [n for n in lamps if n is not None]
        picks = [snap(p['pos']) for p in feats[a]['pickups']]; picks = [n for n in picks if n is not None]
        flags_open = {f['flag']: True for m in g.mechs for f in m.get('flags', [])}
        row = []
        for name, flags, strip in (('open', flags_open, False), ('default', {}, False), ('nosides', {}, True)):
            mechs = g.mechs
            if strip:   # doors without a known side: the prototype's rule (ordinary doors always passable)
                saved = [dict(m) for m in g.mechs]
                for m in g.mechs:
                    if m.get('behavior') in ('normal', 'oneway'):
                        m['operable_points'] = []; m['side'] = 'unknown'
            state = scen.scenario_state(g, 'live', flags)
            if strip:
                g.mechs[:] = saved
            M = g.csr(state)
            pairs = ok = pk = pkok = 0
            if lamps:
                D = dijkstra(M, directed=True, indices=lamps)
                for i, s in enumerate(lamps):
                    for t in lamps:
                        if s != t:
                            pairs += 1; ok += np.isfinite(D[i, t])
                    for t in picks:
                        pk += 1; pkok += np.isfinite(D[i, t])
            row.append(f'{name}: pairs {ok}/{pairs} pickups {pkok}/{pk}')
            tot[name][0] += ok + pkok; tot[name][1] += pairs + pk
        print(a, ' | '.join(row))
    print('TOTAL', {k: f'{v[0]}/{v[1]} ({100 * v[0] / max(1, v[1]):.1f}%)' for k, v in tot.items()})


if __name__ == '__main__':
    main()
