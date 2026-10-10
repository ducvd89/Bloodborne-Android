// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_timestamps.h"

#include <array>
#include <cmath>
#include <cstdio>

#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/host_shaders/gpu_timestamp_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {
struct Push {
    u32 address[2];
    u32 base_ticks[2];
    u32 base_clock[2];
    u32 mask[2];
    u32 ratio;
    u32 slot;
};

void Split(u64 value, u32 (&out)[2]) {
    out[0] = static_cast<u32>(value);
    out[1] = static_cast<u32>(value >> 32);
}

/// Everything before it before everything after it (a few timestamps a frame).
void FullBarrier(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}
} // namespace

GpuTimestamps::GpuTimestamps(const Instance& instance_, Scheduler& scheduler_,
                             VideoCore::BufferCache& buffer_cache_)
    : instance{instance_}, scheduler{scheduler_}, buffer_cache{buffer_cache_} {
    const auto physical = instance.GetPhysicalDevice();
    const auto families = physical.getQueueFamilyProperties();
    const u32 family = instance.GetGraphicsQueueFamilyIndex();
    valid_bits = family < families.size() ? families[family].timestampValidBits : 0;
    period_ns = physical.getProperties().limits.timestampPeriod;
    if (valid_bits == 0 || period_ns <= 0.0) {
        valid_bits = 0;
        std::printf("GPU: no timestamps on the graphics queue: the GPU clock is written by the "
                    "CPU after the GPU\n");
        return;
    }
    const auto device = instance.GetDevice();
    pool = Check(device.createQueryPoolUnique({
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = NumSlots,
    }));
    results = std::make_unique<VideoCore::Buffer>(instance, 0, NumSlots * sizeof(u64),
                                                  VideoCore::MemoryType::DeviceLocal,
                                                  "GPU timestamps");
    const vk::DescriptorSetLayoutBinding binding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
    };
    set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = 1,
        .pBindings = &binding,
    }));
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                      .offset = 0,
                                      .size = sizeof(Push)};
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    }));
    const auto module = CompileSPV(GPU_TIMESTAMP_COMP, device);
    pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *pipeline_layout,
            }));
    device.destroyShaderModule(module);
}

GpuTimestamps::~GpuTimestamps() = default;

void GpuTimestamps::Calibrate() {
    calibrated = true;
    // A timestamp taken while the GPU has nothing else to do, between two host clock readings.
    scheduler.EndRendering();
    scheduler.Finish();
    scheduler.Record([pool = *pool](vk::CommandBuffer cmdbuf) {
        cmdbuf.resetQueryPool(pool, 0, 1);
        cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, 0);
    });
    const u64 before = AmdGpu::GetGpuClock64();
    scheduler.Finish();
    const u64 after = AmdGpu::GetGpuClock64();
    u64 ticks = 0;
    const auto result =
        instance.GetDevice().getQueryPoolResults(*pool, 0, 1, sizeof(ticks), &ticks, sizeof(ticks),
                                                 vk::QueryResultFlagBits::e64);
    if (result != vk::Result::eSuccess) {
        valid_bits = 0;
        std::printf("GPU: the calibration timestamp was not available (%s): the GPU clock is "
                    "written by the CPU after the GPU\n",
                    vk::to_string(result).c_str());
        return;
    }
    base_ticks = ticks;
    base_clock = before + (after - before) / 2;
    ratio = static_cast<u32>(std::llround(period_ns / 10.0 * double(1u << 24)));
    std::printf("GPU: the GPU clock (100 MHz) from Vulkan timestamps: %.3f ns a tick, %u valid "
                "bits, tied to the host clock within %.0f us\n",
                period_ns, valid_bits, double(after - before) * 10.0 / 1000.0 / 2.0);
}

bool GpuTimestamps::Write(VAddr address, bool end_of_pipe) {
    std::scoped_lock lk{mutex};
    if (!calibrated && Supported()) {
        Calibrate();
    }
    if (!Supported()) {
        return false;
    }
    scheduler.EndRendering(); // the copy and the dispatch are outside render passes
    // The value is read by the CPU: in place (a VRAM copy would show it a stale one).
    buffer_cache.force_writes_in_place = true;
    const auto [buffer, offset] = buffer_cache.ObtainBuffer(address, sizeof(u64), true);
    buffer_cache.force_writes_in_place = false;
    const u32 slot = next_slot;
    next_slot = next_slot + 1 == NumSlots ? 1 : next_slot + 1;
    Push push{};
    Split(buffer->BufferDeviceAddress() + offset, push.address);
    Split(base_ticks, push.base_ticks);
    Split(base_clock, push.base_clock);
    Split(valid_bits >= 64 ? ~0ull : (1ull << valid_bits) - 1, push.mask);
    push.ratio = ratio;
    push.slot = slot;
    scheduler.Record([this, slot, end_of_pipe, push](vk::CommandBuffer cmdbuf) {
        cmdbuf.resetQueryPool(*pool, slot, 1);
        // The end of the pipe: after all the work before it; else when the GPU gets here.
        cmdbuf.writeTimestamp2(end_of_pipe ? vk::PipelineStageFlagBits2::eAllCommands
                                           : vk::PipelineStageFlagBits2::eNone,
                               *pool, slot);
        cmdbuf.copyQueryPoolResults(*pool, slot, 1, results->Handle(), slot * sizeof(u64),
                                    sizeof(u64),
                                    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
        FullBarrier(cmdbuf);
        const vk::DescriptorBufferInfo info{results->Handle(), 0, NumSlots * sizeof(u64)};
        const vk::WriteDescriptorSet write{.dstBinding = 0,
                                           .descriptorCount = 1,
                                           .descriptorType = vk::DescriptorType::eStorageBuffer,
                                           .pBufferInfo = &info};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, write);
        cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                             &push);
        cmdbuf.dispatch(1, 1, 1);
        FullBarrier(cmdbuf);
    });
    ++written;
    return true;
}

} // namespace Vulkan
