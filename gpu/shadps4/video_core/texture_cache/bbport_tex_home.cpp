// SPDX-License-Identifier: GPL-2.0-or-later
// bbport BB_TEX_HOME=1: textures homed in the GPU (docs/ANDROID_UNIFIED_MEMORY_PLAN.md, "Plan A";
// the runtime side: src/runtime_memory.c, "Textures homed in the GPU").
//
// Homing (the collector's pass, GPU command thread, the draw pipe drained, `mutex` held): a sampled
// texture the CPU has not written for BB_TEX_HOME_IDLE_S is re-tiled by the GPU into a host-cached
// staging buffer (zeros where no texel is). Once the GPU is past that (a later pass, no wait), the
// runtime traps every alias of the texture's whole pages, compares them with the re-tile and, when
// equal, punches them out of the game's memory. Only the image holds them then.
//
// Rebuilding (TexHomeRehydrateFrom), wherever the pages are needed:
//  - GPU command thread (stage A): the draw pipe is drained, then the image is re-tiled, the GPU
//    waited for (scheduler.Finish), the bytes written into the game's memory through the backing
//    view and the trap lifted (TexHomeRehydrate), inline, also in a fault handler (as the buffer
//    cache's readbacks with assume_locks).
//  - draw recording thread (stage B): inline the same way (it owns the caches while it runs).
//  - any other thread (the game's, the runtime's host threads): the GPU command thread does it
//    (Liverpool::SendCommand, run after its drain) while this one waits; the game's threads wait as
//    long as it takes, host threads 250 ms (the GPU thread may be waiting for them: copy and
//    recording workers, the texture binding helper), then give up (counted, logged once): a fault
//    then ends the process with the fault report, a runtime read goes on with zeros. Those threads
//    are kept away from homed pages: every copy of guest memory the GPU side queues for them is
//    preceded by BbTexHome::BeforeAccess on the queuing (GPU) thread, and homing waits for the copies
//    in flight first.
//  - a thread with a speculative-read recovery point (draw preparation): not handled, it recovers.
#include <algorithm>
#include <chrono>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "bbport_fault.h"
#include "bbport_guest_memory.h"
#include "bbport_tex_home.h"
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

extern "C" int runtime_memory_direct_phys(uintptr_t address, uint64_t* phys, uintptr_t* end);
extern "C" int runtime_memory_granule_shared(uint64_t phys);
extern "C" __thread sigjmp_buf* runtime_fault_recover;

namespace BbTexHome {

namespace {
std::atomic<int> state{0}; ///< 0 not decided, 1 on, 2 off
VideoCore::TextureCache* cache = nullptr;

u32 NowSeconds() {
    return u32(std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

bool EnvOn(const char* name) {
    const char* env = std::getenv(name);
    return env && env[0] == '1';
}

/// Once, at the collector's first pass (the buffer cache's probes have settled the memory model).
void Decide(VideoCore::TextureCache* texture_cache) {
    if (!EnvOn("BB_TEX_HOME")) {
        state.store(2, std::memory_order_release);
        return;
    }
    const char* why = nullptr;
    if (VideoCore::GuestInPlace()) {
        why = "the in-place memory model (BB_GUEST_IN_PLACE=1 without BB_UMA)";
    } else if (!VideoCore::WriteTracking()) {
        why = "no write tracking";
    } else if (!BbFault::Prepare()) {
        why = "BB_FAULT_PREPARE=0 (its map of the write-protected pages keeps a texture's tracking)";
    } else if (EnvOn("BB_UFFD")) {
        why = "userfaultfd write tracking (BB_UFFD=1)";
    } else if (BbGuestMemory::Unified() || EnvOn("BB_GUEST_GPU_MEMORY")) {
        why = "direct memory in GPU chunks (BB_UNIFIED / BB_GUEST_GPU_MEMORY)";
    }
    if (why) {
        std::printf("Texture home: off: %s\n", why);
        state.store(2, std::memory_order_release);
        return;
    }
    cache = texture_cache;
    fault_handler.store([](void* context, std::uintptr_t address) {
        return cache->TexHomeOnFault(context, address);
    }, std::memory_order_release);
    range_handler.store([](std::uintptr_t address, std::uint64_t size) {
        cache->TexHomeRehydrateRange(address, size, ByAccess);
    }, std::memory_order_release);
    runtime_memory_set_home_hook([](std::uint32_t ticket) -> int {
        return cache->TexHomeRehydrateFrom(ticket, nullptr, ByRuntime) ? 1 : 0;
    });
    active.store(true, std::memory_order_release);
    state.store(1, std::memory_order_release);
    std::printf("Texture home: on (BB_TEX_HOME=1): sampled textures the CPU left alone for %llu s "
                "live only in the GPU (host page %llu bytes)\n",
                (unsigned long long)IdleSeconds(), (unsigned long long)runtime_memory_home_granule());
}

/// A format whose texels are the guest's bytes (the host format substituted only by name).
bool BitwiseFormat(vk::Format host, vk::Format guest) {
    if (host == guest) {
        return true;
    }
    const auto pair = [&](vk::Format a, vk::Format b) {
        return (host == a && guest == b) || (host == b && guest == a);
    };
    return pair(vk::Format::eR8Unorm, vk::Format::eR8Srgb) ||
           pair(vk::Format::eR8G8Unorm, vk::Format::eR8G8Srgb);
}

void NoteRebuild(Rebuild why, u64 ns) {
    counters.rebuilt[why].fetch_add(1, std::memory_order_relaxed);
    counters.rebuild_ns_total.fetch_add(ns, std::memory_order_relaxed);
    u64 max = counters.rebuild_ns_max.load(std::memory_order_relaxed);
    while (ns > max && !counters.rebuild_ns_max.compare_exchange_weak(max, ns)) {
    }
}
} // namespace

bool Enabled() {
    return state.load(std::memory_order_acquire) == 1;
}

std::uint64_t IdleSeconds() {
    static const u64 seconds = [] {
        const char* env = std::getenv("BB_TEX_HOME_IDLE_S");
        return env ? std::clamp<u64>(std::strtoull(env, nullptr, 10), 1, 3600) : u64{10};
    }();
    return seconds;
}

} // namespace BbTexHome

namespace VideoCore {

using namespace BbTexHome;

namespace {
constexpr u64 MinHomeBytes = 256_KB;
constexpr size_t MaxVerifies = 4;   ///< re-tiles in flight
constexpr u32 VisitsPerScan = 256;  ///< images looked at per scan
constexpr auto ScanPeriod = std::chrono::milliseconds(250);
constexpr auto HostWaitLimit = std::chrono::milliseconds(250);

/// The image's whole host pages: [*begin, *end); false when there is none.
bool HomePages(const Image& image, VAddr* begin, VAddr* end) {
    const u64 page = runtime_memory_home_granule();
    *begin = Common::AlignUp(image.info.guest_address, page);
    *end = Common::AlignDown(image.info.guest_address + image.info.guest_size, page);
    return *end > *begin;
}
} // namespace

bool TextureCache::TexHomeEligible(const Image& image, u32 now, Refusal& why) {
    const auto refuse = [&](Refusal reason) {
        why = reason;
        return false;
    };
    if (False(image.flags & ImageFlagBits::Registered) || !image.backing) {
        return refuse(Usage);
    }
    if (image.tex_home_never) {
        return refuse(Mismatch);
    }
    VAddr begin = 0, end = 0;
    if (image.info.guest_size < MinHomeBytes || !HomePages(image, &begin, &end)) {
        return refuse(Small);
    }
    if (!image.usage.texture || image.usage.storage || image.usage.render_target ||
        image.usage.depth_target || image.usage.vo_surface || image.scene_proxy ||
        image.binding.is_target) {
        return refuse(Usage);
    }
    if (True(image.flags & (ImageFlagBits::GpuModified | ImageFlagBits::GpuDirty)) ||
        image.gpu_write_tick != 0) {
        return refuse(GpuWritten);
    }
    if (image.info.num_samples > 1 || image.backing->num_samples > 1 ||
        image.backing_images.size() != 1) {
        return refuse(Msaa);
    }
    if (image.info.props.is_depth || image.info.props.has_stencil || image.info.stencil_addr != 0 ||
        image.info.meta_info.cmask_addr != 0 || image.info.meta_info.fmask_addr != 0 ||
        image.info.meta_info.htile_addr != 0) {
        return refuse(DepthStencil);
    }
    if (!BitwiseFormat(image.backing->image.image_ci.format, image.info.pixel_format)) {
        return refuse(Format);
    }
    if (True(image.flags & ImageFlagBits::Dirty) || image.tex_home_stamp == 0 ||
        image.track_addr != image.guest_begin || image.track_addr_end != image.guest_end) {
        return refuse(BbTexHome::Dirty);
    }
    if (now - image.tex_home_stamp < IdleSeconds()) {
        return refuse(Recent);
    }
    // One direct memory mapping, no shared UMA granule (the runtime checks both again).
    u64 phys = 0;
    uintptr_t mapping_end = 0;
    if (!runtime_memory_direct_phys(begin, &phys, &mapping_end) || mapping_end < end) {
        return refuse(Mapping);
    }
    for (u64 at = 0; at < end - begin; at += 256_KB) {
        if (runtime_memory_granule_shared(phys + at)) {
            return refuse(Shared);
        }
    }
    if (runtime_memory_granule_shared(phys + (end - begin) - 1)) {
        return refuse(Shared);
    }
    bool overlap = false;
    ForEachImageInRegion(image.info.guest_address, image.info.guest_size,
                         [&](ImageId, Image& other) { overlap |= &other != &image; });
    if (overlap) {
        return refuse(Overlap);
    }
    if (buffer_cache.IsRegionGpuModified(image.info.guest_address, image.info.guest_size)) {
        return refuse(GpuData);
    }
    return true;
}

Vulkan::StagingBufferRef TextureCache::TexHomeTile(Image& image, bool deferred) {
    // The inverse of RefreshImage's upload: the same regions, re-tiled by TileManager.
    boost::container::small_vector<vk::BufferImageCopy, 14> copies;
    for (u32 m = 0; m < image.info.resources.levels; ++m) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];
        copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {mip_pitch ? std::min(mip_pitch, width) : width,
                            mip_height ? std::min(mip_height, height) : height, depth},
        });
    }
    const u64 bytes = Common::AlignUp(u64(image.info.guest_size), 256);
    const auto staging =
        runtime.GetStagingPool().Request(bytes, MemoryType::HostCached, 256, deferred);
    runtime.BeforeImageAccess();
    scheduler.EndRendering();
    runtime.FillBuffer(staging.buffer, staging.offset, bytes, 0);
    tile_manager.TileImage(image, copies, staging.buffer, staging.offset, true);
    runtime.FlushBarriers();
    // The host reads it once the GPU is past this (no later command reads the staging).
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    });
    return staging;
}

u32 TextureCache::TexHomeTicketOf(ImageId image_id) {
    std::scoped_lock lk{home_mutex};
    const auto it = home_of_image.find(image_id.index);
    return it == home_of_image.end() ? 0 : it->second;
}

bool TextureCache::TexHomeRehydrate(u32 ticket, Rebuild why) {
    std::scoped_lock lk{home_mutex};
    const auto it = homes.find(ticket);
    if (it == homes.end()) {
        // No record (brought back meanwhile, or never this cache's): what is left of it comes back
        // as zeros rather than trapping for good.
        if (runtime_memory_home_alive(ticket) != 0 &&
            runtime_memory_home_restore(ticket, nullptr, 0) > 0) {
            counters.lost.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }
    const HomeRecord record = it->second;
    const auto forget = [&] {
        homes.erase(ticket);
        home_of_image.erase(record.image_id.index);
        homes_count.fetch_sub(1, std::memory_order_relaxed);
        counters.homed_images.fetch_sub(1, std::memory_order_relaxed);
        counters.homed_bytes.fetch_sub(record.size, std::memory_order_relaxed);
    };
    if (runtime_memory_home_alive(ticket) == 0) {
        forget(); // released with its memory: nothing to rebuild
        return true;
    }
    Image* image = TryGetImage(record.image_id, record.image_uid);
    if (!image || !image->backing) {
        runtime_memory_home_restore(ticket, nullptr, 0);
        counters.lost.fetch_add(1, std::memory_order_relaxed);
        forget();
        return true;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto staging = TexHomeTile(*image, false);
    scheduler.Finish();
    staging.Invalidate();
    const int restored = runtime_memory_home_restore(
        ticket, staging.mapped + (record.address - image->info.guest_address), record.size);
    if (restored < -1) {
        std::fprintf(stderr, "Texture home: rebuild of %#llx+%#llx refused by the runtime (%d)\n",
                     (unsigned long long)record.address, (unsigned long long)record.size, restored);
    }
    forget();
    image->tex_home_stamp = NowSeconds() | 1u; // not homed again before it was left alone again
    NoteRebuild(why, u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count()));
    return true;
}

bool TextureCache::TexHomeRehydrateFrom(u32 ticket, const void* context, Rebuild why) {
    if (runtime_memory_home_alive(ticket) == 0) {
        return true; // gone (its record goes at the next scan or rebuild)
    }
    auto* rasterizer = Core::Memory::Instance()->GetRasterizer();
    if (rasterizer->IsGpuSideThread()) {
        rasterizer->DrainDrawPipe(); // stage A: the recording thread idle (a no-op on it)
        return TexHomeRehydrate(ticket, why);
    }
    counters.forwarded.fetch_add(1, std::memory_order_relaxed);
    struct Request {
        std::atomic<u32> done{0};
    };
    const auto request = std::make_shared<Request>();
    liverpool->SendCommand([this, ticket, why, request] {
        TexHomeRehydrate(ticket, why);
        request->done.store(1, std::memory_order_release);
    });
    const bool guest = runtime_thread_is_guest(context) != 0;
    const auto start = std::chrono::steady_clock::now();
    bool warned = false;
    for (u32 spins = 0; request->done.load(std::memory_order_acquire) == 0; ++spins) {
        if (spins < 64) {
            std::this_thread::yield();
            continue;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        const auto waited = std::chrono::steady_clock::now() - start;
        if (!guest && waited > HostWaitLimit) {
            counters.timeouts.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<u32> logged{0};
            if (logged.fetch_add(1) < 4) {
                char name[17] = "?";
                pthread_getname_np(pthread_self(), name, sizeof(name));
                std::fprintf(stderr,
                             "Texture home: host thread %s needs homed pages and the GPU thread did "
                             "not bring them back within 250 ms (it may be waiting for this thread)\n",
                             name);
            }
            return false;
        }
        if (guest && !warned && waited > std::chrono::seconds(5)) {
            warned = true;
            std::fprintf(stderr, "Texture home: a game thread has waited 5 s for a rebuild\n");
        }
    }
    return true;
}

void TextureCache::TexHomeRehydrateRange(VAddr addr, u64 size, Rebuild why) {
    for (int round = 0; round < 8; ++round) {
        std::array<u32, 16> tickets{};
        const int n = runtime_memory_home_tickets(addr, size, tickets.data(), int(tickets.size()));
        if (n <= 0) {
            return;
        }
        bool progress = false;
        for (int i = 0; i < n; ++i) {
            progress |= TexHomeRehydrateFrom(tickets[i], nullptr, why);
        }
        if (!progress) {
            return;
        }
    }
}

void TextureCache::TexHomeRehydrateImage(ImageId image_id, Rebuild why) {
    if (const u32 ticket = TexHomeTicketOf(image_id)) {
        TexHomeRehydrateFrom(ticket, nullptr, why);
    }
}

bool TextureCache::TexHomeOnFault(void* context, VAddr addr) {
    if (runtime_fault_recover) {
        return false; // a speculative reader: it recovers
    }
    const u32 ticket = runtime_memory_home_ticket(addr);
    if (ticket == 0) {
        return runtime_memory_home_unstale(addr) != 0; // brought back meanwhile: retried
    }
    return TexHomeRehydrateFrom(ticket, context, ByFault);
}

void TextureCache::TexHomeOnRegister(VAddr addr, u64 size) {
    // Off the GPU threads (video out buffers registered by the game's thread) a rebuild would wait for
    // the GPU thread with `mutex` held: left to the reads themselves (BeforeAccess, faults).
    if (Core::Memory::Instance()->GetRasterizer()->IsGpuSideThread()) {
        TexHomeRehydrateRange(addr, size, ByOverlap);
    }
}

void TextureCache::TexHomeBeforeUnmap(VAddr addr, u64 size) {
    for (int round = 0; round < 4; ++round) {
        boost::container::small_vector<u32, 8> tickets;
        {
            std::scoped_lock lk{mutex};
            ForEachImageInRegion(addr, size, [&](ImageId image_id, Image&) {
                if (const u32 ticket = TexHomeTicketOf(image_id)) {
                    tickets.push_back(ticket);
                }
            });
        }
        if (tickets.empty()) {
            return;
        }
        for (const u32 ticket : tickets) {
            TexHomeRehydrateFrom(ticket, nullptr, ByFree);
        }
    }
}

void TextureCache::TexHomeOnFree(ImageId image_id) {
    const u32 ticket = TexHomeTicketOf(image_id);
    if (ticket == 0) {
        return;
    }
    auto* rasterizer = Core::Memory::Instance()->GetRasterizer();
    if (rasterizer->IsGpuSideThread()) {
        TexHomeRehydrate(ticket, ByFree); // `mutex` held here: stage B is idle
        return;
    }
    // Another thread holding `mutex` cannot wait for the GPU thread (it may need the lock): the home
    // is forgotten. UnmapMemory brings homes back before it locks; this is what slips past it.
    std::scoped_lock lk{home_mutex};
    if (runtime_memory_home_alive(ticket) != 0 &&
        runtime_memory_home_restore(ticket, nullptr, 0) > 0) {
        counters.lost.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<u32> logged{0};
        if (logged.fetch_add(1) < 4) {
            std::fprintf(stderr, "Texture home: an image freed off the GPU threads was homed: its "
                                 "pages come back as zeros\n");
        }
    }
    if (const auto it = homes.find(ticket); it != homes.end()) {
        counters.homed_images.fetch_sub(1, std::memory_order_relaxed);
        counters.homed_bytes.fetch_sub(it->second.size, std::memory_order_relaxed);
        homes.erase(it);
        homes_count.fetch_sub(1, std::memory_order_relaxed);
    }
    home_of_image.erase(image_id.index);
}

void TextureCache::TexHomeFinish(HomeVerify& verify) {
    Image* image = TryGetImage(verify.image_id, verify.image_uid);
    if (!image) {
        return;
    }
    const u32 now = NowSeconds();
    Refusal why{};
    if (image->tex_home_stamp != verify.stamp) {
        why = BbTexHome::Dirty; // uploaded again meanwhile
    } else if (TexHomeEligible(*image, now, why)) {
        why = RefusalKinds;
    }
    if (why != RefusalKinds) {
        counters.refused[why].fetch_add(1, std::memory_order_relaxed);
        image->tex_home_retry = now + 5;
        return;
    }
    VAddr begin = 0, end = 0;
    HomePages(*image, &begin, &end);
    verify.staging.Invalidate();
    // Copies of guest memory still running on the copy threads may read these pages.
    scheduler.WaitHostCopies();
    std::scoped_lock lk{home_mutex};
    u32 ticket = 0;
    const int rc = runtime_memory_home(
        begin, end - begin, verify.staging.mapped + (begin - image->info.guest_address), &ticket);
    switch (rc) {
    case HomeOk:
        homes.emplace(ticket, HomeRecord{verify.image_id, verify.image_uid, begin, end - begin});
        home_of_image[verify.image_id.index] = ticket;
        homes_count.fetch_add(1, std::memory_order_relaxed);
        counters.homes.fetch_add(1, std::memory_order_relaxed);
        counters.homed_images.fetch_add(1, std::memory_order_relaxed);
        counters.homed_bytes.fetch_add(end - begin, std::memory_order_relaxed);
        return;
    case HomeMismatch: {
        image->tex_home_never = true;
        counters.refused[Mismatch].fetch_add(1, std::memory_order_relaxed);
        static std::atomic<u32> logged{0};
        if (std::getenv("BB_TEX_HOME_LOG") && logged.fetch_add(1) < 40) {
            std::printf("Texture home: %#llx %ux%u fmt %u tile %u mips %u layers %u: re-tile differs\n",
                        (unsigned long long)image->info.guest_address, image->info.size.width,
                        image->info.size.height, u32(image->info.pixel_format),
                        u32(image->info.tile_mode), image->info.resources.levels,
                        image->info.resources.layers);
        }
        return;
    }
    case HomeMapping:
        why = Mapping;
        break;
    case HomeShared:
        why = Shared;
        break;
    default:
        why = Runtime;
        break;
    }
    counters.refused[why].fetch_add(1, std::memory_order_relaxed);
    image->tex_home_retry = now + 30;
}

void TextureCache::TexHomePass() {
    if (state.load(std::memory_order_acquire) == 0) {
        Decide(this);
    }
    if (!Enabled()) {
        return;
    }
    auto* rasterizer = Core::Memory::Instance()->GetRasterizer();
    if (!rasterizer || !rasterizer->IsGpuSideThread()) {
        return;
    }
    std::scoped_lock lock{mutex};
    // Re-tiles the GPU is done with: compared and homed.
    for (size_t i = 0; i < home_verifies.size();) {
        auto& verify = home_verifies[i];
        if (!scheduler.IsFree(verify.tick)) {
            ++i;
            continue;
        }
        TexHomeFinish(verify);
        runtime.GetStagingPool().FreeDeferred(verify.staging);
        home_verifies[i] = home_verifies.back();
        home_verifies.pop_back();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - home_last_scan < ScanPeriod) {
        return;
    }
    home_last_scan = now;
    // Records of homes released with their memory.
    if (homes_count.load(std::memory_order_relaxed) != 0) {
        std::scoped_lock lk{home_mutex};
        for (auto it = homes.begin(); it != homes.end();) {
            if (runtime_memory_home_alive(it->first) != 0) {
                ++it;
                continue;
            }
            counters.homed_images.fetch_sub(1, std::memory_order_relaxed);
            counters.homed_bytes.fetch_sub(it->second.size, std::memory_order_relaxed);
            home_of_image.erase(it->second.image_id.index);
            it = homes.erase(it);
            homes_count.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    const u32 seconds = NowSeconds();
    const u32 capacity = u32(slot_images.capacity());
    for (u32 visited = 0; visited < VisitsPerScan && capacity != 0 &&
                          home_verifies.size() < MaxVerifies;
         ++visited) {
        const ImageId image_id{home_cursor++ % capacity};
        if (!slot_images.is_allocated(image_id)) {
            continue;
        }
        Image& image = slot_images[image_id];
        if (image.tex_home_retry > seconds) {
            continue;
        }
        Refusal why{};
        if (!TexHomeEligible(image, seconds, why)) {
            counters.refused[why].fetch_add(1, std::memory_order_relaxed);
            const bool lasting = why == Small || why == Usage || why == GpuWritten || why == Msaa ||
                                 why == DepthStencil || why == Format || why == Mismatch;
            image.tex_home_retry =
                why == Recent ? image.tex_home_stamp + u32(IdleSeconds())
                              : seconds + (lasting ? 60 : 5);
            continue;
        }
        if (TexHomeTicketOf(image_id) != 0) {
            image.tex_home_retry = seconds + 5;
            continue;
        }
        bool pending = false;
        for (const auto& verify : home_verifies) {
            pending |= verify.image_id == image_id;
        }
        if (pending) {
            continue;
        }
        HomeVerify verify{image_id, image.image_uid, 0, image.tex_home_stamp, {}};
        verify.staging = TexHomeTile(image, true);
        verify.tick = scheduler.CurrentTick();
        home_verifies.push_back(verify);
        counters.verifies.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace VideoCore
