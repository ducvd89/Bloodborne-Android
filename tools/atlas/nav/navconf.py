"""Shared paths and tunable parameters for the Hunter's Atlas navigation prototype."""
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
_SP = Path('/tmp/claude-1000/-home-cid-BloodbornePC/90433437-7c99-4f34-884b-308ec8b4042b/scratchpad')   # dev default; override with the NAV_* variables
MAPS = Path(os.environ.get('NAV_MAPS', _SP / 'atlas-out/maps'))      # generator output: <area>.json, features.json, atlas.json
GAME = Path(os.environ.get('NAV_GAME', '/home/cid/BloodbornePC/game/CUSA03173'))
GAME_DATA = GAME / 'dvdroot_ps4'
OUT = Path(os.environ.get('NAV_OUT', _SP / 'nav-out'))             # graphs, reports, visualisations
GENSRC = os.environ.get('NAV_GENSRC', str(_SP / 'gen-src'))                         # patched Hunter's Atlas generator src (collision decoder)

AREAS = ['m21_00_00_00', 'm21_01_00_00', 'm22_00_00_00', 'm23_00_00_00', 'm24_00_00_00', 'm24_01_00_00',
         'm24_02_00_00', 'm25_00_00_00', 'm26_00_00_00', 'm27_00_00_00', 'm28_00_00_00', 'm32_00_00_00',
         'm33_00_00_00', 'm34_00_00_00', 'm35_00_00_00', 'm36_00_00_00']

# Tunables (all in metres). Overridable per run via build_graph.py --param key=value.
PARAMS = dict(
    cell=0.5,            # grid pitch
    merge_dy=0.35,       # nodes in one cell closer than this in height are merged (layer-seam duplicates)
    step=1.0,            # max |dy| of a normal walk link (stair risers sampled at 0.5 m reach 0.7-0.9; see README)
    slope_max=3.0,       # max |dy|/horizontal distance of a walkable link
    ledge_max=2.5,       # step < |dy| <= ledge_max: seam-repair links, penalised (0 = off)
    ledge_cost=8.0,
    drop_max=6.0,        # max one-way fall
    drop_min=0.8,        # smaller falls are just steps down (handled by walk links if step allows)
    drop_reach=2.0,      # max horizontal distance of a drop link
    drop_cost=6.0,       # extra cost (metres-equivalent) per drop
    ladder_cost=2.5,     # cost multiplier per metre climbed
    wall_tol=0.0,
)
