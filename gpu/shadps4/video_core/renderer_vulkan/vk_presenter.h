// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <deque>

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "core/libraries/videoout/buffer.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#ifdef __ANDROID__
#include "video_core/renderer_vulkan/vk_lsfg.h"
#endif
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Image image;
    vk::ImageView image_view;
    vk::Fence present_done;
    vk::Semaphore ready_semaphore;
    u64 ready_tick;
    bool is_hdr{false};
    u8 id{};
    u64 latency_id{}; ///< bbport: BB_LSFG_LATENCY record of the game frame (vk_frame_latency.h), 0: none
};

enum SchedulerType {
    Draw,
    Present,
    CpuFlip,
};

class Rasterizer;

class Presenter {
public:
    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    ~Presenter();

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    bool IsHDRSupported() const {
        return swapchain.HasHDR();
    }

    void SetHDR(bool enable) {
        if (!IsHDRSupported()) {
            return;
        }
        swapchain.SetHDR(enable);
        pp_settings.hdr = enable ? 1 : 0;
    }

    VideoCore::Image& RegisterVideoOutSurface(
        const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const auto image_id = texture_cache.FindImage(desc);
        auto& image = texture_cache.GetImage(image_id);
        image.usage.vo_surface = 1u;
        return image;
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address);

    Frame* PrepareBlankFrame(bool present_thread);

    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);
    /// bbport: waits until the GPU has finished the commands that render `frame` (any thread).
    void WaitRendered(const Frame* frame);
    Frame* PrepareLastFrame();
    /// bbport: the frame generated for the frame PrepareFrame just returned (FSR 3.1 frame
    /// generation), to be presented half a frame before it; null when there is none.
    Frame* TakeGeneratedFrame() {
        Frame* frame = generated_frame;
        generated_frame = nullptr;
        return frame;
    }

private:
    /// What a presentation submission blits into the swapchain image: a game frame (`frame`: waits
    /// for its rendering, signals its present_done), or a generated frame (`external`: an
    /// AHardwareBuffer image of the frame generation's device, taken over from the queue family
    /// EXTERNAL; `wait` is complete when it is signaled, `done` is signaled when it was read).
    struct PresentSource {
        vk::Image image;
        u32 width;
        u32 height;
        Frame* frame{};
        bool external{};
        vk::Semaphore wait{};
        vk::Fence done{};
    };
    /// Acquires a swapchain buffer (it becomes the current one), records the blit of the source into
    /// it plus the overlay and queues it; false when there is no buffer (the frame is skipped).
    bool SubmitToSwapchain(const PresentSource& source);
    /// Hands the current swapchain buffer to the app.
    bool SendToApp();
#ifdef __ANDROID__
    /// Lossless Scaling frame generation for one real frame: queues it and its generated frames and
    /// hands them to the app in order, spaced. False: not done (frame generation failed), present
    /// the frame as usual.
    bool PresentWithLsfg(Frame* frame, u64 latency_id);
    /// Follows the app's live Lossless Scaling settings (bb_lsfg_state.h): called at the start of
    /// every present, between two frames, it looks at the state file at most every 250 ms and applies
    /// a change there and then (ApplyLsfgState).
    void PollLsfgState();
    /// Turns frame generation on, off or to other settings: a new Lsfg (the framegen's flow scale,
    /// model and generated-frame count are fixed when it starts), then the swapchain buffers and the
    /// app's presentation mode to match (ApplyLsfgOutputMode).
    void ApplyLsfgState(const BbLsfgState::State& next);
    /// Tells the swapchain the multiplier that runs now (0: none): the buffer count and FIFO follow.
    void ApplyLsfgOutputMode();
    /// The game-frame bound (frames_cap) for what runs now.
    void ApplyFramesCap();
#endif
    /// `capped`: a game frame (PrepareFrame, the GPU command thread): with frame generation on it also
    /// waits while frames_cap frames are out (taken, not yet presented).
    Frame* GetRenderFrame(bool capped = false);

    void RecreateFrame(Frame* frame, u32 width, u32 height);

    void SetExpectedGameSize(s32 width, s32 height);

private:
    float expected_ratio{1920.0 / 1080.0f};
    u32 expected_frame_width{1920};
    u32 expected_frame_height{1080};

    Frontend::WindowSDL& window;
    Instance instance;
    HostPasses::FsrPass fsr_pass;
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::PostProcessingPass pp_pass;
    std::mutex passes_mutex; ///< bbport: the passes are recorded on the recording threads
    AmdGpu::Liverpool* liverpool;
    Scheduler draw_scheduler;
    std::deque<u64> recent_frame_ticks; ///< bbport: BB_FRAMES_AHEAD bound (PrepareFrame)
    Frame* generated_frame = nullptr; ///< bbport: TakeGeneratedFrame
    Scheduler present_scheduler;
    Scheduler flip_scheduler;
#ifdef __ANDROID__
    // bbport: Lossless Scaling frame generation. Declared before the swapchain: the multiplier that
    // runs at start sets its first buffer count and presentation mode.
    BbLsfgState::State lsfg_requested; ///< what the state file last asked for
    BbLsfgState::Watcher lsfg_watch;
    std::unique_ptr<Lsfg> lsfg; ///< null: off, or it could not start / failed
    bool lsfg_paused = false; ///< bbport: frame generation paused while a movie plays (PollLsfgState)
    std::chrono::steady_clock::time_point lsfg_movie_seen{}; ///< when a movie last played (PollLsfgState)
#endif
    Swapchain swapchain;
    Runtime runtime;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    vk::UniqueCommandPool command_pool;
    std::vector<Frame> present_frames;
    std::queue<Frame*> free_queue;
    Frame* last_submit_frame;
    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::condition_variable_any frame_cv;
    std::vector<VAddr> vo_buffers_addr;
    u64 last_present_tick{}; ///< bbport: present_scheduler tick of the last SubmitToSwapchain
    /// bbport: with Lossless Scaling frame generation, the most game frames out at once (taken by
    /// PrepareFrame, not yet presented); 0: no bound but the pool (BB_LSFG_FRAMES, Lsfg::FramesCap).
    std::atomic<u32> frames_cap{0};
};

} // namespace Vulkan
