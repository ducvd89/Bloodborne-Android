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
| Linked modules | `libc.prx` (0.9 MB) and other `sce_module/*.prx`: translated the same way |

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

## Compile time estimate (2026-10-24, proxy measurement)

These numbers come from synthetic "lifted" C, not from the real game code:

- **The proxy.** A generator writes C in the style of a lifted x86 function: guest state in a
  struct, flags computed explicitly, guest memory through raw pointers. It matches the game's
  function sizes, branch density, call and AVX mix.
- **How it was compiled.** NDK 29 clang for `aarch64-linux-android30`, one core of the Ryzen AI 9
  HX 370, under `nice 19`.
- **Results:**
  - A 500-function shard (28.7 k x86 instructions) took 5.4 s at -O1: front end 0.27 s, optimizer
    2.0 s, codegen 2.9 s. That is about **5.3 k x86 instructions/s per core**, and about 2.1 ARM64
    instructions per x86 instruction.
  - 2,000-function shards (120 k instructions) took 54–101 s each. That is superlinear: the
    interprocedural passes (IPSCCP, function attrs, inliner) grow with module size. So use small
    shards and a custom pass pipeline without IPO.
  - Peak memory was 850 MB per job with 2,000-function shards.
- **Extrapolated to the whole game.** 11.27 M x86 instructions ≈ **35 core-minutes on a Zen 5 core**
  with plain -O1. With a tuned pipeline (no IPO, IR emitted directly with no C front end, fast
  instruction selection for cold code) it would be about 20–25.

Converting to an 8 Gen 2 (Thor: 1× X3 3.19 GHz, 4× A715/A710 2.8 GHz, 3× A510 2.0 GHz):

- **Assumed speed of each core, relative to one Zen 5 core, for compiler work:** X3 ≈ 0.6,
  A715 ≈ 0.4, A510 ≈ 0.13.
- **The whole chip:** about 2.7 Zen 5 cores, or about 2 sustained once it heats up.
- **Memory:** 4–6 jobs on an 8 GB phone; all 8 on a 12–16 GB device.

| What is compiled | Thor (fan, sustained) | 8 Gen 2 phone (throttles) |
|---|---|---|
| Whole game, plain -O1 | about 18–25 min | about 25–40 min |
| Whole game, tuned pipeline, cold code with fast codegen | about 8–12 min | about 12–20 min |
| Hot functions only (corner D, about 0.3–1 M instructions, -O2) | about 1–3 min | about 2–5 min |
| A patch or hook change (only the changed functions) | seconds | seconds |

Huge functions are a risk to these numbers. The real game's code is heavily skewed: the median
function is 62 bytes, but the largest 10 % hold 63 % of the code, and the biggest is 78 KB (about
20 k instructions). Per-function passes such as register allocation, GVN and scheduling can be
superlinear on such functions, so add up to 1.5–2× on the whole-game rows until phase 0 measures
the real IR. A quick check on the device itself: Termux's clang running the same benchmark on the
Thor.

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
