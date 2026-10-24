# bb-recomp (PC tool: FEX lifter, IR census, LLVM lowering, ARM64 object/.so, ASM test runner)

Lifts the game's x86-64 code with FEXCore's own front end (decoder, OpcodeDispatcher, IR passes), reports what
the FEX IR looks like (step 1), lowers it to LLVM IR and compiles it to an AArch64 object or Android `.so`
(step 2: `lower_*.cpp`, `codegen.cpp`, `compile.cpp`), and runs FEX's `unittests/ASM` through the same lowering
on the x86-64 host (`bb-recomp-asmtest`). Design and plan: `docs/STATIC_RECOMPILE_PLAN.md`; how the compiled
code runs: `docs/RECOMP_ABI.md`.

## Build

Linux x86-64, Clang. FEX only allows an x86-64 host with `ENABLE_X86_HOST_DEBUG` (its CI mode); the JIT is
compiled but never run: `bb-recomp` builds a thread state with the decoder and the dispatcher builder
and nothing else (`lifter.cpp`).

    cmake -S tools/recomp -B out/recomp-build -G Ninja -DFEX_GIT_DIR=/path/to/BloodbornePC/third_party/FEX
    nice -n 19 cmake --build out/recomp-build -j 22      # about 20 s on 22 cores after the first FEX build
    # -> out/recomp-build/bb-recomp and out/recomp-build/bb-recomp-asmtest

`-DBB_RECOMP_LLVM=ON` is the default: the system LLVM (23, shared library, with the AArch64 and X86 targets) is
linked. `-DBB_RECOMP_LLVM=OFF` builds the lifter and census only. The NDK for `--so` is
`.local-deps/android/sdk/ndk/29.0.14206865` of the main checkout (`--ndk` to change).

nasm for the ASM tests is not packaged here: build it once from source into `.local-deps/nasm` (ignored by git):

    mkdir -p .local-deps/nasm/dl && cd .local-deps/nasm/dl && curl -fsSLO https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/nasm-2.16.03.tar.xz
    tar xf nasm-2.16.03.tar.xz -C .. && mkdir -p ../build && cd ../build
    nice -n 19 ../nasm-2.16.03/configure --prefix=$PWD/../install && nice -n 19 make -j22 install

`FEX_GIT_DIR` defaults to `third_party/FEX`, which is empty in a git worktree. FEX's own submodules (fmt,
unordered_dense, ...) are not initialised there either, so `fetch-fex.sh` makes a prepared tree in
`out/recomp-fex` (`git archive` plus shallow fetches of the pinned submodule commits; needs network once,
the checkout is not modified). Pass `-DFEX_SOURCE_DIR=` to use a ready tree. Only target `bb-recomp` is
built (FEXCore, no thunks, tests, FEXConfig, Zydis, jemalloc, LTO).

## Run

    bb-recomp --elf out/eboot.elf                    # census over all FDE functions -> out/recomp/
    bb-recomp --elf out/eboot.elf --dump 0x175340    # FEX IR of the function containing that vaddr
    bb-recomp --elf out/eboot.elf --scan --funcs funcs.txt   # function list only (checked against ehscan.py)
    python3 tools/recomp/irclass.py <FEX>/FEXCore/Source/Interface/IR/IR.json out/recomp/ir_ops.csv

### Game functions against native x86 (`bb-recomp-difftest`)

    out/recomp-build/bb-recomp-difftest --elf out/eboot.elf --inputs 8 [--limit N --every K] [--func hexvaddr] [-v]

Every function of the eboot is run twice with the same random arguments: natively (the original bytes, mapped read-only in
this process at the lift base; in a forked grandchild under a seccomp filter that kills it on any system call, with a
20 ms timer) and lowered (FEX IR -> LLVM -> x86-64 -> ORC, in the worker). Return value, callee-saved registers, rsp, xmm0/1
and two 8 KB buffers (reached through pointer arguments and pointers inside the buffers) must agree. Calls and tail jumps
dispatch to other functions, compiled on demand, so call/return, `rip != retaddr`, jump tables and the gap units are
exercised too. Inputs where either side faults or times out are skipped (random arguments rarely form a valid object, and
calls into imports end in a jump to an unresolved stub); functions with `rdtsc` are skipped. Writes to the data segment
are allowed and undone after each lowered run. Result: `out/recomp/difftest.txt`.

Options: `--host armv8.2|armv8.4|armv9`, `--no-multiblock`, `--no-passes`, `--tso`, `--validate`,
`--no-gap-units`, `--jobs`, `--every N`. The process renices itself to 19 and uses at most 22 threads.
`--boot out/boot-linked.bin [--prx-dir <sce_module>] --module libc.prx` tests one module of the runtime's image (the functions of
`libc.prx`; calls may leave it). The native oracle is the real 80-bit x87 unit, while the lowering is FEX's reduced-precision x87: functions
with x87 code differ by design (status words, `fist` overflow results, extended precision), see "Where things stand".
`--dump-range <lo> <hi> <file>` writes image bytes for `objdump -D -b binary -mi386:x86-64`.

### Variants and hook slots (phase 4: patches and hooks without the fallback)

    bb-recomp ... --so out/aot/libbbaot.so --variants tools/recomp/variants [--variant-report]

`--variants` takes catalog files or directories of `*.txt` (repeatable). A catalog lists patch *groups* (`group <name> [excl=<set>]`, `w <vaddr> <bytes>` over existing code,
`c <vaddr> <bytes>` for new code such as the cheats' cave) and native hook sites (`hook <vaddr> <len> <snap|regs> [skip] [resume=...]`); format in `src/variants.h`. For every function the groups
touch, and every consistent subset of them, the function is lifted again from the patched bytes and exported as a variant the runtime selects by the hash of the bytes it finds; every hook site gets a slot test in the units that
hold it. Design: `docs/RECOMP_ABI.md` ("Variants", "Hook slots"); what writes the game's code and how each writer is handled: `docs/RECOMP_HOOKS.md`.

The catalogs are generated from the sources and checked in (they hold addresses, patch bytes and our own hook declarations, no game code):

    tools/recomp/variants/gen_patch_parts.py   > tools/recomp/variants/patch-parts.txt   # tools/android/build_patch_parts.py (needs out/eboot.elf)
    tools/recomp/variants/gen_bbsc.py                                                     # bbsc-patches.txt + hooks-bbsc.txt (gpu/bbnet20; needs the eboot and bbsc_sites)
    tools/recomp/variants/gen_hooks_shim.py    > tools/recomp/variants/hooks-shim.txt     # gpu/shim/bbport_guest_hooks.cpp
    tools/recomp/variants/gen_patchers.py                                                 # patchers.txt + hooks-patchers.txt: runs the frame rate, display and guard patchers (needs the image)
    (each has --check: exit 1 when the catalog is out of date)

Tests: `bb-recomp-asmtest --granular-selftest` (a unit entered at every instruction boundary of a granular function, flags from the context, a non-boundary refused),
`bb-recomp-asmtest --variant-selftest` (a patched function and a cave compiled as variants, run in the in-process JIT, hashes and alternatives checked),
`bb-recomp-asmtest --hook-selftest` (a hook callback fires, edits registers, flags survive, resume points in the middle of a block and at block entries, return, dispatcher jump, loops),
`tests/run_aot_patchers_test.sh` (the patchers' hook slot callbacks against the semantics of the stubs they replace), `tests/run_aot_variants_test.sh` (the runtime's classification of functions, redirects, un-dirtying and hook slots on synthetic tables) and `tests/run_guest_code_hooks_test.sh` (the shim's slot path).

Sanitizers: `cmake ... -DBB_RECOMP_SANITIZE=address,undefined` builds the tool and the test tools with ASan and UBSan (FEXCore itself uncovered). Under ASan the default image base 0x200000000 and the tests'
low fixed addresses lie in ASan's shadow gap: use `--base 200000000000` (bb-recomp), `--high-addresses` (bb-recomp-difftest) and `ASAN_OPTIONS=detect_leaks=0:protect_shadow_gap=0`.

### Gaps: granular entries, the misses log, `--extra-entries` (phase 6: native only, no FEX fallback)

The runtime logs every guest address no unit covers (`files/bbport/logs/aot-misses.log`) and, with `BB_AOT_STRICT=1` (always in the `BB_AOT_ONLY` runtime), stops there instead of
running FEX (docs/RECOMP_ABI.md, "Misses, strict mode and the misses log"). The tool closes gaps in two ways:

    --granular off|jumps|all   units of a granular function can be entered at every instruction boundary they hold (dynamic jump targets inside a function). Default `jumps`: the
                               functions with a table-like indirect jump the lifter could not resolve for sure; --granular-funcs <file> adds more
    --extra-entries <file>     addresses to make unit entries: a misses log as it is, or one hex address per line

The loop for a tester's report: take the `miss +0x...` lines of `aot-misses.log`, rebuild with them, redeploy the `.so`:

    nice -n 19 out/recomp-build/bb-recomp --elf out/eboot.elf --boot out/boot-linked.bin --prx-dir <game>/sce_module --target aarch64 --so out/aot/libbbaot.so -O1 \
        --variants tools/recomp/variants --extra-entries aot-misses.log --ndk <ndk>      # prints how many addresses were new units, mid-function units, or rejected

and to check a list without compiling (about 2 s):

    out/recomp-build/bb-recomp --elf out/eboot.elf --boot out/boot-linked.bin --prx-dir <game>/sce_module --units-only --extra-entries aot-misses.log \
        --check-entries 20,210ebc0,23a9c9c --dump-units out/units.txt     # each address: unit entry | granular entry | case of a resolved jump table | NOT COVERED (exit status 3)

`--dump-units` writes the unit table (`unit <entry> <end> <extra> <function> <granular>`), the granular functions and the weak (unresolved or only scanned) indirect jumps with the reason.

### The runtime's image: eboot + linked modules, discovery, secondary entries

    bb-recomp --elf out/eboot.elf --boot out/boot-linked.bin [--prx-dir <game>/sce_module] ...

`--boot` lifts the flat memory image the runtime builds (`scripts/link_modules.py`, format BBPROBE5: the eboot, then `libc.prx` at
image `+0x56e0000` and `libSceFios2.prx` at `+0x57c0000`; the .prx files are plaintext SELFs, so nothing needs decrypting), so
every unit address is relative to the one image base and the `.so` has one unit table for all three modules. The boot file also
carries the import names and relocations, which discovery needs. `--elf` then only supplies the eboot's program headers. Without
`--boot` (plain `eboot.elf`) there are no relocations and no setjmp detection. The module names come from `link.json` next to the
boot file. Steps before lifting (each prints a line): the non-FDE entries (module starts, PLT stubs), discovery
(`--no-discovery`), landing pads and setjmp sites (`--no-secondary`). Switch tables are resolved unless `--no-switch-tables`.
`--no-dirty-check` drops the per-unit `bbr_dirty[]` test (default on, +8.5 % `.text`). `--target x86_64 --so` links a PC runtime library with clang+lld. `--resume-entries` adds the resume switch to every unit (docs/RECOMP_ABI.md). `bbr_image_hash` is the FNV-1a of the flat image.

Compile resources: `--plan-only` prints the plan; the shard and worker counts follow the available RAM (`--mem-fraction`, default
0.6; `BBR_FAKE_MEM_GB=12` pretends a smaller PC). `BBR_DEBUG_JUMPS=1` prints the instructions before every indirect jump.

### LLVM modes

    bb-recomp --elf out/eboot.elf --emit-ll 0x175340 f.ll        # LLVM IR of one function (all its units), before optimisation
    bb-recomp --elf out/eboot.elf --emit-ll all out/ll/g.ll --limit 2000
    bb-recomp --elf out/eboot.elf --func 0x175340 --obj f.o --shards 1 -O1   # one function to an AArch64 object
    bb-recomp --elf out/eboot.elf --obj out/obj/game.o --shards 64 -O1       # everything: game.000.o ... game.063.o + game.table.o
    bb-recomp --elf out/eboot.elf --so out/so/libbbaot.so -O1                # compile and link with the NDK's ld.lld
    bb-recomp ... --target x86_64                                            # same code for the PC (the test runner uses this)

`--shards N` (default 64; 1 for `--emit-ll`) splits the functions into N modules of equal code size, compiled on up to 22
threads at nice 19. The pass pipeline has no interprocedural passes (SROA/mem2reg, EarlyCSE, InstCombine, SimplifyCFG, DSE;
GVN and a second round at -O2). `--verify-llvm` runs the LLVM verifier on every module. A summary goes to stdout and
`out/recomp/compile.txt`: counts, CPU time per phase, `.text`, peak RSS, and the IR ops that still become `bbr_trap`.
`--flags-at-calls` and `--no-callee-saved` switch off two ABI shortcuts (RECOMP_ABI.md).

### ASM tests (`bb-recomp-asmtest`)

    out/recomp-build/bb-recomp-asmtest                      # every unittests/ASM test -> out/recomp/asmtest.txt
    out/recomp-build/bb-recomp-asmtest --filter Primary/ -v # a subset, one line per test
    BBR_DUMP_IR=1 BBR_DUMP_LL=1 out/recomp-build/bb-recomp-asmtest --filter TwoByte/0F_F5   # FEX IR and our LLVM IR of one test

Each test runs in a forked child (the tests map fixed addresses): nasm assembles it the way FEX's CMake does, the code is
lifted with the front end, lowered, compiled for the host with LLVM ORC, run on a default `CPUState` until `hlt`, and
`RegData` is compared. Skipped without trying: MMX/3DNow (27), self-modifying code, 32-bit, SHA/CLZERO/RAND
tests, FEX's own disabled and known-failure lists.

x87: FEX tests it in two modes. `unittests/ASM/X87` and every other x87 test without an `Env` key expect FEX's default x87 (80-bit
precision, `X87ReducedPrecision=0`); `unittests/ASM/X87_F64` (and the tests with `"Env": {"FEX_X87REDUCEDPRECISION": "1"}`) expect the
reduced mode, where x87 runs as F64 (`X87F64.cpp`, `x87StackOptimizationPass.cpp`). The game runs reduced (`tools/android/fex-compatible.json`:
`X87ReducedPrecision=1`, `TSOEnabled=0`, `Multiblock=1`) and the lifter sets exactly that (`lifter.cpp`, `setConfig`), so the reduced
tests are the ones that apply and are counted in `TOTAL`. The 80-bit ones still run, in the category `x87 80-bit (info)` outside the
totals: they check extended significands in MM registers, x87 exception flags and 80-bit rounding, which the reduced mode does not
have by design (the game never needs them).

### x87 helpers (`tools/recomp/helpers`)

Ops with no sensible inline form call `bbr_x87_*` functions: the 80-bit memory conversions of reduced mode (`fld/fstp tbyte`: `F80CVT`,
`F80CVTTo`), the integer and BCD conversions, and the F64 transcendental and remainder ops (`F64FPREM/FPREM1/SCALE/F2XM1/FYL2X/FYL2XP1/ATAN/TAN/SIN/COS`).
They are FEX's own fallback handlers (`F80Fallbacks.h`, MIT) over the SoftFloat 3e that FEX bundles (BSD-3; a subset is copied to
`helpers/softfloat`, see `helpers/NOTICE`), as plain C. `bb-recomp` compiles `helpers/bbr_x87_all.c` for the target with that target's own clang
(`--ndk` clang with `--target=aarch64-linux-android30`; the system clang for `--target x86_64`), writes it as `<out>.helpers.o`, and
links it into the `.so` (with libm as `DT_NEEDED`, `--helpers-dir` to use other sources). The runtime provides nothing for them
(docs/RECOMP_ABI.md, "x87 helpers"). The test tools link the same file and register the symbols with the in-process JIT.
`helpers/test/test_x87.c` checks the conversions against the host's x87 unit (6.4 M random and edge-case values, 4 rounding modes): 0 mismatches
(only unnormal 80-bit encodings differ).

## How it works

- `elf_image.cpp`: maps the PT_LOAD segments at `base + vaddr` (default base 0x200000000: vaddr 0 cannot be
  mapped). FEX decodes from host memory at the guest RIP, so IR addresses are `base + vaddr`; the lowering
  subtracts the base.
- `fde_scan.cpp`: `.eh_frame_hdr` table of every module -> 162,959 + 2,814 functions (the eboot list is identical to `ehscan.py`);
  `addNonFdeEntries`: module starts and PLT stubs. `elf_image.cpp`: the ELF loader and the boot-linked image loader.
- `jumptable.cpp`: recognises `cmp/ja; lea; movsxd; add; jmp` (and variants) in the decoded instructions of a unit and reads the
  rel32 table. The lifter adds the case targets (and in-function branches the decoder would not follow, > 4 pages ahead) as block
  entries and decodes again until nothing new appears; the lowering turns the `jmp` into an LLVM `switch`.
- `discover.cpp`: code without an FDE (relocation pointers, `lea`, branch targets outside every function), to a fixpoint.
  `eh_frame.cpp`: landing pads from the LSDAs, setjmp call targets from the boot file's imports.
- `lifter.cpp`: per function, `SetupDecodeInstructionsAtEntry` + `DecodeLoop` (multiblock, bounded to the
  FDE range through the decoder's `SectionMax/MinAddress` and `QueryGuestExecutableRange`), then
  OpDispatchBuilder per instruction, `Finalize`, and the passes. This repeats `ContextImpl::GenerateIR`
  (Core.cpp:521) because that runs FEX's PassManager, which ends in register allocation; LLVM wants the
  SSA IR before it. Passes run: `CreateX87StackOptimizationPass` and
  `CreateDeadFlagCalculationEliminination` (RedundantFlagCalculationElimination.cpp:751).
- Code the entry unit cannot reach (switch case bodies, only reached through the jump table) is found by
  sweeping the uncovered bytes and lifted as extra units.
- Failures are caught per function (signals and FEX asserts longjmp back; the unit is rebuilt).
  Release FEX compiles its asserts out, so use `--validate` for IR checks.

## Where things stand (2026-10-24, step 2c)

- **ASM tests** (x86-64 host, -O1; step 2d: x87 enabled): 1755 pass, 5 fail, 192 not run because an op is still unlowered, 53 skipped:
  99.7 % of the tests that run pass. `bb-recomp-asmtest --resume-selftest` checks the resume entries (PASS). Every category
  except the unlowered-op ones is 100 %, with **X87_F64 149/150** (the reduced-precision x87 suite, 150 tests; the one failure,
  `D9_F0_02_F64`, expects f2xm1(0.5) = 0x3FDA827999FCEF32 but FEX's own handler is `exp2(x) - 1.0`, which is 0x...34 with every libm
  we tried: the test was added with an inline JIT sequence that this FEX version does not have, so the helper keeps FEX's handler),
  Primary 197/197, PrimaryGroup 152/152, Secondary 72/72, TwoByte 220/220, Atomics 12/12, ConstProp, Flags, REP/REPNE 67/67,
  OpSize 110/110, SSE4a 4/4, VEX 329/332, FEX_bugs 93/93, (top level) 16/17. The other 4 failures: `DAZTest` (denormals-are-zero needs
  FPCR.FIZ, FEAT_AFP) and `vcvtps2ph_rd/ru/trunc` (a half-precision conversion with a rounding mode other than nearest: LLVM's x86
  `fptrunc` ignores MXCSR; on ARM64 the rounding helpers set FPCR). The 192 tests not run need ops the game never uses (AVX2
  gathers and masked moves 105, string compares 39, FMA 14, AES/SHA/PCLMUL ~30). The 80-bit x87 tests (`x87 80-bit (info)`, outside the
  totals): 85 of 257 pass in reduced mode, `X87/` 56 of 214; of the 158 `X87/` failures 92 have an `X87_F64` sibling that passes (the reduced
  expectation of the same instruction) and 66 are precision, invalid-operation and status-word tests with no reduced counterpart.
- **Whole image (eboot + libc.prx + libSceFios2.prx, 182,663 functions, 184,695 units, 54.4 M IR ops)**: no lift failure; every
  module passes the LLVM verifier; **0 units contain a `bbr_trap`** (step 2c had 118 in libc.prx: `F80CVT` 104, `F80CVTTo` 82, `F64FPREM1` 6,
  `F64SCALE` 3, `NZCVSelectV` 1, now lowered: x87 helpers above, NZCVSelectV an LLVM select). Timings are in "Compile time" in
  `docs/STATIC_RECOMPILE_PLAN.md`: 62 s, 102.9 MB `.text`, 123.8 MB `.so`, 14.2 GB peak.
- **Discovery** (details in the plan, "Discovery"): FDE bytes reached from entries alone 92.00 % -> 98.84 % (the rest is padding and
  table data); 3,847 of 15,562 indirect jumps are jump tables and resolve (57 table-like ones do not; 8,848 are register tail calls,
  2,810 vtable tail calls); 692 landing pads, 132 setjmp return sites, 16,890 pseudo-functions (2.8 MB of vtable thunks and static
  initialisers that only data pointers reach, 816 PLT stubs, 2 module starts); call-return resume entries cost +23 % `.text` and are optional.
- **Differential test on the game** (`bb-recomp-difftest`, 6 inputs per function, all 162,959 functions, x86-64 host,
  -O1; re-run after jump tables became LLVM switches, with the same 30 mismatches as before): 60,795 functions agree on at least
  one input, 100,427 are skipped (every input faulted or timed out, mostly
  random arguments that are not valid objects), 1,436 skipped for `rdtsc`, 271 not liftable by the in-process lifter,
  **30 mismatch**. None is a lowering bug: 18 use `rcpps`/`rsqrtps` (x86 returns a 12-bit table estimate, the lowering
  computes 1/x; the code refines with a Newton step, so only the last bits differ), 2 differ only in the sign of a
  default NaN (x86 `ffc00000`, ARM `7fc00000`), 10 return host addresses (a tail jump into an import stub that
  allocates; the native and lowered runs live in different processes). One real bug was found and fixed on the way: LLVM
  23's x86 `x86-fixup-bw-insts` rewrites `movb 552(%rcx),%cl` to `movzbl` although the upper bits of rcx are live,
  which drops the old upper bits of a byte-merge such as `mov al,[rax+0x228]` (about 26 functions). The x86 host path
  now sets `-fixup-byte-word-insts=0`; the AArch64 backend is not affected.
- **Differential test on libc.prx** (`bb-recomp-difftest --boot out/boot-linked.bin --module libc.prx --inputs 8`, 2,164 functions): 852 ok,
  1,272 skipped (every input faulted or timed out), 7 rdtsc, **33 mismatch**, none in non-x87 code except one that stores a host
  address: 26 contain x87 instructions and 6 more call such functions (`fmod`/`remainder`-style `fprem1` wrappers, `fistp`, `fucomip`
  chains), where native x86 is the true 80-bit unit (status words 0x7200 against 0x3000, `fist` of NaN or overflow gives the integer indefinite
  natively and 0 / saturation in the reduced lowering) and the lowering is FEX's reduced x87; the last one is `__setjmp`-style code (`0x5733d76`)
  that saves its return address, a host address in the native run. An x86-64 run of the reduced x87 against a true oracle is the
  X87_F64 suite above. The eboot, re-run with `--every 8` (20,370 functions) after the change: 7,490 ok, 3 mismatch (xmm, the known `rcpps`/NaN
  classes), the harness no longer reports "not liftable" for functions that an earlier call had already compiled (`liftfail`: duplicate symbol).
- The lowering follows the Arm64 JIT op by op. Lowered: every op the census lists (204 of 424), the scalar and vector ops FEX's tests use,
  and the x87 ops of the reduced mode. Not lowered: the 80-bit F80 arithmetic ops (`X87ReducedPrecision=0`: the game does not use it), AES/SHA/PCLMUL,
  `VPCMP*STRX`, FMA (`VFMLA*`), masked loads/stores and gathers.

## What the census found at step 1 (CUSA03173 eboot, armv8.4 profile, Multiblock=1, TSO off; before jump table resolution: see above for the current numbers)

- 162,959 of 162,959 functions lift cleanly; no failures, no crashes, IR validation passes on every unit.
  The whole image takes 0.32 s wall (6 s CPU), 37 M x86 instructions/s.
- 11.81 M x86 instructions: 9.39 M in entry units, 2.42 M in 39,566 extra units (2,525 functions).
- 61.2 M IR ops (5.18 per x86 instruction); 39.6 M (3.36 per instruction) without `Inline*` constants,
  block markers and `GuestOpcode`.
- 204 of 424 IR.json ops are used; 220 never appear (all F80, all crypto/AES/SHA/PCLMUL/CRC32, all F64
  transcendentals, 256-bit vectors, SVE masked ops, many NEON-specific vector ops; list in
  `out/recomp/ir_ops_unused.txt`).
- Coverage: 98.92% of FDE bytes are in lifted blocks. The rest is padding 0.18 MB and switch tables
  0.27 MB (rel32 tables sit in .text inside the FDE); 19 bytes undecodable.
- Without the extra units, only 92.0% of FDE bytes are reached: about 7% of the code is switch case bodies
  that need jump-table resolution or extra entry points.
- Host profile changes the IR by under 2% (armv8.2 62.2 M ops, armv9 61.2 M). The x87 and dead-flag passes remove
  13% of the ops (70.3 M without them). Single-block mode makes 1.6 M extra units, so Multiblock is the right
  setting. TSO adds 1%.

Top IR ops: LoadRegister 20.1%, StoreRegister 13.9%, GuestOpcode 12.6%, InlineConstant 9.2%, LoadMem 4.2%,
CodeBlock/BeginBlock/EndBlock 4.2% each, StoreMem 2.3%, EntrypointOffset 2.1%, Constant 1.9%, Push 1.7%,
CondJump 1.6%, ExitFunction 1.5%. Categories: StaticRA 36%, Misc 27%, ALU 21%, Memory 11%, Branch 4%,
Vector 1.3% (72 of 119 ops used). The full list is in `out/recomp/ir_ops.csv`, op sizes in
`ir_ops_by_size.csv` (no op is wider than 128 bits).
