"""Scenarios: map every mechanism of a graph to 'open' | 'closed' | 'oneway'.

all_open   everything passable (upper bound of what geometry + ladders/drops can give)
mech       "levers pulled": lifts / kick-down ladders / lever gates active; ordinary doors open; one-way doors passable from
           their operable side; key doors, scripted doors and permanently-shut (static) doors blocked
fresh      "new game state": like `mech` but lifts, kick-down ladders and lever gates inactive
all_closed everything blocked (default state of every flag-controlled object)
live       built from a dict of event flags {flag_id: bool} (what the app does at run time)
"""
from graph import NavGraph


def state_for(m, scenario, flags=None):
    kind = m.get('kind'); beh = m.get('behavior')
    if scenario == 'all_open':
        return 'open'
    if scenario == 'all_closed':
        return 'closed'
    if scenario == 'live':
        return live_state(m, flags or {})
    active = scenario == 'mech'
    if kind == 'fog':
        return 'open'
    if kind == 'door':
        if beh == 'normal':
            return 'open'
        if beh == 'oneway':                      # operable side unknown (not derivable from events) -> optimistic: passable
            return 'oneway' if m.get('operable_points') else 'open'
        if beh == 'lever':
            return 'open' if active else ('oneway' if m.get('operable_points') else 'closed')
        if beh in ('scripted', 'unknown'):       # opened by story events: assume done in `mech`
            return 'open' if active else 'closed'
        return 'closed'                          # key / special (boss) / static
    if kind in ('lift', 'ladder'):
        return 'open' if active else 'closed'
    return 'open'


def default_state(m):
    """The state of a gate when none of its candidate flags is set (the app's rule; tools/atlas/build_pack.py gate_table has the
    same one):  ordinary and one-way doors are opened by walking up to them, so they can be crossed from the side the action
    button works from (`side` front / back; both sides or unknown: either way), fog walls are walked through, lever doors, lifts
    and kick-down ladders can be crossed starting from the side of their lever, key, story, special and static doors and
    lifts / ladders with no known lever are closed."""
    kind = m.get('kind'); beh = m.get('behavior'); side = m.get('side', 'unknown')
    known = side in ('front', 'back') or bool(m.get('operable_points'))
    if kind == 'fog':
        return 'open'
    if kind == 'door':
        if beh in ('normal', 'oneway'):
            return 'oneway' if known else 'open'
        if beh == 'lever':
            return 'oneway' if known else ('open' if side == 'both' else 'closed')
        return 'closed'
    if kind in ('lift', 'ladder'):
        return 'oneway' if m.get('levers') else 'closed'
    return 'open'


def live_state(m, flags):
    """Resolve a mechanism from live event flags: open if any candidate flag is set, else its default state."""
    for f in m.get('flags', []):
        if flags.get(f['flag']):
            return 'open'
    return default_state(m)


def scenario_state(g, scenario, flags=None):
    return {i: state_for(m, scenario, flags) for i, m in enumerate(g.mechs)}
