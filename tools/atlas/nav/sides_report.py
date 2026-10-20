"""Which side of each door / mechanism the action button works from, and how that compares with the event-derived regions.

    python sides_report.py   (reads OUT/mech/*.json)   -> console tables + OUT/sides.md
"""
import glob
import json
import math
from collections import Counter, defaultdict

import numpy as np

import interact
import objinfo
from navconf import OUT


def main():
    rows = []
    kinds = defaultdict(Counter)
    snap_ok = snap_bad = 0
    ev_rows = []
    lever_ok = lever_bad = 0
    for p in sorted(glob.glob(str(OUT / 'mech' / '*.json'))):
        d = json.load(open(p))
        for L in d.get('lifts', []):
            kinds['lift']['total'] += 1; kinds['lift']['has region'] += bool(L.get('levers'))
        for m in d['mechanisms']:
            if m['kind'] not in ('door', 'fog', 'ladder', 'lever'):
                continue
            beh = m.get('behavior') or m['kind']
            key = m['kind'] if m['kind'] != 'door' else 'door/' + beh
            kinds[key]['total'] += 1
            if m['kind'] == 'door':
                kinds[key][m['side']] += 1
                kinds[key]['side determined'] += m['side'] in ('front', 'back', 'both')
            elif m['kind'] == 'ladder':
                kinds[key]['has region'] += bool(m.get('levers'))     # kick-down ladders: the lever that kicks them down
            elif m['regions']:
                kinds[key]['has region'] += 1
            fr = m.get('frame')
            # internal check: the dummy the character snaps to (ObjActParam objDummyId) is on the same side as the button region
            if m['kind'] == 'door' and fr and m['regions'] and beh != 'lever':
                for a in m['objacts']:
                    r = a.get('row')
                    if not r:
                        continue
                    ab = interact.action_buttons().get(r['button'])
                    if not ab or r['dummy_id'] == ab['dummy1'] or r['dummy_id'] < 0:
                        continue
                    reg = next((q for q in m['regions'] if q['objact'] == a['entity']), None)
                    for pos, f in interact.dummy_model_space(m['model'], r['dummy_id']):
                        w = objinfo.local_to_world(m['pos'], m['rot'], pos)
                        s1 = interact._side_of(fr, (w[0], w[2])); s2 = interact._side_of(fr, interact.region_centre(reg)) if reg else 0
                        if s1 and s2:
                            snap_ok += s1 == s2; snap_bad += s1 != s2
            # event-derived one-way grant regions
            if m['kind'] == 'door' and beh == 'oneway' and m.get('event_operable_points') and fr and m['regions']:
                ev = m['event_operable_points'][0]
                near = min(math.hypot(ev[0] - c[0], ev[1] - c[1]) for c in [interact.region_centre(r) for r in m['regions']])
                ev_rows.append((m['area'], m['part'], interact._side_of(fr, ev), m['side'], round(near, 1)))
    out = ['# Interaction regions: operable sides', '', '| kind | total | side determined / has region | front | back | both | unknown |', '|---|---|---|---|---|---|---|']
    for k, c in sorted(kinds.items()):
        det = c['side determined'] if k.startswith('door') else c['has region']
        out.append(f"| {k} | {c['total']} | {det} | {c['front']} | {c['back']} | {c['both']} | {c['unknown']} |")
    out += ['', f'Check 1 (all doors whose ObjAct snap dummy differs from the button dummy): snap position side == region side: {snap_ok} agree, {snap_bad} disagree.', '',
            'Check 2 (one-way doors, event-derived SpEffect 4150 grant point vs the button region): area, door, event side (+1 front, -1 back), region side, distance (m) grant point to region centre', '']
    for r in ev_rows:
        out.append('* ' + ', '.join(map(str, r)))
    text = '\n'.join(out)
    (OUT / 'sides.md').write_text(text + '\n')
    print(text)


if __name__ == '__main__':
    main()
