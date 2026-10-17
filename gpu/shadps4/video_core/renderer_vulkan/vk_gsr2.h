// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: Qualcomm Snapdragon Game Super Resolution 2 (BSD-3-Clause, gpu/third_party/snapdragon-gsr2)
// as an alternative to the FSR 3.1 context of TemporalUpscaler: the 2-pass compute variant
// (convert: depth clip, YCoCg color, motion; upscale: reprojected history plus a Lanczos kernel).
// It takes the same inputs as the FSR 4 hook: scene color, depth, the port's motion vectors
// (render pixels, previous - current) and the jitter offset. History and the intermediate images
// are its own and follow the render and output sizes. BB_GSR2_PROFILE=1 logs GPU ms per pass.

#pragma once

#include <array>
#include <memory>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

class GsrUpscaler {
public:
    GsrUpscaler(const Instance& instance, Scheduler& scheduler);
    ~GsrUpscaler();

    struct Frame {
        vk::CommandBuffer cmdbuf;
        /// Views in General layout: color and depth (depth aspect) sampled, motion (RG16F)
        /// sampled, output (RGBA16F) written. Color and depth may be larger than the scene.
        vk::ImageView color, depth, motion, output;
        u32 render_width, render_height, output_width, output_height;
        std::array<float, 2> jitter; ///< FSR's convention: pixels, the projection's shift
        float vertical_fov;          ///< radians
        bool camera_still, reset;
    };

    /// Records both passes into `frame.cmdbuf`; the output stays in General. False when the
    /// pipelines cannot be made.
    bool Record(const Frame& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
