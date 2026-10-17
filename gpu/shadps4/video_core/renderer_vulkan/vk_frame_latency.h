// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: BB_LSFG_LATENCY=1: where a real frame spends its time between the game's flip and the
// hand-over to the app, with and without Lossless Scaling frame generation (vk_lsfg.h).
//
// Every game frame gets a record when its flip is submitted (Begin, on the GPU command thread). The
// presenter marks CPU steps on it (Mark) and names GPU steps by what signals them: a timeline value
// (a scheduler's work semaphore) or a sync file (WatchFd, the framegen's output). A thread of its own
// polls those every ~0.25 ms and stamps them when they signal, so nothing in the frame's path waits
// for it. Every 5 s one log line per mode gives the mean and 95th percentile of each step, in ms
// after the flip. Off (the default), every call returns at once.
#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::FrameLatency {

enum class Stage : u32 {
    GameSubmit,   ///< the game handed the frame's command buffers over (sceGnmSubmitAndFlip)
    Flip,         ///< its flip reached the video out driver (the record starts here)
    PresentStart, ///< the presenter's thread started on it
    GpuDone,      ///< the GPU finished rendering it
    CopyStart,    ///< frame generation: the copy into the framegen's input was recorded
    CopyDone,     ///< ... and finished on the GPU
    FramegenDone, ///< the first generated frame is complete (framegen device)
    GenHanded,    ///< the first generated frame was handed to the app
    GenReady,     ///< ... and its blit into the app's buffer finished
    RealHanded,   ///< the real frame was handed to the app
    RealReady,    ///< ... and its blit into the app's buffer finished
    Count,
};

bool Enabled();

/// The game submitted a frame (sceGnmSubmitAndFlip, any thread).
void NoteGameSubmit();

/// A new record for a frame whose flip was just submitted; 0 when off.
u64 Begin();
/// Stamps a CPU step now.
void Mark(u64 id, Stage stage);
/// Stamps `stage` when the timeline semaphore reaches `value`.
void WatchTimeline(u64 id, Stage stage, vk::Device device, vk::Semaphore semaphore, u64 value);
/// Stamps `stage` when the sync file signals; takes `fd` (a dup the caller made) over.
void WatchFd(u64 id, Stage stage, int fd);
/// No more steps come for this record: it is counted once its watches resolved.
/// `lsfg`: frame generation multiplier (0: off), for the report.
void Finish(u64 id, u32 lsfg);
/// Frames the presenter's thread had waiting when this one was queued (driver.cpp).
void NoteQueueDepth(u32 depth);

} // namespace Vulkan::FrameLatency
