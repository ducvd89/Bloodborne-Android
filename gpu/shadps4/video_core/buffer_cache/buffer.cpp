// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <numeric>
#include <algorithm>
#include <functional>
#include <mutex>
#include <fmt/format.h>

#include "common/alignment.h"
#include "common/assert.h"
#include "bbport_toggles.h"
#include "video_core/buffer_cache/buffer.h"
#include "bbport_guest_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

std::string_view BufferTypeName(MemoryType type) {
    switch (type) {
    case MemoryType::HostUncached:
        return "HostUncached";
    case MemoryType::HostCached:
        return "HostCached";
    case MemoryType::Stream:
        return "Stream";
    case MemoryType::DeviceLocal:
        return "DeviceLocal";
    case MemoryType::Sparse:
        return "Sparse";
    case MemoryType::Backed:
        return "Backed";
    default:
        return "Invalid";
    }
}

[[nodiscard]] VkMemoryPropertyFlags MemoryUsagePreferredVmaFlags(MemoryType type) {
    return type != MemoryType::DeviceLocal ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                           : VkMemoryPropertyFlagBits{};
}

[[nodiscard]] VmaAllocationCreateFlags MemoryUsageVmaFlags(MemoryType type) {
    switch (type) {
    case MemoryType::HostUncached:
    case MemoryType::Stream:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT |
               VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    case MemoryType::HostCached:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    case MemoryType::DeviceLocal:
    default:
        return {};
    }
}

[[nodiscard]] VmaMemoryUsage MemoryUsageVma(MemoryType type) {
    switch (type) {
    case MemoryType::DeviceLocal:
    case MemoryType::Stream:
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    case MemoryType::HostUncached:
    case MemoryType::HostCached:
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    default:
        return VMA_MEMORY_USAGE_UNKNOWN;
    }
}

namespace {
constexpr u32 MaxBufferKinds = 64;
std::mutex buffer_kinds_mutex;
std::vector<std::string> buffer_kind_names{"other"};
std::array<std::atomic<u64>, MaxBufferKinds> buffer_kind_bytes{};
std::array<std::atomic<u32>, MaxBufferKinds> buffer_kind_count{};

void CountBuffer(u32 kind, u64 bytes, bool add) {
    if (add) {
        buffer_kind_bytes[kind].fetch_add(bytes, std::memory_order_relaxed);
        buffer_kind_count[kind].fetch_add(1, std::memory_order_relaxed);
    } else {
        buffer_kind_bytes[kind].fetch_sub(bytes, std::memory_order_relaxed);
        buffer_kind_count[kind].fetch_sub(1, std::memory_order_relaxed);
    }
}
} // namespace

u32 BufferKind(std::string_view name) {
    // "Buffer 0x...:0x..." and "StreamBuffer(...):0x...": the part before the address.
    if (const auto at = name.find(":0x"); at != std::string_view::npos) {
        name = name.substr(0, at);
    }
    if (const auto at = name.find(" 0x"); at != std::string_view::npos) {
        name = name.substr(0, at);
    }
    std::scoped_lock lk{buffer_kinds_mutex};
    for (u32 i = 0; i < buffer_kind_names.size(); ++i) {
        if (buffer_kind_names[i] == name) {
            return i;
        }
    }
    if (buffer_kind_names.size() == MaxBufferKinds) {
        return 0;
    }
    buffer_kind_names.emplace_back(name);
    return static_cast<u32>(buffer_kind_names.size() - 1);
}

std::string DescribeBufferKinds() {
    std::vector<std::pair<u64, u32>> kinds;
    {
        std::scoped_lock lk{buffer_kinds_mutex};
        for (u32 i = 0; i < buffer_kind_names.size(); ++i) {
            const u64 bytes = buffer_kind_bytes[i].load(std::memory_order_relaxed);
            if (bytes >= (1ull << 20)) {
                kinds.emplace_back(bytes, i);
            }
        }
    }
    std::sort(kinds.begin(), kinds.end(), std::greater<>{});
    std::string out;
    u64 total = 0;
    for (const auto& [bytes, kind] : kinds) {
        total += bytes;
        std::scoped_lock lk{buffer_kinds_mutex};
        out += fmt::format("{}{} {} MiB ({})", out.empty() ? "" : ", ", buffer_kind_names[kind],
                           bytes >> 20, buffer_kind_count[kind].load(std::memory_order_relaxed));
    }
    return fmt::format("{} MiB: {}", total >> 20, out);
}

UniqueBuffer::UniqueBuffer(vk::Device device_, VmaAllocator allocator_)
    : device{device_}, allocator{allocator_} {}

UniqueBuffer::~UniqueBuffer() {
    Destroy();
}

void UniqueBuffer::Destroy() {
    if (stats_bytes) {
        CountBuffer(stats_kind, stats_bytes, false);
        stats_bytes = 0;
    }
    if (allocation) {
        vmaDestroyBuffer(allocator, buffer, allocation);
    } else if (buffer) {
        device.destroyBuffer(buffer);
    }
    if (memory) {
        // bbport MemoryType::Backed: the buffer's own memory goes with it.
        device.freeMemory(memory);
        BbStats::device_free_bytes.fetch_add(memory_size, std::memory_order_relaxed);
    }
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    memory_size = 0;
    bda_addr = 0;
}

void UniqueBuffer::Create(vk::BufferCreateInfo& buffer_ci, MemoryType mem_type,
                          VmaAllocationInfo* out_alloc_info) {
    const bool with_bda = bool(buffer_ci.usage & vk::BufferUsageFlagBits::eShaderDeviceAddress);
    if (mem_type != MemoryType::Sparse && mem_type != MemoryType::Backed) {
        const VmaAllocationCreateFlags bda_flag =
            with_bda ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
        const VmaAllocationCreateInfo alloc_ci = {
            .flags =
                VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | bda_flag | MemoryUsageVmaFlags(mem_type),
            .usage = MemoryUsageVma(mem_type),
            .requiredFlags = 0,
            .preferredFlags = MemoryUsagePreferredVmaFlags(mem_type),
            .pool = VK_NULL_HANDLE,
            .pUserData = nullptr,
        };

        const VkBufferCreateInfo buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
        VkBuffer unsafe_buffer{};
        VkResult result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &alloc_ci, &unsafe_buffer,
                                          &allocation, out_alloc_info);
        ASSERT_MSG(result == VK_SUCCESS, "Failed allocating buffer with error {}",
                   vk::to_string(vk::Result{result}));
        buffer = vk::Buffer{unsafe_buffer};
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(allocator, allocation, &info);
        stats_bytes = info.size;
        CountBuffer(stats_kind, stats_bytes, true);
    } else if (mem_type == MemoryType::Backed) {
        // bbport: an arena without sparse binding (BufferCache::NonSparseArenas): a plain buffer
        // with one dedicated memory of its full size, allocated and bound here. Device-local
        // memory first (Mali is UMA: the same memory is host-visible), then any type it accepts.
        const vk::Result created = device.createBuffer(&buffer_ci, nullptr, &buffer);
        ASSERT_MSG(created == vk::Result::eSuccess, "Failed to create a {} MiB arena buffer: {}",
                   buffer_ci.size >> 20, vk::to_string(created));
        const auto reqs = device.getBufferMemoryRequirements(buffer);
        const VkPhysicalDeviceMemoryProperties* props = nullptr;
        vmaGetMemoryProperties(allocator, &props);
        std::array<u32, VK_MAX_MEMORY_TYPES> candidates{};
        u32 candidate_count = 0;
        for (const bool want_device_local : {true, false}) {
            for (u32 i = 0; i < props->memoryTypeCount; ++i) {
                const bool local = props->memoryTypes[i].propertyFlags &
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                if ((reqs.memoryTypeBits & (1u << i)) && local == want_device_local) {
                    candidates[candidate_count++] = i;
                }
            }
        }
        const vk::MemoryAllocateFlagsInfo flags_info{
            .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
        };
        vk::Result allocated = vk::Result::eErrorOutOfDeviceMemory;
        for (u32 i = 0; i < candidate_count && !memory; ++i) {
            const vk::MemoryAllocateInfo allocate_info{
                .pNext = with_bda ? &flags_info : nullptr,
                .allocationSize = reqs.size,
                .memoryTypeIndex = candidates[i],
            };
            allocated = device.allocateMemory(&allocate_info, nullptr, &memory);
            if (allocated != vk::Result::eSuccess) {
                memory = VK_NULL_HANDLE;
            }
        }
        ASSERT_MSG(memory, "Out of memory for a {} MiB buffer cache arena ({} memory types tried): {}",
                   reqs.size >> 20, candidate_count, vk::to_string(allocated));
        memory_size = reqs.size;
        BbStats::device_alloc_bytes.fetch_add(memory_size, std::memory_order_relaxed);
        const vk::Result bound = device.bindBufferMemory(buffer, memory, 0);
        ASSERT_MSG(bound == vk::Result::eSuccess, "Binding the memory of a {} MiB arena failed: {}",
                   reqs.size >> 20, vk::to_string(bound));
    } else {
        buffer_ci.flags |=
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency;
        // Bound to exported guest memory chunks (BB_GUEST_IN_PLACE) as well as VRAM blocks.
        const vk::ExternalMemoryBufferCreateInfo external{
            .handleTypes = BbGuestMemory::HandleType(),
        };
        if (GuestInPlace()) {
            buffer_ci.pNext = &external;
            if (const auto [result, created] = device.createBuffer(buffer_ci);
                result == vk::Result::eSuccess) {
                buffer = created;
                // bbport: VRAM blocks are bound into it too: the external memory type must not
                // keep device-local memory out (a driver may restrict it).
                const auto reqs = device.getBufferMemoryRequirements(created);
                const VkPhysicalDeviceMemoryProperties* props = nullptr;
                vmaGetMemoryProperties(allocator, &props);
                bool vram = false;
                for (u32 i = 0; i < props->memoryTypeCount; ++i) {
                    vram |= (reqs.memoryTypeBits & (1u << i)) &&
                            (props->memoryTypes[i].propertyFlags &
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                }
                if (!vram) {
                    std::printf("Guest memory: the arena for guest memory cannot take VRAM; the GPU "
                                "uses copies in VRAM as before\n");
                    device.destroyBuffer(created);
                    buffer = vk::Buffer{};
                    DisableGuestInPlace();
                }
            } else {
                std::printf("Guest memory: the arena cannot take exported memory (%s); the GPU "
                            "uses copies in VRAM as before\n",
                            vk::to_string(result).c_str());
                DisableGuestInPlace();
            }
            buffer_ci.pNext = nullptr;
        }
        if (!buffer) {
            const auto created = device.createBuffer(buffer_ci);
            if (created.result != vk::Result::eSuccess) {
                std::printf("Sparse buffer of %llu bytes failed: %s\n",
                            static_cast<unsigned long long>(buffer_ci.size),
                            vk::to_string(created.result).c_str());
            }
            buffer = Vulkan::Check(created);
        }
    }

    if (with_bda) {
        const vk::BufferDeviceAddressInfo bda_info = {
            .buffer = buffer,
        };
        auto bda_result = device.getBufferAddress(bda_info);
        ASSERT_MSG(bda_result != 0, "Failed to get buffer device address");
        bda_addr = bda_result;
    }
}

Buffer::Buffer(const Vulkan::Instance& instance, u64 size_bytes_, vk::DeviceMemory memory,
               std::string_view debug_name)
    : size_bytes{size_bytes_}, mem_type{MemoryType::HostCached},
      buffer{instance.GetDevice(), instance.GetAllocator()} {
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = BbGuestMemory::HandleType(),
    };
    // bbport BB_LAYER_MEMORY: what the GPU binds for the game's memory in place (no arena):
    // vertex, index and indirect data and device addresses too (the memory has them).
    const bool layer = BbGuestMemory::LayerMemory();
    vk::BufferUsageFlags usage =
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eUniformBuffer |
        vk::BufferUsageFlagBits::eUniformTexelBuffer | vk::BufferUsageFlagBits::eStorageTexelBuffer;
    if (layer) {
        usage |= vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
                 vk::BufferUsageFlagBits::eIndirectBuffer |
                 vk::BufferUsageFlagBits::eShaderDeviceAddress;
    }
    const vk::BufferCreateInfo buffer_ci = {
        .pNext = &external,
        .size = size_bytes,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const auto device = instance.GetDevice();
    buffer.buffer = Vulkan::Check(device.createBuffer(buffer_ci));
    const auto result = device.bindBufferMemory(buffer.buffer, memory, 0);
    ASSERT_MSG(result == vk::Result::eSuccess, "Binding a guest memory chunk failed: {}",
               vk::to_string(result));
    if (layer) {
        buffer.bda_addr = device.getBufferAddress({.buffer = buffer.buffer});
    }
    Vulkan::SetObjectName(device, Handle(), debug_name);
    is_coherent = true;
}

Buffer::Buffer(const Vulkan::Instance& instance, VAddr cpu_addr_, u64 size_bytes_,
               MemoryType mem_type_, std::string_view debug_name)
    : cpu_addr{cpu_addr_}, size_bytes{size_bytes_}, mem_type{mem_type_},
      buffer{instance.GetDevice(), instance.GetAllocator()} {

    vk::BufferCreateInfo buffer_ci = {
        .size = size_bytes,
        .usage = AllFlags,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    // bbport: arenas are also read by the readback queue (BufferCache::DownloadMemory).
    const std::array<u32, 2> families = {instance.GetGraphicsQueueFamilyIndex(),
                                         instance.GetReadbackQueueFamilyIndex()};
    if ((mem_type == MemoryType::Sparse || mem_type == MemoryType::Backed) &&
        instance.GetReadbackQueue()) {
        buffer_ci.sharingMode = vk::SharingMode::eConcurrent;
        buffer_ci.queueFamilyIndexCount = static_cast<u32>(families.size());
        buffer_ci.pQueueFamilyIndices = families.data();
    }
    VmaAllocationInfo alloc_info{};
    buffer.stats_kind = BufferKind(
        debug_name.empty() ? fmt::format("unnamed {}", BufferTypeName(mem_type)) : debug_name);
    buffer.Create(buffer_ci, mem_type, &alloc_info);

    const auto device = instance.GetDevice();
    if (!debug_name.empty()) {
        Vulkan::SetObjectName(device, Handle(), debug_name);
    } else {
        Vulkan::SetObjectName(device, Handle(), "Buffer {:#x}:{:#x}", cpu_addr, size_bytes);
    }

    if (mem_type != MemoryType::Sparse && mem_type != MemoryType::Backed) {
        VkMemoryPropertyFlags property_flags{};
        vmaGetAllocationMemoryProperties(instance.GetAllocator(), buffer.allocation,
                                         &property_flags);
        if (alloc_info.pMappedData) {
            mapped_data = std::span<u8>{std::bit_cast<u8*>(alloc_info.pMappedData), size_bytes};
        }
        is_coherent = property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
}

void Buffer::Flush(u64 offset, u64 size) {
    if (mapped_data.empty() || is_coherent) {
        return;
    }
    vmaFlushAllocation(buffer.allocator, buffer.allocation, offset, size);
}

void Buffer::Invalidate(u64 offset, u64 size) {
    if (mapped_data.empty() || is_coherent) {
        return;
    }
    vmaInvalidateAllocation(buffer.allocator, buffer.allocation, offset, size);
}

constexpr u64 WATCHES_INITIAL_RESERVE = 0x100;
constexpr u64 WATCHES_RESERVE_CHUNK = 0x100;

StreamBuffer::StreamBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler_,
                           MemoryType mem_type, u64 size_bytes)
    : Buffer{instance, 0, size_bytes, mem_type}, scheduler{scheduler_},
      non_coherent_atom_size{instance.NonCoherentAtomSize()} {
    ReserveWatches(current_watches, WATCHES_INITIAL_RESERVE);
    ReserveWatches(previous_watches, WATCHES_INITIAL_RESERVE);
    Vulkan::SetObjectName(instance.GetDevice(), Handle(), "StreamBuffer({}):{:#x}",
                          BufferTypeName(mem_type), size_bytes);
}

bool StreamBuffer::PrepareMap(u64 size, u64 alignment, bool allow_wait) {
    if (!mapped_data.empty() && !is_coherent) {
        size = Common::AlignUp(size, non_coherent_atom_size);
        alignment =
            alignment > 0 ? std::lcm(alignment, non_coherent_atom_size) : non_coherent_atom_size;
    }

    if (size > this->size_bytes) {
        return false;
    }

    if (alignment > 0) {
        offset = Common::AlignUp(offset, alignment);
    }

    if (offset + size > this->size_bytes) {
        // The buffer would overflow, save the amount of used watches and reset the state.
        invalidation_mark = current_watch_cursor;
        current_watch_cursor = 0;
        offset = 0;

        // Swap watches and reset waiting cursors.
        std::swap(previous_watches, current_watches);
        wait_cursor = 0;
        wait_bound = 0;
    }

    if (!WaitPendingOperations(offset + size, allow_wait)) {
        return false;
    }

    mapped_size = size;
    return true;
}

std::pair<u8*, u64> StreamBuffer::Map(u64 size, u64 alignment, bool allow_wait) {
    if (!PrepareMap(size, alignment, allow_wait)) {
        return {nullptr, 0};
    }
    u8* const data = mapped_data.empty() ? nullptr : mapped_data.data() + offset;
    return {data, offset};
}

void StreamBuffer::Commit() {
    if (mem_type == MemoryType::HostCached) {
        Invalidate(offset, mapped_size);
    } else {
        Flush(offset, mapped_size);
    }
    AdvanceAndWatch();
}

std::optional<u64> StreamBuffer::Reserve(u64 size, u64 alignment, bool allow_wait) {
    if (!PrepareMap(size, alignment, allow_wait)) {
        return std::nullopt;
    }
    const u64 reserved_offset = offset;
    AdvanceAndWatch();
    return reserved_offset;
}

void StreamBuffer::AdvanceAndWatch() {
    offset += mapped_size;
    const u64 tick = scheduler.CurrentTick();
    last_tick = tick;

    // Extend the last watch if it belongs to the same tick.
    if (current_watch_cursor != 0 && current_watches[current_watch_cursor - 1].tick == tick) {
        current_watches[current_watch_cursor - 1].upper_bound = offset;
        return;
    }

    if (current_watch_cursor + 1 >= current_watches.size()) {
        // Ensure that there are enough watches.
        ReserveWatches(current_watches, WATCHES_RESERVE_CHUNK);
    }

    auto& watch = current_watches[current_watch_cursor++];
    watch.upper_bound = offset;
    watch.tick = tick;
}

void StreamBuffer::ReserveWatches(std::vector<Watch>& watches, std::size_t grow_size) {
    watches.resize(watches.size() + grow_size);
}

bool StreamBuffer::WaitPendingOperations(u64 requested_upper_bound, bool allow_wait) {
    if (!invalidation_mark) {
        return true;
    }
    while (requested_upper_bound > wait_bound && wait_cursor < *invalidation_mark) {
        auto& watch = previous_watches[wait_cursor];
        if (!scheduler.IsFree(watch.tick) && !allow_wait) {
            return false;
        }
        scheduler.Wait(watch.tick);
        wait_bound = watch.upper_bound;
        ++wait_cursor;
    }
    return true;
}

} // namespace VideoCore
