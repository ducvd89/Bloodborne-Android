"""Compact on-disk navigation format ("BNV1") proposed for map pack v2, with encoder + decoder + round-trip check.

The grid makes edges implicit, so only nodes, 4 link nibbles per node, the (few) drop links and the mechanism tables are
stored.  Everything numeric is delta / varint coded and the whole thing is deflated.

    python pack.py [area ...]      encode every area, verify the decode, print sizes   (writes OUT/pack/<area>.bnv)

Layout (all little endian, after zlib inflate of each section):

  file      : 'BNV1' u8 ver | u32 n_sections | per section: u8 id, u32 comp_len, u32 raw_len | section bytes ...
  1 HEADER  : f32 cell | f32 ox | f32 oz | u32 W(cells in x) | u32 H(cells in z) | u32 n_grid | f32 ymin | f32 yres
              (node (ci,cj,y): ci,cj = cell indices, x = ox + (ci+.5)*cell, z = oz + (cj+.5)*cell, y = ymin + q*yres)
  2 KEYS    : n_grid varints: key delta, key = ci*H + cj, nodes sorted by (key, y); delta 0 = floor stacked in the same cell
  3 Y       : n_grid zigzag varints: q[i] - q[i-1]
  4 LINKS   : n_grid u32: 4 bytes for the forward directions (+1,0) (0,+1) (+1,+1) (+1,-1);
              byte = 0 no link, else bit7 = 'ledge' (seam repair, penalised) and bits0-6 = rank+1 of the target node in the
              neighbour cell (nodes of one cell are ordered by height)
  5 DROPS   : varint count | per drop: varint (u - prev_u) | u8 (dx+8)<<4 | (dz+8) | u8 rank | u8 gate+1 | i8 side   (one-way u -> v; arrays stored column-wise)
  6 GATES   : varint count | per gated link: varint (u - prev_u) | u8 dir | u8 rank | u8 gate | i8 side   (blocked / one-way by gate)
  8 MULTI   : varint count | per extra walk link (second+ target in the same neighbour cell): varint (u - prev_u) | u8 dir | u8 nibble
  7 EXTRA   : JSON: virtual nodes [[x,y,z]], extra edges [[u,v,cost,type,gate]] (ladders, lifts, portals), mechanism table
              (id, kind, behaviour, aperture frame, candidate flags ...)
"""
import json
import struct
import sys
import zlib

import numpy as np

from graph import NavGraph, WALK, DROP, LEDGE, LADDER, LIFT, PORTAL
from navconf import OUT, AREAS

DIRS4 = [(1, 0), (0, 1), (1, 1), (1, -1)]
YRES = 0.02


def uvarint(v):
    v = np.asarray(v, np.uint64)
    nb = 1 + (v >= 1 << 7).astype(np.int64) + (v >= 1 << 14) + (v >= 1 << 21) + (v >= 1 << 28)
    pos = np.cumsum(nb) - nb
    out = np.empty(int(nb.sum()), np.uint8)
    for k in range(5):
        m = nb > k
        out[pos[m] + k] = ((v[m] >> np.uint64(7 * k)) & np.uint64(0x7f)).astype(np.uint8) | ((nb[m] > k + 1).astype(np.uint8) << 7)
    return out.tobytes()


def read_uvarint(buf, count, off=0):
    a = np.frombuffer(buf, np.uint8, offset=off)
    ends = np.nonzero((a & 0x80) == 0)[0][:count]
    starts = np.concatenate([[0], ends[:-1] + 1])
    val = np.zeros(count, np.uint64)
    for k in range(5):
        idx = starts + k
        ok = idx <= ends
        b = a[np.minimum(idx, len(a) - 1)].astype(np.uint64) & np.uint64(0x7f)
        val |= np.where(ok, b << np.uint64(7 * k), np.uint64(0))
    return val.astype(np.int64), off + int(ends[-1]) + 1 if count else off


def zz(v):
    v = np.asarray(v, np.int64)
    return ((v << 1) ^ (v >> 63)).astype(np.uint64)


def unzz(u):
    u = np.asarray(u, np.int64)
    return (u >> 1) ^ -(u & 1)


def encode(g):
    gr = g.grid
    n = g.n_grid
    ymin = float(g.y[:n].min())
    q = np.rint((g.y[:n] - ymin) / YRES).astype(np.int64)
    sec = {}
    sec[1] = struct.pack('<fffIIIff', gr.c, gr.ox, gr.oz, gr.W, gr.H, n, ymin, YRES)
    sec[2] = uvarint(np.diff(gr.key, prepend=0))
    sec[3] = uvarint(zz(np.diff(q, prepend=0)))
    # links: rank of the target inside its cell
    et = g.et; eu = g.eu; ev = g.ev
    walk = ((et == WALK) | (et == LEDGE) | (et == 5)) & (ev < n) & (eu < n)
    # direction of each walk edge (stored forward: u -> v with v in +dir cell, or reversed storage -> normalise)
    cu = gr.ci[eu[walk]].astype(np.int64); cv = gr.ci[ev[walk]].astype(np.int64)
    ju = gr.cj[eu[walk]].astype(np.int64); jv = gr.cj[ev[walk]].astype(np.int64)
    u = eu[walk].astype(np.int64); v = ev[walk].astype(np.int64)
    di = cv - cu; dj = jv - ju
    swap = (di < 0) | ((di == 0) & (dj < 0))
    u2 = np.where(swap, v, u); v2 = np.where(swap, u, v); di = np.abs(di); dj = np.where(swap, -dj, dj)
    dirk = np.full(len(u), -1, np.int64)
    for k, (a, b) in enumerate(DIRS4):
        dirk[(di == a) & (dj == b)] = k
    assert (dirk >= 0).all()
    # rank of v2 in its cell
    first = np.searchsorted(gr.key, gr.key[v2], 'left')
    rank = v2 - first
    assert rank.max() < 127
    led = (et[walk] == LEDGE)
    nib = (rank + 1) | (led.astype(np.int64) << 7)
    # one primary link per (node, direction) goes in the bitfield; further targets in the same neighbour cell are listed apart
    order = np.lexsort((rank, dirk, u2))
    sk = u2[order] * 4 + dirk[order]
    prim_sorted = np.ones(len(order), bool); prim_sorted[1:] = sk[1:] != sk[:-1]
    primary = np.zeros(len(order), bool); primary[order] = prim_sorted
    word = np.zeros(n, np.uint32)
    np.add.at(word, u2[primary], (nib[primary].astype(np.uint32) << (8 * dirk[primary]).astype(np.uint32)))
    sec[4] = word.tobytes()
    mi = np.nonzero(~primary)[0]
    mo = np.argsort(u2[mi], kind='stable'); mi = mi[mo]
    sec[8] = (uvarint([len(mi)]) + uvarint(np.diff(u2[mi], prepend=0)) + dirk[mi].astype(np.uint8).tobytes() + nib[mi].astype(np.uint8).tobytes())
    # drops
    dr = (et == DROP) & (eu < n) & (ev < n)
    du = eu[dr].astype(np.int64); dv = ev[dr].astype(np.int64)
    dg = g.eg[dr]; ds = g.eside[dr]
    o = np.argsort(du, kind='stable'); du, dv, dg, ds = du[o], dv[o], dg[o], ds[o]
    ddi = gr.ci[dv].astype(np.int64) - gr.ci[du]; ddj = gr.cj[dv].astype(np.int64) - gr.cj[du]
    drank = dv - np.searchsorted(gr.key, gr.key[dv], 'left')
    assert np.abs(ddi).max(initial=0) <= 7 and np.abs(ddj).max(initial=0) <= 7
    body = uvarint([len(du)]) + uvarint(np.diff(du, prepend=0)) + (((ddi + 8) << 4 | (ddj + 8)).astype(np.uint8)).tobytes() + drank.astype(np.uint8).tobytes() + (dg + 1).astype(np.uint8).tobytes() + ds.astype(np.int8).tobytes()
    sec[5] = body
    # gates: gated grid walk links
    gsel = np.nonzero(walk & (g.eg >= 0))[0]
    gu = np.where(swap, ev[walk], eu[walk])
    # map walk-subset order -> positions
    wpos = np.nonzero(walk)[0]
    gmask = g.eg[wpos] >= 0
    gu_ = u2[gmask]; gdir = dirk[gmask]; grank = rank[gmask]; ggate = g.eg[wpos][gmask]; gside = np.where(swap, -g.eside[wpos], g.eside[wpos])[gmask]
    o = np.argsort(gu_, kind='stable')
    sec[6] = (uvarint([len(gu_)]) + uvarint(np.diff(gu_[o], prepend=0)) + gdir[o].astype(np.uint8).tobytes() + grank[o].astype(np.uint8).tobytes() +
              ggate[o].astype(np.uint8).tobytes() + gside[o].astype(np.int8).tobytes())
    # extra
    nonmove = ~(walk | dr)
    extra = dict(virt=[[round(float(x), 2), round(float(y), 2), round(float(z), 2)] for x, y, z in zip(g.x[n:], g.y[n:], g.z[n:])],
                 edges=[[int(a), int(b), round(float(c), 2), int(t), int(gg), int(dd), int(ss)] for a, b, c, t, gg, dd, ss in
                        zip(eu[nonmove], ev[nonmove], g.ec[nonmove], et[nonmove], g.eg[nonmove], g.edir[nonmove], g.eside[nonmove])],
                 mechs=g.mechs, params={k: v for k, v in g.meta['params'].items()})
    sec[7] = json.dumps(extra, separators=(',', ':'), ensure_ascii=False).encode()
    out = [b'BNV1', struct.pack('<BI', 1, len(sec))]
    comp = {k: zlib.compress(v, 9) for k, v in sec.items()}
    for k in sorted(sec):
        out.append(struct.pack('<BII', k, len(comp[k]), len(sec[k])))
    for k in sorted(sec):
        out.append(comp[k])
    return b''.join(out), {k: (len(sec[k]), len(comp[k])) for k in sec}


def decode(buf, cost_params=None):
    """-> NavGraph (without .grid / .area) rebuilt from the packed bytes."""
    assert buf[:4] == b'BNV1'
    ver, ns = struct.unpack_from('<BI', buf, 4)
    off = 9; heads = []
    for _ in range(ns):
        heads.append(struct.unpack_from('<BII', buf, off)); off += 9
    sec = {}
    for k, cl, rl in heads:
        sec[k] = zlib.decompress(buf[off:off + cl]); off += cl
    c, ox, oz, W, H, n, ymin, yres = struct.unpack('<fffIIIff', sec[1])
    keyd, _ = read_uvarint(sec[2], n); key = np.cumsum(keyd)
    qd, _ = read_uvarint(sec[3], n); q = np.cumsum(unzz(qd))
    ci = key // H; cj = key % H
    x = (ox + (ci + 0.5) * c).astype(np.float32); z = (oz + (cj + 0.5) * c).astype(np.float32); y = (ymin + q * yres).astype(np.float32)
    word = np.frombuffer(sec[4], np.uint32)
    eu, ev, et = [], [], []
    mdirs = []
    for k, (a, b) in enumerate(DIRS4):
        nib = (word >> (8 * k)) & 0xff
        us = np.nonzero(nib)[0]
        r = (nib[us] & 127).astype(np.int64) - 1
        nk = key[us] + a * H + b
        vs = np.searchsorted(key, nk, 'left') + r
        eu.append(us); ev.append(vs); et.append(np.where(nib[us] & 128, LEDGE, WALK))
    mcount, o = read_uvarint(sec[8], 1); mcount = int(mcount[0])
    if mcount:
        md, o2 = read_uvarint(sec[8], mcount, o)
        mu = np.cumsum(md); mdir = np.frombuffer(sec[8], np.uint8, mcount, o2); mnib = np.frombuffer(sec[8], np.uint8, mcount, o2 + mcount)
        for k in range(4):
            sel = mdir == k
            if sel.any():
                us = mu[sel]; nb = mnib[sel]
                eu.append(us); ev.append(np.searchsorted(key, key[us] + DIRS4[k][0] * H + DIRS4[k][1], 'left') + (nb & 127).astype(np.int64) - 1)
                et.append(np.where(nb & 128, LEDGE, WALK))
                mdirs.append((k, len(us)))
    gcount, o = read_uvarint(sec[6], 1)
    gcount = int(gcount[0])
    gd, o2 = read_uvarint(sec[6], gcount, o) if gcount else (np.zeros(0, np.int64), o)
    gu = np.cumsum(gd); gdir = np.frombuffer(sec[6], np.uint8, gcount, o2); grank = np.frombuffer(sec[6], np.uint8, gcount, o2 + gcount); ggate = np.frombuffer(sec[6], np.uint8, gcount, o2 + 2 * gcount)
    gside = np.frombuffer(sec[6], np.int8, gcount, o2 + 3 * gcount)
    dcount, o = read_uvarint(sec[5], 1); dcount = int(dcount[0])
    dd, o2 = read_uvarint(sec[5], dcount, o) if dcount else (np.zeros(0, np.int64), o)
    du = np.cumsum(dd); off2 = np.frombuffer(sec[5], np.uint8, dcount, o2); drank = np.frombuffer(sec[5], np.uint8, dcount, o2 + dcount)
    dgate = np.frombuffer(sec[5], np.uint8, dcount, o2 + 2 * dcount).astype(np.int32) - 1; dside = np.frombuffer(sec[5], np.int8, dcount, o2 + 3 * dcount)
    ddi = (off2 >> 4).astype(np.int64) - 8; ddj = (off2 & 15).astype(np.int64) - 8
    dk = key[du] + ddi * H + ddj
    dv = np.searchsorted(key, dk, 'left') + drank
    EU = np.concatenate(eu + [du]); EV = np.concatenate(ev + [dv]); ET = np.concatenate(et + [np.full(len(du), DROP)])
    EG = np.full(len(EU), -1, np.int32); ES = np.zeros(len(EU), np.int8)
    nw = len(EU) - len(du); EG[nw:] = dgate; ES[nw:] = dside
    ED = np.concatenate([np.zeros(sum(len(a) for a in eu), np.int8), np.ones(len(du), np.int8)])
    EU = EU; 
    if gcount:       # tag gated walk links
        wkey = {}
        base = 0
        dirs_of = [k for k in range(4)] + [k for k, _ in mdirs]
        for k, a, b in zip(dirs_of, eu, ev):
            for i, (uu, vv) in enumerate(zip(a.tolist(), b.tolist())):
                wkey[(uu, k, vv - int(np.searchsorted(key, key[vv], 'left')))] = base + i
            base += len(a)
        for uu, dd_, rr, gg, ss in zip(gu, gdir, grank, ggate, gside):
            i = wkey[(int(uu), int(dd_), int(rr))]
            EG[i] = gg; ES[i] = ss
    extra = json.loads(sec[7].decode())
    vx = np.array([v[0] for v in extra['virt']], np.float32); vy = np.array([v[1] for v in extra['virt']], np.float32); vz = np.array([v[2] for v in extra['virt']], np.float32)
    ex = extra['edges']
    p = extra['params']
    # costs: geometry (+ penalties) recomputed from the quantised nodes
    d3 = np.sqrt((x[EU] - x[EV]) ** 2 + (z[EU] - z[EV]) ** 2 + (y[EU] - y[EV]) ** 2)
    cost = d3 + np.where(ET == LEDGE, p.get('ledge_cost', 8.0), 0) + np.where(ET == DROP, p['drop_cost'], 0)
    if ex:
        EU = np.concatenate([EU, [e[0] for e in ex]]); EV = np.concatenate([EV, [e[1] for e in ex]])
        cost = np.concatenate([cost, [e[2] for e in ex]]); ET = np.concatenate([ET, [e[3] for e in ex]])
        EG = np.concatenate([EG, [e[4] for e in ex]]).astype(np.int32); ED = np.concatenate([ED, [e[5] for e in ex]]).astype(np.int8)
        ES = np.concatenate([ES, np.array([e[6] if len(e) > 6 else 0 for e in ex], np.int8)])
    return NavGraph(np.concatenate([x, vx]), np.concatenate([y, vy]), np.concatenate([z, vz]), n, EU.astype(np.int32), EV.astype(np.int32),
                    cost.astype(np.float32), ET.astype(np.uint8), EG, ED, ES, extra['mechs'], meta=dict(params=p))


def roundtrip(mid):
    import build
    g = build.build_area(mid)
    buf, parts = encode(g)
    h = decode(buf)
    ok = h.n == g.n and len(h.eu) == len(g.eu)
    def arcs(gx):
        a = set(zip(gx.eu.tolist(), gx.ev.tolist(), gx.et.tolist(), gx.eg.tolist(), gx.eside.tolist()))
        return a
    A, B = arcs(g), arcs(h)
    # undirected storage orientation may differ for walk links: normalise
    def norm(s):
        out = set()
        for u, v, t, gg, sd in s:
            if t in (WALK, LEDGE) and u > v:
                u, v, sd = v, u, -sd
            out.add((u, v, t, gg, sd))
        return out
    same = norm(A) == norm(B)
    return g, buf, parts, ok, same, h


if __name__ == '__main__':
    (OUT / 'pack').mkdir(parents=True, exist_ok=True)
    tot = 0; totraw = 0; bad = []
    for a in sys.argv[1:] or AREAS:
        g, buf, parts, ok, same, h = roundtrip(a)
        (OUT / 'pack' / f'{a}.bnv').write_bytes(buf)
        tot += len(buf)
        if not (ok and same):
            bad.append(a)
        print(f'{a} nodes {g.n:7d} edges {len(g.eu):8d} -> {len(buf)/1e6:6.2f} MB ({len(buf)*8/g.n:5.1f} bits/node)  sections(raw/comp) '
              + ' '.join(f'{k}:{v[0]//1024}K/{v[1]//1024}K' for k, v in sorted(parts.items())) + f'  roundtrip nodes/edges ok={ok} identical_edge_set={same}')
    print('total', round(tot / 1e6, 2), 'MB')
    if bad:
        print('ROUND TRIP FAILED:', ' '.join(bad))
        sys.exit(1)
    print('round trip ok for', len(sys.argv[1:] or AREAS), 'areas')
