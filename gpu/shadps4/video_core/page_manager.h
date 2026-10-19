// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include "common/alignment.h"
#include "common/types.h"
#include "video_core/buffer_cache//region_definitions.h"

namespace Vulkan {
class Rasterizer;
}

namespace VideoCore {

struct UffdImpl;
struct SignalImpl;

class PageManager {
    // PAGE_SIZE and PAGE_BITS conflicts with machine/param.h definitions on freebsd!
    // Use the same page size as the tracker.
    static constexpr size_t PM_PAGE_BITS = TRACKER_PAGE_BITS;
    static constexpr size_t PM_PAGE_SIZE = TRACKER_BYTES_PER_PAGE;

    // Keep the lock granularity the same as region granularity. (since each regions has
    // itself a lock)
    static constexpr size_t PAGES_PER_LOCK = NUM_PAGES_PER_REGION;

public:
    explicit PageManager(Vulkan::Rasterizer* rasterizer);
    ~PageManager();

    /// bbport: the guest code behind write faults on tracked pages over the last window (frame
    /// stats): which game functions write GPU memory, to replace tracking by explicit hooks.
    static void ReportFaultSites();
    /// bbport: the write fault being handled on this thread hit an image (diagnostics: which guest
    /// code writes textures, BB_WRITE_TRACKING=1).
    static void NoteImageFault(VAddr image_address, u64 image_size, u32 width, u32 height,
                               u32 format, u32 tiling);

    /// Register a range of mapped gpu memory.
    void OnGpuMap(VAddr address, size_t size);

    /// Unregister a range of gpu memory that was unmapped.
    void OnGpuUnmap(VAddr address, size_t size);

    /// bbport: whether a watcher write-protects the page at `address` (a lock-free read: a stale
    /// answer costs a fault or a call that finds nothing to do).
    [[nodiscard]] bool IsWriteProtected(VAddr address) const;

    /// bbport BB_UMA (BbUma::Dynamic): [addr, addr + size) is bound in place through a shared
    /// granule's buffer: the 2 MiB regions it touches, within [clip_begin, clip_end) (its direct
    /// mapping and granule) in whole 64 KiB blocks, get the one-shot write trap
    /// (BbLayer::WriteTraps::UmaWatch) where not armed or dynamic yet, each region at once. Pages
    /// this manager denies reads are left out; they get it when the manager lets them go
    /// (ApplyProtect). False when a block of the range stays unarmed (BbUma::Maps cap): the range
    /// must not be bound in place.
    bool UmaArm(VAddr addr, u64 size, VAddr clip_begin, VAddr clip_end) const;
    /// bbport BB_UMA: the CPU writes, or is about to write, [addr, addr + size) (a write fault, a
    /// libc copy, a file read): the armed regions it touches become dynamic whole, and their trap is
    /// lifted on every page of them this manager does not restrict itself (on the others when it
    /// lets them go).
    void UmaNoteWrite(VAddr addr, u64 size) const;

    /// Updates watches in the pages touching the specified region.
    template <bool track>
    void UpdatePageWatchers(VAddr addr, u64 size) const;

    /// Updates watches in the pages touching the specified region using a mask.
    template <bool track, bool is_read = false>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const;

    /// Returns page aligned address.
    static constexpr VAddr GetPageAddr(VAddr addr) {
        return Common::AlignDown(addr, PM_PAGE_SIZE);
    }

    /// Returns address of the next page.
    static constexpr VAddr GetNextPageAddr(VAddr addr) {
        return Common::AlignUp(addr + 1, PM_PAGE_SIZE);
    }

private:
    friend struct UffdImpl;
    friend struct SignalImpl;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VideoCore
