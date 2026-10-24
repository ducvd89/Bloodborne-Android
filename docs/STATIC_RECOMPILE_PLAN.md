# Removing FEXCore: ahead-of-time x86-64 → ARM64 recompilation (plan, 2026-10-23)

Goal: the game's x86-64 code runs on Android as ARM64 code compiled ahead of time, and FEXCore
(`cpu/guest_cpu_fex.cpp`, `libbbcpu.so`, `fex-compatible.json`, the FEX submodule) is removed.
The runtime already reaches the CPU through one narrow interface, `src/guest_cpu.h`, which has two
implementations: native on x86-64 (`src/runtime_cpu.c`) and FEXCore elsewhere. The new backend is a
third implementation behind the same interface, so the runtime, GPU code and app keep working.

## What has to be translated (measured on CUSA03173 `out/eboot.elf`)

| | |
|---|---|
| Executable segment | 0x0–0x50d96dc (81 MB, including rodata); code is 0xa0–0x2bbe4a5 (46 MB) |
| Functions (`.eh_frame_hdr` FDEs) | 162,959, covering 41.7 MB: function starts and ends come almost for free |
| Instructions (linear sweep) | 11.27 M; 720 distinct mnemonics |
| AVX (`v*`) | 1.03 M (9 %): AVX1 at 128 and 256 bits (Jaguar, no AVX2 or FMA) |
| x87 | about 2,300 (rare; FEX already runs with `X87ReducedPrecision=1`, so f64 is fine) |
| Indirect calls | 165,596 `call *mem` (vtables), 8,345 `call *reg`, 1,863 `call *rip` |
| Indirect jumps | 7,513 `jmp *mem` and 14,644 `jmp *reg` (switch tables, tail calls) |
| Atomics | about 12 k `lock` instructions (xadd, cmpxchg, inc), 14.6 k `xchg`, 60 `mfence` |
| Other | 3,161 `rdtsc`, 4 `cpuid`, BMI1 (`bextr`, `andn`, `tzcnt`), `lzcnt`, `movbe`, `popcnt` |
| Linked modules | `libc.prx` (0.9 MB) and `libSceFios2.prx` are linked into the runtime image (`boot-linked.bin`) and translated the same way (the other five `sce_module/*.prx` are not loaded) |

The memory model is already de-risked: the shipped `fex-compatible.json` turns TSO emulation off
(`TSOEnabled=0`, `VectorTSOEnabled=0`) and the game runs. Plain ARM64 loads and stores are therefore
what the game sees today; `lock` operations become LSE atomics.

## The hard constraint: translated code cannot be shipped

ARM64 code produced from the eboot is derived from the game binary, so it cannot go in the APK or on
GitHub. It has to be produced **on the player's device**, from their own dump, like the image that
`bb-prepare` builds today. That decides the architecture:

- **`bb-recomp`** is a separate on-device tool (not part of the game process). It reads the prepared
  image and writes `aot/<image-hash>.so` into the app's files. It runs once after game preparation,
  and again when the code changes (patch set or game update).
- **Code generation: our lifter emits LLVM IR, and LLVM's AArch64 backend compiles it.** LLVM gives
  register allocation across blocks, dead-flag elimination and real instruction selection, which a
  hand-written emitter would take months to match. Only the AArch64 target is linked statically
  into `bb-recomp`, which adds roughly 25–35 MB. That is either bundled in the APK or downloaded on
  first use, like the GPU drivers.
- Compile time on the phone is the main unknown. Phase 0 measures it. The fallback is chunked
  compilation (functions in shards, all big cores, resumable, progress in the existing
  `PkgProgressDialog`), with `-O1` for cold code and `-O2` for hot code.

**Decision 2026-10-24: compile on the PC.** `bb-recomp` is a small PC program (Linux and Windows)
that the player runs on their own dump. It writes the `.so`, which they copy to the device, like
the game files. Consequences:

- No LLVM in the APK, which stays about 17 MB.
- No phone heat or battery cost. A whole-game compile takes minutes on a desktop CPU: about 35
  core-minutes measured, so 2–5 min on 8–16 cores.
- Corner D (hot code first, to keep the device compile short) is no longer needed: everything is
  compiled at once.
- FEX's front end lives only in the PC tool, never in the APK. So "FEX out of the repo" stops
  mattering: what counts is FEX out of the game process.
- What remains open: players without a PC (or without the `.so`) need either the FEX JIT kept in
  the APK as a fallback, or a PC becomes required.

## How the translated code runs

- **One host function per guest function** (FDE starts, plus entries found by recursive descent,
  relocations, vtables, exports and exception landing pads). Each one takes a `Cpu*`: 16 GPRs,
  flags, 16 YMM, MXCSR, a small x87 stack, and the FS/GS bases. LLVM keeps registers in SSA inside the
  function and spills them only at calls and exits.
- **The guest stack stays real.** `call` pushes the guest return address, then calls the host
  function directly. `ret` pops it and compares it with the expected address; on a mismatch
  (longjmp, stack tricks) it goes through the dispatcher. This keeps libc.prx's unwinder, the stack
  scans in `runtime_kernel.c`/`page_manager.cpp`, and `guest_cpu_return_address` working.
- **Flags are computed lazily.** x86 flags are dead across calls and returns, so most flag
  computation is removed at compile time.
- **Indirect branches** look the guest address up in an open-addressing table (about 200 k entries),
  then call the host function. Switch tables are resolved statically where the usual pattern
  (`cmp`/`ja`/`jmp [base+idx*8]`, `lea`+`movslq`+`add`+`jmp reg`) matches.
- **Misses (code not found ahead of time)** go to a small **x86 interpreter** that shares the
  lifter's instruction semantics, and the address is logged. The next start compiles the logged
  functions into an incremental `aot/<hash>.extra.so`. This replaces FEX as the safety net.
- **Imports** become direct calls to the host functions with the SysV argument mapping (RDI…R9,
  XMM0–7). That removes the `guest_cpu_host_function` thunk cost.
- **Signals**: guest memory is host memory (same address space below 1 TiB), so write-tracking
  faults in translated code resume natively. `guest_cpu_signal_regs` maps the host PC to a guest RIP
  through a per-block side table, which is enough for the diagnostics that use it.

## x86 semantics to get exactly right

These are the places where a naive translation would differ from FEX/x86 and break the game quietly:

- `cvtt*2si` out of range gives 0x80000000…, not ARM saturation.
- `minps`/`maxps`/`minss`: the second operand wins when either is NaN.
- The x86 default NaN is negative QNaN; ARM's is positive.
- MXCSR rounding and FZ/DAZ map to FPCR.
- `pshufb`, `pmovmskb`, `ptest`, `blendv`, `dpps`, `roundps`, `pcmpestri` need emulation sequences.
- 256-bit AVX becomes pairs of 128-bit NEON operations. VEX zeroing of the upper halves must be
  honoured.
- `lock` on an address that crosses a cache line: ARM faults where x86 succeeds, so a SIGBUS slow
  path emulates it under a global lock (FEX has the same handler).
- `rdtsc` maps to `guest_cpu_tsc`; `cpuid` returns the Jaguar values.

## Hooks, patches, cheats and mods

- **Byte patches** (`fps=uncap.bin`, `res-scene.bin`, `graphics-heap.bin` …) are applied by
  `bb-prepare` before compilation. The cache key is the hash of the *patched* image. To avoid a full
  recompile per patch combination, patched functions are compiled as small variant objects and
  selected at load time.
- **Runtime hooks** (`guest_cpu_hook`, `bbport_guest_code_hooks.cpp`: Hunter's Atlas, bbnet20 online,
  BBSC sites, live cheats) are known site lists. `bb-recomp` puts a patchable call-out at each known
  site: one load and branch through a slot table. Installing a hook just sets the slot, so no x86
  trampoline is written.
- **Unknown runtime code writes** (`guest_cpu_invalidate`) mark the containing functions dirty. They
  run in the interpreter until the next recompile.
- **Mods** replace data files, not code, so they are not affected.

## Phases

Each phase has a gate. FEX stays in the build as the **oracle and fallback** until phase 6.

| # | Phase | Output | Gate | Est. |
|---|---|---|---|---|
| 0 | Feasibility | PC tool `bb-recomp-scan`: discovery coverage against FDEs, how many switch tables resolve, the instructions to support. Lift about 50 hot functions (memcpy-like routines, math, FMOD mixer) to LLVM and differential-test them against FEX. **Time a full-image LLVM compile on the PC and on the Thor.** Check that the game never maps executable memory at run time (`sceKernelMprotect` with EXEC) | Compile time on the Thor fits an install step (target under 30 min); discovery reaches ≥99 % of FDE bytes | 1–2 wk |
| 1 | Integer core | Decoder (Zydis is already in the tree), CPU state, integer ISA with lazy flags, control flow, call/ret/stack, switch tables, dispatcher. A differential harness runs a function under FEX and under AOT with the same registers and memory snapshot, and compares the results | All discovered integer-only functions pass the differential tests | 3–4 wk |
| 2 | SSE/AVX/x87/BMI | Vector and FP semantics listed above | Differential tests pass on random and edge-case inputs | 3–4 wk |
| 3 | Hybrid in game | `cpu/guest_cpu_aot.cpp` implements `guest_cpu.h`: AOT where present, FEX for misses. The game boots, loads a save, `route_bench.sh` runs, saves load, online works | Same frames as FEX on the route; simpleperf CPU per thread compared with `thor-cpu-waste-profile` | 2–3 wk |
| 4 | Hooks and patches | Hook slot table, patch variants, invalidate → dirty functions | Atlas, online (Seamless Hunt), cheats and FPS uncap work on AOT | 1–2 wk |
| 5 | On device | `bb-recomp` NDK build with static LLVM AArch64, run after `bb-prepare` with a progress UI, cache in `files/aot/`, incremental `extra.so` | A fresh install on the Thor and on a Mali device reaches gameplay | 2–3 wk |
| 6 | Interpreter replaces FEX | x86 interpreter for misses and dirty code, miss log → recompile at next start | A full route with FEX disabled; zero or only logged misses | 2–3 wk |
| 7 | Remove FEXCore | Delete `cpu/guest_cpu_fex.cpp`, `fex_project.cmake`, the FEX submodule and build steps, `fex-compatible.json`, FEX env in `run-bionic.sh`, the FEX notice | Release build and APK without FEX | 1 wk |

Total: **about 4–5 months** of agent-driven work. The first in-game result (hybrid, phase 3) comes
at about 2–2.5 months.

## What it buys, honestly

- The Thor profile (2026-10-19) put translated game code at about 82 % of one core out of 154 %, at
  27.5 FPS, and the Thor is **GPU-bound**. Expect lower CPU power and heat, and more headroom on CPU-bound
  devices (the N5 heat limit, weaker Mali phones), not a large FPS gain on the Thor.
- **No JIT compile stalls**: FEX translates code the first time it runs, which causes hitches in new
  areas. With AOT that cost moves to install time.
- Calls into the runtime get cheaper (no FEX thunks), and the large-code cases where FEX is weakest
  (vtable-heavy C++) become direct table lookups.

## Using FEXCore as the reference: the shorter route

FEX 2609.1 (`third_party/FEX`, MIT) already has the hardest parts of phases 1, 2 and 6:

- **Front end (x86 → FEX IR).** `X86Tables`, `Frontend.cpp`, `OpcodeDispatcher*` and
  `RedundantFlagCalculationElimination`, about 17 k lines. This is every x86 instruction the game
  uses, with the edge cases above already solved (cvtt, NaN rules, AVX on NEON as `AVX_128.cpp`,
  x87 as f64 in `X87F64.cpp`, lazy flags).
- **IR.** `IR/IR.json` has about 440 ops, chosen to map to AArch64, so they lower to LLVM almost one
  to one.
- **Arm64 JIT** (`Core/JIT/*.cpp`, 12.5 k lines). For each op it shows the exact ARM64 sequence, so it
  is the specification for our LLVM lowering.
- **Tests.** 2,265 `unittests/ASM` instruction tests with expected registers, plus
  `InstructionCountCI` and the gcc target tests.

The cut corners, the time each saves, and what each gives up:

| Corner | Saves | Gives up |
|---|---|---|
| **A. FEX front end as our lifter.** `bb-recomp` links FEXCore, takes the IR after FEX's passes, and lowers it to LLVM. We write only the FEX IR → LLVM backend | Phases 1–2 shrink from 6–8 wk to 3–4 wk, and the semantics risk mostly goes | FEX code stays in the repo (inside `bb-recomp`, not in the game process) |
| **B. FEX's tests and FEX JIT as the oracle.** Run the 2,265 ASM tests through our backend, and differentially test each function: same IR, FEX JIT against LLVM | Most of the test-harness work | Nothing |
| **C. FEX JIT kept in the game only for misses** (code not found ahead of time, runtime-written hook trampolines, invalidated code) | Phase 6 (interpreter, 2–3 wk) and most of phase 4 (hooks stay x86 trampolines) | The game process still links FEXCore, but cold |
| **D. Hot functions first.** Compile the few thousand functions that hold most of the CPU time (an address list, which is not game code, can ship); FEX JIT runs the rest | The on-device compile drops from tens of minutes to minutes; less LLVM risk | The cold code stays on the JIT until "compile all" is turned on |
| **E. No static switch-table resolution.** Indirect `jmp` goes through the dispatcher | About 1 wk | A small cost on switch-heavy code |

What cannot be cut:

- Compiling on the device (legal reasons).
- LLVM. Using FEX's own backend ahead of time, with its disk cache (`DiskCache`,
  `EnableCodeCachingWIP`), only removes JIT stalls; the code quality is the same as now.
- The differential tests.

Estimates:

| Route | First in-game result | FEX gone from the game process | FEX gone from the repo |
|---|---|---|---|
| Full plan (above) | 2–2.5 months | 4–5 months | 4–5 months |
| A+B+C+D+E | **about 5–6 wk** (hot code on LLVM) | +4–5 wk: compile all code, plus our own small interpreter for misses and hooks | +6–8 wk (our own front end, with FEX's tests as the specification) |
| FEX disk cache only | 1–2 wk | never | never |

## Compile time (2026-10-24, measured on the real game)

Measured with `bb-recomp` (FEX front end -> LLVM 23 -> AArch64 object, `aarch64-linux-android30`, generic ARMv8.2 + LSE),
on the dev PC (Ryzen AI 9 HX 370: 12 cores, 24 threads), 22 worker threads at `nice 19`, 64 shards of equal code size, a
per-function pass pipeline without interprocedural passes. Input: CUSA03173 `eboot.elf`, all 162,959 FDE functions plus the
gap units = 202,525 units, 11.81 M x86 instructions, 58.7 M FEX IR ops. All ops lowered: no unit contains a `bbr_trap`.

| | -O1 | -O2 |
|---|---|---|
| **Wall time, lift to objects** | **63.9 s** | **70.1 s** |
| Thread-seconds: lift + lower / optimise / codegen | 51 / 220 / 916 (1187 total) | 51 / 332 / 925 (1308 total) |
| `.text` | 102.97 MB | 103.41 MB |
| Object files | 135.4 MB | 135.8 MB |
| Peak RSS (22 workers, 64 shards) | 15.2 GB | 15.2 GB |
| `.so` (objects + unit table + link with the NDK `ld.lld`, 0.2 s) | 123.3 MB | |

- That is 20 thread-minutes of work at -O1 for the whole game, about 100 us per x86 instruction. The proxy estimated 35
  core-minutes; the real code is about 40 % cheaper because most of the proxy's cost was a C front end that is gone.
- **Code generation is 77 % of the time**, the optimiser 18 %, lifting and lowering 4 %. -O2 costs +10 % wall and gives no
  smaller code at this pipeline, so -O1 is the default; the codegen opt level follows (-O1: `Less`).
- Code size: 2.2 AArch64 instructions per x86 instruction (25.7 M instructions), as the proxy predicted (2.1). The first
  lowering had 3.3: storing NZCV and PF/AF before every call and return, and storing every written register at every exit
  (now: only registers modified since the last store, flags only at tail jumps, rbx/rbp/r12-r15 not reloaded after a call)
  took `.text` from 156 MB to 103 MB and codegen time by a third. Half of the remaining instructions are loads and stores
  (spills at calls and exits, guest memory).
- **The PC tool compiles the whole game in about a minute** on this machine. A 6-core/12-thread desktop needs about 3 min.
  Memory: about 0.7 GB per worker, so cap the workers at (RAM - 2 GB) / 0.7 GB on smaller PCs.
- If an on-device compile ever returns: 1187 thread-seconds on the Thor (about 2-2.7 Zen-5-core equivalents sustained) is
  roughly 8-15 minutes for the whole game, with 4-6 workers of 0.7 GB.
- Guest memory is plain align-1 loads and stores with no TBAA. Huge functions did not blow up: the largest function
  (78 KB, 16 k instructions, 3,094 blocks) compiles in 1.1 s, and the one with the most IR (0x26bc320: 64 KB, 358 units,
  570 k IR ops) in 5.2 s, alone.

**Update after step 2c (whole image: eboot + libc.prx + libSceFios2.prx, 182,663 functions, 184,271 units, 54.0 M IR ops, with jump tables resolved
and the discovery above; same machine, -O1, 22 threads, `nice 19`, another agent building at the same time, load average 11-23):**

| | -O1 |
|---|---|
| Wall time, lift to objects + table + link | **67.1 s** |
| Thread-seconds: lift + lower / optimise / codegen | 52.7 / 235.4 / 970.9 (1,259 total) |
| `.text` | **94.07 MB** (was 102.97 MB: resolving the tables removed the 39 k duplicated gap units) |
| Objects / `.so` | 124.6 MB / 110.5 MB |
| Peak RSS (22 workers, 64 shards of 0.71 MB) | 13.7 GB |
| With `--resume-entries` | 115.69 MB `.text`, 83.2 s, 16.3 GB |
| 12 GB machine (`BBR_FAKE_MEM_GB=12`, target 60 % = 7.2 GB) | 22 workers, **197 shards of 0.23 MB: peak 5.5 GB, 67.5 s** |

- **The compile adapts to the RAM** (`--mem-fraction`, default 0.6 of MemAvailable, also limited by a cgroup v2 `memory.max`).
  Measured: a worker holds its shard as an LLVM module, about **0.93 GB per MB of x86 code in the shard** (+ 0.04 GB); on a
  30,000-function sample with 2 workers the peak was 1.47 GB at 16 shards, 0.61 GB at 64, 0.40 GB at 256, and the wall
  time did not grow (133, 124, 101 s). So the plan makes the shards smaller before it lowers the worker count (the model
  uses 1.15 GB/MB to stay on the safe side): 44 GB free -> 64 shards, 22 workers (est. 18 GB, measured 13.7); 20 GB free -> 102 shards;
  12 GB -> 197 shards; 8 GB -> 315 shards; 4 GB -> 511 shards and 14 workers; 2 GB -> 5 workers. A 16 GB PC fits with every core.
- Code size fell although more code is compiled: gap units had duplicated the code after every switch.

## Discovery: how complete the unit set is (2026-10-24, step 2c)

`bb-recomp --elf out/eboot.elf --boot out/boot-linked.bin` lifts the runtime's own image: the eboot plus `libc.prx` and
`libSceFios2.prx` (the only linked modules; the other five `.prx` in `sce_module/` are not loaded) at their image offsets, so
one `.so` and one unit table cover everything. Census (lift only, 0.7 s wall on 22 threads):

| | before (2c start) | now |
|---|---|---|
| FDE bytes reached from function entries alone (no gap sweep, eboot) | 92.00 % | **98.84 %** (the rest: 0.18 MB padding, 0.27 MB table data, 0.03 MB dead or unresolved: 99.9 % of the real code) |
| Extra (gap) units with the sweep on | 39,566 in 2,525 functions | 1,159 in 505 functions |
| Functions / units in the `.so` | 162,959 / 202,525 | 165,773 FDE + 16,890 without an FDE / 184,271 |

- **Switch tables.** 15,562 indirect `jmp reg/mem` in the eboot (16,780 with the modules): **3,847 are jump tables and resolve**
  (3,741 bounded by the `cmp`/`ja` or an `and` mask, 106 scanned to the first entry that is no code address; 66,347 case
  entries, 0.27 MB of tables; 4,002 / 68,435 with the modules). Not resolved: 8,848 `jmp reg` that are no table (register tail
  calls: a vtable slot loaded, then `jmp rax`), 2,810 `jmp [reg+x]` (vtable tail calls; the 828 `[rip+x]` import stubs are in the
  PLT), and **57 table-like jumps** (53 with the table base set somewhere the walk does not reach, e.g. `jmp at 0x210e7c1`;
  4 where the index is loaded from the stack, e.g. `0x6ce423`). All of them still work through `bbr_dispatch_tail`. Absolute
  8-byte tables do not occur (1 `jmp [base+index*scale+x]`). The census (`out/recomp/census.txt`, `failures.txt`) lists them.
- **Landing pads.** 225 of the 165,773 FDEs have an LSDA: 1,223 call-site records, **692 landing pads**; 317 are new units, 375
  were already gap-unit entries.
- **setjmp.** The eboot imports `setjmp` (NID `gNQ1V2vfXDE`; `longjmp` ends in a jump to the saved return address): 1 GOT slot, the
  PLT stub at `0x2bbe8f8` and libc's export `0x570fcb0`; **132 call sites**, so 132 return-site units (cross-checked with an
  independent byte scan). No other returns-twice import exists in the game.
- **Code without an FDE, 16,890 pseudo-functions.** 4.1 MB of the eboot's code range lies between FDEs: 1.3 MB is
  alignment padding, **2.8 MB is real code**: 16,069 functions that data relocations point to (vtable thunks and static
  initialisers, such as a 62 KB run of `__cxx_global_var_init` at `0x392380`), 2 reached through `lea [rip+f]`, 1 through a
  branch, plus 816 PLT stubs and the two module start routines. They are lifted as units; after that only padding remains
  between functions. Plain `--elf` has no relocations and finds only 3 of them: use `--boot`.
- **Linked modules** (per module, functions incl. pseudo-functions): eboot 179,692, 98.76 % of the bytes in lifted blocks, 10.67 M
  x86 instructions; libc.prx 2,242 (2,164 FDE, 0.51 MB, 98.22 %, 148,771 instructions, 595 gap units: assembly with
  switch bodies); libSceFios2.prx 729 (650 FDE, 0.23 MB, 98.33 %, 58,575 instructions). All exported function symbols of both
  (1,597 + 165) are FDE starts. **New op gaps from libc.prx**: 118 units contain a `bbr_trap` for an op the eboot never
  used: x87 conversions to and from the 80-bit format (`F80CVT` 104 units, `F80CVTTo` 82: `fld/fstp tbyte` in
  `printf`/`strtold`), `F64FPREM1` 6, `F64SCALE` 3, `NZCVSelectV` 1.
- **Mid-function resume entries** (every call-return block, `--resume-entries`): 639,660 call-return blocks. Measured on the
  whole game at -O1: `.text` 94.07 -> 115.69 MB (**+23.0 %**), wall 67 -> 83 s (+24 %), codegen +31 %, peak RSS 13.7 -> 16.3 GB. Under the 30 % limit, so it is implemented
  (and tested: `bb-recomp-asmtest --resume-selftest`, plus the LLVM verifier on every module), but **off by default**:
  nothing in the game is known to enter a call-return address other than setjmp and unwinding, and the cost is paid by every
  unit (a flag load and branch at entry, a switch, phi moves). The runtime asks for it with `--resume-entries` and sees it in
  `bbr_abi_flags`. Design in `RECOMP_ABI.md`.
- **Remaining gaps.** (1) The 57 unresolved table-like jumps and any table whose pattern is not clang's (none seen) go
  through the dispatcher. (2) Code the game generates or maps at run time is not in the image. (3) Function pointers built
  from computed values (not stored as relocations or `lea`) that land in FDE-less code of the eboot would miss: none found;
  every function-pointer-like relocation target outside an FDE is now a unit. (4) Plain `eboot.elf` mode cannot find the
  relocation-reached thunks. (5) Mid-function returns (fibers, stack switching) rely on `--resume-entries` or the fallback.

## Risks

- **On-device compile time and APK size** (the phase 0 gate). The fallback is a downloadable
  compiler and sharded, background compilation.
- **Code found only at run time**: the interpreter plus the miss log covers it, but a hot miss is
  slow until the next start.
- **Subtle FP differences**: a desync or physics difference would show up only in long play. The
  differential tests in phases 1–2 against FEX are the main defence, and should keep running in CI
  on the PC.
- **Game updates and other dumps**: everything is keyed by image hash and nothing is hard-coded,
  so other versions recompile from scratch.
