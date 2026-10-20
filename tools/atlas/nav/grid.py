"""Height-aware grid nodes + walk/drop links from a Hunter's Atlas area JSON (version 2)."""
import json
import numpy as np

from navconf import MAPS, PARAMS


def load_area(mid, maps=None):
    with open((maps or MAPS) / f'{mid}.json', encoding='utf-8') as f:
        return json.load(f)


def raster_layer(L, c, ox, oz, chunk=4_000_000):
    """Cell-centre sampling of one layer's floor triangles -> (ci, cj, y)."""
    t = np.asarray(L['triangles'], np.float64).reshape(-1, 6)
    h = np.asarray(L['heights'], np.float64).reshape(-1, 3)
    n = len(t)
    e32 = (np.empty(0, np.int32), np.empty(0, np.int32), np.empty(0, np.float32))
    if n == 0:
        return e32
    x = t[:, 0::2]; z = t[:, 1::2]
    imin = np.ceil((x.min(1) - ox) / c - 0.5).astype(np.int64)
    imax = np.floor((x.max(1) - ox) / c - 0.5).astype(np.int64)
    jmin = np.ceil((z.min(1) - oz) / c - 0.5).astype(np.int64)
    jmax = np.floor((z.max(1) - oz) / c - 0.5).astype(np.int64)
    nx = np.maximum(imax - imin + 1, 0); nz = np.maximum(jmax - jmin + 1, 0); cnt = nx * nz
    cs = np.cumsum(cnt); start = 0
    oi, oj, oy = [], [], []
    while start < n:
        base = cs[start - 1] if start > 0 else 0
        end = int(np.searchsorted(cs, base + chunk, side='right'))
        end = min(max(end, start + 1), n)
        k = cnt[start:end]; tot = int(k.sum())
        if tot:
            tid = np.repeat(np.arange(start, end), k)
            loc = np.arange(tot) - np.repeat(np.cumsum(k) - k, k)
            nxr = nx[tid]
            ci = imin[tid] + loc % nxr; cj = jmin[tid] + loc // nxr
            px = ox + (ci + 0.5) * c; pz = oz + (cj + 0.5) * c
            x0, x1, x2 = x[tid, 0], x[tid, 1], x[tid, 2]
            z0, z1, z2 = z[tid, 0], z[tid, 1], z[tid, 2]
            det = (z1 - z2) * (x0 - x2) + (x2 - x1) * (z0 - z2)
            ok = np.abs(det) > 1e-12
            d = np.where(ok, det, 1.0)
            l0 = ((z1 - z2) * (px - x2) + (x2 - x1) * (pz - z2)) / d
            l1 = ((z2 - z0) * (px - x2) + (x0 - x2) * (pz - z2)) / d
            l2 = 1 - l0 - l1
            m = ok & (l0 >= -1e-7) & (l1 >= -1e-7) & (l2 >= -1e-7)
            y = l0 * h[tid, 0] + l1 * h[tid, 1] + l2 * h[tid, 2]
            oi.append(ci[m]); oj.append(cj[m]); oy.append(y[m])
        start = end
    if not oi:
        return e32
    return (np.concatenate(oi).astype(np.int32), np.concatenate(oj).astype(np.int32),
            np.concatenate(oy).astype(np.float32))


class Grid:
    """Nodes sorted by (cell key, height); cell (i, j) -> nodes[cell_lo:cell_hi]."""

    def __init__(self, area, params=None):
        p = dict(PARAMS); p.update(params or {})
        self.p = p; c = p['cell']; self.c = c
        # origin from actual triangle extents
        mn = np.array([1e18, 1e18]); mx = np.array([-1e18, -1e18])
        for L in area['layers']:
            if L['triangles']:
                t = np.asarray(L['triangles'], np.float64).reshape(-1, 3, 2)
                mn = np.minimum(mn, t.reshape(-1, 2).min(0)); mx = np.maximum(mx, t.reshape(-1, 2).max(0))
        self.ox = float(np.floor(mn[0]) - 4 * c); self.oz = float(np.floor(mn[1]) - 4 * c)
        self.W = int((mx[0] - self.ox) / c) + 8; self.H = int((mx[1] - self.oz) / c) + 8
        I, J, Y, Ls = [], [], [], []
        for li, L in enumerate(area['layers']):
            i, j, y = raster_layer(L, c, self.ox, self.oz)
            I.append(i); J.append(j); Y.append(y); Ls.append(np.full(len(i), li, np.int16))
        I = np.concatenate(I).astype(np.int64); J = np.concatenate(J).astype(np.int64)
        Y = np.concatenate(Y); Ls = np.concatenate(Ls)
        key = I * self.H + J
        o = np.lexsort((Y, Ls, key))
        key, Y, Ls = key[o], Y[o], Ls[o]
        # drop exact duplicates in one layer (shared triangle edges), then seam duplicates across layers
        keep = np.ones(len(key), bool)
        dup = (key[1:] == key[:-1]) & (np.abs(Y[1:] - Y[:-1]) < p['merge_dy'])
        keep[1:] &= ~dup
        key, Y, Ls = key[keep], Y[keep], Ls[keep]
        o = np.lexsort((Y, key)); key, Y, Ls = key[o], Y[o], Ls[o]
        self.key = key; self.y = Y.astype(np.float32); self.layer = Ls
        self.ci = (key // self.H).astype(np.int32); self.cj = (key % self.H).astype(np.int32)
        self.n = len(key)
        self.x = (self.ox + (self.ci + 0.5) * c).astype(np.float32)
        self.z = (self.oz + (self.cj + 0.5) * c).astype(np.float32)

    def cell_range(self, ci, cj):
        k = np.asarray(ci, np.int64) * self.H + np.asarray(cj, np.int64)
        return np.searchsorted(self.key, k, 'left'), np.searchsorted(self.key, k, 'right')

    def nearest_node(self, x, y, z, max_dist=3.0, dy_scale=1.0):
        """Best node near world position (x, y, z); None if nothing within max_dist (horizontal) -- y matters."""
        c = self.c; ci0 = int((x - self.ox) / c); cj0 = int((z - self.oz) / c)
        r = int(np.ceil(max_dist / c)); best = None; bd = 1e18
        for di in range(-r, r + 1):
            for dj in range(-r, r + 1):
                lo, hi = self.cell_range(ci0 + di, cj0 + dj)
                for k in range(int(lo), int(hi)):
                    d = np.hypot(self.x[k] - x, self.z[k] - z)
                    if d > max_dist:
                        continue
                    d = d + abs(self.y[k] - y) * dy_scale
                    if d < bd:
                        bd, best = d, k
        return best, bd

    def nodes_in_radius(self, x, z, r):
        c = self.c; ci0 = int((x - self.ox) / c); cj0 = int((z - self.oz) / c); rc = int(np.ceil(r / c)) + 1
        out = []
        for di in range(-rc, rc + 1):
            for dj in range(-rc, rc + 1):
                lo, hi = self.cell_range(ci0 + di, cj0 + dj)
                for k in range(int(lo), int(hi)):
                    if np.hypot(self.x[k] - x, self.z[k] - z) <= r:
                        out.append(k)
        return out


DIRS4 = [(1, 0), (0, 1), (1, 1), (1, -1)]      # forward half of the 8-neighbourhood
