# AOT on the AYN Thor: status (branch recomp-rt, 2026-10-24)

The game's x86-64 code runs as ahead-of-time units (bb-recomp: FEX front end -> LLVM -> AArch64 `.so`) on the Thor
(Snapdragon 8 Gen 2), with FEXCore as the fallback on the same CPU state (cpu/guest_cpu_aot.cpp, docs/RECOMP_ABI.md).

## Paused at (2026-10-24, second pause)

**Done since the final report:** nothing merged or released; device back on the **stock 1.2.0 runtime** (`aot_device.sh off`,
user's `bionic-local.sh` and `bbport.ini` restored, md5 checked).

**In progress: open issue 2 (phase 4b frame-rate patcher in slot mode stops the game, AOT only).** Findings so far:
- It is not a hang in a wait: the process main thread (which runs the game's main loop) **spins at 100 % CPU** inside units
  (watchdog dump: in the unit of +0x21b8140, called from +0x2195f7c on the resolution-setup path +0x1d96bf3); the render thread and
  workers wait for it. Its AOT trace (SIGUSR2 dump, now printed per thread) repeats one cycle of host calls (bb-probe+0x2e07c,
  +0x2df64, +0x411e8, +0x4946c, +0x44764/+0x44788, +0x40c34..+0x40d1c, +0x48c50..+0x49da4) returning to guest +0x208b166,
  +0x262d9b2, +0x20368d3, +0x207f41d, +0x207c407, +0x20e4318, +0x2804b43, +0x208400a.., +0x263b374..: the main loop keeps
  iterating without making progress (symbolize those offsets against out/bionic/bin/bb-probe, unstripped).
- **Not** the callbacks, the redirects, the Live struct's place, the byte patches or the slots themselves: with
  `BB_FRAME_SLOT_TEST=63` (pacer/step/fixed-step callbacks do nothing, no redirects, no byte patches, no slots set) and with
  `BB_FRAME_LIVE_NEAR=1` (Live in its own page) it still hangs; `BB_FRAME_HOOKS=0` hangs too.
- **Not** a miscompiled unit of the 54 frame-rate functions: with all of them on FEX (`BB_AOT_FALLBACK_RANGES`) it still hangs.
- The only working combination so far: the patcher in **detour mode** (`BB_AOT_NO_SLOTS=1572a7d`, or the release libbbgpu): it
  allocates a stub page near the code (`guest_cpu_code`) and its detours put ~32 functions on FEX. So the difference is in what
  detour mode does besides the patches (memory layout, the stub page registered as guest code, timing), or in another
  libbbgpu (phase 4b shim) path that slot mode/hooks-off take: compare the shim's guest-code hooks (bbport_guest_code_hooks.cpp)
  and display slots between the two modes next.

**Next step:** symbolize the main thread's host-call cycle (which runtime calls it loops on: likely a wait/poll the game repeats
because a condition never becomes true), then diff the shim's behaviour between detour and slot mode. Then open issue 3
(flip IRQ assertion) by building the branch runtime with the phase 4b shim reverted.

## Result

- **Title screen, main menu, save loaded (Continue), gameplay**: all in AOT mode. A 13-minute session at the save's spot
  (walking, camera turns, attacks, rolls; 32 rounds of input, after the Continue loading screen) ran without a crash, mean 33.1 FPS.
- **Units cover the game.** Over the session: 2.80 G unit entries through the dispatcher (direct unit-to-unit calls not
  counted), 208 k misses = **0.0074 %**. Per thread, fallback (FEX) time averaged over 1-minute windows:

  | thread | fallback | unit entries | misses | what the misses are |
  |---|---|---|---|---|
  | GXWorker:1-5 | 0.00 % | 1 627 M | 68 | a patched unit (+0xee5920) |
  | GXRenderThread | 0.08 % | 358 M | 96 180 | patched units (frame-rate detours of the release GPU library: +0x12a43b0 ...) |
  | CSChrThread0-5 | 0.05 % | 291 M | 85 363 | +0x210ebc0 (a jump into a loop head the units do not cover; CFI-bounded) |
  | HavokWorkerThread x5 | 0.00 % | 208 M | 0 | |
  | EzWorkPool_Type | 0.00 % | 186 M | 746 | +0x23a9c9c (unresolved switch case; CFI-bounded) |
  | MOMainThread | 0.00 % | 77 M | 0 | |
  | FMOD mixer | 0.00 % | 31 M | 0 | |
  | all other guest threads | 0.00 % | ~25 M | 0 | |
  | bb-probe (process main thread) | in one FEX run | 0.06 M | 25 745 | entered the game at +0x20 (start stub, no unit) and stays in that run; its CPU time is the same with FEX (46 %): host work |

  A FEX-free runtime therefore needs only: the start stub (+0x20), the two jump targets above, and the patched functions
  (the frame-rate patcher's detours; with phase 4b's slots they become native, see open issue 2).
- **FPS and CPU (same spot, same settings, 60 s each, `tools/android/dev/thread_profile.py`).** The Thor is GPU-bound here,
  so AOT and FEX give the same FPS and the same total CPU:

  | | FPS (mean of 12 frame-stat samples) | process CPU (% of one core) | guest threads | FMOD mixer | GXWorker x5 | GXRenderThread |
  |---|---|---|---|---|---|---|
  | capped (user settings: limit 60, LSFG x2, GSR 2) AOT | 29.6 | 274 % | 93.8 % | 10.2 % | 41.2 % | 14.5 % |
  | capped FEX | 29.5 | 277 % | 91.5 % | 14.2 % | 35.3 % | 14.4 % |
  | uncapped (limit 0, LSFG off) AOT | 36.6 | 383 % | 136.6 % | 12.7 % | 59.9 % | 21.0 % |
  | uncapped FEX | 37.2 | 384 % | 131.4 % | 18.8 % | 51.2 % | 20.1 % |

  Compute-bound guest code is faster as units (FMOD mixer: -28 % / -32 % CPU). The GX workers spend more: they wait for work
  in a spin loop that runs faster per iteration (more spins in the same wall time), which hides any gain there. The host
  threads (bb:DrawRec, shadPS4:GpuComm, bb:VkRecorder, the bb-probe main thread) dominate and are the same in both modes.
  These runs used the release libbbgpu with this branch's libbbcpu/bb-probe and a .so from before the phase 4b merge.

## Bugs found and fixed on the device (commits on recomp-rt)

| commit | bug |
|---|---|
| fc5c2ba | unaligned x86 `lock` operations: LSE atomics in units fault across 16 bytes (SIGBUS); emulated with FEX's non-JIT unaligned handler. `BB_AOT=0` switches AOT off |
| e673413 | the .so did not load: Android's linker does not resolve a dlopen()ed library's symbols against the executable's DT_NEEDED libraries (`cannot locate symbol "bbr_dispatch"`); the .so now has DT_NEEDED libbbcpu.so. Units with unsupported ops start dirty |
| bd4c1e5 | merged tool ABI: bbr_dirty_entry/bbr_dirty[] instead of patching the .so's text (no execmod on Android), exact unit ranges. **Fallback runs entered in the middle of a function had RSP 16 bytes off** (FEX's HandleCallback pushes its trampoline): crash at +0x52d6ae8 after a tail jump to +0xd81376; a stub restores RSP |
| af5b56c | a jump into the middle of a function ran FEX to the end of the guest stack (a thread's whole loop for good: MOMainThread, GXRenderThread, Havok, CSChr): now FEX runs only to the function's return, found from the image's `.eh_frame` CFI (bbr_eh_frame_hdr). The lifter also makes in-function exits it left out unit entries (432 -> 2144) |
| adeede7, 6bfe353 | after a longjmp (Havok workers: libc's longjmp `ret`s to a setjmp return site) every unit unwound to the thread's root and the rest of the loop ran on FEX (99 % fallback); units now tail-call `bbr_unwound`, which runs the unit at the new rip from the current host frame (nesting capped at 16) |
| 5f909b6 | switch tables in units entered inside a loop (base `lea` before the loop): reuse the table the function's entry unit resolved (MOMainThread +0x27f224d, GXRenderThread +0x217608c); 57 -> 45 unresolved |
| 88d9f1a | A/B switches: `BB_AOT_VARIANTS=0`, `BB_AOT_HOOK_SLOTS=0`, `BB_AOT_NO_SLOTS=hex,..|all`, `BB_AOT_FALLBACK_RANGES=lo-hi,..`, `BB_AOT_CFI=0`; per-thread statistics (fallback/host time, misses) |

Merged: `testing` steps 2c and 2d (boot-linked image incl. libc.prx and libSceFios2.prx, jump tables, landing pads, x87
lowering) and `recomp-hooks` phase 4b (patch variants, native hook slots). The tool is built with
`cmake -S tools/recomp -B out/recomp-build -G Ninja -DFEX_SOURCE_DIR=/home/cid/BloodbornePC-recomp/out/recomp-fex`.

## Open issues

1. **x87 hang: not reproducible, no fix.** Before the power outage about 1 launch in 3 stopped before the title (all threads
   parked in host waits); it followed the step 2d merge, and keeping the 127 x87-helper units on FEX avoided it (6/6).
   Bisection ended at libc.prx's `fabsl` (+0x571cc10: unit 65 alone on FEX, 6/6 clean), but the control (unit 66 alone on FEX)
   was also 6/6 clean, the baseline after the device's restart was 8/8 clean, and 30+ launches since never stalled. The
   lowered IR of `fabsl` matches FEX's reduced-precision path (F80 load + bbr_x87_f80_to_f64, x87 TOP/tag updates). If it comes
   back: `BB_AOT_FALLBACK_RANGES=571cc10-571cc11`, and a watchdog dump (kill -ALRM bb-probe) of the stall.
2. **Phase 4b: the frame-rate patcher in slot mode stops the game before the title** (deterministic, AOT only; 4/4). Bisected
   to the frame-rate patcher's slots (0x203493d pacer mode, 0x2018e36 step, 0x1572a7d / 0x17f9b71 fixed steps) and its Live
   struct in the image (+0x56d3f40). Any one of them without a slot (`BB_AOT_NO_SLOTS=1572a7d`) makes the patcher use its
   old detours, and the game runs (then 32 patched functions are on the fallback). The display slots (scene size
   +0x1d96a6b/+0x1d96a7a) work ("the game's scene size 960x540 (native hook slots)"); patch variants work (45 patched functions
   native, 0 dirty entries). Phase 4b was checked on synthetic tables (aot_so_check), not in the running game; it needs the
   patcher's owner. Online (BBSC) hooks were not tested on the device.
3. **"Out of order flip IRQ" GPU assertion** (shadPS4 video_out.cpp:357) at start, about 1 launch in 5-10 since the merges, in
   AOT mode *and* in FEX mode with this branch's libbbcpu (2/4 with the branch GPU library), never with the stock 1.2.0
   runtime (5/5, and ~40 launches before the merges). A race between the flip IRQ and the buffer labels: not an AOT
   correctness bug, but it blocks "10 clean launches". With the release libbbgpu and the branch libbbcpu: 1 in 3.
4. Remaining misses: +0x210ebc0 (CSChr: a jump into a loop head from code the lifter does not resolve) and +0x23a9c9c
   (EzWorkPool: unresolved switch); both cost a CFI-bounded FEX run, ~0.05 % of the thread. The process main thread stays in
   the FEX run of the start stub (+0x20).
5. ASM tests (`bb-recomp-asmtest`, x86-64 host): the run here reports 177 failures, 158 of them the 80-bit X87 suite, plus
   xsave/MMX/x87-state tests; the `testing` and `recomp-hooks` builds fail the same tests in this harness setup, so they are
   not from this branch's changes. The resume, hook-slot and variant self-tests pass. No longjmp-shaped regression test for
   bbr_unwound was written yet.

## Device state (left)

- App: **1.2.0 release** (installed over 1.1.0 with `adb install -r`, same key; saves kept; backups in `out/thor-backup/`).
- Runtime: **stock 1.2.0, FEX JIT only**: bb-probe, libbbcpu.so, libbbgpu.so, libbbnet20.so and run-bionic.sh are the
  release's (md5 checked against `out/release-1.2.0/runtime`); `bionic-local.sh` is the user's original
  (`bionic-local.sh.pre-aot`); `bbport.ini` is the user's original (something rewrote `preset` and `lsfg_multiplier` to 3/0
  during the tests; restored to 4/2). Checked with one launch. Reason for leaving FEX on: open issues 2 and 3, AOT was not
  stable over 10 launches with the merged runtime.
- `files/bbport/aot/libbbaot.so` (the units, 133 MB) stays; the release run-bionic.sh ignores it.

## Switching AOT on or off

    bash tools/android/dev/aot_device.sh on      # this branch's libbbcpu + bb-probe + run-bionic.sh + out/aot/libbbaot.so (release libbbgpu)
    bash tools/android/dev/aot_device.sh on-gpu  # the same with this branch's libbbgpu/libbbnet20 (phase 4b shim; see open issues 2-3)
    bash tools/android/dev/aot_device.sh off     # the release runtime back
    bash tools/android/dev/aot_device.sh status

With the branch runtime installed, `export BB_AOT=0` in `files/bbport/bionic-local.sh` keeps FEX alone; with the branch GPU
library add `export BB_AOT_NO_SLOTS=1572a7d` (open issue 2). Statistics: `export BB_AOT_STATS=1 BB_AOT_STATS_SEC=60`
(per-thread fallback/host time, misses, top miss addresses, in `logs/android-game.log`); `BB_AOT_TRACE=1` logs every
fallback run with the thread's recent transitions, and the first hits of each hook slot.

## Building what the device runs

    nice -n 19 cmake --build out/recomp-build -j 22
    nice -n 19 out/recomp-build/bb-recomp --elf out/eboot.elf --boot out/boot-linked.bin \
        --prx-dir <game>/CUSA03173/sce_module --target aarch64 --so out/aot/libbbaot.so -O1 \
        --variants tools/recomp/variants --ndk <BloodbornePC>/.local-deps/android/sdk/ndk/29.0.14206865     # ~100 s, 133 MB
    BB_NDK=<ndk> BB_ARM64_HEADERS=<BloodbornePC>/.local-deps/root/usr bash tools/android/build_bionic.sh
    # out/deploy: llvm-strip copies of out/bionic/lib/{libbbcpu,libbbgpu,libbbnet20}.so and out/bionic/bin/bb-probe, plus run-bionic.sh

The device's `data/boot-linked.bin` must be the image the .so was built from (sha256 b5cb1da3..., identical to
`out/boot-linked.bin`; eboot.bin sha256 d65f0b4f... on both). The runtime hashes each unit against the loaded image: patched
units become variants or fall back by themselves.
