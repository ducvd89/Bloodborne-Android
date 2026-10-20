"""Markdown report: coverage tables, worst failures (with gap diagnosis), mechanism inventory summary, sizes.

    python report.py       ->  OUT/report.md, OUT/coverage.json, OUT/inventory.csv
"""
import csv
import json
import math
from collections import Counter, defaultdict
from multiprocessing import Pool

import numpy as np

import coverage
import mechanisms
import scenario as scen
from navconf import AREAS, OUT, PARAMS

SC = ['all_open', 'mech', 'fresh', 'all_closed']


def near_mechs(inv, pt, r=15, k=3):
    rows = []
    for m in inv['mechanisms']:
        d = math.hypot(m['pos'][0] - pt[0], m['pos'][2] - pt[2])
        if d < r and m['kind'] in ('door', 'ladder', 'lift', 'fog', 'bridge', 'lever'):
            rows.append((d, abs(m['pos'][1] - pt[1]), m))
    for L in inv.get('lifts', []):
        for s in L['stops']:
            d = math.hypot(s['pos'][0] - pt[0], s['pos'][2] - pt[2])
            if d < r:
                rows.append((d, abs(s['pos'][1] - pt[1]), dict(kind='lift-stop', model='', desc=L.get('desc', ''))))
    rows.sort(key=lambda t: t[0] + 0.2 * t[1])
    return [f"{m['kind']} {m['model']} {m['desc'][:18]} ({d:.0f} m, dy {dy:.0f})" for d, dy, m in rows[:k]]


def area_job(mid):
    out, g, (lamps, pickups, npcs) = coverage.run_area(mid, scenarios=SC, keep_graph=True)
    inv = mechanisms.run(mid)
    res = dict(out)
    gaps = []
    for scn in ('all_open', 'mech'):
        M = g.csr(scen.scenario_state(g, scn))
        seen = set()
        for a, b, pa, pb in out['scenarios'][scn]['lamp_fail']:
            la = next(l for l in lamps if l['name'] == a); lb = next(l for l in lamps if l['name'] == b)
            r = coverage.gap_between(g, M, la['node'], lb['node'])
            if not r:
                continue
            key = (tuple(r['a']), tuple(r['b']))
            if key in seen:
                continue
            seen.add(key)
            gaps.append(dict(scenario=scn, a=a, b=b, gap=r, near_a=near_mechs(inv, r['a']), near_b=near_mechs(inv, r['b'])))
    res['gaps'] = gaps
    # failing pickups / npcs in all_open: cluster by position, report the largest clusters
    st = scen.scenario_state(g, 'all_open'); M = g.csr(st)
    from scipy.sparse.csgraph import dijkstra
    srcs = [l for l in lamps if l['node'] is not None]
    bad = []
    if srcs:
        reach = np.zeros(g.n, bool)
        for l in srcs:
            reach |= np.isfinite(dijkstra(M, directed=True, indices=l['node']))
        for it in pickups + npcs:
            if it['node'] is not None and not reach[it['node']]:
                bad.append(dict(name=it['name'], kind=it['kind'], pos=[round(v, 1) for v in it['pos']]))
    res['unreached_all_open'] = bad
    res['inventory'] = inv_summary(inv)
    return res


def inv_summary(inv):
    c = Counter(); beh = Counter(); conf = Counter()
    for m in inv['mechanisms']:
        c[m['kind']] += 1
        if m['kind'] == 'door':
            beh[m.get('behavior')] += 1
            fl = [f for f in m.get('flags', [])]
            conf['door_high_flag' if any(f['conf'] == 'high' for f in fl) else 'door_objact_flag_only' if fl else 'door_no_flag'] += 1
    return dict(kinds=dict(c), door_behaviors=dict(beh), flag_conf=dict(conf), lifts=len(inv.get('lifts', [])),
                lifts_with_gate_flag=sum(1 for L in inv.get('lifts', []) if L['gate_flags']),
                ladders_conditional=sum(1 for m in inv['mechanisms'] if m['kind'] == 'ladder' and m.get('flags')),
                oneway_doors=sum(1 for m in inv['mechanisms'] if m.get('behavior') == 'oneway'),
                oneway_with_side=sum(1 for m in inv['mechanisms'] if m.get('behavior') == 'oneway' and m.get('operable_points')))


def pct(a, b):
    return '-' if not b else f'{100 * a / b:.0f}%'


def main():
    with Pool(8) as p:
        res = p.map(area_job, AREAS, chunksize=1)
    (OUT / 'coverage.json').write_text(json.dumps(res, indent=1, default=str))
    L = []
    L.append('# Hunter\'s Atlas route navigation prototype: coverage report\n')
    L.append('Parameters: ' + ', '.join(f'{k}={v}' for k, v in PARAMS.items()) + '\n')
    L.append('Lamp pairs = every ordered pair of lamps of the area (ordered, directed reachability); pickups / NPCs = lamp -> target for every lamp.\n')
    for s in SC:
        L.append(f'\n## Scenario `{s}`\n')
        L.append('| area | lamp pairs | lamp->pickup | lamp->NPC | route/straight median (p90) |')
        L.append('|---|---|---|---|---|')
        tl = [0, 0, 0, 0, 0, 0]
        for r in res:
            v = r['scenarios'][s]
            L.append(f"| {r['area']} {r['name']} | {v['lamp_pairs_ok']}/{v['lamp_pairs']} ({pct(v['lamp_pairs_ok'], v['lamp_pairs'])}) | {v['pickups_ok']}/{v['pickups']} ({pct(v['pickups_ok'], v['pickups'])}) | {v['npcs_ok']}/{v['npcs']} ({pct(v['npcs_ok'], v['npcs'])}) | "
                     + (f"{v['ratio_median']:.2f} ({v['ratio_p90']:.2f})" if v['ratio_median'] else '-') + ' |')
            for i, k in enumerate(('lamp_pairs_ok', 'lamp_pairs', 'pickups_ok', 'pickups', 'npcs_ok', 'npcs')):
                tl[i] += v[k]
        L.append(f'| **all** | {tl[0]}/{tl[1]} ({pct(tl[0], tl[1])}) | {tl[2]}/{tl[3]} ({pct(tl[2], tl[3])}) | {tl[4]}/{tl[5]} ({pct(tl[4], tl[5])}) | |')
    L.append('\n## Worst failures (lamp pairs, with the closest approach of the reachable set to the target set)\n')
    L.append('`gap` = shortest 3D distance between the nodes reachable from the source lamp and the nodes that can reach the target lamp: where a link is missing.\n')
    for r in res:
        if not r['gaps']:
            continue
        L.append(f"\n### {r['area']} {r['name']}\n")
        for g in r['gaps']:
            gp = g['gap']
            L.append(f"- `{g['scenario']}` {g['a']} -> {g['b']}: gap {gp['dist']:.1f} m (plan {gp['plan']:.1f}, dy {gp['dy']:+.1f}) between {gp['a']} and {gp['b']}; near source side: {'; '.join(g['near_a']) or 'nothing'}; near target side: {'; '.join(g['near_b']) or 'nothing'}")
    L.append('\n## Pickups / NPCs unreachable from every lamp in `all_open` (geometry or missing link)\n')
    for r in res:
        if r['unreached_all_open']:
            L.append(f"- {r['area']} {r['name']}: {len(r['unreached_all_open'])} of {r['pickups'] + r['npcs']}: " + '; '.join(f"{b['name'][:22]} {b['pos']}" for b in r['unreached_all_open'][:6]) + (' ...' if len(r['unreached_all_open']) > 6 else ''))
    L.append('\n## Mechanism inventory summary\n')
    L.append('| area | doors (behaviour) | lifts (with gate flag) | ladders (conditional) | levers | fog walls | one-way doors (side known) | door flag confidence |')
    L.append('|---|---|---|---|---|---|---|---|')
    for r in res:
        s = r['inventory']; k = s['kinds']
        L.append(f"| {r['area']} | {k.get('door', 0)} ({', '.join(f'{a} {b}' for a, b in s['door_behaviors'].items())}) | {s['lifts']} ({s['lifts_with_gate_flag']}) | {k.get('ladder', 0)} ({s['ladders_conditional']}) | {k.get('lever', 0)} | {k.get('fog', 0)} | {s['oneway_doors']} ({s['oneway_with_side']}) | {', '.join(f'{a} {b}' for a, b in s['flag_conf'].items())} |")
    # sizes
    try:
        bench = json.loads((OUT / 'bench.json').read_text())
        L.append('\n## Size and speed\n')
        L.append('| area | nodes | undirected edges | npz | BNV1 pack | Python A* median / max (ms) | expansions median | regions >=50 nodes |')
        L.append('|---|---|---|---|---|---|---|---|')
        for b in bench:
            if 'error' in b:
                continue
            import os
            bn = OUT / 'pack' / f"{b['area']}.bnv"
            sz = os.path.getsize(bn) if bn.exists() else 0
            L.append(f"| {b['area']} | {b['nodes']} | {b['edges_undirected']} | {b['npz_bytes']/1e6:.1f} MB | {sz/1e6:.2f} MB | {b.get('astar_py_ms_median')} / {b.get('astar_py_ms_max')} | {b.get('expansions_median')} | {b['regions_ge50']} |")
    except Exception as e:
        L.append(f'\n(bench.json missing: {e})\n')
    (OUT / 'report.md').write_text('\n'.join(L) + '\n')
    # flat inventory csv
    with open(OUT / 'inventory.csv', 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f)
        w.writerow(['area', 'kind', 'behavior', 'entity', 'model', 'desc_jp', 'x', 'y', 'z', 'state_flags (flag:kind:means:conf)', 'note'])
        for a in AREAS:
            inv = mechanisms.run(a)
            for m in inv['mechanisms']:
                if m['kind'] in ('lever',):
                    pass
                w.writerow([a, m['kind'], m.get('behavior', ''), m['entity'], m['model'], m['desc'], *m['pos'],
                            ' '.join(f"{x['flag']}:{x['kind']}:{x['enabled_means']}:{x['conf']}" for x in m.get('flags', [])), m.get('behavior_note', '')])
            for L2 in inv.get('lifts', []):
                st = L2['stops']
                w.writerow([a, 'lift', L2['source'], L2.get('entity', ''), L2.get('model', ''), L2.get('desc', ''), *(st[0]['pos'] if st else ['', '', '']),
                            ' '.join(f'{x}:gate:open:medium' for x in L2['gate_flags']), f"{len(st)} stops at y " + ', '.join(f"{s['pos'][1]:.1f}" for s in st)])
    print('report written', OUT / 'report.md')


if __name__ == '__main__':
    main()
