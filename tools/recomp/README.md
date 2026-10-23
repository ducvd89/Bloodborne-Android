# bb-recomp (PC tool, step 1: lifter skeleton and IR census)

Lifts the game's x86-64 code with FEXCore's own front end (decoder, OpcodeDispatcher, IR passes) and
reports what the FEX IR looks like. Nothing is lowered to LLVM yet (`-DBB_RECOMP_LLVM=ON` is reserved
for step 2). Design and plan: `docs/STATIC_RECOMPILE_PLAN.md`.

## Build

Linux x86-64, Clang. FEX only allows an x86-64 host with `ENABLE_X86_HOST_DEBUG` (its CI mode); the JIT is
compiled but never run: `bb-recomp` builds a thread state with the decoder and the dispatcher builder
and nothing else (`lifter.cpp`).

    cmake -S tools/recomp -B out/recomp-build -G Ninja -DFEX_GIT_DIR=/path/to/BloodbornePC/third_party/FEX
    nice -n 19 cmake --build out/recomp-build -j 22      # about 20 s on 22 cores after the first FEX build
    # -> out/recomp-build/bb-recomp

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

Options: `--host armv8.2|armv8.4|armv9`, `--no-multiblock`, `--no-passes`, `--tso`, `--validate`,
`--no-gap-units`, `--jobs`, `--every N`. The process renices itself to 19 and uses at most 22 threads.

## How it works

- `elf_image.cpp`: maps the PT_LOAD segments at `base + vaddr` (default base 0x200000000: vaddr 0 cannot be
  mapped). FEX decodes from host memory at the guest RIP, so IR addresses are `base + vaddr`; the lowering
  subtracts the base.
- `fde_scan.cpp`: `.eh_frame_hdr` table -> 162,959 functions (identical to `ehscan.py`).
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

## What the census found (CUSA03173 eboot, armv8.4 profile, Multiblock=1, TSO off)

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
