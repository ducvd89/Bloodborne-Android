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
    std::atomic<std::uint64_t> watch_faults{0};      ///< write faults on that trap
    std::atomic<std::uint64_t> watch_notes{0};       ///< announced writes (libc, file reads) there
    std::atomic<std::uint64_t> dynamic_blocks{0};    ///< blocks marked dynamic (any cause)
    std::atomic<std::uint64_t> dynamic_verify{0};    ///< regions marked by a verify mismatch
    std::atomic<std::uint64_t> dynamic_regions{0};   ///< 2 MiB regions marked whole (rings)
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
    /// Refusals per class (UmaDirect, classes allowed): mapping, cross, dynamic, gpu-data, image,
    /// written, backoff/promotion.
    static constexpr std::size_t RefusalKinds = 7;
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

// ---- dynamic blocks: memory the CPU rewrites while it is bound in place ----
/// A binding bound in place (BufferCache::UmaDirect) arms a one-shot write trap
/// (BbLayer::WriteTraps::UmaWatch) on its 64 KiB guest blocks; the first CPU write there (a fault,
/// or a write the runtime announces: libc copies, file reads) makes the block dynamic and lifts the
/// trap. Dynamic blocks are never bound in place again (copies, as in copy mode) until the
/// mapping there changes. Keyed by guest VA, below 2^40; lock-free bitmaps (any thread, the fault
/// handler included). The traps themselves are set and lifted by the page manager, which owns the
/// pages' protection (VideoCore::PageManager::UmaArm / UmaNoteWrite).
namespace Dynamic {
constexpr unsigned BlockShift = 16;
constexpr std::uint64_t BlockBytes = std::uint64_t{1} << BlockShift;
/// Whether any block of [address, address + size) is dynamic.
bool Any(std::uint64_t address, std::uint64_t size);
/// Whether any block of the range is armed (bound in place, not dynamic).
bool AnyArmed(std::uint64_t address, std::uint64_t size);
/// Whether any block of the range is armed or dynamic (its pages may carry the trap).
bool AnyWatched(std::uint64_t address, std::uint64_t size);
bool Armed(std::uint64_t block);
bool IsDynamic(std::uint64_t block);
/// Marks the block armed; true when it was neither armed nor dynamic (the caller sets the trap).
bool TryArm(std::uint64_t block);
/// What Mark did: `fresh` when the block was not dynamic before; [first, end) the blocks of the
/// 2 MiB regions marked with it (empty: none), when another dynamic block lies within 2 MiB.
struct Marked {
    bool fresh = false;
    std::uint64_t first = 0, end = 0;
};
/// Marks the block dynamic (and no longer armed). A second dynamic block within 2 MiB (a ring of
/// per-frame slots) makes the 2 MiB-aligned regions of both dynamic (`dynamic_regions`).
Marked Mark(std::uint64_t block);
/// The 2 MiB regions of the range dynamic (verify mismatches); their traps go at the next write.
void MarkRange(std::uint64_t address, std::uint64_t size);
/// The mapping changed (map or unmap): the blocks forget both marks (the runtime forgot the traps).
void Forget(std::uint64_t address, std::uint64_t size);
} // namespace Dynamic

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
