// SPDX-License-Identifier: GPL-2.0-or-later
// bbport BB_TEX_HOME=1: textures homed in the GPU (docs/ANDROID_UNIFIED_MEMORY_PLAN.md, "Plan A").
// A sampled texture the CPU has left alone exists only as the GPU's image: the game's tiled copy of
// its whole pages is dropped (the runtime traps every guest alias, no access, and punches the memfd)
// once a re-tile of the image equals it byte for byte, and is re-tiled back into the game's memory
// on any access (a fault, or the runtime and the GPU side announcing a read or write of it). The
// texture cache does the homing (TextureCache::TexHome*); this header is what the other parts of the
// GPU library need: the switch, the access check and the counters.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

extern "C" {
int runtime_memory_home(std::uintptr_t address, std::uint64_t size, const void* expected,
                        std::uint32_t* ticket);
int runtime_memory_home_restore(std::uint32_t ticket, const void* data, std::uint64_t size);
std::uint64_t runtime_memory_home_alive(std::uint32_t ticket);
std::uint32_t runtime_memory_home_ticket(std::uintptr_t address);
int runtime_memory_home_tickets(std::uintptr_t address, std::uint64_t size, std::uint32_t* out,
                                int max);
int runtime_memory_home_touches(std::uintptr_t address, std::uint64_t size);
int runtime_memory_home_unstale(std::uintptr_t address);
std::uint64_t runtime_memory_home_granule(void);
std::uint64_t runtime_memory_homed_pages(void);
void runtime_memory_set_home_hook(int (*rehydrate)(std::uint32_t ticket));
int runtime_thread_is_guest(const void* ucontext);
}

namespace BbTexHome {

/// The runtime's trap reason of homed pages (BbLayer::WriteTraps::TexHome).
inline constexpr unsigned TrapReason = 64;
/// runtime_memory_home's answers (src/runtime.h, RUNTIME_HOME_*).
enum HomeResult : int { HomeOk, HomeArgs, HomeMapping, HomeShared, HomeHomed, HomeAliases, HomeFull, HomeMismatch };

/// BB_TEX_HOME=1 and a memory model it works with (copy mode or BB_UMA: write tracking by page
/// protection with BB_FAULT_PREPARE's map, no userfaultfd, no in-place or v1 chunks); decided at the
/// texture collector's first pass (after the buffer cache's probes), false before.
bool Enabled();
/// BB_TEX_HOME_IDLE_S (10): seconds since the last CPU write (upload) before a texture may be homed.
std::uint64_t IdleSeconds();

/// Something may be homed: set before the first home (so the fault handler looks from then on).
inline std::atomic<bool> active{false};
inline bool Active() {
    return active.load(std::memory_order_relaxed);
}

/// The texture cache's rebuild of every home behind guest [address, address + size) (any thread;
/// how it is done depends on the thread, TextureCache::TexHomeRehydrateFrom).
using RangeHandler = void (*)(std::uintptr_t address, std::uint64_t size);
inline std::atomic<RangeHandler> range_handler{nullptr};

/// The GPU side is about to read or write guest [address, address + size) on the CPU (an upload's
/// source, a copy, a download's destination) or let the GPU write it (a written binding): homed
/// pages there are rebuilt first. One relaxed load while nothing is homed.
inline void BeforeAccess(std::uintptr_t address, std::uint64_t size) {
    if (!Active() || size == 0 || !runtime_memory_home_touches(address, size)) {
        return;
    }
    if (const auto handler = range_handler.load(std::memory_order_acquire)) {
        handler(address, size);
    }
}

/// A fault on a page with TrapReason (PageManager's handler): rebuilt, true (the access is
/// retried); false: the fault is not ours to resolve (a speculative reader's, or a thread that
/// cannot wait for the rebuild).
using FaultHandler = bool (*)(void* context, std::uintptr_t address);
inline std::atomic<FaultHandler> fault_handler{nullptr};

/// Why a texture is not homed (candidates the collector looked at), and why a home failed.
enum Refusal : std::size_t {
    Small,       ///< under 256 KiB, or no whole host page inside it
    Usage,       ///< not only sampled (render/depth target, storage, video out, proxy)
    GpuWritten,  ///< GPU-modified or GPU-dirty, written by the GPU once
    Msaa,        ///< multisampled
    DepthStencil,///< depth or stencil
    Format,      ///< the host format is not a bitwise copy of the guest's
    Dirty,       ///< not uploaded and clean, or not tracked whole
    Recent,      ///< the CPU wrote it (uploaded) less than IdleSeconds() ago
    Mapping,     ///< not all in one direct memory mapping
    Shared,      ///< in a shared UMA granule (or a v1 chunk)
    Overlap,     ///< another image overlaps it
    GpuData,     ///< the buffer cache has GPU data on its pages
    Mismatch,    ///< its re-tile differs from the game's bytes (never tried again)
    Runtime,     ///< refused by the runtime otherwise (aliases, records, already homed)
    RefusalKinds
};
inline constexpr std::array<const char*, RefusalKinds> RefusalNames{
    "small", "usage", "gpu-written", "msaa", "depth/stencil", "format", "dirty", "recent",
    "mapping", "shared", "overlap", "gpu-data", "bytes differ", "runtime"};

/// How a home came back.
enum Rebuild : std::size_t { ByFault, ByAccess, ByRuntime, ByCollector, ByFree, ByGpuWrite, ByOverlap, RebuildKinds };
inline constexpr std::array<const char*, RebuildKinds> RebuildNames{
    "fault", "access", "runtime", "collector", "free/unmap", "gpu-write", "new image"};

struct Counters {
    std::atomic<std::uint64_t> homes{0}, homed_images{0}, homed_bytes{0};
    std::atomic<std::uint64_t> verifies{0};
    std::array<std::atomic<std::uint64_t>, RefusalKinds> refused{};
    std::array<std::atomic<std::uint64_t>, RebuildKinds> rebuilt{};
    std::atomic<std::uint64_t> rebuild_ns_total{0}, rebuild_ns_max{0};
    std::atomic<std::uint64_t> forwarded{0};   ///< rebuilds a guest/host thread asked the GPU thread for
    std::atomic<std::uint64_t> timeouts{0};    ///< ... that it could not wait for
    std::atomic<std::uint64_t> lost{0};        ///< homes forgotten without their bytes (see the docs)
};
inline Counters counters;

} // namespace BbTexHome
