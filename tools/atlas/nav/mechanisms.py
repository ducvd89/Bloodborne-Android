"""Per-area mechanism inventory: doors / one-way doors / key doors / levers / lifts / ladders / fog walls / bridges.

Sources (all read-only):
  * MSB parts (objects with their Japanese designer descriptions), MSB ObjAct events (entity, flag), MSB regions
  * ObjActParam rows (door/lever animation class, key item requirement, one-way special-effect requirement)
  * decompiled EMEVD (evsparse): which events animate / restore / unlock each object and under which flags
  * object collision / FLVER bounds (objinfo): door aperture, ladder height
Output: OUT/mech/<area>.json   (consumed by build.py / coverage.py / viz.py)

State flags are *candidates*, each tagged with its evidence and a confidence (see README):
  restore   flag under which an event snaps the object to its open pose on load  (EndOfAnimation)       high
  slot      the persistence flag of such an event (event id + slot)                                      high
  objact    the MSB ObjAct 'flag' (6xxxxxxx) the engine sets when the action is performed                medium
  unlock    flag enabled when a lift / door is first unlocked (EnableFlag next to EnableObjectActivation)  medium
"""
import json
import math
import re
from collections import defaultdict

import numpy as np

import evsparse
import interact
import msbread
import objinfo
from navconf import OUT, AREAS

DOOR_ANIMS = set(range(101100, 101126)) | {8022, 8023}
LEVER_ANIMS = {101150, 101151, 101230, 101210}
CHEST_ANIMS = {101141, 101142, 101143, 101144}
DOOR_WORDS = re.compile('扉|門|ドア|ゲート|鉄柵|格子|戸|シャッター|ハッチ')
LIFT_WORDS = re.compile('エレベ|リフト|昇降')
LADDER_WORDS = re.compile('はしご|ハシゴ|梯子')
FOG_WORDS = re.compile('霧')
BRIDGE_WORDS = re.compile('橋|ブリッジ')
DOOR_MARGIN = 1.0      # the opening is wider than the door leaves; the gate line is extended into the wall frame by this much
NOISE_WORDS = re.compile('ダミー|会話|NPC|宝|死体|ワープ|ライト|影|門番|ミミズ|カットシーン')


def _ints(call):
    for v in list(call.args) + list(call.kw.values()):
        if isinstance(v, int):
            yield v


def _cond_flags(call):
    return [(f, p) for f, p in call.cond]


def extract_area(mid):
    msb = msbread.read_msb(mid)
    rows = msbread.objact_params()
    parts = msb['parts']
    by_entity = {p['entity']: p for p in parts if p['entity'] > 0}
    acts_by_part = defaultdict(list)
    acts_by_entity = {}
    for a in msb['objacts']:
        a = dict(a)
        a['row'] = rows.get(a['param'])
        acts_by_entity[a['entity']] = a
        if 0 <= a['part_index'] < len(parts):
            acts_by_part[a['part_index']].append(a)
    inst = evsparse.expand(mid)
    # index instances by entity
    ent_inst = defaultdict(list)
    for i in inst:
        if i.eid in (0, 50):
            continue
        seen = set()
        for v in i.binding.values():
            if isinstance(v, int):
                seen.add(v)
        for c in i.calls:
            seen.update(_ints(c))
        for v in seen:
            if v in by_entity or v in acts_by_entity:
                ent_inst[v].append(i)

    ladders = {}                                    # entity -> (start flag, stop flag)
    for i in inst:
        for c in i.calls:
            if c.name == 'RegisterLadder':
                ent = c.get(2, 'obj')
                if isinstance(ent, int):
                    ladders[ent] = (c.get(0, 'start_climbing_flag'), c.get(1, 'stop_climbing_flag'))

    mechs = []

    def base(p, kind, sub=None):
        b = objinfo.model_bounds(p['model']) if p['model'] else dict(hkx=None, flver=None)
        return dict(id=f'{mid}:{p["name"]}', kind=kind, sub=sub, area=mid, part=p['name'], entity=p['entity'], model=p['model'],
                    desc=p['desc'], pos=[round(v, 3) for v in p['pos']], rot=[round(v, 2) for v in p['rot']],
                    scale=[round(v, 3) for v in p['scale']], bounds=b)

    # ------------------------------------------------------------------ per-object analysis
    for p in parts:
        if p['kind'] not in (1, 9) or not p['model'].startswith('o'):
            continue
        ent = p['entity']
        acts = acts_by_part.get(p['index'], [])
        anims = {a['row']['player_anim'] for a in acts if a['row']}
        desc = p['desc']
        events = ent_inst.get(ent, []) if ent > 0 else []
        is_ladder = ent in ladders
        is_lift = bool(LIFT_WORDS.search(desc)) and not re.search('レバー|スイッチ|頭蓋骨|ワープ', desc)
        is_lever = bool(anims & LEVER_ANIMS) or bool(re.search('レバー|スイッチ|ハンドル', desc))
        is_door = bool(anims & DOOR_ANIMS) or (bool(DOOR_WORDS.search(desc)) and not NOISE_WORDS.search(desc) and not FOG_WORDS.search(desc))
        is_fog = bool(FOG_WORDS.search(desc))
        is_bridge = bool(BRIDGE_WORDS.search(desc)) and not NOISE_WORDS.search(desc) and not is_door
        is_ladder_name = bool(LADDER_WORDS.search(desc)) and not is_door
        if not (is_ladder or is_lift or is_lever or is_door or is_fog or is_bridge or is_ladder_name):
            continue
        kind = ('ladder' if is_ladder else 'lift' if is_lift else 'fog' if is_fog else 'lever' if is_lever and not is_door else
                'door' if is_door else 'bridge' if is_bridge else 'ladder_name')
        m = base(p, kind)
        m['objacts'] = [dict(entity=a['entity'], flag=a['flag'], param=a['param'], default_state=a['default_state'],
                             row=None if not a['row'] else dict(name=a['row']['name'], anim=a['row']['player_anim'], obj_anim=a['row']['obj_anim'],
                                                                 button=a['row']['button'], dummy_id=a['row']['dummy_id'], valid_dist=a['row']['valid_dist'],
                                                                 valid_player_angle=a['row']['valid_player_angle'], valid_obj_angle=a['row']['valid_obj_angle'],
                                                                 sp_type=a['row']['sp_type'], sp_id=a['row']['sp_id'],
                                                                 pass_flag=a['row']['pass_flag'], failed_msg=a['row']['failed_msg'],
                                                                 success_msg=a['row']['success_msg'])) for a in acts]
        m['events'] = sorted({(i.eid, i.slot) for i in events})
        # ---- state-restore evidence: EndOfAnimation(obj, anim) under a flag condition
        restore, forces, activators = [], [], []
        for i in events:
            for c in i.calls:
                if c.name == 'EndOfAnimation' and c.get(None, 'obj') == ent:
                    restore.append(dict(anim=c.get(None, 'animation_id'), cond=[(f, bool(pl)) for f, pl in c.cond], event=i.eid, slot=i.slot))
                elif c.name == 'ForceAnimation' and c.get(0, 'entity') == ent:
                    forces.append(dict(anim=c.get(1, 'animation_id'), cond=[(f, bool(pl)) for f, pl in c.cond], event=i.eid, slot=i.slot))
            # lever -> target: objact activated in an instance that animates this object
            if any(c.name == 'ForceAnimation' and c.get(0, 'entity') == ent for c in i.calls):
                for c in i.calls:
                    if c.name == 'ObjectActivated':
                        a = acts_by_entity.get(c.get(None, 'obj_act_id'))
                        if a and 0 <= a['part_index'] < len(parts):
                            activators.append(dict(objact=a['entity'], part=parts[a['part_index']]['name'], entity=parts[a['part_index']]['entity'],
                                                   desc=parts[a['part_index']]['desc'], event=i.eid, slot=i.slot))
        m['restore'] = restore; m['forces'] = forces; m['activators'] = activators
        # ---- candidate open flags
        flags = []
        own = [a for a in acts if a['row'] and a['row']['player_anim'] in DOOR_ANIMS]
        open_anims = {a['row']['obj_anim'] for a in own if a['row']} | {f['anim'] for f in forces if f['anim'] not in (0, None)}
        for r in restore:
            if r['anim'] in (0, None):                 # closed pose: polarity of label branches is unreliable, ignored
                continue
            else:
                for f, pol in r['cond']:
                    if f == r['event'] + r['slot'] or True:
                        kindf = 'slot' if f == r['event'] + r['slot'] else 'restore'
                        flags.append(dict(flag=f, kind=kindf, enabled_means='open' if pol else 'closed',
                                          evidence=f"EndOfAnimation(anim {r['anim']}) in event {r['event']} slot {r['slot']}", conf='high'))
        for a in own:
            flags.append(dict(flag=a['flag'], kind='objact', enabled_means='open', evidence=f"MSB ObjAct {a['entity']} flag", conf='medium'))
        for act in activators:
            a = acts_by_entity.get(act['objact'])
            if a and a['flag'] not in [f['flag'] for f in flags]:
                flags.append(dict(flag=a['flag'], kind='objact', enabled_means='open', evidence=f"MSB ObjAct {a['entity']} (activator {act['part']}) flag", conf='medium'))
        # dedupe
        seen = set(); fl = []
        for f in flags:
            k = (f['flag'], f['kind'], f['enabled_means'])
            if k not in seen and isinstance(f['flag'], int):
                seen.add(k); fl.append(f)
        m['flags'] = fl
        if is_ladder:
            m['ladder_flags'] = ladders[ent]
        mechs.append(m)

    region_by_entity = {r['entity']: r for r in msb['regions'] if r['entity'] > 0}
    groups = _sp4150_groups(inst, region_by_entity)
    for m in mechs:
        if m['kind'] == 'door':
            _classify_door(m, parts, groups)
        elif m['kind'] == 'fog' and m['model']:
            m['frame'] = _door_frame(m); m['behavior'] = 'fog'
            m['behavior_note'] = 'boss / multiplayer fog wall: passable, only blocks while its event is active'
    interact.annotate(mechs)
    out = dict(area=mid, mechanisms=mechs, oneway_groups=groups)
    out['lift_stops'] = _lift_stops(msb)
    out['lifts'] = _build_lifts(out['lift_stops'], mechs, inst, by_entity, acts_by_entity, parts, region_by_entity)
    interact.annotate_lifts(out)
    out['oneway_regions'] = [dict(entity=r['entity'], name=r['name'], pos=r['pos'], rot=r['rot'], size=r['size'], shape=r['shape'])
                             for r in msb['regions'] if re.search('一方通行', r['name'])]
    return out


def _sp4150_groups(inst, regions):
    """Events granting SpEffect 4150 (the one-way-door key effect) while the player stands in a region.
    -> [dict(event, slot, regions=[dict(entity, pos, needs_flag)])]  (needs_flag: flag checked right after that region)"""
    out = []
    for i in inst:
        if i.eid in (0, 50):
            continue
        if not any(c.name == 'AddSpecialEffect' and c.get(1, 'sp_effect_id') == 4150 for c in i.calls):
            continue
        regs = []
        for c in i.calls:
            if c.name == 'CharacterInsideRegion':
                ent = c.get(1, 'region')
                if isinstance(ent, int):
                    r = regions.get(ent)
                    regs.append(dict(entity=ent, pos=r['pos'] if r else None, needs_flag=None))
            elif c.name == 'FlagEnabled' and regs and regs[-1]['needs_flag'] is None:
                f = c.get(0, 'flag')
                if isinstance(f, int):
                    regs[-1]['needs_flag'] = f
        if regs:
            out.append(dict(event=i.eid, slot=i.slot, regions=regs))
    return out


def _door_frame(m):
    """Aperture segment of a door object: along the longer horizontal extent of its collision/FLVER box."""
    lo, hi = _bounds(m)
    sx, sy, sz = m['scale'] if m.get('scale') else (1, 1, 1)
    ex, ez = (hi[0] - lo[0]) * abs(sx), (hi[2] - lo[2]) * abs(sz)
    pos = np.array(m['pos'])
    ax = objinfo.local_to_world(np.zeros(3), m['rot'], [1, 0, 0]); az = objinfo.local_to_world(np.zeros(3), m['rot'], [0, 0, 1])
    if ez > ex:                                       # door built along local Z (e.g. o322110, o321100)
        t, n, t0, t1 = az, ax, lo[2] * sz, hi[2] * sz
    else:
        t, n, t0, t1 = ax, az, lo[0] * sx, hi[0] * sx
    a = pos + t * (t0 - DOOR_MARGIN); b = pos + t * (t1 + DOOR_MARGIN)
    return dict(segment=[round(float(a[0]), 3), round(float(a[2]), 3), round(float(b[0]), 3), round(float(b[2]), 3)],
                y_lo=round(float(pos[1]) - 0.5, 3), y_hi=round(float(pos[1] + max(hi[1] * sy, 2.0)), 3),
                center=[round(float((a[0] + b[0]) / 2), 3), round(float((a[2] + b[2]) / 2), 3)],
                normal=[round(float(n[0]), 4), round(float(n[2]), 4)], width=round(float(t1 - t0 + 2 * DOOR_MARGIN), 2))


def _bounds(m):
    b = m['bounds']
    v = b.get('hkx') or b.get('flver')
    if v and (abs(v[0][0]) > 1e6 or abs(v[1][0]) > 1e6):      # FLVER with the empty-box sentinel
        v = None
    if not v:
        return np.array([-1.0, 0, -0.3]), np.array([1.0, 2.5, 0.3])
    return np.array(v[0]), np.array(v[1])


def _classify_door(m, parts, groups):
    m['frame'] = _door_frame(m)
    own = [a for a in m['objacts'] if a['row'] and a['row']['anim'] in DOOR_ANIMS]
    beh = None; why = ''
    if own:
        row = own[0]['row']
        if '一方通行' in row['name'] or (row['sp_type'] == 2 and row['sp_id'] == 4150):
            beh, why = 'oneway', row['name']
        elif row['sp_type'] == 1:
            beh, why = 'key', f"{row['name']} (item {row['sp_id']})"
        elif row['sp_type'] == 2:
            beh, why = 'special', f"{row['name']} (speffect {row['sp_id']})"
        else:
            beh, why = 'normal', row['name']
        m['sp'] = dict(type=row['sp_type'], id=row['sp_id'], pass_flag=row['pass_flag'])
    elif m['activators']:
        beh, why = 'lever', 'opened by ' + ', '.join(sorted({a['desc'] or a['part'] for a in m['activators']}))
    elif m['entity'] > 0 and (m['forces'] or m['restore']):
        beh, why = 'scripted', 'animated by event script only'
    elif m['entity'] <= 0:
        beh, why = 'static', 'no entity id and no ObjAct: not scripted, not openable'
    else:
        beh, why = 'unknown', ''
    m['behavior'] = beh; m['behavior_note'] = why
    # operable-side reference points
    pts = []
    if beh == 'oneway' and groups:
        c = np.array([m['frame']['center'][0], m['frame']['center'][1]])
        best = None
        for g in groups:
            ds = [np.hypot(r['pos'][0] - c[0], r['pos'][2] - c[1]) for r in g['regions'] if r['pos']]
            if ds and (best is None or min(ds) < best[0]):
                best = (min(ds), g)
        if best and best[0] < 12:
            for r in best[1]['regions']:
                if r['pos'] and r['needs_flag'] is None:
                    pts.append([r['pos'][0], r['pos'][2]])
            m['oneway_group'] = dict(event=best[1]['event'], slot=best[1]['slot'], regions=best[1]['regions'], dist=round(float(best[0]), 1))
            for r in best[1]['regions']:
                if r['needs_flag'] is not None:
                    m['flags'].append(dict(flag=r['needs_flag'], kind='unlock', enabled_means='open_both_sides',
                                           evidence=f"event {best[1]['event']}: far side of the one-way door works once this flag is set", conf='medium'))
    elif beh == 'lever':
        for a in m['activators']:
            for p in parts:
                if p['entity'] == a['entity'] and p['entity'] > 0:
                    pts.append([p['pos'][0], p['pos'][2]])
    m['operable_points'] = pts


def _flag_refs(i):
    for c in i.calls:
        if c.name in ('FlagEnabled', 'FlagDisabled'):
            f = c.get(0, 'flag')
        elif c.name.startswith('GotoIfFlag') and c.name not in ('GotoIfFlagRangeAnyEnabled',):
            f = c.get(1, 'flag')
        else:
            continue
        if isinstance(f, int):
            yield f


def _mover_lifts(inst, by_entity, acts_by_entity, parts, regions, skip):
    """Objects animated by lever events: ObjectActivated(lever) + ForceAnimation(mover) (+ player-in-region stop zones).
    Stops = lever objects and stop regions of those events, clustered by height."""
    mov = {}
    for i in inst:
        if i.eid in (0, 50):
            continue
        acts = [c for c in i.calls if c.name == 'ObjectActivated']
        if not acts:
            continue
        for c in i.calls:
            if c.name != 'ForceAnimation':
                continue
            x = c.get(0, 'entity')
            p = by_entity.get(x) if isinstance(x, int) else None
            if not p or p['kind'] not in (1, 9) or not p['model'].startswith('o') or x in skip:
                continue
            d = mov.setdefault(x, dict(points=[], events=set()))
            d['events'].add(i.eid)
            for a in acts:
                act = acts_by_entity.get(a.get(None, 'obj_act_id'))
                if act and 0 <= act['part_index'] < len(parts):
                    d['points'].append(('lever', parts[act['part_index']]['entity'], parts[act['part_index']]['pos']))
            for r in i.calls:
                if r.name == 'CharacterInsideRegion':
                    reg = regions.get(r.get(1, 'region'))
                    if reg:
                        d['points'].append(('region', reg['entity'], reg['pos']))
    out = []
    for x, d in mov.items():
        p = by_entity[x]; px, py, pz = p['pos']
        pts = [q for q in d['points'] if math.hypot(q[2][0] - px, q[2][2] - pz) < 30]
        pts.sort(key=lambda q: q[2][1])
        clusters = []
        for q in pts:
            if clusters and abs(q[2][1] - clusters[-1][-1][2][1]) < 3.0:
                clusters[-1].append(q)
            else:
                clusters.append([q])
        if len(clusters) < 2:
            continue
        stops = []
        for cl in clusters:
            reg = [q for q in cl if q[0] == 'region']
            ref = reg[0] if reg else cl[0]
            stops.append(dict(pos=list(ref[2]), region=ref[1] if reg else None, lever=next((q[1] for q in cl if q[0] == 'lever'), None)))
        out.append(dict(stops=stops, source='mover', entity=x, model=p['model'], desc=p['desc'], _obj=None, _events=sorted(d['events'])))
    return out


def _build_lifts(stops, mechs, inst, by_entity, acts_by_entity, parts, regions=None):
    """Lifts = pairs of top/bottom 'elevator switch' regions at the same x,z (authoritative stop positions).
    Lift objects without a region pair get stops from the levers sharing their designer-name prefix.
    Flags: those checked by every event instance that mentions the lift, its stop regions or its levers."""
    lifts = []
    used = set()
    tops = [s for s in stops if s['side'] == 'top']; bots = [s for s in stops if s['side'] == 'bottom']
    for t in tops:
        best = None
        for b in bots:
            d = math.hypot(t['pos'][0] - b['pos'][0], t['pos'][2] - b['pos'][2])
            if d < 2.5 and b['entity'] not in used and (best is None or d < best[0]):
                best = (d, b)
        if best:
            used.add(best[1]['entity'])
            lifts.append(dict(stops=[dict(pos=best[1]['pos'], region=best[1]['entity']), dict(pos=t['pos'], region=t['entity'])], source='regions'))
    objs = [m for m in mechs if m['kind'] == 'lift']
    skip = {m['entity'] for m in mechs if m['kind'] in ('door', 'ladder', 'fog', 'lever')}
    for L in _mover_lifts(inst, by_entity, acts_by_entity, parts, regions or {}, skip):
        # a mover whose stops coincide with an already found region-pair lift refines that lift instead
        dup = next((K for K in lifts if any(math.hypot(a['pos'][0] - b['pos'][0], a['pos'][2] - b['pos'][2]) < 8 and abs(a['pos'][1] - b['pos'][1]) < 3
                                           for a in K['stops'] for b in L['stops'])), None)
        if dup is not None:
            dup.update(entity=L['entity'], model=L['model'], desc=L['desc'])
        else:
            lifts.append(L)
    for L in lifts:
        cx, cz = L['stops'][0]['pos'][0], L['stops'][0]['pos'][2]
        near = [m for m in objs if math.hypot(m['pos'][0] - cx, m['pos'][2] - cz) < 14]
        if near:
            m = min(near, key=lambda m: math.hypot(m['pos'][0] - cx, m['pos'][2] - cz))
            L.update(entity=m['entity'], model=m['model'], desc=m['desc'], objact_flags=[a['flag'] for a in m['objacts']]); L['_obj'] = m['id']
    taken = {L.get('_obj') for L in lifts}
    for m in objs:
        if m['id'] in taken:
            continue
        key = m['desc'].split('_')[0]
        lev = [p for p in parts if p['entity'] > 0 and re.search('レバー', p['desc']) and p['desc'].split('_')[0] == key]
        if len(lev) >= 2:
            lifts.append(dict(stops=[dict(pos=p['pos'], lever=p['entity']) for p in lev], source='levers', entity=m['entity'], model=m['model'], desc=m['desc'], _obj=m['id']))
        else:
            lifts.append(dict(stops=[], source='none', entity=m['entity'], model=m['model'], desc=m['desc'], _obj=m['id']))
    for k, L in enumerate(lifts):
        L['id'] = f'lift{k}'
        ents = {s.get('region') for s in L['stops']} | {s.get('lever') for s in L['stops']} | {L.get('entity')}
        ents.discard(None)
        # levers near each stop (objects with a lever ObjAct) also count
        for m in mechs:
            if m['kind'] == 'lever' and any(math.hypot(m['pos'][0] - s['pos'][0], m['pos'][2] - s['pos'][2]) < 6 for s in L['stops']):
                ents.add(m['entity'])
        sets = []
        for i in inst:
            if i.eid in (0, 50):
                continue
            seen = set(v for v in i.binding.values() if isinstance(v, int))
            for c in i.calls:
                seen.update(_ints(c))
            if seen & ents:
                sets.append(set(_flag_refs(i)))
        common = set.intersection(*sets) if sets else set()
        allf = set().union(*sets) if sets else set()
        L['gate_flags'] = sorted(common); L['all_flags'] = sorted(allf)
        L['events'] = len(sets)
        L.pop('_obj', None)
    return lifts


def _lift_stops(msb):
    """Pairs of regions named ...エレベータ(ー)スイッチ/起動 上|下 at the same x,z -> stops."""
    regs = [r for r in msb['regions'] if re.search('エレベ|リフト', r['name']) and re.search('上|下|【', r['name']) and r['entity'] > 0]
    stops = []
    for r in regs:
        n = r['name']
        if '初回' in n:
            continue
        side = 'top' if ('上' in n) else 'bottom' if ('下' in n) else None
        if side:
            stops.append(dict(entity=r['entity'], name=n, side=side, pos=r['pos']))
    return stops


def run(mid, force=False):
    p = OUT / 'mech' / f'{mid}.json'
    p.parent.mkdir(parents=True, exist_ok=True)
    if p.exists() and not force:
        return json.loads(p.read_text())
    d = extract_area(mid)
    p.write_text(json.dumps(d, ensure_ascii=False, indent=1))
    return d


if __name__ == '__main__':
    import sys
    from multiprocessing import Pool
    areas = sys.argv[1:] or AREAS
    for a in areas:
        d = run(a, force=True)
        c = defaultdict(int)
        for m in d['mechanisms']:
            c[m['kind']] += 1
        print(a, dict(c))
    objinfo.save_cache()
