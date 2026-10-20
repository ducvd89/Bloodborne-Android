"""Object model extents (from obj/oNNNNNN.objbnd.dcx): Havok collision bounds when decodable, FLVER bbox otherwise.

Used for ladder heights, door apertures and lift travel.  Results are cached in OUT/objbounds.json.
"""
import json
import struct
import sys

import numpy as np
from soulstruct.containers import Binder

from navconf import GAME_DATA, GENSRC, OUT

_CACHE = None
_FILE = OUT / 'objbounds.json'


def _collision_mesh():
    if GENSRC and GENSRC not in sys.path:
        sys.path.insert(0, GENSRC)
    from generate_navigation_maps import collision_mesh      # Havok 2014 compressed-mesh decoder of the generator
    return collision_mesh


def _load():
    global _CACHE
    if _CACHE is None:
        try:
            _CACHE = json.loads(_FILE.read_text())
        except Exception:
            _CACHE = {}
    return _CACHE


def model_bounds(model):
    """-> dict(hkx=[[x0,y0,z0],[x1,y1,z1]] | None, flver=[[..],[..]] | None) in the model's local frame."""
    cache = _load()
    if model in cache:
        return cache[model]
    out = dict(hkx=None, flver=None)
    path = GAME_DATA / 'obj' / f'{model}.objbnd.dcx'
    if path.exists():
        try:
            b = Binder.from_path(path)
            for e in b.entries:
                low = e.name.lower()
                if low.endswith('.hkx') and out['hkx'] is None:
                    try:
                        t = _collision_mesh()(e.get_uncompressed_data()).reshape(-1, 3)
                        out['hkx'] = [t.min(0).round(3).tolist(), t.max(0).round(3).tolist()]
                    except Exception:
                        pass
                elif low.endswith('.flver') and out['flver'] is None:
                    d = e.get_uncompressed_data()[:0x40]
                    if d[:5] == b'FLVER':
                        v = struct.unpack_from('<6f', d, 0x28)
                        out['flver'] = [[round(x, 3) for x in v[:3]], [round(x, 3) for x in v[3:]]]
        except Exception:
            pass
    cache[model] = out
    return out


def save_cache():
    if _CACHE is not None:
        OUT.mkdir(parents=True, exist_ok=True)
        _FILE.write_text(json.dumps(_CACHE))


def best_bounds(model):
    """hkx bounds when available else flver, as np arrays (lo, hi), or None."""
    b = model_bounds(model)
    v = b['hkx'] or b['flver']
    return (np.array(v[0]), np.array(v[1])) if v else None


def yaw_matrix(rot_deg_y):
    t = np.radians(rot_deg_y); c, s = np.cos(t), np.sin(t)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def local_to_world(part_pos, part_rot, local):
    """MSB convention used by the generator: world = Rz Ry Rx * (scale*local) + pos (degrees)."""
    a = np.radians(part_rot); cx, cy, cz = np.cos(a); sx, sy, sz = np.sin(a)
    rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return (rz @ ry @ rx) @ np.asarray(local, float) + np.asarray(part_pos, float)
