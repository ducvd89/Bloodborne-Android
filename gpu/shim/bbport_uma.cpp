// SPDX-License-Identifier: GPL-2.0-or-later
// bbport BB_UMA=1: binding classes, verify mode and the start-up probes of unified memory v2
// (docs/ANDROID_UNIFIED_MEMORY_PLAN.md).
#include "bbport_uma.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sched.h>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <xxhash.h>
#if defined(__ANDROID__)
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#endif
#include "bbport_guest_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"

extern "C" int runtime_memory_direct_phys(uintptr_t address, uint64_t* phys, uintptr_t* end);

namespace BbUma {
namespace {
using u64 = std::uint64_t;
using u32 = std::uint32_t;
constexpr u64 MiB = 1ull << 20;
} // namespace

// ---------------------------------------------------------------------------------------------
// Binding classes
// ---------------------------------------------------------------------------------------------

const char* ClassName(Class cls) {
    static constexpr std::array<const char*, ClassCount> names = {
        "none", "index", "vertex", "constant", "storage", "texel", "indirect"};
    return names[static_cast<std::size_t>(cls)];
}

namespace {
struct ClassConfig {
    unsigned mask = 0; ///< bit per Class
    std::string list;
};

const ClassConfig& Config() {
    static const ClassConfig config = [] {
        ClassConfig result;
        const char* env = std::getenv("BB_UMA_CLASSES");
        const std::string text = env && *env ? env : "index,vertex";
        std::size_t pos = 0;
        while (pos <= text.size()) {
            std::size_t end = text.find(',', pos);
            if (end == std::string::npos) {
                end = text.size();
            }
            std::string token = text.substr(pos, end - pos);
            pos = end + 1;
            // Trim spaces, lower case.
            const std::size_t first = token.find_first_not_of(" \t");
            if (first == std::string::npos) {
                continue;
            }
            token = token.substr(first, token.find_last_not_of(" \t") - first + 1);
            std::transform(token.begin(), token.end(), token.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (token.empty() || token == "none") {
                continue;
            }
            if (token == "all") {
                for (std::size_t i = 1; i < ClassCount; ++i) {
                    result.mask |= 1u << i;
                }
                continue;
            }
            bool known = false;
            for (std::size_t i = 1; i < ClassCount; ++i) {
                if (token == ClassName(static_cast<Class>(i))) {
                    result.mask |= 1u << i;
                    known = true;
                    break;
                }
            }
            if (!known) {
                std::printf("UMA: unknown class '%s' in BB_UMA_CLASSES ignored (index, vertex, "
                            "constant, storage, texel, indirect, all)\n",
                            token.c_str());
            }
        }
        for (std::size_t i = 1; i < ClassCount; ++i) {
            if (result.mask & (1u << i)) {
                result.list += result.list.empty() ? "" : ",";
                result.list += ClassName(static_cast<Class>(i));
            }
        }
        if (result.list.empty()) {
            result.list = "none";
        }
        return result;
    }();
    return config;
}
} // namespace

bool ClassAllowed(Class cls) {
    return cls != Class::None && (Config().mask & (1u << static_cast<unsigned>(cls))) != 0;
}

const std::string& ClassList() {
    return Config().list;
}

// ---------------------------------------------------------------------------------------------
// Verify mode: data a binding read in place must not change while the GPU uses it
// ---------------------------------------------------------------------------------------------

u32 VerifyEvery() {
    static const u32 every = [] {
        // Only with BB_UMA: in copy mode there is nothing in place to verify.
        const char* env = std::getenv("BB_UMA_VERIFY");
        if (!BbGuestMemory::UmaRequested() || !env || !*env) {
            return 0u;
        }
        return u32(std::min<unsigned long>(std::strtoul(env, nullptr, 10), 1ul << 30));
    }();
    return every;
}

namespace {
struct VerifyEntry {
    Class cls = Class::None;
    u64 address = 0, size = 0, hash = 0, tick = 0;
};
constexpr std::size_t VerifyRing = 4096;
std::mutex verify_mutex;
std::array<VerifyEntry, VerifyRing> verify_ring;
std::size_t verify_tail = 0, verify_count = 0;
std::atomic<std::size_t> verify_pending{0}; ///< verify_count, readable without the lock

constexpr u64 VerifyCap = 1 * MiB;

/// Hash of the guest bytes, or false where the memory is no longer mapped (checked, not proof
/// against an unmap in the next instruction: this is a debugging aid).
bool HashGuest(u64 address, u64 size, u64& hash) {
    const u64 bytes = std::min(size, VerifyCap);
    u64 phys = 0;
    uintptr_t mapping_end = 0;
    if (!runtime_memory_direct_phys(address, &phys, &mapping_end) || address + bytes > mapping_end) {
        return false;
    }
    hash = XXH3_64bits(reinterpret_cast<const void*>(address), bytes);
    return true;
}
} // namespace

void VerifyRecord(Class cls, u64 address, u64 size, u64 tick) {
    const u32 every = VerifyEvery();
    if (every == 0 || size == 0) {
        return;
    }
    static std::atomic<u64> seen{0};
    if (seen.fetch_add(1, std::memory_order_relaxed) % every != 0) {
        return;
    }
    u64 hash = 0;
    if (!HashGuest(address, size, hash)) {
        return;
    }
    std::scoped_lock lk{verify_mutex};
    if (verify_count == VerifyRing) {
        counters.verify_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    verify_ring[(verify_tail + verify_count) % VerifyRing] = {cls, address, size, hash, tick};
    verify_pending.store(++verify_count, std::memory_order_release);
}

void VerifyProcess(bool (*done)(void* context, u64 tick), void* context) {
    if (verify_pending.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::scoped_lock lk{verify_mutex};
    while (verify_count != 0) {
        const VerifyEntry entry = verify_ring[verify_tail];
        if (!done(context, entry.tick)) {
            break; // ticks complete in order: the rest are newer
        }
        verify_tail = (verify_tail + 1) % VerifyRing;
        verify_pending.store(--verify_count, std::memory_order_release);
        u64 hash = 0;
        if (!HashGuest(entry.address, entry.size, hash)) {
            continue;
        }
        counters.verify_checked.fetch_add(1, std::memory_order_relaxed);
        if (hash == entry.hash) {
            continue;
        }
        const u64 n = counters.verify_mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 50 || n % 1000 == 0) {
            std::printf("UMA verify: %s binding %#llx+%llu changed while the GPU used it (tick %llu)\n",
                        ClassName(entry.cls), (unsigned long long)entry.address,
                        (unsigned long long)entry.size, (unsigned long long)entry.tick);
        }
        // Belt and braces next to the write traps: never bound in place again (copies).
        Dynamic::MarkRange(entry.address, entry.size);
    }
}

// ---------------------------------------------------------------------------------------------
// Dynamic blocks
// ---------------------------------------------------------------------------------------------

namespace Dynamic {
namespace {
constexpr u64 Limit = u64{1} << 40;
constexpr u64 Words = (Limit >> BlockShift) / 64;
/// Two bitmaps of Words each (armed, then dynamic), reserved at the first arm, not committed.
std::atomic<std::atomic<u64>*> bits{nullptr};

std::atomic<u64>* Table(bool create) {
    std::atomic<u64>* table = bits.load(std::memory_order_acquire);
    if (table || !create) {
        return table;
    }
    void* mem = mmap(nullptr, 2 * Words * sizeof(u64), PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) {
        return nullptr;
    }
    std::atomic<u64>* expected = nullptr;
    if (!bits.compare_exchange_strong(expected, static_cast<std::atomic<u64>*>(mem),
                                      std::memory_order_acq_rel)) {
        munmap(mem, 2 * Words * sizeof(u64));
        return expected;
    }
    return static_cast<std::atomic<u64>*>(mem);
}
std::atomic<u64>* ArmedWord(std::atomic<u64>* table, u64 block) {
    return &table[block >> 6];
}
std::atomic<u64>* DynamicWord(std::atomic<u64>* table, u64 block) {
    return &table[Words + (block >> 6)];
}
bool InRange(u64 block) {
    return block < (Limit >> BlockShift);
}
/// Calls `f(block)` for the blocks of [address, address + size) below the limit until it says stop.
template <typename F>
void ForEachBlock(u64 address, u64 size, F&& f) {
    if (size == 0) {
        return;
    }
    const u64 first = address >> BlockShift, last = (address + size - 1) >> BlockShift;
    for (u64 block = first; block <= last && InRange(block); ++block) {
        if (!f(block)) {
            return;
        }
    }
}
} // namespace

bool Armed(u64 block) {
    std::atomic<u64>* table = Table(false);
    return table && InRange(block) &&
           (ArmedWord(table, block)->load(std::memory_order_acquire) >> (block & 63) & 1);
}

bool IsDynamic(u64 block) {
    std::atomic<u64>* table = Table(false);
    return table && InRange(block) &&
           (DynamicWord(table, block)->load(std::memory_order_acquire) >> (block & 63) & 1);
}

bool Any(u64 address, u64 size) {
    if (!Table(false)) {
        return false;
    }
    bool found = false;
    ForEachBlock(address, size, [&](u64 block) { return !(found = IsDynamic(block)); });
    return found;
}

bool AnyArmed(u64 address, u64 size) {
    if (!Table(false)) {
        return false;
    }
    bool found = false;
    ForEachBlock(address, size, [&](u64 block) { return !(found = Armed(block)); });
    return found;
}

bool AnyWatched(u64 address, u64 size) {
    if (!Table(false)) {
        return false;
    }
    bool found = false;
    ForEachBlock(address, size,
                 [&](u64 block) { return !(found = Armed(block) || IsDynamic(block)); });
    return found;
}

bool TryArm(u64 block) {
    std::atomic<u64>* table = Table(true);
    if (!table || !InRange(block) || IsDynamic(block)) {
        return false;
    }
    const u64 bit = u64{1} << (block & 63);
    return !(ArmedWord(table, block)->fetch_or(bit, std::memory_order_acq_rel) & bit);
}

namespace {
constexpr unsigned RegionShift = 21; // 2 MiB of guest VA
constexpr u64 RegionBlocks = u64{1} << (RegionShift - BlockShift);
static_assert(RegionBlocks == 32, "a region is half a bitmap word");

/// Every block of region `region` dynamic (and no longer armed); true when one of them was not.
bool MarkRegion(std::atomic<u64>* table, u64 region) {
    const u64 block = region * RegionBlocks;
    if (!InRange(block)) {
        return false;
    }
    const u64 mask = u64{0xffffffffu} << ((region & 1) * 32);
    const u64 before = DynamicWord(table, block)->fetch_or(mask, std::memory_order_acq_rel);
    ArmedWord(table, block)->fetch_and(~mask, std::memory_order_acq_rel);
    const u64 fresh = mask & ~before;
    if (fresh == 0) {
        return false;
    }
    counters.dynamic_blocks.fetch_add(static_cast<u64>(std::popcount(fresh)),
                                      std::memory_order_relaxed);
    counters.dynamic_regions.fetch_add(1, std::memory_order_relaxed);
    return true;
}

/// A dynamic block other than `block` within RegionBlocks blocks of it (2 MiB either side), or ~0.
u64 DynamicNeighbour(std::atomic<u64>* table, u64 block) {
    const u64 from = block >= RegionBlocks ? block - RegionBlocks : 0;
    const u64 to = std::min<u64>(block + RegionBlocks, (Limit >> BlockShift) - 1); // inclusive
    for (u64 word = from >> 6; word <= to >> 6; ++word) {
        u64 bits = table[Words + word].load(std::memory_order_acquire);
        const u64 lo = word == from >> 6 ? from & 63 : 0, hi = word == to >> 6 ? to & 63 : 63;
        bits &= (hi == 63 ? ~u64{0} : (u64{2} << hi) - 1) & ~((u64{1} << lo) - 1);
        if (word == block >> 6) {
            bits &= ~(u64{1} << (block & 63));
        }
        if (bits != 0) {
            return (word << 6) + static_cast<u64>(std::countr_zero(bits));
        }
    }
    return ~u64{0};
}
} // namespace

Marked Mark(u64 block) {
    Marked marked{};
    std::atomic<u64>* table = Table(true);
    if (!table || !InRange(block)) {
        return marked;
    }
    const u64 bit = u64{1} << (block & 63);
    marked.fresh = !(DynamicWord(table, block)->fetch_or(bit, std::memory_order_acq_rel) & bit);
    ArmedWord(table, block)->fetch_and(~bit, std::memory_order_acq_rel);
    if (!marked.fresh) {
        return marked;
    }
    // No log here: this runs in the fault handler (the binding thread reports, UmaDirect).
    counters.dynamic_blocks.fetch_add(1, std::memory_order_relaxed);
    // A second dynamic block within 2 MiB: a ring of per-frame slots (skinned or animated
    // geometry rotating through 256 KiB vertex slots). The 2 MiB regions of both are dynamic at
    // once, instead of one verify mismatch or trap fault per slot.
    if (const u64 neighbour = DynamicNeighbour(table, block); neighbour != ~u64{0}) {
        const u64 a = std::min(block, neighbour) / RegionBlocks;
        const u64 b = std::max(block, neighbour) / RegionBlocks;
        bool any = false;
        for (u64 region = a; region <= b; ++region) {
            any |= MarkRegion(table, region);
        }
        if (any) {
            marked.first = a * RegionBlocks;
            marked.end = (b + 1) * RegionBlocks;
        }
    }
    NoteVerdictChange();
    return marked;
}

void MarkRange(u64 address, u64 size) {
    std::atomic<u64>* table = Table(true);
    if (!table || size == 0) {
        return;
    }
    // A verify mismatch: the whole 2 MiB regions of the binding at once (a binding that changed
    // under the GPU is per-frame data; its neighbours are usually its other slots).
    const u64 first = (address >> RegionShift), last = ((address + size - 1) >> RegionShift);
    bool any = false;
    for (u64 region = first; region <= last; ++region) {
        if (MarkRegion(table, region)) {
            any = true;
            counters.dynamic_verify.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (any) {
        NoteVerdictChange();
    }
}

void Forget(u64 address, u64 size) {
    std::atomic<u64>* table = Table(false);
    if (!table) {
        return;
    }
    bool changed = false;
    ForEachBlock(address, size, [&](u64 block) {
        const u64 bit = u64{1} << (block & 63);
        changed |= (ArmedWord(table, block)->fetch_and(~bit, std::memory_order_acq_rel) & bit) != 0;
        changed |= (DynamicWord(table, block)->fetch_and(~bit, std::memory_order_acq_rel) & bit) != 0;
        return true;
    });
    if (changed) {
        NoteVerdictChange();
    }
}
} // namespace Dynamic

// ---------------------------------------------------------------------------------------------
// DMA fills and copies into shared granules
// ---------------------------------------------------------------------------------------------

bool DmaGpu() {
    static const bool on = [] {
        const char* env = std::getenv("BB_UMA_DMA_GPU");
        return env && env[0] == '1';
    }();
    return on;
}

namespace {
struct SharedDmaState {
    static constexpr u32 Listed = 32;
    struct Entry {
        u64 address = 0, size = 0;
        bool fill = false, cpu = false; ///< of the first one there
    };
    std::atomic<u64> count{0}, bytes{0}, cpu{0}, gpu{0};
    std::atomic<u32> listed{0};
    std::mutex mutex; ///< entries, reported (only taken until the list is full)
    std::array<Entry, Listed> entries{};
    u32 reported = 0;
};
SharedDmaState shared_dma;
} // namespace

void NoteSharedDma(u64 address, u64 size, bool fill, bool cpu) {
    auto& state = shared_dma;
    state.count.fetch_add(1, std::memory_order_relaxed);
    state.bytes.fetch_add(size, std::memory_order_relaxed);
    (cpu ? state.cpu : state.gpu).fetch_add(1, std::memory_order_relaxed);
    if (state.listed.load(std::memory_order_acquire) >= SharedDmaState::Listed) {
        return;
    }
    std::scoped_lock lock{state.mutex};
    const u32 listed = state.listed.load(std::memory_order_relaxed);
    if (listed >= SharedDmaState::Listed) {
        return;
    }
    for (u32 i = 0; i < listed; ++i) {
        if (state.entries[i].address == address) {
            return;
        }
    }
    state.entries[listed] = {address, size, fill, cpu};
    state.listed.store(listed + 1, std::memory_order_release);
}

SharedDmaTotals SharedDma() {
    const auto& state = shared_dma;
    return {state.count.load(std::memory_order_relaxed), state.bytes.load(std::memory_order_relaxed),
            state.cpu.load(std::memory_order_relaxed), state.gpu.load(std::memory_order_relaxed)};
}

bool SharedDmaNewList(std::string& text) {
    auto& state = shared_dma;
    std::scoped_lock lock{state.mutex};
    const u32 listed = state.listed.load(std::memory_order_relaxed);
    if (listed == state.reported) {
        return false;
    }
    state.reported = listed;
    constexpr std::size_t MaxLine = 1500;
    text.clear();
    for (u32 i = 0; i < listed; ++i) {
        const auto& entry = state.entries[i];
        char item[96];
        std::snprintf(item, sizeof(item), "%s%s %#llx+%#llx %s", i ? ", " : "",
                      entry.fill ? "fill" : "copy", (unsigned long long)entry.address,
                      (unsigned long long)entry.size, entry.cpu ? "cpu" : "gpu");
        if (text.size() + std::strlen(item) > MaxLine) {
            text += ", ...";
            break;
        }
        text += item;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Probes
// ---------------------------------------------------------------------------------------------

namespace {
enum class RunResult { Ok, Failed, TimedOut };

/// Records `record(cmd)` on a one-time command buffer of `queue`, submits it and waits (5 s).
/// `d` is the dispatcher of `dev` (the default one for the game's device, a local one for a
/// throwaway device). After a timeout nothing is destroyed: the GPU may still own it.
template <typename Dispatch, typename Record>
RunResult RunOn(vk::Device dev, vk::Queue queue, u32 family, const Dispatch& d, Record&& record) {
    const vk::CommandPoolCreateInfo pool_ci{.queueFamilyIndex = family};
    const auto [pool_result, pool] = dev.createCommandPool(pool_ci, nullptr, d);
    if (pool_result != vk::Result::eSuccess) {
        return RunResult::Failed;
    }
    RunResult result = RunResult::Failed;
    const vk::CommandBufferAllocateInfo allocate_info{
        .commandPool = pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };
    const auto [alloc_result, cmdbufs] = dev.allocateCommandBuffers(allocate_info, d);
    const auto [fence_result, fence] = dev.createFence({}, nullptr, d);
    if (alloc_result == vk::Result::eSuccess && fence_result == vk::Result::eSuccess) {
        const vk::CommandBuffer cmd = cmdbufs[0];
        const vk::CommandBufferBeginInfo begin_info{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
        if (cmd.begin(begin_info, d) == vk::Result::eSuccess) {
            record(cmd);
            const vk::MemoryBarrier to_host{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                            .dstAccessMask = vk::AccessFlagBits::eHostRead};
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                vk::PipelineStageFlagBits::eHost, {}, to_host, {}, {}, d);
            if (cmd.end(d) == vk::Result::eSuccess) {
                const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd};
                if (queue.submit(submit, fence, d) == vk::Result::eSuccess) {
                    const vk::Result waited = dev.waitForFences(fence, true, 5'000'000'000ull, d);
                    result = waited == vk::Result::eSuccess   ? RunResult::Ok
                             : waited == vk::Result::eTimeout ? RunResult::TimedOut
                                                              : RunResult::Failed;
                }
            }
        }
    }
    if (result == RunResult::TimedOut) {
        return result;
    }
    if (fence_result == vk::Result::eSuccess) {
        dev.destroyFence(fence, nullptr, d);
    }
    dev.destroyCommandPool(pool, nullptr, d);
    return result;
}

/// A deterministic 64-bit word for position `index`; `salt` tells the tests apart.
inline u64 Word(u64 index, u64 salt) {
    return ((index + 1) * 0x9E3779B97F4A7C15ull) ^ (salt * 0xD6E8FEB86659FD93ull);
}

struct Mismatch {
    u64 count = 0;
    u64 first = ~0ull; ///< word index of the first
};

std::string Describe(const char* what, const Mismatch& m, u64 words) {
    char text[200];
    std::snprintf(text, sizeof(text), "%s: %llu of %llu words differ, first at +%#llx", what,
                  (unsigned long long)m.count, (unsigned long long)words,
                  (unsigned long long)(m.first * 8));
    return text;
}
} // namespace

bool ProbeCoherence(const Vulkan::Instance& instance, u32 memory_type, std::string& detail) {
    constexpr u64 Size = 32 * MiB, Half = Size / 2, Quarter = Size / 4;
    constexpr u64 HalfWords = Half / 8, QuarterWords = Quarter / 8;
    const auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    const vk::Device dev = instance.GetDevice();
    const vk::Queue queue = instance.GetGraphicsQueue();
    const u32 family = instance.GetGraphicsQueueFamilyIndex();

    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
    };
    const vk::BufferCreateInfo buffer_ci{
        .pNext = &external,
        .size = Size,
        .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const auto [buffer_result, buffer_value] = dev.createBuffer(buffer_ci, nullptr, d);
    if (buffer_result != vk::Result::eSuccess) {
        detail = "creating the 32 MiB buffer failed (" + vk::to_string(buffer_result) + ")";
        return false;
    }
    const vk::Buffer buffer = buffer_value;
    vk::DeviceMemory memory{};
    void* map = MAP_FAILED;
    bool timed_out = false;
    const auto release = [&] {
        if (timed_out) {
            return; // the GPU may still use them: leaked on purpose, UMA is off anyway
        }
        if (map != MAP_FAILED) {
            munmap(map, Size);
        }
        dev.destroyBuffer(buffer, nullptr, d);
        if (memory) {
            dev.freeMemory(memory, nullptr, d);
        }
    };
    const auto requirements = dev.getBufferMemoryRequirements(buffer, d);
    if (!(requirements.memoryTypeBits & (1u << memory_type))) {
        detail = "memory type " + std::to_string(memory_type) + " is not allowed for a 32 MiB buffer";
        release();
        return false;
    }
    const vk::ExportMemoryAllocateInfo export_info{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
    };
    const vk::MemoryAllocateInfo allocate_info{
        .pNext = &export_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    const auto [memory_result, allocated] = dev.allocateMemory(allocate_info, nullptr, d);
    if (memory_result != vk::Result::eSuccess) {
        detail = "allocating 32 MiB of exportable memory failed (" + vk::to_string(memory_result) + ")";
        release();
        return false;
    }
    memory = allocated;
    if (const auto result = dev.bindBufferMemory(buffer, memory, 0, d); result != vk::Result::eSuccess) {
        detail = "binding the 32 MiB buffer failed (" + vk::to_string(result) + ")";
        release();
        return false;
    }
    const auto [fd_result, fd] = dev.getMemoryFdKHR(
        {.memory = memory, .handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT}, d);
    if (fd_result != vk::Result::eSuccess) {
        detail = "dma-buf export failed (" + vk::to_string(fd_result) + ")";
        release();
        return false;
    }
    // As the runtime maps a granule: the dma-buf fd, shared, read-write, offset 0.
    map = mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        detail = std::string("mmap of the 32 MiB dma-buf failed: ") + std::strerror(errno);
        release();
        return false;
    }
    volatile u64* w = static_cast<volatile u64*>(map);

    const auto run = [&](auto&& record) {
        const RunResult result = RunOn(dev, queue, family, d, record);
        timed_out |= result == RunResult::TimedOut;
        return result == RunResult::Ok;
    };
    const auto compare = [&](u64 first_word, u64 words, u64 index_offset, u64 salt) {
        Mismatch m;
        for (u64 i = 0; i < words; ++i) {
            if (w[first_word + i] != Word(index_offset + i, salt)) {
                m.first = m.count == 0 ? i : m.first;
                ++m.count;
            }
        }
        return m;
    };
    std::vector<std::string> failures;

    // 1. The CPU writes the first half; the destination half holds other data the CPU wrote (its
    //    lines are in the CPU caches, dirty); the GPU copies; the CPU checks both halves.
    for (u64 i = 0; i < HalfWords; ++i) {
        w[HalfWords + i] = ~Word(i, 1);
    }
    for (u64 i = 0; i < HalfWords; ++i) {
        w[i] = Word(i, 2);
    }
    if (!run([&](vk::CommandBuffer cmd) {
            cmd.copyBuffer(buffer, buffer, vk::BufferCopy{0, Half, Half}, d);
        })) {
        failures.push_back("the whole-buffer GPU copy failed or timed out");
    } else {
        const Mismatch dst = compare(HalfWords, HalfWords, 0, 2);
        const Mismatch src = compare(0, HalfWords, 0, 2);
        if (dst.count) {
            failures.push_back(Describe("CPU write -> GPU copy (16 MiB)", dst, HalfWords));
        }
        if (src.count) {
            failures.push_back(Describe("source half changed during the GPU copy", src, HalfWords));
        }
    }

    // 2. 64 single 64-byte lines: written by the CPU (destination line dirty in its caches), then a
    //    small GPU copy of just that line at once, then the CPU reads the destination line.
    u64 bad_lines = 0, first_bad_line = ~0ull;
    for (u64 i = 0; i < 64 && failures.empty(); ++i) {
        const u64 line = (i * 104729ull + 7) % (Half / 64);
        const u64 src = line * 64, dst = Half + line * 64;
        for (u64 k = 0; k < 8; ++k) {
            w[dst / 8 + k] = ~Word(i * 8 + k, 5);
            w[src / 8 + k] = Word(i * 8 + k, 4);
        }
        if (!run([&](vk::CommandBuffer cmd) {
                cmd.copyBuffer(buffer, buffer, vk::BufferCopy{src, dst, 64}, d);
            })) {
            failures.push_back("a single-line GPU copy failed or timed out");
            break;
        }
        bool ok = true;
        for (u64 k = 0; k < 8; ++k) {
            ok &= w[dst / 8 + k] == Word(i * 8 + k, 4);
        }
        if (!ok) {
            first_bad_line = bad_lines == 0 ? i : first_bad_line;
            ++bad_lines;
        }
    }
    if (bad_lines) {
        failures.push_back(std::to_string(bad_lines) + " of 64 single 64-byte lines written before a "
                           "GPU copy were not seen (first: #" + std::to_string(first_bad_line) + ")");
    }

    // 3. The CPU has read the destination half (its lines are cached, clean), then the GPU fills it.
    if (failures.empty()) {
        u64 sum = 0;
        for (u64 i = 0; i < HalfWords; ++i) {
            sum += w[HalfWords + i];
        }
        static volatile u64 sink;
        sink = sum;
        if (!run([&](vk::CommandBuffer cmd) {
                cmd.fillBuffer(buffer, Half, Half, 0x600dcafeu, d);
            })) {
            failures.push_back("the GPU fill failed or timed out");
        } else {
            u64 bad = 0, first = ~0ull;
            for (u64 i = 0; i < HalfWords; ++i) {
                if (w[HalfWords + i] != 0x600dcafe600dcafeull) {
                    first = bad == 0 ? i : first;
                    ++bad;
                }
            }
            if (bad) {
                failures.push_back(Describe("GPU fill over lines the CPU had read (16 MiB)",
                                            {bad, first}, HalfWords));
            }
        }
    }

    // 4. A writer thread on another core writes a quarter, the GPU copies it, the CPU checks.
    std::vector<int> cpus;
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0) {
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &allowed)) {
                cpus.push_back(cpu);
            }
        }
    }
    std::string cores = "one core";
    if (cpus.size() >= 2 && failures.empty()) {
        const std::array<int, 2> pinned = {cpus.front(), cpus.back()};
        cores = "cores " + std::to_string(pinned[0]) + " and " + std::to_string(pinned[1]);
        for (u64 region = 0; region < 2 && failures.empty(); ++region) {
            const u64 base = region * QuarterWords;
            for (u64 i = 0; i < QuarterWords; ++i) {
                w[HalfWords + base + i] = ~Word(base + i, 7);
            }
            std::thread writer([&, region] {
                cpu_set_t set;
                CPU_ZERO(&set);
                CPU_SET(pinned[region], &set);
                sched_setaffinity(0, sizeof(set), &set); // this thread only
                for (u64 i = 0; i < QuarterWords; ++i) {
                    w[base + i] = Word(base + i, 6 + region);
                }
            });
            writer.join();
            if (!run([&](vk::CommandBuffer cmd) {
                    cmd.copyBuffer(buffer, buffer,
                                   vk::BufferCopy{region * Quarter, Half + region * Quarter, Quarter},
                                   d);
                })) {
                failures.push_back("the GPU copy of a thread's writes failed or timed out");
                break;
            }
            const Mismatch m = compare(HalfWords + base, QuarterWords, base, 6 + region);
            if (m.count) {
                failures.push_back(Describe(
                    ("writes of a thread pinned to cpu " + std::to_string(pinned[region]) +
                     " -> GPU copy (8 MiB)")
                        .c_str(),
                    m, QuarterWords));
            }
        }
    } else if (cpus.size() < 2) {
        cores = "one core (the writer test was skipped)";
    }

    release();
    if (!failures.empty()) {
        detail.clear();
        for (const auto& text : failures) {
            detail += detail.empty() ? "" : "; ";
            detail += text;
        }
        return false;
    }
    detail = "32 MiB through mmap of the dma-buf: whole-buffer copy, 64 single lines, GPU fill over "
             "cached lines, writers on " + cores + " all matched";
    return true;
}

bool ProbeSparseDmaBuf(const Vulkan::Instance& instance, std::string& detail) {
    constexpr u64 SparseSize = 8 * MiB, BindOffset = 64 * 1024, BindSize = 2 * MiB;
    const vk::PhysicalDevice physical = instance.GetPhysicalDevice();

    // A queue family with sparse binding and transfers; the game's family when it has both.
    const auto families = physical.getQueueFamilyProperties();
    const auto usable = [&](u32 i) {
        const auto flags = families[i].queueFlags;
        return (flags & vk::QueueFlagBits::eSparseBinding) &&
               (flags & (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute |
                         vk::QueueFlagBits::eTransfer));
    };
    u32 family = ~0u;
    if (const u32 preferred = instance.GetGraphicsQueueFamilyIndex();
        preferred < families.size() && usable(preferred)) {
        family = preferred;
    }
    for (u32 i = 0; i < families.size() && family == ~0u; ++i) {
        if (usable(i)) {
            family = i;
        }
    }
    if (family == ~0u) {
        detail = "no queue family with sparse binding and transfers";
        return false;
    }

    // The throwaway device: same physical device, only what the probe needs.
    const float priority = 1.0f;
    const vk::DeviceQueueCreateInfo queue_ci{
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    const std::array<const char*, 2> extensions = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                                   VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
    const vk::PhysicalDeviceFeatures features{.sparseBinding = true, .sparseResidencyBuffer = true};
    const vk::DeviceCreateInfo device_ci{
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_ci,
        .enabledExtensionCount = static_cast<u32>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
        .pEnabledFeatures = &features,
    };
    const auto [device_result, device_value] = physical.createDevice(device_ci);
    if (device_result != vk::Result::eSuccess) {
        detail = "creating the throwaway device failed (" + vk::to_string(device_result) + ")";
        return false;
    }
    const vk::Device dev = device_value;
    // Its own function table: the game's device keeps the default one.
    VULKAN_HPP_DEFAULT_DISPATCHER_TYPE dld;
    dld.init(static_cast<VkInstance>(instance.GetInstance()),
             VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr, static_cast<VkDevice>(dev),
             VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr);
    const vk::Queue queue = dev.getQueue(family, 0, dld);
    const auto props = physical.getMemoryProperties();

    vk::Buffer sparse{}, plain{};
    vk::DeviceMemory exported{}, readback{};
    void* map = MAP_FAILED;
    void* plain_map = nullptr;
    bool timed_out = false;
    const auto release = [&] {
        if (timed_out) {
            return; // the GPU may still use the device: leaked on purpose, UMA is off anyway
        }
        (void)dev.waitIdle(dld);
        if (plain_map) {
            dev.unmapMemory(readback, dld);
        }
        if (plain) {
            dev.destroyBuffer(plain, nullptr, dld);
        }
        if (sparse) {
            dev.destroyBuffer(sparse, nullptr, dld);
        }
        if (map != MAP_FAILED) {
            munmap(map, BindSize);
        }
        if (readback) {
            dev.freeMemory(readback, nullptr, dld);
        }
        if (exported) {
            dev.freeMemory(exported, nullptr, dld);
        }
        dev.destroy(nullptr, dld);
    };
    const auto fail = [&](const char* what, vk::Result result) {
        detail = std::string(what) + " failed (" + vk::to_string(result) + ")";
        release();
        return false;
    };
    const auto run = [&](auto&& record) {
        const RunResult result = RunOn(dev, queue, family, dld, record);
        timed_out |= result == RunResult::TimedOut;
        return result == RunResult::Ok;
    };

    // The sparse buffer as the arena is made: sparse binding and residency, the dma-buf handle type.
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
    };
    const vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eTransferSrc |
                                       vk::BufferUsageFlagBits::eTransferDst |
                                       vk::BufferUsageFlagBits::eStorageBuffer;
    const vk::BufferCreateInfo sparse_ci{
        .pNext = &external,
        .flags = vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = SparseSize,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const auto [sparse_result, sparse_buffer] = dev.createBuffer(sparse_ci, nullptr, dld);
    if (sparse_result != vk::Result::eSuccess) {
        return fail("creating a sparse buffer for dma-buf memory", sparse_result);
    }
    sparse = sparse_buffer;
    const auto sparse_reqs = dev.getBufferMemoryRequirements(sparse, dld);
    if (sparse_reqs.alignment == 0 || BindOffset % sparse_reqs.alignment != 0 ||
        BindSize % sparse_reqs.alignment != 0) {
        detail = "the sparse buffer's alignment (" + std::to_string(sparse_reqs.alignment) +
                 ") does not divide the 64 KiB offset or 2 MiB size";
        release();
        return false;
    }

    // Exportable memory: the type of a plain exportable buffer, host-visible and CPU-cached, that
    // the sparse buffer takes too.
    const vk::BufferCreateInfo export_buffer_ci{
        .pNext = &external,
        .size = BindSize,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const auto [probe_result, probe_buffer] = dev.createBuffer(export_buffer_ci, nullptr, dld);
    if (probe_result != vk::Result::eSuccess) {
        return fail("creating an exportable buffer", probe_result);
    }
    const u32 export_bits = dev.getBufferMemoryRequirements(probe_buffer, dld).memoryTypeBits;
    dev.destroyBuffer(probe_buffer, nullptr, dld);
    bool coherent = false;
    const u32 type =
        BbGuestMemory::PickUnifiedType(props, export_bits & sparse_reqs.memoryTypeBits, &coherent);
    if (type == ~0u) {
        detail = "no CPU-cached host-visible memory type is allowed by both the exportable and the "
                 "sparse buffer";
        release();
        return false;
    }
    const vk::ExportMemoryAllocateInfo export_info{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
    };
    const vk::MemoryAllocateInfo allocate_info{
        .pNext = &export_info, .allocationSize = BindSize, .memoryTypeIndex = type};
    const auto [memory_result, memory] = dev.allocateMemory(allocate_info, nullptr, dld);
    if (memory_result != vk::Result::eSuccess) {
        return fail("allocating exportable memory", memory_result);
    }
    exported = memory;
    const auto [fd_result, fd] = dev.getMemoryFdKHR(
        {.memory = exported, .handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT}, dld);
    if (fd_result != vk::Result::eSuccess) {
        return fail("dma-buf export", fd_result);
    }
    map = mmap(nullptr, BindSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        detail = std::string("mmap of the exported dma-buf failed: ") + std::strerror(errno);
        release();
        return false;
    }

    // A plain buffer in host-coherent memory to read the GPU's copy through Vulkan's own mapping.
    const vk::BufferCreateInfo plain_ci{
        .size = BindSize,
        .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const auto [plain_result, plain_buffer] = dev.createBuffer(plain_ci, nullptr, dld);
    if (plain_result != vk::Result::eSuccess) {
        return fail("creating the readback buffer", plain_result);
    }
    plain = plain_buffer;
    const auto plain_reqs = dev.getBufferMemoryRequirements(plain, dld);
    u32 plain_type = ~0u;
    const auto coherent_host = vk::MemoryPropertyFlagBits::eHostVisible |
                               vk::MemoryPropertyFlagBits::eHostCoherent;
    for (u32 i = 0; i < props.memoryTypeCount && plain_type == ~0u; ++i) {
        if ((plain_reqs.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & coherent_host) == coherent_host) {
            plain_type = i;
        }
    }
    if (plain_type == ~0u) {
        detail = "no host-coherent memory type for the readback buffer";
        release();
        return false;
    }
    const auto [readback_result, readback_memory] = dev.allocateMemory(
        {.allocationSize = plain_reqs.size, .memoryTypeIndex = plain_type}, nullptr, dld);
    if (readback_result != vk::Result::eSuccess) {
        return fail("allocating the readback memory", readback_result);
    }
    readback = readback_memory;
    if (const auto result = dev.bindBufferMemory(plain, readback, 0, dld);
        result != vk::Result::eSuccess) {
        return fail("binding the readback buffer", result);
    }
    const auto [plain_map_result, plain_pointer] = dev.mapMemory(readback, 0, BindSize, {}, dld);
    if (plain_map_result != vk::Result::eSuccess) {
        return fail("mapping the readback buffer", plain_map_result);
    }
    plain_map = plain_pointer;

    // Bind the dma-buf memory into the sparse buffer at 64 KiB.
    const vk::SparseMemoryBind bind{
        .resourceOffset = BindOffset, .size = BindSize, .memory = exported, .memoryOffset = 0};
    const vk::SparseBufferMemoryBindInfo buffer_bind{.buffer = sparse, .bindCount = 1, .pBinds = &bind};
    const auto [fence_result, fence] = dev.createFence({}, nullptr, dld);
    if (fence_result != vk::Result::eSuccess) {
        return fail("creating a fence", fence_result);
    }
    const vk::BindSparseInfo bind_info{.bufferBindCount = 1, .pBufferBinds = &buffer_bind};
    if (const auto result = queue.bindSparse(bind_info, fence, dld); result != vk::Result::eSuccess) {
        dev.destroyFence(fence, nullptr, dld);
        return fail("vkQueueBindSparse of dma-buf memory", result);
    }
    const vk::Result waited = dev.waitForFences(fence, true, 5'000'000'000ull, dld);
    if (waited == vk::Result::eTimeout) {
        timed_out = true;
        detail = "vkQueueBindSparse of dma-buf memory did not finish in 5 s";
        release();
        return false;
    }
    dev.destroyFence(fence, nullptr, dld);
    if (waited != vk::Result::eSuccess) {
        return fail("waiting for vkQueueBindSparse", waited);
    }

    // GPU writes through the sparse buffer, the CPU reads through the mapping (its lines hot).
    constexpr u64 Words = BindSize / 4;
    volatile u32* cpu = static_cast<volatile u32*>(map);
    for (u64 i = 0; i < Words; ++i) {
        cpu[i] = 0xdeadbeefu;
    }
    if (!run([&](vk::CommandBuffer cmd) {
            cmd.fillBuffer(sparse, BindOffset, BindSize, 0x600dcafeu, dld);
        })) {
        detail = timed_out ? "the GPU fill through the sparse buffer did not finish in 5 s"
                           : "the GPU fill through the sparse buffer failed";
        release();
        return false;
    }
    u64 bad = 0;
    for (u64 i = 0; i < Words; ++i) {
        bad += cpu[i] != 0x600dcafeu;
    }
    if (bad) {
        detail = "GPU fill through the sparse buffer: the CPU read " + std::to_string(bad) + " of " +
                 std::to_string(Words) + " words wrong through the mapping";
        release();
        return false;
    }

    // CPU writes through the mapping, the GPU copies from the sparse buffer into the plain one.
    for (u64 i = 0; i < Words; ++i) {
        cpu[i] = 0xa5000000u + u32(i);
    }
    if (!run([&](vk::CommandBuffer cmd) {
            cmd.copyBuffer(sparse, plain, vk::BufferCopy{BindOffset, 0, BindSize}, dld);
        })) {
        detail = timed_out ? "the GPU copy out of the sparse buffer did not finish in 5 s"
                           : "the GPU copy out of the sparse buffer failed";
        release();
        return false;
    }
    const auto* copied = static_cast<const volatile u32*>(plain_map);
    bad = 0;
    for (u64 i = 0; i < Words; ++i) {
        bad += copied[i] != 0xa5000000u + u32(i);
    }
    if (bad) {
        detail = "CPU writes through the mapping: the GPU copy out of the sparse buffer read " +
                 std::to_string(bad) + " of " + std::to_string(Words) + " words wrong";
        release();
        return false;
    }
    release();
    detail = std::string("sparse buffer bound to 2 MiB of dma-buf memory at 64 KiB (queue family ") +
             std::to_string(family) + ", memory type " + std::to_string(type) + (coherent ? "" : ", not coherent") +
             "): GPU fill and copy match the CPU's view through the mapping";
    return true;
}

// ---------------------------------------------------------------------------------------------
// KGSL diagnostic
// ---------------------------------------------------------------------------------------------

#if defined(__ANDROID__)
namespace {
// The few definitions needed, from Qualcomm's msm_kgsl.h (SPDX GPL-2.0 WITH Linux-syscall-note; the
// copy in Mesa's src/freedreno/vulkan/msm_kgsl.h, used by Turnip's tu_knl_kgsl.cc). Only the
// structures and numbers of IOCTL_KGSL_GPUOBJ_IMPORT / _INFO / _FREE.
constexpr unsigned KgslIocType = 0x09;
constexpr unsigned KgslUserMemTypeDmabuf = 0x00000003;
constexpr u64 KgslMemflagsIocoherent = 1ull << 31;
constexpr unsigned KgslCachemodeMask = 0x0C000000u;
constexpr unsigned KgslCachemodeShift = 26;
constexpr unsigned KgslCachemodeWriteback = 3;
struct KgslGpuobjFree {
    u64 flags;
    u64 priv;
    unsigned int id;
    unsigned int type;
    unsigned int len;
};
struct KgslGpuobjInfo {
    u64 gpuaddr;
    u64 flags;
    u64 size;
    u64 va_len;
    u64 va_addr;
    unsigned int id;
};
struct KgslGpuobjImport {
    u64 priv;
    u64 priv_len;
    u64 flags;
    unsigned int type;
    unsigned int id;
};
struct KgslGpuobjImportDmaBuf {
    int fd;
};
#define BB_IOCTL_KGSL_GPUOBJ_FREE _IOW(KgslIocType, 0x46, KgslGpuobjFree)
#define BB_IOCTL_KGSL_GPUOBJ_INFO _IOWR(KgslIocType, 0x47, KgslGpuobjInfo)
#define BB_IOCTL_KGSL_GPUOBJ_IMPORT _IOWR(KgslIocType, 0x48, KgslGpuobjImport)

/// Imports a fresh 2 MiB dma-heap buffer into KGSL with `flags`; the text says what KGSL made of it.
std::string KgslImport(int kgsl, int heap, u64 flags) {
    char text[160];
    dma_heap_allocation_data alloc{};
    alloc.len = 2 * MiB;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc) != 0) {
        std::snprintf(text, sizeof(text), "failed (DMA_HEAP_IOCTL_ALLOC: errno %d, %s)", errno,
                      std::strerror(errno));
        return text;
    }
    KgslGpuobjImportDmaBuf dmabuf{static_cast<int>(alloc.fd)};
    KgslGpuobjImport import{};
    import.priv = reinterpret_cast<uintptr_t>(&dmabuf);
    import.priv_len = sizeof(dmabuf);
    import.flags = flags;
    import.type = KgslUserMemTypeDmabuf;
    std::string result;
    if (ioctl(kgsl, BB_IOCTL_KGSL_GPUOBJ_IMPORT, &import) != 0) {
        std::snprintf(text, sizeof(text), "failed (GPUOBJ_IMPORT: errno %d, %s)", errno,
                      std::strerror(errno));
        result = text;
    } else {
        KgslGpuobjInfo info{};
        info.id = import.id;
        if (ioctl(kgsl, BB_IOCTL_KGSL_GPUOBJ_INFO, &info) != 0) {
            std::snprintf(text, sizeof(text), "failed (GPUOBJ_INFO: errno %d, %s)", errno,
                          std::strerror(errno));
            result = text;
        } else {
            std::snprintf(text, sizeof(text), "iocoherent=%d cachemode=%u",
                          (info.flags & KgslMemflagsIocoherent) ? 1 : 0,
                          unsigned((info.flags & KgslCachemodeMask) >> KgslCachemodeShift));
            result = text;
        }
        KgslGpuobjFree release{};
        release.id = import.id;
        ioctl(kgsl, BB_IOCTL_KGSL_GPUOBJ_FREE, &release);
    }
    close(static_cast<int>(alloc.fd));
    return result;
}
} // namespace
#endif

void KgslDiagnostic() {
#if defined(__ANDROID__)
    const int kgsl = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (kgsl < 0) {
        std::printf("UMA KGSL: cannot open /dev/kgsl-3d0 (errno %d, %s)\n", errno,
                    std::strerror(errno));
        return;
    }
    const int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (heap < 0) {
        std::printf("UMA KGSL: cannot open /dev/dma_heap/system (errno %d, %s)\n", errno,
                    std::strerror(errno));
        close(kgsl);
        return;
    }
    const std::string plain = KgslImport(kgsl, heap, 0);
    const std::string coherent =
        KgslImport(kgsl, heap, KgslMemflagsIocoherent | (u64(KgslCachemodeWriteback) << KgslCachemodeShift));
    std::printf("UMA KGSL: import flags=0 -> %s; requested iocoherent -> %s\n", plain.c_str(),
                coherent.c_str());
    close(heap);
    close(kgsl);
#endif
}

} // namespace BbUma
