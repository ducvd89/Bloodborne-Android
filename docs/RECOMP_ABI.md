# bb-recomp: how recompiled code runs (ABI, 2026-10-24)

Shared design for the LLVM lowering (the Rebuild Tool, the `tools/recomp` submodule: https://github.com/ducvd89/Bloodborne-android-rebuild-tool) and the runtime backend
(cpu/guest_cpu_aot.cpp). See STATIC_RECOMPILE_PLAN.md for the plan. The tool repository keeps a copy of this file; this one describes the runtime and wins where they differ.

> **Since phase 7 (2.0.0) there is no fallback CPU.** Where this document says a unit, a function or a hook "runs on the fallback", "falls back" or "is dirty", read: the
> game stops with a miss (strict mode, below). Strict mode is the only mode; FEXCore is not part of the runtime.

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
  target unit, which tests its own byte, so no stale unit runs. `bbr_abi_flags` bit 1 = the check is present (bits 2 and 3: variants and hook slots, below). The runtime
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

## Variants: precompiled patched code (additive, ABI version still 1; `bbr_abi_flags` bit 2)

The runtime patches the game's code at start-up (patch parts, the port's own hooks) and while the game runs (BBSC's toggled patches). Patches whose bytes are known
at build time are described to `bb-recomp` in a **variant catalog** (`tools/recomp/variants/*.txt`, docs/RECOMP_HOOKS.md: a list of *groups*, each a set of writes `vaddr + bytes`
that the runtime applies together, plus `excl=` sets for alternatives and `c` lines for new code such as the cheats' cave). `bb-recomp --variants <file|dir>`:

- finds the functions each group writes into (a write outside every function is a *cave piece*: new code, a pseudo-function of its own);
- for every function and every consistent non-empty subset S of the groups that touch it (alternatives of one `excl` set never together; a function with more than six groups gets single
  groups and the full set only) applies the writes of S to the image, lifts the function again, follows the jumps into cave pieces and back into the middle of the function (a unit
  fixpoint like the discovery's, so the cave's jump back lands on a unit), lowers the result as **alternate units** `bbrv<k>_<vaddr>` (`k` unique per variant, hidden visibility, no dirty check), and undoes the writes;
- does not build a variant for a function that holds a unit with unsupported ops (it runs on the fallback), and skips (and reports) a variant that fails to lift or lowers an unsupported op.

Exports (all read-only data except where noted):

| symbol | meaning |
|---|---|
| `bbr_vfuncs[]`, `bbr_vfunc_count` | per variant `{ u64 func_start; u32 first_unit, unit_count, name_offset, 0; }`, sorted by `func_start`; the variants of one function are adjacent. `first_unit ..+unit_count` index `bbr_vunits[]` (sorted by vaddr) |
| `bbr_vunits[]`, `bbr_vunit_count` | per variant unit `{ u64 vaddr, end, ehash; void (*fn)(GuestState*); i64 orig; }`: the unit's code range in the *patched* image, the FNV-1a of those bytes (byte 0x65 hashed as 0x64, as `bbr_unit_ehash`), the function, and the index of the original unit with that entry in `bbr_units[]` (-1: only the variant has this unit: a cave piece, or an entry the patch made necessary) |
| `bbr_vnames` | NUL-separated variant names (`<func>+<group>+<group>`), for diagnostics |

**What the runtime does** (`cpu/guest_cpu_aot_hooks.inc`). A function is *classified* from the bytes in memory each time they may have changed: at the start-up check (every unit whose hash differs) and
at every `guest_cpu_invalidate` / `guest_cpu_hook` that overlaps the function (the call must come after the write). The classification hashes every unit of the function (`bbr_funcs[]` gives its units):

1. every unit hashes as built: **original**. The units run. Their dirty bytes are cleared if a patch was undone, and the redirects dropped;
2. otherwise a variant of the function (`bbr_vfuncs` by function start) whose every unit hashes as the current bytes: **variant**. The dirty byte of every original unit of the function is set and a *redirect pointer* is stored
   per original unit (`V.redirect[i]`: the variant unit with the same address, or null for a unit the variant lacks). The dispatch table entry (`E.fns[i]`) is replaced by the variant's function, so indirect calls and
   tail jumps through `bbr_dispatch` skip the original;
3. otherwise **dirty**: all units dirty, no redirects, the fallback CPU runs the function (as before phase 4).

Direct unit-to-unit calls and tail jumps keep calling `bbr_<vaddr>` of the *original* unit. That unit tests its dirty byte, which is set, and tail-calls `bbr_dirty_entry(s, vaddr)`. `bbr_dirty_entry` now first looks up the unit
(the dispatch hash table, O(1)) and, if it has a redirect pointer, calls the variant's unit with the same state (`variant(s)`): the extra hop costs a few instructions per call and only on patched functions. Calls inside a variant to a unit
of its own variant (including its cave pieces and entries it added) go to `bbrv<k>_<vaddr>` directly; calls to any other unit go to `bbr_<vaddr>` and so reach *that* function's variant the same way. The `.so`'s text is never written and
nothing about the lowering of ordinary units changed (the per-unit dirty test is the redirect's switch).

A function that holds a unit the tool started dirty (unsupported ops) is never classified: it stays on the fallback. Without `bbr_funcs`, exact unit hashes or the dirty byte (an older tool's `.so`, `bbr_abi_flags` bit 1 clear) the
old behaviour applies: a changed unit leaves the table or goes dirty for good.

Transient states are visible: a group whose sites are written one at a time (`guest_cpu_invalidate` after each) is dirty, then a variant once the last site is in, and the original again when the originals are written back.

## Hook slots: native hook sites (additive; `bbr_abi_flags` bit 3)

A catalog `hook <vaddr> <len> <snap|regs> [skip] [resume=a,b,...]` line declares an instruction the runtime may hook. `bb-recomp` then:

- forces a `GuestOpcode` marker at the site and at every resume address (FEX only emits one for instructions with side effects), so each is an instruction boundary in the IR;
- at the start of the guest instruction at the site, in **every unit that contains it** (the entry unit, secondary-entry copies, variants), emits

      if (bbr_hook_slot[k] != null) {                  // relaxed atomic load of a pointer, branch weights 1:4000
          spill registers and flags to the state; s->rip = site;
          act = bbr_hook_slot[k](s, k);                // uint64_t (*)(GuestState*, uint64_t)
          reload every register (the callback may have changed any of them);
          switch (act) { case 0: run the instruction;
                         case 2: return;               // the callback made a `ret`: rip, rsp set; Return semantic of ExitFunction
                         default: switch (s->rip - image_base) { case <resume address>: continue there; default: tail dispatch } }
      }

  The flags are put into the state's per-unit flag cells before the test and are not touched by the callback: the fall-through path continues with the lazy flag descriptor, the jump path from the cells.
  A resume address that is no block entry gets a block of its own (the block is split there). A jump to an address that is not a resume point of the site goes through `bbr_dispatch_tail` (a jump
  into the middle of another unit's code then runs on the fallback). Resume addresses of a site that are not in the unit are ignored.
- exports `bbr_hook_sites[]`, `bbr_hook_count`: `{ u64 vaddr; u32 len, flags, resume_first, resume_count; }` sorted by vaddr (flags: 1 the site was compiled into at least one unit, 2 `skip`, 4 kind `regs`),
  `bbr_hook_resume[]` (u64), and the writable `bbr_hook_slot[]` (all null at load; the runtime stores a function pointer).

The runtime (`cpu/guest_cpu_aot_hooks.inc`) sets every slot to one function, `RunSlot(s, k)`, and keeps a record per site (`src/guest_cpu.h`):

- `guest_cpu_hook(address, size, GuestHook)`: in AOT mode first tries the slot (same call, same `GuestHook`: the callback edits `GuestRegs` and must set rip; rip as set becomes a jump, rip unchanged runs the instruction); otherwise the old path;
- `guest_cpu_hook_slot(address, size, GuestHookEx, context)`: `GuestRegsEx` has rflags (a constant 0x202; the flags survive the call), mxcsr (not written back) and ymm (written back); the result is `GUEST_HOOK_CONTINUE`, `_JUMP` or `_RETURN`
  (the runtime then does the `ret`: rip = [rsp], rsp += 8);
- `guest_cpu_has_hook_slot(address)`, `guest_cpu_hook_slot_clear(address)`.

A slot is only set when the site's function is currently native (original or variant); if it is dirty the call returns 0 and the caller hooks the old way. `InstallGuestCodeHook` (`gpu/shim/bbport_guest_code_hooks.cpp`) asks first: with a slot it
writes no trampoline and no code, `SlotAdapter` converts `GuestRegsEx` to the `GuestRegisterSnapshot` the hooks know (registers, rflags, mxcsr, ymm), calls `HookDispatch`, converts back and returns `GUEST_HOOK_RETURN` when the skip flag was set (and clears it,
as the trampoline did). Differences from the x86 trampoline: the callback runs on the host stack (not the guest stack), registers are the callback's to change but rbx, rbp, r12-r15 must be the original values on a `GUEST_HOOK_RETURN`
(a return from a function must preserve them anyway), and rflags in the snapshot is not the guest's.

A slot callback may call guest code (`guest_cpu_call`): the nested call starts below the red zone of the rsp the unit stored before the slot call. The port's patchers use the slot mechanism too
(`gpu/shim/bbport_hook_sites.h`: `Slot(address, image_base, length, callback, context, {resume addresses})` installs, and in the generator's build prints the catalog line).

A hook installed in a slot stays set while its function runs on the fallback (a later write made it dirty) but does not fire there: the runtime prints `AOT: the hook at +X is inactive` once per site and the hook works again when the function is native again.

## Granular entries: dynamic jump targets inside functions (additive; `bbr_abi_flags` bit 4)

A jump the lifter cannot resolve (a table-like `jmp reg` whose table or bound it did not find, a table it only scanned) may land on any instruction of its
function. **Granular functions** answer that without a fallback CPU: every unit of such a function gets, right after its prologue, a switch over every
instruction boundary it holds (the resume-entry switch of the call-return blocks, with all instruction boundaries as cases).

- **Which functions** (`bb-recomp --granular off|jumps|all`): `jumps` (the default) = functions with an indirect jump the lifter could not resolve for sure (unresolved
  table-like jumps, and tables resolved only by scanning to the first non-target entry); `all` = every function (an experiment, see the plan's "Granular entries" for the
  cost); `--granular-funcs <file>` adds the functions holding the addresses in a file (a misses log works). Register tail calls (`jmp reg` to another function) and
  `jmp [mem]` are not table jumps and do not make a function granular.
- **Lifting.** The lifter puts a `GuestOpcode` marker at every instruction of a granular function; the lowering splits a block at each instruction that is no block
  entry (like a hook resume point: the fall-through path joins the new block with its flags in the allocas) and lists every block entry and split in the switch.
  Entering from the switch loads the flags from the context (flags are live across a jump, unlike after a call: the jumping unit stored them).
- **Protocol** (the resume flag, `GuestState::_pad1`, offset 0xb8): the dispatcher sets `s->rip = target; s->_pad1 = 1` and calls a unit of the function. A unit that
  holds the address clears the flag and runs the code from there, as if the jump had gone to it (a `ret` in it returns to the caller of the function: the target was a
  jump, or a call whose return address is on the stack); a unit that does not returns at once with the flag still set. The dispatcher tries the function's units
  (`bbr_funcs[]`, entry unit first) until the flag is clear. A dirty unit returns at once (flag set), a patch variant of a granular function has the same switch.
- **Exports:** `bbr_func_granular[bbr_func_count]` (u8, 1 = granular) and bit 4 of `bbr_abi_flags`.
- **Runtime:** on a miss (`bbr_dispatch`, `bbr_dispatch_tail`, a return to an address in no unit, `bbr_unwound`) the engine asks `GranularEntry` before anything else.
  An address no unit and no granular entry covers is a **miss** (below).

## Misses, strict mode and the misses log

A *miss* is guest code about to run at an address no unit starts at, no granular function has as an instruction boundary, and that is not a host-function stub, a PLT
thunk or an import trap: a jump into the middle of a unit, a call to code the tool did not find, a return after a longjmp to an address no unit holds, an entry of a
patched function with no variant, an address outside the image.

- **Always:** the miss is appended to `logs/aot-misses.log` (`BB_AOT_MISS_LOG=<path>` to move it; the working directory of the runtime is
  `files/bbport`): `miss +0x210ebc0 kind=jump thread=CSChrThread0 tid=1234 rsp=0x... backtrace=+0x2110a20,+0x1c40` (image offsets; kind: call, jump, unwind,
  patched; `outside 0x...` for an address outside the image; the backtrace is the return address on the guest stack, then the rbp chain).
- **The game stops** (strict mode, the only mode): the miss is logged and the process exits with status 22 and
  `STOP: AOT: native code is missing an entry at +0x...; please report this (details: logs/aot-misses.log)` (a patched function with no variant: `native code for a patched or hooked
  function has no variant at +0x...`; outside the image: `guest code ran at 0x..., outside the image, which native code cannot run`). No fallback CPU exists to carry on.
- **Closing a gap without a code change:** `bb-recomp ... --extra-entries aot-misses.log` makes every logged address a unit entry (an address inside a function becomes an extra unit of
  it, one outside every function a pseudo-function; addresses that do not decode as code are reported and ignored) and the `.so` is rebuilt. `--check-entries a,b,c` (with
  `--units-only`) says for each address whether it is a unit entry, a granular entry, a case of a resolved jump table, or not covered (exit status 3).
- **Import traps.** `src/probe.c` gives unresolved imports 32-byte x86 stubs (`mov rsi,rdi; mov edi,<index>; movabs rax,<handler stub>; jmp rax`) in a code range of
  their own. The engine recognises exactly that sequence and calls the handler stub's host function with (index, rdi) instead of running it, so no CPU executes it.

## libbbcpu: the engine and its platform services

`cpu/CMakeLists.txt` builds `libbbcpu.so` from `guest_cpu_aot.cpp`, `guest_cpu_native.cpp` and `host_call_arm64.S` (`tools/android/build_bionic.sh`; `build.sh` on AArch64 hosts). `BB_AOT=<libbbaot.so>` is
required (the runtime stops at start-up without it), and no guest instruction ever runs on a CPU other than the units. The services the engine needs (`cpu/guest_cpu_native.h`, namespace `bbnative`):

| service | how |
|---|---|
| thread state | `GuestState` (`cpu/guest_state.h`; the layout is the x86-64 front end's `CPUState`, pinned by `static_assert`s; `tests/guest_state_check.cpp` compares it with FEX's headers when a FEX tree is given to `tools/check_recomp_abi.sh`), allocated per thread |
| host-function stubs | a table of 16-byte slots below 1 TiB, never executed: the dispatcher recognises the addresses |
| guest calls (`guest_cpu_call`, `guest_cpu_call_floats`) | units; the sentinel is stub slot 0 |
| CPUID, XGETBV | `cpu/guest_cpu_cpuid.h`: the model the game was verified with (ported from FEX-Emu, MIT): the Skylake identity with the host features fixed to the lifter's profile; cache and TLB leaves as constants; the product name from core 0's MIDR |
| TSC | `cntvct_el0 << shift`, shift so the rate is at least 1 GHz |
| unaligned `lock` operations | `cpu/guest_cpu_atomics.h`: decodes LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN/SWP/CAS/CASP (every width, any acquire/release form), performs them byte-wise under one global lock, advances the pc |
| faults | the unaligned atomics only; everything else is the host crash report |
| `guest_cpu_hook` / `guest_cpu_invalidate` | no code is written: a hook is a native slot or it is refused (logged); an invalidated function is classified again (original, variant, or no native code) |

The ABI version the runtime accepts is `BbrAbiVersion` (1) in `cpu/guest_cpu_aot.cpp`; `tools/check_recomp_abi.sh` compares it with the app's `NativeCpu.ABI_VERSION` and the tool's `bbr_abi_version`.

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
| `bbr_x87_*` | none to provide: the x87 helpers are linked into the `.so` ("x87 helpers" below) |

`__divti3`/`__udivti3` (128-by-64-bit `div`/`idiv`) come from the compiler builtins library, which the link
step adds (`libclang_rt.builtins-aarch64-android.a`).

## x87 helpers (linked into the `.so`, nothing for the runtime to provide)

The runtime runs x87 with reduced precision (FEX's `X87ReducedPrecision=1`, the configuration the game was verified with): the stack is F64 and most
instructions lower inline (`fadd`, `fmul`, `fcom`, `fsqrt` ...). A few ops have no inline form and call the helpers below. Unlike the
`bbr_*` helpers above, these are **defined in the generated `.so`** (hidden visibility, no import): `bb-recomp` compiles
`tools/recomp/helpers/bbr_x87_all.c` with the target's clang (NDK clang, `aarch64-linux-android30`, for the Android `.so`; the system clang for
`--target x86_64`) and links the object in. The only new dependency is libm (`DT_NEEDED libm.so`: `sin cos tan atan2 exp2 log2 fmod remainder trunc`,
which FEX's handlers call too). The helpers are FEX's fallback handlers (`F80Fallbacks.h`, MIT) over its bundled SoftFloat 3e (BSD-3,
renamed `bbrsf_*`); see `helpers/NOTICE`.

`GuestState* s` is the first argument where the helper needs the x87 state: it reads the control word `FCW` (`CPUState` offset 1200) to build the
SoftFloat rounding mode, as FEX's `SoftFloatStateFromFCW`, and sets the IE flag (`flags[X87FLAG_IE_LOC]`, offset 1040) when SoftFloat raises the
invalid-operation flag, as `ScopedSoftFloatState` does. The offsets are `static_assert`ed against `CPUState` in `lower_vector.cpp`. An 80-bit
value is passed as two `uint64_t` (significand; sign and exponent in the low 16 bits) and returned as `struct {uint64_t sig, top;}`, which both
the x86-64 SysV and the AArch64 ABI return in two integer registers; in the IR it is the low 10 bytes of a 128-bit FPR (bytes above are zero).

| helper | IR op, FEX handler |
|---|---|
| `double bbr_x87_f80_to_f64(s, sig, top)`, `float bbr_x87_f80_to_f32(s, sig, top)` | `F80CVT` size 8 / 4 (`fstp tbyte` path of reduced mode; `OpHandlers<OP_F80CVT>`) |
| `bbr_f80 bbr_x87_f64_to_f80(s, double)`, `bbr_f80 bbr_x87_f32_to_f80(s, float)` | `F80CVTTo` (`fld tbyte` path; `OP_F80CVTTO`) |
| `int64_t bbr_x87_f80_to_int(s, sig, top, size, truncate)` | `F80CVTInt` (size 2, 4, 8; 16-bit overflow gives `INT16_MIN`) |
| `bbr_f80 bbr_x87_int_to_f80(s, int64, size)` | `F80CVTToInt` (size 2, 4) |
| `bbr_f80 bbr_x87_bcd_load(s, sig, top)`, `bbr_f80 bbr_x87_bcd_store(s, sig, top)` | `F80BCDLoad`, `F80BCDStore` (`fbld`, `fbstp`) |
| `double bbr_x87_f64_fprem(a, b)`, `_fprem1`, `_scale`, `_f2xm1(a)`, `_fyl2x(x, y)`, `_fyl2xp1(x, y)`, `_atan(y, x)`, `_tan(a)`, `_sin(a)`, `_cos(a)` | `F64FPREM` (`fmod`), `F64FPREM1` (`remainder`), `F64SCALE` (`a == 0 ? a : a * exp2(trunc(b))`), `F64F2XM1` (`exp2(a) - 1`), `F64FYL2X` (`y * log2(x)`), `F64FYL2XP1`, `F64ATAN` (`atan2`), `F64TAN`, `F64SIN`, `F64COS` |

Not provided: the 80-bit arithmetic ops (`F80Add`, `F80Mul`, `F80FPREM` ... used only with `X87ReducedPrecision=0`; the lifter is configured reduced, like
the game) and `F64SINCOS` (the reduced stack pass emits separate `F64SIN` and `F64COS`). Both would be further `bbr_x87_*` helpers over the same SoftFloat.

`NZCVSelectV` (a floating-point select on the flags, `fcsel`) is plain LLVM: `select` on the condition, the result in lane 0 and the rest zeroed.

## Test path on the PC

The lowering uses target-independent LLVM IR wherever possible, so the same IR also compiles for
x86-64 and runs on the PC:

- **`bb-recomp --target x86_64`** compiles for the PC, and the ASM test runner executes it in
  process.
- **FEX's `unittests/ASM/*.asm`** give the expected `RegData` and `MemoryRegions`.
- **Game functions** are checked against native x86 execution of the original code on the PC. Native
  x86 is the oracle, which is stronger than FEX.
- **AArch64-only intrinsics** each need a generic fallback, so the x86-64 test build keeps working.
