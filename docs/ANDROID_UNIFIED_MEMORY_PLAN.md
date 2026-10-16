# Android unified memory plan

Goal: the game's direct memory and the GPU share the same physical pages on phones (one RAM for CPU and
GPU, like the PS4), instead of the GPU side keeping copies of the game's buffers. Target: much less RAM
(the Find N5 uses ~4.4 GB process + 5.9 GB GPU memory in game), no readbacks for in-place ranges, FPS
equal or better. Textures stay copies (the GPU cannot sample the PS4's tiled layout).

Branch: `testing` (local `bionic-ndk`). Opt-in switch until proven: `BB_UNIFIED=1` / bbport.ini
`unified_memory=1`. Test devices: Thor (Adreno 740), Find N5 (Adreno 830), a Mali/PanVK device for the
fallback.

## Where we are

Most of the machinery exists already, merged from upstream 0.5 (the PC "memory module"):

- `src/runtime_memory.c` keeps the game's direct memory in one memfd mapped at the game's fixed
  addresses (0x10_0000_0000..). When the GPU library registers a chunk allocator, `map_phys` maps
  **Vulkan-exported dma-bufs** there instead (whole mode: one region per direct allocation).
- `gpu/shim/bbport_guest_memory.cpp` allocates those chunks (`AllocChunk`) and probes them
  (`Usable()`, `DmaBufWorks`); `gpu/layer/bblayer_gpu_memory.*` and `buffer_cache_layer.cpp` bind the
  same chunk buffers **in place** for the GPU (BDA + page table for bindings of 64 MiB and more).
- On an integrated GPU `GarlicInVram()` is false: no VRAM mirrors, which is what a phone wants
  (upstream's Steam Deck path).
- Android hard-codes the old copy model: `tools/android/run-bionic.sh` sets `BB_GUEST_IN_PLACE=0
  BB_UFFD=0 BB_PREUPLOAD=0` (since the first Thor script, no recorded reason).

Why switching it on today would fall back to copies:

1. **Memory type.** Turnip exposes only DEVICE_LOCAL types (type 0 write-combined + coherent; cached +
   coherent only with KGSL IO-coherence). `FindType` / `DmaBufWorks` require a cached,
   non-device-local type, so `Usable()` fails ("PC memory model is off with this driver").
2. **Gating.** `Install()` returns early with `NonSparseArenas()` and `buffer_cache.cpp` disables
   in-place without sparse buffers; the layer module does not actually need sparse.

Today's copies, per frame: game memory → HostUncached staging → arena buffer (buffers); staging →
detile buffer → VkImage (textures). In place removes the first copy of both.

## Architecture

- Direct memory = whole-allocation dma-buf chunks exported by Turnip (`VK_KHR_external_memory_fd` +
  `VK_EXT_external_memory_dma_buf`; Turnip allocates them from `/dev/dma_heap/system`, CPU-cached),
  mapped by `map_phys` at the game's addresses for FEX, bound as VkBuffers for the GPU.
- Flexible memory and non-direct ranges stay on the memfd and today's arena path.
- Guest-write detection: mprotect write tracking (`BB_WRITE_TRACKING=1`) first — the game's libc runs
  under FEX, so the "announced writes" model (libc/loader hooks) is not trusted on Android yet. `BB_UFFD`
  stays off (no userfaultfd on dma-buf; SELinux).
- Honest labels (forced on in place) keep CPU/GPU ordering as on the PS4.
- Alternative for later: allocate the chunks in the runtime from `/dev/dma_heap/system` and import them
  into Vulkan (removes the Vulkan-before-memory ordering and the 4 GiB per-allocation clamp).

## Phases

### 1. Measure (no behaviour change)
- Log the Vulkan memory type table (flags, heap per type) and the `Sparse arena probe` lines.
- Log a memory split every ~10 s with `BB_FRAME_STATS=1`: images (tiled/linear), arena residency,
  staging, chunk bytes; `/proc/self/smaps_rollup` (memfd / dmabuf / anon) and KGSL per-process sysfs.
- Baseline on the N5 and the Thor: same save, same route, 5 minutes.
- **Checkpoint:** a table of where the 10 GB goes; go/no-go on the expected saving.

### 2. Probe the hardware
- Extend `DmaBufWorks` with GPU tests: GPU write → CPU read, CPU write → GPU read (as `HostImportWorks`
  does), cached vs write-combined bandwidth, LSE/exclusive atomics through the mapping, mprotect +
  SIGSEGV on a dma-buf mapping, `DMA_BUF_IOCTL_SYNC` need.
- **Checkpoint:** pass/fail lines per device. An uncached-only device (FEX would crawl) stays on copies.

### 3. Make it start
- `FindType`/`DmaBufWorks`: on an integrated GPU accept DEVICE_LOCAL|HOST_VISIBLE, prefer
  cached+coherent, else cached (with explicit flush/invalidate), never write-combined for game memory.
- Allow layer mode without sparse; non-chunk ranges keep the Backed arenas.
- `run-bionic.sh`: `BB_UNIFIED=1` → `BB_GUEST_IN_PLACE=1 BB_LAYER_MEMORY=1 BB_PC_MODEL=1
  BB_WRITE_TRACKING=1`, keep `BB_COPY_GPU_BUFFERS=1` (PM4 command copies, unrelated).
- **Checkpoint:** log "direct memory … in GPU-visible memory (dma-buf)", title menu renders on the N5.

### 4. Correctness
- Load, play, cutscenes, area changes, character creation, LSFG on/off; watch `Layer memory:` and
  image-sync logs; constants rewritten while draws are queued (`BB_CONSTANTS_IN_PLACE`).
- **Checkpoint:** 15+ minutes on both devices without device lost, stale data or texture ghosting.

### 5. Memory and speed
- Retune the texture budget formula (arena and staging memory vanish), staging keep, direct memory
  commit (dma-heap is not lazy: 5056 MB is real from the start; check lmkd).
- **Checkpoint:** measured RAM drop vs phase 1, FPS equal or better, no low-memory kills on 12 GB.

### 6. Rollout
- Automatic fallback on Mali/PanVK and drivers that fail the probes (today's path, logged).
- Settings toggle (Experimental: Unified memory), then default on for Adreno once tested; later try
  announced writes instead of mprotect tracking for less fault overhead.

## Risks

- **Coherence:** KGSL IO-coherence of imported dma-bufs is unverified; may need dma-buf sync ioctls.
- **Committed memory:** dma-heap pages are allocated up front (the memfd was lazy).
- **Limits:** 4 GiB-1 per allocation, 2 GiB storage range (page-table path for big bindings; 64-bit
  pointer loads cost on Adreno unknown), 256 chunk slots, `vm.max_map_count` with many trap ranges.
- **Races:** in place, the GPU reads what the game writes now; queued draws can see rewritten
  constants (copies hid this). Honest labels and constant snapshots must hold.
- **Mali/PanVK:** dma-buf export on kbase unknown — must fall back cleanly.

## Key files

- `src/runtime_memory.c` (map_phys, whole chunks, regions)
- `gpu/shim/bbport_guest_memory.cpp` (chunk allocator, memory type choice, probes, Install)
- `gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp`, `buffer_cache_layer.cpp`, `page_manager.cpp`
- `gpu/layer/bblayer_gpu_memory.*`
- `gpu/shadps4/core/libraries/videoout/driver.cpp` (Memory: stats line)
- `tools/android/run-bionic.sh`
- Background: `docs/MEMORY_MODULE_PLAN.ru.md`, upstream 0.6-pre1 `docs/CHANGES_after_0.5.md`
