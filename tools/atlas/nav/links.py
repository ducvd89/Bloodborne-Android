"""Walk links (8-neighbour, step/slope limited, wall-blocked) and one-way drop links."""
import numpy as np

from grid import DIRS4


def seg_cross(ax, az, bx, bz, cx, cz, dx, dz):
    """Proper crossing of segment AB with segment CD (vectorised)."""
    def orient(px, pz, qx, qz, rx, rz):
        return (qx - px) * (rz - pz) - (qz - pz) * (rx - px)
    o1 = orient(ax, az, bx, bz, cx, cz); o2 = orient(ax, az, bx, bz, dx, dz)
    o3 = orient(cx, cz, dx, dz, ax, az); o4 = orient(cx, cz, dx, dz, bx, bz)
    return (o1 * o2 < 0) & (o3 * o4 < 0)


class WallIndex:
    """Wall segments of all layers, registered per grid cell (with 1-cell dilation)."""

    def __init__(self, area, grid, exclude_frames=()):
        """exclude_frames: door/fog frames (dict center, normal, width) whose neighbourhood walls are dropped
        (the generator's collision walls include the plugs of passable fog gates)."""
        segs, lay = [], []
        for li, L in enumerate(area['layers']):
            if L['walls']:
                w = np.asarray(L['walls'], np.float64).reshape(-1, 4)
                for fr in exclude_frames:
                    cx, cz = fr['center']; nx, nz = fr['normal']; tx, tz = -nz, nx; hw = fr['width'] / 2 + 0.4
                    def inside(px, pz):
                        return (np.abs((px - cx) * tx + (pz - cz) * tz) <= hw) & (np.abs((px - cx) * nx + (pz - cz) * nz) <= 1.0)
                    w = w[~(inside(w[:, 0], w[:, 1]) & inside(w[:, 2], w[:, 3]))]
                if not len(w):
                    continue
                segs.append(w); lay.append(np.full(len(w), li, np.int32))
        self.n = 0
        if not segs:
            self.seg = np.empty((0, 4)); self.layer = np.empty(0, np.int32)
            self.pk = np.empty(0, np.int64); self.pw = np.empty(0, np.int64)
            return
        self.seg = np.concatenate(segs); self.layer = np.concatenate(lay); self.n = len(self.seg)
        c = grid.c; H = grid.H
        ln = np.hypot(self.seg[:, 2] - self.seg[:, 0], self.seg[:, 3] - self.seg[:, 1])
        ns = np.maximum(np.ceil(ln / (c / 2)).astype(np.int64), 1) + 1      # samples per segment (incl. both ends)
        wid = np.repeat(np.arange(self.n), ns)
        loc = np.arange(len(wid)) - np.repeat(np.cumsum(ns) - ns, ns)
        t = loc / np.repeat(ns - 1, ns)
        px = self.seg[wid, 0] + t * (self.seg[wid, 2] - self.seg[wid, 0])
        pz = self.seg[wid, 1] + t * (self.seg[wid, 3] - self.seg[wid, 1])
        ci = np.floor((px - grid.ox) / c).astype(np.int64); cj = np.floor((pz - grid.oz) / c).astype(np.int64)
        ks, ws = [], []
        for di in (-1, 0, 1):
            for dj in (-1, 0, 1):
                ks.append((ci + di) * H + (cj + dj)); ws.append(wid)
        k = np.concatenate(ks); w = np.concatenate(ws)
        u = np.unique(k * (self.n + 1) + w)
        self.pk = u // (self.n + 1); self.pw = u % (self.n + 1)       # sorted by cell key

    def blocked(self, grid, u, v):
        """Bool mask: link u->v crosses a wall of the layers of either endpoint."""
        res = np.zeros(len(u), bool)
        if self.n == 0 or len(u) == 0:
            return res
        ka = grid.key[u]
        lo = np.searchsorted(self.pk, ka, 'left'); hi = np.searchsorted(self.pk, ka, 'right')
        cnt = hi - lo
        idx = np.nonzero(cnt)[0]
        if len(idx) == 0:
            return res
        csum = np.cumsum(cnt[idx]); start = 0; lim = 6_000_000
        while start < len(idx):
            base = csum[start - 1] if start else 0
            end = min(max(int(np.searchsorted(csum, base + lim, 'right')), start + 1), len(idx))
            ii = idx[start:end]; k = cnt[ii]
            lid = np.repeat(ii, k)
            wl = np.arange(int(k.sum())) - np.repeat(np.cumsum(k) - k, k) + np.repeat(lo[ii], k)
            wid = self.pw[wl]
            la = grid.layer[u[lid]]; lb = grid.layer[v[lid]]
            okl = (self.layer[wid] == la) | (self.layer[wid] == lb)
            lid = lid[okl]; wid = wid[okl]
            S = self.seg[wid]
            hit = seg_cross(grid.x[u[lid]].astype(np.float64), grid.z[u[lid]].astype(np.float64),
                            grid.x[v[lid]].astype(np.float64), grid.z[v[lid]].astype(np.float64),
                            S[:, 0], S[:, 1], S[:, 2], S[:, 3])
            res[np.unique(lid[hit])] = True
            start = end
        return res


def _maxm(grid):
    return int(np.bincount(np.unique(grid.key, return_inverse=True)[1]).max())


def walk_links(grid, walls, p):
    """Undirected walk links in the forward half of the 8-neighbourhood.

    Returns (u, v, cost, is_ledge).  Normal links: |dy| <= step.  'Ledge' links (step < |dy| <= ledge_max) repair seams of
    the generator's re-tessellated floors (stair treads sampled into ramps); they carry a cost penalty."""
    us, vs = [], []
    maxm = _maxm(grid)
    lmax = max(p.get('ledge_max', 0.0), p['step'])
    for di, dj in DIRS4:
        dist = grid.c * np.hypot(di, dj)
        nk = grid.key + di * grid.H + dj
        lo = np.searchsorted(grid.key, nk, 'left'); hi = np.searchsorted(grid.key, nk, 'right')
        for k in range(maxm):
            idx = lo + k
            valid = idx < hi
            u = np.nonzero(valid)[0]; v = idx[valid]
            dy = np.abs(grid.y[v] - grid.y[u])
            m = (dy <= lmax) & (dy / dist <= max(p['slope_max'], lmax / dist))
            us.append(u[m]); vs.append(v[m])
    u = np.concatenate(us); v = np.concatenate(vs)
    bl = walls.blocked(grid, u, v)
    u, v = u[~bl], v[~bl]
    dy = np.abs(grid.y[u] - grid.y[v])
    d = np.sqrt((grid.x[u] - grid.x[v]) ** 2 + (grid.z[u] - grid.z[v]) ** 2 + dy ** 2)
    led = dy > p['step']
    d = d + led * p.get('ledge_cost', 8.0)
    return u.astype(np.int32), v.astype(np.int32), d.astype(np.float32), led


DIR8 = [(1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1)]


def neighbour_mask(grid, lmax):
    """uint8 bitmask per node: bit k set when a floor node within +-lmax height exists in compass cell DIR8[k]."""
    maxm = _maxm(grid)
    mask = np.zeros(grid.n, np.uint8)
    for k, (di, dj) in enumerate(DIR8):
        nk = grid.key + di * grid.H + dj
        lo = np.searchsorted(grid.key, nk, 'left'); hi = np.searchsorted(grid.key, nk, 'right')
        has = np.zeros(grid.n, bool)
        for m in range(maxm):
            idx = lo + m; valid = idx < hi
            u = np.nonzero(valid)[0]; v = idx[valid]
            ok = np.abs(grid.y[v] - grid.y[u]) <= lmax
            has[u[ok]] = True
        mask |= (has.astype(np.uint8) << k)
    return mask


def _dir_bit(di, dj):
    """Compass bits that a drop with plan offset (di, dj) leaves through (dominant axis, both on ties)."""
    if di == 0 and dj == 0:
        return 0xff                       # stacked: any open side
    bits = 0
    ai, aj = abs(di), abs(dj)
    for k, (x, z) in enumerate(DIR8):
        if (x == np.sign(di) and ai >= aj and x != 0) or (z == np.sign(dj) and aj >= ai and z != 0):
            bits |= 1 << k
    return bits


def drop_links(grid, walls, p):
    """One-way falls: ledge node -> node below within drop_reach horizontally, drop_min..drop_max below.

    A node may only fall off a side where its floor ends (no floor node within `ledge_max` height in that compass
    cell); stacked floors are therefore never connected through each other.  Per source node and compass sector (8)
    only the nearest landing is kept, to bound the edge count.
    """
    c = grid.c; reach = int(round(p['drop_reach'] / c))
    nm = neighbour_mask(grid, max(p.get('ledge_max', 0.0), p['step']))
    us, vs = [], []
    maxm = _maxm(grid)
    for di in range(-reach, reach + 1):
        for dj in range(-reach, reach + 1):
            if np.hypot(di, dj) * c > p["drop_reach"] + 1e-6:
                continue
            bits = _dir_bit(di, dj)
            nk = grid.key + di * grid.H + dj
            lo = np.searchsorted(grid.key, nk, 'left'); hi = np.searchsorted(grid.key, nk, 'right')
            for k in range(maxm):
                idx = lo + k; valid = idx < hi
                u = np.nonzero(valid)[0]; v = idx[valid]
                dy = grid.y[u] - grid.y[v]
                m = (dy >= p['drop_min']) & (dy <= p['drop_max']) & ((~nm[u] & bits) != 0)
                us.append(u[m]); vs.append(v[m])
    e = (np.empty(0, np.int32), np.empty(0, np.int32), np.empty(0, np.float32))
    if not us:
        return e
    u = np.concatenate(us); v = np.concatenate(vs)
    if len(u) == 0:
        return e
    bl = walls.blocked(grid, u, v)
    u, v = u[~bl], v[~bl]
    dx = grid.x[v] - grid.x[u]; dz = grid.z[v] - grid.z[u]
    sec = ((np.arctan2(dz, dx) + np.pi) / (np.pi / 4)).astype(np.int64) % 8
    hd = np.hypot(dx, dz)
    o = np.lexsort((hd, sec, u)); u, v, sec = u[o], v[o], sec[o]
    first = np.ones(len(u), bool); first[1:] = (u[1:] != u[:-1]) | (sec[1:] != sec[:-1])
    u, v = u[first], v[first]
    d = np.sqrt((grid.x[u] - grid.x[v]) ** 2 + (grid.z[u] - grid.z[v]) ** 2 + (grid.y[u] - grid.y[v]) ** 2) + p['drop_cost']
    return u.astype(np.int32), v.astype(np.int32), d.astype(np.float32)
