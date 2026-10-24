# AOT runtime backend: state at the handoff (2026-10-24, branch recomp-rt)

> Historical (before phase 7): written for the hybrid runtime with FEXCore as fallback; see docs/STATIC_RECOMPILE_PLAN.md, "Phase 7 status".

Design: `cpu/guest_cpu_aot.cpp` exports guest_cpu.h; the old CPU (src/runtime_cpu.c on x86-64, cpu/guest_cpu_fex.cpp on ARM64) is the
fallback, compiled under the names bb_fb_* (`cpu/guest_cpu_rename.h`, BB_AOT_FALLBACK). Details and requests: docs/RECOMP_ABI.md.

## How far the PC run got (x86-64 units + native fallback)

- Build: `source <env: CMAKE_PREFIX_PATH etc. as build-local.sh>; nice -n 19 bash build.sh` (links the GPU lib with LTO every time, ~90 s);
  quicker for engine changes: `c++ -std=c++20 -O2 -g -Wall -Wextra -Werror -pthread -Icpu -Isrc -c cpu/guest_cpu_aot.cpp -o out/guest_cpu_aot.o`
  then the final `cc` line of build.sh with `-DBB_AOT_FALLBACK out/guest_cpu_aot.o -lstdc++ -ldl`.
- Units: `out/recomp-build/bb-recomp --elf out/eboot.elf --target x86_64 --so out/aot/libbbaot_x86.so -O1` (70-90 s, 147 MB, 202,525 units,
  no trap). Tool built with `cmake -S tools/recomp -B out/recomp-build -G Ninja -DFEX_GIT_DIR=<main>/third_party/FEX -DFEX_SOURCE_DIR=<recomp worktree>/out/recomp-fex`.
- Run (window opens; do not leave it running): `BB_GAME_DIR=<game>/CUSA03173 BB_AOT=out/aot/libbbaot_x86.so BB_AOT_STATS=1 BB_AOT_STATS_SEC=30 BB_AOT_TRACE=1
  BB_AOT_DEBUG=1 BB_FRAME_STATS=1 BB_PAD_FILE=out/pad BB_PREBUILT=1 BB_PROBE=out/bb-probe timeout 150 bash run.sh` (BB_PREBUILT+BB_PROBE skip build.sh).
  The prepared image, saves (user/savedata, copied from the main checkout) and the shader cache are in the worktree's out/ and user/.
- Result of the last full run (150 s, ended by timeout, no crash): image accepted ("image checked in 163 ms: 202525 units; 38 differ (patched or
  hooked), 52 units with their functions' other units run on the fallback CPU"); all guest threads created (FMOD, GXRenderThread, EzWorkPool...),
  the game rendered 46,680 frames, `Frame stats: 428.7 FPS` (vblank 480 Hz, uncapped, title/menu scene, 46 draws per frame), no `Guest fault`/`STOP`.
  Whether the title screen was reached and shown was not confirmed visually (no screenshot tool worked); no pad input was sent; no save was loaded.
  A later run (b1, 420 s budget) was killed by the user after about 50 s.
- Counters at 130 s: 5.76 M PLT thunks followed, 3.10 M unit entries through the dispatcher, 5.65 M host function calls, 112,685 fallback runs
  (0.22 Gcycles), 20 mid-unit jumps, 28 distinct miss addresses. Top: +0x56ed570 (74,866), +0x56ed9d0, +0x56eda60, +0x56f3130, +0x570fc70,
  +0x56ee4c0 (all libc.prx at image +0x56e0000, which has no units), then game addresses with 1-5 calls (+0x20e42fb, +0x109d00d, +0x2067bc6, +0xd496f0...).
- No native baseline FPS was measured.

## Bugs found and fixed

1. A dirty gap unit (switch case body) entered by a tail jump was run as a call: the runtime replaced the "return address" on top of the stack, which was a
   local variable (crash at +0x217590e). Fix: dirty marking takes the whole function (tool flag `bbr_unit_extra`).
2. Jumps into the middle of a unit (unresolved switch, +0x21758d2) did the same. Fix: `bbr_dispatch_tail` to an address that is no unit entry runs the fallback
   to the end of the guest stack (counter "mid-unit jumps"). Open: should become unit entries (RECOMP_ABI.md, request 1).
3. The native fallback clobbered guest R11 when resuming mid-function (crash at +0x2067fca, write through r11). Fix: target passed through TLS.
4. Every import call missed (PLT thunks outside every FDE). Fix: the dispatcher follows `jmp *[rip+disp]` to the host stub.

## Open suspects / not done

- x86 flags are not passed to the fallback; flags are only stored by units at tail jumps. Fine at calls; unchecked for a tail jump into native code.
- (Phase 4, branch recomp-hooks: patches with fixed bytes run as precompiled variants, and hook sites are native slots: docs/RECOMP_HOOKS.md, RECOMP_ABI.md "Variants" / "Hook slots".)
- Hooks installed at run time rely on `guest_cpu_invalidate`; start-up hooks/patches are found by the unit hashes. Entry patching of dirty units needs a
  writable .so text (mprotect): refused by Android's policy probably, then direct calls from other units still run stale units (request: dirty flag in units).
- `guest_call_sites` (wait profiling, runtime_kernel.c) scans the host stack on x86-64 and finds no guest frames in AOT mode (diagnostics only).
- On x86-64 a host function called from native code that calls back into the game gets a separate guest stack (1 MB, per nesting level): not exercised yet.
- Threads: guest stack of 4 MB is allocated for threads without `guest_cpu_thread_stack` (x86-64 only); the runtime's thread stack sizes are not used.
- libc.prx and the other linked modules have no units: every call into them is a native fallback run (the hottest misses); the tool should lift them too.
- ARM64: engine and FEX fallback compile and link (out/bionic/fex/libbbcpu.so, bb-probe linked against the bionic worktree's libbbgpu) but were never run.
  Untested ARM64 points: `bb_fb_run` (FEX HandleCallback with RSP = slot+16, return detection by RSP), the sentinel stub, SIGBUS on unaligned `lock` in units
  (not handled), FEX threads created for every guest thread even when AOT covers it, SVE256 hosts refuse AOT.
- Not done: scripted pad input / save loading on the PC, FPS comparison with native.
