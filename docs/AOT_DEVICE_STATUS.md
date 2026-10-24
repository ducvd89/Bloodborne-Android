# AOT on the AYN Thor: status (branch recomp-rt, 2026-10-24)

> Historical (before phase 7): FEXCore as the fallback, `aot_device.sh`, `BB_AOT=0` and the FEX builds described below no longer exist in this repository. The `BB_AOT_ONLY` results are the runtime that phase 7 made the only one (docs/STATIC_RECOMPILE_PLAN.md).

The game's x86-64 code runs as ahead-of-time units (bb-recomp: FEX front end -> LLVM -> AArch64 `.so`) on the Thor
(Snapdragon 8 Gen 2), with FEXCore as the fallback on the same CPU state (cpu/guest_cpu_aot.cpp, docs/RECOMP_ABI.md).

## Current state (2026-10-24, after phase 4b and phase 6; supersedes the older results below where they differ)

**The device runs in AOT mode** (all 10 verification launches clean): the merged recomp-rt runtime (libbbcpu with FEX as the
idle fallback, bb-probe, run-bionic.sh, this branch's libbbgpu with phase 4b's native hook slots) and the units .so
(`--variants tools/recomp/variants`, phase 6's granular jumps), the user's own `bionic-local.sh` and `bbport.ini`.
`bash tools/android/dev/aot_device.sh off` puts the stock 1.2.0 runtime back.

**Verification on the Thor** (each: launch, title, Continue, 2 minutes of walking/attacks/rolls):

| configuration | launches | result |
|---|---|---|
| AOT, non-strict (FEX build) | 10 | 10 clean, ~29.6 FPS; misses log empty; **0 misses, 0 fallback runs, 0 dirty entries** in the statistics: no code ran on FEX |
| AOT, `BB_AOT_STRICT=1` (FEX build) | 2 | 2 clean, misses log empty |
| `BB_AOT_ONLY` runtime (no FEXCore: libbbcpu 1.1 MB) | 3 | 3 clean ("ahead-of-time units only"), misses log empty |
| FEX mode with this branch's runtime (`BB_AOT=0`) | 8 | 8 clean |
| phase 4b slot mode | all of the above | 45 patched functions as variants, 0 on the fallback, 0 dirty entries; frame-rate slots (pacer, step, fixed steps), the scene-size slots and the redirects all native |

No "Out of order flip IRQ" assertion in these ~26 launches (see open issue 3 below).

**CPUID / XGETBV** (`BB_AOT_CPUID_LOG=1`; FEX's model against the native one strict mode and BB_AOT_ONLY use): on the Thor the logged leaves were equal
except 0x80000001 ECX bit 6 (SSE4a: native 1, FEX 0), 0x80000002 brand string (FEX: the host core's name, "Cortex-A510"; native: "Unknown ARM CPU") and
0x80000008 EBX bit 0 (CLZERO: FEX 1, native 0). XGETBV(0) = 7 in both. **Fixed in cpu/guest_cpu_cpuid.h (testing branch):** SSE4a is off, CLZERO is on
(from DCZID_EL0, as FEX), and the brand string is FEX's product-name lookup of core 0's MIDR (`/sys/devices/system/cpu/cpu0/regs/identification/midr_el1`;
HideHybrid, FEX's default, makes every core report core 0's name). A full read of FEX's CPUID.cpp found three more differences the log did not cover, all
fixed: leaf 0x15 ECX (FEX: the counter's own frequency and EBX = 1 << scale; the native Init passed the scaled frequency, so ECX * EBX was 64 times too
large), leaf 0x40000001 sub-leaf 1 (FEX's version string "FEX-2609.1"; native: zeros), and leaf 0x80000005 (AMD-only in FEX, zeros with the Intel identity;
native answered values). The model is pinned by `tests/test_aot_native.cpp` (exact register values, brand strings of known MIDRs, MIDR parsing) and compared
with FEXCore's own `CPUIDEmu` (x86-64 build of the tool, Thor host features) over 651 function/sub-leaf pairs by `bb-recomp-asmtest --cpuid-diff` (0 differ);
the brand string, leaf 0x15 and the host nibble of 0x40000001.0 are AArch64-only code in FEX and are checked by the pinned values instead. Not re-measured
on the device yet: run once with `BB_AOT_CPUID_LOG=1` (`DIFFERS` must not appear, leaf 7 sub-leaf values depend on FEX's TSO configuration there).

**FPS and CPU, capped (user settings: limit 60, LSFG x2, GSR 2), 60 s each at the save spot, merged runtime:**
AOT 29.7 FPS, process 325 % of a core (guest threads 113.6 %); FEX 29.4 FPS, 376 % (guest threads 124.0 %). The host GPU
threads (bb:DrawRec, GpuComm, VkRecorder), which the CPU backend does not touch, also differ by ~20 % between the two runs, so
part of the total gap is run-to-run variance; the guest side is reliable: FMOD mixer 13.8 vs 21.2 % (-35 %), GXWorker x5 42.3 vs
47.0 %, GXRenderThread 16.6 vs 18.4 %. The game is GPU-bound at this spot: FPS is the same.

**Fixed in this round:**

| commit | bug |
|---|---|
| ef1a331 | **spin-waits hoisted**: `L: cmp eax, [r13 + 0x18]; jg L` (the main thread waiting for the GX workers, +0x21b8420) had no store in the loop, so the backend hoisted the load and the unit spun for ever on a stale value. Loads in loops without stores, atomics, fences or exits are now volatile (36,776 in the game). This was the "phase 4b slot-mode stall" (deterministic there) and, most likely, the intermittent pre-title stalls before the power outage that looked like an x87 problem |
| c58a088 | **lift base collided with plain constants**: constants inside [0x200000000, +image size) were relocated as image addresses; `movabs rax, 0x200000002` (two packed counts, +0x221b124) became image + 2, a count of 0x104002, a 1 GB allocation, null, guest fault at +0x2264d20. Lift base now 0x3a7700000000; 186 wrongly relocated constants in the game |
| ef66498 | `tests/run_recomp_device_regressions.sh`: both cases on the game's functions (fails with the old base, passes now) |
| ac0058c, 0764277 | diagnostics: SIGUSR2 dumps print each thread's AOT trace (host functions as module+offset), `BB_AOT_FALLBACK_RANGES` pins functions against the variant classifier, `BB_AOT_PROBE_CALL`, `BB_AOT_CPUID_LOG`, strict mode announced, frame-rate shim `BB_FRAME_SLOT_TEST`/`BB_FRAME_LIVE_NEAR` |
| 158e546 | merge of recomp-p6 (phase 6: misses fixed, granular entries, strict mode, BB_AOT_ONLY) |

How it was found: the stall's thread was spinning (state R, 100 % CPU) at one host pc (unit +0x21b8140 offset 0x5b4: `cmp; b.gt .`);
the crash after it was traced with an indirect-call probe (the allocation's size register) back to the struct a unit built.
Ruled out on the way: the frame-rate callbacks, the Live struct in the image (+0x56d3f40 is unused, RW), the redirects, the slots
themselves, memory layout.

## Earlier results (before phase 4b/6)

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

1. **(Explained: the spin-wait hoisting fixed in ef1a331.)** x87 hang: not reproducible, no fix. Before the power outage about 1 launch in 3 stopped before the title (all threads
   parked in host waits); it followed the step 2d merge, and keeping the 127 x87-helper units on FEX avoided it (6/6).
   Bisection ended at libc.prx's `fabsl` (+0x571cc10: unit 65 alone on FEX, 6/6 clean), but the control (unit 66 alone on FEX)
   was also 6/6 clean, the baseline after the device's restart was 8/8 clean, and 30+ launches since never stalled. The
   lowered IR of `fabsl` matches FEX's reduced-precision path (F80 load + bbr_x87_f80_to_f64, x87 TOP/tag updates). If it comes
   back: `BB_AOT_FALLBACK_RANGES=571cc10-571cc11`, and a watchdog dump (kill -ALRM bb-probe) of the stall.
2. **(Fixed: ef1a331 and c58a088, see above.)** Phase 4b: the frame-rate patcher in slot mode stops the game before the title (deterministic, AOT only; 4/4). Bisected
   to the frame-rate patcher's slots (0x203493d pacer mode, 0x2018e36 step, 0x1572a7d / 0x17f9b71 fixed steps) and its Live
   struct in the image (+0x56d3f40). Any one of them without a slot (`BB_AOT_NO_SLOTS=1572a7d`) makes the patcher use its
   old detours, and the game runs (then 32 patched functions are on the fallback). The display slots (scene size
   +0x1d96a6b/+0x1d96a7a) work ("the game's scene size 960x540 (native hook slots)"); patch variants work (45 patched functions
   native, 0 dirty entries). Phase 4b was checked on synthetic tables (aot_so_check), not in the running game; it needs the
   patcher's owner. Online (BBSC) hooks were not tested on the device.
3. **"Out of order flip IRQ" GPU assertion** (update: not seen in ~26 launches after the lowering fixes and the phase 6 merge,
   AOT and FEX mode; the earlier hits came in a period of heavy back-to-back testing, partly with the miscompiling .so; not
   reproducible, so not bisected. If it returns: count it per configuration with the scratchpad `flipruns` style loop) (shadPS4 video_out.cpp:357) at start, about 1 launch in 5-10 since the merges, in
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

## Phase 6 (branch recomp-p6, 2026-10-24): the remaining misses are fixed in the tool

Items 4 and the process-main-thread note above: +0x20, +0x210ebc0 and +0x23a9c9c are covered by the next `.so` (docs/PHASE6_STATUS.md; `tests/run_aot_entries_check.sh`). The `.so` has to be rebuilt and
deployed to see it on the device. `BB_AOT_STRICT=1` logs misses to `logs/aot-misses.log` and stops the game; `bb-recomp --extra-entries aot-misses.log` closes a gap by rebuilding.
