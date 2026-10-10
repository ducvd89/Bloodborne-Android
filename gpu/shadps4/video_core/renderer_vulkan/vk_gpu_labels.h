// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

/// End-of-pipe label using core Vulkan only. Must be recorded outside rendering. The caller
/// supplies a coherent, GPU-visible guest buffer, a 4/8-byte aligned offset and size 4 or 8.
/// Prior device writes become visible to host readers before the low (polled) word is written.
void RecordPortableGpuLabel(vk::CommandBuffer command, vk::Buffer buffer, u64 offset,
                            u64 value, u32 num_bytes);

} // namespace Vulkan
