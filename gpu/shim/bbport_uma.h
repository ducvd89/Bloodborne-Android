// SPDX-License-Identifier: GPL-2.0-or-later
// bbport BB_UMA=1: unified memory v2 (docs/ANDROID_UNIFIED_MEMORY_PLAN.md). The game's buffers live in
// shared (dma-buf) granules the GPU binds in place; textures and every copy stay CPU snapshots.
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
    std::array<std::atomic<std::uint64_t>, ClassCount> refused_class{}; ///< class off, or None
    std::atomic<std::uint64_t> image_writes{0};      ///< images re-tiled into shared blocks
    std::atomic<std::uint64_t> verify_checked{0};
    std::atomic<std::uint64_t> verify_mismatches{0};
    std::atomic<std::uint64_t> verify_dropped{0};    ///< ring full
};
inline Counters counters;

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
