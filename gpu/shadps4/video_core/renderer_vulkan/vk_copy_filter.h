// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: BB_COPY_SKIP_SAME (on by default, =0 off): the game's copy shader (~5000 copies of ~250
// bytes a frame, in ~57 calls) runs only the copies that change something.
//
// A copy is left out when the last copy into exactly its destination (nothing else copied over it
// since) had the same source bytes, and the destination was not touched meanwhile: a call whose
// copies are all left out is not recorded at all (on a tiler it would end the render pass for
// nothing: the G-buffer pass was broken ~47 times a frame by these copies). The check runs on a
// thread of its own (bb:CopyFilter), fed by stage A (the draw pipe's producer) in packet order;
// stage B (the recording thread) takes the result of its packet and never waits for it.
//
// Copy mode (BB_UMA): the copies run as HLE on VRAM copies of the game's memory. The destination
// holds the last copy's result exactly while the game's memory under it is unchanged (a CPU write
// there uploads over it, and the copy has to run again), so the history keeps a signature of the
// destination's pages in the game's memory (read from the backing view in precise readbacks mode,
// where GPU-written pages are protected), next to the hash of the source bytes. Sources the GPU
// modified are not trusted (their game memory is older): the call runs whole then. The filter is
// off where the translated shader runs itself (BB_GUEST_IN_PLACE without BB_UMA).
#pragma once

#include <atomic>
#include <thread>
#include <unordered_map>
#include <map>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"

namespace Vulkan {

class CopyFilter {
public:
    struct Job {
        u64 packet = 0;
        u64 epoch = 0;
        std::vector<CopyEntry> entries;
        std::vector<u8> control; ///< the kept copies' control list entries; empty: all kept
        u32 kept = 0;
        /// Page-aligned source ranges (address, size) of the copies left out: their data must be
        /// the game's (not modified by the GPU).
        std::vector<std::pair<u64, u64>> skipped_sources;
    };

    /// BB_COPY_SKIP_SAME (default on) and the copy shader runs as HLE.
    [[nodiscard]] static bool On();
    /// A filter exists (its history may need dropping).
    [[nodiscard]] static bool Started();
    static CopyFilter& Get();

    /// Stage A: the copies of the call in packet `packet` (taken from `entries`). A full queue
    /// posts nothing (stage B then finds no job and runs the call whole).
    void Post(u64 packet, std::vector<CopyEntry>& entries);

    /// Stage B: the checked job for `packet`, or null (none: the history is dropped). Jobs of
    /// packets before it never reached here (another path ran them): dropped the same way. Not
    /// checked yet: null with `unchecked` set, taken (the call runs whole, which writes what the
    /// check assumes anyway: the history holds). Never waits for the checking thread.
    Job* Take(u64 packet, bool& unchecked);
    /// Stage B, after Take returned a job: done with it.
    void Release();
    /// Stage B: a call did not run as its job said: the history starts again.
    void Invalidate();
    [[nodiscard]] u64 Epoch() const {
        return epoch.load(std::memory_order_acquire);
    }

private:
    struct Last {
        u64 size, hash, pages; ///< bytes, their hash, signature of the destination's pages
    };

    CopyFilter();
    void Run();
    void Check(Job& job);

    std::array<Job, 512> jobs;
    std::atomic<u64> head{0};      ///< stage A posts
    std::atomic<u64> processed{0}; ///< the checking thread checked
    std::atomic<u64> tail{0};      ///< stage B took
    std::atomic<u64> epoch{0};
    std::atomic<u64> unchecked_calls{0};
    // The checking thread's.
    std::unordered_map<u64, Last> last; ///< destination -> its last copy
    std::map<u64, u64> ranges;          ///< destination -> size (the overlap check)
    std::unordered_map<u64, u64> page_hashes; ///< of the job being checked
    std::vector<std::pair<u64, u64>> spans;
    u64 history_epoch = 0, total = 0, left_out = 0, calls = 0, printed_epoch = 0;
    u32 printed = 0;
    std::thread thread;
};

} // namespace Vulkan
