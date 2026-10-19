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
#include <cstring>
#include <thread>
#include <fmt/format.h>

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
    std::printf("Texture home: on (BB_TEX_HOME=1): sampled textures of %llu KiB and more the CPU left "
                "alone for %llu s live only in the GPU (host page %llu bytes)%s\n",
                (unsigned long long)(MinBytes() >> 10), (unsigned long long)IdleSeconds(),
                (unsigned long long)runtime_memory_home_granule(),
                First() ? fmt::format("; texture-first: UMA leaves granules over {}% textures to it",
                                      FirstPercent()).c_str()
                        : "");
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

std::uint64_t MinBytes() {
    static const u64 bytes = [] {
        const char* env = std::getenv("BB_TEX_HOME_MIN_KB");
        return (env ? std::clamp<u64>(std::strtoull(env, nullptr, 10), 4, 65536) : u64{64}) * 1024;
    }();
    return bytes;
}

bool First() {
    static const bool wanted = EnvOn("BB_TEX_HOME_FIRST") && EnvOn("BB_TEX_HOME");
    return wanted && state.load(std::memory_order_relaxed) != 2;
}

std::uint64_t FirstPercent() {
    static const u64 percent = [] {
        const char* env = std::getenv("BB_TEX_HOME_FIRST_PCT");
        return env ? std::clamp<u64>(std::strtoull(env, nullptr, 10), 1, 100) : u64{50};
    }();
    return percent;
}

} // namespace BbTexHome

namespace VideoCore {

using namespace BbTexHome;

namespace {
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

std::optional<u64> TextureCache::SampledTextureBytes(VAddr addr, u64 size) {
    // The binding thread asks (UmaDirect); as OverlapsGpuImage: a bounded wait for the lock.
    std::unique_lock lock{mutex, std::defer_lock};
    for (int attempt = 0; attempt < 256 && !lock.owns_lock(); ++attempt) {
        if (!lock.try_lock()) {
            std::this_thread::yield();
        }
    }
    if (!lock.owns_lock()) {
        return std::nullopt;
    }
    boost::container::small_vector<std::pair<VAddr, VAddr>, 32> parts;
    const VAddr end = addr + size;
    ForEachImageInRegion(addr, size, [&](ImageId, Image& image) {
        if (!image.usage.texture || image.usage.render_target || image.usage.depth_target ||
            image.usage.storage || image.usage.vo_surface) {
            return;
        }
        parts.emplace_back(std::max(image.info.guest_address, addr),
                           std::min<VAddr>(image.info.guest_address + image.info.guest_size, end));
    });
    std::sort(parts.begin(), parts.end());
    u64 covered = 0;
    VAddr reached = addr;
    for (const auto& [from, to] : parts) {
        const VAddr start = std::max(from, reached);
        if (to > start) {
            covered += to - start;
            reached = to;
        }
    }
    return covered;
}

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
    if (image.info.guest_size < MinBytes() || !HomePages(image, &begin, &end)) {
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

Vulkan::StagingBufferRef TextureCache::TexHomeTile(Image& image, bool deferred, u32 fill,
                                                   u32 hole_fill) {
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
    // Tiled: the tiler writes the tiled bytes from the linear copy (`fill` where no texel is), and
    // `hole_fill` stays where it writes nothing; a linear image is copied straight into the staging,
    // around its padding.
    runtime.FillBuffer(staging.buffer, staging.offset, bytes,
                       image.info.props.is_tiled ? hole_fill : fill);
    tile_manager.TileImage(image, copies, staging.buffer, staging.offset, fill);
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
        counters.patch_bytes.fetch_sub(record.patch_bytes.size(), std::memory_order_relaxed);
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
    // The game's padding bytes the re-tile has as zeros.
    for (u64 at = 0; const auto& [offset, length] : record.patch_runs) {
        std::memcpy(staging.mapped + offset, record.patch_bytes.data() + at, length);
        at += length;
    }
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
        counters.patch_bytes.fetch_sub(it->second.patch_bytes.size(), std::memory_order_relaxed);
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
    const VAddr base = image->info.guest_address;
    const u64 size = image->info.guest_size;
    verify.staging.Invalidate();
    verify.staging_ones.Invalidate();
    u8* const zeros = verify.staging.mapped;
    const u8* const ones = verify.staging_ones.mapped;
    // The two re-tiles agree on texels. Where they differ it is padding (0x00 / 0xff: a linear byte
    // no texel fills) or a hole (0x00 / 0x5a: a tiled byte no linear byte lands on). A hole means
    // two linear bytes land on one tiled byte elsewhere: which one wins is a race, and a rebuild
    // might not repeat this re-tile. Holes anywhere in the image: refused. Without them the tiling is
    // one-to-one over the image (as many writes as bytes, all landed), so a rebuild repeats it.
    u64 holes = 0;
    if (image->info.props.is_tiled) {
        for (u64 i = 0; i < size; ++i) {
            holes += zeros[i] != ones[i] && ones[i] != 0xff;
        }
    }
    // The game's bytes now, through the backing view (the runtime compares them again under its
    // traps; this copy tells where they differ).
    std::vector<u8> guest(size);
    runtime_memory_read_backing(base, guest.data(), size);
    const u64 from = begin - base, to = end - base;
    u64 texel_diffs = 0;
    std::vector<std::pair<u32, u32>> runs;
    std::vector<u8> bytes;
    for (u64 i = from; i < to; ++i) {
        if (zeros[i] == guest[i]) {
            continue;
        }
        if (zeros[i] == ones[i]) {
            ++texel_diffs;
            continue;
        }
        if (!runs.empty() && runs.back().first + runs.back().second == i) {
            ++runs.back().second;
        } else {
            runs.emplace_back(u32(i), 1u);
        }
        bytes.push_back(guest[i]);
    }
    const bool too_much = bytes.size() > (to - from) / 4 || runs.size() > 65536;
    const bool differs = texel_diffs != 0 || !bytes.empty();
    const char* verdict = holes         ? "tiling not one-to-one: refused"
                          : texel_diffs ? "texels differ: refused"
                          : too_much    ? "padding only, too much: refused"
                          : differs     ? "padding only: kept aside, homed"
                                        : "equal";
    if (std::getenv("BB_TEX_HOME_LOG") && (differs || holes)) {
        TexHomeLogDiff(*image, begin, end, zeros, ones, guest.data(), holes, verdict);
    }
    if (holes || texel_diffs || too_much) {
        image->tex_home_never = true;
        const Refusal reason = holes ? Unmapped : texel_diffs ? Mismatch : Padding;
        (reason == Padding ? counters.verify_padding_failures
                           : reason == Mismatch ? counters.verify_texel_failures
                                                : counters.verify_hole_failures)
            .fetch_add(1, std::memory_order_relaxed);
        counters.refused[reason].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The re-tile with the game's padding is the game's bytes: what the runtime compares and drops.
    for (u64 at = 0; const auto& [offset, length] : runs) {
        std::memcpy(zeros + offset, bytes.data() + at, length);
        at += length;
    }
    TexHomeCommit(verify, *image, begin, end, zeros + from, std::move(runs), std::move(bytes));
}

void TextureCache::TexHomeCommit(HomeVerify& verify, Image& image, VAddr begin, VAddr end,
                                 const u8* expected, std::vector<std::pair<u32, u32>> patch_runs,
                                 std::vector<u8> patch_bytes) {
    const u32 now = NowSeconds();
    // Copies of guest memory still running on the copy threads may read these pages.
    scheduler.WaitHostCopies();
    std::scoped_lock lk{home_mutex};
    u32 ticket = 0;
    const int rc = runtime_memory_home(begin, end - begin, expected, &ticket);
    Refusal why{};
    switch (rc) {
    case HomeOk: {
        const bool patched = !patch_bytes.empty();
        const u64 kept = patch_bytes.size();
        homes.emplace(ticket, HomeRecord{verify.image_id, verify.image_uid, begin, end - begin,
                                         std::move(patch_runs), std::move(patch_bytes)});
        home_of_image[verify.image_id.index] = ticket;
        homes_count.fetch_add(1, std::memory_order_relaxed);
        counters.homes.fetch_add(1, std::memory_order_relaxed);
        counters.homed_images.fetch_add(1, std::memory_order_relaxed);
        counters.homed_bytes.fetch_add(end - begin, std::memory_order_relaxed);
        if (patched) {
            counters.patched_homes.fetch_add(1, std::memory_order_relaxed);
            counters.patch_bytes.fetch_add(kept, std::memory_order_relaxed);
        }
        return;
    }
    case HomeMismatch:
        // Changed between the copy above and the traps (or a re-tile that is not repeatable).
        image.tex_home_never = true;
        counters.verify_texel_failures.fetch_add(1, std::memory_order_relaxed);
        counters.refused[Mismatch].fetch_add(1, std::memory_order_relaxed);
        return;
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
    image.tex_home_retry = now + 30;
}

void TextureCache::TexHomeLogDiff(const Image& image, VAddr begin, VAddr end, const u8* zeros,
                                  const u8* ones, const u8* guest, u64 holes, const char* verdict) {
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) >= 60) {
        return;
    }
    const auto& info = image.info;
    const VAddr base = info.guest_address;
    u64 mips_bytes = 0;
    for (u32 m = 0; m < info.resources.levels; ++m) {
        mips_bytes += info.mips_layout[m].size;
    }
    // Where an offset lies: its mip, slice (layer x depth, padded to the tile thickness) and
    // whether that mip is stored micro tiled inside a macro-tiled image (the small mips).
    const auto where = [&](u64 offset) {
        u32 mip = 0;
        while (mip + 1 < info.resources.levels && offset >= info.mips_layout[mip + 1].offset) {
            ++mip;
        }
        const auto& m = info.mips_layout[mip];
        const u32 bpe = info.num_bits / 8;
        const u64 pitch = info.props.is_block ? std::max((m.pitch + 3) / 4, 1u) : m.pitch;
        const u64 height = info.props.is_block ? std::max((m.height + 3) / 4, 1u) : m.height;
        const u64 slice_bytes = std::max<u64>(pitch * height * bpe, 1);
        const u64 inside = offset - m.offset;
        const u32 mw = std::max(info.size.width >> mip, 1u), mh = std::max(info.size.height >> mip, 1u);
        return fmt::format("+{:#x} (mip {} {}x{} in {}x{}{}, slice {} of {})", offset, mip, mw, mh,
                           m.pitch, m.height,
                           (info.micro_tiled_mips >> mip) & 1 ? ", micro-tiled tail mip" : "",
                           inside / slice_bytes, m.size / slice_bytes);
    };
    u64 texel = 0, padding = 0, first_texel = ~u64{0}, first_padding = ~u64{0};
    for (u64 i = begin - base; i < end - base; ++i) {
        if (zeros[i] == guest[i]) {
            continue;
        }
        if (zeros[i] == ones[i]) {
            first_texel = std::min(first_texel, i);
            ++texel;
        } else {
            first_padding = std::min(first_padding, i);
            ++padding;
        }
    }
    u64 padding_bytes = 0, first_hole = ~u64{0};
    for (u64 i = 0; i < info.guest_size; ++i) {
        const bool differ = zeros[i] != ones[i];
        padding_bytes += differ && ones[i] == 0xff;
        if (differ && ones[i] != 0xff && first_hole == ~u64{0}) {
            first_hole = i;
        }
    }
    const auto describe = [&](u64 first, const char* kind) -> std::string {
        if (first == ~u64{0}) {
            return fmt::format("no {} byte differs", kind);
        }
        return fmt::format("first {} diff {} guest {:#04x} re-tile {:#04x}", kind, where(first),
                           guest[first], zeros[first]);
    };
    std::printf("Texture home diff: %#llx %s (dfmt %u nfmt %u) tile %s array %u %ux%ux%u layers %u "
                "mips %u pitch %u bits %u%s%s bank swizzle %u; guest %u bytes, re-tiled %llu (padding "
                "%llu, holes %llu%s), pages %#llx+%#llx; differing: texel %llu, padding %llu; %s; %s; "
                "%s\n",
                (unsigned long long)base, vk::to_string(info.pixel_format).c_str(),
                info.guest_data_format, info.guest_number_format,
                std::string(AmdGpu::NameOf(info.tile_mode)).c_str(), u32(info.array_mode),
                info.size.width, info.size.height, info.size.depth, info.resources.layers,
                info.resources.levels, info.pitch, info.num_bits, info.props.is_block ? " block" : "",
                info.props.is_pow2 ? " pow2" : "", info.bank_swizzle, info.guest_size,
                (unsigned long long)mips_bytes, (unsigned long long)padding_bytes,
                (unsigned long long)holes,
                first_hole == ~u64{0} ? "" : (", first " + where(first_hole)).c_str(),
                (unsigned long long)(begin - base), (unsigned long long)(end - begin),
                (unsigned long long)texel, (unsigned long long)padding,
                describe(first_texel, "texel").c_str(), describe(first_padding, "padding").c_str(),
                verdict);
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
        runtime.GetStagingPool().FreeDeferred(verify.staging_ones);
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
            counters.patch_bytes.fetch_sub(it->second.patch_bytes.size(), std::memory_order_relaxed);
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
        HomeVerify verify{image_id, image.image_uid, 0, image.tex_home_stamp, {}, {}};
        verify.staging = TexHomeTile(image, true);
        verify.staging_ones = TexHomeTile(image, true, 0xffffffffu, 0x5a5a5a5au);
        verify.tick = scheduler.CurrentTick();
        home_verifies.push_back(verify);
        counters.verifies.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace VideoCore
