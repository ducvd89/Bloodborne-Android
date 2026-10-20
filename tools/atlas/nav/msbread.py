"""Minimal read-only Bloodborne MSB / ObjActParam reader (same byte layouts as the Hunter's Atlas generator).

Only the fields the navigation prototype needs: parts (objects, collisions ...), ObjAct events, regions.
"""
import struct
from functools import lru_cache
from pathlib import Path

from soulstruct.dcx import decompress
from soulstruct.containers import Binder

from navconf import GAME_DATA


def _raw(path):
    b = Path(path).read_bytes()
    return decompress(b)[0] if b[:4] == b'DCX\0' else b


def _lists(b):
    def text(o):
        end = o
        while b[end:end + 2] != b'\0\0':
            end += 2
        return b[o:end].decode('utf-16le')
    lists = {}; off = 16
    while off:
        _, count, no = struct.unpack_from('<iiq', b, off)
        offs = struct.unpack_from(f'<{count}q', b, off + 16)
        lists[text(no)] = offs[:-1]; off = offs[-1]
    return lists, text


PART_KINDS = {0: 'MapPiece', 1: 'Object', 2: 'Enemy', 4: 'Player', 5: 'Collision', 8: 'Navmesh', 9: 'DummyObject',
              10: 'DummyEnemy', 11: 'ConnectCollision'}


@lru_cache(maxsize=None)
def read_msb(mid):
    """-> dict(parts=[...], objacts=[...], regions=[...]) for map id like 'm24_01_00_00'."""
    b = _raw(GAME_DATA / 'map/mapstudio' / f'{mid}.msb.dcx')
    lists, text = _lists(b)
    models = [text(o + struct.unpack_from('<q', b, o)[0]) for o in lists['MODEL_PARAM_ST']]
    parts = []
    for idx, o in enumerate(lists['PARTS_PARAM_ST']):
        desc, name = struct.unpack_from('<qq', b, o)
        kind = struct.unpack_from('<i', b, o + 20)[0]; model = struct.unpack_from('<i', b, o + 28)[0]
        common, sub = struct.unpack_from('<qq', b, o + 176)
        parts.append(dict(index=idx, name=text(o + name), desc=text(o + desc), kind=kind,
                          kind_name=PART_KINDS.get(kind, str(kind)), model=models[model] if model >= 0 else '',
                          pos=list(struct.unpack_from('<3f', b, o + 40)), rot=list(struct.unpack_from('<3f', b, o + 52)),
                          scale=list(struct.unpack_from('<3f', b, o + 64)), entity=struct.unpack_from('<i', b, o + common)[0]))
    objacts = []
    for o in lists['EVENT_PARAM_ST']:
        name, index, kind = struct.unpack_from('<qii', b, o)
        if kind != 7:
            continue
        common, sub = struct.unpack_from('<qq', b, o + 24)
        entity, part, param, state, flag = struct.unpack_from('<iiiB3xi', b, o + sub)
        objacts.append(dict(name=text(o + name), entity=entity, part_index=part, param=param, default_state=state, flag=flag))
    regions = []
    for o in lists['POINT_PARAM_ST']:
        name = text(o + struct.unpack_from('<q', b, o)[0])
        idx = struct.unpack_from('<i', b, o + 12)[0]; shape = struct.unpack_from('<i', b, o + 16)[0]
        pos = struct.unpack_from('<3f', b, o + 20); rot = struct.unpack_from('<3f', b, o + 32)
        offs = struct.unpack_from('<4q', b, o + 0x30)
        ent = struct.unpack_from('<i', b, o + offs[3])[0]
        sh = struct.unpack_from('<3f', b, o + offs[0])
        regions.append(dict(index=idx, name=name, shape=shape, pos=list(pos), rot=list(rot), size=list(sh), entity=ent))
    return dict(parts=parts, objacts=objacts, regions=regions)


@lru_cache(maxsize=None)
def objact_params():
    """ObjActParam rows from gameparam (fields by paramdef order). -> {row_id: dict}"""
    path = GAME_DATA / 'param/gameparam/gameparam.parambnd.dcx'
    entry = next(e for e in Binder.from_path(path).entries if e.name.replace('\\', '/').endswith('/ObjActParam.param'))
    b = entry.get_uncompressed_data()
    assert b[44:46] == b'\0\x04', 'expected 64-bit little-endian param layout'
    count = struct.unpack_from('<H', b, 10)[0]; rows = {}
    for i in range(count):
        rid, _, offset, nameoff = struct.unpack_from('<iiqq', b, 64 + 24 * i)
        end = nameoff
        while b[end:end + 2] != b'\0\0':
            end += 2
        (enable_msg, failed_msg, pass_flag, player_anim, chr_anim, valid_dist, sp_id, sp_id2, dummy_id, _pad,
         obj_anim, valid_player_angle, sp_type, sp_type2, valid_obj_angle, sorb, kick, _p1, _p2,
         button, success_msg) = struct.unpack_from('<iiiIIHHHBBIBBBBBBBBii', b, offset)
        rows[rid] = dict(name=b[nameoff:end].decode('utf-16le'), enable_msg=enable_msg, failed_msg=failed_msg,
                         pass_flag=pass_flag, player_anim=player_anim, valid_dist=valid_dist, sp_id=sp_id, sp_id2=sp_id2,
                         dummy_id=dummy_id, obj_anim=obj_anim, valid_player_angle=valid_player_angle, sp_type=sp_type,
                         sp_type2=sp_type2, valid_obj_angle=valid_obj_angle, button=button, success_msg=success_msg)
    return rows
