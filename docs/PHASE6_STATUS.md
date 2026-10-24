# Phase 6 (native-only AOT runtime): done on branch recomp-p6

Branch recomp-p6 (from recomp-rt 142f8e8); nothing pushed, nothing run on the device, the game was never run on the PC. Design and numbers: docs/STATIC_RECOMPILE_PLAN.md
("Phase 6"), the interfaces: docs/RECOMP_ABI.md ("Granular entries", "Misses, strict mode and the misses log", "BB_AOT_ONLY"), the tool: tools/recomp/README.md.

## What is in it
- The three misses of the device session are fixed in the tool, in general terms (discovery counts direct branch targets anywhere in a module's executable segment; secondary units feed
  discovery; `cltq`/cdq/mul/... no longer hide the table base lea; flag-preserving instructions may sit between `cmp` and `ja`). `tests/run_aot_entries_check.sh` checks them on the real image.
- Granular entries for the 62 functions with unresolved/scanned-only table-like jumps: +0.8 % `.text`, +12 % codegen CPU. For every function: not affordable (+77 %, 15x codegen on a sample), so the
  miss-log loop (`--extra-entries`) covers cross-function mid-targets.
- Runtime: granular dispatch, `logs/aot-misses.log`, `BB_AOT_STRICT=1`, `BB_AOT_ONLY` libbbcpu (1.1 MB, no FEX symbols) with native CPUID/XGETBV/TSC, unaligned-atomic emulation, host-function stubs,
  thread state, import-trap peephole.
- Tests: `tests/run_aot_native_test.sh` (atomic emulator on instruction words, CPUID model, granular dispatch, misses log, strict mode child processes, import traps),
  `bb-recomp-asmtest --granular-selftest` (with the resume, hook and variant self-tests), `tests/run_aot_entries_check.sh`, the existing PC tests, and `build_bionic.sh` in both forms (below).

## Build status
- Normal: `BB_NDK=... BB_ARM64_HEADERS=... bash tools/android/build_bionic.sh` links (all outputs, symbol check clean).
- `BB_AOT_ONLY=1 bash tools/android/build_bionic.sh` (reuses out/bionic, replaces libbbcpu.so and relinks bb-probe into out/bionic-only) links, symbol check clean.
- The tool: `nice -n 19 cmake --build out/recomp-build -j 22`; the full `.so` with `--granular jumps` builds in ~2 min (128 s), verifier clean.

## Not done / needs the device
- Nothing was run on the Thor: the strict-mode and BB_AOT_ONLY runtimes, the native CPUID values (a port of FEX's model with the host features fixed; compare `cpuid` as the game sees it today),
  and the unaligned-atomic emulator on real SIGBUS faults are untested there. Granular dispatch and strict mode are tested on the PC with synthetic units; the granular lowering on x86 by the self-test.
- The unresolved-import path (`unresolved` host function) is recognised, not run.
- Still FEX-bound: `cpu/guest_cpu_fex.cpp` and what it links, in the normal (non-ONLY) build (non-strict fallback, `guest_cpu_call` without BB_AOT, FEX's hook trampolines `guest_cpu_hook` writes);
  in strict mode of the FEX build only FEX's initialisation and thread state remain (CPUID, XGETBV, TSC, unaligned atomics, misses are native).
- Phase 4b hook-slot hang and GPU flip regression belong to the device agent on recomp-rt; recomp-rt has newer commits (7c939e6, 5f259e2) touching cpu/guest_cpu_aot.cpp (crash-report printing),
  src/probe.c and gpu/shim/bbport_frame_rate.cpp: a small conflict is expected in guest_cpu_aot.cpp's `bbaot_crash_report` when merging. This branch's edits to the hook-slot code and the shim: none.
