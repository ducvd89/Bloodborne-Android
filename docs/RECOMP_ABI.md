# bb-recomp: how recompiled code runs (ABI, 2026-10-24)

Shared design for the LLVM lowering (tools/recomp) and the runtime backend (cpu/guest_cpu_aot.cpp).
See STATIC_RECOMPILE_PLAN.md for the plan.

## Units

- **One unit per FEX IR multiblock.** That is one FDE function (the *entry unit*), plus extra units for code the entry unit does
  not reach: gap units (leftovers of unresolved jump tables, dead code) and *secondary entries* (below). Each unit becomes one
  LLVM function: `void bbr_<vaddr hex>(GuestState *s)`, with external linkage and the C calling convention.
- **One image.** The `.so` covers the eboot and the linked system modules (`libc.prx`, `libSceFios2.prx`) in the layout the
  runtime builds (`scripts/link_modules.py`, `out/boot-linked.bin`): a module sits at its own image offset (`image +0x56e0000` for
  libc.prx, `+0x57c0000` for libSceFios2.prx) and every unit address, in the table and in the code, is relative to the one image
  base. `bb-recomp --boot out/boot-linked.bin` lifts that flat image (so the bytes are the ones the runtime maps, `mov rax, fs:[0]`
  already rewritten to `gs:` by the linker); `--elf` only supplies the eboot's program headers.
- **Guest addresses** in the IR are image vaddr + the lift base. The lowering emits *image-relative*
  addresses (vaddr) plus a runtime image base, a global `bbr_image_base` that the loader sets. The
  `.so` is then position independent, and the runtime may map the image anywhere (on Android
  today: 0x800000000).
- **Unit table.** The `.so` exports `bbr_units[]`, a sorted array of `{ uint64_t vaddr; void (*fn)(GuestState*); }`
  with one entry per unit: every FDE start of every module, every gap unit and every secondary entry. It also exports
  `bbr_unit_count`, `bbr_image_hash` (FNV-1a 64 of the image the `.so` was built from: the flat boot image, or the ELF file for a
  plain `--elf` build), `bbr_abi_version` (1) and the writable `bbr_image_base`. The unit functions themselves have hidden
  visibility: the table is the only way in.
- **Per-unit tables** (additive, ABI version still 1; indexed like `bbr_units[]`): `bbr_unit_end[i]` (u64: the vaddr just past the
  last decoded byte of unit i; the unit's code lies in `[vaddr, end)`, with holes where other units' code or table data sit),
  `bbr_unit_extra[i]` (u8: 1 = not the start of a function: case body, landing pad, setjmp return site, jump target into the middle
  of a function; it belongs to the function of the nearest preceding unit with 0 and is entered by jumps), `bbr_unit_hash[i]`
  (u64 FNV-1a of the image bytes `[units[i], units[i+1])`, the last unit of a segment to the segment end; byte 0x65 hashed as
  0x64 because bb-prepare turns `fs:` into `gs:`), `bbr_unit_ehash[i]` (the same over the exact range `[vaddr, bbr_unit_end[i])`,
  for marking units dirty without false positives from neighbours), `bbr_text_end` (end of the last executable segment).
- **Dirty units (default on; `--no-dirty-check` removes it).** The runtime must be able to take a unit out of service when its
  code was patched or hooked without touching the `.so` text (Android's SELinux may refuse `execmod`). The `.so` exports a
  writable `uint8_t bbr_dirty[unit_count]`; the first thing every unit does is
  `if (__builtin_expect(bbr_dirty[i], 0)) { bbr_dirty_entry(s, vaddr); return; }` (a tail call, `vaddr` image-relative) (i = the unit's index in `bbr_units[]`;
  one byte load and a predictable branch). `bbr_dirty_entry(GuestState*, uint64_t vaddr)` has the contract of `bbr_dispatch`
  with the table lookup skipped (the unit does not set `s->rip`: the runtime sets it to `image_base + vaddr`): run the guest code at that address on the fallback CPU until the guest returns to the address on top
  of the stack (a call) or leaves (a jump). With resume entries on, a dirty unit entered with the resume flag set returns at
  once with the flag still set (it is not the place to resume). Direct calls and tail jumps between units go straight to the
  target unit, which tests its own byte, so no stale unit runs. `bbr_abi_flags` bit 1 = the check is present. The runtime
  marks a function dirty by setting the bytes of all its units (`bbr_funcs[]`). Cost: about 6 MB of code for 184 k units (see the plan).
- **Direct jumps and calls to any unit.** A constant call or jump target that is the entry of any unit (not only a function
  start) is a direct call (`musttail` for jumps), and every in-function target that another unit jumps or calls into, or that
  can be entered from outside its own unit (a tail jump into the middle of a function, a case body reached from a gap unit), is a
  unit entry: the tool finds them by lifting every unit and collecting its direct branch/call targets that lie outside its own range
  (432 in the game, mostly in a handful of large functions), lifts them as units (`bbr_unit_extra` = 1) and repeats to a fixpoint.
- **Function table** (additive, ABI version still 1). `bbr_funcs[]`, `bbr_func_count`: `{ uint32_t start, end, first_unit, unit_count; }`
  per FDE function in address order, where `first_unit .. first_unit+unit_count` is the run of `bbr_units[]` entries that lie in
  `[start, end)` (entry unit first). A dispatcher that has to resume in the middle of a function (below) finds the function by
  binary search and tries those units. `bbr_abi_flags` (u64): bit 0 = the units have resume entries.
- **Code without an FDE** (needs the boot-linked image, which carries the relocations). The runtime also enters code no FDE
  describes; each of these is a pseudo-function (a unit bounded by the next function or entry): the eboot entry and the
  linked modules' start routines (`image +0x56e0000`, `+0x57c0000`: hand-written, run by `guest_cpu_call` at start-up), the
  816 PLT stubs (`jmp [rip+GOT]`, which an import call or a vtable reaches), and **16,070 thunks and static initialisers**
  (2.8 MB) that data pointers reach: every relocation that stores an image address inside a module's code range and lands
  outside any FDE function (vtable thunks, `.init_array`), plus branch and `lea [rip+x]` targets outside every function,
  iterated to a fixpoint. Without them every virtual call through a thunk and every static initialiser would miss the unit table.
  A call or jump target that lies inside another function but is no unit entry gets a unit too (none in the game).
- **Secondary entries.** Addresses the runtime can enter without a function start get units of their own, bounded to their FDE
  (a multiblock lifted from that address; the code that follows is duplicated from the entry unit, which costs little because
  there are few):
  - *C++ landing pads*: the call-site tables of every FDE's LSDA (`.eh_frame` CIE/FDE augmentation, `gcc_except_table`).
    The unwinder (`_Unwind_*` in libc.prx, `__Ux86_64_setcontext`) resumes there with a register restore and a jump.
  - *setjmp return sites*: the return address of every `call` to `setjmp`, found by the import name (`gNQ1V2vfXDE`, the NID of
    `setjmp`, in the boot file's import table; the binding gives libc.prx's export, the relocations give the GOT slot and
    the PLT stub in the eboot). `longjmp` ends in a jump to the saved return address, which lands in the middle of the caller.
  Case counts are in `docs/STATIC_RECOMPILE_PLAN.md` ("Discovery").
- **Resume entries (every call-return block; optional, `--resume-entries`).** When a guest `ret` or `jmp` lands on a call-return
  address (the address after a `call`) that is not an entry above, for instance after a stack switch (a fiber or coroutine
  restores another guest stack and `ret`s into a caller that has no host frame), the dispatcher can enter the unit that holds
  the address instead of falling back to the slow path. Design: **no per-site table and no per-site stub.** Each unit that
  contains calls gets a resume switch right after its prologue (which already loads every register it uses from `s`):

      if (s->resume != 0) {            // GuestState offset 0xb8: FEX's `_pad1`, unused by the lowering
          s->resume = 0;
          switch (s->rip - image_base) { case <return address k>: goto the call-return block k; ... }
          s->resume = 1; return;       // not a call-return block of this unit: the dispatcher tries the next unit
      }

  The call-return block is the IR block after the call; entering it from the prologue is valid because the state is exactly what
  the ordinary path has there (all registers in `s`, flags dead). The dispatcher (`bbr_dispatch`/`bbr_dispatch_tail` on a miss in
  `bbr_units[]`): binary-search `bbr_funcs[]` for the function containing `s->rip`, then for each of its units in order set
  `s->resume = 1` and call it; when it returns with `s->resume` still 1 try the next unit; when none takes it, use the fallback
  CPU. Cost (measured on a 20,000-function sample at -O1, then the whole game): see the plan, "Resume entries". It is **off by
  default**; `bbr_abi_flags` bit 0 tells the runtime whether the `.so` has it.

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
  - **Switch tables.** The lifter recognises the compiler's jump table pattern (`cmp idx, N; ja default; lea base, [rip+T];
    movsxd off, [base+idx*4]; add off, base; jmp off`, also `jbe/jb` into the table code, an `and idx, mask` bound, the
    base kept in another register or set before a loop) and reads the rel32 table from `.text`. The case targets are added as
    block entries of the same multiblock unit, and that `jmp` becomes an LLVM `switch` on `target - image_base` over the case
    blocks, with `bbr_dispatch_tail` (the code above) as the default: an address that is not a case (a table the
    recogniser got wrong, or a computed target) still works, only slower. Registers and flags are not spilled on the switch
    edges (the case blocks are blocks of the same unit); the default edge spills as for any tail jump.
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
