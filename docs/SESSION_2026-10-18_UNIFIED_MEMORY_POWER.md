# Session log 2026-10-18/19: unified memory v2, staging, Plan A, CPU power

Handoff for the next session. Branch `testing` = local `bionic-ndk` in `~/BloodbornePC-bionic`
(HEAD `3bd4e52`, pushed). Design and switch reference: `docs/ANDROID_UNIFIED_MEMORY_PLAN.md`.
Test devices: AYN Thor `70538e86` (Adreno 740, the main test phone), Oppo Find N5 `53a262ea`
(Adreno 830).

## Commits (all on origin/testing)

| Commit | What |
|---|---|
| `bc6b50b` | Unified memory v2 setup (BB_UMA): lazy granules, probes, classes, verify, switch |
| `150e277` | Device fixes: unshared memory is exact copy mode; shared memory only through per-granule VkBuffers (never sparse-bound); one-shot dynamic-block traps; verdict memo |
| `947f0ea` | Staging ring: reuse the current block else the lowest free one, so idle blocks trim |
| `e99b7f8` | App: crash-safe settings/state writes (temp + fsync + rename); NUL-filled files read as missing |
| `00c8a58` | Runtime: report faults inside the fault handler and stack overflows (was silent exit 139) |
| `ece0f2a` | GPU threads: short spin then futex block instead of yield-spinning; `BB_GPUCOMM_CPUS` pin |
| `3bd4e52` | gpu_modified_ranges race fix (LockedRangeSet), faster dynamic-ring flagging, arena-by-class stats, Plan A (`BB_TEX_HOME`, off) |

## What we learned

- **v1 failures** (BB_UNIFIED): up-front commit of all direct memory; copies became live GPU reads;
  a different addressing model. Coherence was NOT the cause: KGSL imports dma-bufs IO-coherent by
  default (`UMA KGSL: import flags=0 -> iocoherent=1`).
- **Never sparse-bind dma-buf memory on KGSL**: binding shared granules into the 4 GiB arena VBO
  hard-reset the Thor (run 4). Ordinary per-granule VkBuffers are fine.
- **The AMD "arena in place" path changes copy-mode behaviour for unshared memory** (small constants
  get 64 KiB arena copies: 2.3 GB VRAM copies, driver segfault). Under BB_UMA it is off; only shared
  blocks are special.
- **Hard resets wipe bbport.ini** (NUL bytes) -> fixed in the app (`e99b7f8`, not yet in an APK on
  the phones). Until then: keep test switches in `files/bbport/bionic-local.sh`, not in settings.
- **Staging** never trimmed because the ring rotated through every block; trim needs ~3000 frames
  (~2 min at 24 FPS; the counter is per frame). `BB_STAGING_KEEP_MB=32` showed no stutter moving.
- **32 MiB granules** crashed via a gpu_modified_ranges race in ReadMemory (twins block) found by the
  new nested-fault report; fixed. User: "don't sweat for several MB" -> 8 MiB granules stay.
- **Which classes matter** ("Arena by class"): vertex 202 MiB (182 CPU pages), texel 95 (75 GPU-only),
  constant 0, storage 0 -> sharing constant/storage saves nothing; only vertex left (cross-granule).
- **Potatoborne mod removes corpses** (not a bug).
- **CPU**: shadPS4:GpuComm burned ~86% of the prime core in sched_yield (DrawPipe::Drain); real work
  ~12%. All clusters near max clock.

## Measured (Thor, usual Yharnam spot, standing)

| Config | Total PSS | FPS | CPU |
|---|---|---|---|
| Copy mode (run 9) | 5382 MB | 24 | |
| UMA v2 index+vertex + staging fix (run 13) | 4944 MB | 24 | |
| UMA (index,vertex,constant) + blocking waits + pin, 60 cap, Plan A off | 5308 MB | 28.0-28.2 | |
| same, Plan A on | 5210 MB | 27.5-27.9 | |
| Spinning waits, 60 cap (B60) | | 27.4-27.9 | 342% of one core |
| Blocking waits + GpuComm pin, 60 cap (D3) | | 27.8-28.0 | 219% of one core |

The ~5.3 GB of the later runs vs 4.94 GB of run 13 is not explained yet (different settings: 60
cap, constant class, verify on, more images loaded); re-measure at the 30 cap before calling it a
regression. The Thor is GPU-bound at this spot (~28 FPS even uncapped).

## Device state now

- **Thor**: APK `Bloodborne-uma-bc6b50b` (0.11.0) + pushed runtime: `bb-probe` and `libbbgpu.so` from
  the full build at `3bd4e52` (stripped with `llvm-strip --strip-debug`). `files/bbport/bionic-local.sh`
  = `tools/android/dev/bionic-local.thor-example.sh` (UMA v2 index,vertex,constant, 8 MiB, FIFO cap 128,
  staging keep 32, verify 64, frame stats, `BB_GPUCOMM_CPUS=7`, Plan A off). **bbport.ini has
  `fps_limit=60`** (set for the A/B; set back in the app if wanted). Driver: Turnip Gen8 V37 (StevenMXZ).
- **N5**: APK `Bloodborne-uma-bc6b50b` + run 16 libbbgpu and a run-16 `bionic-local.sh`. It overheated
  at a 60 cap (thermal critical, 40 -> 6-9 FPS); started warm. Not re-tested since.
- Deploying a library to a running game: copy to a temp name and `mv` over it (copying over a mapped
  .so corrupts the running process). `bb-probe` cannot be replaced while the game runs.

## Tools (tools/android/dev/)

- `thread_profile.py SERIAL SECONDS OUT.json`: per-thread CPU % and core class split (run-as, no root).
- `dmesg_poll.py OUT`: kernel log capture that survives a hard reset (polls `dmesg | tail -400`).
- `gamelog_poll.sh OUT`: copies the game log every 0.5 s (survives log rotation).
- `simpleperf` works via `adb shell run-as com.ducvd89.bloodborne simpleperf record -t TID -e task-clock ...`
  (cpu-clock is refused); symbolize with `out/bionic/gpu/libbbgpu.so` (same Build ID as the pushed one).

## In flight / next

1. **Plan A refusals: built, UNCOMMITTED, not tested on a device.** In the tree on top of 3bd4e52
   (texture_cache/bbport_tex_home.cpp, bbport_tex_home.h, texture_cache.h, tile_manager.*,
   image_info.*, buffer_cache.cpp UmaDirect, bbport_uma.h, driver.cpp, the plan doc):
   - verification re-tiles twice (padding 0x00/output 0x00 and padding 0xff/output 0x5a): texels vs
     padding vs holes. Holes or texel differences: refused for good. Padding-only differences: the
     game's padding bytes are kept aside and restored on every rebuild (max 1/4 of the dropped pages).
     `BB_TEX_HOME_LOG=1` prints "Texture home diff: ..." per differing image (first 60).
   - `BB_TEX_HOME_MIN_KB` (default 64, was 256 KiB fixed).
   - `BB_TEX_HOME_FIRST=1` (+ `BB_TEX_HOME_FIRST_PCT`, 50): UMA does not promote granules mostly
     covered by sampled textures; counted as `texture-first` in the UMA line.
   Deployed to the Thor (libbbgpu only; bionic-local.sh has BB_TEX_HOME=1 BB_TEX_HOME_LOG=1).
   Round 2 on the Thor (usual spot): 170 textures homed, 101 MiB freed, 0 verify failures (60 homed with
   padding kept aside, 9 KiB), 0 rebuilds, FPS 27.5-27.8, PSS 5200 MiB vs 5308 baseline. Still refused:
   shared 1604 (UMA granules), small 2145 (<64 KiB), usage 790.
   Texture-first (BB_TEX_HOME_FIRST=1): no effect, its counter stayed 0 (granules are promoted while
   loading, before their textures are registered); 167 homed / 99 MiB, PSS 5187 MiB, 2 rebuilds 10.7 ms.
   Decision: stop Plan A here (~-100 MB, stable); making texture-first work needs un-sharing granules later.
   Next was: run at the usual spot ~2.5 min, read "Texture home" + "Texture home diff" lines and PSS;
   then the same with BB_TEX_HOME_FIRST=1; compare with the Plan A-off baseline (5308 MB). Commit
   if clean.
2. **Power plan** (only steps 1-2 done): light threads (FMOD, loaders, EzWorkPool, present) to the
   little cores; ADPF session only for truly frame-critical threads and honest work durations;
   verify off for normal play; 30 FPS cap on the N5; measure watts unplugged (wireless adb).
   Consider making `BB_GPUCOMM_CPUS` default per device (Thor: 7).
3. Not started: 8 GB-phone simulation (memory balloon over adb), user said "not now".
4. Defaults: none of the new things is on by default except blocking waits (`BB_SPIN_WAITS=1` reverts)
   and the staging ring order. BB_UMA, the pin, Plan A, staging keep 32 and the FIFO cap 128 are
   test settings in bionic-local.sh only.
