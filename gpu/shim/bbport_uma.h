// SPDX-License-Identifier: GPL-2.0-or-later
// bbport BB_UMA=1: unified memory v2 (docs/ANDROID_UNIFIED_MEMORY_PLAN.md). The game's buffers live in
// shared (dma-buf) granules the GPU reads in place through each granule's own buffer (never
// sparse-bound); textures and every copy stay CPU snapshots.
// This header holds what the buffer cache and the rasterizer share: the binding classes that may move
// memory into shared granules, the counters, the verify mode and the start-up probes. The granule
// allocator and the mode switch itself are in bbport_guest_memory.h (BbGuestMemory::Uma()).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace Vulkan {
class Instance;
}

namespace BbUma {

/// What a buffer binding is for, set by the rasterizer around BufferCache::ObtainBuffer. Only the
/// classes of BB_UMA_CLASSES may move a copy granule of the game's memory into shared memory.
/// Everything that does not say what it is (texture sources, DMA, labels, downloads...) is None.
/// The recompiler binds every guest buffer as a storage buffer descriptor, so the split is by use:
/// Texel = formatted (texel) buffers, Storage = buffers the shader writes, Constant = the other,
/// read-only guest buffers.
enum class Class : std::uint8_t { None, Index, Vertex, Constant, Storage, Texel, Indirect };
constexpr std::size_t ClassCount = 7;

/// "none", "index", "vertex", "constant", "storage", "texel", "indirect".
const char* ClassName(Class cls);
/// BB_UMA_CLASSES (comma list of index,vertex,constant,storage,texel,indirect, or all; default
/// "index,vertex"), parsed once. None is never allowed.
bool ClassAllowed(Class cls);
/// The allowed classes as text for the logs, e.g. "index,vertex".
const std::string& ClassList();

inline thread_local Class current_class = Class::None;
inline Class Current() {
    return current_class;
}

/// The class of the bindings the calling thread makes while it lives.
class ClassScope {
public:
    explicit ClassScope(Class cls) : previous{current_class} {
        current_class = cls;
    }
    ~ClassScope() {
        current_class = previous;
    }
    ClassScope(const ClassScope&) = delete;
    ClassScope& operator=(const ClassScope&) = delete;

private:
    Class previous;
};

/// GPU-side counters (the runtime's own are in BbGuestMemory::UmaRuntimeStats).
struct Counters {
    std::atomic<std::uint64_t> promote_requests{0};  ///< runtime_memory_promote calls
    std::atomic<std::uint64_t> promote_failures{0};  ///< not every granule of the range was shared
    std::atomic<std::uint64_t> refused_image{0};     ///< range over a GPU-modified image
    std::atomic<std::uint64_t> refused_size{0};      ///< binding over BB_UMA_MAX_PROMOTE_MB
    std::atomic<std::uint64_t> refused_backoff{0};   ///< granule failed a moment ago, not asked again
    /// BB_TEX_HOME_FIRST: granule mostly under sampled textures, left to Plan A (not promoted).
    std::atomic<std::uint64_t> refused_texture_first{0};
    std::array<std::atomic<std::uint64_t>, ClassCount> refused_class{}; ///< class off, or None
    std::atomic<std::uint64_t> refused_written{0};   ///< written binding (not shared yet): a copy
    std::atomic<std::uint64_t> refused_cross{0};     ///< binding crosses a granule: a copy
    std::atomic<std::uint64_t> refused_mapping{0};   ///< not in one direct memory mapping: a copy
    std::atomic<std::uint64_t> refused_gpu_data{0};  ///< GPU data in the arena's copy: the copy
    std::atomic<std::uint64_t> refused_dynamic{0};   ///< touches a block the CPU rewrites: a copy
    std::atomic<std::uint64_t> watch_armed{0};       ///< 64 KiB blocks given the UmaWatch trap
    std::atomic<std::uint64_t> armed_regions{0};     ///< 2 MiB regions armed (whole, at once)
    std::atomic<std::uint64_t> watch_faults{0};      ///< write faults on that trap
    std::atomic<std::uint64_t> watch_notes{0};       ///< announced writes (libc, file reads) there
    std::atomic<std::uint64_t> dynamic_blocks{0};    ///< blocks marked dynamic (any cause)
    std::atomic<std::uint64_t> dynamic_verify{0};    ///< regions marked by a verify mismatch
    std::atomic<std::uint64_t> dynamic_regions{0};   ///< 2 MiB regions marked dynamic (whole)
    /// Not armed because the process has BB_UMA_MAX_MAPS memory mappings: a copy (Maps).
    std::atomic<std::uint64_t> refused_maps_cap{0};
    std::atomic<std::uint64_t> memo_hits{0};         ///< UmaDirect verdicts reused (no lookups)
    std::atomic<std::uint64_t> memo_misses{0};       ///< ... evaluated
    std::atomic<std::uint64_t> direct_bindings{0};   ///< bound through a shared granule's buffer
    std::atomic<std::uint64_t> direct_bytes{0};      ///< their bytes
    std::atomic<std::uint64_t> image_writes{0};      ///< images re-tiled over shared granules
    /// Arena residency (VRAM copies) by the class current when each block became resident, [1]:
    /// its guest pages were in RAM then (the CPU had touched them, or a shared granule: a copy
    /// duplicates them), [0]: not (GPU-only data: sharing it would save nothing).
    std::array<std::array<std::atomic<std::uint64_t>, 2>, ClassCount> arena_bytes{};
    std::atomic<std::uint64_t> arena_mincore_failures{0};
    /// Demotion SHARED -> CPU (BufferCache::UmaDemoter): by reason, granules whose demotion was
    /// given up (the runtime stayed busy), retries, granules shared again after a demotion.
    std::atomic<std::uint64_t> demote_released{0};     ///< no direct memory allocated there any more
    std::atomic<std::uint64_t> demote_idle_dynamic{0}; ///< idle, mostly dynamic blocks
    std::atomic<std::uint64_t> demote_idle{0};         ///< idle
    std::atomic<std::uint64_t> demote_retries{0};      ///< the runtime was busy: tried again later
    std::atomic<std::uint64_t> demote_given_up{0};     ///< ... too often: shared again
    std::atomic<std::uint64_t> repromotions{0};        ///< promoted again after a demotion
    std::atomic<std::uint64_t> refused_demoting{0};    ///< binding over a granule being demoted
    /// Refusals per class (UmaDirect, classes allowed): mapping, cross, dynamic, gpu-data, image,
    /// written, backoff/promotion, maps cap, demoting.
    static constexpr std::size_t RefusalKinds = 9;
    std::array<std::array<std::atomic<std::uint64_t>, RefusalKinds>, ClassCount> refused_by{};
    std::atomic<std::uint64_t> verify_checked{0};
    std::atomic<std::uint64_t> verify_mismatches{0};
    std::atomic<std::uint64_t> verify_dropped{0};    ///< ring full
};
inline Counters counters;

/// Bumped whenever a verdict of BufferCache::UmaDirect may change without a change of the runtime's
/// mapping table: a block became dynamic or forgot its marks, the GPU wrote (copy path) over an
/// armed block, an image became GPU-written. UmaDirect's per-thread memo compares it (any thread,
/// the fault handler included).
inline std::atomic<std::uint64_t> verdict_epoch{0};
inline void NoteVerdictChange() {
    verdict_epoch.fetch_add(1, std::memory_order_acq_rel);
}

/// BB_UMA_VERIFY=N: every Nth in-place read binding is hashed when recorded and again when the
/// submission it was recorded in has completed. 0 (default): off.
std::uint32_t VerifyEvery();
inline bool VerifyEnabled() {
    static const bool on = VerifyEvery() != 0;
    return on;
}
/// Notes the binding (sampled: one in N), hashing up to 1 MiB of it now. Any thread.
void VerifyRecord(Class cls, std::uint64_t address, std::uint64_t size, std::uint64_t tick);
/// Hashes again the bindings whose tick `done` says has completed, oldest first (the buffer
/// cache's per-packet maintenance); a changed hash is logged and counted.
void VerifyProcess(bool (*done)(void* context, std::uint64_t tick), void* context);

// ---- DMA fills and copies into shared granules ----
/// BB_UMA_DMA_GPU=1 (env, read once; default off): a DMA fill or copy (PM4 DMA_DATA, COPY_DATA) whose
/// destination touches a shared granule goes to the GPU in stream order (the arena's copy, marked
/// GPU-modified; the game's memory keeps the old bytes). Default: done as copy mode does it, by the
/// CPU into the game's memory (BufferCache::DmaDstInPlace).
bool DmaGpu();
/// Counts a DMA fill (`fill`) or copy into a shared granule: done by the CPU (`cpu`) or recorded for the
/// GPU. Any thread; the first 32 distinct destinations are remembered (SharedDmaNewList).
void NoteSharedDma(std::uint64_t address, std::uint64_t size, bool fill, bool cpu);
struct SharedDmaTotals {
    std::uint64_t count = 0, bytes = 0, cpu = 0, gpu = 0;
};
SharedDmaTotals SharedDma();
/// When destinations were remembered since the last call that returned true: `text` is the list
/// ("fill 0x..+0x.. cpu, copy ..."), capped in length. For one reporting thread.
bool SharedDmaNewList(std::string& text);

// ---- dynamic blocks: memory the CPU rewrites while it is bound in place ----
/// A binding bound in place (BufferCache::UmaDirect) arms a one-shot write trap
/// (BbLayer::WriteTraps::UmaWatch) on the 2 MiB-aligned regions of guest VA it touches, whole (the
/// region clipped to its direct mapping and granule); the first CPU write in an armed region (a
/// fault, or a write the runtime announces: libc copies, file reads) makes the whole region dynamic
/// and lifts the trap on all of it at once. Per region, not per 64 KiB block: every change of a
/// page's protection splits the kernel's mappings (vm.max_map_count), and per-block traps had the
/// process at 30000+ mappings within minutes. Dynamic memory is never bound in place again (copies,
/// as in copy mode) until the mapping there changes. The marks are kept per 64 KiB block (a region
/// clipped to a mapping is armed only in part, a remap forgets part of one), keyed by guest VA below
/// 2^40; lock-free bitmaps (any thread, the fault handler included). The traps themselves are set
/// and lifted by the page manager, which owns the pages' protection (VideoCore::PageManager::UmaArm
/// / UmaNoteWrite).
namespace Dynamic {
constexpr unsigned BlockShift = 16;
constexpr std::uint64_t BlockBytes = std::uint64_t{1} << BlockShift;
constexpr unsigned RegionShift = 21; // 2 MiB of guest VA
constexpr std::uint64_t RegionBytes = std::uint64_t{1} << RegionShift;
constexpr std::uint64_t RegionBlocks = std::uint64_t{1} << (RegionShift - BlockShift); // 32
/// Whether any block of [address, address + size) is dynamic.
bool Any(std::uint64_t address, std::uint64_t size);
/// Whether any block of the range is armed (bound in place, not dynamic).
bool AnyArmed(std::uint64_t address, std::uint64_t size);
/// Whether every block of the range is armed.
bool AllArmed(std::uint64_t address, std::uint64_t size);
/// Whether any block of the range is armed or dynamic (its pages may carry the trap).
bool AnyWatched(std::uint64_t address, std::uint64_t size);
bool Armed(std::uint64_t block);
bool IsDynamic(std::uint64_t block);
/// The region's blocks armed / dynamic, bit i = block `region * RegionBlocks + i`.
std::uint32_t RegionArmed(std::uint64_t region);
std::uint32_t RegionDynamic(std::uint64_t region);
/// Marks the blocks of `mask` (in `region`) armed, those not dynamic; returns the ones that were
/// neither armed nor dynamic (the caller sets their trap).
std::uint32_t ArmBlocks(std::uint64_t region, std::uint32_t mask);
/// What MarkRegion did: `fresh` when a block of the region was not dynamic before, `had_armed` when
/// a block was armed (its trap is still set: lift it on the whole region).
struct Marked {
    bool fresh = false;
    bool had_armed = false;
};
/// Every block of the region dynamic and no longer armed (a CPU write there).
Marked MarkRegion(std::uint64_t region);
/// The 2 MiB regions of the range dynamic (verify mismatches). Armed marks stay: the next write
/// there lifts the trap on the whole region (UmaNoteWrite).
void MarkRange(std::uint64_t address, std::uint64_t size);
/// The mapping changed (map or unmap): the blocks forget both marks (the runtime forgot the traps).
void Forget(std::uint64_t address, std::uint64_t size);
} // namespace Dynamic

// ---- the kernel's mapping count (vm.max_map_count) ----
/// Every page protection change splits the process's mappings; at the kernel's limit every mmap
/// fails (Turnip's command buffers: vkQueueSubmit -> VK_ERROR_MEMORY_MAP_FAILED, and the game's own).
/// A low-priority thread counts /proc/self/maps every 2 s; between counts each region armed adds an
/// estimate of 2. Above BB_UMA_MAX_MAPS no new region is armed: bindings outside armed regions take
/// copies (refused "maps cap").
namespace Maps {
/// BB_UMA_MAX_MAPS (env, read once): default 40000; 0: no cap.
std::uint64_t Cap();
/// vm.max_map_count, or 65530 (the kernel's default) when it cannot be read (Android apps cannot).
std::uint64_t KernelLimit();
/// The last count plus the estimate since (0 before the first count).
std::uint64_t Estimate();
/// Whether new regions may be armed (starts the counting thread; logs once when the cap engages).
bool ArmingAllowed();
/// A region was armed (one or two new mappings until the next count).
void NoteArmed();
/// Whether the cap has refused arming at least once.
bool Engaged();
} // namespace Maps

// ---- start-up probes (bbport_uma.cpp); each returns pass/fail and a short detail for the log ----

/// Strict CPU/GPU coherence on a 32 MiB exported buffer of `memory_type`, mapped with mmap of its
/// dma-buf fd as the runtime maps granules: CPU writes seen by a GPU copy, single cache lines,
/// GPU writes over lines the CPU has read, writers on other cores. On the game's device.
bool ProbeCoherence(const Vulkan::Instance& instance, std::uint32_t memory_type, std::string& detail);
/// A sparse buffer bound to exported dma-buf memory at an offset, used by the GPU and the CPU
/// through the mapping. On a throwaway VkDevice (a driver refusal must not reach the game's).
bool ProbeSparseDmaBuf(const Vulkan::Instance& instance, std::string& detail);
/// Android: what KGSL does with an imported dma-heap buffer (diagnostic, log only).
void KgslDiagnostic();

} // namespace BbUma
