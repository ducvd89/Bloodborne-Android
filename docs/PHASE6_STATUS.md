# Phase 6 (native-only AOT runtime): paused

Branch recomp-p6 (from recomp-rt 142f8e8). Paused on request; nothing is running. Not pushed.

## Done (tool side, committed as WIP; builds, lifter/discovery checked on the real image, not the whole .so)
- Discovery: direct branch targets count anywhere in a module's executable segment. The start stub at +0x20 (static initialiser runner,
  called from the entry +0xa0, before the first FDE) is now a unit.
- Discovery: secondary units (landing pads, setjmp sites, entryUnits) now report their out-of-unit targets, and setjmp return sites found in them
  are queued (worklist). Mid-function entries 2145 -> ~3000.
- Switch tables: `cltq`/`cdq`/mul/div/xchg/... no longer clobber every register (per-instruction write masks), and flag-preserving instructions
  may sit between `cmp` and `ja`. This resolves the CSChrThread +0x210ebc0 switch (its table has 0x210ebc0 as a case, `cltq` hid the base lea)
  and the EzWorkPool +0x23a9c9c switch (`cmp rax,11; mov; ja`). Unresolved table-like jumps 45 -> 8, scanned (unbounded) 124 -> 75.
- Granular entries (tool): `--granular off|jumps|all`, `--granular-funcs`, lowering gives every unit of a granular function a resume-flag switch over every
  instruction boundary (blocks split like hook resume points), `bbr_func_granular[]`, `bbr_abi_flags` bit 4. Default `jumps`: 62 functions.
- `--extra-entries <misses log>`, `--dump-units`, `--check-entries a,b,c`, `--units-only`.

## In progress / not built or tested yet
- `--check-entries` was just changed to also accept "case of a resolved jump table" (compile.cpp, not rebuilt). Last run of the check:
  +0x20 unit entry; +0x210ebc0 and +0x23a9c9c now covered by resolved switches (the old wording said NOT AN ENTRY).
- Cost measurement: a full `--so` run with `--granular off` was started (out/aot/base.so) and killed; no size/time numbers yet. Plan: base vs `jumps`
  on the whole image, and `all` on a `--limit` sample, to decide whether all-function granularity is affordable (else the miss-log loop).
- Granular selftest in bb-recomp-asmtest (like --resume-selftest): not written.

## Next steps
1. Rebuild the tool, re-run `--check-entries 20,210ebc0,23a9c9c`, measure granular cost (.text, compile time), write the selftest.
2. Runtime (cpu/guest_cpu_aot.cpp): granular dispatch on a miss (bbr_funcs + bbr_func_granular, try the function's units with s->resume=1), miss log
   (files/bbport/logs/aot-misses.log: `miss +0x... thread=... backtrace=...`), `BB_AOT_STRICT=1` abort message, non-strict keeps FEX.
3. `BB_AOT_ONLY` build of guest_cpu_aot.cpp without FEX: own cpuid/xgetbv/TSC, unaligned-atomic emulation, host-function/trap stubs
   (probe.c's unresolved-import traps are x86 code and need a native path), fault handling, thread state; list what stays FEX-bound.
4. Tests (unit tests, PC-side check, build_bionic.sh normal and BB_AOT_ONLY), docs (RECOMP_ABI.md "Granular entries", "Strict mode", README loop).
Note: recomp-rt has uncommitted edits to cpu/guest_cpu_aot.cpp (crash report) and src/probe.c; expect a small merge there.
