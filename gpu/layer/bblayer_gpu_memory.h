// SPDX-License-Identifier: GPL-2.0-or-later
// bbport layer: the PS4 GPU's view of memory on a PC GPU, without sparse resources.
//
// The PS4 GPU addresses the one shared memory through its own virtual memory: every V#, T# and
// pointer a shader gets is a guest virtual address. Here the GPU reaches that memory through
// ordinary Vulkan buffers ("sources"): the game's own memory (chunks of direct memory the GPU can
// use where they are: dma-buf or imported host memory) and mirrors (VRAM copies of guest ranges,
// kept current by the caller). A range that lies whole in one source is bound like any buffer;
// the module answers where. Mirrors cover runs of blocks moved to VRAM: one buffer per run (runs
// that meet are merged into a new mirror), so a range all in VRAM is one buffer range too.
//
// Independent of the shadPS4 code: plain Vulkan handles, guest addresses, physical addresses of
// direct memory. See docs/MEMORY_MODULE_PLAN.ru.md.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace BbLayer {

/// A Vulkan buffer over memory the GPU reads guest data from.
struct MemorySource {
    enum class Kind : std::uint8_t {
        Guest,  ///< a chunk of the game's direct memory, used in place (CPU-coherent)
        Mirror, ///< a VRAM copy of guest addresses [base, base + size)
    };
    Kind kind = Kind::Guest;
    std::uint64_t base = 0; ///< Guest: first physical address it holds; Mirror: first guest address
    std::uint64_t size = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress device_address = 0; ///< 0 when the memory has no device address
    void* owner = nullptr;              ///< the caller's object for this source (its buffer wrapper)
};

/// Part of a guest range in one source: [va, va + size) is at `offset` in `source`.
struct Span {
    const MemorySource* source = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t va = 0;
    std::uint64_t size = 0;
};

/// Where a guest range is for the GPU.
struct Resolution {
    enum class Kind : std::uint8_t {
        None,    ///< not GPU-visible game memory (no source holds its start)
        InPlace, ///< whole in one guest chunk, nothing of it in VRAM
        Mirror,  ///< whole in one mirror, every block of it valid there
        Mixed,   ///< in place and in VRAM, across mirrors or chunks
    };
    Kind kind = Kind::None;
    Span span; ///< InPlace, Mirror
};

/// The guest's translation of a virtual address of direct memory: its physical address and the
/// end of the mapping (one physically contiguous run) it lies in. False: not direct memory.
using GuestTranslate = bool (*)(std::uint64_t va, std::uint64_t* phys, std::uint64_t* end);

class GpuMemory {
public:
    static GpuMemory& Get();

    void SetTranslate(GuestTranslate translate);
    /// Blocks (the unit moved to VRAM) of 1 << shift bytes.
    void SetBlockShift(std::uint32_t shift);
    std::uint32_t BlockShift() const {
        return block_shift;
    }

    /// A chunk of direct memory [phys, phys + size) the GPU uses in place through `buffer`.
    /// Chunks never overlap and stay for the whole run.
    void AddGuestSource(std::uint64_t phys, std::uint64_t size, VkBuffer buffer,
                        VkDeviceAddress device_address, void* owner);

    /// The guest source holding physical address `phys`, or null.
    const MemorySource* GuestSource(std::uint64_t phys) const;

    /// [va, va + size) whole in one guest source and physically contiguous: bound in place as
    /// one buffer range. Empty when part of it is not direct memory in a source, or it spans
    /// two sources or two mappings that are not contiguous in physical memory.
    std::optional<Span> ResolveInPlace(std::uint64_t va, std::uint64_t size) const;

    /// The longest start of [va, va + size) that ResolveInPlace would take (its span), or empty
    /// when even va is not in a source.
    std::optional<Span> ResolvePrefix(std::uint64_t va, std::uint64_t size) const;

    /// Where [va, va + size) is: in place, in one mirror, or mixed (see Resolution).
    Resolution Resolve(std::uint64_t va, std::uint64_t size) const;

    // --- Mirrors (VRAM copies). The caller allocates and frees their buffers; the module keeps
    // which guest range each covers and which blocks hold current data. ---

    /// A mirror of guest addresses [start, start + size). Mirrors never overlap.
    void AddMirror(std::uint64_t start, std::uint64_t size, VkBuffer buffer,
                   VkDeviceAddress device_address, void* owner);
    /// Forgets the mirror starting at `start` (its blocks must be invalid or moved first).
    void RemoveMirror(std::uint64_t start);
    /// The mirror holding guest address va, or null (a copy: valid until the next change).
    std::optional<MemorySource> MirrorAt(std::uint64_t va) const;
    /// Mirrors overlapping [start, end), in address order.
    std::vector<MemorySource> MirrorsIn(std::uint64_t start, std::uint64_t end) const;

    /// Blocks [first, end) hold current data in their mirror (it must cover them) / no longer.
    void MarkValid(std::uint64_t first_block, std::uint64_t end_block);
    void MarkInvalid(std::uint64_t first_block, std::uint64_t end_block);
    bool AnyValid(std::uint64_t first_block, std::uint64_t end_block) const;
    bool AllValid(std::uint64_t first_block, std::uint64_t end_block) const;
    /// Valid runs within [first, end): f(run_first, run_end).
    template <typename F>
    void ForEachValid(std::uint64_t first_block, std::uint64_t end_block, F&& f) const {
        std::shared_lock lk{mirror_mutex};
        auto it = valid.upper_bound(first_block);
        if (it != valid.begin()) {
            --it;
        }
        for (; it != valid.end() && it->first < end_block; ++it) {
            const std::uint64_t a = std::max(it->first, first_block);
            const std::uint64_t b = std::min(it->second, end_block);
            if (a < b) {
                f(a, b);
            }
        }
    }
    /// Bytes of valid blocks (statistics).
    std::uint64_t ValidBytes() const;
    /// Changes with every MarkValid and MarkInvalid (lock-free: callers memoize answers by it).
    std::uint64_t ValidVersion() const {
        return valid_version.load(std::memory_order_acquire);
    }

    /// Calls f(Span) for each piece of [va, va + size): pieces in place carry their source,
    /// the rest a null source (not GPU-visible game memory). In address order.
    template <typename F>
    void ForEachInPlace(std::uint64_t va, std::uint64_t size, F&& f) const {
        const std::uint64_t end = va + size;
        for (std::uint64_t at = va; at < end;) {
            std::uint64_t phys = 0, map_end = 0;
            if (!translate || !translate(at, &phys, &map_end) || map_end <= at) {
                // Not direct memory: up to the next mapping we know of is unknown; one page.
                const std::uint64_t next = std::min(end, (at | 0xfff) + 1);
                f(Span{nullptr, 0, at, next - at});
                at = next;
                continue;
            }
            const MemorySource* source = GuestSource(phys);
            std::uint64_t piece = std::min(end, map_end) - at;
            if (source) {
                piece = std::min(piece, source->base + source->size - phys);
            }
            f(Span{source, source ? phys - source->base : 0, at, piece});
            at += piece;
        }
    }

private:
    bool AnyValidLocked(std::uint64_t first_block, std::uint64_t end_block) const;
    bool AllValidLocked(std::uint64_t first_block, std::uint64_t end_block) const;

    GuestTranslate translate = nullptr;
    std::uint32_t block_shift = 16;
    std::mutex write_mutex;
    /// Sorted by base; replaced whole on each addition (readers take no lock; old lists stay
    /// allocated, a few hundred additions at most).
    std::atomic<const std::vector<MemorySource*>*> guest_sources{nullptr};

    mutable std::shared_mutex mirror_mutex;
    std::atomic<std::uint64_t> valid_version{0};
    std::map<std::uint64_t, MemorySource> mirrors;   ///< by start (guest address)
    std::map<std::uint64_t, std::uint64_t> valid;    ///< valid block runs: first -> end
    std::uint64_t valid_blocks = 0;
};

} // namespace BbLayer
