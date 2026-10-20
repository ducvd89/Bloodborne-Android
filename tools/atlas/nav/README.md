# Hunter's Atlas route navigation: PC prototype

Route finding for the Hunter's Atlas map (Android port): "route to a lamp / pickup / NPC / any spot". It builds a walkable graph
per area from the generator's floor / wall data, adds ladders, lifts, drops and gates (doors) whose state depends on game
event flags, and measures how well lamps, pickups and NPCs are connected. `pack.py` writes the graphs (BNV1) that map pack
version 2 carries (`../build_pack.py --nav`, `../generate.sh`); the app (AtlasNav, NavData, NavGate in
tools/android/frontend) reads them. `javaref.py`, `livecov.py` and `sides_report.py` are the checks around that (below).

## Run

Needs the generator venv (numpy, scipy, shapely, soulstruct 2.3.2, pillow). Defaults point at the scratch directories of the
session that produced this; override with environment variables:

| variable | meaning |
|---|---|
| `NAV_MAPS` | generator output (`<area>.json` v2, `features.json`) |
| `NAV_GAME` | game dump folder (`CUSA03173`, read only) |
| `NAV_OUT` | output (graphs, reports, html) |
| `NAV_GENSRC` | patched Hunter's Atlas generator `src` (only for its Havok collision decoder, used on object `.hkx`) |

```
PY=/path/to/venv/bin/python tools/atlas/nav/run_all.sh        # everything, about 4 minutes, nice 19
$PY mechanisms.py [area..]    # mechanism inventory           -> OUT/mech/<area>.json
$PY coverage.py  [area..]     # coverage test, 4 scenarios     -> OUT/coverage.json (+ console summary)
$PY report.py                 # tables, failures, inventory    -> OUT/report.md, OUT/inventory.csv
$PY bench.py                  # sizes + A* timing              -> OUT/bench.json
$PY pack.py                   # compact BNV1 format + roundtrip -> OUT/pack/*.bnv
$PY viz.py m24_01_00_00 m24_00_00_00                          -> OUT/<area>.html
$PY tune.py step=0.6,1.0 ledge_max=0,2.5                      # parameter sweep over all areas
```

## Files

| file | role |
|---|---|
| `navconf.py` | paths, tunable `PARAMS` |
| `grid.py` | 0.5 m height-aware grid: triangles rasterised at cell centres (barycentric height), one node per distinct floor per cell |
| `links.py` | walk / ledge links (8-neighbourhood, wall-blocked via a cell-registered wall index), drop links |
| `msbread.py` | read-only MSB (parts, ObjAct events, regions) + ObjActParam reader (same byte layouts as the generator) |
| `evsparse.py` | expands the decompiled vanilla EMEVD (`soulstruct/bloodborne/events/vanilla/*.evs.py`) into event instances with resolved arguments and flag conditions |
| `objinfo.py` | object model extents (Havok collision, FLVER bbox fallback): ladder height, door aperture |
| `mechanisms.py` | the mechanism inventory (below) |
| `build.py` | assembles the graph: grid + links + gates + ladders + lifts |
| `graph.py`, `scenario.py` | graph container, scenario masks (open / closed / one-way), CSR export, A* |
| `coverage.py`, `report.py`, `tune.py` | coverage test, failure diagnosis (`gap_between`), report, sweeps |
| `bench.py`, `pack.py` | sizes / timing, compact on-disk format (`pack.py` also fails when a file does not decode to the same graph) |
| `interact.py`, `sides_report.py` | interaction regions of doors / levers / lifts (ActionButtonParam + FLVER dummy polygons) and the report on them |
| `javaref.py`, `livecov.py` | reference route costs for the Java test (`tests/android-nav/run.sh`), coverage under the app's live rules |
| `viz.py` | HTML visualisation (also run with chromium `--screenshot` for PNG) |

## Graph

* **Nodes**: 0.5 m cells, one node per distinct floor height per cell (370k nodes for Central Yharnam; 29k to 520k per area).
  The generator's floor triangles are per 3 m band, so floors are sampled per layer and merged (nodes closer than 0.35 m in one cell are dropped).
* **Walk links** (8 neighbours): `|dy| <= step` (1.0 m). Blocked when the link crosses a collision wall segment of either endpoint's layer.
* **Ledge links** (`step < |dy| <= 2.5 m`, cost +8): seam repair, see "what each fix did".
* **Drops** (one way): from a node whose floor *ends* on that side, to a floor 0.8-6 m below within 2 m horizontally (cost +6). Falling through a floor is not possible.
* **Ladders**: `RegisterLadder` markers; bottom landing 0.7 m in front of the model origin, top landing at the object's collision height (FLVER fallback; for models without geometry: nearest floor 2-30 m above). Joined through virtual nodes. 47 of 48 found both ends.
* **Lifts**: stop pairs from the "elevator switch / start" regions (authoritative stop positions), or from lever objects of the event that animates the object; landings within 5 m of each stop. Cost 12 + 0.6 per metre.
* **Gates** (doors, grates, fog walls): an aperture segment from the model's longer horizontal extent plus 1 m each side (the opening is wider than the door leaves). Every walk / drop link crossing it carries the gate index and its crossing side. The scenario decides: open / closed / one-way (passable only from the operable side).

Scenarios (`scenario.py`): `all_open`, `mech` (levers pulled, ordinary doors open, key / static / boss doors blocked, scripted doors open), `fresh` (like `mech` but lifts, kick-down ladders, lever gates, scripted doors inactive), `all_closed`, and `live` (from a dict of event flags: the app's mode).

## Mechanism inventory (what gates movement)

`OUT/mech/<area>.json`, flat table in `OUT/inventory.csv`. Per object: kind, behaviour, entity id, model, designer description (Japanese), position, candidate state flags with evidence and confidence.

What decides each kind (all decoded from the dump, nothing guessed from appearance):

* **Doors**: ObjAct events (MSB) attach an ObjActParam row to the door. The row gives the class: `sp_type 1` = key item needed (`sp_id` = goods id, `spQualifiedPassEventFlag` = flag that bypasses it), `sp_type 2` with id 4150 or "一方通行" in the name = **one-way** door (the player gets SpEffect 4150 from an event only while standing in one of two regions; the far-side region needs a flag, which is set after the first opening: this gives the operable side and a second flag), name with "SC" = shortcut gate, no ObjAct but lever events = lever door, no entity id and no ObjAct = permanently shut prop door (`static`). Door aperture axis and width come from the object's collision / FLVER box (some doors are built along local Z).
* **Levers**: ObjAct rows with lever animations (101150/101151/101230); linked to their targets through the event that waits for the ObjAct and animates the target.
* **Lifts**: regions "...エレベータ起動領域【上】/【下】", "...エレベータースイッチ上/下" (stop positions) and the event that waits for lever ObjActs and animates the mover; the gate flag is the flag checked by every event of that lift (Central Yharnam lift: 12410330, set when the player first enters the bottom region).
* **Ladders**: `RegisterLadder(start_flag, stop_flag, obj)`; kick-down ladders have an event with a slot flag (e.g. 12410285/6).
* **Fog walls** (`霧壁`): passable; the wall segments inside their footprint are removed from the wall index.
* **Bridges / breakable walls / collision toggles**: listed (`bridge` kind, `EnableMapCollision` etc. seen in events) but **not modelled**.

### State flags: semantics and confidence

Flags are *candidates* with a kind, which value means "open", the evidence, and a confidence:

| kind | meaning | confidence |
|---|---|---|
| `slot` / `restore` | flag under which an event snaps the door to its open pose on load (`EndOfAnimation(door, open_anim)` inside `if flag` / `GotoIf` branch). For `Event_12410110`-style events the flag is the event's own slot flag (event id + slot, e.g. door 2410701 -> 12410112) | high |
| `objact` | the MSB ObjAct `flag` (`6xxxxxxx`) that the engine sets when the action is performed (scripts also clear it when a door re-closes) | medium |
| `unlock` | lift gate flag / flag opening the far side of a one-way door | medium |

Per area the numbers are in the report (e.g. Central Yharnam: 10 of 17 doors have a high-confidence flag; fog / static / dummy objects have none). The app should read all candidate flags of a gate and OR them. **Not validated against a game**: no save could be decoded (see below). Recommended calibration on device: open one door of each kind and watch which candidate flag flips (the app can already poll arbitrary flags).

The decompiled event scripts come from soulstruct 2.3.2 (`vanilla/*.evs.py`). They contain exactly the same event ids as the dump's `.emevd.dcx` (checked for 4 areas: 250/250, 246/246, 101/101, 160/160 events) but were not diffed instruction by instruction.

### Save file

`user/savedata/1/CUSA00207/SPRJ0005/userdata0000` is unencrypted: a table of (offset, size) sections, one of 150,001 bytes (= 1 + 1200 x 125) that looks like 1200 flag groups of 1000 bits (the runtime's `group of 1000 flags = 125 bytes, MSB first` layout; `src/runtime_atlas.c`), but the group-id -> pool-index map (a std::map in the engine) is not in the file, and the 8-digit map flags (12410xxx...) could not be located. So the "real mid-game save" scenario was **not** done; `mech` is the stand-in.

## Coverage results (details: `report.md`)

Lamp pairs connected (ordered), lamp->pickup, lamp->NPC, all 16 areas:

| scenario | lamp pairs | pickups | NPCs |
|---|---|---|---|
| all_open | 79/98 (81%) | 712/768 (93%) | 180/187 (96%) |
| mech | 75/98 (77%) | 695/768 (90%) | 159/187 (85%) |
| fresh | 65/98 (66%) | 601/768 (78%) | 152/187 (81%) |
| all_closed | 57/98 (58%) | 566/768 (74%) | 137/187 (73%) |

Route length / straight 3D line: median 1.2-2.3 per area (Central Yharnam 2.3, the winding one), p90 up to 3.8.
Remaining `all_open` failures (19 lamp pairs): Byrgenwerth / Lecture Building / Moonside Lake (10: 50 m and 85 m geometry gaps; these are separate nightmare / lake spaces reached by warp, not by walking), Upper Cathedral Ward x4 and Yahar'gul x3 (a 0.5 m collision wall seam between two floors, props such as o281052 next to the gap), Cainhurst x2 (9.5 m climb without any object nearby), plus pickups sitting on unreachable ledges. Missing collision decodes (generator `failures`: `h000020` in Hunter's Dream, `h007000` in m24_02, `h000029/254/255/7000` in m28) leave real holes in those areas.

### What each fix did (lamp pairs / pickups / NPCs, all_open unless noted)

| step | change | result |
|---|---|---|
| 0 | grid 0.5 m, step 0.6, slope 1.2, drop 4 m / 1.5 m, no ladders | Central Yharnam 4 lamps -> 3 of 4 connected, 17% largest walk component |
| 1 | stacked-floor drops (same cell) | tomb pocket found, **but** this let the route fall through floors: removed again in step 6 |
| 2 | step 0.6 -> 1.0, slope 3.0 | 51 -> 57 pairs, pickups 511 -> 583 (stairs sampled into ramps give 0.7-0.9 m risers) |
| 3 | drop 6 m / reach 2 m (was 4 / 1.5) | pickups +40 |
| 4 | ladder height fixed: FLVER header test (`b'FLVER'` vs 6 bytes) made every model without Havok data height-less; models with an empty mesh use "nearest floor above" | 44/48 -> 47/48 ladders linked, 51 -> 63 pairs together with lifts |
| 5 | lifts from stop regions / lever events (was one lift) | m25, m26, m24_00 lifts linked |
| 6 | **ledge links** (1.0-2.5 m, +8 cost) repair seams where the generator's re-tessellated floors turn stair treads into ramps | 63 -> 82 pairs, 676 -> 719 pickups (saturates at 2.5 m) |
| 7 | drops only from floor edges (no falling through a floor) | 82 -> 79 pairs, correct physics; `all_closed` Central Yharnam no longer escapes the clinic through a floor hole |
| 8 | door aperture: longer horizontal axis and +1 m margin per side (the old 0.15 m margin left bypasses beside the door; FLVER fallback was silently broken) | closed doors seal rooms |
| 9 | fog walls: passable, their wall plugs removed | |
| 10 | cell 0.35 / 0.75 / 1.0 instead of 0.5 | 80 / 79 / 74 pairs, pickups 708 / 672 / 659: 0.5 m is the knee |
| ? | wall removal experiment (`no_walls=1`) | 69 vs 63 pairs before the ledge fix: walls do false-block some stair rails; kept (they matter for routes) |

## Size and speed

Graph sizes: 29k-520k nodes and 0.12-2.3M undirected edges per area (4-ish edges per node). As float32/int32 CSR in memory about 35 MB for Central Yharnam, 55 MB for the biggest (Forbidden Woods).

Python A* (heapq, CSR, straight-line heuristic): 5 microseconds per node expansion; typical long routes expand 15k-300k nodes: 0.1-2.4 s (Central Yharnam median 1.0 s). `scipy` all-targets Dijkstra: 10-135 ms per area. In Java / ART on a phone with primitive arrays and a binary heap, 0.2-0.4 microseconds per expansion is realistic: **roughly 5-120 ms per long route**, 100-250 ms worst case, plus the graph decode.

Heuristic weakness: the straight-line heuristic is weak with drop / ladder penalties; expansions are 40-60% of the area for cross-map routes. Two cheap fixes if needed: weighted A* (1.5x) and a coarse region graph (below).

### Compact on-disk format for pack v2: `BNV1` (`pack.py`, round-trip verified: identical node and edge set, all 16 areas)

Edges are implicit in the grid, so only these are stored (all deflated): per-cell key deltas, quantised heights (2 cm) as zigzag deltas, **4 bytes per node** saying for the forward half of the 8-neighbourhood whether a link exists, to which stacked floor of the neighbour cell and whether it is a ledge, plus the drop links, the gated links (gate index and side), and a small JSON block with virtual nodes, ladder / lift / portal edges and the mechanism table (aperture, behaviour, candidate flags). Costs are recomputed on load from geometry.

Result: **7.8 MB for all 16 areas** (0.03-1.2 MB each, 7-21 bits per node; Central Yharnam 0.66 MB). The loader builds the CSR in well under a second per area. Layout documented in the `pack.py` docstring.

### Precomputing

* Not needed for correctness; the grid graph alone answers a route in ~0.1 s on a phone.
* Worth adding if latency matters: a **region graph**: strongly connected components of the walk graph per scenario-independent link set (only 2-487 regions of at least 50 nodes per area, 150-6000 region links). Route = Dijkstra on regions (microseconds), then A* restricted to the corridor of regions on that route; also gives an instant "unreachable under current flags" answer. Store region id per node (about 1 byte per node after deflate).
* Gated links are tagged with the gate index, so a change of event flags only flips a bit vector (`open[gate]`), no rebuild: region links through gates can be kept as a table `(region a, region b, gate, side)`.

## Limitations

* Floors and walls are the generator's 2D vector layers: stair treads are smoothed into ramps, walls are cut at one height per 3 m band (`anchor + 1.2 m`), so low rails or walls above the plane are missed / invented. Ledge links paper over the seams; they can also create implausible 1-2.5 m climbs.
* Prop objects with collision (crates, carts, barricades) and breakable walls are not in the data; closed doors only block where an aperture is derived from a door-class object.
* `static` doors (no entity id, no ObjAct) are treated as permanently shut in every non-open scenario; a few of them may just be decoration in an open frame.
* The operable side of every one-way door now comes from the action-button regions (see "Interaction regions"); the older `mech` / `fresh` scenarios (coverage numbers above) still treat ordinary doors as passable both ways, the `live` scenario and the app do not.
* Flag semantics need the on-device calibration described above; no save decoding.
* Lifts: stops from regions where present; five Research Hall / m24 lifts have partial or no stops.


## Pack version 2 and the app

`generate.sh` runs `pack.py` (graph + gate table per area, round trip checked) and `build_pack.py --nav`, which copies the
`.bnv` files to `maps/nav/<id>.bnv` and writes `maps/nav/<id>.gates.json` (format in build_pack.py's docstring). The app
loads a graph on a background thread when a destination needs it (two areas at most), searches with A* on primitive arrays
(about 0.3 us per expansion, 4-70 ms per route on this PC, `tests/android-nav/run.sh`) and applies the gate states live:

* a gate is **open** when any of its candidate flags reads 1 (unknown -1 counts as not set); with none set it is in its
  **default** state: ordinary doors and one-way doors whose operable side is known may be crossed *starting from that side*
  (the player opens them on the way), doors operable from both sides and fog walls are passable, lever doors, lifts and
  kick-down ladders may be crossed starting from the side of their lever (the route then carries a "pull the lever" mark),
  key doors (no inventory reading yet), story doors, special and static doors, lifts and ladders without a known lever are
  closed. When no route exists, a second search lets the route through key doors (from their side when known) and the app
  says "Route needs <key>".
* the card of a door says whether it can be opened from the player's side ("Can open from here", "Opens from the other
  side"), what key it needs, or that it is opened by events / does not open; it lists the candidate flags with their live values.

## Interaction regions (which side a door opens from)

The first prototype resolved the operable side only for the 3 one-way doors whose events grant SpEffect 4150 in a region;
`interact.py` now does it for every object with an ObjAct. An ObjAct event attaches an ObjActParam row; its
`actionButtonParamId` (e.g. 7000 "open a door") names an ActionButtonParam row (regionType, dummyPoly1, radius, angle,
height, baseHeightOffset; read from gameparam, layout from soulstruct's paramdef) whose region is placed at a dummy polygon of
the object's FLVER model (dummy table and bone transforms read from the objbnd) and opens along that dummy's forward
vector. A door has one such dummy per side (100 / 101, 120 / 121...), so one ObjAct = operable from one side, two ObjActs
(e.g. m26 "表" / "裏") = both. Ids that are not in this game's table (3500000...) fall back to the ObjAct's own dummy
(`objDummyId`, where the character is placed; its forward points at the object, so the region is mirrored). Lever doors keep
the side of their lever (event-derived), lifts and kick-down ladders carry their levers (positions and regions).

`sides_report.py` writes `OUT/sides.md`. Cross-checks (all 16 areas): the dummy the character snaps to (ObjActParam
`objDummyId`) lies on the same side of the door plane as the button region for 91 of 91 doors where the two dummies differ.
The prototype's event-derived grant points of the 3 one-way doors (circles of about 4 m around a point 2 m from the door, so
they cover both sides) are within 0.5-2.6 m of the button region's middle: they do not decide a side by themselves,
the button region does.

Per kind (doors: side determined of total): normal 55 of 55 (34 one side, 21 both), key 12 of 12, one-way 9 of 9, lever
8 of 8 (from the lever), special 1 of 1, scripted 0 of 10, static 0 of 6, unknown 0 of 17 (no ObjAct: opened by events or props);
lifts 20 of 26 have a lever near a stop, kick-down ladders 1 of 2; 58 of 72 levers have a region.
