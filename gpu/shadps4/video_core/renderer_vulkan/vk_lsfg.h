// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: Lossless Scaling frame generation for the Bionic build (BB_LSFG_MULTIPLIER 2-4).
//
// The frame generation itself is Bloodframe, libbloodframe.so (tools/android/lsfg, based on GameNative's lsfg-vk fork's
// framegen backend; build_bloodframe.sh), loaded with dlopen the first time frame generation turns on. It
// runs on its own VkDevice of the same driver and physical device as the game's and exchanges images
// with this device as AHardwareBuffers: this class owns the game's side of that hand-over.
//
// For every real game frame (Presenter::Present):
//  1. the final picture (Frame::image, before the overlay) is blitted into the next of two input
//     buffers, in a submission of its own that signals a sync-file semaphore (ProcessFrame);
//  2. the library is given that sync file and makes the generated frames (multiplier - 1) into its
//     output buffers on the framegen device, signaling a sync file for each (the same call returns
//     them) and one for "done with the input buffers";
//  3. the presenter blits each output into a swapchain buffer (waiting for its sync file on the GPU)
//     and shows the generated frames first, the real frame last, spaced evenly over the real frame
//     interval; the "done with the input buffers" file is waited for by the next frame's copy.
// Buffers are handed between the two devices with ownership transfers to VK_QUEUE_FAMILY_EXTERNAL, as
// the fork's layer does. If a sync file cannot be exported or imported, the call waits on the CPU
// instead (BB_LSFG_SYNC=host forces that); if anything else fails, frame generation is off from then
// on and the game goes on with its own frames.
//
// Settings: multiplier (2, 3, 4), flow scale (0.25 .. 1.0, default 0.75) and performance (1: the light
// model, default) come from bb_lsfg_state.h (the app's live lsfg.state file; the presenter makes a new
// Lsfg when they change). Environment: BB_LSFG_DLL (the player's Lossless.dll, else bbport/ or shared
// storage), BB_LSFG_FORMAT (rgba16f, default, like the layer; rgba8),
// BB_LSFG_SYNC (fd, default; host), BB_LSFG_PACE (1, default: the presenter spaces the frames),
// BB_LSFG_STATS (1: real/generated presents per second in the log), BB_LSFG_LIB (path of the library).
#pragma once

#ifdef __ANDROID__
#include <array>
#include <chrono>
#include <memory>
#include "bb_lsfg_state.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

struct AHardwareBuffer;

namespace Vulkan {

class Instance;
class Scheduler;

class Lsfg {
public:
    /// A generated frame, ready to be shown: an AHardwareBuffer image (general layout, owned by the
    /// queue family EXTERNAL) that is complete once `wait` (when set) is signaled.
    struct Output {
        vk::Image image{};
        vk::Semaphore wait{}; ///< binary, imported from a sync file; null: nothing to wait for
        vk::Fence done{};     ///< to signal with the submission that reads `image` (reset by the caller)
    };

    /// Null when `state` is off or frame generation cannot run (one line in the log says why; the
    /// game goes on without it). The framegen's flow scale, model and generated-frame count are
    /// fixed when it starts: a change of any is a new Lsfg (destroy this one, Create another).
    static std::unique_ptr<Lsfg> Create(const Instance& instance, const BbLsfgState::State& state);
    ~Lsfg();

    u32 Multiplier() const {
        return multiplier;
    }
    const BbLsfgState::State& Settings() const {
        return settings;
    }
    bool Paced() const {
        return pace;
    }

    /// The next real frame: `image` (width x height, general layout) is complete when the timeline
    /// semaphore `ready` reaches `ready_tick`. Makes the generated frames and returns how many
    /// (0 for the first frames while the temporal history fills, else Multiplier() - 1), or -1 when
    /// frame generation failed and is off from now on. Output(i) is valid for i below the count.
    int ProcessFrame(Scheduler& scheduler, vk::Image image, u32 width, u32 height, vk::Semaphore ready,
                     u64 ready_tick);
    const Output& GetOutput(u32 index) const {
        return outputs[index];
    }

    /// Smoothed real frame interval in ms (for spacing the generated frames).
    double FrameIntervalMs() const {
        return interval_ms;
    }

private:
    struct Api;
    struct Context;
    struct SharedImage;
    struct Stats;

    explicit Lsfg(const Instance& instance);
    bool Initialize();
    bool EnsureContext(u32 width, u32 height);
    void DestroyContext(Context& context);
    bool CreateSharedImage(SharedImage& image, u32 width, u32 height, u64 ahb_usage, bool output);
    void DestroySharedImage(SharedImage& image);
    bool ImportSyncFile(vk::Semaphore semaphore, int fd);
    void Disable(const char* reason);

    const Instance& instance;
    std::unique_ptr<Api> api;
    std::unique_ptr<Context> context;
    std::unique_ptr<Stats> stats;
    BbLsfgState::State settings;
    u32 multiplier = 0;
    u32 shared_format = 0; ///< VkFormat of the shared images
    bool pace = true;
    bool sync_host = false; ///< no sync files: wait on the CPU
    bool failed = false;
    double interval_ms = 33.3;
    std::chrono::steady_clock::time_point last_frame{};
    std::array<Output, 3> outputs{};
    // Game-device synchronization objects.
    vk::Semaphore input_ready{};                    ///< exportable (sync file): the copy into the input
    std::array<vk::Semaphore, 2> batch_wait{};      ///< imported: the framegen device is done with the inputs
    std::array<vk::Semaphore, 3> output_wait{};     ///< imported: output i is complete
    std::array<vk::Fence, 3> output_done{};         ///< the presents reading output i have completed
    vk::Fence copy_done{};                          ///< the copy into the input has completed
    bool batch_wait_valid = false;
    u32 batch_slot = 0;
};

} // namespace Vulkan
#endif // __ANDROID__
