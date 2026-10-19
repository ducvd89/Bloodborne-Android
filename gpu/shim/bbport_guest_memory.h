// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: guest direct memory in GPU-visible system memory (BB_GUEST_GPU_MEMORY=1).
//
// The runtime asks for direct memory in chunks (runtime_memory.c, CHUNK); each chunk is a
// Vulkan allocation of cached system memory, exported as a dma-buf that the runtime maps at the
// game's addresses and in its backing view. The GPU can then read the game's data in place: no
// CPU copy into staging buffers before an upload. A chunk that cannot be made stays in the
// runtime's memfd, as before.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace BbGuestMemory {
struct Chunk {
    std::uint64_t phys = 0; ///< first byte of direct memory it holds
    std::uint64_t size = 0;
    vk::Buffer buffer;      ///< the whole chunk, for transfers
    vk::DeviceMemory memory;
    std::uint32_t index = 0; ///< creation order (stable key)
    // bbport BB_UMA: what the demotion of a shared granule needs (BufferCache::UmaDemoter), any thread.
    /// The scheduler tick of its last binding in place (the largest one: BufferCache::UmaUse).
    mutable std::atomic<std::uint64_t> last_tick{0};
    /// BbStats::coarse_second of its last binding in place (0: none yet).
    mutable std::atomic<std::uint32_t> last_second{0};
    /// The guest address of the granule's first byte in the mapping of a binding (0: none yet).
    mutable std::atomic<std::uint64_t> granule_va{0};
    /// Being demoted: no new binding in place (UmaDirect refuses, "demoting"); stays set once retired.
    mutable std::atomic<bool> demoting{false};
    /// Its memory is about to be freed: the buffer cache makes no wrapper for it any more.
    mutable std::atomic<bool> retired{false};
};

/// Whether the runtime can use such chunks: dma-buf export and mmap of the dma-buf. Checked once
/// with a small chunk. A driver that maps at page offsets gets 256 MiB chunks (the game's mappings
/// start inside them); NVIDIA's dma-buf maps at offset 0 only (the grid made every mapping after
/// the first fail and the game panic at startup), so there each direct allocation is its own chunk:
/// the game maps its allocations whole, at offset 0.
bool Usable(const Vulkan::Instance& instance);
/// Whether the PC memory model (BB_GUEST_IN_PLACE, the launcher's new memory and translation
/// model) may run on this GPU: tested on AMD only. Elsewhere it stays off (the 0.3 model) unless
/// BB_PC_MODEL_ANY_GPU=1.
bool PcModelGpu(const Vulkan::Instance& instance);
/// Whether the chunks are imported host memory (NVIDIA with BB_PC_MODEL_ANY_GPU=1). Valid after
/// Usable(). The arena then keeps the first binding of every block (BufferCache FixedArena).
bool HostImported();
/// bbport BB_LAYER_MEMORY=1: the layer's memory module (gpu/layer/bblayer_gpu_memory.h) gives the
/// GPU the game's memory through the chunk buffers instead of a sparse arena. Chunks are then one
/// per direct memory allocation (a mapping lies whole in one chunk), their buffers take every use
/// (vertex, index, indirect, texel, device address) and are registered with the module.
bool LayerMemory();
/// The external memory handle type of the chunks (dma-buf or imported host memory): buffers bound
/// to them (the arena, chunk buffers) are created with it. Valid after Usable().
vk::ExternalMemoryHandleTypeFlagBits HandleType();
/// BB_UNIFIED=1 (v1, Android, opt-in; no longer wired to the Debug option, kept for reference and
/// off whenever BB_UMA=1 is set): the game's direct memory is dma-buf chunks of CPU-cached,
/// host-visible device memory (an integrated GPU: the phone's one RAM) the GPU uses in place through
/// the layer's memory module, no sparse buffers needed. Whether it was asked for.
bool UnifiedRequested();
/// Whether it may run on this GPU (requested, integrated, not Mali/PanVK, the layer's memory module
/// on, dma-buf export); logs the reason once when it may not. The probes of Usable() come on top.
bool UnifiedEligible(const Vulkan::Instance& instance);
/// Unified memory is active: valid after Usable() (false when a probe failed: copies).
bool Unified();
/// The chosen chunk memory type is cached but not coherent: CPU/GPU handoffs would need explicit
/// cache maintenance (not implemented: such a type is only used when the roundtrip probes pass).
bool UnifiedNeedsSync();
/// Why Usable() said no (empty before it ran or when it said yes), for the fallback message.
const char* FallbackReason();
/// Bytes of direct memory in chunks so far.
std::uint64_t ChunkBytes();
/// Hands the runtime direct memory chunks from now on (when enabled and supported).
void Install(const Vulkan::Instance& instance);
/// The chunk holding direct memory address `phys`, or null. With BB_UMA a lock-free lookup of the
/// granule table (one Chunk per shared granule).
const Chunk* Find(std::uint64_t phys);

// ---- BB_UMA=1: unified memory v2 (docs/ANDROID_UNIFIED_MEMORY_PLAN.md) ----
/// The game's buffers live in shared granules: the runtime (src/runtime_memory.c) keeps direct memory
/// in granules, CPU (memfd) or SHARED (a dma-buf made here, AllocGranule); the buffer cache asks it to
/// promote the granule a read-only buffer binding of an enabled class (bbport_uma.h) lies in, then
/// binds that granule's own buffer (never the sparse arena). Everything is behind BB_UMA=1; unset,
/// nothing here runs.

/// BB_UMA=1 in the environment.
bool UmaRequested();
/// Whether it may run on this GPU: requested, integrated, not Mali/PanVK, sparse buffers (the
/// arena must be sparse) and dma-buf export. Logs the reason once when it may not. The probes of
/// Usable() come on top.
bool UmaEligible(const Vulkan::Instance& instance);
/// UMA is active: valid after Usable() (false when a probe failed: copies).
bool Uma();
/// BB_UMA_GRANULE_KB (a power of two, 256..65536; default 2048) in bytes.
std::uint64_t UmaGranuleBytes();
/// Bytes of direct memory in shared granules so far.
std::uint64_t UmaSharedBytes();
/// Why UMA is not active (empty before Usable() or when it is).
const char* UmaFallbackReason();
/// Asks the runtime to share every granule overlapping direct memory [phys, phys + size): true when
/// all of them are shared afterwards. Allocates granules (AllocGranule) on the calling thread.
bool UmaPromote(std::uint64_t phys, std::uint64_t size);
/// The runtime's own counters (runtime_memory_uma_stats), same layout as RuntimeUmaStats there.
struct UmaRuntimeStats {
    std::uint64_t granule_bytes, shared_granules, shared_bytes, promotions, promotion_failures,
        refused_exec, refused_busy, freeze_faults, io_waits, promote_ns_total, promote_ns_max;
};
UmaRuntimeStats UmaRuntime();
/// Demotes every shared granule overlapping [phys, phys + size) back to the runtime's memfd
/// (runtime_memory_demote): true when none is shared afterwards. The granule's GPU memory stays
/// allocated (its Chunk stays in the table, `demoting` set): UmaRetire frees it once the GPU is done.
bool UmaDemote(std::uint64_t phys, std::uint64_t size);
/// The granule's GPU memory freed (its buffer and memory) and its slot in the table cleared; the Chunk
/// record itself is kept a minute more (lock-free readers may still hold it). The runtime must have
/// demoted it first; no GPU work may use its memory any more. Any thread.
void UmaRetire(const Chunk* chunk);
/// runtime_memory_granule_scan: per granule of direct memory, bits UmaGranuleShared / Allocated /
/// Mapped; returns the number of granules (0 without UMA).
enum : std::uint8_t { UmaGranuleShared = 1, UmaGranuleAllocated = 2, UmaGranuleMapped = 4 };
std::size_t UmaGranuleScan(std::uint8_t* out, std::size_t count);
/// The runtime's demotion counters (runtime_memory_uma_demote_stats), same layout.
struct UmaDemoteStats {
    std::uint64_t demotions, released, busy, failures, kept_bytes, demoted_bytes, demote_ns_total,
        demote_ns_max;
};
UmaDemoteStats UmaDemoteRuntime();
/// A host-visible, CPU-cached memory type among `bits` (cached and coherent first), never a
/// write-combined one; ~0u when none. Sets *coherent when given.
std::uint32_t PickUnifiedType(const vk::PhysicalDeviceMemoryProperties& props, std::uint32_t bits,
                              bool* coherent);
} // namespace BbGuestMemory
