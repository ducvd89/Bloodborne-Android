// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_guest_memory.h"

#include <array>
#include <chrono>
#include <string>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <csignal>
#include <mutex>
#include <string_view>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "bblayer_gpu_memory.h"

extern "C" void runtime_memory_set_guest_chunk_allocator(int (*alloc)(uint64_t phys,
                                                                      uint64_t size));
extern "C" void runtime_memory_set_guest_chunk_whole(int whole);
extern "C" void* runtime_memory_backing_pointer(uint64_t phys);

namespace BbGuestMemory {
namespace {
using u64 = std::uint64_t;
constexpr std::size_t MaxChunks = 256;

vk::Device device;
vk::PhysicalDeviceMemoryProperties memory_properties;
std::mutex mutex;
std::array<Chunk*, MaxChunks> chunks{}; // sorted by phys
std::size_t chunk_count = 0;
bool whole_only = false; // the driver's dma-buf maps at offset 0 only: a chunk per allocation
enum class Mode { DmaBuf, HostImport };
Mode mode = Mode::DmaBuf;
std::atomic<u64> chunk_bytes{0};
bool unified_mode = false;      // BB_UNIFIED: set in Usable() before the probes, cleared if one fails
bool unified_noncoherent = false; // the chosen type is HOST_CACHED without HOST_COHERENT
std::uint32_t unified_type = ~0u;
std::string fallback_reason;

bool GuestInVram() {
    // bbport BB_GUEST_VRAM=1 (experiment): the game's direct memory in VRAM the CPU maps through
    // the PCI BAR (Resizable BAR), as GDDR is the PS4's one memory: no VRAM copies.
    static const bool on = [] {
        const char* env = std::getenv("BB_GUEST_VRAM");
        return env && env[0] == '1';
    }();
    return on;
}

/// Unified memory: a host-visible type the CPU maps cached (the game's code runs on it), device-local
/// or not (an integrated GPU's memory is all of it); cached and coherent first, else cached only
/// (explicit cache maintenance would be needed at handoffs, see UnifiedNeedsSync). Never a
/// write-combined (uncached) type: the game would crawl on it.
std::uint32_t PickUnifiedType(const vk::PhysicalDeviceMemoryProperties& props, std::uint32_t bits,
                              bool* coherent) {
    const auto cached = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached;
    const auto both = cached | vk::MemoryPropertyFlagBits::eHostCoherent;
    for (const auto want : {both, cached}) {
        for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) {
                if (coherent) {
                    *coherent = (want & vk::MemoryPropertyFlagBits::eHostCoherent) ==
                                vk::MemoryPropertyFlagBits::eHostCoherent;
                }
                return i;
            }
        }
    }
    return ~0u;
}

std::uint32_t FindType(std::uint32_t bits) {
    if (unified_mode) {
        return PickUnifiedType(memory_properties, bits, nullptr);
    }
    if (GuestInVram()) {
        const auto want = vk::MemoryPropertyFlagBits::eHostVisible |
                          vk::MemoryPropertyFlagBits::eDeviceLocal;
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & want) == want) {
                return i;
            }
        }
    }
    const auto want = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached;
    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        const auto flags = memory_properties.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (flags & want) == want &&
            !(flags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            return i;
        }
    }
    return ~0u;
}

/// Uses of a chunk's buffer. With the layer's memory module the chunk buffers are what the GPU
/// binds (no sparse arena): every buffer use, texel buffers and a device address too.
vk::BufferUsageFlags ChunkBufferUsage() {
    vk::BufferUsageFlags usage =
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eUniformBuffer |
        vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
        vk::BufferUsageFlagBits::eIndirectBuffer;
    if (LayerMemory()) {
        usage |= vk::BufferUsageFlagBits::eUniformTexelBuffer |
                 vk::BufferUsageFlagBits::eStorageTexelBuffer |
                 vk::BufferUsageFlagBits::eShaderDeviceAddress;
    }
    return usage;
}

/// A new chunk becomes a source of the layer's memory module.
void RegisterChunk(Chunk* chunk) {
    if (!LayerMemory()) {
        return;
    }
    const vk::DeviceAddress address = device.getBufferAddress({.buffer = chunk->buffer});
    BbLayer::GpuMemory::Get().AddGuestSource(chunk->phys, chunk->size, chunk->buffer, address,
                                             chunk);
}

/// The runtime's chunk allocator: a dma-buf fd of `size` bytes of GPU-visible memory, or -1.
int AllocChunk(u64 phys, u64 size) {
    {
        std::scoped_lock lk{mutex};
        if (chunk_count == MaxChunks) {
            return -1;
        }
    }
    static std::atomic<int> failures{0};
    const auto fail = [&](const char* what, vk::Result result) {
        if (failures.fetch_add(1) < 4) {
            std::fprintf(stderr, "Guest memory: %s failed (%s); chunk %#llx stays in the memfd\n",
                         what, vk::to_string(result).c_str(), (unsigned long long)phys);
        }
        return -1;
    };
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
    };
    const vk::BufferCreateInfo buffer_ci{
        .pNext = &external,
        .size = size,
        .usage = ChunkBufferUsage(),
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const auto [buffer_result, buffer] = device.createBuffer(buffer_ci);
    if (buffer_result != vk::Result::eSuccess) {
        return fail("buffer creation", buffer_result);
    }
    const auto requirements = device.getBufferMemoryRequirements(buffer);
    const std::uint32_t type = FindType(requirements.memoryTypeBits);
    if (type == ~0u) {
        device.destroyBuffer(buffer);
        return fail("finding a cached host-visible memory type", vk::Result::eErrorFeatureNotPresent);
    }
    const vk::MemoryAllocateFlagsInfo address_flags{
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    const vk::ExportMemoryAllocateInfo export_info{
        .pNext = LayerMemory() ? &address_flags : nullptr,
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
    };
    const auto [memory_result, memory] = device.allocateMemory({
        .pNext = &export_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    });
    if (memory_result != vk::Result::eSuccess) {
        device.destroyBuffer(buffer);
        return fail("allocation", memory_result);
    }
    if (const auto result = device.bindBufferMemory(buffer, memory, 0);
        result != vk::Result::eSuccess) {
        device.freeMemory(memory);
        device.destroyBuffer(buffer);
        return fail("binding", result);
    }
    const auto [fd_result, fd] = device.getMemoryFdKHR({
        .memory = memory,
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
    });
    if (fd_result != vk::Result::eSuccess) {
        device.freeMemory(memory);
        device.destroyBuffer(buffer);
        return fail("dma-buf export", fd_result);
    }
    {
        std::scoped_lock lk{mutex};
        std::size_t i = chunk_count++;
        for (; i > 0 && chunks[i - 1]->phys > phys; --i) {
            chunks[i] = chunks[i - 1];
        }
        static std::uint32_t next_index = 0;
        chunks[i] = new Chunk{phys, size, buffer, memory, next_index++};
        RegisterChunk(chunks[i]);
    }
    const u64 total = chunk_bytes.fetch_add(size) + size;
    std::printf("Guest memory: direct memory %#llx+%llu MiB in GPU-visible memory (dma-buf%s, memory "
                "type %u), %llu MiB so far\n",
                (unsigned long long)phys, (unsigned long long)(size >> 20),
                unified_mode ? ", UNIFIED MEMORY: shared with the CPU, no GPU copy" : "", type,
                (unsigned long long)(total >> 20));
    return fd;
}

/// Host memory import (VK_EXT_external_memory_host): the chunk stays in the runtime's memfd, the
/// GPU imports its backing view. CPU access is ordinary cached memory; the GPU reads and writes it
/// over the bus, as a PC game's upload heap. For drivers whose dma-buf does not fit (NVIDIA: maps
/// at offset 0 only, CPU access through it may be uncached).
vk::DeviceSize import_alignment = 4096;

std::uint32_t FindHostType(std::uint32_t bits) {
    // Cached host memory first, then any host-visible type the import allows.
    for (const bool cached : {true, false}) {
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            const auto flags = memory_properties.memoryTypes[i].propertyFlags;
            if (!(bits & (1u << i)) || !(flags & vk::MemoryPropertyFlagBits::eHostVisible) ||
                (flags & vk::MemoryPropertyFlagBits::eDeviceLocal) ||
                (cached && !(flags & vk::MemoryPropertyFlagBits::eHostCached))) {
                continue;
            }
            return i;
        }
    }
    return ~0u;
}

constexpr vk::BufferUsageFlags ChunkUsage =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eUniformBuffer |
    vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer;

/// Imports [pointer, pointer + size) as device memory with a buffer over it; false on failure.
bool ImportHost(void* pointer, u64 size, vk::Buffer& buffer, vk::DeviceMemory& memory,
                vk::Result& error) {
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
    };
    const auto [buffer_result, created] = device.createBuffer({
        .pNext = &external,
        .size = size,
        .usage = ChunkBufferUsage(),
        .sharingMode = vk::SharingMode::eExclusive,
    });
    if (buffer_result != vk::Result::eSuccess) {
        error = buffer_result;
        return false;
    }
    const auto [props_result, props] = device.getMemoryHostPointerPropertiesEXT(
        vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, pointer);
    const auto requirements = device.getBufferMemoryRequirements(created);
    const std::uint32_t type = props_result == vk::Result::eSuccess
                                   ? FindHostType(props.memoryTypeBits & requirements.memoryTypeBits)
                                   : ~0u;
    if (type == ~0u) {
        device.destroyBuffer(created);
        error = props_result != vk::Result::eSuccess ? props_result
                                                     : vk::Result::eErrorFeatureNotPresent;
        return false;
    }
    const vk::MemoryAllocateFlagsInfo address_flags{
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    const vk::ImportMemoryHostPointerInfoEXT import_info{
        .pNext = LayerMemory() ? &address_flags : nullptr,
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
        .pHostPointer = pointer,
    };
    const auto [memory_result, allocated] = device.allocateMemory({
        .pNext = &import_info,
        .allocationSize = size,
        .memoryTypeIndex = type,
    });
    if (memory_result != vk::Result::eSuccess) {
        device.destroyBuffer(created);
        error = memory_result;
        return false;
    }
    if (const auto result = device.bindBufferMemory(created, allocated, 0);
        result != vk::Result::eSuccess) {
        device.freeMemory(allocated);
        device.destroyBuffer(created);
        error = result;
        return false;
    }
    buffer = created;
    memory = allocated;
    return true;
}

/// The runtime's chunk allocator in host import mode: CHUNK_IMPORTED (-3, the chunk stays in the
/// memfd) once its backing view is imported, or -1.
int AllocImported(u64 phys, u64 size) {
    {
        std::scoped_lock lk{mutex};
        if (chunk_count == MaxChunks) {
            return -1;
        }
    }
    void* pointer = runtime_memory_backing_pointer(phys);
    if (!pointer || reinterpret_cast<std::uintptr_t>(pointer) % import_alignment != 0 ||
        size % import_alignment != 0) {
        return -1;
    }
    vk::Buffer buffer;
    vk::DeviceMemory memory;
    vk::Result error{};
    if (!ImportHost(pointer, size, buffer, memory, error)) {
        static std::atomic<int> failures{0};
        if (failures.fetch_add(1) < 4) {
            std::fprintf(stderr, "Guest memory: importing chunk %#llx failed (%s); it stays in the "
                                 "memfd\n",
                         (unsigned long long)phys, vk::to_string(error).c_str());
        }
        return -1;
    }
    {
        std::scoped_lock lk{mutex};
        std::size_t i = chunk_count++;
        for (; i > 0 && chunks[i - 1]->phys > phys; --i) {
            chunks[i] = chunks[i - 1];
        }
        static std::uint32_t next_index = 0;
        chunks[i] = new Chunk{phys, size, buffer, memory, next_index++};
        RegisterChunk(chunks[i]);
    }
    const u64 total = chunk_bytes.fetch_add(size) + size;
    std::printf("Guest memory: direct memory %#llx+%llu MiB imported by the GPU (host memory), "
                "%llu MiB so far\n",
                (unsigned long long)phys, (unsigned long long)(size >> 20),
                (unsigned long long)(total >> 20));
    return -3;
}

/// Runs `record` on the graphics queue and waits; false on any failure.
template <typename Record>
bool RunOnce(const Vulkan::Instance& instance, Record&& record) {
    const vk::Device dev = instance.GetDevice();
    const auto [pool_result, pool] = dev.createCommandPool(
        {.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex()});
    if (pool_result != vk::Result::eSuccess) {
        return false;
    }
    bool ok = false;
    const auto [alloc_result, cmdbufs] = dev.allocateCommandBuffers(
        {.commandPool = pool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1});
    const auto [fence_result, fence] = dev.createFence({});
    if (alloc_result == vk::Result::eSuccess && fence_result == vk::Result::eSuccess) {
        const vk::CommandBuffer cmd = cmdbufs[0];
        if (cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}) ==
            vk::Result::eSuccess) {
            record(cmd);
            const vk::MemoryBarrier to_host{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                            .dstAccessMask = vk::AccessFlagBits::eHostRead};
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                vk::PipelineStageFlagBits::eHost, {}, to_host, {}, {});
            if (cmd.end() == vk::Result::eSuccess) {
                const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd};
                ok = instance.GetGraphicsQueue().submit(submit, fence) == vk::Result::eSuccess &&
                     dev.waitForFences(fence, true, 5'000'000'000ull) == vk::Result::eSuccess;
            }
        }
    }
    if (fence_result == vk::Result::eSuccess) {
        dev.destroyFence(fence);
    }
    dev.destroyCommandPool(pool);
    return ok;
}

/// Host import as the PC model uses it, on a small memfd mapped twice (the guest's view and the
/// backing view): imported through one view, bound into a sparse buffer like the arena, written by
/// the GPU and read by the CPU through the other view, and the other way round.
bool HostImportWorks(const Vulkan::Instance& instance, const char*& why) {
    why = "VK_EXT_external_memory_host unavailable";
    if (!instance.IsHostMemoryImportSupported()) {
        return false;
    }
    const vk::Device dev = instance.GetDevice();
    vk::PhysicalDeviceExternalMemoryHostPropertiesEXT host_props{};
    vk::PhysicalDeviceProperties2 props2{.pNext = &host_props};
    instance.GetPhysicalDevice().getProperties2(&props2);
    import_alignment = std::max<vk::DeviceSize>(host_props.minImportedHostPointerAlignment, 4096);
    constexpr u64 Size = 4 << 20;
    if (Size % import_alignment != 0) {
        why = "import alignment above 4 MiB";
        return false;
    }
    const int fd = memfd_create("bb-guest-memory-probe", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, Size) != 0) {
        why = "memfd";
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    auto* view = static_cast<std::uint32_t*>(
        mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    auto* guest = static_cast<std::uint32_t*>(
        mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    close(fd);
    bool ok = false;
    vk::Buffer buffer, sparse;
    vk::DeviceMemory memory;
    vk::Result error{};
    if (view == MAP_FAILED || guest == MAP_FAILED) {
        why = "mmap of the memfd";
    } else if (!ImportHost(view, Size, buffer, memory, error)) {
        static std::string text;
        text = "the import of a memfd mapping failed (" + vk::to_string(error) + ")";
        why = text.c_str();
    } else if (LayerMemory()) {
        // The layer's memory module binds the imported buffers themselves (no sparse arena): the
        // same GPU and CPU writes through the imported buffer.
        guest[1024] = 0x5ca1ab1e;
        const bool ran = RunOnce(instance, [&](vk::CommandBuffer cmd) {
            cmd.fillBuffer(buffer, 0, 4096, 0x600dcafe);
            cmd.copyBuffer(buffer, buffer, vk::BufferCopy{4096, 8192, 4});
        });
        why = "GPU work on imported memory";
        if (ran) {
            const bool gpu_to_cpu = guest[0] == 0x600dcafe && guest[1023] == 0x600dcafe;
            const bool cpu_to_gpu = guest[2048] == 0x5ca1ab1e;
            why = !gpu_to_cpu ? "GPU writes not seen by the CPU" : "CPU writes not seen by the GPU";
            ok = gpu_to_cpu && cpu_to_gpu;
        }
    } else {
        const vk::ExternalMemoryBufferCreateInfo external{
            .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
        };
        const auto [sparse_result, created] = dev.createBuffer({
            .pNext = &external,
            .flags = vk::BufferCreateFlagBits::eSparseBinding |
                     vk::BufferCreateFlagBits::eSparseResidency,
            .size = Size,
            .usage = ChunkUsage,
            .sharingMode = vk::SharingMode::eExclusive,
        });
        why = "a sparse buffer for imported memory";
        // The arena also binds VRAM blocks: the buffer must take device-local memory too.
        bool vram_ok = false;
        if (sparse_result == vk::Result::eSuccess) {
            const auto reqs = dev.getBufferMemoryRequirements(created);
            for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
                vram_ok |= (reqs.memoryTypeBits & (1u << i)) &&
                           (memory_properties.memoryTypes[i].propertyFlags &
                            vk::MemoryPropertyFlagBits::eDeviceLocal);
            }
            if (!vram_ok) {
                why = "a sparse buffer for imported memory cannot take VRAM";
                dev.destroyBuffer(created);
            }
        }
        if (sparse_result == vk::Result::eSuccess && vram_ok) {
            sparse = created;
            const vk::SparseMemoryBind bind{.resourceOffset = 0, .size = Size, .memory = memory,
                                            .memoryOffset = 0};
            const vk::SparseBufferMemoryBindInfo buffer_bind{.buffer = sparse, .bindCount = 1,
                                                             .pBinds = &bind};
            const auto [fence_result, fence] = dev.createFence({});
            why = "binding imported memory into a sparse buffer";
            if (fence_result == vk::Result::eSuccess &&
                instance.GetGraphicsQueue().bindSparse(
                    vk::BindSparseInfo{.bufferBindCount = 1, .pBufferBinds = &buffer_bind},
                    fence) == vk::Result::eSuccess &&
                dev.waitForFences(fence, true, 5'000'000'000ull) == vk::Result::eSuccess) {
                // GPU writes through the sparse buffer, CPU reads through the guest view; CPU
                // writes through the guest view, the GPU copies them through the sparse buffer.
                guest[1024] = 0x5ca1ab1e;
                const bool ran = RunOnce(instance, [&](vk::CommandBuffer cmd) {
                    cmd.fillBuffer(sparse, 0, 4096, 0x600dcafe);
                    cmd.copyBuffer(sparse, buffer, vk::BufferCopy{4096, 8192, 4});
                });
                why = "GPU work on imported memory";
                if (ran) {
                    const bool gpu_to_cpu = guest[0] == 0x600dcafe && guest[1023] == 0x600dcafe;
                    const bool cpu_to_gpu = guest[2048] == 0x5ca1ab1e;
                    why = !gpu_to_cpu ? "GPU writes not seen by the CPU"
                                      : "CPU writes not seen by the GPU";
                    ok = gpu_to_cpu && cpu_to_gpu;
                }
            }
            if (fence_result == vk::Result::eSuccess) {
                dev.destroyFence(fence);
            }
        }
    }
    if (sparse) {
        dev.destroyBuffer(sparse);
    }
    if (buffer) {
        dev.destroyBuffer(buffer);
    }
    if (memory) {
        dev.freeMemory(memory);
    }
    if (view != MAP_FAILED) {
        munmap(view, Size);
    }
    if (guest != MAP_FAILED) {
        munmap(guest, Size);
    }
    return ok;
}

/// CPU read speed through a dma-buf mapping, MB/s (the game's code runs on this memory: an
/// uncached mapping would slow it to a crawl).
double MappedReadSpeed(const volatile std::uint64_t* p, u64 size) {
    std::uint64_t sum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int pass = 0; pass < 4; ++pass) {
        for (u64 i = 0; i < size / 8; i += 8) {
            sum += p[i];
        }
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    static volatile std::uint64_t sink;
    sink = sum;
    return s > 0 ? 4.0 * size / s / 1e6 : 1e9;
}
} // namespace

namespace {
/// The GPU's choice of LayerMemory without BB_LAYER_MEMORY: -1 not known yet, 0 or 1.
std::atomic<int> layer_default{-1};
} // namespace

bool PcModelGpu(const Vulkan::Instance& instance) {
    static const bool ok = [&] {
        // The layer's memory module on every GPU; the startup checks in Usable() decide whether
        // the driver can do it. The sparse arena (BB_LAYER_MEMORY=0) is for AMD; not on NVIDIA
        // (only with BB_PC_MODEL_ANY_GPU=1):
        // its host memory import passes those checks, but each rebinding of the game's memory (vkQueueBindSparse, blocks moved to and from VRAM) held the GPU for 10 s
        // and more, the desktop frozen with it (GTX 1660 Ti, driver 615.71). The Linux driver's
        // sparse binding has slowed down since 555: the time grows with the pages already bound
        // (NVIDIA developer forum, "Sparse texture binding is painfully slow"), and the arena has
        // gigabytes of 64 KiB pages bound. Without rebinding (BB_FIXED_ARENA=1, all in place) the
        // GPU reads the game's data over the bus: 48 FPS against 210 on an RX 7800 XT.
        constexpr std::uint32_t AmdVendor = 0x1002, NvidiaVendor = 0x10de;
        const std::uint32_t vendor = instance.GetVendorID();
        const char* any = std::getenv("BB_PC_MODEL_ANY_GPU");
        // AMD keeps the sparse arena, every other GPU gets the layer's memory module (no sparse
        // binding of the game's memory, see LayerMemory); BB_LAYER_MEMORY=1/0 by hand. On an
        // RX 7800 XT the module was on par with the arena on a route in Yharnam, but in the
        // Hunter's Dream it kept ~1 GB of the area's data out of VRAM (ranges with a few
        // unannounced blocks stay in place without volatile blocks): 100 FPS against ~200.
        layer_default.store(vendor == AmdVendor ? 0 : 1, std::memory_order_relaxed);
        if (LayerMemory()) {
            std::printf("Guest memory: the new memory model through the layer's memory module on "
                        "this GPU (vendor 0x%04x): no sparse binding of the game's memory\n",
                        vendor);
        }
        if (vendor == AmdVendor || LayerMemory() || (any && any[0] == '1')) {
            return true;
        }
        std::printf("Guest memory: the new memory model is for AMD GPUs%s; this GPU (vendor "
                    "0x%04x) uses the model of 0.3 (BB_PC_MODEL_ANY_GPU=1: try it)\n",
                    vendor == NvidiaVendor ? " (NVIDIA: rebinding memory holds the GPU for seconds)"
                                           : "",
                    vendor);
        return false;
    }();
    return ok;
}

bool UnifiedRequested() {
    static const bool on = [] {
        const char* env = std::getenv("BB_UNIFIED");
        return env && env[0] == '1';
    }();
    return on;
}

bool UnifiedEligible(const Vulkan::Instance& instance) {
    static const bool eligible = [&] {
        if (!UnifiedRequested()) {
            return false;
        }
        constexpr std::uint32_t ArmVendor = 0x13b5;
        const char* why = nullptr;
        const char* layer = std::getenv("BB_LAYER_MEMORY");
        if (instance.GetVendorID() == ArmVendor ||
            instance.GetDriverID() == vk::DriverId::eMesaPanvk ||
            instance.GetDriverID() == vk::DriverId::eArmProprietary) {
            why = "Mali/PanVK always stays on copies";
        } else if (!instance.IsIntegrated()) {
            why = "the GPU is not integrated (its memory is not the CPU's)";
        } else if (layer && layer[0] == '0') {
            why = "BB_LAYER_MEMORY=0 (it needs the layer's memory module)";
        } else if (!instance.IsGuestMemoryExportSupported()) {
            why = "the driver cannot export memory as a dma-buf";
        }
        if (why) {
            fallback_reason = why;
            std::printf("Guest memory: unified memory (BB_UNIFIED=1) is NOT used: %s; the GPU uses "
                        "copies\n",
                        why);
            return false;
        }
        return true;
    }();
    return eligible;
}

bool Unified() {
    return unified_mode;
}

bool UnifiedNeedsSync() {
    return unified_mode && unified_noncoherent;
}

const char* FallbackReason() {
    return fallback_reason.c_str();
}

std::uint64_t ChunkBytes() {
    return chunk_bytes.load(std::memory_order_relaxed);
}

bool HostImported() {
    return mode == Mode::HostImport;
}

bool LayerMemory() {
    // BB_LAYER_MEMORY=1/0 chooses; else PcModelGpu (which asks first) sets the module for every GPU
    // (BB_LAYER_MEMORY=0: the sparse arena; NVIDIA's sparse binding stalls for seconds).
    static const bool on = [] {
        const char* env = std::getenv("BB_LAYER_MEMORY");
        if (env && *env) {
            return env[0] == '1';
        }
        if (UnifiedRequested()) {
            return true; // unified memory is the layer's memory module
        }
        return layer_default.load(std::memory_order_relaxed) == 1;
    }();
    return on;
}

namespace {
bool ProbesForced() {
    static const bool on = [] {
        const char* env = std::getenv("BB_GUEST_PROBES");
        return env && env[0] == '1';
    }();
    return on;
}

bool bind_ok(vk::Device dev, vk::Buffer buffer, vk::DeviceMemory memory) {
    if (!memory) {
        return false;
    }
    if (const auto result = dev.bindBufferMemory(buffer, memory, 0); result != vk::Result::eSuccess) {
        fallback_reason = "binding the probe buffer failed (" + vk::to_string(result) + ")";
        return false;
    }
    return true;
}

void ProbeLine(const char* name, bool pass, const char* detail = "") {
    std::printf("Guest memory probe: %-26s %s%s%s\n", name, pass ? "pass" : "FAIL",
                *detail ? " - " : "", detail);
    if (!pass && fallback_reason.empty()) {
        fallback_reason = std::string("probe '") + name + "' failed" + (*detail ? ": " : "") + detail;
    }
}

/// Write protection on a dma-buf mapping, the game's write tracking (BB_WRITE_TRACKING): a write to a
/// PROT_READ page must raise SIGSEGV. Done in a forked child that sets SIGSEGV/SIGBUS to the default
/// action first (the runtime's and FEX's handlers are untouched in this process: the child has its own
/// copy) and uses system calls only; the parent reads how it ended. Skipped (not a failure) where
/// fork is not allowed or the child cannot be waited for. BB_GUEST_PROBE_MPROTECT=0 skips it.
bool ProbeWriteProtect(void* mapping, const char*& detail) {
    const char* env = std::getenv("BB_GUEST_PROBE_MPROTECT");
    if (env && env[0] == '0') {
        detail = "skipped (BB_GUEST_PROBE_MPROTECT=0)";
        return true;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = fork();
    if (pid < 0) {
        detail = "skipped (fork not allowed)";
        return true;
    }
    if (pid == 0) {
        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGBUS, &sa, nullptr);
        const rlimit no_core{0, 0};
        setrlimit(RLIMIT_CORE, &no_core);
        auto* word = static_cast<volatile std::uint32_t*>(mapping);
        if (mprotect(mapping, 4096, PROT_READ) != 0) {
            _exit(10);
        }
        const std::uint32_t read = *word; // reads stay allowed
        (void)read;
        if (mprotect(mapping, 4096, PROT_READ | PROT_WRITE) != 0) {
            _exit(11);
        }
        *word = 1; // allowed again: must not fault
        if (mprotect(mapping, 4096, PROT_READ) != 0) {
            _exit(13);
        }
        *word = 2; // must raise SIGSEGV
        _exit(12);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != pid) {
        detail = "skipped (child could not be waited for)";
        return true;
    }
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV) {
        detail = "write to a PROT_READ page raised SIGSEGV; re-enabling write works";
        return true;
    }
    static std::string text;
    text = WIFSIGNALED(status) ? "child died with signal " + std::to_string(WTERMSIG(status))
           : WEXITSTATUS(status) == 12 ? "write to a PROT_READ page did not fault"
                                       : "mprotect failed (child exit " +
                                             std::to_string(WEXITSTATUS(status)) + ")";
    detail = text.c_str();
    return false;
}

/// The probes of unified memory on the exported buffer `buffer` (2 MiB, `whole` its CPU mapping):
/// 1 MiB halves, CPU lines dirty in the destination before the GPU writes it (a non-coherent mapping
/// shows stale or lost data). Logs one pass/fail line each.
bool RunGuestProbes(const Vulkan::Instance& instance, vk::Buffer buffer, void* whole, u64 size,
                    bool coherent) {
    const u64 half = size / 2;
    const u64 words = half / 4;
    auto* w = static_cast<volatile std::uint32_t*>(whole);
    bool all = true;

    // GPU write -> CPU read.
    for (u64 i = 0; i < words; ++i) {
        w[words + i] = 0xdeadbeefu;
    }
    bool ran = RunOnce(instance, [&](vk::CommandBuffer cmd) {
        cmd.fillBuffer(buffer, half, half, 0x600dcafeu);
    });
    u64 bad = 0;
    if (ran) {
        for (u64 i = 0; i < words; ++i) {
            bad += w[words + i] != 0x600dcafeu;
        }
    }
    bool pass = ran && bad == 0;
    ProbeLine("GPU write -> CPU read", pass,
              !ran ? "the GPU work failed" : bad ? "the CPU saw stale data (no coherence?)" : "");
    all &= pass;

    // CPU write -> GPU read (a GPU copy of what the CPU wrote, read back by the CPU).
    for (u64 i = 0; i < words; ++i) {
        w[i] = 0xa5000000u + std::uint32_t(i);
        w[words + i] = 0;
    }
    ran = RunOnce(instance, [&](vk::CommandBuffer cmd) {
        cmd.copyBuffer(buffer, buffer, vk::BufferCopy{0, half, half});
    });
    bad = 0;
    if (ran) {
        for (u64 i = 0; i < words; ++i) {
            bad += w[words + i] != 0xa5000000u + std::uint32_t(i);
        }
    }
    pass = ran && bad == 0;
    ProbeLine("CPU write -> GPU read", pass,
              !ran ? "the GPU work failed" : bad ? "the GPU read stale data (no coherence?)" : "");
    all &= pass;

    const char* detail = "";
    pass = ProbeWriteProtect(whole, detail);
    ProbeLine("mprotect + write -> SIGSEGV", pass, detail);
    all &= pass;

    if (!coherent && all) {
        std::printf("Guest memory probe: the chosen memory type is cached but NOT coherent: the "
                    "roundtrips above passed, but no explicit flush/invalidate (or "
                    "DMA_BUF_IOCTL_SYNC) is done at CPU/GPU handoffs yet\n");
    }
    // Not probed: LSE/exclusive atomics through the mapping (FEX runs the game's locked
    // instructions on it), DMA_BUF_IOCTL_SYNC need, cached vs write-combined bandwidth beyond the
    // read-speed check.
    return all;
}
} // namespace

namespace {
/// dma-buf chunks: exported cached system memory the runtime maps at the game's addresses.
bool DmaBufWorks(const Vulkan::Instance& instance) {
    {
        if (!instance.IsGuestMemoryExportSupported()) {
            std::printf("Guest memory: the driver cannot export system memory as a dma-buf\n");
            fallback_reason = "the driver cannot export memory as a dma-buf";
            return false;
        }
        const vk::Device dev = instance.GetDevice();
        const vk::PhysicalDeviceMemoryProperties props = instance.GetPhysicalDevice().getMemoryProperties();
        constexpr u64 Size = 2 << 20, Offset = 1 << 20;
        const vk::ExternalMemoryBufferCreateInfo external{
            .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
        };
        const auto [buffer_result, buffer] = dev.createBuffer({
            .pNext = &external,
            .size = Size,
            .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
                     vk::BufferUsageFlagBits::eStorageBuffer,
            .sharingMode = vk::SharingMode::eExclusive,
        });
        if (buffer_result != vk::Result::eSuccess) {
            fallback_reason = "dma-buf buffer creation failed (" + vk::to_string(buffer_result) + ")";
            return false;
        }
        const auto requirements = dev.getBufferMemoryRequirements(buffer);
        std::uint32_t type = ~0u;
        bool coherent = true;
        if (unified_mode) {
            type = PickUnifiedType(props, requirements.memoryTypeBits, &coherent);
        } else {
            const auto want = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached;
            for (std::uint32_t i = 0; i < props.memoryTypeCount && type == ~0u; ++i) {
                const auto flags = props.memoryTypes[i].propertyFlags;
                if ((requirements.memoryTypeBits & (1u << i)) && (flags & want) == want &&
                    !(flags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
                    type = i;
                }
            }
        }
        if (type == ~0u) {
            fallback_reason = unified_mode ? "no host-visible CPU-cached memory type the dma-buf allows "
                                             "(only write-combined or device-only types: the game would "
                                             "crawl on them)"
                                           : "no exportable cached system memory";
        } else if (unified_mode) {
            std::printf("Guest memory: unified memory type %u: %s (%s)\n", type,
                        vk::to_string(props.memoryTypes[type].propertyFlags).c_str(),
                        coherent ? "coherent" : "NOT coherent");
        }
        bool ok = false;
        vk::DeviceMemory memory{};
        int fd = -1;
        const vk::ExportMemoryAllocateInfo export_info{
            .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
        };
        if (type != ~0u) {
            const auto [memory_result, allocated] = dev.allocateMemory(
                {.pNext = &export_info, .allocationSize = requirements.size, .memoryTypeIndex = type});
            if (memory_result == vk::Result::eSuccess) {
                memory = allocated;
                const auto [fd_result, exported] = dev.getMemoryFdKHR(
                    {.memory = memory, .handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT});
                fd = fd_result == vk::Result::eSuccess ? exported : -1;
                if (fd < 0) {
                    fallback_reason = "dma-buf export failed (" + vk::to_string(fd_result) + ")";
                }
            } else {
                fallback_reason = "allocating exportable memory failed (" +
                                  vk::to_string(memory_result) + ")";
            }
        }
        if (fd >= 0) {
            // The whole buffer and one page inside it, writing through one, reading through the other.
            void* whole = mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, Offset);
            // BB_GUEST_WHOLE_CHUNKS=1: as if the offset mapping failed (tests the NVIDIA path).
            const char* force_whole = std::getenv("BB_GUEST_WHOLE_CHUNKS");
            if (force_whole && force_whole[0] == '1' && page != MAP_FAILED) {
                munmap(page, 4096);
                page = MAP_FAILED;
            }
            if (whole != MAP_FAILED && page != MAP_FAILED) {
                *static_cast<volatile std::uint32_t*>(page) = 0x5ca1ab1e;
                ok = static_cast<volatile std::uint32_t*>(whole)[Offset / 4] == 0x5ca1ab1e;
            } else if (whole != MAP_FAILED) {
                // Offset 0 only (NVIDIA): a second mapping of the start must alias the first.
                void* again = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (again != MAP_FAILED) {
                    *static_cast<volatile std::uint32_t*>(again) = 0x5ca1ab1e;
                    ok = whole_only = static_cast<volatile std::uint32_t*>(whole)[0] == 0x5ca1ab1e;
                    munmap(again, 4096);
                }
            }
            if (!ok && fallback_reason.empty()) {
                fallback_reason = "the dma-buf cannot be mapped (or CPU writes are not seen through a second mapping)";
            }
            // The game's code works on this memory: an uncached CPU mapping would crawl.
            if (ok) {
                const double speed =
                    MappedReadSpeed(static_cast<const volatile std::uint64_t*>(whole), Size);
                std::printf("Guest memory: CPU reads through the driver's dma-buf: %.0f MB/s\n",
                            speed);
                if (speed < 1000.0) {
                    std::printf("Guest memory: CPU reads of the driver's dma-buf run at %.0f MB/s "
                                "(uncached?): not used\n",
                                speed);
                    ok = false;
                    whole_only = false;
                    fallback_reason = "CPU reads of the dma-buf are too slow (uncached mapping)";
                }
            }
            // Unified memory (or BB_GUEST_PROBES=1): CPU/GPU roundtrips and write protection.
            if (ok && whole != MAP_FAILED && (unified_mode || ProbesForced())) {
                ok = bind_ok(dev, buffer, memory) &&
                     RunGuestProbes(instance, buffer, whole, Size, coherent);
                if (!ok && fallback_reason.empty()) {
                    fallback_reason = "probe failed (see the lines above)";
                }
                if (ok && unified_mode) {
                    unified_noncoherent = !coherent;
                    unified_type = type;
                }
            }
            if (whole != MAP_FAILED) {
                munmap(whole, Size);
            }
            if (page != MAP_FAILED) {
                munmap(page, 4096);
            }
            close(fd);
        }
        if (memory) {
            dev.freeMemory(memory);
        }
        dev.destroyBuffer(buffer);
        if (!ok) {
            std::printf("Guest memory: the driver's dma-buf cannot be mapped (%s)\n",
                        fd >= 0 ? "mmap failed" : "no exportable cached system memory");
        } else if (whole_only) {
            std::printf("Guest memory: the driver's dma-buf maps at offset 0 only: one chunk per "
                        "direct memory allocation\n");
        }
        return ok;
    }
}
} // namespace

bool Usable(const Vulkan::Instance& instance) {
    static const bool usable = [&] {
        device = instance.GetDevice();
        memory_properties = instance.GetPhysicalDevice().getMemoryProperties();
        // BB_GUEST_MEMORY=dmabuf|host chooses; else AMD uses dma-buf chunks (tested the most) and
        // other GPUs host memory import first (NVIDIA: its dma-buf maps at offset 0 only, and
        // CPU access through it may be slow).
        const char* forced = std::getenv("BB_GUEST_MEMORY");
        const bool force_host = forced && std::string_view{forced} == "host";
        const bool force_dmabuf = forced && std::string_view{forced} == "dmabuf";
        // Unified memory is dma-buf chunks (Turnip's host import cannot share device memory).
        unified_mode = !force_host && UnifiedEligible(instance);
        const bool host_first =
            force_host || (!force_dmabuf && !unified_mode && instance.GetVendorID() != 0x1002);
        const char* why = "";
        if (host_first && HostImportWorks(instance, why)) {
            mode = Mode::HostImport;
            std::printf("Guest memory: host memory imported by the GPU (VK_EXT_external_memory_host, "
                        "alignment %llu)\n",
                        (unsigned long long)import_alignment);
            return true;
        }
        if (host_first) {
            std::printf("Guest memory: host memory import does not work here: %s\n", why);
            if (force_host) {
                return false;
            }
        }
        mode = Mode::DmaBuf;
        const bool requested = unified_mode;
        const bool ok = DmaBufWorks(instance);
        if (requested && !ok) {
            unified_mode = false;
            std::printf("Guest memory: unified memory (BB_UNIFIED=1) is NOT used: %s; the GPU uses "
                        "copies\n",
                        fallback_reason.empty() ? "the dma-buf probes failed" : fallback_reason.c_str());
        } else if (ok && unified_mode) {
            fallback_reason.clear();
            std::printf("Guest memory: UNIFIED MEMORY: all probes passed (memory type %u%s)\n",
                        unified_type, unified_noncoherent ? ", not coherent" : "");
        }
        return ok;
    }();
    return usable;
}

vk::ExternalMemoryHandleTypeFlagBits HandleType() {
    return mode == Mode::HostImport ? vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT
                                    : vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
}

namespace {
/// Phase 1 measurement: the Vulkan memory types and heaps, once.
void LogMemoryTypes(const Vulkan::Instance& instance) {
    static std::once_flag once;
    std::call_once(once, [&] {
        const auto props = instance.GetPhysicalDevice().getMemoryProperties();
        std::printf("Vulkan memory: %u heaps, %u types (%s, %s)\n", props.memoryHeapCount,
                    props.memoryTypeCount, std::string(instance.GetModelName()).c_str(),
                    instance.IsIntegrated() ? "integrated" : "not integrated");
        for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            const auto& type = props.memoryTypes[i];
            const auto& heap = props.memoryHeaps[type.heapIndex];
            std::printf("Vulkan memory type %2u: %-60s heap %u (%llu MiB, %s)\n", i,
                        vk::to_string(type.propertyFlags).c_str(), type.heapIndex,
                        (unsigned long long)(heap.size >> 20), vk::to_string(heap.flags).c_str());
        }
    });
}
} // namespace

void Install(const Vulkan::Instance& instance) {
    LogMemoryTypes(instance);
    // The buffer cache has no sparse arena to bind guest memory chunks into (NonSparseArenas): only
    // unified memory goes on, with the layer's memory module (no arena for the chunks), and only when
    // the buffer cache kept the in-place model (it checked the driver: UnifiedEligible, Usable).
    if (VideoCore::NonSparseArenas() &&
        !(UnifiedRequested() && VideoCore::GuestInPlace() && LayerMemory())) {
        return;
    }
    // BB_GUEST_IN_PLACE (the GPU uses this memory in place) needs it too.
    const char* env = std::getenv("BB_GUEST_GPU_MEMORY");
    const char* in_place = std::getenv("BB_GUEST_IN_PLACE");
    if (!(env && env[0] == '1') && !(in_place && in_place[0] == '1' && PcModelGpu(instance))) {
        return;
    }
    if (!Usable(instance)) {
        std::printf("Guest memory: direct memory stays in the memfd\n");
        return;
    }
    const bool host = mode == Mode::HostImport;
    // One chunk per direct allocation where the dma-buf maps at offset 0 only, and with the
    // layer's memory module (a mapping then lies whole in one chunk buffer).
    runtime_memory_set_guest_chunk_whole(LayerMemory() || (!host && whole_only) ? 1 : 0);
    runtime_memory_set_guest_chunk_allocator(host ? &AllocImported : &AllocChunk);
    std::printf("Guest memory: direct memory chunks come from Vulkan (BB_GUEST_GPU_MEMORY=1)%s\n",
                Unified() ? "; UNIFIED MEMORY is active (BB_UNIFIED=1): the game's direct memory is "
                            "shared with the GPU, in place"
                          : "");
}

const Chunk* Find(std::uint64_t phys) {
    std::scoped_lock lk{mutex};
    // The last chunk starting at or below phys.
    std::size_t lo = 0, hi = chunk_count;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (chunks[mid]->phys <= phys) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return nullptr;
    }
    const Chunk* c = chunks[lo - 1];
    return phys < c->phys + c->size ? c : nullptr;
}
} // namespace BbGuestMemory
