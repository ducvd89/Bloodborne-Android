// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_gpu_labels.h"

namespace Vulkan {

void RecordPortableGpuLabel(vk::CommandBuffer command, vk::Buffer buffer, u64 offset,
                            u64 value, u32 num_bytes) {
    const auto barrier = [&](vk::PipelineStageFlags2 source, vk::AccessFlags2 source_access,
                             vk::PipelineStageFlags2 dest, vk::AccessFlags2 dest_access) {
        const vk::MemoryBarrier2 memory{.srcStageMask = source, .srcAccessMask = source_access,
                                        .dstStageMask = dest, .dstAccessMask = dest_access};
        command.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &memory});
    };
    // Wait for all earlier work, including reads of a reused label, and publish its outputs
    // to the CPU before publishing completion. updateBuffer copies these local words while
    // recording; no host write to guest memory or callback writes the label.
    barrier(vk::PipelineStageFlagBits2::eAllCommands,
            vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            vk::PipelineStageFlagBits2::eTransfer | vk::PipelineStageFlagBits2::eHost,
            vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eHostRead);
    if (num_bytes == 8) {
        const u32 high = u32(value >> 32);
        command.updateBuffer(buffer, offset + 4, sizeof(high), &high);
        // A CPU polling the low word must never see its new value with the old high word.
        barrier(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                vk::PipelineStageFlagBits2::eTransfer | vk::PipelineStageFlagBits2::eHost,
                vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eHostRead);
    }
    const u32 low = u32(value);
    command.updateBuffer(buffer, offset, sizeof(low), &low);
    // Subsequent GPU consumers and coherent host polling both see the label.
    barrier(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eAllCommands | vk::PipelineStageFlagBits2::eHost,
            vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
                vk::AccessFlagBits2::eHostRead);
}

} // namespace Vulkan
