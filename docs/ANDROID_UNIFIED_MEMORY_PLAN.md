# Android unified memory (v2)

Goal: memory that both the CPU and the GPU use exists **once** in RAM, as on the PS4, instead of the
game's copy plus GPU copies of it. Where sharing is not possible or not safe, today's copy model stays,
unchanged.

Branch `testing` (local `bionic-ndk`). Switch: Debug options "Unified memory (experimental)" =
bbport.ini `unified_memory=1` = `BB_UMA=1`. Off by default; off is exactly today's copy model.

## Where the memory goes today (Thor, Yharnam, copy mode, 2026-10-16)

| | Copy mode | Unified v1 |
|---|---|---|
| dumpsys TOTAL PSS | 5988 MB | 7828 MB |
| Game memory, memfd (only touched pages) | 2739 MB | 93 MB |
| dma-buf chunks (KGSL imported) | 25 MB | 4837 MB |
| Staging / arena residency (buffer copies) | 608 / 320 MB | 192 / 0 MB |
| KGSL GPU memory (images, driver, ...) | 3100 MB | 2889 MB |

- The shareable part is the **buffer copies**: staging and arena residency, about 930 MB, plus the CPU
  time spent copying them every frame.
- **Textures cannot be single-copy.** The GPU cannot sample the PS4's tiled layout, and the game's
  tiled copy is the source of every refresh, every re-creation after the texture collector frees an
  image, and every buffer read of texture memory. Nothing can rebuild it from the VkImage without loss
  (stencil, pitch padding, substituted formats). Textures keep both copies.

## What went wrong in v1 (2026-10-16)

v1 mapped all direct memory as whole-allocation dma-bufs and bound them through the layer memory
module. In game: static world missing or black, lighting gone, textures glitched or lost; and it used
1.8 GB **more** memory.

1. **Up-front commit.** dma-heap pages are allocated when the dma-buf is; v1 made one per direct
   allocation, so all 4589 MiB the game allocated was committed at once (the memfd only held the 2.7 GB
   the game touched).
2. **Copies became live GPU reads.** In copy mode the CPU copies guest data into staging when the draw is
   recorded. In v1, texture uploads (`ObtainBufferForImage` → `LayerBind(may_promote=false)`) and
   arena uploads (`BB_CHUNK_UPLOADS`) were GPU copies out of the game's memory at execution time: data
   the game changed in between, or stale CPU caches, reached the GPU.
3. **A different GPU addressing model.** Layer mode resolves ranges to per-chunk wrapper buffers, cuts
   bindings at chunk ends, sends 64 MiB+ bindings through a BDA page table and misses to the arena. Copy
   mode uses none of this: a large untested surface.
4. **The GPU wrote into the game's memory.** `SynchronizeMemoryFromImage` re-tiles GPU-written images
   into the bound buffer and fills HTile regions with 0xF; in copy mode that buffer is a VRAM copy, in
   place it is the game's memory.
5. **Coherence unproven.** Turnip allocates exportable memory from `/dev/dma_heap/system` and imports it
   into KGSL with **no IOCOHERENT flag and no cache mode** (`tu_knl_kgsl.cc` `kgsl_bo_init_dmabuf`); the
   CPU maps it cached through the dma-buf fd; nobody calls `DMA_BUF_IOCTL_SYNC` or cleans caches. Whether
   the GPU sees CPU writes then depends on the kernel. The 2 MiB roundtrip probe passed, but it does not
   prove it for game-sized data.

## Principles

1. **Share only what both sides use as buffers.** Pages the GPU never binds as a buffer (code, heaps,
   textures, audio) stay in the memfd exactly as today.
2. **Lazy.** Sharing converts memory the game already touched; it never commits untouched memory
   (beyond rounding to a granule).
3. **Copies stay CPU snapshots at record time.** Only in-place buffer bindings read live memory. Texture
   uploads, arena uploads and DMA sources never GPU-copy out of shared memory.
4. **Copy mode's GPU addressing.** The GPU reaches shared pages through the same sparse arena copy mode
   uses (the PC "arena in place" path); descriptors and offsets do not change. No layer memory module.
5. **The GPU never writes emulator-made data into shared memory** (re-tiled images, HTile fills): blocks
   those land on stay copies.
6. **Proven before used.** Coherence and sparse binding of shared memory are checked at start; any
   failure means copy mode with one log line naming why.
7. **Per class, observable.** Buffer classes are enabled one at a time; a verify mode reports data that
   changed while the GPU used it.

## Memory layout

```
 guest VA (FEX)      0x10_0000_0000..  direct mappings (aliases allowed) ──┐
 backing view        backing_base + phys (runtime's own view) ─────────────┤ same pages
 GPU                 arena (sparse, VA-indexed, 64 KiB blocks) ────────────┘ for SHARED granules

 physical pool [0, 5056 MiB), in granules of BB_UMA_GRANULE_KB (2 MiB):
   CPU     memfd range, lazily committed       GPU: VRAM copy + staging upload (copy mode)
   SHARED  dma-buf granule, CPU-cached         GPU: arena blocks bound to it, no copy
 flexible memory [5056 MiB, +1 GiB)  always CPU
```

### Granules

- Direct memory is split into fixed granules (default 2 MiB, a multiple of the 64 KiB arena block). The
  runtime keeps one state per granule: CPU (memfd) or SHARED (dma-buf fd, chunk record on the GPU side).
- Allocating direct memory changes nothing (unlike v1): every granule starts CPU.
- Granules are allocated by the GPU library as exportable Vulkan memory (`AllocChunk`, one granule
  each) and exported as a dma-buf; the runtime maps it. Lookups are O(1) by `phys / granule` (v1 had a
  sorted list capped at 256 chunks).
- Cost per shared granule: one dma-buf in the system heap, two fds (ours and Turnip's), one KGSL
  object. 1 GB shared = 512 granules: far below Android's fd limit.

### Promotion (CPU → SHARED)

Requested by the GPU side when a buffer binding of an enabled class needs arena residency on a CPU
granule (`BindInPlace`). Under the runtime's write lock:

1. Allocate the granule (GPU library callback) and map it once at a scratch address.
2. **Freeze writers:** every guest alias of the granule becomes read-only (trap reason `Promote`). A game
   write faults; the runtime's fault handler sees a promotion in progress, waits for it, and returns so
   the write is retried on the new mapping. Runtime writers wait on the lock (`write_backing` takes it),
   and file reads into guest memory hold a promotion guard across the `read()` (the kernel would get
   `EFAULT` on a frozen page instead of a fault).
3. Copy the memfd range into the granule.
4. `mmap(MAP_FIXED)` the granule over the backing view and every guest alias, with each alias's current
   protection (traps re-applied).
5. Punch the memfd range (the RAM moves, it does not double).
6. Unfreeze; hand the granule to the GPU side, which binds the arena blocks.

Never promoted: granules holding executable mappings, flexible memory, granules the GPU library cannot
allocate (they stay CPU, the binding takes a copy).

A granule fully released by the game is **demoted** later (phase 4): GPU blocks rebound to the
evicted-memory block, VkDeviceMemory freed after the GPU tick, backing view back to the memfd. Until
then released shared memory is cleared, not freed (as today's chunks).

### GPU view

- `EnsureResident` → `BindInPlace` (already there for the PC path) binds arena blocks to chunk memory
  when the block is in one direct mapping, block-aligned in physical memory and its granule is SHARED.
  v2 adds: a CPU granule is promoted first if the binding's class is enabled; otherwise the block gets
  a VRAM copy as in copy mode.
- A block's first residency decides its backing. A block already copied stays a copy (the copy FIFO
  returns it later); a shared block stays shared.
- `GarlicInVram()` is false on phones: no VRAM mirrors of shared blocks.
- The copy FIFO (`BB_VRAM_COPY_QUEUE`) stays active for the blocks that are still copies (today it is
  off whenever `GuestInPlace()`).

### Classes

The rasterizer tags each buffer binding before `ObtainBuffer`; only enabled classes may promote:

| Class | Bindings | Hazard | Default |
|---|---|---|---|
| `index` | index buffers | low (mostly static) | on |
| `vertex` | vertex buffers (V# fetch) | low-medium | on |
| `constant` | uniform/constant buffers over 16 KiB, constants in place | high (rewritten every frame) | off |
| `storage` | storage buffers, read and written | medium | off |
| `texel` | formatted (texel) buffers | medium; may overlap GPU-written images | off |
| `indirect` | indirect draw/dispatch arguments | low | off |

Always copies: texture upload sources, DMA sources, arena uploads (`BB_CHUNK_UPLOADS` off), blocks
written by `SynchronizeMemoryFromImage`, bindings of 16 KiB and less (stream buffer, constant ring).

### Coherence

Checked once at start, before any granule is shared:

1. **KGSL flags diagnostic:** import a dma-heap buffer into our own `/dev/kgsl-3d0` context, once with
   flags 0 (what Turnip does) and once with `IOCOHERENT | WRITEBACK`; log the flags `GPUOBJ_INFO`
   returns. This says what the kernel does by default and whether IO-coherent import exists.
2. **Strict roundtrips** on a 32 MiB exported buffer through a runtime-style `mmap` of the dma-buf fd:
   CPU write → GPU read (whole buffer, and a single 64-byte line written right before the submit);
   GPU write → CPU read with the lines pre-loaded into the CPU caches; from two threads on different
   cores.
3. **Sparse binding of shared memory**, on a separate throwaway VkDevice (a KGSL refusal must not lose
   the game's device): bind an exported dma-buf into a sparse buffer at a 64 KiB offset, GPU write
   through the sparse buffer / CPU read through the mmap, and the other way round.

Any failure → copy mode. If (1) shows the kernel does not make Turnip's imports coherent but accepts
`IOCOHERENT`, the fix is a small patch to our Turnip build (import with the memory type's cache flags);
a cache-maintenance mode (clean CPU-written ranges before a submit, invalidate GPU-written ones before
the CPU reads, using write tracking) is the fallback for drivers we do not build.

### Ordering (record vs execute)

In copy mode the GPU sees data as of recording; in place, as of execution. The game may rewrite a
buffer once it believes the GPU is done (labels, EOP). `BB_HONEST_LABELS` keeps labels after host GPU
completion. Phase 3 adds an optional order trap (`BB_UMA_ORDER=1`): shared blocks with GPU reads in
flight are write-trapped; a game write waits for that GPU tick.

### Verify mode

`BB_UMA_VERIFY=N`: one in N in-place read bindings is hashed when recorded and again when its GPU tick
completes. A mismatch means the game changed the data while the GPU used it; it is logged with class,
guest address, size and frame. This replaces guessing which binding causes a glitch.

### Accounting

- `Memory split` line: shared granules (count, MiB), promotions and failures, memfd MiB, arena
  residency, staging, verify mismatches.
- The texture collector's usage counts every Vulkan allocation: shared granule bytes are subtracted
  (v1 left the collector permanently "under pressure").
- Staging keep (`BB_STAGING_KEEP_MB`) drops to 128 MiB per ring under `BB_UMA` (buffers no longer go
  through staging; texture uploads still do).

## Switches

| Switch | Meaning | Default |
|---|---|---|
| `BB_UMA=1` | unified memory v2 (run-bionic.sh adds `BB_GUEST_IN_PLACE=1 BB_LAYER_MEMORY=0 BB_WRITE_TRACKING=1 BB_CHUNK_UPLOADS=0`) | off |
| `BB_UMA_CLASSES` | comma list of classes, or `all` | `index,vertex` |
| `BB_UMA_GRANULE_KB` | granule size, 64 KiB multiple, 256..65536 | 2048 |
| `BB_UMA_VERIFY` | verify one in N in-place bindings (0: off) | 0 |
| `BB_UMA_ALLOW_NONCOHERENT=1` | debugging only: share even if the coherence probe fails | off |
| `BB_UMA_ORDER=1` | phase 3: game writes wait for in-flight GPU reads of shared blocks | off |

`BB_UNIFIED=1` (v1, layer mode) stays in the code for reference but is no longer wired to the Debug
option.

## Phases

1. **Setup (this change):** design, runtime granules and promotion, probes and diagnostics, class
   policy, arena binding of shared granules, accounting, verify mode, switch. Builds; untested on a
   device.
2. **Bring-up on the Thor:** read the probe and KGSL lines; `BB_UMA_CLASSES=index`, then `index,vertex`,
   with `BB_UMA_VERIFY=64`. Checkpoint: title and Yharnam render as copy mode, no verify mismatches,
   memory split shows staging/arena down.
3. **More classes:** `storage`, `texel`, `indirect`, then `constant`; order trap where verify finds
   hazards. Checkpoint: 15 minutes on Thor and N5 per class without glitches.
4. **Memory:** demotion of released granules, granule size tuning, lmkd behaviour (dma-buf pages are not
   swappable). Checkpoint: PSS below copy mode by the staging + arena share, FPS equal or better.
5. **Rollout:** default on for Adreno with the bundled driver once 2–4 hold; Mali/PanVK and drivers
   failing the probes stay on copies.

## Risks

- **Coherence** (see above): the main suspect for v1's missing world.
- **Kernel writes into frozen pages** other than file reads (syscalls writing into direct memory)
  would see `EFAULT` during a promotion (about a millisecond per granule). Direct memory is rarely a
  syscall buffer; logged if seen.
- **Swap:** shared granules are dma-buf pages, which zram cannot compress when the game is in the
  background.
- **KGSL limits:** binding imported dma-bufs into sparse buffers (VBOs) is unverified (probe 3);
  `KGSL_PROP_GPU_VA64_SIZE` unknown.

## Other waste (not unified memory)

- `BB_STAGING_KEEP_MB` keeps up to 512 MiB of idle staging per ring.
- The initial upload of a new render target reads its never-written guest pages, which commits them in
  the memfd.

## Key files

- `src/runtime_memory.c`: granules, promotion, freeze, map_phys, zero_phys; `src/probe.c` fault chain;
  `src/runtime_file.c` read guard.
- `gpu/shim/bbport_guest_memory.cpp`: UMA mode, probes, granule allocator, chunk index.
- `gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp`: `BindInPlace`, `ObtainBufferForImage`,
  `CopyQueueActive`, `SynchronizeMemoryFromImage`.
- `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp`: binding classes.
- `gpu/shadps4/video_core/texture_cache/texture_cache.cpp`: collector usage.
- `gpu/shadps4/core/libraries/videoout/driver.cpp`: memory split line.
- `tools/android/run-bionic.sh`, `DebugDialog.java`: the switch.
- Background: `docs/MEMORY_MODULE_PLAN.ru.md` (the PC memory module this builds on).
