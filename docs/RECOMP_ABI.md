# bb-recomp: how recompiled code runs (ABI, 2026-10-24)

Shared design for the LLVM lowering (tools/recomp) and the runtime backend (cpu/guest_cpu_aot.cpp).
See STATIC_RECOMPILE_PLAN.md for the plan.

## Units

- **One unit per FEX IR multiblock.** That is one FDE function, plus the extra (gap) units the
  census found. Each unit becomes one LLVM function:
  `void bbr_<vaddr hex>(GuestState *s)`, with external linkage and the C calling convention.
- **Guest addresses** in the IR are image vaddr + the lift base. The lowering emits *image-relative*
  addresses (vaddr) plus a runtime image base, a global `bbr_image_base` that the loader sets. The
  `.so` is then position independent, and the runtime may map the image anywhere (on Android
  today: 0x800000000).
- **Unit table.** The `.so` exports `bbr_units[]`, a sorted array of `{ uint64_t vaddr; void (*fn)(GuestState*); }`
  with one entry per unit entry (every FDE start plus every gap unit). It also exports `bbr_unit_count`,
  `bbr_image_hash` (FNV-1a 64 of the patched image file the `.so` was built from), `bbr_abi_version` (1) and
  the writable `bbr_image_base`. Added by the runtime step (2026-10-24): `bbr_unit_hash[unit_count]` (FNV-1a 64 of the bytes
  `[units[i], units[i+1])` of the executable segment, the last unit to `bbr_text_end`; byte 0x65 hashed as 0x64 because
  bb-prepare turns `fs:` into `gs:` in the image) and `bbr_unit_extra[unit_count]` (1: a switch case body, part of the function
  of the nearest preceding unit with 0). The unit functions themselves have hidden visibility: the table is the only
  way in.
  *Change from the first draft:* call-return blocks are **not** table entries. Re-entering the middle of a
  unit would need an entry stub per call site (about 1.6 M in the game) and a resume switch in every unit.
  Only a longjmp/exception resume lands there, and the rare case goes to the fallback CPU through the
  `bbr_dispatch` miss path, like any other address that is not a unit entry.

## Guest state

- **`GuestState`** is FEX's `FEXCore::Core::CPUState` layout. Use the offsets from FEX's headers,
  so `LoadContext`/`StoreContext` offsets in the IR apply unchanged. `GuestState*` is the only
  argument and is `noalias`.
- **Static-RA registers.** `LoadRegister`/`StoreRegister` on GPR n and FPR n map to a local `alloca` per
  register (GPR 0-15 in x86 order at `gregs[n]`, 16 = PF and 17 = AF at `pf_raw`/`af_raw`, 32-bit; FPR n at
  `xmm.sse.data[n]`, the 128-bit layout of a host without SVE256; the YMM high halves are `avx_high`, reached
  through `LoadContext`/`StoreContext`). Each register the unit touches is loaded from `s` at unit entry.
  The registers the unit has *modified since the last store* are stored back before every exit and call (a
  dataflow over the unit's blocks tracks which those are). After a call that returns to the expected address,
  the caller-saved registers (rax rcx rdx rsi rdi rsp r8-r11, all XMM) are reloaded; **rbx, rbp and r12-r15 are
  not**: the SysV ABI guarantees the callee restored them, and the caller's copy then stays in a host register
  across the call. (`--no-callee-saved` reloads them too; the ASM test runner does.) A callee that returns with
  different callee-saved registers (setjmp/longjmp, a context switch) goes through the `rip != retaddr` path
  anyway. `mem2reg`/`SROA` keep the registers in host registers.
- **`GuestState` initialisation** is the runtime's job and must match FEX's thread setup: a default
  `CPUState` (NZCV word with C set, DF raw = 1, `pf_raw` = 1, MXCSR 0x1F80) plus the segment arrays
  (`segment_arrays[GDT] = segment_arrays[LDT] = gdt[32]`, `cs_idx = 6 << 3`, a 64-bit code segment): the IR
  reads the GDT for segment-register instructions.
- **Flags** are four `i1` allocas, N, Z, C and V, with ARM semantics as FEX defines them:
  - `SubNZCV a,b`: N = sign(a-b), Z = (a==b), C = a≥b unsigned, V = signed overflow.
  - `AddNZCV`, `*WithFlags`, `CarryInvert`, `NZCVSelect`, `CondJump <cond>` read and write these
    allocas.
  - Conditions are built with `icmp` on the original operands where possible, so LLVM folds them into
    `cmp`+`b.cond`.
  - PF and AF go through `StorePF`/`StoreAF` into the context, as the IR says. They are dead in
    almost all code and DSE removes them.
  - Flags are materialised lazily: a flag-setting op only records its operands, and a condition built in
    the same block is one `icmp` on them. At block ends the flags go to the four allocas (dead stores die in
    `mem2reg`/DSE). At tail-jump exits (`ExitFunction ... None`) and at `Break` they are stored to the context
    as the packed NZCV word (`flags[24]`, N<<31 | Z<<30 | C<<29 | V<<28), but only if the unit writes flags at all.
    x86 flags are dead across calls and returns, so those two do not store them (`--flags-at-calls` restores the
    store), and nothing reloads them after a call. PF and AF (`pf_raw`, `af_raw`) follow the flags.
- **Guest memory.** Guest pointers are host pointers (shared address space). `LoadMem`/`StoreMem`
  become plain loads and stores with `align 1` (x86 allows misalignment), no TBAA, and
  `-fno-strict-aliasing` semantics. TSO is off, matching the shipped FEX config. Atomics
  (`CASPair`, `AtomicFetch*`, `CAS`, `AtomicSwap`) become LLVM atomics `seq_cst` (LSE on ARM64).

## Control flow

- **Inside a unit:** `Jump` and `CondJump` become LLVM branches between blocks.
- **`ExitFunction target, Call, retaddr, retblock`.** The IR has already pushed `retaddr` onto the
  guest stack (`Push`), so the guest stack stays a real x86 stack.
  1. If `target` is a constant and a known unit (an FDE start), call `bbr_<target>(s)` directly. Otherwise
     store `target` to `s->rip` and call `bbr_dispatch(s, target)`, which the runtime provides: a table
     lookup, then the unit, else the fallback CPU.
  2. After the call returns, compare `s->rip == retaddr`. If equal, branch to `retblock`. If not
     (longjmp, an exception, or a callee that returned elsewhere), store state and `return` from
     this unit with `s->rip` as is. The caller's own check then propagates the return outward.
     Each level unwinds until a frame's expected return matches, so setjmp/longjmp resume in the
     right frame naturally. The outermost entry (`guest_cpu_call`, a thread start) runs the
     dispatcher loop from `s->rip` when the return is not its sentinel.
- **`ExitFunction target, Return`:** set `s->rip = target` (the popped address), store state,
  `return`.
- **`ExitFunction target, None`** (tail call, `jmp` out of the unit, or an indirect jump):
  - With a constant, known target: `musttail call bbr_<target>(s)`.
  - Otherwise store the target to `s->rip` and `musttail call bbr_dispatch_tail(s)`. *Change:* the target is
    in `s->rip`, not an argument: `musttail` needs the caller's prototype `void(GuestState*)`.
  - Indirect jumps inside the unit (switch tables), once resolved, become an LLVM `switch` over block
    addresses, with dispatch as the default.
- **`bbr_image_base`** is a `uint64_t` the loader sets after `dlopen`; every unit loads it once at entry
  (`!invariant.load`) and adds it to each image-relative address. Constants that are image addresses are
  recognised by value: `EntrypointOffset`, `InlineEntrypointOffset`, and a `Constant` inside the image
  (`liftBase <= k < liftBase + image size`).
- **Host calls (imports):** the runtime gives each host function a guest-callable trap address, as
  today (`guest_cpu_host_function`). `bbr_dispatch` finds it in the same table and calls the host
  function with the SysV mapping: RDI, RSI, RDX, RCX, R8, R9 then the stack for integers, XMM0–7 for
  vectors. The result goes to RAX/XMM0, and the call pops the return address like a `ret`.
  `cpu/host_call_arm64.S` already does this marshalling for FEX.

## Helpers the lowering calls (provided by the runtime, `bbr_` prefix)

All take the `GuestState*` where they need guest registers. Registers are already stored to `s` before the
call (and reloaded after it) for `bbr_dispatch`, `bbr_syscall` and `bbr_trap` of a guest Break.

| helper | meaning |
|---|---|
| `void bbr_dispatch(GuestState* s, uint64_t target)` | non-tail call to a guest address; `s->rip == target` already. Table lookup, the unit, else the fallback CPU, which runs until the guest returns (the pushed return address) |
| `void bbr_dispatch_tail(GuestState* s)` | the same for the guest address in `s->rip` (tail jump out of a unit) |
| `void bbr_trap(GuestState* s, uint64_t kind)` | a guest Break (`hlt`, `int3`, `ud2`, ...): `kind = Signal \| TrapNumber << 8 \| si_code << 16 \| ErrorRegister << 32`; the IR has already stored the faulting `rip`. Bit 63 set: an IR op the tool does not lower (low bits = FEX IR op index, see `out/recomp/*`); bit 62: the stub for a function that could not be lifted (low bits = its vaddr). It may return: the unit then returns |
| `struct {uint64_t lo, hi} bbr_cpuid(GuestState*, uint64_t leaf, uint64_t subleaf)` | the Jaguar values: `lo` = EAX \| EBX << 32, `hi` = ECX \| EDX << 32 (the registers x0/x1 or rax/rdx) |
| `uint64_t bbr_rdtsc(void)` | `guest_cpu_tsc` |
| `void bbr_syscall(GuestState*)` | `syscall`: the handler must advance `s->rip` past the 2-byte instruction |
| `uint64_t bbr_xgetbv(uint64_t ecx)` | EAX \| EDX << 32 |
| `uint64_t bbr_processor_id(void)` | (node << 12) \| cpu |
| `void bbr_set_rounding(uint64_t round_mode, uint64_t mxcsr, uint64_t set_daz)` | `ldmxcsr` rounding/flush mode |
| `uint64_t bbr_get_rounding(void)`, `uint64_t bbr_push_rounding(uint64_t mode)`, `void bbr_pop_rounding(uint64_t saved)` | the host FP environment around rounding-specific conversions (step 2b) |
| `bbr_x87_*` / `bbr_f80_*` | only if ever needed. The census shows none in the eboot |

`__divti3`/`__udivti3` (128-by-64-bit `div`/`idiv`) come from the compiler builtins library, which the link
step adds (`libclang_rt.builtins-aarch64-android.a`).

## Test path on the PC

The lowering uses target-independent LLVM IR wherever possible, so the same IR also compiles for
x86-64 and runs on the PC:

- **`bb-recomp --target x86_64`** compiles for the PC, and the ASM test runner executes it in
  process.
- **FEX's `unittests/ASM/*.asm`** give the expected `RegData` and `MemoryRegions`.
- **Game functions** are checked against native x86 execution of the original code on the PC. Native
  x86 is the oracle, which is stronger than FEX.
- **AArch64-only intrinsics** each need a generic fallback, so the x86-64 test build keeps working.

## Runtime requests (cpu/guest_cpu_aot.cpp, 2026-10-24)

What the runtime does with the units, and what it still wants from the tool.

**Done in the runtime, no lowering change needed**

- **Image check.** The runtime cannot hash the ELF (the prepared image differs: fs->gs prefixes, then start-up patches and
  hooks), so before the first guest call it hashes each unit's range of the loaded image and compares with `bbr_unit_hash`.
  More than 25 % different: the .so is refused (wrong image). Otherwise the differing units are *dirty*: patches and hooks are
  found without any `guest_cpu_invalidate` call. `BB_AOT_ELF=<eboot.elf>` additionally compares `bbr_image_hash`.
- **Dirty units** (the above, plus `guest_cpu_invalidate`/`guest_cpu_hook` ranges later) leave the dispatch table, and the whole
  function goes (the `bbr_unit_extra` units follow their function: case bodies are entered by jumps with no return address on
  top of the stack, so they cannot run on the fallback alone). Direct unit-to-unit calls bypass the table, so the runtime also
  overwrites the entry of a dirty unit with a jump to `bbr_dirty_unit(s, vaddr)` (23 bytes on x86-64, 24 on ARM64; needs the
  .so text writable for a moment: `mprotect`). **Android's SELinux policy may refuse that** (no execmod on app files); then
  only the table changes and direct calls still run the stale unit. Request: *optional flag check at unit entry*, e.g. a
  `bbr_dirty[unit]` byte array and `if (bbr_dirty[idx]) return bbr_dirty_unit(s, vaddr)` as the first thing in each unit (one
  load and a predictable branch), or the same through a patchable slot table.
- **Import thunks.** A call to the game's PLT (`jmp *[rip+disp]`, outside every FDE) has no unit; the dispatcher follows the
  thunk to the host-function stub instead of running the fallback (5.7 M of these in 2 minutes of the title screen).

**Requests**

1. **Mid-unit jump targets.** `bbr_dispatch_tail` is called with targets that are inside a unit but start none (a switch whose
   table the tool did not resolve, jumping into the middle of another case body; 20 in 2 minutes). The code runs natively
   from there, but the stack holds no return address on top, so the runtime runs the fallback to the *end of the guest stack*
   (everything above that frame becomes native/FEX too). Better: the tool makes those targets unit entries. The runtime can
   list them (`BB_AOT_STATS=1` prints the miss addresses; the "mid-unit jumps" counter is separate): feed such a list back as
   `--entries`, or resolve the jump tables. Alternatively export `bbr_unit_end[]` and CFA info so the runtime can find the
   frame's return slot.
2. **Direct calls are not counted**; to measure AOT versus fallback the tool could emit a per-unit counter (debug builds only).
3. **Unit extents.** The runtime assumes a unit covers `[vaddr, next unit's vaddr)`; `bbr_unit_end[]` would make dirty marking exact.

**Fallback CPU conventions** (what the lowering may rely on): the guest stack is real; at a miss the return address is on top of
the stack and the fallback returns exactly there with `s->rip` = that address and rsp popped; if the guest returns anywhere else
the fallback returns with `s->rip` = a sentinel and every unit frame unwinds through the `rip != retaddr` check up to
`guest_cpu_call`, which continues from `s->rip` (a unit entry runs as a unit, anything else on the fallback to the sentinel).
x86 flags are not transferred to the fallback (dead at calls). `GuestState.InlineJITBlockHeader` (offset 0) is used by the
x86-64 native fallback to keep the host stack pointer; the lowering never touches it.
