// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_gsr2.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "video_core/host_shaders/gsr2_convert_comp.h"
#include "video_core/host_shaders/gsr2_upscale_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

namespace {

/// The shaders' push constants (gsr2_convert.comp / gsr2_upscale.comp): 64 bytes.
struct Params {
    u32 render_size[2];
    u32 display_size[2];
    float render_size_rcp[2];
    float display_size_rcp[2];
    float jitter[2];
    float pre_exposure;
    float camera_fov_horizontal;
    float min_lerp_contribution;
    u32 same_camera;
    u32 reset;
    u32 history_filter; ///< gsr2_upscale.comp only: 1 = Catmull-Rom history, 0 = bilinear
};
static_assert(sizeof(Params) == 64);

constexpr u32 kSlices = 4;     ///< profile results are read this many frames later
constexpr u32 kQueries = 3;    ///< start, after convert, after upscale

} // namespace

struct GsrUpscaler::Impl {
    const Instance& instance;
    Scheduler& scheduler;
    vk::Device device;

    vk::UniqueDescriptorSetLayout convert_layout, upscale_layout;
    vk::UniquePipelineLayout convert_pipeline_layout, upscale_pipeline_layout;
    vk::UniquePipeline convert_pipeline, upscale_pipeline;
    vk::UniqueSampler nearest, linear;
    bool pipelines_ok = false;
    bool pipelines_failed = false;

    // Render-size intermediates and display-size history (two, swapped every frame).
    VideoCore::UniqueImage mdc_image, ycocg_image;
    VideoCore::UniqueImage history_image[2];
    vk::UniqueImageView mdc_view, ycocg_view, history_view[2];
    u32 render_w = 0, render_h = 0, display_w = 0, display_h = 0;
    u32 current = 0;
    bool fresh = true;

    // BB_GSR2_PROFILE=1
    const bool profile = [] {
        const char* env = std::getenv("BB_GSR2_PROFILE");
        return env && env[0] == '1';
    }();
    // BB_GSR2_NINE_TAP=1: the 9-sample kernel always (default: when the camera stands still, as
    // upstream's bSameCamera).
    const bool force_nine_tap = [] {
        const char* env = std::getenv("BB_GSR2_NINE_TAP");
        return env && env[0] == '1';
    }();
    // BB_GSR2_MIN_LERP=<0..1>: upstream's minLerpContribution (history kept when it leaves the
    // color box); the sample framework uses 0.25.
    const float min_lerp = [] {
        const char* env = std::getenv("BB_GSR2_MIN_LERP");
        return env && env[0] ? std::clamp(float(std::atof(env)), 0.0f, 1.0f) : 0.25f;
    }();
    // BB_GSR2_HISTORY=bilinear (or 0): upstream's single bilinear history fetch (default: a
    // Catmull-Rom filter, which does not blur the history a little more every frame).
    const bool history_catmull_rom = [] {
        const char* env = std::getenv("BB_GSR2_HISTORY");
        return !(env && (std::strcmp(env, "bilinear") == 0 || std::strcmp(env, "0") == 0));
    }();
    vk::UniqueQueryPool pool;
    double period_ns = 1.0;
    u64 frames = 0;
    std::array<bool, kSlices> recorded{};
    double sum_convert = 0.0, sum_upscale = 0.0;
    u32 samples = 0;
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();

    Impl(const Instance& instance_, Scheduler& scheduler_)
        : instance{instance_}, scheduler{scheduler_}, device{instance_.GetDevice()} {}

    ~Impl() {
        if (mdc_image || history_image[0]) {
            scheduler.Finish();
        }
    }

    bool CreatePipelines() {
        if (pipelines_ok) {
            return true;
        }
        if (pipelines_failed) {
            return false;
        }
        const auto make_layout = [&](u32 samplers, u32 storages, vk::UniqueDescriptorSetLayout& layout,
                                     vk::UniquePipelineLayout& pipeline_layout) {
            std::array<vk::DescriptorSetLayoutBinding, 5> bindings{};
            for (u32 i = 0; i < samplers + storages; ++i) {
                bindings[i] = {.binding = i,
                               .descriptorType = i < samplers
                                                     ? vk::DescriptorType::eCombinedImageSampler
                                                     : vk::DescriptorType::eStorageImage,
                               .descriptorCount = 1,
                               .stageFlags = vk::ShaderStageFlagBits::eCompute};
            }
            layout = Check(device.createDescriptorSetLayoutUnique({
                .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
                .bindingCount = samplers + storages,
                .pBindings = bindings.data(),
            }));
            const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(Params)};
            pipeline_layout = Check(device.createPipelineLayoutUnique({
                .setLayoutCount = 1,
                .pSetLayouts = &*layout,
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &push,
            }));
        };
        make_layout(3, 2, convert_layout, convert_pipeline_layout);
        make_layout(3, 2, upscale_layout, upscale_pipeline_layout);
        const auto sampler = [&](vk::Filter filter) {
            return Check(device.createSamplerUnique({
                .magFilter = filter, .minFilter = filter,
                .mipmapMode = vk::SamplerMipmapMode::eNearest,
                .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                .addressModeW = vk::SamplerAddressMode::eClampToEdge,
            }));
        };
        nearest = sampler(vk::Filter::eNearest);
        linear = sampler(vk::Filter::eLinear);
        const auto compute = [&](const auto& code, vk::PipelineLayout layout) {
            const auto module = CompileSPV(code, device);
            auto pipeline = CreateComputePipeline(device, module, layout);
            device.destroyShaderModule(module);
            return pipeline;
        };
        convert_pipeline = compute(GSR2_CONVERT_COMP, *convert_pipeline_layout);
        upscale_pipeline = compute(GSR2_UPSCALE_COMP, *upscale_pipeline_layout);
        if (profile) {
            pool = Check(device.createQueryPoolUnique({.queryType = vk::QueryType::eTimestamp,
                                                       .queryCount = kSlices * kQueries}));
            period_ns = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
        }
        pipelines_ok = true;
        return true;
    }

    void MakeImage(VideoCore::UniqueImage& image, vk::UniqueImageView& view, vk::Format format,
                   u32 width, u32 height, vk::ImageUsageFlags usage) {
        view.reset();
        image = VideoCore::UniqueImage(device, instance.GetAllocator());
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        view = Check(device.createImageViewUnique({
            .image = vk::Image(image),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    }

    void EnsureImages(u32 rw, u32 rh, u32 ow, u32 oh) {
        if (mdc_image && rw == render_w && rh == render_h && ow == display_w && oh == display_h) {
            return;
        }
        // Submitted frames may still use the old images: destroyed once the GPU is past them
        // (Finish() here would send the command buffer being recorded).
        if (mdc_image) {
            scheduler.DeferOperation([images = std::array{std::move(mdc_image), std::move(ycocg_image),
                                                          std::move(history_image[0]),
                                                          std::move(history_image[1])},
                                      views = std::array{std::move(mdc_view), std::move(ycocg_view),
                                                         std::move(history_view[0]),
                                                         std::move(history_view[1])}]() mutable {
                for (auto& view : views) view.reset();
                for (auto& image : images) image.Destroy();
            });
        }
        const auto sampled_storage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
        MakeImage(mdc_image, mdc_view, vk::Format::eR16G16B16A16Sfloat, rw, rh, sampled_storage);
        MakeImage(ycocg_image, ycocg_view, vk::Format::eR32Uint, rw, rh, sampled_storage);
        for (u32 i = 0; i < 2; ++i) {
            MakeImage(history_image[i], history_view[i], vk::Format::eR16G16B16A16Sfloat, ow, oh,
                      sampled_storage | vk::ImageUsageFlagBits::eTransferDst);
        }
        render_w = rw;
        render_h = rh;
        display_w = ow;
        display_h = oh;
        current = 0;
        fresh = true;
        std::printf("Upscaler: GSR 2 %ux%u -> %ux%u\n", rw, rh, ow, oh);
        std::printf(history_catmull_rom
                        ? "Upscaler: GSR 2 history Catmull-Rom (BB_GSR2_HISTORY=bilinear: upstream's bilinear)\n"
                        : "Upscaler: GSR 2 history bilinear (upstream's; default: Catmull-Rom)\n");
    }

    void CollectProfile(u32 slice) {
        if (!recorded[slice]) {
            return;
        }
        std::array<u64, kQueries * 2> data{}; // value, availability
        const auto result = device.getQueryPoolResults(
            *pool, slice * kQueries, kQueries, sizeof(data), data.data(), 2 * sizeof(u64),
            vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
        if (result != vk::Result::eSuccess || !data[1] || !data[3] || !data[5]) {
            return; // not finished yet: tried again at the slice's next turn
        }
        recorded[slice] = false;
        sum_convert += double(data[2] - data[0]) * period_ns * 1e-6;
        sum_upscale += double(data[4] - data[2]) * period_ns * 1e-6;
        ++samples;
        const auto now = std::chrono::steady_clock::now();
        if (now - window >= std::chrono::seconds(5) && samples) {
            std::printf("GSR 2 GPU: convert %.3f ms, upscale %.3f ms, total %.3f ms "
                        "(%u frames, %ux%u -> %ux%u)\n",
                        sum_convert / samples, sum_upscale / samples,
                        (sum_convert + sum_upscale) / samples, samples, render_w, render_h,
                        display_w, display_h);
            sum_convert = sum_upscale = 0.0;
            samples = 0;
            window = now;
        }
    }

    bool Record(const Frame& frame) {
        if (!CreatePipelines()) {
            return false;
        }
        EnsureImages(frame.render_width, frame.render_height, frame.output_width,
                     frame.output_height);
        const auto cmd = frame.cmdbuf;
        const u32 slice = u32(frames % kSlices);
        if (profile) {
            CollectProfile(slice);
            cmd.resetQueryPool(*pool, slice * kQueries, kQueries);
            cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *pool, slice * kQueries);
        }
        const bool clear = frame.reset || fresh;
        const auto all = vk::PipelineStageFlagBits2::eAllCommands;
        const auto compute = vk::PipelineStageFlagBits2::eComputeShader;
        const auto range = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const auto previous = 1 - current;

        // The scene, depth and motion writes, and last frame's history use, before this frame.
        const vk::MemoryBarrier2 inputs{
            .srcStageMask = all,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = compute,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        };
        const auto rw = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite;
        const auto rw_all = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
        std::array<vk::ImageMemoryBarrier2, 4> barriers{};
        // Intermediates and the output: contents are rewritten every frame.
        barriers[0] = {.srcStageMask = all, .srcAccessMask = rw_all, .dstStageMask = compute,
                       .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                       .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral,
                       .image = vk::Image(mdc_image), .subresourceRange = range};
        barriers[1] = barriers[0];
        barriers[1].image = vk::Image(ycocg_image);
        // The history written this frame (read two frames ago); the one read keeps its contents
        // unless this frame starts over (then both are cleared below).
        barriers[2] = barriers[0];
        barriers[2].dstAccessMask = rw;
        barriers[2].image = vk::Image(history_image[current]);
        barriers[2].oldLayout = clear ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral;
        barriers[3] = barriers[2];
        barriers[3].dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
        barriers[3].image = vk::Image(history_image[previous]);
        if (clear) {
            // No stale or uninitialized history (NaN would survive a zero weight).
            for (auto* b : {&barriers[2], &barriers[3]}) {
                b->dstStageMask = vk::PipelineStageFlagBits2::eClear;
                b->dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
                b->newLayout = vk::ImageLayout::eTransferDstOptimal;
            }
        }
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &inputs,
                              .imageMemoryBarrierCount = u32(barriers.size()),
                              .pImageMemoryBarriers = barriers.data()});
        if (clear) {
            const vk::ClearColorValue zero{std::array<float, 4>{}};
            for (u32 i = 0; i < 2; ++i) {
                cmd.clearColorImage(vk::Image(history_image[i]), vk::ImageLayout::eTransferDstOptimal,
                                    zero, range);
            }
            const std::array<vk::ImageMemoryBarrier2, 2> cleared{{
                {.srcStageMask = vk::PipelineStageFlagBits2::eClear,
                 .srcAccessMask = vk::AccessFlagBits2::eTransferWrite, .dstStageMask = compute,
                 .dstAccessMask = rw, .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                 .newLayout = vk::ImageLayout::eGeneral,
                 .image = vk::Image(history_image[current]), .subresourceRange = range},
                {.srcStageMask = vk::PipelineStageFlagBits2::eClear,
                 .srcAccessMask = vk::AccessFlagBits2::eTransferWrite, .dstStageMask = compute,
                 .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
                 .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                 .newLayout = vk::ImageLayout::eGeneral,
                 .image = vk::Image(history_image[previous]), .subresourceRange = range},
            }};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = u32(cleared.size()),
                                  .pImageMemoryBarriers = cleared.data()});
        }

        Params params{};
        params.render_size[0] = frame.render_width;
        params.render_size[1] = frame.render_height;
        params.display_size[0] = frame.output_width;
        params.display_size[1] = frame.output_height;
        params.render_size_rcp[0] = 1.0f / float(frame.render_width);
        params.render_size_rcp[1] = 1.0f / float(frame.render_height);
        params.display_size_rcp[0] = 1.0f / float(frame.output_width);
        params.display_size_rcp[1] = 1.0f / float(frame.output_height);
        params.jitter[0] = frame.jitter[0];
        params.jitter[1] = frame.jitter[1];
        params.pre_exposure = 1.0f;
        // tan(vertical fov / 2) * width / height
        params.camera_fov_horizontal = std::tan(frame.vertical_fov * 0.5f) *
                                       float(frame.render_width) / float(frame.render_height);
        params.min_lerp_contribution = min_lerp;
        params.same_camera = frame.camera_still || force_nine_tap ? 1u : 0u;
        params.reset = clear ? 1u : 0u;
        params.history_filter = history_catmull_rom ? 1u : 0u;

        const auto image = [](vk::Sampler sampler, vk::ImageView view) {
            return vk::DescriptorImageInfo{.sampler = sampler, .imageView = view,
                                           .imageLayout = vk::ImageLayout::eGeneral};
        };
        const auto write = [](u32 binding, vk::DescriptorType type, const vk::DescriptorImageInfo* info) {
            return vk::WriteDescriptorSet{.dstBinding = binding, .descriptorCount = 1,
                                          .descriptorType = type, .pImageInfo = info};
        };
        constexpr auto sampled = vk::DescriptorType::eCombinedImageSampler;
        constexpr auto storage = vk::DescriptorType::eStorageImage;

        {
            const std::array<vk::DescriptorImageInfo, 5> infos{
                image(*nearest, frame.color), image(*nearest, frame.depth),
                image(*nearest, frame.motion), image({}, *mdc_view), image({}, *ycocg_view)};
            const std::array<vk::WriteDescriptorSet, 5> writes{
                write(0, sampled, &infos[0]), write(1, sampled, &infos[1]),
                write(2, sampled, &infos[2]), write(3, storage, &infos[3]),
                write(4, storage, &infos[4])};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *convert_pipeline);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *convert_pipeline_layout, 0,
                                     writes);
            cmd.pushConstants(*convert_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                              sizeof(params), &params);
            cmd.dispatch((frame.render_width + 7) / 8, (frame.render_height + 7) / 8, 1);
        }
        if (profile) {
            cmd.writeTimestamp2(all, *pool, slice * kQueries + 1);
        }
        const vk::MemoryBarrier2 converted{
            .srcStageMask = compute,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = compute,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        };
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &converted});
        {
            const std::array<vk::DescriptorImageInfo, 5> infos{
                image(*linear, *history_view[previous]), image(*linear, *mdc_view),
                image(*nearest, *ycocg_view), image({}, frame.output),
                image({}, *history_view[current])};
            const std::array<vk::WriteDescriptorSet, 5> writes{
                write(0, sampled, &infos[0]), write(1, sampled, &infos[1]),
                write(2, sampled, &infos[2]), write(3, storage, &infos[3]),
                write(4, storage, &infos[4])};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *upscale_pipeline);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *upscale_pipeline_layout, 0,
                                     writes);
            cmd.pushConstants(*upscale_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                              sizeof(params), &params);
            cmd.dispatch((frame.output_width + 7) / 8, (frame.output_height + 7) / 8, 1);
        }
        if (profile) {
            cmd.writeTimestamp2(all, *pool, slice * kQueries + 2);
            recorded[slice] = true;
        }
        current = previous;
        fresh = false;
        ++frames;
        return true;
    }
};

GsrUpscaler::GsrUpscaler(const Instance& instance, Scheduler& scheduler)
    : impl{std::make_unique<Impl>(instance, scheduler)} {}

GsrUpscaler::~GsrUpscaler() = default;

bool GsrUpscaler::Record(const Frame& frame) {
    return impl->Record(frame);
}

} // namespace Vulkan
