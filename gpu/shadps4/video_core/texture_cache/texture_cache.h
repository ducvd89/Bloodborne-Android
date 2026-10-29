// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <atomic>
#include <chrono>
#include "bbport_toggles.h"
#include "bbport_tex_home.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <boost/container/small_vector.hpp>
#include <queue>
#include <tsl/robin_map.h>

#include "common/lru_cache.h"
#include "common/slot_vector.h"
#include "shader_recompiler/resource.h"
#include "video_core/multi_level_page_table.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/sampler.h"
#include "video_core/texture_cache/tile_manager.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {
class Runtime;
}

namespace VideoCore {

class BufferCache;
class PageManager;

class TextureCache {
    // Default values for garbage collection
    static constexpr s64 DEFAULT_PRESSURE_GC_MEMORY = 1_GB + 512_MB;
    static constexpr s64 DEFAULT_CRITICAL_GC_MEMORY = 3_GB;
    static constexpr s64 TARGET_GC_THRESHOLD = 8_GB;

    using ImageIds = boost::container::small_vector<ImageId, 16>;

    struct Traits {
        using Entry = ImageIds;
        static constexpr size_t AddressSpaceBits = 40;
        static constexpr size_t FirstLevelBits = 10;
        static constexpr size_t PageBits = 20;
    };
    using PageTable = MultiLevelPageTable<Traits>;

public:
    enum class BindingType : u32 {
        Texture,
        Storage,
        RenderTarget,
        DepthTarget,
        VideoOut,
    };

    struct ImageDesc {
        ImageInfo info;
        ImageViewInfo view_info;
        BindingType type{BindingType::Texture};

        ImageDesc() = default;
        ImageDesc(const AmdGpu::Image& image, const Shader::ImageResource& desc)
            : info{image, desc}, view_info{image, desc},
              type{desc.is_written ? BindingType::Storage : BindingType::Texture} {}
        ImageDesc(const AmdGpu::ColorBuffer& buffer, AmdGpu::CbDbExtent hint)
            : info{buffer, hint}, view_info{buffer}, type{BindingType::RenderTarget} {}
        ImageDesc(const AmdGpu::DepthBuffer& buffer, AmdGpu::DepthView view,
                  AmdGpu::DepthControl ctl, VAddr htile_address, AmdGpu::CbDbExtent hint,
                  bool write_buffer = false)
            : info{buffer, view.NumSlices(), htile_address, hint, write_buffer},
              view_info{buffer, view, ctl}, type{BindingType::DepthTarget} {}
        ImageDesc(const Libraries::VideoOut::BufferAttributeGroup& group, VAddr cpu_address)
            : info{group, cpu_address}, type{BindingType::VideoOut} {}
    };

public:
    TextureCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                 Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool, BufferCache& buffer_cache,
                 PageManager& tracker);
    ~TextureCache();

    /// bbport (diagnostics): the images over [addr, addr + size), described briefly.
    std::string DescribeImagesIn(VAddr addr, u64 size);

    /// bbport BB_UMA: an image the GPU writes (render or depth target, storage image, or GPU-modified)
    /// overlaps [addr, addr + size): memory under it is not the game's data to share. True too when
    /// the cache is busy (the lock is not taken for long): the answer that keeps a copy.
    [[nodiscard]] bool OverlapsGpuImage(VAddr addr, u64 size);

    /// bbport: changes whenever an image is registered or unregistered.
    [[nodiscard]] u64 RegistryGeneration() const noexcept {
        return registry_generation.load(std::memory_order_acquire);
    }

    /// bbport: FindImage's access tick for an image found by a memoized lookup. The LRU touch
    /// FindImage also does happens in UpdateImage (FindTexture), which every binding reaches.
    void MarkFound(ImageId image_id) {
        slot_images[image_id].tick_accessed_last = scheduler.CurrentTick();
    }

    TileManager& GetTileManager() noexcept {
        return tile_manager;
    }

    /// Invalidates any image in the logical page range.
    void InvalidateMemory(VAddr addr, size_t size);

    /// Marks an image as dirty if it exists at the provided address.
    /// bbport: `whole_only`: only an image the write covers whole (a partial write must keep the
    /// image's other GPU-only texels; upstream bloodborne_pc 0.6 makes that the rule).
    void InvalidateMemoryFromGPU(VAddr address, size_t max_size, bool whole_only = false);

    /// Evicts any images that overlap the unmapped range.
    void UnmapMemory(VAddr cpu_addr, size_t size);

    /// Schedules a copy of pending images for download back to CPU memory.
    void ProcessDownloadImages();

    /// Retrieves the image handle of the image with the provided attributes.
    [[nodiscard]] ImageId FindImage(ImageDesc& desc, bool exact_fmt = false);

    /// Retrieves image whose address matches provided
    [[nodiscard]] ImageId FindImageFromRange(VAddr address, size_t size, bool ensure_valid = true);

    /// The single-level array (or cube) image one of whose layers is exactly
    /// [address, address + size), and that layer.
    [[nodiscard]] ImageId FindImageLayer(VAddr address, size_t size, u32& layer);

    /// bbport: a FindView result remembered by the caller for the same image and view info.
    struct ViewMemo {
        ImageId image_id{};
        const void* backing = nullptr;
        ImageViewId view_id{};
    };

    /// Retrieves an image view with the properties of the specified image id.
    /// `refresh` false: an image found out of date is used as is (the texture binding helper
    /// checked it beforehand; only a guest write racing with the draw can make it so).
    [[nodiscard]] ImageView& FindTexture(ImageId image_id, const ImageDesc& desc,
                                         ViewMemo* memo = nullptr, bool refresh = true);

    /// Retrieves the render target with specified properties
    [[nodiscard]] ImageView& FindRenderTarget(ImageId image_id, const ImageDesc& desc);

    /// Retrieves the depth target with specified properties
    [[nodiscard]] ImageView& FindDepthTarget(ImageId image_id, const ImageDesc& desc);

    /// bbport: whether UpdateImage has nothing to do for this image (its fast path).
    [[nodiscard]] bool IsUpToDate(ImageId image_id) const {
        const Image& image = slot_images[image_id];
        const u32 flags = std::atomic_ref<u32>(const_cast<u32&>(reinterpret_cast<const u32&>(image.flags)))
                              .load(std::memory_order_acquire);
        constexpr u32 Dirty = static_cast<u32>(ImageFlagBits::Dirty);
        constexpr u32 Registered = static_cast<u32>(ImageFlagBits::Registered);
        return (flags & (Dirty | Registered)) == Registered &&
               image.track_addr == image.guest_begin && image.track_addr_end == image.guest_end &&
               image.lru_touched_tick == gc_tick;
    }

    /// Updates image contents if it was modified by CPU.
    void UpdateImage(ImageId image_id) {
        // bbport: a clean image already tracked and touched in this GC period needs nothing.
        // Every texture binding comes here; the mutex (shared with the fault handlers of the
        // guest threads) was ~3% of the GPU thread. Flags are read atomically: an invalidation
        // racing with this check races the same way with the locked path.
        if (!BbToggle::Disabled(BbToggle::UpdateImageFastPath)) {
            const Image& image = slot_images[image_id];
            const u32 flags = std::atomic_ref<u32>(const_cast<u32&>(reinterpret_cast<const u32&>(image.flags)))
                                  .load(std::memory_order_acquire);
            constexpr u32 Dirty = static_cast<u32>(ImageFlagBits::Dirty);
            constexpr u32 Registered = static_cast<u32>(ImageFlagBits::Registered);
            if ((flags & (Dirty | Registered)) == Registered &&
                image.track_addr == image.guest_begin && image.track_addr_end == image.guest_end &&
                image.lru_touched_tick == gc_tick) {
                return;
            }
        }
        std::scoped_lock lock{mutex};
        Image& image = slot_images[image_id];
        TrackImage(image_id);
        WatchImage(image, "update image (slow path)");
        TouchImage(image);
        RefreshImage(image);
    }

    /// Resolves overlap between existing cache image and pending merged image
    [[nodiscard]] std::tuple<ImageId, int, int> ResolveOverlap(const ImageInfo& info,
                                                               BindingType binding,
                                                               ImageId cache_img_id,
                                                               ImageId merged_image_id);

    /// Resolves depth overlap and either re-creates the image or returns existing one
    [[nodiscard]] ImageId ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                              ImageId cache_img_id);

    /// Creates a new image with provided image info and copies subresources from image_id
    [[nodiscard]] ImageId ExpandImage(const ImageInfo& info, ImageId image_id);

    /// Reuploads image contents.
    void RefreshImage(Image& image);
    void WatchImage(const Image& image, const char* what);

    /// Retrieves the sampler that matches the provided S# descriptor.
    /// extra_lod_bias: bbport, added to the S#'s bias (reduced scene rendering).
    [[nodiscard]] vk::Sampler GetSampler(const AmdGpu::Sampler& sampler,
                                         AmdGpu::BorderColorBuffer border_color_base,
                                         bool is_depth, float extra_lod_bias = 0.0f);

    /// Retrieves the image with the specified id.
    Image* TryGetImage(ImageId id, u64 uid) {
        if (!slot_images.is_allocated(id)) return nullptr;
        auto& image = slot_images[id];
        return !uid || image.image_uid == uid ? &image : nullptr;
    }
    [[nodiscard]] Image& GetImage(ImageId id) {
        auto& image = slot_images[id];
        TouchImage(image);
        return image;
    }

    /// Retrieves the image view with the specified id.
    [[nodiscard]] ImageView& GetImageView(ImageId id) {
        return slot_image_views[id];
    }

    /// Get the associated depth stencil image if it is still valid.
    ImageId GetAssociatedDepth(Image& image) {
        if (!image.depth_id) {
            return {};
        }
        if (slot_images.is_allocated(image.depth_id)) {
            auto& depth_image = slot_images[image.depth_id];
            if (depth_image.image_uid == image.depth_uid &&
                depth_image.flags & ImageFlagBits::Registered) {
                return image.depth_id;
            }
        }
        // The linked depth image is no longer valid, disassociate it.
        image.DisassociateDepth();
        return {};
    }

    enum class MetaType {
        CMask,
        FMask,
        HTile,
    };

    /// Returns meta type if the specified address is a metadata surface.
    std::optional<MetaType> IsMeta(VAddr address) const {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it->second.type;
        }
        return std::nullopt;
    }

    /// Returns true if a slice of the specified metadata surface has been cleared.
    bool IsMetaCleared(VAddr address, u32 slice) const {
        const auto& it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it.value().clear_mask & (1u << slice);
        }
        return false;
    }

    /// Clears all slices of the specified metadata surface.
    bool ClearMeta(VAddr address) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            it.value().clear_mask = u32(-1);
            return true;
        }
        return false;
    }

    /// Updates the state of a slice of the specified metadata surface.
    bool TouchMeta(VAddr address, u32 slice, bool is_clear) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            if (is_clear) {
                it.value().clear_mask |= 1u << slice;
            } else {
                it.value().clear_mask &= ~(1u << slice);
            }
            return true;
        }
        return false;
    }

    /// Runs the garbage collector.
    void RunGarbageCollector();

    /// bbport BB_TEX_HOME (bbport_tex_home.cpp, docs/ANDROID_UNIFIED_MEMORY_PLAN.md "Plan A"): the
    /// homing pass of the collector (GPU command thread, the draw pipe drained): verifies re-tiles
    /// started before and homes what matched, starts a few new ones.
    void TexHomePass();
    /// The home `ticket` back in the game's memory, from any thread: inline on the GPU threads,
    /// else by the GPU command thread while this one waits (`context`: the signal's, or null).
    /// True when the home is gone afterwards.
    bool TexHomeRehydrateFrom(u32 ticket, const void* context, BbTexHome::Rebuild why);
    /// Every home whose image overlaps guest [addr, addr + size) back (any thread, no lock held).
    void TexHomeRehydrateRange(VAddr addr, u64 size, BbTexHome::Rebuild why);
    /// A fault on a homed page (BbTexHome::fault_handler).
    bool TexHomeOnFault(void* context, VAddr addr);

    /// bbport: VRAM ran out (BufferCache::AllocateResidency, any thread): for the next 30 s the
    /// collector evicts by submissions under pressure again, as before 0.5, instead of keeping
    /// textures unused for seconds.
    static void NoteVramShort();

    template <typename Func>
    void ForEachImageInRegion(VAddr cpu_addr, size_t size, Func&& func) {
        using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        ImageIds images;
        ForEachPage(cpu_addr, size, [this, &images, cpu_addr, size, func](u64 page) {
            const auto it = page_table.find(page);
            if (it == nullptr) {
                if constexpr (BOOL_BREAK) {
                    return false;
                } else {
                    return;
                }
            }
            for (const ImageId image_id : *it) {
                Image& image = slot_images[image_id];
                if (image.flags & ImageFlagBits::Picked) {
                    continue;
                }
                if (!image.Overlaps(cpu_addr, size)) {
                    continue;
                }
                image.flags |= ImageFlagBits::Picked;
                images.push_back(image_id);
                if constexpr (BOOL_BREAK) {
                    if (func(image_id, image)) {
                        return true;
                    }
                } else {
                    func(image_id, image);
                }
            }
            if constexpr (BOOL_BREAK) {
                return false;
            }
        });
        for (const ImageId image_id : images) {
            slot_images[image_id].flags &= ~ImageFlagBits::Picked;
        }
    }

private:
    /// Iterate over all page indices in a range
    template <typename Func>
    static void ForEachPage(PAddr addr, size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> Traits::PageBits;
        for (u64 page = addr >> Traits::PageBits; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    /// Copies image memory back to CPU.
    void DownloadImageMemory(ImageId image_id, bool sync = false);

    /// Thread function for copying downloaded images out to CPU memory.
    void DownloadedImagesThread(const std::stop_token& token);

    /// Create an image from the given parameters
    [[nodiscard]] ImageId InsertImage(const ImageInfo& info, VAddr cpu_addr);

    /// Register image in the page table
    void RegisterImage(ImageId image);

    /// Unregister image from the page table
    void UnregisterImage(ImageId image);

    /// Track CPU reads and writes for image
    void TrackImage(ImageId image_id);
    void TrackImageHead(ImageId image_id);
    void TrackImageTail(ImageId image_id);

    /// Stop tracking CPU reads and writes for image
    void UntrackImage(ImageId image_id);
    void UntrackImageHead(ImageId image_id);
    void UntrackImageTail(ImageId image_id);

    void MarkAsMaybeDirty(ImageId image_id, Image& image);

    /// Removes the image and any views/surface metas that reference it.
    void DeleteImage(ImageId image_id);

    /// Touch the image in the LRU cache.
    void TouchImage(const Image& image);

    void FreeImage(ImageId image_id) {
        if (homes_count.load(std::memory_order_relaxed) != 0) {
            TexHomeOnFree(image_id); // bbport BB_TEX_HOME: its bytes back first
        }
        UntrackImage(image_id);
        UnregisterImage(image_id);
        DeleteImage(image_id);
    }

    void GarbageCollectImages();
    void GarbageCollectSamplers();

    /// bbport BB_TEX_HOME (bbport_tex_home.cpp).
    struct HomeRecord {
        ImageId image_id{};
        u64 image_uid = 0;
        VAddr address = 0; ///< the homed pages: [address, address + size), inside the image
        u64 size = 0;
        /// The game's padding bytes the re-tile does not have (zeros there): {offset from the
        /// image's start, length} runs and their bytes, put back on every rebuild.
        std::vector<std::pair<u32, u32>> patch_runs;
        std::vector<u8> patch_bytes;
    };
    struct HomeVerify {
        ImageId image_id{};
        u64 image_uid = 0;
        u64 tick = 0; ///< the re-tile is in the staging once the GPU is past it
        u32 stamp = 0;
        Vulkan::StagingBufferRef staging{}; ///< padding zeros (what a rebuild makes)
        /// A second re-tile, padding 0xff and tiled bytes no texel lands on 0x5a: bytes that differ
        /// from the first are padding, or holes (the tiling is not one-to-one there: refused).
        Vulkan::StagingBufferRef staging_ones{};
    };
    /// The home's image re-tiled into the game's memory (a GPU thread, stage B idle or on it).
    bool TexHomeRehydrate(u32 ticket, BbTexHome::Rebuild why);
    /// Its home back if the image has one (GPU thread).
    void TexHomeRehydrateImage(ImageId image_id, BbTexHome::Rebuild why);
    /// The ticket of the image's home, 0 none.
    u32 TexHomeTicketOf(ImageId image_id);
    void TexHomeOnFree(ImageId image_id);
    /// A new image over [addr, addr + size) (`mutex` held): homed pages there back, on the GPU threads.
    void TexHomeOnRegister(VAddr addr, u64 size);
    /// Before UnmapMemory locks: the homes of every image it will free back (any thread).
    void TexHomeBeforeUnmap(VAddr addr, u64 size);
    bool TexHomeEligible(const Image& image, u32 now, BbTexHome::Refusal& why);
    /// The image re-tiled into a host-cached staging buffer, once the commands recorded now have
    /// run: `fill` where the linear copy has no texel (padding), `hole_fill` where no linear byte
    /// lands in the tiled layout.
    Vulkan::StagingBufferRef TexHomeTile(Image& image, bool deferred, u32 fill = 0,
                                         u32 hole_fill = 0);
    /// The re-tiles the GPU is done with: compared and homed (or refused).
    void TexHomeFinish(HomeVerify& verify);
    /// Homes [begin, end) of the image with `expected` (the re-tile with the patches applied).
    void TexHomeCommit(HomeVerify& verify, Image& image, VAddr begin, VAddr end, const u8* expected,
                       std::vector<std::pair<u32, u32>> patch_runs, std::vector<u8> patch_bytes);
    /// BB_TEX_HOME_LOG=1: one line on an image whose re-tile differed.
    void TexHomeLogDiff(const Image& image, VAddr begin, VAddr end, const u8* zeros, const u8* ones,
                        const u8* guest, u64 holes, const char* verdict);

public:
    /// bbport BB_TEX_HOME_FIRST: the bytes of [addr, addr + size) under registered sampled textures
    /// (any thread; nullopt when the cache is busy).
    std::optional<u64> SampledTextureBytes(VAddr addr, u64 size);

private:
    /// After `mutex`, the runtime's lock after it. Recursive: a rebuild's Finish() may run deferred
    /// work that writes guest memory, which may ask for another rebuild on this thread.
    std::recursive_mutex home_mutex;
    tsl::robin_map<u32, HomeRecord> homes;  ///< by ticket
    tsl::robin_map<u32, u32> home_of_image; ///< ImageId index -> ticket
    std::atomic<u32> homes_count{0};
    std::vector<HomeVerify> home_verifies; ///< `mutex`
    u32 home_cursor = 0;
    std::chrono::steady_clock::time_point home_last_scan{};

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    Vulkan::Runtime& runtime;
    AmdGpu::Liverpool* liverpool;
    BufferCache& buffer_cache;
    PageManager& tracker;
    BlitHelper blit_helper;
    TileManager tile_manager;
    Common::SlotVector<Image> slot_images;
    Common::SlotVector<ImageView> slot_image_views;
    tsl::robin_map<u64, Sampler> samplers;
    std::unordered_set<ImageId> download_images;
    u64 total_used_memory = 0;
    u64 gc_evictions = 0, gc_downloads = 0; ///< bbport: pressure report
    std::chrono::steady_clock::time_point gc_report_time{};
    /// bbport: gc_tick at each of the last 64 seconds (GarbageCollectImages).
    std::array<u64, 64> gc_tick_at_second{};
    u64 gc_second = 0;
    u64 trigger_gc_memory = 0;
    u64 pressure_gc_memory = 0;
    u64 critical_gc_memory = 0;
    u64 total_used_samplers = 0;
    u64 trigger_gc_samplers = 0;
    u64 pressure_gc_samplers = 0;
    u64 critical_gc_samplers = 0;
    u64 gc_tick = 0;
    Common::LeastRecentlyUsedCache<ImageId, u64> lru_cache;
    Common::LeastRecentlyUsedCache<u64, u64> sampler_lru_cache;
    const bool readback_linear_images;
    PageTable page_table;
    std::mutex mutex;
    static inline std::atomic<u64> vram_short_until{0}; ///< steady seconds (NoteVramShort)
    // bbport: FindImage results for unchanged image registrations (guarded by `mutex`).
    struct FindImageCacheEntry {
        VAddr address = 0;
        u64 size = 0;
        Extent3D extent{};
        vk::Format format{};
        AmdGpu::ImageType type{};
        bool exact_fmt = false;
        BindingType binding{};
        u32 levels = 0;
        u32 layers = 0;
        u64 generation = ~0ULL;
        ImageId image_id{};
        int view_mip = -1;
        int view_slice = -1;
    };
    std::array<FindImageCacheEntry, 1024> find_image_cache{};
    std::atomic<u64> registry_generation{0};
    std::mutex samplers_mutex;
    std::mutex download_images_mutex;
    std::atomic<bool> downloads_queued{false}; ///< download_images may be non-empty (checked without the lock)
    struct MetaDataInfo {
        MetaType type;
        s32 clear_mask = -1;
    };
    tsl::robin_map<VAddr, MetaDataInfo> surface_metas;
};

} // namespace VideoCore
