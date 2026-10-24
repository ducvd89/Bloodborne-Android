# Code writers of the port, and how the recompiler handles each (phase 4, 2026-10-24)

Everything that changes the game's x86-64 code, at start-up or while the game runs, and what `bb-recomp` / the AOT runtime
(`cpu/guest_cpu_aot.cpp`) does with it. Addresses are image offsets (run-time address = image base + offset; the base is 0x800000000 on
Android). "Fixed" means the written bytes are known at build time; "computed" means they hold a run-time address (a stub page, a host
function) or a value from a setting. The exhaustive site lists are the catalogs in `tools/recomp/variants/` (generated, see the end).

How the three mechanisms apply (docs/RECOMP_ABI.md has the details):

| mechanism | for | what happens |
|---|---|---|
| **variant** | fixed byte patches, including new code (a cave) they jump to | `bb-recomp` lifts the touched function again from the patched bytes (every consistent subset of the groups that touch it) and exports it keyed by the hash of each unit's bytes. The runtime hashes a function whenever its bytes may have changed and switches to the matching variant, or back to the original when the bytes are original again. |
| **hook slot** | a callback at a known instruction (observers, hooks that edit registers, function-entry skips) | the units at that address test `bbr_hook_slot[k]`; `guest_cpu_hook` / `InstallGuestCodeHook` set the slot and write no code. |
| **fallback** | everything else that writes code | the function is dirty: it runs on the fallback CPU (FEX on ARM64), as before phase 4. |

## 1. Inventory

### 1.1 Start-up, before the first guest instruction (src/probe.c order)

Order of events: relocations (data pointers), `guest_cpu_code(image)`, the `--patches` BBPATCH2 files in order (`apply_patches`), the C++
patchers (`bbgpu_patch_image`: Gnm census, game menu, frame rate, display, guest guards), the fallback patch files (only when the frame-rate
hooks did not install), segment protections, `runtime_net_image_ready` (BBSC hooks and patches), cheats and Atlas threads. The AOT runtime
checks the image once before the first guest call, so every start-up write is in it by then.

| # | writer (source) | sites | size | bytes | applied | condition | handling |
|---|---|---|---|---|---|---|---|
| 1 | patch parts, effect switches (`scripts/patches.py`, `build_patch_parts.py`: effect_chromatic_aberration=0, effect_dof=0, effect_dynamic_shadows=0, effect_game_aa=0, effect_motion_blur=0, effect_ssao=0, effect_ssr=1) | 16 writes in 7 parts | 1-12 B | fixed | start-up (`--patches`) | one setting each (bbport.ini) | **variant** (alternatives share an `excl` set) |
| 2 | patch parts, skip_intro=1, debug_menu=1, debug_camera=1 | 3 + 10 + 569 writes | 1-256 B | fixed | start-up | settings, off by default | **variant** |
| 3 | patch parts, model_lod=-2/1/2 | 1 write each | 6 B | fixed | start-up | setting | **variant** (exclusive) |
| 4 | patch parts, res-scene.bin, graphics-heap.bin, gx-heap.bin | 66 + 2 + 2 writes | 1-4 B | fixed (a 1280x720 template) | start-up | res-scene when a render size is set, graphics-heap with a big scene, gx-heap by default | **variant**; the scene's two immediates are then rewritten by `PatchSceneSize` (row 12) |
| 5 | patch part fps=uncap.bin (`FPS_PRESETS['uncap']`) | 172 writes | 1-9 B | fixed | start-up, as `--fallback-patches` (only without the frame-rate hooks) or `--patches` with `BB_FRAME_HOOKS=0` | rare | **variant** |
| 6 | patch part skip_network_choice=1.bin | 1 write | 46 B | fixed | start-up | setting | **variant** |
| 7 | patch part cheats.bin (`cheat_writes`) | 1 jmp (0x18f78b1, 6 B) + 4 call rel32 (0x18f983d, 0x18fa58c, 0x18fa0b4, 0x18fa0cf) + cave 0x50d9700 (32 B) + stub 0x50d9740 (20 B) | | fixed; the cave tests two flag bytes (0x56d3f00/1) at run time | start-up, always on Android; the flags are data, written live by `cheat_poller` | always | **variant** (the cave is new code: a unit of the variant of the patched function; the cheats themselves switch with data, no code write) |
| 8 | frame rate hooks (`gpu/shim/bbport_frame_rate.cpp`): 6 detours (0x203493d 8 B, 0x2034d96 9 B, 0x2018e36 7 B, 0x17f9b71 7 B, 0x1572a7d 7 B, 0x1514c24 8 B) | 6 | 7-9 B | **computed**: `jmp rel32` to a stub on a page mapped at run time; the stubs hold absolute addresses of the `live` struct and of FrameStep's guest-callable stub | start-up | `BB_FPS` above 30 (the default: uncap) and `BB_FRAME_HOOKS` not 0 | **fallback** (see 3) |
| 9 | frame rate hooks: 48 operand redirects (`Redirects[]`: 24 dt, 16 cloth, step_down 2, step_up 2, approach30, approach10, decay95, decay08), the rel32 of a `[rip+disp]` VEX operand | 48 | 4 B | **computed**: the disp points into the run-time `live` page, whose fields `FrameStep` rewrites every frame | start-up | as row 8 | **fallback** (see 3) |
| 10 | display hooks (`bbport_display.cpp`): 5 camera, 3 marker, 1 viewport, 1 HUD detours (0x1436c2c 0x1436d8a 0x1436e68 0x1436eec 0x1436f4b; 0x1644379 0x1644c45 0x16452b7; 0x1f510; 0x1bf9e10) | 10 | 5-16 B | **computed**: stubs on a run-time page with absolute addresses of a data block (aspect values) and of HudFrame's host stub | start-up | output aspect other than 16:9 | **fallback** |
| 11 | display: viewport aspect immediate (inside the stub of 0x1f510) | 1 | 4 B | computed from the aspect | start-up | as row 10 | fallback |
| 12 | display `PatchSceneSize`: the two immediates at 0x1d96a6b+1 and 0x1d96a7a+1 (the scene's width and height) | 2 | 3 B each | **computed** from `BB_RENDER_RES` | start-up | `BB_RENDER_RES` set (the Android launcher sets it with a render resolution) | **fallback** (a run-time value; see 3) |
| 13 | guest guards (`bbport_guest_guards.cpp`): 2 detours (0xbbc3e0 sample clamp, 0x2199310 null node) + 3 call retargets (0xebbc86, 0xebbca4, 0xebbcc2) | 5 | 4-6 B | **computed** (stub page, host function addresses) | start-up | 0x2199310 always; clamp and fx-clear by env | **fallback** |
| 14 | game menu hooks (`bbport_game_menu.cpp`): 2 detours (0xf6f340, 0x1b4e2a0) | 2 | 6-10 B | computed (thunks) | start-up | x86-64 hosts only; not used on Android | not applicable on ARM64 |
| 15 | Gnm census (`bbport_gnm_hooks.cpp`, table `bbport_gnm_table.inc`): up to 513 function entries (500 libGnm + 13 ConstantCommandBuffer methods), 336 observers | 513 | 5-11 B | computed (slots on a run-time page) | start-up | `BB_GNM_CENSUS=1` / `BB_GNM_OBSERVE=1` (diagnostics, off by default) | not handled (diagnostics) |
| 16 | guest hooks (`bbport_guest_hooks.cpp`, `guest_cpu_hook`): 6 sites by default (0x216d5ea and 0x2171fea memcpy calls, 0x26aa255 allocator return, 0x11e6629 0x11e6736 0x11e6846 heap returns), 8 with `BB_FRAME_STATS` (0x26aa860) and `BB_HEAP_SITES` (0xbb5fb0) | 6-8 | 2-5 B | the handler is host code; `guest_cpu_hook` writes `syscall` (FEX) | GPU init (BufferCache), after the start-up patches | in-place guest memory without UMA (not the Android default); the extra two by env | **hook slot** (kind `regs`, resume points declared) |
| 17 | free check (`bbport_free_check.cpp`): 2 int3 (0x263b9b0, 0x26aa986) | 2 | 1 B | fixed | lazily at the first trap-mode call | `BB_FREE_CHECK=1`, x86-64 hosts only | not handled (diagnostics, x86-64 only) |
| 18 | BBSC seamless patches (`bloodborne_re.cpp`, `InstallSeamlessCoopPatches`): 13 literal patches | 13 | 2-16 B | fixed | start-up of the online module | `BB_BBSC=1` and `SHADPS4_BLOODBORNE_SEAMLESS_COOP` (default on); never reverted | **variant** (`bbsc:seamless`) |
| 19 | BBSC persistence tables (`bbsc_patch_tables.h`): 27 call retargets, 3 entry returns, 17 byte edits, 11 wide edits, 1 entry jump = 59 entries in 17 groups; 53 written at install with the default switches | 59 | 1-5 B | fixed (rel32 between image offsets; `PersistNoOpOffset` 0x013001E0 and `ReturnZeroOffset` 0x01300AA0 are inside the image) | install (all enabled groups but Lamp); **toggled at run time** | group switches (`SHADPS4_BBSC_*`), 31 on by default | **variant** (one group per table group, `bbsc:<Group>`) |
| 20 | BBSC code hooks (`InstallGuestCodeHook`): 5 seamless + 154 trace loop (table `Sites`) + 34 individual = 193 attempted, 190 install on the pristine eboot (refused: 0x0132E010 not whole instructions, 0x0154AFC0 relative addressing, 0x0157F6D1 changed by seamless patches 3/4); 12 use `skip_flag` | 190 | 5-16 B | **computed**: `jmp rel32` to a trampoline in a run-time `mmap` area, absolute host addresses (hook, stats, host-function stub, skip flag) | start-up of the online module, after the BBSC patches; never removed | `BB_BBSC=1`, switches decide 34 of them | **hook slot** (kind `snap`, `skip` on 12) |
| 21 | heap sites (`bbport_heap_sites.cpp`): 6 function pointer slots of libc.prx's malloc table (0x579a190..0x579a1b8) | 6 | 8 B each | computed (host addresses) | run time, once (`WatchOverlapLeak`) or `BB_HEAP_SITES=1` | counters on by default with `BB_SUBMIT_AHEAD` | data, not code: no recompiler involvement (the wrappers need a host-call stub, an existing runtime concern) |

### 1.2 While the game runs

| writer | sites | size | bytes | when | handling |
|---|---|---|---|---|---|
| BBSC session patches (`SetPersistentSessionPatches`): Stage 8, Revive 2, Boss 2, MatchCheck 2, Tasks 1 entries | 13 default (15 with Boss) | 5 B | fixed, original and patched | suspended on quit to title, Silencing Blank, host/invader death, guest sent home, host gone 180 s; re-applied 20 s later (`TickBbscSessionEnd`); written one entry at a time, each followed by `GuestCodeWritten` | **variant**; the runtime re-classifies the function after every write: the original when the bytes are original again |
| BBSC role patches (`SetHostLampPatches`): Lamp 3 entries | 3 | 1-5 B | fixed | by the lamp role tick (`TickBbscLampRole`) | **variant** |
| cheats: data flag bytes 0x56d3f00/1 | 2 | 1 B | data | `cheat_poller` every 250 ms | no code written; nothing to do |
| BBSC hooks | none after install | | | | |
| Hunter's Atlas (`runtime_atlas.c`), `runtime_pad.c`, `runtime_kernel.c`, THD (`thd_session.cpp`: one data byte at 0x546cc09) | 0 code writes | | | | read-only observers / data |

### 1.3 bb-prepare (not at run time)

`bb-prepare` (`tools/android/prepare`, the image linker `scripts/link_modules.py`) writes the memory image the runtime maps and `bb-recomp`
lifts (`out/boot-linked.bin`): it rewrites `fs:` to `gs:` (byte 0x65 for 0x64), which is why the unit hashes treat 0x65 as 0x64, links
`libc.prx` and `libSceFios2.prx`, and stores the relocations. It applies no game patches; the BBPATCH2 parts are applied by the loader
at start (row 1-7). The `tools/android/build_patch_parts.py` output is the data of rows 1-7.

### 1.4 Counts per kind

| kind | sites | mechanism |
|---|---|---|
| Fixed byte patches, start-up (patch parts) | 851 writes in 19 parts (rows 1-7; 2 are cave writes) | variant |
| Fixed byte patches, BBSC | 13 seamless + 59 table entries (17 groups, 13+3 toggled at run time) | variant |
| Native observer / replacement hooks | 190 BBSC (`snap`) + 6-8 shim (`regs`) | hook slot |
| Computed detours and redirects, default Android configuration | 6 + 48 (frame rate) + 5 (guards) = 59, plus 2 scene immediates when a render size is set | fallback |
| Computed, optional / diagnostic | 10 display (aspect other than 16:9), 2 game menu (x86-64), 513 Gnm census, 2 free check | not handled |
| Data writes | 6 malloc table slots, 2 cheat flag bytes, 1 THD byte | not code |

## 2. What is native now

* Everything in rows 1-7, 18, 19 and the run-time toggles of 19: the patched function runs as a precompiled variant, the original when the bytes are
  original. The hash check is per function, so the transient state while a group's sites are written one at a time is "dirty" (fallback) for a
  moment and then a variant.
* Rows 16 and 20: hooks fire from the recompiled units; no trampoline, no code write, the function stays native.

## 3. What still needs the fallback, and why

* **Rows 8, 9, 13 (frame rate, guards), 10 and 12 (display)** write run-time addresses into the code (a stub page, the `live` struct, host-function
  stubs, a resolution). A variant cannot hold them. They make about 35 functions dirty in the default Android configuration (the frame-rate
  redirects touch the main loop, the pacer, cloth, physics and camera code). Follow-ups, in order of value:
  1. Operand redirect slots: the lowering loads the address of a `[rip+disp]` operand from a slot (`bbr_operand_slot[k]`, null = the original constant); the 48 redirects become 48
     slot values and the functions stay native. `live` is a plain data struct.
  2. The six frame-rate detours and the two guards as `regs` hook slots with callbacks that do the stub's work in C++ (a few lines each: the stubs only set a few
     fields and run the displaced instruction; FrameStep is already a host function).
  3. The scene immediates: a value slot like 1 (`mov eax, imm32` reads the width and height from `bbr_value_slot`).
  4. The display stubs likewise, or a fixed layout (stubs and data at deterministic addresses in the image's page padding, so the patched bytes are fixed and the stubs are new code
     like the cheats' cave).
* A function that is touched by an unknown write (any `guest_cpu_invalidate` on a range no catalog describes, or bytes the catalog does not combine) is dirty
  and runs on the fallback; the table of variants has every subset of the groups that touch one function (single groups and all of them when a function
  has more than six).
* A hook at a site that is not in the hook table (`bbr_hook_sites`), or whose function is dirty, keeps the old trampoline / `syscall` path (and makes
  its function dirty).
* Hooks stop firing while their function runs on the fallback (another write made it dirty without a variant); the runtime says so once per site
  (`AOT: the hook at +X is inactive`).

## 3b. Measured (whole image, aarch64, -O1, `--verify-llvm`, all four catalogs)

* 37 groups (35 touch code), **117 variants** built (0 skipped): 169 units, 48 of them only in the variant (the cheats' cave pieces and the entries a jump back into a function needs), 0.43 MB of code,
  3.8 s of the compile. 68 patch-part writes lie outside every function (floats and other data). **198 hook sites** declared (190 BBSC + 8 shim), all 198 compiled into at least one unit.
* The whole compile: 76.6 s (62 s without the variants and hook slots, which add forced `GuestOpcode` markers and one slot test per site), 14.5 GB peak, `.so` 124.5 MB.
* `tests/aot_so_check.cpp` (dev check, needs the game's prepared image): the real x86-64 `.so` with the real image and BBPATCH2 parts applied as the loader does: cheats + res-scene + gx-heap + skip_network_choice = 17 functions,
  all matched to a variant, 0 dirty; with every BBSC group applied on top: 56 variants, 0 dirty (functions touched by several groups included).
* Known, unrelated: `bb-recomp --func <vaddr>` (one function) fails intermittently with a glibc heap error at exit (about 8 % of runs, also on the `testing` tool before these changes); the whole-image compile does not.

## 4. The catalogs and their generators

| file | generator | source | needs |
|---|---|---|---|
| `tools/recomp/variants/patch-parts.txt` | `gen_patch_parts.py` | `tools/android/build_patch_parts.py` (all patch parts, cheats) | `out/eboot.elf` |
| `tools/recomp/variants/bbsc-patches.txt` | `gen_bbsc.py` (+ `gen_bbsc_tables.cpp`) | `gpu/bbnet20/bbsc/core/bbsc_patch_tables.h` (compiled and run), constants of `bloodborne_re.cpp` (the 13 seamless patches) | a C++ compiler |
| `tools/recomp/variants/hooks-bbsc.txt` | `gen_bbsc.py` | the sites `InstallGuestCodeHook` is called for, from `tools/android/dev/bbsc_sites.sh` (BBSC's own install code on the pristine eboot) | `out/eboot.elf`, the tool's build or `--sites-tool` |
| `tools/recomp/variants/hooks-shim.txt` | `gen_hooks_shim.py` | constants of `gpu/shim/bbport_guest_hooks.cpp` | nothing |

`gen_patch_parts.py --check <file>`, `gen_bbsc.py --check`, `gen_hooks_shim.py --check <file>` exit 1 when a catalog is out of date: run them when a
patch table, a patch part or a hook site changes.
