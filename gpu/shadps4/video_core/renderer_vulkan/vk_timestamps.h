// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the command processor's GPU clock (end-of-pipe timestamps, COPY_DATA from the clock) as
// Vulkan timestamps, written by the GPU in stream order.
//
// The PS4's GPU clock counts at 100 MHz. Here one time line serves the CPU and the GPU: the host's
// monotonic clock in 10 ns ticks (AmdGpu::GetGpuClock64, what the CPU writes where the GPU
// cannot). A Vulkan timestamp is taken where the command processor would take its value (at the
// end of the pipe, or when it gets to a COPY_DATA), copied out of its query and turned into that
// time line by a one-thread dispatch (gpu_timestamp.comp) that writes it into the game's memory
// through its device address. The device ticks are tied to the host clock once, with a timestamp
// taken while the GPU is idle (the first use waits for the GPU twice). Values land before later
// labels: the dispatch is recorded where the packet is, outside a render pass (one is ended for
// it; games take a few timestamps a frame at most).
#pragma once

#include <memory>
#include <mutex>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {
class BufferCache;
struct Buffer;
} // namespace VideoCore

namespace Vulkan {

class Instance;
class Scheduler;

class GpuTimestamps {
public:
    GpuTimestamps(const Instance& instance, Scheduler& scheduler,
                  VideoCore::BufferCache& buffer_cache);
    ~GpuTimestamps();

    /// Whether the device has timestamps on the graphics queue.
    bool Supported() const noexcept {
        return valid_bits != 0;
    }

    /// The GPU clock written as a 64-bit value at `address` by the GPU in stream order: once the
    /// work before it is done (`end_of_pipe`), else when the GPU gets to it (recording thread).
    /// False: no timestamps here (the caller writes it with the CPU).
    bool Write(VAddr address, bool end_of_pipe);

    /// Timestamps written so far (BB_PM4_SELFTEST, statistics).
    u64 Written() const noexcept {
        return written;
    }

private:
    static constexpr u32 NumSlots = 256;

    void Calibrate();

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::BufferCache& buffer_cache;
    vk::UniqueQueryPool pool;
    std::unique_ptr<VideoCore::Buffer> results;
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline pipeline;
    u32 valid_bits = 0;
    double period_ns = 1.0;
    bool calibrated = false;
    u64 base_ticks = 0, base_clock = 0;
    u32 ratio = 0; ///< 8.24 fixed point: GPU clock ticks (10 ns) per device tick
    u32 next_slot = 1; ///< slot 0: the calibration
    u64 written = 0;
    std::mutex mutex; ///< the GPU command thread (compute queues) and the draw recording thread
};

} // namespace Vulkan
