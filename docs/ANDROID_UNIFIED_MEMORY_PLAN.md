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
4. **Copy mode's GPU addressing, plus one buffer per shared granule.** Everything that is not shared
   goes through copy mode's sparse arena unchanged. A shared granule is reached through its own ordinary
   VkBuffer, never sparse-bound into the arena (see GPU view). No layer memory module.
5. **The GPU never writes emulator-made data into shared memory** (re-tiled images, HTile fills): blocks
   those land on stay copies.
6. **Proven before used.** Coherence of shared memory is checked at start; any failure means copy mode
   with one log line naming why.
7. **Per class, observable.** Buffer classes are enabled one at a time; a verify mode reports data that
   changed while the GPU used it.

## Memory layout

```
 guest VA (FEX)      0x10_0000_0000..  direct mappings (aliases allowed) ──┐
 backing view        backing_base + phys (runtime's own view) ─────────────┤ same pages
 GPU                 the granule's own VkBuffer (one per granule) ─────────┘ for SHARED granules

 physical pool [0, 5056 MiB), in granules of BB_UMA_GRANULE_KB (2 MiB):
   CPU     memfd range, lazily committed       GPU: VRAM copy + staging upload (copy mode)
   SHARED  dma-buf granule, CPU-cached         GPU: its buffer for read-only bindings in it, no copy;
                                               anything else: copy mode (arena copy, upload)
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

Requested by the GPU side when a read-only buffer binding of an enabled class lies in one CPU granule
(`UmaDirect`). Under the runtime's write lock:

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
6. Unfreeze; hand the granule to the GPU side, which binds the granule's buffer.

Never promoted: granules holding executable mappings, flexible memory, granules the GPU library cannot
allocate (they stay CPU, the binding takes a copy).

A granule fully released by the game is **demoted** later (phase 4): GPU blocks rebound to the
evicted-memory block, VkDeviceMemory freed after the GPU tick, backing view back to the memfd. Until
then released shared memory is cleared, not freed (as today's chunks).

### GPU view

**Granule buffers, bound directly (2026-10-18).** The first design bound arena blocks to shared
granules in place (`EnsureResident` → `BindInPlace`, the PC path). Device run 4 on the AYN Thor (Adreno
740, KGSL, Turnip Gen8 V37, `BB_UMA_CLASSES=index`, verify 64): 8 granules were promoted, and right
after "5 MiB of the arena bound in place" the **whole phone hard-reset** (no tombstone, no kernel
message). Sparse-binding dma-buf granule memory into the real arena (a 4 GiB sparse buffer, a KGSL VBO,
concurrent sharing, device addresses) kills the kernel, although the start-up probe (8 MiB exclusive
sparse buffer on a throwaway device) had passed. v1 bound the same kind of exported dma-buf memory
through ordinary (non-sparse) VkBuffers and never reset the phone. So: **shared memory is never
sparse-bound on KGSL** (nor anywhere under `BB_UMA`).

Now (`BufferCache::UmaDirect`, called by `ObtainBuffer` before copy mode's stream/arena paths):

- A binding is bound through its granule's own buffer (an ordinary VkBuffer over the granule's
  dma-buf memory, made once per granule, `GuestChunkSource`), at offset `phys - granule phys`, when
  all of this holds:
  - its class is enabled (`BbUma::ClassAllowed`);
  - it is read-only (a written binding takes a copy, logged once: GPU writes into shared memory would
    diverge from copy mode's tracking of the arena's copy);
  - it is larger than the stream threshold (16 KiB) and not `force_resident` (DMA operands, fault
    refetches): smaller reads stay in the stream buffer / constant ring as in copy mode;
  - it lies in **one** direct mapping and **one** granule (`phys` granule-aligned down);
  - it is not GPU-modified in copy mode's tracking (pages or `gpu_modified_ranges`): the arena's copy
    holds newer GPU data until the CPU's access brings it back (`DownloadMemory`);
  - it overlaps no GPU-written image (render/depth/storage target), and a texel binding no image or
    metadata at all (those are synced from the image on copy mode's path);
  - its granule is shared, or promotion succeeds (`runtime_memory_promote`; a refused granule is not
    asked again for 5 s).
- No arena residency, no upload and no page protection for it. Everything else takes copy mode's
  arena path unchanged; its upload reads shared pages through the guest VA as a CPU snapshot, which is
  fine. A range may be read live through the granule's buffer and also have an arena copy: the copy
  keeps being refreshed by copy mode's write tracking, and as written bindings always take the copy,
  the copy can hold newer GPU data only while the range is GPU-modified, which sends reads to the copy.
- Consequence: **bindings crossing a 2 MiB granule boundary take copies**, and so does every index or
  vertex range larger than a granule. `BB_UMA_GRANULE_KB` larger (4096, 8192, ...) shares more such
  bindings at the cost of commit rounding (a promoted granule commits all of its pages).
- The arena is exactly copy mode's: no external-memory flag, `in_place_blocks` stays empty,
  `IsInPlace()` is false, unmaps change nothing in it. `IsAnyInPlace()` answers "touches a shared
  granule" (`runtime_memory_granule_shared`): DMA fills and copies into shared memory are done by the
  GPU in stream order (into the arena's copy, then GPU-modified: earlier draws reading the granule live
  see the old bytes, later ones the copy), DMA sources there are CPU snapshots (`ObtainSnapshot`).
- `GarlicInVram()` is false on phones; the copy FIFO (`BB_VRAM_COPY_QUEUE`) works as in copy mode.
- Counters (the `UMA:` line): granule buffer bindings and MiB, refusals by class, written, cross-granule,
  mapping, GPU data, image, backoff.

### Dynamic blocks: one-shot write traps (2026-10-18)

Runs 5 (`index`) and 6 (`index,vertex`) with granule buffers were stable (28-30 FPS, no reset); verify
found one per-frame vertex buffer rewritten while the GPU used it. So a binding bound in place arms a
one-shot write trap (`BbLayer::WriteTraps::UmaWatch` = 4, read-only) on its 64 KiB guest blocks
(`PageManager::UmaArm`, blocks not armed or dynamic yet). The first CPU write there, as a fault
(`GuestFaultSignalHandler`) or announced by the runtime (libc copies through
`runtime_memory_prepare_cpu_write` → `Rasterizer::OnCpuWrite`, file reads and loaders through
`NoteAssetWrite`), marks the block **dynamic** (`BbUma::Dynamic`, lock-free bitmaps by guest VA) and
lifts the trap; a verify mismatch marks its blocks too. `UmaDirect` refuses ranges touching dynamic
blocks (copies; `dynamic` in the `UMA:` line). A block stays dynamic until its mapping changes (map and
unmap hooks forget both marks; the runtime forgets the traps then, and keeps them across promotions
and the game's mprotect).

The page manager (copy mode's write tracking) protects the same pages with the runtime's other
mprotect, which ignores the traps (the last change wins). Hence: pages it denies reads are not armed
(they get the trap when it lets them go); when it lets an armed page go it keeps it read-only for the
trap (`UmaProtect`); the trap is lifted only where it does not restrict the page itself, elsewhere when
it lets the page go; a fault on a page both protect goes on to its own handler.

### Verdict memo (2026-10-18)

Run 7 (`index,vertex`, 8 MiB granules, dynamic traps) was stable but the GPU thread went from ~30-35
to ~54 us a draw. `UmaDirect` now keeps the last verdict per range in a per-thread table (1024
entries, no lock): a direct binding (granule buffer + offset) stays valid while the runtime's mapping
table generation and `BbUma::verdict_epoch` are unchanged; mapping, cross-granule and dynamic refusals
while the generation is. The epoch moves when a block becomes dynamic or forgets its marks, when the
copy path is about to mark GPU data over an armed block (the only place copy mode marks pages
GPU-modified), and the first time each image becomes GPU-written (`Image::NoteGpuWritten`, at every
site that sets `GpuModified`, `render_target`, `depth_target` or `storage`). GPU-data, image and
backoff refusals and texel buffers are evaluated every time. `UmaArm` reads the armed bit before any
atomic write or lock. The `UMA:` line shows memo hits / misses.

### Copy mode everywhere else (2026-10-18)

The first device run (`BB_UMA_CLASSES=none`, nothing shared) used 2.3 GB of VRAM copies against
copy mode's 0.5 GB and crashed the driver: `BB_GUEST_IN_PLACE=1` had switched on the PC in-place model
(`GuestInPlace()`: GPU-ordered WRITE_DATA/labels/timestamps/GDS stores, image downloads into the arena,
the copy shader translated, game hooks, unmaps evicting copies, ...) for memory that is not shared.
Now, under `BB_UMA`:

- `GuestInPlace()` is **false** (guest_in_place state 3): every site of the generic in-place model,
  `ConstantsInPlace()` included, behaves as `BB_GUEST_IN_PLACE=0`. `WriteTracking()` is on.
- `UmaActive()` (state 3) is asked only by the probes, `ObtainBuffer` (`UmaDirect`, see GPU view) and
  `IsAnyInPlace` (shared granules). (The arena-in-place variant of this fix, with `ArenaInPlace()`,
  `UmaInPlaceWritten` and the spare-block unmaps, is gone with the move to granule buffers.)
- WRITE_DATA, labels and image downloads stay CPU writes at copy mode's moment; into a shared granule
  they land in the memory the GPU reads, so no upload is needed. The cost: GPU work recorded earlier but
  run later sees the new value (the ordering hazard below; verify mode reports it).
- The ProbesOnly / failed-probe paths turn UMA off in the buffer cache's constructor, before the
  first binding exists: copy mode throughout.

### Classes

The rasterizer tags each buffer binding before `ObtainBuffer`; only enabled classes may promote, and
only their read-only bindings use shared memory (written ones take copies for now):

| Class | Bindings | Hazard | Default |
|---|---|---|---|
| `index` | index buffers | low (mostly static) | on |
| `vertex` | vertex buffers (V# fetch) | low-medium | on |
| `constant` | uniform/constant buffers over 16 KiB, constants in place | high (rewritten every frame) | off |
| `storage` | storage buffers, read and written | medium | off |
| `texel` | formatted (texel) buffers | medium; may overlap GPU-written images | off |
| `indirect` | indirect draw/dispatch arguments | low | off |

Always copies: texture upload sources, DMA sources, arena uploads (`BB_CHUNK_UPLOADS` off), blocks
written by `SynchronizeMemoryFromImage`, bindings of 16 KiB and less (stream buffer, constant ring),
written bindings, bindings crossing a granule or a direct mapping, GPU-modified ranges.

### Coherence

Checked once at start, before any granule is shared:

1. **KGSL flags diagnostic:** import a dma-heap buffer into our own `/dev/kgsl-3d0` context, once with
   flags 0 (what Turnip does) and once with `IOCOHERENT | WRITEBACK`; log the flags `GPUOBJ_INFO`
   returns. This says what the kernel does by default and whether IO-coherent import exists.
2. **Strict roundtrips** on a 32 MiB exported buffer through a runtime-style `mmap` of the dma-buf fd:
   CPU write → GPU read (whole buffer, and a single 64-byte line written right before the submit);
   GPU write → CPU read with the lines pre-loaded into the CPU caches; from two threads on different
   cores.
3. ~~Sparse binding of shared memory~~ on a throwaway VkDevice: **not used** (shared memory is never
   sparse-bound; it passed on the Thor while the real arena hard-reset the phone). The code stays;
   `BB_UMA_SPARSE_PROBE=1` runs it for the log only.

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
- Staging keep (`BB_STAGING_KEEP_MB`) stays at copy mode's 512 MiB per ring (the buffers that are not
  shared still go through staging).

## Switches

| Switch | Meaning | Default |
|---|---|---|
| `BB_UMA=1` | unified memory v2 (run-bionic.sh adds `BB_GUEST_IN_PLACE=1 BB_LAYER_MEMORY=0 BB_WRITE_TRACKING=1 BB_CHUNK_UPLOADS=0`) | off |
| `BB_UMA_CLASSES` | comma list of classes, or `all` | `index,vertex` |
| `BB_UMA_GRANULE_KB` | granule size, power of two, 256..65536 (larger: fewer bindings cross a granule, more commit rounding) | 2048 |
| `BB_UMA_VERIFY` | verify one in N in-place bindings (0: off) | 0 |
| `BB_UMA_ALLOW_NONCOHERENT=1` | debugging only: share even if the coherence probe fails | off |
| `BB_UMA_ORDER=1` | phase 3: game writes wait for in-flight GPU reads of shared blocks | off |

`BB_UNIFIED=1` (v1, layer mode) stays in the code for reference but is no longer wired to the Debug
option.

## Phases

1. **Setup (this change):** design, runtime granules and promotion, probes and diagnostics, class
   policy, binding of shared granules (now through granule buffers, see GPU view), accounting, verify mode, switch. Builds; untested on a
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
- **KGSL limits:** sparse binding of imported dma-bufs hard-resets the Thor (run 4): never done.
  One ordinary VkBuffer per shared granule (512 per GiB) is what v1 did without trouble;
  `KGSL_PROP_GPU_VA64_SIZE` unknown.
- **Granule boundaries:** bindings crossing a granule (and all ranges larger than one) take copies.

## Other waste (not unified memory)

- `BB_STAGING_KEEP_MB` keeps up to 512 MiB of idle staging per ring.
- The initial upload of a new render target reads its never-written guest pages, which commits them in
  the memfd.

## Key files

- `src/runtime_memory.c`: granules, promotion, freeze, map_phys, zero_phys; `src/probe.c` fault chain;
  `src/runtime_file.c` read guard.
- `gpu/shim/bbport_guest_memory.cpp`: UMA mode, probes, granule allocator, chunk index.
- `gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp`: `UmaDirect`, `UmaAnyShared`,
  `GuestChunkSource`, `ObtainBufferForImage`, `SynchronizeMemoryFromImage`.
- `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp`: binding classes.
- `gpu/shadps4/video_core/texture_cache/texture_cache.cpp`: collector usage.
- `gpu/shadps4/core/libraries/videoout/driver.cpp`: memory split line.
- `tools/android/run-bionic.sh`, `DebugDialog.java`: the switch.
- Background: `docs/MEMORY_MODULE_PLAN.ru.md` (the PC memory module this builds on).
