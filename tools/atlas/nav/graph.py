"""Navigation graph container, scenario masks, CSR export, Dijkstra / A*."""
import heapq
import json
from pathlib import Path

import numpy as np
from scipy.sparse import csr_matrix, coo_matrix

# edge types
WALK, DROP, LADDER, LIFT, PORTAL, GATE_WALK, LEDGE = 0, 1, 2, 3, 4, 5, 6
TYPE_NAMES = {WALK: 'walk', DROP: 'drop', LADDER: 'ladder', LIFT: 'lift', PORTAL: 'portal', GATE_WALK: 'gate', LEDGE: 'ledge'}


class NavGraph:
    """Nodes: x, y, z, plus the edge list.  Edges are stored once (u -> v); `edir` says which directions exist:
    0 both, 1 forward only (u->v), -1 backward only.  `eg` is the index of the mechanism gating the edge (-1: none).
    `eside` (gate edges): +1 if u->v crosses the gate from its operable side to the far side, -1 for the reverse."""

    def __init__(self, x, y, z, n_grid, eu, ev, ec, et, eg, edir, eside, mechs=None, meta=None):
        self.x, self.y, self.z = x, y, z
        self.n_grid = n_grid
        self.n = len(x)
        self.eu, self.ev, self.ec, self.et, self.eg, self.edir, self.eside = eu, ev, ec, et, eg, edir, eside
        self.mechs = mechs or []
        self.meta = meta or {}

    # ------------------------------------------------------------------ scenarios
    def edge_masks(self, state):
        """state: dict mech_index -> one of 'open', 'closed', 'oneway' (closed, operable from the front side).
        Mechanisms missing from `state` are treated as open.  -> (fwd_ok, bwd_ok) bool arrays."""
        fwd = np.ones(len(self.eu), bool); bwd = np.ones(len(self.eu), bool)
        fwd &= self.edir >= 0; bwd &= self.edir <= 0
        gated = np.nonzero(self.eg >= 0)[0]
        if len(gated):
            code = np.zeros(len(self.mechs), np.int8)        # 0 open, 1 closed, 2 oneway
            for mi, s in state.items():
                code[mi] = {'open': 0, 'closed': 1, 'oneway': 2}[s]
            c = code[self.eg[gated]]
            closed = c == 1
            fwd[gated[closed]] = False; bwd[gated[closed]] = False
            ow = c == 2
            gi = gated[ow]
            # one-way: only the crossing from the operable side (eside=+1 means u->v is front->back)
            fwd[gi] &= self.eside[gi] > 0
            bwd[gi] &= self.eside[gi] < 0
        return fwd, bwd

    def csr(self, state=None):
        fwd, bwd = self.edge_masks(state or {})
        u = np.concatenate([self.eu[fwd], self.ev[bwd]])
        v = np.concatenate([self.ev[fwd], self.eu[bwd]])
        c = np.concatenate([self.ec[fwd], self.ec[bwd]])
        # parallel arcs (a walk and a drop link between the same two nodes, ...) keep the cheaper one (a plain coo -> csr sums them)
        order = np.lexsort((c, v, u))
        u, v, c = u[order], v[order], c[order]
        first = np.ones(len(u), bool)
        first[1:] = (u[1:] != u[:-1]) | (v[1:] != v[:-1])
        m = coo_matrix((c[first].astype(np.float64), (u[first], v[first])), shape=(self.n, self.n))
        return m.tocsr()

    # ------------------------------------------------------------------ IO
    def save(self, path):
        path = Path(path)
        np.savez_compressed(path, x=self.x, y=self.y, z=self.z, n_grid=self.n_grid, eu=self.eu, ev=self.ev, ec=self.ec,
                            et=self.et, eg=self.eg, edir=self.edir, eside=self.eside,
                            mechs=np.frombuffer(json.dumps(self.mechs).encode(), np.uint8),
                            meta=np.frombuffer(json.dumps(self.meta).encode(), np.uint8))

    @staticmethod
    def load(path):
        d = np.load(path)
        return NavGraph(d['x'], d['y'], d['z'], int(d['n_grid']), d['eu'], d['ev'], d['ec'], d['et'], d['eg'], d['edir'],
                        d['eside'], json.loads(bytes(d['mechs']).decode()), json.loads(bytes(d['meta']).decode()))


def astar(g, csr, src, dst, heuristic_scale=1.0):
    """Plain Python A* over a CSR matrix (straight-line heuristic).  -> (cost, [nodes]) or (None, [])."""
    indptr, indices, data = csr.indptr, csr.indices, csr.data
    x, y, z = g.x, g.y, g.z
    tx, ty, tz = float(x[dst]), float(y[dst]), float(z[dst])

    def h(n):
        return heuristic_scale * ((float(x[n]) - tx) ** 2 + (float(y[n]) - ty) ** 2 + (float(z[n]) - tz) ** 2) ** 0.5
    best = {src: 0.0}; prev = {}
    heap = [(h(src), 0.0, src)]
    closed = set()
    expanded = 0
    while heap:
        f, gc, n = heapq.heappop(heap)
        if n in closed:
            continue
        closed.add(n); expanded += 1
        if n == dst:
            path = [n]
            while path[-1] in prev:
                path.append(prev[path[-1]])
            return gc, path[::-1], expanded
        for k in range(indptr[n], indptr[n + 1]):
            m = int(indices[k])
            ng = gc + data[k]
            if ng < best.get(m, 1e30):
                best[m] = ng; prev[m] = n
                heapq.heappush(heap, (ng + h(m), ng, m))
    return None, [], expanded
