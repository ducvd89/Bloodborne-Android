// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "bbport_copy.h"

#include <atomic>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <array>
#include <memory>
#include <optional>
#include <unordered_map>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

/// bbport: an IntervalList whose every change advances `generation`: lookups of the buffer
/// cache's residency remember their answers for as long as nothing changed (ResidencyMemo).
template <class IV = Interval>
class TrackedIntervalList : public IntervalList<IV> {
public:
    void Add(IV value) {
        ++generation;
        IntervalList<IV>::Add(value);
    }
    void Subtract(u64 start, u64 end) {
        ++generation;
        IntervalList<IV>::Subtract(start, end);
    }
    void Clear() {
        ++generation;
        IntervalList<IV>::Clear();
    }
    static inline u64 generation = 1;
};

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    static constexpr u64 ARENA_PAGE_BITS = 32;
    static constexpr u64 ARENA_PAGE_SIZE = u64{1} << ARENA_PAGE_BITS;
    static constexpr u64 NUM_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;

public:
    /// Read-only bindings up to this size are copied into a stream buffer (bbport: public for
    /// the draw pipeline's constant ring).
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// bbport: a guest write fault at `device_addr` was handled; unprotect its neighbourhood.
    void ExtendWriteFault(VAddr device_addr, u64 guest_rip = 0);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// bbport BB_LAYER_READ_TRAPS: a CPU access hit a page whose newest data the GPU wrote into a
    /// VRAM copy only: the block's GPU data is copied back into the game's memory first (on the
    /// GPU command thread unless `assume_locks`). False: not such a page.
    bool LayerReadTrapHit(VAddr address, bool assume_locks);

    /// bbport: a new draw or dispatch is being recorded: the per-binding housekeeping of
    /// EnsureResident (unmaps, demotions, assets, late writes) runs once for it, not per binding.
    void NewPacket() noexcept {
        ++packet_epoch;
    }
    /// bbport: GPU writes of the bindings resolved now stay in the game's memory (no VRAM copy).
    bool force_writes_in_place = false;

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// bbport BB_GUEST_IN_PLACE: where a command processor write (WRITE_DATA) the GPU performs in
    /// stream order lands: the arena over the game's memory, if the range is bound there in place.
    /// Not counted as a GPU write of the block (those move blocks to VRAM): it is CPU data, as a PC
    /// game's constant updates. The range counts as GPU-modified until the GPU has done it, so no
    /// copy of it is taken before (IsRegionGpuModified).
    [[nodiscard]] std::optional<std::pair<const Buffer*, u64>> CommandWriteTarget(VAddr device_addr,
                                                                                u32 size);
    /// bbport BB_LAYER_MEMORY: the mirror holding all of a command write's range, written along
    /// with the game's memory (the range keeps its VRAM copy); empty: none, it goes in place.
    [[nodiscard]] std::optional<std::pair<const Buffer*, u64>> CommandWriteMirror(VAddr device_addr,
                                                                                u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

    /// bbport (BB_PREUPLOAD): notes a guest mapping or unmapping (Rasterizer::MapMemory): the
    /// game's GPU memory (direct memory of type 3 with GPU access) is pre-uploaded.
    void NotePreuploadMapping(VAddr addr, u64 size, bool mapped);
    /// bbport (BB_PREUPLOAD): uploads up to `budget` bytes of that memory which the CPU changed and
    /// then left alone for a few frames into the arena, ahead of its use (draw recording thread).
    void Preupload(u64 budget);
    /// bbport (BB_GPU_WRITE_TWINS): forgets the twins of unmapped guest memory.
    void DropTwins(VAddr addr, u64 size);

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);
    /// bbport (GPU watchdog): prints how far the GPU got with the arena binds (vkQueueBindSparse).
    void ReportArenaBinds();

    /// bbport BB_GUEST_IN_PLACE: the game unmapped [addr, addr + size) (a guest thread): arena
    /// blocks bound to its memory there are unbound before the next binding (GPU side).
    void UnmapInPlace(VAddr addr, u64 size);
    /// Whether the range is bound to the game's own memory (no uploads, tracking or readbacks).
    [[nodiscard]] bool IsInPlace(VAddr addr, u64 size) const;
    /// bbport BB_GUEST_IN_PLACE without write tracking: [addr, addr + size) gets data through a path
    /// the GPU side hears of (file reads, the game's resource loaders, DMA): its blocks may then
    /// keep a VRAM copy (any thread).
    void NoteAssetWrite(VAddr addr, u64 size);
    /// bbport: the game's GPU memory allocator handed out [addr, addr + size) again (any thread): its
    /// blocks lose the asset mark and are bound in place, since what writes them next may be code
    /// the GPU side does not hear of.
    void NoteFreshRange(VAddr addr, u64 size);
    /// bbport, without write tracking: the command processor wrote `data` at addr in decode order.
    /// Where the GPU's data there lives in VRAM (GPU-modified, not in place) the same bytes are
    /// written into it in stream order and true is returned; else the caller invalidates.
    bool UpdateGpuWritten(VAddr addr, std::span<const u8> data);
    /// bbport: copies of [addr, addr + size) in VRAM (ShadowCopy) are stale from here on in stream
    /// order: the GPU or the command processor wrote there (GPU side thread).
    void DropShadows(VAddr addr, u64 size);
    /// bbport, without write tracking (any thread): the command processor wrote these bytes into the
    /// game's memory outside the command stream (labels, timestamps, occlusion results; through
    /// the backing mapping, which nothing sees). Where VRAM holds a copy of them it gets them too, at
    /// the next binding.
    void NoteLateCommandWrite(VAddr addr, const void* data, u64 size);
    /// bbport, without write tracking (any thread): the CPU wrote [addr, addr + size) next to data
    /// the GPU keeps in VRAM. Exactly those bytes are copied into VRAM from the game's memory at the
    /// next binding: no readback of the GPU's data, nothing uploaded over it.
    void NotePreciseUpload(VAddr addr, u64 size);
    /// bbport: the CPU wrote [addr, addr + size) where the GPU side asked to hear of it (no write
    /// tracking): counted per block like a write fault, for moving per-frame data in place.
    void NoteCpuWriteRange(VAddr addr, u64 size);
    /// BB_CHUNK_TEXTURES: texture data read by the GPU straight from the guest memory chunk it is
    /// in (no CPU copy, no VRAM buffer copy), when the range is contiguous there. Also the target
    /// of labels the GPU writes (Rasterizer::WriteLabelOnGpu). GPU side threads.
    std::optional<std::pair<const Buffer*, u64>> GuestChunkSource(VAddr address, u64 size);
    /// bbport BB_LAYER_MEMORY: whether the layer's memory module binds the game's memory.
    static bool LayerActive();
    /// bbport BB_LAYER_MEMORY: [addr, addr + size) touches a block with a VRAM copy (it may hold the
    /// GPU's newest data): bound through the module, never copied from the game's memory.
    [[nodiscard]] static bool LayerMirrored(VAddr addr, u64 size);
    /// Paged bindings in use (BB_LAYER_PAGED=0: off); GPU-written data in VRAM (BB_LAYER_GPU_WRITES).
    static bool LayerPagedActive();
    /// BB_LAYER_GPU_WRITES: where GPU writes may land in VRAM copies: 0 nowhere (in place), 1 (the
    /// default) in blocks the CPU has not written since they were handed out (layer_cpu_blocks:
    /// render targets and other GPU-only data), 2 anywhere.
    static int LayerGpuWritesMode();
    bool LayerGpuWriteToVram(VAddr address, u64 size);
    bool LayerInImage(VAddr address, u64 size);
    /// A paged binding (Shader::IsPagedBuffer): the record its shader reads ({address, size,
    /// trash address}, see EmitContext::PagedPointer), with page table entries for the range.
    std::pair<const Buffer*, u64> LayerPagedRecord(VAddr address, u64 size, bool is_written);
    /// The CPU wrote [address, address + size): VRAM copies there are refreshed before paged
    /// bindings read them.
    void LayerNoteCpuWrite(VAddr address, u64 size);
    /// bbport BB_LAYER_MEMORY: [address, address + size) in place as a range of its chunk's buffer,
    /// when the layer's memory module finds it whole in one chunk.
    /// clamp_huge: a binding of 64 MiB or more that runs on past its chunk is cut at the end of
    /// the part in place (the descriptor range is clamped to the buffer).
    std::optional<std::pair<const Buffer*, u64>> LayerInPlace(VAddr address, u64 size,
                                                              bool clamp_huge = false);
    /// Whether any part of the range is.
    [[nodiscard]] bool IsAnyInPlace(VAddr addr, u64 size) const {
        if (LayerActive() && size != 0) {
            return !IsInVramLayer(addr, size); // bbport BB_LAYER_MEMORY: not all in one mirror
        }
        return GuestInPlace() && size != 0 &&
               in_place_blocks.Overlaps(addr >> block_shift, ((addr + size - 1) >> block_shift) + 1);
    }
    /// bbport BB_LAYER_MEMORY: [addr, addr + size) whole in one VRAM mirror.
    [[nodiscard]] bool IsInVramLayer(VAddr addr, u64 size) const;

private:
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    /// `in_place`: bind what can be in place even when Garlic (texture data, read once into an
    /// image: a VRAM copy of it would only cost an upload).
    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block,
                        bool in_place = false);
    /// BB_GUEST_IN_PLACE: binds the blocks of [start, end) that are direct memory in a guest memory
    /// chunk to that memory; the others are added to `rest` (VRAM, as before).
    struct ResidentBind {
        u64 start, end; ///< blocks
        vk::DeviceMemory memory;
        u64 offset;
    };
    void BindInPlace(u64 start, u64 end, std::vector<ResidentBind>& out, IntervalList<>& rest,
                     bool any_type = false);
    /// bbport: the housekeeping EnsureResident did before every binding (Process*), once a packet.
    void Maintain();
    u64 packet_epoch = 1, maintained_epoch = 0;
    /// bbport: answers of EnsureResident ("all resident") and IsInPlace for block ranges, valid
    /// while resident_ranges and in_place_blocks keep their generation.
    struct ResidencyMemo {
        u64 first = ~0ull, last = 0, generation = 0;
        bool resident = false, in_place = false;
    };
    std::array<ResidencyMemo, 256> residency_memo{};
    static u64 ResidencyGeneration() noexcept {
        return TrackedIntervalList<Backing>::generation + TrackedIntervalList<>::generation;
    }
    void ProcessPendingUnmaps();
    /// bbport BB_GUEST_IN_PLACE: a write fault (guest thread). Blocks the CPU keeps writing (faults in
    /// 3 frames of 60) move from VRAM to the game's memory in place: dynamic data, like an upload
    /// heap; static data stays in VRAM, like a default heap.
    void NoteCpuWrite(VAddr address, u64 guest_rip);
    /// GPU side: moves the requested blocks once the submission that may still upload into their
    /// VRAM copy was submitted; the rebind waits for it on the GPU (bind_wait_tick).
    void ProcessDemotions();
    u64 demotions_scanned_tick = ~0ULL;
    void ProcessPendingAssets();
    void ProcessLateWrites();
    /// BB_FRAME_STATS: GPU-written bindings over 64 KiB, reported every 5 s (where they are bound,
    /// which images lie over them).
    void NoteLargeGpuWrite(VAddr addr, u64 size);
    /// bbport BB_GUEST_IN_PLACE on a discrete GPU, BB_SHADOWS=1 (off by default: 85 copies, 20 MB a
    /// frame in Yahar'gul cost more than reading in place, 48 -> 44 FPS): a read-only binding of data the
    /// CPU writes in place is copied once per submission by the GPU into VRAM and bound there, as a
    /// PC game copies an upload heap into a default heap: one sequential copy over PCIe instead of
    /// every shader reading it across the bus.
    std::optional<std::pair<const Buffer*, u64>> ShadowCopy(VAddr addr, u64 size);
    struct Shadow {
        VAddr addr;
        u64 size, offset;
    };
    std::unique_ptr<StreamBuffer> shadow_buffer;
    std::vector<Shadow> shadows; ///< this submission's copies
    u64 shadow_tick = 0;
    /// BB_TRACE_SIZE=bytes (diagnostics): bindings over GPU-written buffers of that size, counted by
    /// the path they take, reported every 5 s.
    void TraceBinding(VAddr addr, u64 size, bool is_written, int kind);
    std::vector<std::pair<VAddr, u64>> traced_ranges;
    std::array<u64, 10> trace_counts{};
    u64 trace_texel = 0, trace_min = ~0ull, trace_max = 0;
    std::chrono::steady_clock::time_point trace_report{};
    struct LargeWrite {
        VAddr addr = 0;
        u64 size = 0, count = 0;
        u64 fresh = 0; ///< ranges handed out again over it in the window
        bool in_place = false;
        std::string images;
    };
    std::unordered_map<u64, LargeWrite> large_writes;
    std::chrono::steady_clock::time_point large_writes_report{};
    /// Without write tracking: [addr, addr + size) is bound for GPU writes (GPU side thread).
    void NoteGpuWrite(VAddr addr, u64 size);
    /// Whether a block may keep a VRAM copy without write tracking: Garlic data only loaders wrote.
    bool VramEligible(u64 block, bool garlic_known = false);
    /// Called as a submission goes out (its tick `submitted`): candidate blocks get VRAM for the next
    /// submission, bound once `submitted` is done on the GPU, filled from the game's memory there.
    /// Called as a submission goes out: blocks partly handed out again whose GPU data is in VRAM get
    /// that data copied back into the game's memory at its end, and are bound in place for the
    /// next submission (once `submitted` is done).
    void QueueCopyBacks(u64 submitted);
    void QueuePromotions(u64 submitted);
    void ReportInPlaceBlocks(); ///< bbport BB_RESIDENCY_REPORT (diagnostics)
    /// Arena residency memory from the 64 MiB blocks: the memory and the byte offset in it.
    std::pair<vk::DeviceMemory, u64> AllocateResidency(u64 bytes);
    /// bbport: BbStats::residency_unused_bytes after the free list or the current block changed.
    void NoteResidencyUnused();
    /// bbport: idle VRAM blocks (see group_use).
    void NoteUse(VAddr address, u64 size);
    /// bbport BB_RESIDENCY_MIX=1 (diagnostics): bindings over blocks in VRAM and in place at once.
    void NoteResidencyMix(u64 first_block, u64 last_block, u64 size, bool is_written);
    void ProcessIdleBlocks();
    /// A VRAM slot of a block that left it: reused by the next move to VRAM.
    void ReleaseResidencySlot(vk::DeviceMemory memory, u64 offset);
    /// Unbinds a VRAM block of memory the game unmapped (uploaded again if mapped and used).
    void EvictVramBlock(u64 block);
    [[nodiscard]] static u32 VramIdleSeconds();
    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    /// bbport: batched small copies on the copy threads (BbCopy::QueueCopy).
    static void RunGuestCopy(const BbCopy::Item& item);
    void SmallGuestCopy(const BbCopy::Item& item);

    const Buffer* UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                               size_t total_size_bytes);

    /// The image at device_addr, if any, tiled into `buffer` at `offset` (where the range is).
    bool SynchronizeMemoryFromImage(const Buffer* buffer, u64 offset, VAddr device_addr, u32 size);
    /// bbport BB_LAYER_MEMORY (statistics): a binding not whole in one chunk.
    void NoteLayerMiss(VAddr address, u64 size, bool is_written);
    /// bbport BB_LAYER_MEMORY (buffer_cache_layer.cpp): a binding through the layer's memory
    /// module: a mirror in VRAM, or in place (empty: neither; the arena takes it). may_promote
    /// false (image uploads): a mirror only where the range is in VRAM already, none made for it.
    std::optional<std::pair<const Buffer*, u64>> LayerBind(VAddr address, u64 size, bool is_written,
                                                           bool is_texel_buffer,
                                                           bool may_promote = true);
    /// Whether a block may get a VRAM copy (all of it loaded data, in a guest chunk).
    bool LayerEligible(u64 block);
    /// BB_LAYER_WATCH: whether a block with unannounced bytes may get a watched VRAM copy.
    bool LayerWatchable(u64 block);
    static bool LayerWatchEnabled();
    static bool LayerWatchHot();
    /// A watched block the CPU wrote: uploaded again before the next binding, trap set again.
    void LayerRefreshWatched(u64 block);
    /// Every block of [first_block, end_block) in VRAM already or eligible (LayerEligible).
    bool LayerAllKnown(u64 first_block, u64 end_block);
    /// Candidate blocks (sorted) copied to VRAM: mirrors created or merged.
    void LayerPromote(const std::vector<u64>& blocks);
    u64 LayerPromoteRun(u64 first_block, u64 end_block);
    /// The end block of a mirror over [lo, hi) that grows: room above it to grow into.
    u64 LayerGrowthEnd(u64 lo, u64 hi);
    /// Blocks [first, end) back in place (copy_back: what the GPU wrote in VRAM first).
    /// reason (statistics): 0 written binding, 1 mixed binding, 2 command write, 3 demotion
    /// request, 4 copy back, 5 unmap, 6 idle.
    void LayerDemote(u64 first_block, u64 end_block, bool copy_back, int reason);
    /// Frees retired mirrors the GPU is done with.
    void LayerMaintain();
    /// Mirrored blocks not bound for BB_VRAM_IDLE_SECONDS go back in place.
    void LayerProcessIdle();
    /// VRAM near the texture collector's critical mark: idle mirrors go back in place.
    void LayerYieldToImages(u32 now);
    /// Makes mirrors with memory left without valid blocks again over those blocks only.
    u64 LayerCompactMirrors(u64 wanted);
    /// The GPU writes [address, address + size) in the submission being recorded (write ticks).
    void NoteWriteTick(VAddr address, u64 size);
    /// Page table entry of a block (LayerPagedRecord), its writes, and rewrites after changes.
    u64 LayerEntry(u64 block);
    void LayerWriteEntries(u64 first_block, u64 end_block);
    void LayerRepublish(u64 first_block, u64 end_block);
    /// CPU writes into blocks with VRAM copies, uploaded before paged bindings.
    void LayerFlushUploads();
    void LayerForgetGpuData();
    /// The GPU's data in mirrors over [first, end) copied into the game's memory (blocks stay).
    void LayerCopyBack(u64 first_block, u64 end_block);
    /// Blocks of [first, end) that may go to VRAM become promotion candidates.
    void LayerQueuePromotion(u64 first_block, u64 end_block);
    /// Unknown blocks (LayerEligible false) made volatile in a mirror (layer_volatile).
    void LayerMakeVolatile(const std::vector<u64>& blocks);
    /// Write traps on blocks with VRAM copies (buffer_cache_layer.cpp): set when a block gets its
    /// copy, removed when it goes back in place or the CPU announces a write there (then it goes
    /// back in place too); hits are handled before the next binding.
    void LayerArmTraps(u64 first_block, u64 end_block);
    void LayerDisarmTraps(u64 first_block, u64 end_block, bool demote);
    void LayerProcessTraps();
    static u64 LayerTrapFaults();
    /// Before a binding of a mirror over volatile blocks: their bytes in it refreshed.
    void LayerRefreshVolatile(VAddr address, u64 size);
    static bool GarlicInVramForLayer();
    /// VRAM past BB_VRAM_PROMOTE_PERCENT of the budget, or within PromoteCriticalMargin of the
    /// texture collector's critical mark: no new copies of loaded blocks for now.
    bool VramPromotionsPaused();
    /// Headroom below the texture collector's critical mark: copies stop at it, and past
    /// ShrinkCriticalMargin idle mirrors go back in place (LayerProcessIdle).
    static constexpr u64 PromoteCriticalMargin = 384_MB;
    /// Freed beyond the pause mark when stale mirrors go (LayerYieldToImages): room for the
    /// copies waiting.
    static constexpr u64 StaleHeadroom = 256_MB;
    /// The mirrors' share of the driver's VRAM budget the texture collector's mark does not take.
    static u64 LayerMirrorFloor(u64 budget);
    static constexpr u64 ShrinkCriticalMargin = 128_MB;

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    // bbport: texel buffer ranges known not to alias an image, per image registry generation.
    struct ImageMiss {
        VAddr address = 0;
        u32 size = 0;
        u64 generation = ~0ULL;
    };
    std::array<ImageMiss, 256> image_miss_cache{};
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    RangeSet gpu_modified_ranges;

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;

    std::array<const Buffer*, NUM_ARENA_PAGES> address_space{};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    std::mutex preupload_mutex;               ///< the map hooks come from guest threads
    std::map<VAddr, VAddr> preupload_ranges; ///< start -> end of pre-uploaded guest memory
    VAddr preupload_cursor = 0;
    u64 preupload_first_bytes = 0, preupload_again_bytes = 0; ///< BB_FRAME_STATS report
    Vulkan::Semaphore memory_semaphore;
    /// bbport BB_LAYER_MEMORY: mirrors by start address, retired ones with the tick to wait for.
    std::map<VAddr, std::unique_ptr<Buffer>> layer_mirrors;
    std::vector<std::pair<std::unique_ptr<Buffer>, u64>> layer_retired;
    u64 layer_generation = 0;
    u64 layer_copied_back_bytes = 0; ///< statistics
    IntervalList<> layer_published; ///< blocks whose page table entries are written
    std::mutex layer_uploads_mutex;
    std::vector<std::pair<VAddr, u64>> layer_pending_uploads;
    /// CPU writes the GPU side heard of: their bytes lose their GPU-modified marks (LayerForgetGpuData).
    std::vector<std::pair<VAddr, u64>> layer_cpu_written;
    std::atomic<bool> layer_cpu_written_pending{false};
    std::atomic<bool> layer_uploads_pending{false};
    std::unique_ptr<Buffer> layer_trash; ///< where paged accesses out of range go
    /// Blocks with bytes nobody announced (an edge shared with other data, padding between
    /// images) in a mirror only so that the rest of a reading binding's range can be there:
    /// refreshed from the game's memory for every binding over them. Paged bindings and the
    /// module's readers see them in place.
    IntervalList<> layer_volatile;
    /// Blocks the CPU wrote since they were handed out (traps, announced writes): GPU writes there
    /// stay in place (LayerGpuWriteToVram).
    IntervalList<> layer_cpu_blocks;
    /// Blocks with bytes the CPU wrote unannounced, in a mirror for reading bindings anyway
    /// (BB_LAYER_WATCH, LayerWatchable): a write trap catches the CPU's next write, the block is
    /// uploaded again before the next binding and the trap set again. The GPU never writes them
    /// in VRAM (layer_cpu_blocks). On NVIDIA shaders reading the game's memory over the bus were
    /// slow where the model of 0.3 had copies in VRAM.
    IntervalList<> layer_watched;
    /// Watched blocks that went back in place for writes (LayerDemote): not watched again.
    IntervalList<> layer_watch_failed;
    std::unordered_map<u64, u8> layer_watch_strikes; ///< watched blocks sent back for requests
    /// Ranges a GPU-writing binding sent back in place once (LayerBind): the second time they
    /// stay there (dynamic_blocks).
    IntervalList<> layer_write_bounces;
    /// Per 2 MiB of guest addresses (as group_use): the second a binding last took a mirror there
    /// directly (LayerBind), or blocks there moved to VRAM. Paged bindings do not count. Stale
    /// ones go back in place first when VRAM is short (LayerYieldToImages).
    std::vector<u32> layer_group_use;
    void NoteDirectUse(VAddr address, u64 size);
    /// Dynamic blocks made so by the CPU's writes (ProcessDemotions), not by the GPU's.
    IntervalList<> layer_cpu_hot;
    u64 layer_volatile_refresh_bytes = 0; ///< statistics
    u64 layer_volatile_in_pass_bytes = 0; ///< statistics: volatile reads bound in place in passes
    /// BB_LAYER_READ_TRAPS: pages trapped for reads since the GPU wrote them in a VRAM copy only
    /// (their mutex: the draw recording thread arms, the GPU command thread lifts), the newest
    /// copy back recorded (a page without GPU data left waits for it), statistics.
    RangeSet layer_read_trapped;
    std::mutex layer_read_trap_mutex;
    u64 layer_copy_back_tick = 0;
    std::atomic<u64> layer_read_trap_hits{0}, layer_read_trap_bytes{0};
    static bool LayerReadTrapsOn();
    void LayerArmReadTraps(VAddr address, u64 size);
    void LayerCopyBackForCpu(VAddr address);
    u64 layer_watch_refreshes = 0;        ///< statistics: watched blocks uploaded again
    /// Statistics: trap hits announced; by the game's code on watched / other blocks; by other
    /// code on watched / other blocks.
    std::array<u64, 5> layer_trap_kinds{};
    std::atomic<u64> arena_bind_calls{0}, arena_bind_ranges{0}; ///< vkQueueBindSparse calls, ranges
    std::atomic<u64> arena_bind_signaled{0}; ///< memory_semaphore value the last call signals

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset; ///< in blocks, like start and end (SubRange, CanMergeWith)
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start)};
        }
    };
    TrackedIntervalList<Backing> resident_ranges;
    /// bbport BB_GUEST_IN_PLACE: blocks bound to the game's own memory, and guest unmaps to apply.
    TrackedIntervalList<> in_place_blocks;
    std::mutex pending_unmaps_mutex;
    std::vector<std::pair<VAddr, u64>> pending_unmaps;
    std::atomic<bool> unmaps_pending{false};
    std::mutex dynamic_mutex;
    std::unordered_map<u64, std::pair<u32, u32>> cpu_write_frames; ///< block -> last frame, frames
    std::unordered_map<u64, std::pair<u32, u32>> gpu_write_frames; ///< written range -> last frame, frames
    std::vector<u64> demote_requests;
    std::atomic<bool> demotes_requested{false};
    struct Demotion {
        u64 block;
        u64 tick;         ///< requested at
        bool cpu_written; ///< written by the CPU often: in place from now on (else: handed out again)
        bool idle = false;  ///< bbport: not bound for BB_VRAM_IDLE_SECONDS (ProcessIdleBlocks)
        bool force = false; ///< bbport: its chunk is emptied, idle or not (back once bound)
    };
    std::vector<Demotion> demotions_waiting;
    IntervalList<> dynamic_blocks;                     ///< bound in place even when Garlic
    /// Bytes last written through a path the GPU side hears of (file reads, the game's resource
    /// loaders, DMA) and not handed out again by the game's GPU memory allocator since. Only blocks
    /// made of such bytes keep a VRAM copy: anything else in a block may be written by code we do
    /// not hear of.
    IntervalList<> asset_bytes;
    IntervalList<> promote_candidates; ///< in-place blocks that may get a VRAM copy again
    IntervalList<> copy_back_blocks;   ///< VRAM blocks with GPU data, handed out again in part
    IntervalList<> gpu_written_bytes;  ///< bound for GPU writes since handed out (known data too)
    u64 promoted_bytes = 0;
    /// VRAM slots of blocks that left VRAM, for the next blocks moved there (in their chunks).
    u64 free_slot_count = 0;
    std::mutex pending_assets_mutex;
    struct PendingRange {
        VAddr addr;
        u64 size;
        bool fresh; ///< handed out again by the game's GPU memory allocator (else: an asset)
    };
    std::vector<PendingRange> pending_assets;
    std::atomic<bool> assets_pending{false};
    std::mutex late_writes_mutex;
    std::vector<std::pair<VAddr, std::vector<u8>>> late_writes;
    std::vector<std::pair<VAddr, u64>> precise_uploads;
    std::atomic<bool> late_writes_pending{false};
    u64 bind_wait_tick = 0;                            ///< the next arena binds wait for it
    std::array<std::unique_ptr<Buffer>, 256> chunk_buffers{}; ///< per guest memory chunk (Chunk::index)

    u32 arena_memory_type_index{};
    /// bbport: system memory for residency when VRAM is full (4 GB cards), if sparse binding takes it
    std::optional<u32> arena_fallback_type_index;
    vk::DeviceMemory residency_memory{}; ///< bbport: the 64 MiB block arena residency comes from
    u64 residency_size = 0, residency_used = 0;
    std::future<vk::DeviceMemory> spare_residency; ///< the next block, allocated in the background
    /// bbport: VRAM blocks no longer needed. Per 2 MiB of guest addresses, the second
    /// (BbStats::coarse_second) it was last bound: blocks in VRAM unused for BB_VRAM_IDLE_SECONDS
    /// (60, 0: never) go back to the game's memory, and residency chunks left without blocks
    /// go back to the driver (ProcessIdleBlocks). Blocks of unmapped memory leave VRAM too.
    static constexpr u64 USE_GROUP_BITS = 21;
    std::vector<u32> group_use;
    /// Groups with blocks moved in place for idleness, and those blocks: bound again, they get a
    /// VRAM copy again (QueuePromotions). Else the GPU kept reading them over PCIe (FPS 90 -> 55).
    std::vector<u8> group_demoted;
    IntervalList<> idle_demoted;
    struct ResidencyChunk {
        u64 size;
        u64 used;         ///< bytes handed out and not returned
        u32 empty_second; ///< when `used` dropped to 0
        u64 empty_tick;   ///< the submission then being recorded (it carries the rebinds)
        std::vector<u64> free_offsets; ///< slots of blocks that left (byte offsets)
        u32 evacuated_second = 0;      ///< when its blocks were last sent away (ProcessIdleBlocks)
    };
    std::unordered_map<VkDeviceMemory, ResidencyChunk> residency_chunks;
    u32 idle_scan_second = 0;
    u32 last_evacuation = 0; ///< ProcessIdleBlocks: when a chunk was last emptied
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
