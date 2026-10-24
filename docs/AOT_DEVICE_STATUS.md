# AOT on the AYN Thor: status (branch recomp-rt)

## Paused at (2026-10-24, power outage)

**What works.** The AArch64 units (`bb-recomp --boot out/boot-linked.bin --target aarch64 --so`, -O1, 185,934 units incl.
libc.prx/libSceFios2.prx, testing step 2d merged) load and run on the Thor under libbbcpu from this branch, on the 1.2.0
release app (installed in place, same signing key, saves kept; backups in `out/thor-backup/`). With AOT on, the game reached
the title screen, the main menu, loaded the save (Continue) and gameplay ran (character walks). In gameplay nearly every
thread runs in units (per-thread fallback time 0-1 %; GX/CSChr/EzWorkPool all in units); two Havok workers sit in long
CFI-bounded FEX runs. FPS at the cap is the same as FEX (29.8, limit 60 with LSFG x2); CPU per-thread profiles not measured yet.

**Fixed so far** (commits on recomp-rt): unaligned `lock` atomics in units (SIGBUS -> FEX's non-JIT handler);
dlopen of the .so on Bionic (DT_NEEDED libbbcpu.so); merged tool ABI (bbr_dirty_entry/bbr_dirty[], no .so text patching,
exact unit ranges); free runs entered with a wrong RSP (crash at +0x52d6ae8); trap units start dirty; in-function exits the
lifter did not decode become unit entries (432 -> 2144); free runs replaced by CFI-bounded fallback runs; per-thread stats.

**Current bug.** Intermittent hang before the title screen (about 1 launch in 3: all threads parked in host waits, no frame
presented). It started with the testing step 2d merge (x87 ops lowered instead of run on FEX). Putting the 127 x87-helper
units on FEX (BB_AOT_FALLBACK_RANGES) removes it (6/6 good). Bisection: the eboot's 7 x87 units are not it; among the 119
libc.prx x87 units (`out/dbg/libc_x87.txt`, sorted), with units 61-90, 61-75 and 61-68 on FEX all runs were good (6/6 each),
with 1-60 on FEX it still hung: the culprit is most likely in units 61-68 (+0x571a470 ... +0x571cc10 range, list line 61-68).

**Next step.** Bisect 61-64 / 65-68 (6 trials each with `trial`-style launches), then compare the unit's lowering with FEX's
reduced-precision x87 (bb-recomp --emit-ll / --dump), fix in the x87 lowering (testing's area: report it) or keep those units
dirty. Then: CPU time per thread (simpleperf) AOT vs FEX at the save spot, BB_AOT_STATS ratio, final doc.

## Switching AOT on or off on the device

- Files: `files/bbport/aot/libbbaot.so` (the units), `files/bbport/bionic/lib/libbbcpu.so` and `bin/bb-probe` (this branch),
  `files/bbport/run-bionic.sh` (this branch: picks up aot/libbbaot.so, honours BB_AOT=0).
- **Off (current state):** `files/bbport/bionic-local.sh` ends with `export BB_AOT=0`: FEX JIT only, checked working.
- **On:** delete that line (`adb shell run-as com.ducvd89.bloodborne sh -c "grep -v '^export BB_AOT=' files/bbport/bionic-local.sh > t && mv t files/bbport/bionic-local.sh"`).
  The same file sets BB_AOT_STATS/TRACE/DEBUG for the test (statistics every 20 s in logs/android-game.log).
- Full restore of the user's original 1.2.0 runtime: install `out/release-1.2.0/Bloodborne-1.2.0.apk` again is not enough
  (same runtime digest: the app does not re-extract); copy `out/thor-backup/runtime-1.2.0/{bb-probe,libbbcpu.so,run-bionic.sh}`
  back, delete `files/bbport/aot`, and restore `files/bbport/bionic-local.sh.pre-aot` to `bionic-local.sh`.
