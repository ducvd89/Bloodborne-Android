// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: see vk_copy_filter.h.

// ~5000 copies of ~250 bytes a frame: hashed inline, not a library call each.
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bbport_toggles.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/renderer_vulkan/vk_copy_filter.h"

namespace Vulkan {

namespace {
std::atomic<bool> started{false};

constexpr u64 PageSize = 4096;

/// The game's memory, hashed. With precise readbacks the GPU-written pages are protected against
/// reads (a read fault would wait for the GPU thread, which may wait for this one): the backing
/// view then.
u64 HashGuest(u64 address, u64 size) {
    static const bool precise = EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise;
    if (!precise) {
        return XXH3_64bits(reinterpret_cast<const void*>(address), size);
    }
    thread_local std::vector<u8> bytes;
    bytes.resize(size);
    Core::Memory::Instance()->ReadBacking(address, bytes.data(), size);
    return XXH3_64bits(bytes.data(), size);
}
} // namespace

bool CopyFilter::On() {
    static const bool env = [] {
        const char* value = std::getenv("BB_COPY_SKIP_SAME");
        return !value || value[0] != '0';
    }();
    return env && CopyShaderIsHle();
}

bool CopyFilter::Started() {
    return started.load(std::memory_order_relaxed);
}

CopyFilter& CopyFilter::Get() {
    static CopyFilter instance;
    return instance;
}

CopyFilter::CopyFilter() : thread{[this] { Run(); }} {
    started.store(true, std::memory_order_relaxed);
    thread.detach();
}

void CopyFilter::Post(u64 packet, std::vector<CopyEntry>& entries) {
    const u64 at = head.load(std::memory_order_relaxed);
    // A slot is free once stage B took it and the checking thread checked it (either order).
    const u64 free_from =
        std::min(tail.load(std::memory_order_acquire), processed.load(std::memory_order_acquire));
    if (at - free_from >= jobs.size()) {
        return;
    }
    auto& job = jobs[at % jobs.size()];
    job.packet = packet;
    job.entries.swap(entries);
    head.store(at + 1, std::memory_order_release);
    head.notify_one();
}

CopyFilter::Job* CopyFilter::Take(u64 packet, bool& unchecked) {
    unchecked = false;
    for (;;) {
        const u64 at = tail.load(std::memory_order_relaxed);
        if (at == head.load(std::memory_order_acquire)) {
            Invalidate();
            return nullptr;
        }
        Job& job = jobs[at % jobs.size()];
        if (job.packet == packet) {
            if (processed.load(std::memory_order_acquire) <= at) {
                unchecked = true;
                tail.store(at + 1, std::memory_order_release);
                ++unchecked_calls;
                return nullptr;
            }
            return &job;
        }
        if (job.packet > packet) {
            Invalidate();
            return nullptr;
        }
        Invalidate();
        tail.store(at + 1, std::memory_order_release);
    }
}

void CopyFilter::Release() {
    tail.store(tail.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

void CopyFilter::Invalidate() {
    epoch.fetch_add(1, std::memory_order_acq_rel);
}

void CopyFilter::Run() {
    Common::SetCurrentThreadName("bb:CopyFilter");
    for (u64 at = 0;; ++at) {
        for (u64 h = head.load(std::memory_order_acquire); h <= at;
             h = head.load(std::memory_order_acquire)) {
            head.wait(h, std::memory_order_acquire);
        }
        Check(jobs[at % jobs.size()]);
        processed.store(at + 1, std::memory_order_release);
    }
}

void CopyFilter::Check(Job& job) {
    const u64 now_epoch = Epoch();
    if (now_epoch != history_epoch) {
        last.clear();
        ranges.clear();
        history_epoch = now_epoch;
    }
    job.epoch = now_epoch;
    const auto& entries = job.entries;
    job.control.clear();
    job.skipped_sources.clear();
    page_hashes.clear();
    spans.clear();
    u32 kept = 0;
    // The pages of a destination (one or two for a copy): hashed once a job.
    const auto pages_of = [&](const CopyEntry& e) {
        const u64 first = e.dst / PageSize, end = (e.dst + e.size - 1) / PageSize;
        u64 signature = 0;
        for (u64 page = first; page <= end; ++page) {
            auto [it, fresh] = page_hashes.try_emplace(page, 0);
            if (fresh) {
                it->second = HashGuest(page * PageSize, PageSize);
            }
            signature = signature * 0x9E3779B97F4A7C15ull + it->second;
        }
        return signature;
    };
    // In list order against the history as it stands: a copy into a destination overlapping a
    // later one takes that one's note away, and the later one runs.
    for (u32 i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        const u64 hash = HashGuest(e.src, e.size);
        const u64 pages = pages_of(e);
        const auto it = last.find(e.dst);
        if (it != last.end() && it->second.size == e.size && it->second.hash == hash &&
            it->second.pages == pages) {
            spans.emplace_back(e.src / PageSize * PageSize,
                               (e.src + e.size + PageSize - 1) / PageSize * PageSize);
            continue; // nothing changes
        }
        ++kept;
        job.control.resize(job.control.size() + sizeof(e.control));
        std::memcpy(job.control.data() + job.control.size() - sizeof(e.control), e.control,
                    sizeof(e.control));
        // Noted copies never overlap each other: one into exactly a noted destination updates it
        // in place (the ordered map only for new destinations).
        if (it != last.end() && it->second.size == e.size) {
            it->second = {e.size, hash, pages};
            continue;
        }
        auto lo = ranges.lower_bound(e.dst);
        if (lo != ranges.begin()) {
            auto prev = std::prev(lo);
            if (prev->first + prev->second > e.dst) {
                lo = prev;
            }
        }
        while (lo != ranges.end() && lo->first < e.dst + e.size) {
            last.erase(lo->first);
            lo = ranges.erase(lo);
        }
        ranges.emplace(e.dst, e.size);
        last[e.dst] = {e.size, hash, pages};
    }
    total += entries.size();
    left_out += entries.size() - kept;
    ++calls;
    job.kept = kept;
    if (kept == entries.size()) {
        job.control.clear(); // all kept: the call's own list
    }
    // The sources of the copies left out, merged.
    std::sort(spans.begin(), spans.end());
    size_t merged = 0;
    for (size_t i = 1; i < spans.size(); ++i) {
        if (spans[i].first <= spans[merged].second) {
            spans[merged].second = std::max(spans[merged].second, spans[i].second);
        } else {
            spans[++merged] = spans[i];
        }
    }
    spans.resize(spans.empty() ? 0 : merged + 1);
    for (const auto& span : spans) {
        job.skipped_sources.emplace_back(span.first, span.second - span.first);
    }
    if (last.size() > 200000) {
        Invalidate();
    }
    const u32 now = BbStats::coarse_second.load(std::memory_order_relaxed);
    if (now - printed >= 5) {
        static const bool where = std::getenv("BB_COPY_WHERE") != nullptr;
        if (printed != 0 && where) {
            std::printf("Copy filter (5 s): %llu of %llu copies left out in %llu calls; history "
                        "restarted %llu times; %llu calls ran unchecked\n",
                        (unsigned long long)left_out, (unsigned long long)total,
                        (unsigned long long)calls, (unsigned long long)(now_epoch - printed_epoch),
                        (unsigned long long)unchecked_calls.exchange(0));
        }
        printed = now;
        printed_epoch = now_epoch;
        total = left_out = calls = 0;
    }
}

} // namespace Vulkan
