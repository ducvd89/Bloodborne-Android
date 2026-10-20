"""Event-script instance expander for Bloodborne's decompiled vanilla EVS files (shipped with soulstruct 2.3.2).

soulstruct ships `soulstruct/bloodborne/events/vanilla/mXX_YY_ZZ_WW.evs.py` (+ common.evs.py): the game's EMEVD
scripts decompiled to Python.  They contain the same event set as the 1.09 dump's .emevd.dcx (checked per area).
We parse them with `ast` (no soulstruct import needed) and expand every event *instance*:

    Constructor -> Event_12410110(2, obj=2410701, obj_act_id=12411301, ...)  ->  instance(event 12410110, slot 2, args)

Each instance carries the flat, source-ordered list of instruction calls with their arguments resolved to numbers
(parameters substituted).  That is enough to recognise the door / lever / lift / ladder patterns without a full
interpreter.
"""
import ast
import re
from functools import lru_cache
from pathlib import Path

import importlib.util

EVS_DIR = Path(list(importlib.util.find_spec('soulstruct').submodule_search_locations)[0]) / 'bloodborne' / 'events' / 'vanilla'
PLAYER = 10000

_EVENT_NAME = re.compile(r'^Event_(\d+)$')


class Call:
    __slots__ = ('name', 'args', 'kw', 'line', 'cond')

    def __init__(self, name, args, kw, line, cond=()):
        self.name, self.args, self.kw, self.line, self.cond = name, args, kw, line, cond

    def get(self, pos, key, default=None):
        """Argument by keyword or position."""
        if key in self.kw:
            return self.kw[key]
        if pos is not None and pos < len(self.args):
            return self.args[pos]
        return default

    def __repr__(self):
        a = ', '.join([repr(x) for x in self.args] + [f'{k}={v!r}' for k, v in self.kw.items()])
        return f'{self.name}({a})'


class Instance:
    def __init__(self, eid, slot, binding, calls, parent):
        self.eid, self.slot, self.binding, self.calls, self.parent = eid, slot, binding, calls, parent

    @property
    def slot_flag(self):
        return self.eid + self.slot


class EventDef:
    def __init__(self, eid, node, params):
        self.eid, self.node, self.params = eid, node, params


def _resolve(node, binding):
    if isinstance(node, ast.Constant):
        return node.value
    if isinstance(node, ast.Name):
        if node.id == 'PLAYER':
            return PLAYER
        return binding.get(node.id, '$' + node.id)
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
        v = _resolve(node.operand, binding)
        return -v if isinstance(v, (int, float)) else None
    if isinstance(node, ast.Attribute):
        return ast.unparse(node)
    if isinstance(node, (ast.Tuple, ast.List)):
        return tuple(_resolve(e, binding) for e in node.elts)
    return None


_FLAG_CHECKS = {'FlagEnabled': True, 'FlagDisabled': False}
_SLOT_CHECKS = {'ThisEventSlotFlagEnabled': True, 'ThisEventSlotFlagDisabled': False}
_THIS_CHECKS = {'ThisEventFlagEnabled': True, 'ThisEventFlagDisabled': False}
_GOTO = {'GotoIfFlagEnabled': ('flag', True), 'GotoIfFlagDisabled': ('flag', False),
         'GotoIfThisEventSlotFlagEnabled': ('slot', True), 'GotoIfThisEventSlotFlagDisabled': ('slot', False),
         'GotoIfThisEventFlagEnabled': ('this', True), 'GotoIfThisEventFlagDisabled': ('this', False)}


def _flag_cond(call_node, binding, eid, slot):
    """If `call_node` is a flag check -> (flag_id, polarity) else None."""
    if not (isinstance(call_node, ast.Call) and isinstance(call_node.func, ast.Name)):
        return None
    n = call_node.func.id
    if n in _FLAG_CHECKS:
        a = call_node.args[0] if call_node.args else next((k.value for k in call_node.keywords if k.arg == 'flag'), None)
        v = _resolve(a, binding) if a is not None else None
        return (v, _FLAG_CHECKS[n]) if isinstance(v, int) else None
    if n in _SLOT_CHECKS:
        return (eid + slot, _SLOT_CHECKS[n])
    if n in _THIS_CHECKS:
        return (eid, _THIS_CHECKS[n])
    return None


def _test_conds(test, binding, eid, slot):
    """Flag checks implied by an `if` test (only simple call / not / and forms)."""
    if isinstance(test, ast.UnaryOp) and isinstance(test.op, ast.Not):
        return [(f, not p) for f, p in _test_conds(test.operand, binding, eid, slot)]
    if isinstance(test, ast.BoolOp) and isinstance(test.op, ast.And):
        out = []
        for v in test.values:
            out += _test_conds(v, binding, eid, slot)
        return out
    c = _flag_cond(test, binding, eid, slot)
    return [c] if c else []


def _collect_calls(body, binding, eid=0, slot=0):
    """All calls with a plain-name callee, in source order, each tagged with the flag conditions in force
    (enclosing `if FlagEnabled(...)` tests and GotoIf...Label / DefineLabel branches)."""
    calls = []
    labels = {}                                 # label number -> (base_cond, flag, state) of the pending goto

    def emit(node, cond):
        found = []
        for n in ast.walk(node):
            if isinstance(n, ast.Call) and isinstance(n.func, ast.Name):
                found.append(n)
        found.sort(key=lambda n: (n.lineno, n.col_offset))
        for n in found:
            calls.append(Call(n.func.id, [_resolve(a, binding) for a in n.args],
                              {k.arg: _resolve(k.value, binding) for k in n.keywords if k.arg}, n.lineno, tuple(cond)))

    def visit(stmts, cond):
        cur = list(cond)
        for st in stmts:
            if isinstance(st, ast.If):
                tc = _test_conds(st.test, binding, eid, slot)
                emit(st.test, cur)
                visit(st.body, cur + tc)
                visit(st.orelse, cur + [(f, not p) for f, p in tc])
                continue
            if isinstance(st, (ast.For, ast.While, ast.With, ast.Try)):
                visit(getattr(st, 'body', []), cur)
                continue
            if isinstance(st, ast.Expr) and isinstance(st.value, ast.Call) and isinstance(st.value.func, ast.Name):
                name = st.value.func.id
                if name in _GOTO:
                    lab = st.value.args[0] if st.value.args else None
                    ln = re.search(r'L(\d+)', ast.unparse(lab)) if lab is not None else None
                    kind, state = _GOTO[name]
                    if kind == 'flag':
                        a = st.value.args[1] if len(st.value.args) > 1 else next((k.value for k in st.value.keywords if k.arg == 'flag'), None)
                        f = _resolve(a, binding) if a is not None else None
                    elif kind == 'slot':
                        f = eid + slot
                    else:
                        f = eid
                    emit(st, cur)
                    if ln and isinstance(f, int):
                        labels[int(ln.group(1))] = (list(cur), f, state)
                        cur = list(cur) + [(f, not state)]          # fall-through = flag NOT in the jump state
                    continue
                if name == 'DefineLabel':
                    n = st.value.args[0].value if st.value.args and isinstance(st.value.args[0], ast.Constant) else None
                    if n in labels:
                        base, f, state = labels[n]
                        cur = base + [(f, state)]
                    emit(st, cur)
                    continue
            emit(st, cur)

    visit(body, [])
    return calls


class EvsFile:
    def __init__(self, path):
        self.path = Path(path)
        self.tree = ast.parse(self.path.read_text(encoding='utf-8'))
        self.events = {}
        for node in self.tree.body:
            if not isinstance(node, ast.FunctionDef):
                continue
            eid = None
            for d in node.decorator_list:
                if isinstance(d, ast.Call) and d.args and isinstance(d.args[0], ast.Constant):
                    eid = d.args[0].value
            if eid is None:
                m = _EVENT_NAME.match(node.name)
                eid = int(m.group(1)) if m else {'Constructor': 0, 'Preconstructor': 50}.get(node.name)
            if eid is None:
                continue
            params = [a.arg for a in node.args.args][1:]        # drop slot arg
            self.events[eid] = EventDef(eid, node, params)


@lru_cache(maxsize=None)
def load_evs(name):
    p = EVS_DIR / f'{name}.evs.py'
    return EvsFile(p)


def expand(area_id, max_depth=6):
    """-> list[Instance] for all events reachable from the area's Constructor / Preconstructor.

    Common-event instances (RunEvent(7000, ...)) are expanded with the definitions from common.evs.py.
    """
    area = load_evs(area_id)
    common = load_evs('common')
    defs = dict(common.events); defs.update(area.events)
    instances = []
    seen = set()

    def run(eid, slot, args, kwargs, parent, depth):
        d = defs.get(eid)
        if d is None or depth > max_depth:
            return
        binding = {}
        for i, p in enumerate(d.params):
            if i < len(args):
                binding[p] = args[i]
        binding.update(kwargs)
        key = (eid, slot, tuple(sorted((k, repr(v)) for k, v in binding.items())))
        if key in seen:
            return
        seen.add(key)
        calls = _collect_calls(d.node.body, binding, eid, slot)
        inst = Instance(eid, slot, binding, calls, parent)
        instances.append(inst)
        if eid in (0, 50) or depth == 0:
            pass
        for c in calls:
            m = _EVENT_NAME.match(c.name)
            if m and int(m.group(1)) in defs and int(m.group(1)) != eid:
                a = c.args
                run(int(m.group(1)), (a[0] if a and isinstance(a[0], int) else 0), a[1:], c.kw, inst, depth + 1)
            elif c.name == 'RunEvent':
                target = c.get(0, 'event_id'); slot_ = c.kw.get('slot', 0)
                ra = c.kw.get('args', ())
                if isinstance(target, int):
                    run(target, slot_ if isinstance(slot_, int) else 0, tuple(ra) if isinstance(ra, tuple) else (), {}, inst, depth + 1)

    run(0, 0, (), {}, None, 0)
    run(50, 0, (), {}, None, 0)
    return instances
