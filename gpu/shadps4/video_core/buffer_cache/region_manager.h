// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>

#include "bbport_toggles.h"
#include "bbport_fault.h"
#include "common/div_ceil.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"

#ifdef __unix__
#include "common/adaptive_mutex.h"
#endif
// bbport: Bionic has no PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP: LockType falls back to a lock that spins
// briefly and then sleeps (Common::SleepMutex; BB_FAULT_SPINLOCK=1: spins as Common::SpinLock did).
#include "common/spin_lock.h"
#include "common/debug.h"
#include "common/types.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/page_manager.h"

namespace VideoCore {

#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
using LockType = Common::AdaptiveMutex;
#else
using LockType = Common::SleepMutex;
#endif

/**
 * Allows tracking CPU and GPU modification of pages in a contigious 16MB virtual address region.
 * Information is stored in bitsets for spacial locality and fast update of single pages.
 */
class RegionManager {
public:
    explicit RegionManager(PageManager* tracker_, VAddr cpu_addr_)
        : tracker{tracker_}, cpu_addr{cpu_addr_} {
        cpu.Fill();
        gpu.Clear();
        writeable.Fill();
        readable.Fill();
    }
    explicit RegionManager() = default;

    void SetCpuAddress(VAddr new_cpu_addr) {
        cpu_addr = new_cpu_addr;
    }

    VAddr GetCpuAddr() const {
        return cpu_addr;
    }

    static constexpr size_t SanitizeAddress(size_t address) {
        return static_cast<size_t>(std::max<s64>(static_cast<s64>(address), 0LL));
    }

    template <Type type>
    RegionBits& GetRegionBits() noexcept {
        if constexpr (type == Type::CPU) {
            return cpu;
        } else if constexpr (type == Type::GPU) {
            return gpu;
        }
    }

    template <Type type>
    const RegionBits& GetRegionBits() const noexcept {
        if constexpr (type == Type::CPU) {
            return cpu;
        } else if constexpr (type == Type::GPU) {
            return gpu;
        }
    }

    /**
     * Change the state of a range of pages
     *
     * @param dirty_addr    Base address to mark or unmark as modified
     * @param size          Size in bytes to mark or unmark as modified
     */
    template <Type type, bool enable>
    void ChangeRegionState(u64 dirty_addr, u64 size) noexcept(type == Type::GPU) {
        RENDERER_TRACE;
        const size_t offset = dirty_addr - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }

        RegionBits& bits = GetRegionBits<type>();
        if constexpr (type == Type::CPU && enable) {
            CountWriteFaults(start_page, end_page);
            last_cpu_write_frame = BbStats::frame_number.load(std::memory_order_relaxed);
            NotePreuploadChurn();
        }
        // bbport: data the GPU writes is no longer a hot page (the guest must trap on it again).
        [[maybe_unused]] bool hot_dropped = false;
        if constexpr (type == Type::GPU && enable) {
            hot_dropped = DropHotPages(start_page, end_page);
        }
        if constexpr (enable) {
            bits.SetRange(start_page, end_page);
        } else {
            bits.UnsetRange(start_page, end_page);
        }
        if constexpr (type == Type::CPU) {
            UpdateProtection<!enable, false>();
        } else {
            if (hot_dropped) {
                UpdateProtection<true, false>();
            }
            if (EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise) {
                UpdateProtection<enable, true>();
            }
        }
    }

    /// bbport: after a guest write fault, also unprotects the other pages of the window that
    /// are protected for CPU writes and hold no GPU-modified data (see MarkFaultWindow).
    void ExtendWriteFault(VAddr window_addr, u64 size) {
        const size_t offset = window_addr - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }
        RegionBits add(~(gpu | writeable), start_page, end_page);
        if (add.None()) {
            return;
        }
        cpu |= add;
        UpdateProtection<false, false>();
    }

    /**
     * Loop over each page in the given range, turn off those bits and notify the tracker if
     * needed. Call the given function on each turned off range.
     *
     * @param query_cpu_range Base CPU address to loop over
     * @param size            Size in bytes of the CPU range to loop over
     * @param func            Function to call for each turned off region
     */
    template <Type type, bool clear>
    void ForEachModifiedRange(VAddr query_cpu_range, s64 size, auto&& func) {
        RENDERER_TRACE;
        if constexpr (type == Type::CPU) {
            RefreshHot(); // before the early exit below: hot pages count as modified once per epoch
        }
        const size_t offset = query_cpu_range - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }

        RegionBits& bits = GetRegionBits<type>();
        // bbport: nothing modified in the range means nothing to clear or re-protect (CPU bits
        // always match `writeable` after a change); this runs for every buffer binding, so
        // it is checked before building the range mask.
        if (type == Type::CPU && !bits.AnyInRange(start_page, end_page) &&
            !BbToggle::Disabled(BbToggle::PageTrackingEarlyExit)) {
            return;
        }
        RegionBits mask(bits, start_page, end_page);

        if constexpr (clear) {
            bits.UnsetRange(start_page, end_page);
            if constexpr (type == Type::CPU) {
                UpdateProtection<true, false>(); // hot pages stay writable (target cpu | hot)
            } else if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled) {
                UpdateProtection<false, true>();
            }
        }

        for (const auto& [start, end] : mask) {
            func(cpu_addr + start * TRACKER_BYTES_PER_PAGE, (end - start) * TRACKER_BYTES_PER_PAGE);
        }
    }

    /**
     * Returns true when a region has been modified
     *
     * @param offset Offset in bytes from the start of the buffer
     * @param size   Size in bytes of the region to query for modifications
     */
    /// bbport: every page of the region modified. Lock-free callers are fine: for CPU bits, hot
    /// pages a refresh is due for count as modified (see HotRefreshDue), without changing anything.
    template <Type type>
    [[nodiscard]] bool IsRegionFullyModified(u64 offset, u64 size) noexcept {
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return true;
        }
        if constexpr (type == Type::CPU) {
            if (HotRefreshDue()) {
                std::atomic_thread_fence(std::memory_order_acquire); // see ExpireHot
                return (cpu | (hot & ~gpu))
                    .AllInRange(start_page, std::min<size_t>(end_page, NUM_PAGES_PER_REGION));
            }
        }
        return GetRegionBits<type>().AllInRange(start_page, std::min<size_t>(end_page, NUM_PAGES_PER_REGION));
    }

    template <Type type>
    [[nodiscard]] bool IsRegionModified(u64 offset, u64 size) noexcept {
        RENDERER_TRACE;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return false;
        }

        const RegionBits& bits = GetRegionBits<type>();
        if constexpr (type == Type::CPU) {
            // bbport: callers do not hold the lock (the upload check of every buffer binding), so
            // this cannot refresh; it answers as the refresh that ForEachModifiedRange will do.
            if (HotRefreshDue()) {
                if ((hot & ~gpu).AnyInRange(start_page, end_page)) {
                    return true;
                }
                std::atomic_thread_fence(std::memory_order_acquire); // see ExpireHot
            }
        }
        return bits.AnyInRange(start_page, end_page);
    }

    /// bbport (pre-upload): whether some page is CPU-modified but not GPU-modified. With the lock
    /// held. Hot pages do not count: the game rewrites them every frame and they are uploaded
    /// when drawn.
    [[nodiscard]] bool HasCpuOnlyPages() noexcept {
        RefreshHot();
        return (cpu & ~gpu & ~hot).Any();
    }
    /// bbport (pre-upload): the frame of the newest guest write seen in this region.
    u32 last_cpu_write_frame = 0;
    /// bbport (pre-upload): when it was last pre-uploaded, and no pre-upload before this frame:
    /// a region the game writes again soon after a pre-upload backs off, longer each time.
    u32 preupload_frame = 0;
    u32 preupload_backoff_until = 0;
    u8 preupload_strikes = 0;
    u32 preupload_count = 0;
    void NotePreuploadChurn() noexcept {
        constexpr u32 ChurnFrames = 600; // ~10 s: regions the game rewrites every few seconds
        if (preupload_frame == 0 || last_cpu_write_frame - preupload_frame > ChurnFrames) {
            return;
        }
        // One quick rewrite may be the game loading new data there (worth pre-uploading); from
        // the second on it is data it rewrites now and then.
        preupload_strikes = std::min<u8>(preupload_strikes + 1, 7);
        if (preupload_strikes >= 2) {
            preupload_backoff_until = last_cpu_write_frame + (240u << (preupload_strikes - 1));
        }
        preupload_frame = 0;
    }

    LockType lock;

private:
    /**
     * Notify tracker about changes in the CPU tracking state of a word in the buffer
     *
     * @param word_index   Index to the word to notify to the tracker
     * @param current_bits Current state of the word
     * @param new_bits     New state of the word
     *
     * @tparam track True when the tracker should start tracking the new pages
     */
    template <bool track, bool is_read>
    void UpdateProtection() {
        RENDERER_TRACE;
        // Hot pages are always writable: the write side protects what is neither CPU-modified nor hot.
        RegionBits mask = is_read ? (~gpu ^ readable) : ((cpu | hot) ^ writeable);
        if (mask.None()) {
            return;
        }
        if constexpr (is_read) {
            readable = ~gpu;
        } else {
            writeable = cpu | hot;
        }
        tracker->UpdatePageWatchersForRegion<track, is_read>(cpu_addr, mask);
    }

    // bbport: hot pages. Pages the guest writes again and again (per-frame constants, skinning
    // output) cost a protection fault in the writing thread plus an mprotect with TLB shootdowns on
    // every upload. A fault window (BB_FAULT_WINDOW, 256 KiB) that takes write faults in HotStreak
    // (BB_HOT_STREAK, 3) consecutive frames is promoted: its pages that are not GPU-modified stay
    // writable (target cpu | hot) and are not trapped any more, so they are assumed modified and
    // uploaded again at every epoch (RefreshHot) instead of at every fault. Default on
    // (BB_HOT_PAGES=0: off; the live toggle BbToggle::HotPages too).
    // The first attempt (PC, BB_HOT_PAGES=1, commit b90a781, docs/parallel_gpu.md) failed: pages
    // became hot after 2 faults, each was uploaded again at EVERY binding (cpu |= hot on every
    // upload), and the set was unbounded (~14k pages, 56 MB in Hunter's Nightmare; 33 FPS, then a
    // GPU ring timeout). What differs now:
    //  - a window needs a streak of consecutive frames, not two faults at any time;
    //  - a global cap, BB_HOT_PAGES_MB (8) MiB over all regions (BbStats::hot_pages); a window that
    //    does not fit is refused (BbStats::hot_refused) and retried after a new streak;
    //  - the re-upload is once per epoch (BbStats::hot_epoch), not per binding. The epoch is bumped
    //    where the game may have written data the GPU reads next, without a trap: every guest
    //    submission, every wait on a label the guest writes (WaitRegMem), every flip. Bindings
    //    inside one epoch find the hot pages already uploaded;
    //  - the set expires after BB_HOT_PERIOD_S (10) seconds, so a scene change drops its pages, and
    //    pages the GPU writes leave it at once (ChangeRegionState<GPU, true>).
    // All of this state is under the region lock; the lock-free readers use HotRefreshDue.
    static bool HotPagesEnabled() {
        // bbport: opt-in (BB_HOT_PAGES=1) until it has been tested on a device; off is today's
        // write tracking.
        static const bool enabled = [] {
            const char* env = std::getenv("BB_HOT_PAGES");
            return env && env[0] == '1';
        }();
        return enabled && !BbToggle::Disabled(BbToggle::HotPages);
    }
    static u64 EnvValue(const char* name, u64 fallback) {
        const char* env = std::getenv(name);
        char* end = nullptr;
        const u64 value = env ? std::strtoull(env, &end, 10) : 0;
        return env && end != env ? value : fallback;
    }
    /// Consecutive frames with write faults in a window before it is promoted.
    static u8 HotStreak() {
        static const u8 streak = static_cast<u8>(std::clamp<u64>(EnvValue("BB_HOT_STREAK", 3), 1, 255));
        return streak;
    }
    /// All regions together, in pages.
    static s64 HotCapPages() {
        static const s64 cap =
            static_cast<s64>(std::min<u64>(EnvValue("BB_HOT_PAGES_MB", 8), 1 << 20) * 256);
        return cap;
    }
    static std::chrono::seconds HotPeriod() {
        static const std::chrono::seconds period(std::min<u64>(EnvValue("BB_HOT_PERIOD_S", 10), 86400));
        return period;
    }
    /// The unit of promotion: the pages of one fault window.
    static size_t HotWindowPages() {
        static const size_t pages = std::clamp<u64>(FaultWindowBytesFromEnv() / TRACKER_BYTES_PER_PAGE,
                                                    1, NUM_PAGES_PER_REGION);
        return pages;
    }
    static size_t CountPages(const RegionBits& bits) {
        size_t count = 0;
        for (const auto& [start, end] : bits) {
            count += end - start;
        }
        return count;
    }

    /// Whether the next RefreshHot marks hot pages CPU-modified (a new epoch, or the set is due to
    /// expire). Lock-free: for readers that cannot refresh.
    bool HotRefreshDue() const noexcept {
        if (num_hot.load(std::memory_order_acquire) == 0) {
            return false;
        }
        return hot_epoch_seen.load(std::memory_order_acquire) !=
                   BbStats::hot_epoch.load(std::memory_order_relaxed) ||
               !HotPagesEnabled();
    }

    /// With the lock: once per epoch, hot pages count as CPU-modified again (the guest may have
    /// written them without a trap); and the set expires when it is switched off or too old.
    /// The clock is read only when the epoch has changed, and only for regions with hot pages.
    void RefreshHot() {
        if (num_hot.load(std::memory_order_relaxed) == 0) {
            return;
        }
        const u32 epoch = BbStats::hot_epoch.load(std::memory_order_relaxed);
        const bool enabled = HotPagesEnabled();
        if (enabled && hot_epoch_seen.load(std::memory_order_relaxed) == epoch) {
            return;
        }
        if (!enabled || std::chrono::steady_clock::now() - hot_since > HotPeriod()) {
            ExpireHot();
            return;
        }
        cpu |= hot & ~gpu;
        hot_epoch_seen.store(epoch, std::memory_order_release);
    }

    /// The hot pages go back to normal pages: modified now (the guest wrote them untrapped), and
    /// protected again by the next upload (the target no longer includes them).
    void ExpireHot() {
        cpu |= hot;
        // Lock-free readers that find the hot bits gone must find the CPU bits set.
        std::atomic_thread_fence(std::memory_order_release);
        BbStats::hot_pages.fetch_sub(static_cast<s64>(num_hot.load(std::memory_order_relaxed)),
                                     std::memory_order_relaxed);
        BbStats::hot_expired.fetch_add(hot_windows, std::memory_order_relaxed);
        hot.Clear();
        hot_since = {};
        hot_windows = 0;
        hot_window_state.reset();
        num_hot.store(0, std::memory_order_release);
        UpdateProtection<true, false>();
    }

    /// GPU writes in the page range: its hot pages are normal pages again (the caller updates the
    /// protection). Returns whether there were any.
    bool DropHotPages(size_t start_page, size_t end_page) {
        const u32 count = num_hot.load(std::memory_order_relaxed);
        if (count == 0) {
            return false;
        }
        const RegionBits dropped(hot, start_page, end_page);
        const size_t n = CountPages(dropped);
        if (n == 0) {
            return false;
        }
        hot &= ~dropped;
        BbStats::hot_pages.fetch_sub(static_cast<s64>(n), std::memory_order_relaxed);
        if (n == count) {
            hot_windows = 0;
            hot_window_state.reset();
        }
        num_hot.store(count - static_cast<u32>(n), std::memory_order_release);
        return true;
    }

    void CountWriteFaults(size_t start_page, size_t end_page) {
        const bool hot_enabled = HotPagesEnabled();
        const size_t window_pages = HotWindowPages();
        const u32 frame = BbStats::frame_number.load(std::memory_order_relaxed);
        size_t last_window = ~size_t{0};
        for (size_t page = start_page; page < end_page; ++page) {
            if (writeable.Get(page)) {
                continue; // not protected: no fault
            }
            BbStats::tracker_faults.fetch_add(1, std::memory_order_relaxed);
            if (!hot_enabled || page / window_pages == last_window) {
                continue;
            }
            last_window = page / window_pages;
            NoteWindowFault(last_window, frame);
        }
    }

    /// A write fault in a window: a streak of consecutive frames with faults promotes it.
    void NoteWindowFault(size_t window, u32 frame) {
        const size_t window_pages = HotWindowPages();
        if (!hot_window_state) {
            hot_window_state = std::make_unique<HotWindow[]>(NUM_PAGES_PER_REGION / window_pages);
        }
        HotWindow& state = hot_window_state[window];
        if (state.streak != 0 && state.last_frame == frame) {
            return; // one fault a frame is enough
        }
        state.streak = state.streak != 0 && state.last_frame + 1 == frame
                           ? static_cast<u8>(std::min<u32>(state.streak + 1u, 255u))
                           : u8{1};
        state.last_frame = frame;
        if (state.streak < HotStreak()) {
            return;
        }
        state.streak = 0; // promoted or refused: a new streak before the next try
        const size_t begin = window * window_pages;
        const RegionBits added(~(gpu | hot), begin,
                               std::min<size_t>(begin + window_pages, NUM_PAGES_PER_REGION));
        const size_t n = CountPages(added);
        if (n == 0) {
            return;
        }
        // The cap holds over all regions: reserve the pages in the global count or refuse.
        s64 total = BbStats::hot_pages.load(std::memory_order_relaxed);
        do {
            if (total + static_cast<s64>(n) > HotCapPages()) {
                BbStats::hot_refused.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        } while (!BbStats::hot_pages.compare_exchange_weak(total, total + static_cast<s64>(n),
                                                           std::memory_order_relaxed));
        const u32 count = num_hot.load(std::memory_order_relaxed);
        if (count == 0) {
            hot_since = std::chrono::steady_clock::now();
            // Their bits are set below: nothing to refresh in this epoch.
            hot_epoch_seen.store(BbStats::hot_epoch.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
        }
        hot |= added;
        cpu |= added; // writable and untrapped from now on: assumed modified
        ++hot_windows;
        BbStats::hot_promoted.fetch_add(1, std::memory_order_relaxed);
        num_hot.store(count + static_cast<u32>(n), std::memory_order_release);
        UpdateProtection<false, false>();
    }

    struct HotWindow {
        u32 last_frame = 0; ///< the last frame with a write fault in the window
        u8 streak = 0;      ///< consecutive frames up to it
    };
    /// Allocated at the first write fault; one entry per fault window of the region.
    std::unique_ptr<HotWindow[]> hot_window_state;
    RegionBits hot{};
    /// Atomic for the lock-free readers (HotRefreshDue); written with the lock held.
    std::atomic<u32> num_hot{0};
    std::atomic<u32> hot_epoch_seen{0};
    u32 hot_windows = 0; ///< promoted windows still counted in the region (for BbStats::hot_expired)
    std::chrono::steady_clock::time_point hot_since{};

    PageManager* tracker;
    VAddr cpu_addr = 0;
    RegionBits cpu;
    RegionBits gpu;
    RegionBits writeable;
    RegionBits readable;
};

} // namespace VideoCore
