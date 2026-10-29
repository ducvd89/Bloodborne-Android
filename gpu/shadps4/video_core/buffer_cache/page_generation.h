// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>

#include "common/types.h"
#include "video_core/buffer_cache/region_definitions.h"

namespace VideoCore {

/// bbport: a counter per guest 4 KiB page (hashed: two pages may share one, which only makes a
/// reader see a change that did not happen), bumped whenever the CPU data of the page is
/// uploaded over its VRAM copy (RegionManager::ForEachModifiedRange). Bytes cannot tell: a CPU
/// write of the same value uploads the page over what the GPU wrote there. BB_COPY_SKIP_SAME
/// reads it (vk_copy_filter.h).
class PageGeneration {
public:
    static void Bump(VAddr address, u64 size) noexcept {
        for (u64 page = address >> TRACKER_PAGE_BITS; page <= (address + size - 1) >> TRACKER_PAGE_BITS;
             ++page) {
            Slot(page).fetch_add(1, std::memory_order_acq_rel);
        }
    }
    [[nodiscard]] static u32 Get(u64 page) noexcept {
        return Slot(page).load(std::memory_order_acquire);
    }
    /// Signature of the counters of the pages under [address, address + size).
    [[nodiscard]] static u64 Signature(VAddr address, u64 size) noexcept {
        u64 signature = 0;
        for (u64 page = address >> TRACKER_PAGE_BITS; page <= (address + size - 1) >> TRACKER_PAGE_BITS;
             ++page) {
            signature = signature * 0x9E3779B97F4A7C15ull + Get(page) + 1;
        }
        return signature;
    }

private:
    static constexpr u64 SlotBits = 18;
    static std::atomic<u32>& Slot(u64 page) noexcept {
        static std::array<std::atomic<u32>, 1ull << SlotBits> table{};
        return table[(page * 0x9E3779B97F4A7C15ull) >> (64 - SlotBits)];
    }
};

} // namespace VideoCore
