// SPDX-License-Identifier: GPL-2.0-or-later
// bbport BB_LAYER_MEMORY: the BufferCache side of the layer's memory module
// (gpu/layer/bblayer_gpu_memory.h, docs/MEMORY_MODULE_PLAN.ru.md). The game's memory is bound in
// place through its chunk buffers; blocks of known data (what loaders wrote) get VRAM copies in
// mirrors: one buffer per run of such blocks, runs that meet merged into a new mirror. No sparse
// binding: a move is a copy plus the module's bookkeeping, and bindings pick the source.
//
// Step 3a: only loaded data is copied to VRAM, and a range the GPU writes goes back in place
// first, so the game's memory always holds the current data and readers in place (the huge
// bindings over all memory) stay right.

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <sys/mman.h>
#include <ucontext.h>
#include <map>
#include <tuple>
#include <string>
#include <cstdio>
#include <fmt/format.h>
#include "bbport_guest_memory.h"
#include "bbport_sections.h"
#include "bbport_toggles.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "common/signal_context.h"
#include "core/signals.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"
// After the Vulkan headers of the video core (their configuration).
#include "bblayer_gpu_memory.h"
#include "bblayer_write_traps.h"

extern "C" int runtime_memory_vma_info(uintptr_t address, int* prot, int* type, uintptr_t* end);

namespace VideoCore {

namespace {
BbLayer::GpuMemory& Layer() {
    return BbLayer::GpuMemory::Get();
}

bool LayerTrapsOn(); // below, with the traps

/// Guest code (the game's image with its linked modules): the writer behind a trap hit. A hit
/// from our own code (the runtime touching pages before a file read into them) must not set the
/// trap again until that write is done.
bool IsGuestCode(u64 rip) {
    constexpr u64 Image = 0x800000000ull, ImageSize = 128ull << 20;
    return rip >= Image && rip < Image + ImageSize;
}

// Volatile blocks (BB_LAYER_VOLATILE=1; off by default): a range with a few unannounced blocks
// goes into one mirror anyway, those blocks refreshed for every binding that reads them. Off, the
// range stays mixed (in place). Their refreshes were ~300 MB/s of copies and, in render passes,
// up to ~100 pass breaks a frame; on a recorded route off was 4.29-4.31 ms of GPU time a frame,
// on 4.31-4.53 even with the in-pass reads in place, 5.4-7.3 before. They were ~0.1 % of the
// bytes bound.
bool LayerVolatileAllowed() {
    static const bool on = [] {
        const char* env = std::getenv("BB_LAYER_VOLATILE");
        return env && env[0] == '1';
    }();
    return on;
}
} // namespace

std::optional<std::pair<const Buffer*, u64>> BufferCache::LayerBind(VAddr address, u64 size,
                                                                    bool is_written,
                                                                    bool is_texel_buffer,
                                                                    bool may_promote) {
    BB_SECTION(LayerBind);
    // Residency changes queued since the last packet: loads, demotions, unmaps, idle blocks.
    if (maintained_epoch != packet_epoch) {
        Maintain();
    }
    LayerProcessTraps(); // blocks the CPU wrote go back in place before anything binds them
    const u64 first = address >> block_shift;
    const u64 end = ((address + size - 1) >> block_shift) + 1;
    NoteUse(address, size); // idle VRAM blocks go back in place (LayerProcessIdle)
    using Kind = BbLayer::Resolution::Kind;
    // The module's answer for this range while nothing moved (layer_generation: promotions,
    // demotions, unmaps); ranges it does not know yet are asked again.
    struct ResolveMemo {
        VAddr address = 0;
        u64 size = 0;
        u64 generation = 0;
        BbLayer::Resolution resolution;
    };
    thread_local std::array<ResolveMemo, 512> resolve_memo{};
    auto& memo = resolve_memo[((address >> 4) ^ (address >> 13) ^ size) & (resolve_memo.size() - 1)];
    BbLayer::Resolution resolution;
    if (memo.address == address && memo.size == size && memo.generation == layer_generation + 1) {
        resolution = memo.resolution;
    } else {
        resolution = Layer().Resolve(address, size);
        if (resolution.kind != Kind::None) {
            memo = {address, size, layer_generation + 1, resolution};
        }
    }
    // BB_RESIDENCY_MIX=1: bindings per kind (none, in place, mirror, mixed) and written, per 2 s.
    static const bool mix_stats = [] {
        const char* env = std::getenv("BB_RESIDENCY_MIX");
        return env && (env[0] == '1' || env[0] == '2');
    }();
    if (mix_stats) {
        static std::array<std::atomic<u64>, 8> count{}, bytes{};
        static std::atomic<u32> printed{0};
        const size_t k = size_t(resolution.kind) * 2 + (is_written ? 1 : 0);
        // In place: whether all its blocks may go to VRAM (else: why not, by block).
        static std::array<std::atomic<u64>, 4> in_place_why{}; // eligible, asset gap, dynamic, chunk
        // Unknown bytes, by why not watched (BB_LAYER_WATCH): watchable, the GPU's dynamic,
        // volatile, failed before, mapping smaller than a block, written binding.
        static std::array<std::atomic<u64>, 6> unwatched_why{};
        if (resolution.kind == Kind::InPlace) {
            int why = 0;
            for (u64 block = first; block < end && why == 0; ++block) {
                const VAddr a = block << block_shift;
                bool known = true;
                asset_bytes.ForEachGap(a, a + block_size, [&](u64 s, u64 e) {
                    known = known && gpu_written_bytes.Contains(s, e);
                });
                why = !known ? 1 : dynamic_blocks.Contains(block) ? 2
                      : !Layer().ResolveInPlace(a, block_size) ? 3 : 0;
                if (why == 1) {
                    int prot = 0, type = -1;
                    uintptr_t vma_end = 0;
                    const int w = is_written ? 5
                                  : dynamic_blocks.Contains(block) &&
                                            !layer_cpu_hot.Contains(block, block + 1) ? 1
                                  : layer_volatile.Contains(block, block + 1) ? 2
                                  : layer_watch_failed.Contains(block, block + 1) ? 3
                                  : (runtime_memory_vma_info(a, &prot, &type, &vma_end) &&
                                     (prot & 0x2) && vma_end < a + block_size) ? 4 : 0;
                    unwatched_why[w].fetch_add(size, std::memory_order_relaxed);
                }
            }
            in_place_why[why].fetch_add(size, std::memory_order_relaxed);
        }
        static std::atomic<u32> why_printed{0};
        if (BbStats::coarse_second.load() - why_printed.load() >= 2) {
            why_printed.store(BbStats::coarse_second.load());
            std::printf("Layer in place (MiB/2 s): eligible %llu, unknown bytes %llu, dynamic %llu, "
                        "not a whole chunk block %llu; unknown bytes not watched: watchable %llu, "
                        "GPU-dynamic %llu, volatile %llu, failed %llu, mapping %llu, written %llu\n",
                        (unsigned long long)(in_place_why[0].exchange(0) >> 20),
                        (unsigned long long)(in_place_why[1].exchange(0) >> 20),
                        (unsigned long long)(in_place_why[2].exchange(0) >> 20),
                        (unsigned long long)(in_place_why[3].exchange(0) >> 20),
                        (unsigned long long)(unwatched_why[0].exchange(0) >> 20),
                        (unsigned long long)(unwatched_why[1].exchange(0) >> 20),
                        (unsigned long long)(unwatched_why[2].exchange(0) >> 20),
                        (unsigned long long)(unwatched_why[3].exchange(0) >> 20),
                        (unsigned long long)(unwatched_why[4].exchange(0) >> 20),
                        (unsigned long long)(unwatched_why[5].exchange(0) >> 20));
        }
        // BB_RESIDENCY_MIX=2: also the bindings in place or mixed with the most bytes, and per
        // range its blocks in VRAM, eligible, dynamic and with unannounced bytes.
        static const bool mix_top = std::getenv("BB_RESIDENCY_MIX")[0] == '2';
        if (mix_top && (resolution.kind == Kind::InPlace || resolution.kind == Kind::Mixed)) {
            static std::map<std::tuple<VAddr, u64, bool, int>, u64> top;
            static u32 top_printed = 0;
            top[{address, size, is_written, int(resolution.kind)}] += size;
            const u32 second = BbStats::coarse_second.load(std::memory_order_relaxed);
            if (second - top_printed >= 4) {
                top_printed = second;
                std::vector<std::pair<u64, std::tuple<VAddr, u64, bool, int>>> sorted;
                for (const auto& [key, total] : top) {
                    sorted.emplace_back(total, key);
                }
                std::ranges::sort(sorted, std::greater{});
                for (size_t i = 0; i < std::min<size_t>(sorted.size(), 12); ++i) {
                    const auto& [total, key] = sorted[i];
                    const auto& [a, s, w, kind] = key;
                    u64 vram = 0, eligible = 0, dynamic = 0, unknown = 0;
                    for (u64 b = a >> block_shift; b <= (a + s - 1) >> block_shift; ++b) {
                        if (Layer().AnyValid(b, b + 1)) {
                            ++vram;
                        } else if (dynamic_blocks.Contains(b)) {
                            ++dynamic;
                        } else if (LayerEligible(b)) {
                            ++eligible;
                        } else {
                            ++unknown;
                        }
                    }
                    std::printf("Layer top: %#llx+%#llx %s %s, %llu MiB in 4 s; blocks: VRAM %llu, "
                                "eligible %llu, dynamic %llu, unannounced %llu\n",
                                (unsigned long long)a, (unsigned long long)s,
                                kind == int(Kind::Mixed) ? "mixed" : "in place",
                                w ? "written" : "read", (unsigned long long)(total >> 20),
                                (unsigned long long)vram, (unsigned long long)eligible,
                                (unsigned long long)dynamic, (unsigned long long)unknown);
                }
                top.clear();
            }
        }
        count[k].fetch_add(1, std::memory_order_relaxed);
        bytes[k].fetch_add(size, std::memory_order_relaxed);
        const u32 now = BbStats::coarse_second.load(std::memory_order_relaxed);
        if (now - printed.load(std::memory_order_relaxed) >= 2) {
            printed.store(now, std::memory_order_relaxed);
            std::string text;
            constexpr std::array<const char*, 4> names = {"none", "in place", "mirror", "mixed"};
            for (size_t i = 0; i < count.size(); ++i) {
                text += fmt::format(" {}{} {}/{}MiB", names[i / 2], (i & 1) ? " w" : "",
                                    count[i].exchange(0), bytes[i].exchange(0) >> 20);
            }
            std::printf("Layer bindings (2 s):%s\n", text.c_str());
        }
    }
    // As the arena gave VRAM to blocks of known data when first bound: a range all of known data
    // (or in VRAM already) is made one mirror now (the missing blocks copied from the game's
    // memory first, in stream order), then bound there. A few blocks with unannounced bytes
    // among many known ones (where the arena mixed VRAM and the game's memory) go into the
    // mirror as volatile blocks, refreshed for every binding that reads them. Not for a range the
    // GPU writes: copying its bytes back after the draw would also put back stale copies of what
    // the CPU (a frame ahead) wrote there meanwhile (exploding vertices).
    // Experiment bit 8 of BB_TOGGLE_FILE (diagnostics, while the game runs): reads bound in place
    // even where a VRAM copy is (a stale copy then shows by going away; nothing is moved).
    if (BbToggle::Experiment(8) && !is_written && resolution.kind == Kind::Mirror) {
        if (const auto in_place = LayerInPlace(address, size, true)) {
            return in_place;
        }
    }
    const bool vram_allowed =
        !(is_written && (force_writes_in_place || !LayerGpuWriteToVram(address, size))) &&
                              !BbToggle::Experiment(8);
    // Not for image uploads (may_promote false): the image is the GPU's copy of that data, read
    // once per upload, and the arena did not move it either. (Loaded data mostly gets its mirror
    // when loaded, ProcessPendingAssets: this alone did not shrink the mirrors.)
    if (resolution.kind != Kind::Mirror && vram_allowed && may_promote && GarlicInVramForLayer() &&
        !VramPromotionsPaused()) {
        constexpr u64 VolatileMinBlocks = 4, VolatileMaxBlocks = 2;
        // A range found not movable is looked at again next frame or when something moved
        // (what makes blocks known data changes between frames: loads, GPU writes).
        struct TriedMemo {
            u64 first = 0, end = 0, generation = 0;
            u32 frame = 0;
            bool written = false;
        };
        thread_local std::array<TriedMemo, 256> tried{};
        auto& t = tried[(first ^ (end << 7)) & (tried.size() - 1)];
        const u32 frame = static_cast<u32>(BbStats::frame_number.load(std::memory_order_relaxed));
        const bool skip = t.first == first && t.end == end && t.generation == layer_generation &&
                          t.frame == frame && t.written == is_written;
        // Blocks with unannounced bytes are watched copies for a reading binding (BB_LAYER_WATCH).
        std::vector<u64> blocks, unknown, watch;
        for (u64 block = first; !skip && block < end && unknown.size() <= VolatileMaxBlocks;
             ++block) {
            if (!Layer().AnyValid(block, block + 1)) {
                if (LayerEligible(block)) {
                    blocks.push_back(block);
                } else if (!is_written && LayerWatchable(block)) {
                    blocks.push_back(block);
                    watch.push_back(block);
                } else {
                    unknown.push_back(block);
                }
            }
        }
        if (!skip) {
            t = {first, end, layer_generation, frame, is_written};
        }
        if (!skip && (unknown.empty() || (!is_written && end - first >= VolatileMinBlocks &&
                                unknown.size() <= VolatileMaxBlocks && LayerVolatileAllowed() &&
                                !BbToggle::Experiment(16)))) {
            for (const u64 block : watch) {
                layer_watched.Add({block, block + 1});
            }
            LayerPromote(blocks);
            for (const u64 block : watch) {
                if (Layer().AnyValid(block, block + 1)) {
                    layer_cpu_blocks.Add({block, block + 1}); // the GPU writes it in place
                } else {
                    layer_watched.Subtract(block, block + 1); // not moved (VRAM short)
                }
            }
            LayerMakeVolatile(unknown);
            resolution = Layer().Resolve(address, size);
        }
    }
    if (is_written && resolution.kind == Kind::Mirror && layer_volatile.Overlaps(first, end)) {
        // Volatile blocks go back in place (nothing to copy back: the GPU never writes them in
        // a mirror); the range is mixed now.
        std::vector<std::pair<u64, u64>> runs;
        layer_volatile.ForEachInRange(first, end, [&](const Interval& iv) {
            runs.emplace_back(std::max(first, iv.start), std::min(end, iv.end));
        });
        for (const auto& [a, b] : runs) {
            LayerDemote(a, b, false, 1);
        }
        resolution = Layer().Resolve(address, size);
    }
    // A read over volatile blocks while a render pass is open: in place. Their refresh is a copy,
    // and a copy ends the pass: the same pass was stored and loaded again up to ~100 times a frame
    // (light volumes, one draw each), the GPU 1.3-1.5x slower for the whole launch in the launches
    // that made such blocks. Outside a pass (dispatches, the first draw) the mirror is refreshed.
    // BB_LAYER_VOLATILE_IN_PASS=0: refreshed in passes too (as before).
    static const bool volatile_in_place = [] {
        const char* env = std::getenv("BB_LAYER_VOLATILE_IN_PASS");
        return !(env && env[0] == '0');
    }();
    if (resolution.kind == Kind::Mirror && vram_allowed && !is_written && volatile_in_place &&
        scheduler.IsRendering() && layer_volatile.Overlaps(first, end)) {
        if (const auto in_place = LayerInPlace(address, size, true)) {
            BbStats::bound_in_place_bytes.fetch_add(size, std::memory_order_relaxed);
            layer_volatile_in_pass_bytes += size;
            if (is_texel_buffer) {
                SynchronizeMemoryFromImage(in_place->first, in_place->second, address,
                                           static_cast<u32>(size));
            }
            TraceBinding(address, size, false, 2);
            return in_place;
        }
    }
    if (resolution.kind == Kind::Mirror && vram_allowed) {
        auto* mirror = static_cast<Buffer*>(resolution.span.source->owner);
        BbStats::bound_vram_bytes.fetch_add(size, std::memory_order_relaxed);
        NoteDirectUse(address, size); // stale copies yield first (LayerYieldToImages)
        if (layer_volatile.Overlaps(first, end)) {
            // Experiment bit 16 (diagnostics, while the game runs): no refreshes, the range read
            // in place instead.
            if (BbToggle::Experiment(16) && !is_written) {
                if (const auto in_place = LayerInPlace(address, size, true)) {
                    return in_place;
                }
            }
            LayerRefreshVolatile(address, size);
        }
        // Uploads what the CPU changed there since (announced writes), images aliasing it.
        SynchronizeMemory(mirror, address, static_cast<u32>(size), is_written, is_texel_buffer);
        if (is_written) {
            // The GPU's data is in VRAM only: copied back before the blocks go in place.
            gpu_modified_ranges.Add(address, size);
            NoteWriteTick(address, size);
            LayerArmReadTraps(address, size); // and before the CPU reads it
        }
        TraceBinding(address, size, is_written, is_written ? 1 : 4);
        return std::pair<const Buffer*, u64>{mirror, mirror->Offset(address)};
    }
    if (resolution.kind == Kind::Mirror || resolution.kind == Kind::Mixed) {
        // One buffer range must hold it: in place. Ranges over nearly all memory in shaders are
        // paged instead (LayerPagedRecord). Written: its blocks in VRAM go back in place (the
        // GPU's data there copied back first). Read: only the GPU's data is copied back; the
        // blocks stay in VRAM for the bindings that lie whole in their mirror.
        if (is_written) {
            LayerDemote(first, end, true, resolution.kind == Kind::Mirror ? 0 : 1);
            // The GPU writes this range in place: its blocks stay in place (moving them back
            // would only bring them here again; the arena mixed them). All in one mirror: from
            // the second time (a reading binding brought it back every frame meanwhile: ~200 MiB/s
            // each way and traps set and cleared, the recording thread saturated at 42 FPS).
            if (resolution.kind == Kind::Mixed || layer_write_bounces.Overlaps(first, end)) {
                dynamic_blocks.Add({first, end});
            } else {
                layer_write_bounces.Add({first, end});
            }
        } else {
            LayerCopyBack(first, end);
        }
    }
    // In place (huge bindings over several chunks are cut at the end of the first).
    const auto in_place = LayerInPlace(address, size, true);
    if (!in_place) {
        return std::nullopt;
    }
    // Its blocks of known data go to VRAM at the next submission (as the arena gave VRAM to such
    // blocks when first bound): once per range while the residency is unchanged.
    if (may_promote) {
        LayerQueuePromotion(first, end);
    }
    const auto [buffer, offset] = *in_place;
    if (is_written) {
        NoteWriteTick(address, size);
        BbStats::bound_in_place_written_bytes.fetch_add(size, std::memory_order_relaxed);
        // What the GPU writes often moves to VRAM once its blocks are all known data
        // (NoteGpuWrite: written in 3 of 60 frames). Some writers stay in place (the copy
        // shader's destinations: force_writes_in_place).
        if (!force_writes_in_place) {
            NoteGpuWrite(address, size);
        }
    }
    BbStats::bound_in_place_bytes.fetch_add(size, std::memory_order_relaxed);
    if (is_texel_buffer && !is_written) {
        SynchronizeMemoryFromImage(buffer, offset, address, static_cast<u32>(size));
    }
    TraceBinding(address, size, is_written, is_written ? 0 : 2);
    return in_place;
}

bool BufferCache::LayerEligible(u64 block) {
    // Every byte last written by a loader or by the GPU (writes we hear of), in a guest chunk.
    const VAddr address = block << block_shift;
    if (dynamic_blocks.Contains(block)) {
        return false;
    }
    bool known = true;
    asset_bytes.ForEachGap(address, address + block_size, [&](u64 start, u64 end) {
        known = known && gpu_written_bytes.Contains(start, end) &&
                LayerGpuWriteToVram(start, end - start);
    });
    return known && Layer().ResolveInPlace(address, block_size).has_value();
}

bool BufferCache::LayerWatchEnabled() {
    // BB_LAYER_WATCH=1/0 (default 1). The reads over the bus cost far more on NVIDIA's imported
    // host memory than on AMD's dma-buf (GTX 1660 Ti: 30-35 FPS with the module, 60-70 with the
    // model of 0.3; RX 7800 XT: on par). It also replaces most volatile blocks, refreshed for
    // every binding (RX 7800 XT: 1.3 GB/s of copies -> 36 MB/s). Needs the write traps
    // (BB_LAYER_WRITE_TRAPS).
    static const bool on = [] {
        const char* env = std::getenv("BB_LAYER_WATCH");
        const bool enabled = !(env && env[0] == '0');
        if (enabled && LayerTrapsOn()) {
            std::printf("Guest memory: blocks the CPU writes unannounced get VRAM copies too, "
                        "uploaded again after its next write (BB_LAYER_WATCH=1)\n");
        }
        return enabled;
    }();
    return on && LayerTrapsOn();
}

bool BufferCache::LayerWatchHot() {
    // BB_LAYER_WATCH_HOT=1 (experiment): blocks the CPU writes often (NoteCpuWrite: in 3 of 60
    // frames) are watched too and uploaded again after every write, as the model of 0.3 uploads
    // them. Off: they go back in place. Uploading them before bindings ended render passes
    // (RX 7800 XT: the GPU's frame 4.8 -> 7.2 ms).
    static const bool on = [] {
        const char* env = std::getenv("BB_LAYER_WATCH_HOT");
        return env && env[0] == '1';
    }();
    return on;
}

bool BufferCache::LayerWatchable(u64 block) {
    // Dynamic for the GPU's writes (in place in a mixed range): never; for the CPU's (often
    // written) only with BB_LAYER_WATCH_HOT=1.
    const bool dynamic = dynamic_blocks.Contains(block) &&
                         !(LayerWatchHot() && layer_cpu_hot.Contains(block, block + 1));
    if (!LayerWatchEnabled() || dynamic || layer_volatile.Contains(block, block + 1) ||
        layer_watch_failed.Contains(block, block + 1)) {
        return false;
    }
    const VAddr address = block << block_shift;
    if (!Layer().ResolveInPlace(address, block_size)) {
        return false; // no source to upload from
    }
    // A trap must be able to catch every CPU write: the CPU cannot write it, or one mapping
    // holds the whole block (LayerArmTraps).
    int prot = 0, type = -1;
    uintptr_t vma_end = 0;
    if (!runtime_memory_vma_info(address, &prot, &type, &vma_end)) {
        return false;
    }
    return !(prot & 0x2) || vma_end >= address + block_size;
}

void BufferCache::LayerRefreshWatched(u64 block) {
    // The CPU wrote it (a trap hit): uploaded again before the next binding over it (the bound
    // path's SynchronizeMemory; paged bindings: LayerFlushUploads), the trap set again for its
    // next write. The GPU never writes it in VRAM: nothing of the GPU's to keep.
    const VAddr from = block << block_shift;
    memory_tracker->UnmarkRegionAsGpuModified(from, block_size);
    gpu_modified_ranges.Subtract(from, block_size);
    memory_tracker->MarkRegionAsCpuModified(from, block_size);
    {
        std::scoped_lock lk{layer_uploads_mutex};
        layer_pending_uploads.emplace_back(from, block_size);
        layer_uploads_pending.store(true, std::memory_order_release);
    }
    LayerArmTraps(block, block + 1);
    ++layer_watch_refreshes;
}

void BufferCache::LayerPromote(const std::vector<u64>& blocks) {
    BbStats::Timer timer{BbStats::t_layer[1]};
    u64 moved = 0;
    for (size_t i = 0; i < blocks.size();) {
        // A run of consecutive candidate blocks.
        u64 a = blocks[i], b = a + 1;
        for (++i; i < blocks.size() && blocks[i] == b; ++i) {
            ++b;
        }
        // Only blocks still eligible and not in VRAM yet; then their run.
        u64 run_first = ~0ULL, run_end = 0;
        for (u64 block = a; block < b; ++block) {
            if (Layer().AnyValid(block, block + 1) ||
                !(LayerEligible(block) || layer_watched.Contains(block, block + 1))) {
                if (run_first != ~0ULL) {
                    moved += LayerPromoteRun(run_first, run_end);
                    run_first = ~0ULL;
                }
                continue;
            }
            if (run_first == ~0ULL) {
                run_first = block;
            }
            run_end = block + 1;
        }
        if (run_first != ~0ULL) {
            moved += LayerPromoteRun(run_first, run_end);
        }
    }
    if (moved != 0) {
        static u64 promoted = 0;
        if (((promoted + moved) >> 26) != (promoted >> 26)) {
            std::printf("Layer memory: %llu MiB of loaded blocks copied to VRAM, %llu MiB valid "
                        "there in %zu mirrors\n",
                        (unsigned long long)((promoted + moved) >> 20),
                        (unsigned long long)(Layer().ValidBytes() >> 20), layer_mirrors.size());
        }
        promoted += moved;
    }
}

u64 BufferCache::LayerPromoteRun(u64 first, u64 end) {
    // The mirror range: the run and every mirror touching it (or touching what that adds).
    u64 lo = first, hi = end;
    std::vector<BbLayer::MemorySource> touching;
    for (bool grew = true; grew;) {
        grew = false;
        touching = Layer().MirrorsIn((lo - 1) << block_shift, (hi + 1) << block_shift);
        for (const auto& mirror : touching) {
            const u64 m_lo = mirror.base >> block_shift;
            const u64 m_hi = (mirror.base + mirror.size) >> block_shift;
            if (m_lo < lo || m_hi > hi) {
                lo = std::min(lo, m_lo);
                hi = std::max(hi, m_hi);
                grew = true;
            }
        }
    }
    // Page table entries that change: the run, and the blocks of mirrors replaced by a new one.
    std::vector<std::pair<u64, u64>> moved;
    Buffer* target = nullptr;
    if (touching.size() == 1 && touching[0].base == (lo << block_shift) &&
        touching[0].size == ((hi - lo) << block_shift)) {
        target = static_cast<Buffer*>(touching[0].owner);
    } else {
        // A new mirror over all of it; the old ones' valid blocks are copied into it and they
        // go once the GPU is past the commands recorded so far. One that grows (it takes in a
        // mirror it meets) gets room above it for as much again, up to LayerGrowthBlocks: a
        // loader writing a file in pieces made a new mirror, and a copy of the old one, for each
        // piece (15-27 GiB allocated for 1.3 GiB of data on loading a level, 7.4 GiB of VRAM
        // at once; streaming areas did the same while playing).
        // Growth at the top only: a run that fills a gap between mirrors is not a file coming in.
        u64 top = 0;
        for (const auto& mirror : touching) {
            top = std::max<u64>(top, (mirror.base + mirror.size) >> block_shift);
        }
        if (!touching.empty() && end >= top) {
            hi = LayerGrowthEnd(lo, hi);
        }
        const VAddr start = lo << block_shift;
        const u64 bytes = (hi - lo) << block_shift;
        auto mirror = std::make_unique<Buffer>(instance, start, bytes, MemoryType::DeviceLocal,
                                               fmt::format("bbport mirror {:#x}+{:#x}", start, bytes));
        target = mirror.get();
        for (const auto& old : touching) {
            auto* old_buffer = static_cast<Buffer*>(old.owner);
            moved.emplace_back(old.base >> block_shift, (old.base + old.size) >> block_shift);
            boost::container::small_vector<vk::BufferCopy, 8> copies;
            Layer().ForEachValid(old.base >> block_shift, (old.base + old.size) >> block_shift,
                                 [&](u64 v_first, u64 v_end) {
                                     const VAddr from = v_first << block_shift;
                                     copies.push_back({old_buffer->Offset(from),
                                                       target->Offset(from),
                                                       (v_end - v_first) << block_shift});
                                 });
            if (!copies.empty()) {
                runtime.CopyBuffer(old_buffer, target, copies);
            }
            Layer().RemoveMirror(old.base);
            auto node = layer_mirrors.extract(old.base);
            if (!node.empty()) {
                layer_retired.push_back({std::move(node.mapped()), scheduler.CurrentTick()});
            }
        }
        Layer().AddMirror(start, bytes, target->Handle(), target->BufferDeviceAddress(), target);
        layer_mirrors.emplace(start, std::move(mirror));
        BbStats::residency_alloc_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
    // The run's data: copied from the game's memory now (SynchronizeMemory, which also arms the
    // tracking of later CPU writes there).
    Layer().MarkValid(first, end);
    NoteUse(first << block_shift, (end - first) << block_shift);
    NoteDirectUse(first << block_shift, (end - first) << block_shift); // not stale at once
    const VAddr from = first << block_shift;
    const u64 bytes = (end - first) << block_shift;
    memory_tracker->UnmarkRegionAsGpuModified(from, bytes);
    gpu_modified_ranges.Subtract(from, bytes);
    memory_tracker->MarkRegionAsCpuModified(from, bytes);
    SynchronizeMemory(target, from, static_cast<u32>(bytes), false, false);
    LayerArmTraps(first, end); // writes nobody announces send it back in place
    // Paged bindings find the mirror (a merge moved its other blocks too). Not all of it: a
    // mirror of a GiB rewrote 16k entries for every run moved into it (160k in a frame).
    LayerRepublish(first, end);
    for (const auto& [a, b] : moved) {
        LayerRepublish(a, b);
    }
    ++layer_generation;
    return bytes;
}

u64 BufferCache::LayerGrowthEnd(u64 lo, u64 hi) {
    // BB_LAYER_MIRROR_GROWTH_MB (64; 0: none): the most room a growing mirror gets above it.
    static const u64 growth_bytes = [] {
        const char* env = std::getenv("BB_LAYER_MIRROR_GROWTH_MB");
        return (env && *env ? std::strtoull(env, nullptr, 10) : 64ull) << 20;
    }();
    const u64 extra = std::min(hi - lo, growth_bytes >> block_shift);
    if (extra == 0) {
        return hi;
    }
    u64 end = hi + extra;
    // Not over the next mirror, and only blocks that can be valid here: the game's memory whole
    // in a chunk, contiguous with the blocks below.
    for (const auto& mirror : Layer().MirrorsIn(hi << block_shift, end << block_shift)) {
        end = std::min(end, mirror.base >> block_shift);
    }
    if (end <= hi) {
        return hi;
    }
    const auto prefix = Layer().ResolvePrefix(hi << block_shift, (end - hi) << block_shift);
    if (!prefix) {
        return hi;
    }
    return std::min(end, hi + (prefix->size >> block_shift));
}

static std::array<std::atomic<u64>, 7> layer_demoted_bytes{};

void BufferCache::LayerDemote(u64 first, u64 end, bool copy_back, int reason) {
    BbStats::Timer timer{BbStats::t_layer[2]};
    LayerForgetGpuData();
    std::vector<std::pair<u64, u64>> runs;
    Layer().ForEachValid(first, end, [&](u64 a, u64 b) { runs.emplace_back(a, b); });
    if (runs.empty()) {
        return;
    }
    for (const auto& [a, b] : runs) {
        const VAddr from = a << block_shift;
        const u64 bytes = (b - a) << block_shift;
        if (copy_back) {
            // What the GPU wrote in VRAM goes back into the game's memory first.
            const auto mirror = Layer().MirrorAt(from);
            if (mirror) {
                auto* buffer = static_cast<Buffer*>(mirror->owner);
                gpu_modified_ranges.ForEachInRange(from, bytes, [&](VAddr w_from, VAddr w_to) {
                    if (const auto target = LayerInPlace(w_from, w_to - w_from)) {
                        const vk::BufferCopy copy{buffer->Offset(w_from), target->second,
                                                  w_to - w_from};
                        runtime.CopyBuffer(buffer, target->first, std::span{&copy, 1});
                    }
                });
            }
        }
        layer_volatile.Subtract(a, b);
        // A watched block sent back for a GPU write (binding or command) is not watched again (it
        // came back for every binding, ~40 MiB/s while playing; memory handed out anew does not
        // clear it either: the game's allocator recycles some every frame); one sent back for an
        // announced write or a request, after the third time (loading a level sends many back
        // once). Idle or unmapped ones may be again.
        if (reason <= 3) {
            layer_watched.ForEachInRange(a, b, [&](const Interval& iv) {
                for (u64 block = std::max(a, iv.start); block < std::min(b, iv.end); ++block) {
                    if (reason <= 2 || ++layer_watch_strikes[block] >= 3) {
                        layer_watch_failed.Add({block, block + 1});
                    }
                }
            });
        }
        layer_watched.Subtract(a, b);
        LayerDisarmTraps(a, b, false);
        Layer().MarkInvalid(a, b);
        // In place from now on: never uploaded or watched again (writes go where the GPU reads).
        memory_tracker->MarkRegionAsCpuModified(from, bytes);
        layer_demoted_bytes[reason].fetch_add(bytes, std::memory_order_relaxed);
        LayerRepublish(a, b); // paged bindings find them in place
        memory_tracker->UnmarkRegionAsGpuModified(from, bytes);
        gpu_modified_ranges.Subtract(from, bytes);
    }
    // Mirrors left without valid blocks go once the GPU is past the commands recorded so far.
    for (const auto& mirror : Layer().MirrorsIn(first << block_shift, end << block_shift)) {
        if (Layer().AnyValid(mirror.base >> block_shift, (mirror.base + mirror.size) >> block_shift)) {
            continue;
        }
        Layer().RemoveMirror(mirror.base);
        auto node = layer_mirrors.extract(mirror.base);
        if (!node.empty()) {
            BbStats::residency_alloc_bytes.fetch_sub(mirror.size, std::memory_order_relaxed);
            layer_retired.push_back({std::move(node.mapped()), scheduler.CurrentTick()});
        }
    }
    ++layer_generation;
}

void BufferCache::LayerMaintain() {
    LayerProcessTraps();
    // Retired mirrors the GPU is done with.
    std::erase_if(layer_retired, [&](const auto& retired) {
        return scheduler.GetWorkSemaphore()->IsFree(retired.second);
    });
    static u32 printed = 0;
    const u32 now = BbStats::coarse_second.load(std::memory_order_relaxed);
    if (now - printed >= 10) {
        printed = now;
        u64 mirror_bytes = 0, asset_total = 0;
        for (const auto& range : asset_bytes) {
            asset_total += range.end - range.start;
        }
        for (const auto& [start, mirror] : layer_mirrors) {
            mirror_bytes += mirror->SizeBytes();
        }
        std::printf("Layer memory: %llu MiB valid in VRAM, %zu mirrors of %llu MiB (%zu retired); "
                    "loaded data %llu MiB; vkQueueBindSparse: %llu calls, %llu ranges; write traps hit %llu\n",
                    (unsigned long long)(Layer().ValidBytes() >> 20), layer_mirrors.size(),
                    (unsigned long long)(mirror_bytes >> 20), layer_retired.size(),
                    (unsigned long long)(asset_total >> 20),
                    (unsigned long long)arena_bind_calls.load(),
                    (unsigned long long)arena_bind_ranges.load(), (unsigned long long)LayerTrapFaults());
        u64 volatile_blocks = 0;
        for (const auto& range : layer_volatile) {
            volatile_blocks += range.end - range.start;
        }
        u64 watched_blocks = 0;
        for (const auto& range : layer_watched) {
            watched_blocks += range.end - range.start;
        }
        std::printf("Layer memory: %llu volatile blocks, %llu MiB refreshed in 10 s (%llu MiB read in "
                    "place in render passes); %llu watched blocks, %llu uploaded again after CPU writes; GPU writes in VRAM barred in "
                    "%llu MiB the CPU wrote\n",
                    (unsigned long long)volatile_blocks,
                    (unsigned long long)(layer_volatile_refresh_bytes >> 20),
                    (unsigned long long)(layer_volatile_in_pass_bytes >> 20),
                    (unsigned long long)watched_blocks, (unsigned long long)layer_watch_refreshes,
                    (unsigned long long)([&] {
                        u64 blocks = 0;
                        for (const auto& range : layer_cpu_blocks) {
                            blocks += range.end - range.start;
                        }
                        return blocks;
                    }() << block_shift >> 20));
        layer_volatile_refresh_bytes = 0;
        layer_volatile_in_pass_bytes = 0;
        layer_watch_refreshes = 0;
        std::printf("Layer memory: write traps in 10 s: announced %llu; the game's code on watched "
                    "%llu, on other copies %llu; other code on watched %llu, on other copies %llu\n",
                    (unsigned long long)layer_trap_kinds[0], (unsigned long long)layer_trap_kinds[1],
                    (unsigned long long)layer_trap_kinds[2], (unsigned long long)layer_trap_kinds[3],
                    (unsigned long long)layer_trap_kinds[4]);
        layer_trap_kinds = {};
        if (LayerReadTrapsOn()) {
            u64 trapped = 0;
            {
                std::scoped_lock lk{layer_read_trap_mutex};
                layer_read_trapped.ForEach([&](VAddr a, VAddr b) { trapped += b - a; });
            }
            std::printf("Layer memory: read traps (GPU data in VRAM only): %llu KiB trapped; %llu CPU "
                        "accesses in 10 s, %llu KiB copied back for them\n",
                        (unsigned long long)(trapped >> 10),
                        (unsigned long long)layer_read_trap_hits.exchange(0),
                        (unsigned long long)(layer_read_trap_bytes.exchange(0) >> 10));
        }
        std::printf("Layer memory: back in place (MiB): written %llu, mixed %llu, command %llu, "
                    "request %llu, copy back %llu, unmap %llu, idle %llu; GPU data copied back %llu MiB\n",
                    (unsigned long long)(layer_demoted_bytes[0].load() >> 20),
                    (unsigned long long)(layer_demoted_bytes[1].load() >> 20),
                    (unsigned long long)(layer_demoted_bytes[2].load() >> 20),
                    (unsigned long long)(layer_demoted_bytes[3].load() >> 20),
                    (unsigned long long)(layer_demoted_bytes[4].load() >> 20),
                    (unsigned long long)(layer_demoted_bytes[5].load() >> 20),
                    (unsigned long long)(layer_demoted_bytes[6].load() >> 20),
                    (unsigned long long)(layer_copied_back_bytes >> 20));
    }
}

void BufferCache::LayerProcessIdle() {
    const u32 now = BbStats::coarse_second.load(std::memory_order_relaxed);
    if (VramIdleSeconds() == 0) {
        return;
    }
    LayerYieldToImages(now);
    std::vector<std::pair<u64, u64>> idle;
    Layer().ForEachValid(0, ~0ULL, [&](u64 a, u64 b) {
        for (u64 block = a; block < b;) {
            const u64 group = (block << block_shift) >> USE_GROUP_BITS;
            const u64 group_end = std::min(b, ((group + 1) << USE_GROUP_BITS) >> block_shift);
            if (now - group_use[group] >= VramIdleSeconds()) {
                idle.emplace_back(block, group_end);
            }
            block = group_end;
        }
    });
    for (const auto& [a, b] : idle) {
        LayerDemote(a, b, true, 6);
        BbStats::vram_idle_bytes.fetch_add((b - a) << block_shift, std::memory_order_relaxed);
    }
}

void BufferCache::NoteDirectUse(VAddr address, u64 size) {
    const u32 now = BbStats::coarse_second.load(std::memory_order_relaxed);
    const u64 last = std::min<u64>((address + size - 1) >> USE_GROUP_BITS, layer_group_use.size() - 1);
    for (u64 group = address >> USE_GROUP_BITS; group <= last; ++group) {
        if (layer_group_use[group] != now) {
            layer_group_use[group] = now;
        }
    }
}

u64 BufferCache::LayerCompactMirrors(u64 wanted) {
    // Mirror memory without valid blocks (sent back in place, unmapped, handed out anew, the room
    // to grow) stays allocated as long as its mirror lives. Mirrors with at least MinWaste of it,
    // the most first, are made again over their valid blocks only: one new mirror per run of
    // valid blocks (runs closer than Bridge stay in one), their data copied over, the old mirror
    // retired as in a merge. No copy is lost.
    constexpr u64 MinWaste = 8_MB, Bridge = 4_MB;
    std::vector<std::pair<u64, VAddr>> wasteful; // bytes without valid blocks, start
    for (const auto& [start, mirror] : layer_mirrors) {
        const u64 lo = start >> block_shift, hi = (start + mirror->SizeBytes()) >> block_shift;
        u64 valid = 0;
        Layer().ForEachValid(lo, hi, [&](u64 a, u64 b) { valid += b - a; });
        const u64 waste = mirror->SizeBytes() - (valid << block_shift);
        if (waste >= MinWaste) {
            wasteful.emplace_back(waste, start);
        }
    }
    std::ranges::sort(wasteful, std::greater{});
    u64 freed = 0;
    size_t compacted = 0;
    for (const auto& [waste, start] : wasteful) {
        if (freed >= wanted) {
            break;
        }
        auto node = layer_mirrors.extract(start);
        Buffer* old_buffer = node.mapped().get();
        const u64 old_size = old_buffer->SizeBytes();
        const u64 lo = start >> block_shift, hi = (start + old_size) >> block_shift;
        std::vector<std::pair<u64, u64>> segments;
        Layer().ForEachValid(lo, hi, [&](u64 a, u64 b) {
            if (!segments.empty() && ((a - segments.back().second) << block_shift) < Bridge) {
                segments.back().second = b;
            } else {
                segments.emplace_back(a, b);
            }
        });
        Layer().RemoveMirror(start);
        u64 kept = 0;
        for (const auto& [a, b] : segments) {
            const VAddr base = a << block_shift;
            const u64 size = (b - a) << block_shift;
            auto mirror = std::make_unique<Buffer>(instance, base, size, MemoryType::DeviceLocal,
                                                   fmt::format("bbport mirror {:#x}+{:#x}", base, size));
            boost::container::small_vector<vk::BufferCopy, 8> copies;
            Layer().ForEachValid(a, b, [&](u64 v_first, u64 v_end) {
                const VAddr from = v_first << block_shift;
                copies.push_back({old_buffer->Offset(from), mirror->Offset(from),
                                  (v_end - v_first) << block_shift});
            });
            if (!copies.empty()) {
                runtime.CopyBuffer(old_buffer, mirror.get(), copies);
            }
            Layer().AddMirror(base, size, mirror->Handle(), mirror->BufferDeviceAddress(), mirror.get());
            layer_mirrors.emplace(base, std::move(mirror));
            BbStats::residency_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
            kept += size;
        }
        layer_retired.push_back({std::move(node.mapped()), scheduler.CurrentTick()});
        LayerRepublish(lo, hi); // paged bindings find the new buffers
        ++layer_generation;
        freed += old_size - kept;
        ++compacted;
    }
    if (freed != 0) {
        std::printf("Layer memory: VRAM short: %zu mirrors made again over their valid blocks, "
                    "%llu MiB freed\n",
                    compacted, (unsigned long long)(freed >> 20));
    }
    return freed;
}

void BufferCache::LayerYieldToImages(u32 now) {
    BbStats::Timer timer{BbStats::t_layer[4]};
    // VRAM short: the usage where promotions wait (PromoteCriticalMargin short of the texture
    // collector's critical mark). Past that mark the collector evicts images used a moment ago,
    // re-uploaded at once (stutters), and mirrors are what it cannot free. Meanwhile the copies of
    // where the player is now wait for their room, read over the bus (10 FPS less after a lamp
    // travel on a 6 GB card), so room is made for them.
    if (!instance.CanReportMemoryUsage() || layer_mirrors.empty()) {
        return;
    }
    const u64 critical = BbStats::gc_critical_bytes.load(std::memory_order_relaxed);
    const u64 usage = BbStats::gc_used_bytes.load(std::memory_order_relaxed);
    if (critical == 0 || usage + PromoteCriticalMargin < critical) {
        return;
    }
    const u64 wanted = usage + PromoteCriticalMargin + StaleHeadroom - critical;
    static const u32 idle_seconds = [] {
        const char* env = std::getenv("BB_VRAM_PRESSURE_IDLE_SECONDS");
        return env && *env ? u32(std::strtoul(env, nullptr, 10)) : 3u;
    }();
    // 1. Valid blocks in 2 MiB groups no binding took directly for BB_VRAM_PRESSURE_IDLE_SECONDS
    // (3): an area left behind, a lamp travelled from. They go back in place, the longest unused
    // first, whatever the mirrors' floor, as far as the mirrors' memory without valid blocks does
    // not already cover the need. Only direct bindings count (layer_group_use): paged bindings
    // over nearly all memory mark every group as used (group_use), and nothing was ever idle
    // while playing.
    u64 waste = 0;
    for (const auto& [start, mirror] : layer_mirrors) {
        u64 valid = 0;
        Layer().ForEachValid(start >> block_shift, (start + mirror->SizeBytes()) >> block_shift,
                             [&](u64 a, u64 b) { valid += b - a; });
        waste += mirror->SizeBytes() - (valid << block_shift);
    }
    u64 stale_bytes = 0;
    if (idle_seconds != 0 && waste < wanted) {
        std::vector<std::tuple<u32, u64, u64>> stale; // last direct use, first block, end block
        Layer().ForEachValid(0, ~0ULL, [&](u64 a, u64 b) {
            for (u64 block = a; block < b;) {
                const u64 group = (block << block_shift) >> USE_GROUP_BITS;
                const u64 group_end = std::min(b, ((group + 1) << USE_GROUP_BITS) >> block_shift);
                const u32 last = layer_group_use[group];
                if (now - last >= idle_seconds) {
                    stale.emplace_back(last, block, group_end);
                }
                block = group_end;
            }
        });
        std::ranges::sort(stale);
        for (const auto& [last, a, b] : stale) {
            if (waste + stale_bytes >= wanted) {
                break;
            }
            LayerDemote(a, b, true, 6);
            stale_bytes += (b - a) << block_shift;
        }
        if (stale_bytes != 0) {
            BbStats::vram_idle_bytes.fetch_add(stale_bytes, std::memory_order_relaxed);
            static u32 printed = 0;
            if (now - printed >= 5) {
                printed = now;
                std::printf("Layer memory: VRAM %llu MiB, texture collector critical at %llu MiB: "
                            "%llu MiB of VRAM copies not bound for %u s back in place\n",
                            (unsigned long long)(usage >> 20), (unsigned long long)(critical >> 20),
                            (unsigned long long)(stale_bytes >> 20), idle_seconds);
            }
        }
    }
    // 2. The memory without valid blocks is given back (mirrors made again, smaller).
    LayerCompactMirrors(wanted);
}

} // namespace VideoCore

namespace VideoCore {

u64 BufferCache::LayerEntry(u64 block) {
    // The device address of the block's current source: its mirror where valid, else the game's
    // memory (a block mapped whole, in a chunk), else 0 (reads give zero, writes are dropped).
    const VAddr va = block << block_shift;
    if (Layer().AnyValid(block, block + 1) && !layer_volatile.Contains(block, block + 1)) {
        if (const auto mirror = Layer().MirrorAt(va)) {
            return mirror->device_address + (va - mirror->base);
        }
    }
    const auto span = Layer().ResolveInPlace(va, block_size);
    if (!span || span->source->device_address == 0) {
        return 0;
    }
    return span->source->device_address + span->offset;
}

void BufferCache::LayerWriteEntries(u64 first, u64 end) {
    if (first >= end) {
        return;
    }
    // Diagnostics (BB_FRAME_STATS): where the time goes (t_layer 8-11).
    auto mark = std::chrono::steady_clock::now();
    const auto lap = [&](size_t kind) {
        if (!BbStats::enabled) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        BbStats::t_layer[kind].fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count(),
            std::memory_order_relaxed);
        mark = now;
    };
    BbStats::layer_entries_written.fetch_add(end - first, std::memory_order_relaxed);
    const auto staging =
        staging_pool.Request((end - first) * 2 * sizeof(vk::DeviceAddress), MemoryType::HostUncached);
    lap(8);
    // Read/current view, then CPU-visible guest view. A write-through store updates both: the CPU
    // sees the result without discarding all mirrors covered by a huge descriptor.
    auto* entries = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    auto* guest_entries = entries + (end - first);
    // The guest view (as LayerEntry in place: a block mapped whole, in a chunk), one translation
    // per mapping, not per block: each takes the runtime's memory lock, which the game's threads
    // hold while they map memory; a level streaming in kept the GPU thread waiting 30-70 ms per
    // frame on the thousands a large promotion made.
    for (u64 block = first; block < end;) {
        const auto span = Layer().ResolvePrefix(block << block_shift, (end - block) << block_shift);
        const u64 whole = span ? span->size >> block_shift : 0;
        if (whole == 0) {
            guest_entries[block - first] = 0;
            ++block;
            continue;
        }
        const vk::DeviceAddress base =
            span->source->device_address ? span->source->device_address + span->offset : 0;
        for (u64 i = 0; i < whole; ++i) {
            guest_entries[block - first + i] = base ? base + (i << block_shift) : 0;
        }
        block += whole;
    }
    lap(9);
    std::copy_n(guest_entries, end - first, entries);
    // Valid blocks (not volatile) are read from their mirror.
    std::vector<std::pair<u64, u64>> valid;
    Layer().ForEachValid(first, end, [&](u64 a, u64 b) { valid.emplace_back(a, b); });
    for (const auto& [a, b] : valid) {
        std::optional<BbLayer::MemorySource> mirror;
        const bool any_volatile = layer_volatile.Overlaps(a, b);
        for (u64 block = a; block < b; ++block) {
            const VAddr va = block << block_shift;
            if (any_volatile && layer_volatile.Contains(block, block + 1)) {
                continue;
            }
            if (!mirror || va < mirror->base || va >= mirror->base + mirror->size) {
                mirror = Layer().MirrorAt(va);
            }
            if (mirror) {
                entries[block - first] = mirror->device_address + (va - mirror->base);
            }
        }
    }
    lap(10);
    staging.Flush();
    // Keep the original current-view table layout for DMA/ReadConst users too; append the
    // guest-view table in the second half rather than interleaving its entries.
    const u64 bytes = (end - first) * sizeof(vk::DeviceAddress);
    const std::array<vk::BufferCopy, 2> copies{{
        {staging.offset, first * sizeof(vk::DeviceAddress), bytes},
        {staging.offset + bytes, bda_pagetable_buffer->SizeBytes() / 2 +
                                    first * sizeof(vk::DeviceAddress), bytes},
    }};
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
    lap(11);
}

void BufferCache::LayerRepublish(u64 first, u64 end) {
    BbStats::Timer timer{BbStats::t_layer[6]};
    std::vector<std::pair<u64, u64>> runs;
    layer_published.ForEachInRange(first, end, [&](const Interval& iv) {
        runs.emplace_back(std::max(first, iv.start), std::min(end, iv.end));
    });
    for (const auto& [a, b] : runs) {
        LayerWriteEntries(a, b);
    }
}

void BufferCache::LayerNoteCpuWrite(VAddr address, u64 size) {
    if (size == 0) { // guest threads too: no layer_mirrors here
        return;
    }
    {
        // The CPU's bytes are newer than what the GPU wrote there: never copied back over them,
        // and the CPU uses these blocks (layer_cpu_blocks).
        std::scoped_lock lk{layer_uploads_mutex};
        layer_cpu_written.emplace_back(address, size);
        layer_cpu_written_pending.store(true, std::memory_order_release);
    }
    if (Layer().ValidBytes() == 0) {
        return;
    }
    // Announced: the write must not fault (a file read into it would fail): unprotected now, and
    // the block goes back in place.
    LayerDisarmTraps(address >> block_shift, ((address + size - 1) >> block_shift) + 1, true);
    std::scoped_lock lk{layer_uploads_mutex};
    layer_pending_uploads.emplace_back(address, size);
    layer_uploads_pending.store(true, std::memory_order_release);
}

void BufferCache::LayerForgetGpuData() {
    if (!layer_cpu_written_pending.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<std::pair<VAddr, u64>> ranges;
    {
        std::scoped_lock lk{layer_uploads_mutex};
        ranges.swap(layer_cpu_written);
        layer_cpu_written_pending.store(false, std::memory_order_release);
    }
    for (const auto& [address, size] : ranges) {
        gpu_modified_ranges.Subtract(address, size);
        memory_tracker->UnmarkRegionAsGpuModified(address, size);
        layer_cpu_blocks.Add({address >> block_shift, ((address + size - 1) >> block_shift) + 1});
    }
}

void BufferCache::LayerFlushUploads() {
    BbStats::Timer timer{BbStats::t_layer[7]};
    // What the CPU wrote into blocks with a VRAM copy: uploaded now, before a paged binding
    // (it reads mirrors through the page table, without the bound path's own upload).
    if (!layer_uploads_pending.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<std::pair<VAddr, u64>> ranges;
    {
        std::scoped_lock lk{layer_uploads_mutex};
        ranges.swap(layer_pending_uploads);
        layer_uploads_pending.store(false, std::memory_order_release);
    }
    for (const auto& [address, size] : ranges) {
        const u64 first = address >> block_shift;
        const u64 end = ((address + size - 1) >> block_shift) + 1;
        std::vector<std::pair<u64, u64>> runs;
        Layer().ForEachValid(first, end, [&](u64 a, u64 b) { runs.emplace_back(a, b); });
        for (const auto& [a, b] : runs) {
            const auto mirror = Layer().MirrorAt(a << block_shift);
            if (!mirror) {
                continue;
            }
            const VAddr from = std::max<VAddr>(address, a << block_shift);
            const VAddr to = std::min<VAddr>(address + size, b << block_shift);
            SynchronizeMemory(static_cast<Buffer*>(mirror->owner), from, static_cast<u32>(to - from),
                              false, false);
        }
    }
}

std::pair<const Buffer*, u64> BufferCache::LayerPagedRecord(VAddr address, u64 size,
                                                            bool is_written) {
    if (maintained_epoch != packet_epoch) {
        Maintain();
    }
    LayerProcessTraps();
    LayerFlushUploads();
    const u64 first = address >> block_shift;
    const u64 end = ((address + size - 1) >> block_shift) + 1;
    NoteUse(address, size);
    // Page table entries for the blocks not published yet (those mapped now: an unmapped block
    // is tried again next time).
    std::vector<std::pair<u64, u64>> gaps;
    layer_published.ForEachGap(first, end, [&](u64 a, u64 b) { gaps.emplace_back(a, b); });
    for (const auto& [a, b] : gaps) {
        u64 run = a;
        for (u64 block = a; block <= b; ++block) {
            const bool mapped = block < b && Layer().ResolvePrefix(block << block_shift, 1);
            if (!mapped) {
                if (run < block) {
                    LayerWriteEntries(run, block);
                    layer_published.Add({run, block});
                }
                run = block + 1;
            }
        }
    }
    if (is_written) {
        NoteWriteTick(address, size);
        // Where it writes is unknown: what is in VRAM there may now differ from the game's
        // memory; it is copied back before it goes in place (LayerDemote).
        if (!force_writes_in_place) {
            Layer().ForEachValid(first, end, [&](u64 a, u64 b) {
                gpu_modified_ranges.Add(a << block_shift, (b - a) << block_shift);
            });
        }
    }
    if (!layer_trash) {
        layer_trash = std::make_unique<Buffer>(instance, 0, 64_KB, MemoryType::DeviceLocal,
                                               "bbport paged trash");
    }
    const vk::DeviceAddress trash = layer_trash->BufferDeviceAddress();
    const u32 record[8] = {static_cast<u32>(address),
                           static_cast<u32>(address >> 32),
                           static_cast<u32>(std::min<u64>(size, 0xFFFFFFFFull)),
                           u32(is_written && force_writes_in_place),
                           static_cast<u32>(trash),
                           static_cast<u32>(trash >> 32),
                           static_cast<u32>(bda_pagetable_buffer->SizeBytes() /
                                            (2 * sizeof(vk::DeviceAddress))),
                           0};
    const u64 offset = stream_buffer.Copy(record, sizeof(record), instance.StorageMinAlignment());
    BbStats::bound_in_place_bytes.fetch_add(size, std::memory_order_relaxed);
    return {&stream_buffer, offset};
}

bool BufferCache::LayerActive() {
    return GuestInPlace() && BbGuestMemory::LayerMemory();
}

} // namespace VideoCore

namespace VideoCore {
bool BufferCache::LayerPagedActive() {
    // BB_LAYER_PAGED=0 (tests): huge bindings cut in place instead of the page table.
    static const bool paged = [] {
        const char* env = std::getenv("BB_LAYER_PAGED");
        return !(env && env[0] == '0');
    }();
    return paged && LayerActive();
}

int BufferCache::LayerGpuWritesMode() {
    // Anywhere (2) gave exploding vertices and garbage textures: GPU data in a VRAM copy of memory
    // the CPU also uses (the game's buffers, its heap's headers, reused allocations) went stale or
    // was copied back over newer CPU data. Images (render targets) are the GPU's own.
    static const int mode = [] {
        const char* env = std::getenv("BB_LAYER_GPU_WRITES");
        return env && (env[0] == '0' || env[0] == '2') ? env[0] - '0' : 1;
    }();
    return mode;
}

bool BufferCache::LayerInImage(VAddr address, u64 size) {
    bool inside = false;
    texture_cache.ForEachImageInRegion(address, size, [&](ImageId, Image& image) {
        inside = inside || (image.info.guest_address <= address &&
                            address + size <= image.info.guest_address + image.info.guest_size);
    });
    return inside;
}

bool BufferCache::LayerGpuWriteToVram(VAddr address, u64 size) {
    const int mode = LayerGpuWritesMode();
    const bool vram =
        mode == 2 || (mode == 1 && !layer_cpu_blocks.Overlaps(address >> block_shift,
                                                              ((address + size - 1) >> block_shift) + 1));
    // BB_LAYER_WRITE_LOG=1 (diagnostics): large GPU writes kept in place, and the images near them.
    static const bool log = std::getenv("BB_LAYER_WRITE_LOG") != nullptr;
    if (log && !vram && size >= (u64{1} << 20)) {
        static std::map<std::pair<VAddr, u64>, u32> seen;
        if (seen.size() < 64 && seen[{address, size}]++ == 0) {
            std::string near;
            texture_cache.ForEachImageInRegion(address, size, [&](ImageId, Image& image) {
                near += fmt::format(" [{:#x}+{:#x} {}x{} fmt {}]", image.info.guest_address,
                                    image.info.guest_size, image.info.size.width,
                                    image.info.size.height, u32(image.info.pixel_format));
            });
            std::printf("Layer write in place: %#llx+%#llx; images:%s\n",
                        (unsigned long long)address, (unsigned long long)size,
                        near.empty() ? " none" : near.c_str());
        }
    }
    return vram;
}
} // namespace VideoCore

namespace VideoCore {
void BufferCache::LayerCopyBack(u64 first, u64 end) {
    LayerForgetGpuData();
    // The GPU's data in mirrors over [first, end) into the game's memory; the blocks stay valid
    // in VRAM (both copies equal now).
    std::vector<std::pair<u64, u64>> runs;
    Layer().ForEachValid(first, end, [&](u64 a, u64 b) { runs.emplace_back(a, b); });
    for (const auto& [a, b] : runs) {
        const VAddr from = a << block_shift;
        const u64 bytes = (b - a) << block_shift;
        if (!gpu_modified_ranges.Intersects(from, bytes)) {
            continue;
        }
        const auto mirror = Layer().MirrorAt(from);
        if (!mirror) {
            continue;
        }
        auto* buffer = static_cast<Buffer*>(mirror->owner);
        gpu_modified_ranges.ForEachInRange(from, bytes, [&](VAddr w_from, VAddr w_to) {
            if (const auto target = LayerInPlace(w_from, w_to - w_from)) {
                const vk::BufferCopy copy{buffer->Offset(w_from), target->second, w_to - w_from};
                runtime.CopyBuffer(buffer, target->first, std::span{&copy, 1});
                layer_copied_back_bytes += w_to - w_from;
                layer_copy_back_tick = scheduler.CurrentTick();
            }
        });
        gpu_modified_ranges.Subtract(from, bytes);
    }
}


// BB_LAYER_READ_TRAPS=1: what the GPU writes into a VRAM copy only is no access for the CPU until
// it is copied back (LayerReadTrapHit): the game or the translator reading it on the CPU would
// see stale data (on the PS4 the memory is one). Off by default for now.
bool BufferCache::LayerReadTrapsOn() {
    static const bool on = [] {
        const char* env = std::getenv("BB_LAYER_READ_TRAPS");
        return env && env[0] == '1';
    }();
    return on;
}

void BufferCache::LayerArmReadTraps(VAddr address, u64 size) {
    if (!LayerReadTrapsOn() || size == 0) {
        return;
    }
    constexpr u64 Page = 4096;
    const VAddr from = Common::AlignDown(address, Page);
    const VAddr to = Common::AlignUp(address + size, Page);
    std::scoped_lock lk{layer_read_trap_mutex};
    if (layer_read_trapped.Contains(from, to - from)) {
        return;
    }
    layer_read_trapped.ForEachNotInRange(from, to - from, [&](VAddr gap, u64 bytes) {
        BbLayer::WriteTraps::Set(gap, bytes, BbLayer::WriteTraps::VramData, true);
    });
    layer_read_trapped.Add(from, to - from);
}

void BufferCache::LayerQueuePromotion(u64 first, u64 end) {
    thread_local std::array<std::pair<u64, u64>, 256> memo{};
    thread_local std::array<u64, 256> memo_generation{};
    const size_t slot = first & (memo.size() - 1);
    if (memo[slot] == std::pair{first, end} && memo_generation[slot] == layer_generation) {
        return;
    }
    memo[slot] = {first, end};
    memo_generation[slot] = layer_generation;
    if (!GarlicInVramForLayer()) {
        return;
    }
    for (u64 block = first; block < end; ++block) {
        if (!Layer().AnyValid(block, block + 1) && VramEligible(block)) {
            promote_candidates.Add({block, block + 1});
        }
    }
}
} // namespace VideoCore

namespace VideoCore {
bool BufferCache::IsInVramLayer(VAddr addr, u64 size) const {
    return Layer().Resolve(addr, size).kind == BbLayer::Resolution::Kind::Mirror;
}
} // namespace VideoCore

namespace VideoCore {
bool BufferCache::LayerAllKnown(u64 first, u64 end) {
    for (u64 block = first; block < end; ++block) {
        if (!Layer().AnyValid(block, block + 1) && !LayerEligible(block)) {
            return false;
        }
    }
    return true;
}
} // namespace VideoCore

namespace VideoCore {
void BufferCache::LayerMakeVolatile(const std::vector<u64>& blocks) {
    for (const u64 block : blocks) {
        if (Layer().AnyValid(block, block + 1) || !Layer().ResolveInPlace(block << block_shift, block_size)) {
            continue; // in VRAM already, or not a whole block of a chunk (no source to refresh from)
        }
        // Volatile before the page table entries are rewritten (they keep it in place).
        layer_volatile.Add({block, block + 1});
        LayerPromoteRun(block, block + 1);
        if (!Layer().AnyValid(block, block + 1)) {
            layer_volatile.Subtract(block, block + 1);
        }
    }
}

void BufferCache::LayerRefreshVolatile(VAddr address, u64 size) {
    BbStats::Timer timer{BbStats::t_layer[5]};
    // The binding's bytes in volatile blocks: uploaded from the game's memory by the
    // SynchronizeMemory that follows (as CPU-modified: a GPU copy from its chunk, in stream
    // order). Only reading bindings use them (LayerBind).
    const u64 first = address >> block_shift;
    const u64 end = ((address + size - 1) >> block_shift) + 1;
    layer_volatile.ForEachInRange(first, end, [&](const Interval& iv) {
        const VAddr from = std::max<VAddr>(address, iv.start << block_shift);
        const VAddr to = std::min<VAddr>(address + size, iv.end << block_shift);
        if (from >= to) {
            return;
        }
        memory_tracker->UnmarkRegionAsGpuModified(from, to - from);
        gpu_modified_ranges.Subtract(from, to - from);
        memory_tracker->MarkRegionAsCpuModified(from, to - from);
        layer_volatile_refresh_bytes += to - from;
    });
}
} // namespace VideoCore

// bbport BB_LAYER_MEMORY: blocks with a VRAM copy are write-protected (BB_LAYER_WRITE_TRAPS=0:
// off). The game's own code writes some of them without telling us (its heap's headers, parts of
// its resource loaders), and a stale copy then gives exploding vertices and garbage textures. A
// write there is let through at once; the block goes back in place before the next binding (the
// writers seen often stay there: NoteCpuWrite).
namespace VideoCore {
namespace {
std::mutex layer_trap_mutex;
std::unordered_set<u64> layer_trap_armed;              // blocks protected now
std::vector<std::pair<u64, u64>> layer_trap_hits;      // block, guest instruction (0: announced)
std::atomic<bool> layer_trap_pending{false};
std::atomic<u64> layer_trap_faults{0};
u32 layer_trap_shift = 16;

bool LayerTrapsOn() {
    static const bool on = [] {
        const char* env = std::getenv("BB_LAYER_WRITE_TRAPS");
        return !(env && env[0] == '0');
    }();
    return on;
}

void LayerUnprotect(u64 block) {
    // Writable again unless another owner (an image there) still traps it (bblayer_write_traps.h).
    BbLayer::WriteTraps::Set(block << layer_trap_shift, u64{1} << layer_trap_shift,
                             BbLayer::WriteTraps::Mirror, false);
}

bool LayerTrapHandler(void* context, void* fault_address) {
    if (!Common::IsWriteError(context)) {
        return false;
    }
    const u64 block = reinterpret_cast<u64>(fault_address) >> layer_trap_shift;
    {
        std::scoped_lock lk{layer_trap_mutex};
        if (!layer_trap_armed.erase(block)) {
            return false;
        }
        layer_trap_hits.emplace_back(block, u64(reinterpret_cast<uintptr_t>(Common::GetRip(context))));
    }
    LayerUnprotect(block); // the write runs again, now through
    layer_trap_faults.fetch_add(1, std::memory_order_relaxed);
    layer_trap_pending.store(true, std::memory_order_release);
    return true;
}
} // namespace

void BufferCache::LayerArmTraps(u64 first, u64 end) {
    if (!LayerTrapsOn()) {
        return;
    }
    static const bool registered = [this] {
        layer_trap_shift = static_cast<u32>(block_shift);
        Core::Signals::Instance()->RegisterAccessViolationHandler(LayerTrapHandler, 0);
        return true;
    }();
    (void)registered;
    // One mprotect per run of blocks, not per block: each call interrupts every core running the
    // game's threads (TLB shootdown), and a mirror of 100 MiB made 1600 of them (the GPU thread
    // 250 ms in the kernel while an area streamed in).
    std::scoped_lock lk{layer_trap_mutex};
    u64 run_first = 0, run_end = 0;
    const auto flush = [&] {
        if (run_end > run_first) {
            BbStats::Timer timer{BbStats::t_layer[0]};
            BbStats::layer_mprotects.fetch_add(1, std::memory_order_relaxed);
            BbLayer::WriteTraps::Set(run_first << block_shift, (run_end - run_first) << block_shift,
                                     BbLayer::WriteTraps::Mirror, true);
        }
        run_first = run_end = 0;
    };
    int prot = 0, type = -1;
    uintptr_t vma_start = 0, vma_end = 0;
    for (u64 block = first; block < end; ++block) {
        if (layer_volatile.Contains(block, block + 1)) {
            flush(); // refreshed for every binding anyway
            continue;
        }
        const VAddr address = block << block_shift;
        // Only what the game's CPU may write (SCE_KERNEL_PROT_CPU_WRITE), whole in one mapping.
        if (address < vma_start || address >= vma_end) {
            if (!runtime_memory_vma_info(address, &prot, &type, &vma_end)) {
                vma_start = vma_end = 0;
                flush();
                continue;
            }
            vma_start = address;
        }
        if (!(prot & 0x2) || vma_end < address + block_size || !layer_trap_armed.insert(block).second) {
            flush();
            continue;
        }
        if (run_end != block) {
            flush();
            run_first = block;
        }
        run_end = block + 1;
    }
    flush();
}

void BufferCache::LayerDisarmTraps(u64 first, u64 end, bool demote) {
    std::scoped_lock lk{layer_trap_mutex};
    if (layer_trap_armed.empty()) {
        return;
    }
    // As LayerArmTraps: one mprotect per run of blocks that were armed.
    u64 run_first = 0, run_end = 0;
    const auto flush = [&] {
        if (run_end > run_first) {
            BbStats::Timer timer{BbStats::t_layer[0]};
            BbStats::layer_mprotects.fetch_add(1, std::memory_order_relaxed);
            BbLayer::WriteTraps::Set(run_first << layer_trap_shift,
                                     (run_end - run_first) << layer_trap_shift,
                                     BbLayer::WriteTraps::Mirror, false);
        }
        run_first = run_end = 0;
    };
    for (u64 block = first; block < end; ++block) {
        if (!layer_trap_armed.erase(block)) {
            flush();
            continue;
        }
        if (run_end != block) {
            flush();
            run_first = block;
        }
        run_end = block + 1;
        if (demote) {
            layer_trap_hits.emplace_back(block, 0);
            layer_trap_pending.store(true, std::memory_order_release);
        }
    }
    flush();
}

void BufferCache::LayerProcessTraps() {
    BbStats::Timer timer{BbStats::t_layer[3]};
    LayerForgetGpuData(); // before any demotion copies the GPU's data back
    if (!layer_trap_pending.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<std::pair<u64, u64>> hits;
    {
        std::scoped_lock lk{layer_trap_mutex};
        hits.swap(layer_trap_hits);
        layer_trap_pending.store(false, std::memory_order_release);
    }
    for (const auto& [block, rip] : hits) {
        const bool watched = layer_watched.Contains(block, block + 1);
        ++layer_trap_kinds[rip == 0 ? 0 : (watched ? 1 : 2) + (IsGuestCode(rip) ? 0 : 2)];
        // The CPU uses this block: GPU writes there stay in place from now on (until it is handed
        // out again), so a VRAM copy never holds GPU data the CPU's data could meet.
        layer_cpu_blocks.Add({block, block + 1});
        // A watched block the CPU wrote (the game's code or ours: HLE command buffers): it stays in
        // VRAM, uploaded again; with BB_LAYER_WATCH_HOT=1 however often (as the model of 0.3
        // uploads what the CPU wrote), else until it counts as often written (NoteCpuWrite: then
        // a request sends it in place). An announced write may still be under way (a file read):
        // in place as below, no trap set meanwhile. The runtime's file reads retry once their
        // pages fault again (a trap set between its touch and the read).
        if (rip != 0 && !(watched && LayerWatchHot())) {
            NoteCpuWrite(block << block_shift, rip); // often written: in place for good
        }
        if (rip != 0 && watched && Layer().AnyValid(block, block + 1)) {
            LayerRefreshWatched(block);
            continue;
        }
        // Written by the CPU since its copy was made: in place from now on, until it is known
        // data again. An announced write: the GPU's data beside its bytes is copied back first
        // (LayerForgetGpuData took its bytes out). A write nobody announced: the CPU reuses the
        // memory, the GPU's data there is dead (copying it back would land over the CPU's).
        LayerDemote(block, block + 1, rip == 0, 3);
    }
}

u64 BufferCache::LayerTrapFaults() {
    return layer_trap_faults.load(std::memory_order_relaxed);
}
} // namespace VideoCore

namespace VideoCore {
bool BufferCache::LayerMirrored(VAddr addr, u64 size) {
    if (size == 0 || !LayerActive()) {
        return false;
    }
    // Asked for every small binding by two threads: the answer memoized per thread while no
    // block gained or lost its copy (the module's lock taken only then).
    struct Memo {
        VAddr addr = 0;
        u64 size = 0, version = ~0ULL;
        bool mirrored = false;
    };
    thread_local std::array<Memo, 1024> memo{};
    auto& m = memo[((addr >> 4) ^ (addr >> 14) ^ size) & (memo.size() - 1)];
    const u64 version = Layer().ValidVersion();
    if (m.addr == addr && m.size == size && m.version == version) {
        return m.mirrored;
    }
    const u32 shift = Layer().BlockShift();
    const bool mirrored = Layer().AnyValid(addr >> shift, ((addr + size - 1) >> shift) + 1);
    m = {addr, size, version, mirrored};
    return mirrored;
}
} // namespace VideoCore
