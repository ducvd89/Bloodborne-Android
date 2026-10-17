// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: see vk_lsfg.h.
#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include "bb_lsfg.h"
#include "bb_lsfg_state.h"
#include "common/logging/log.h"
#include "vk_driver_loader.h"
#include "video_core/renderer_vulkan/vk_frame_latency.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_lsfg.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

// The first frames only fill the framegen's temporal history (the shaders read the last few
// frames); generated frames from fewer would be garbage. The fork's layer waits for as many.
constexpr u64 WarmupFrames = 4;
constexpr u64 WaitNs = 2'000'000'000;

constexpr u32 FormatRgba8 = 37;   // VK_FORMAT_R8G8B8A8_UNORM
constexpr u32 FormatRgba16f = 97; // VK_FORMAT_R16G16B16A16_SFLOAT

std::string EnvString(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

bool EnvFlag(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::strtol(value, nullptr, 10) != 0 : fallback;
}

u32 AhbFormatOf(u32 vk_format) {
    return vk_format == FormatRgba8 ? AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM
                                    : AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT;
}

vk::Format VkFormatOf(u32 vk_format) {
    return vk_format == FormatRgba8 ? vk::Format::eR8G8B8A8Unorm : vk::Format::eR16G16B16A16Sfloat;
}

} // namespace

struct Lsfg::Api {
    void* library{};
    decltype(&bb_lsfg_init) init{};
    decltype(&bb_lsfg_create_context) create_context{};
    decltype(&bb_lsfg_present) present{};
    decltype(&bb_lsfg_wait) wait{};
    decltype(&bb_lsfg_destroy_context) destroy_context{};
    decltype(&bb_lsfg_shutdown) shutdown{};
    decltype(&bb_lsfg_shader_set) shader_set{};
    decltype(&bb_lsfg_diagnostics) diagnostics{};
    BbLsfgInfo info{};
    bool started{false};
};

struct Lsfg::SharedImage {
    AHardwareBuffer* ahb{};
    vk::Image image{};
    vk::DeviceMemory memory{};
    bool used{false}; ///< acquired from the framegen device before: its layout is general
};

struct Lsfg::Context {
    int id{-1};
    u32 width{};
    u32 height{};
    std::array<SharedImage, 2> input;
    std::array<SharedImage, 3> output;
    u64 frame{0}; ///< presents so far: the parity picks the input buffer (as the framegen's own count)
};

struct Lsfg::Stats {
    std::chrono::steady_clock::time_point window_start{std::chrono::steady_clock::now()};
    u32 real{0};
    u32 generated{0};
    u32 host_waits{0};
    u32 ring_waits{0}; ///< chained: a semaphore's last use had not completed (should stay 0)
    double process_ms{0.0};
    double present_ms{0.0};
};

std::unique_ptr<Lsfg> Lsfg::Create(const Instance& instance, const BbLsfgState::State& state) {
    if (!state.On()) {
        return nullptr;
    }
    // FSR 3.1 frame generation (BB_FRAME_GEN, set at start by run-bionic.sh) makes the frames in
    // the upscaler: the two never run together. The app keeps them exclusive; this is the last word.
    if (const char* fsr = std::getenv("BB_FRAME_GEN"); fsr && fsr[0] == '1') {
        std::printf("LSFG: off (FSR 3.1 frame generation is on: turn it off in Graphics settings and restart)\n");
        std::fflush(stdout);
        return nullptr;
    }
    std::unique_ptr<Lsfg> lsfg{new Lsfg(instance)};
    lsfg->settings = state;
    lsfg->multiplier = state.multiplier;
    if (!lsfg->Initialize()) {
        return nullptr;
    }
    return lsfg;
}

Lsfg::Lsfg(const Instance& instance_) : instance{instance_}, api{std::make_unique<Api>()},
                                         stats{std::make_unique<Stats>()} {}

Lsfg::~Lsfg() {
    const vk::Device device = instance.GetDevice();
    // The framegen device may still be working on the last batch: wait for it before the context
    // and the buffers it writes go.
    if (context && context->id >= 0 && context->frame > 0 && api->started && api->wait) {
        api->wait(context->id, WaitNs);
    }
    if (device) {
        static_cast<void>(device.waitIdle());
    }
    if (context) {
        DestroyContext(*context);
        context.reset();
    }
    if (api->started && api->shutdown) {
        api->shutdown();
    }
    if (device) {
        for (const auto& entry : input_ready) {
            device.destroySemaphore(entry.semaphore);
        }
        for (const auto& entry : batch_wait) {
            device.destroySemaphore(entry.semaphore);
        }
        for (const auto& ring : output_wait) {
            for (const auto& entry : ring) {
                device.destroySemaphore(entry.semaphore);
            }
        }
        for (const auto fence : output_done) {
            device.destroyFence(fence);
        }
        device.destroyFence(copy_done);
    }
    // The library stays loaded (never dlclose'd): frame generation can be turned on again while
    // the game runs, and dlopen of it again would find it in the same state anyway.
}

void Lsfg::Disable(const char* reason) {
    if (!failed) {
        std::printf("LSFG: off (%s); the game goes on without frame generation\n", reason);
        std::fflush(stdout);
    }
    failed = true;
    static_cast<void>(instance.GetDevice().waitIdle());
    if (context) {
        DestroyContext(*context);
        context.reset();
    }
    if (api->started) {
        api->shutdown();
        api->started = false;
    }
}

bool Lsfg::Initialize() {
    const auto fail = [](const std::string& why) {
        std::printf("LSFG: off (%s); the game goes on without frame generation\n", why.c_str());
        std::fflush(stdout);
        return false;
    };
    const std::string dll = BbLsfgState::FindDll();
    if (dll.empty()) {
        return fail("no Lossless.dll: bbport/Lossless.dll or Bloodborne/Lossless.dll of shared storage");
    }
    const std::string lib_path = EnvString("BB_LSFG_LIB").empty() ? "libbloodframe.so" : EnvString("BB_LSFG_LIB");
    api->library = dlopen(lib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!api->library) {
        return fail(std::string{"cannot load "} + lib_path + ": " + dlerror());
    }
    const auto load = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(api->library, name));
        return fn != nullptr;
    };
    if (!load(api->init, "bb_lsfg_init") || !load(api->create_context, "bb_lsfg_create_context") ||
        !load(api->present, "bb_lsfg_present") || !load(api->wait, "bb_lsfg_wait") ||
        !load(api->destroy_context, "bb_lsfg_destroy_context") ||
        !load(api->shutdown, "bb_lsfg_shutdown") || !load(api->shader_set, "bb_lsfg_shader_set") ||
        !load(api->diagnostics, "bb_lsfg_diagnostics")) {
        return fail(lib_path + " lacks the bb_lsfg API");
    }

    BbLsfgConfig config{};
    config.vk_get_instance_proc_addr = reinterpret_cast<void*>(bb_vk_get_instance_proc_addr());
    config.dll_path = dll.c_str();
    const auto props_chain = instance.GetPhysicalDevice()
                                 .getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceIDProperties>();
    const auto& ids = props_chain.get<vk::PhysicalDeviceIDProperties>();
    std::memcpy(config.device_uuid, ids.deviceUUID.data(), 16);
    std::memcpy(config.driver_uuid, ids.driverUUID.data(), 16);
    config.flow_scale = std::clamp(settings.flow, 0.25f, 1.0f);
    config.performance = settings.performance ? 1 : 0;
    config.fp16 = settings.fp16 ? 1 : 0;
    config.generation_count = static_cast<int32_t>(multiplier - 1);
    shared_format = EnvString("BB_LSFG_FORMAT") == "rgba8" ? FormatRgba8 : FormatRgba16f;
    config.shared_format = static_cast<int32_t>(shared_format);
    config.hdr = 0;
    pace = EnvFlag("BB_LSFG_PACE", true);
    chain = EnvFlag("BB_LSFG_CHAIN", true);
    if (const char* value = std::getenv("BB_LSFG_FRAMES"); value && *value) {
        frames_cap = static_cast<u32>(std::clamp(int(std::strtol(value, nullptr, 10)), 0, 8));
    }
    // Comparisons while the game runs: "<state file>.tune" (chain=, pace=, frames=) overrides the
    // environment for every new Lsfg (a change of the state file makes one).
    if (const char* state_path = BbLsfgState::FilePath()) {
        if (FILE* tune = std::fopen((std::string{state_path} + ".tune").c_str(), "r")) {
            char key[32];
            int value = 0;
            while (std::fscanf(tune, " %31[a-z]=%d", key, &value) == 2) {
                const std::string name{key};
                if (name == "chain") {
                    chain = value != 0;
                } else if (name == "pace") {
                    pace = value != 0;
                } else if (name == "frames") {
                    frames_cap = static_cast<u32>(std::clamp(value, 0, 8));
                }
            }
            std::fclose(tune);
            std::printf("LSFG: tuning file: chain %d, pace %d, frames %u\n", chain ? 1 : 0, pace ? 1 : 0, frames_cap);
        }
    }
    sync_host = EnvString("BB_LSFG_SYNC") == "host";

    const auto start = std::chrono::steady_clock::now();
    char error[512] = {};
    if (api->init(&config, &api->info, error, sizeof error) != 0) {
        return fail(error[0] ? error : "the framegen could not start");
    }
    api->started = true;
    if (!api->info.sync_fd_ok && !sync_host) {
        std::printf("LSFG: the framegen device has no sync files: waiting on the CPU\n");
        sync_host = true;
    }
    char diagnostics[768] = {};
    api->diagnostics(diagnostics, sizeof diagnostics);
    std::printf("Bloodframe: x%u, flow scale %.2f, %s model, %s images, %s sync, started in %.0f ms; %s\n",
                multiplier, static_cast<double>(config.flow_scale),
                config.performance ? "performance" : "quality",
                shared_format == FormatRgba8 ? "RGBA8" : "RGBA16F", sync_host ? "CPU" : "sync-file",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                diagnostics);
    std::fflush(stdout);
    LogPrecision();

    // The game device's synchronization objects.
    const vk::Device device = instance.GetDevice();
    const vk::ExportSemaphoreCreateInfo export_info{
        .handleTypes = vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd,
    };
    const auto make_semaphore = [&](vk::Semaphore& out, const void* next) {
        auto [result, semaphore] = device.createSemaphore({.pNext = next});
        out = semaphore;
        return result == vk::Result::eSuccess;
    };
    const auto make_fence = [&](vk::Fence& out, bool signaled) {
        auto [result, fence] = device.createFence(
            {.flags = signaled ? vk::FenceCreateFlagBits::eSignaled : vk::FenceCreateFlags{}});
        out = fence;
        return result == vk::Result::eSuccess;
    };
    bool ok = make_fence(copy_done, true);
    for (auto& entry : input_ready) {
        ok = ok && make_semaphore(entry.semaphore, &export_info);
    }
    for (auto& entry : batch_wait) {
        ok = ok && make_semaphore(entry.semaphore, nullptr);
    }
    for (auto& ring : output_wait) {
        for (auto& entry : ring) {
            ok = ok && make_semaphore(entry.semaphore, nullptr);
        }
    }
    for (auto& fence : output_done) {
        ok = ok && make_fence(fence, true);
    }
    if (!ok) {
        return fail("the Vulkan semaphores and fences could not be created");
    }
    return true;
}

void Lsfg::LogPrecision() {
    char reason[256] = {};
    const int set = api->shader_set(reason, sizeof reason);
    // Said when it starts and again if the first context changes it (a driver that rejects a set
    // makes the framegen use the next one).
    if (precision_logged == set && reason_logged == reason) {
        return;
    }
    precision_logged = set;
    reason_logged = reason;
    switch (set) {
    case BB_LSFG_SET_FP16_SPIRV:
        shader_set_name = "FP16";
        std::printf("Bloodframe: shaders FP16 SPIR-V (%s)\n", reason);
        break;
    case BB_LSFG_SET_FP32_SPIRV:
        shader_set_name = "FP32";
        std::printf("Bloodframe: shaders FP32 SPIR-V (%s)\n", reason);
        break;
    default:
        shader_set_name = "FP32-DXBC";
        std::printf("Bloodframe: shaders DXBC (translated) (%s)\n", reason);
        break;
    }
    std::fflush(stdout);
}

bool Lsfg::CreateSharedImage(SharedImage& image, u32 width, u32 height, u64 ahb_usage, bool output) {
    const vk::Device device = instance.GetDevice();
    const AHardwareBuffer_Desc desc{
        .width = width,
        .height = height,
        .layers = 1,
        .format = AhbFormatOf(shared_format),
        .usage = ahb_usage,
    };
    if (AHardwareBuffer_allocate(&desc, &image.ahb) != 0 || !image.ahb) {
        std::printf("LSFG: AHardwareBuffer_allocate %ux%u (format %u, usage %#llx) failed\n", width, height,
                    desc.format, static_cast<unsigned long long>(ahb_usage));
        return false;
    }
    auto [props_result, props_chain] = device.getAndroidHardwareBufferPropertiesANDROID<
        vk::AndroidHardwareBufferPropertiesANDROID, vk::AndroidHardwareBufferFormatPropertiesANDROID>(
        *image.ahb);
    if (props_result != vk::Result::eSuccess) {
        std::printf("LSFG: vkGetAndroidHardwareBufferPropertiesANDROID failed: %s\n",
                    vk::to_string(props_result).c_str());
        return false;
    }
    const auto& props = props_chain.get<vk::AndroidHardwareBufferPropertiesANDROID>();
    const auto& format_props = props_chain.get<vk::AndroidHardwareBufferFormatPropertiesANDROID>();
    if (format_props.format != VkFormatOf(shared_format)) {
        std::printf("LSFG: the driver maps the buffer to %s, not %s\n",
                    vk::to_string(format_props.format).c_str(), vk::to_string(VkFormatOf(shared_format)).c_str());
        return false;
    }
    const auto required = output ? vk::FormatFeatureFlagBits::eTransferSrc | vk::FormatFeatureFlagBits::eBlitSrc
                                 : vk::FormatFeatureFlagBits::eTransferDst | vk::FormatFeatureFlagBits::eBlitDst;
    if ((format_props.formatFeatures & required) != required) {
        std::printf("LSFG: the driver cannot %s the shared buffers (features %s)\n",
                    output ? "blit from" : "blit into", vk::to_string(format_props.formatFeatures).c_str());
        return false;
    }
    const vk::ExternalMemoryImageCreateInfo external_info{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eAndroidHardwareBufferANDROID,
    };
    auto [image_result, vk_image] = device.createImage({
        .pNext = &external_info,
        .imageType = vk::ImageType::e2D,
        .format = format_props.format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = output ? vk::ImageUsageFlagBits::eTransferSrc : vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    if (image_result != vk::Result::eSuccess) {
        std::printf("LSFG: creating the shared image failed: %s\n", vk::to_string(image_result).c_str());
        return false;
    }
    image.image = vk_image;
    const auto memory_properties = instance.GetPhysicalDevice().getMemoryProperties();
    u32 memory_type = ~0u;
    for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if (!(props.memoryTypeBits & (1u << i))) {
            continue;
        }
        if (memory_type == ~0u) {
            memory_type = i;
        }
        if (memory_properties.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal) {
            memory_type = i;
            break;
        }
    }
    if (memory_type == ~0u) {
        std::printf("LSFG: no memory type for the shared buffer (bits %#x)\n", props.memoryTypeBits);
        return false;
    }
    const vk::MemoryDedicatedAllocateInfo dedicated{.image = vk_image};
    const vk::ImportAndroidHardwareBufferInfoANDROID import_info{.pNext = &dedicated, .buffer = image.ahb};
    auto [memory_result, memory] = device.allocateMemory({
        .pNext = &import_info,
        .allocationSize = props.allocationSize,
        .memoryTypeIndex = memory_type,
    });
    if (memory_result != vk::Result::eSuccess) {
        std::printf("LSFG: importing the shared buffer failed: %s\n", vk::to_string(memory_result).c_str());
        return false;
    }
    image.memory = memory;
    const auto bind_result = device.bindImageMemory(vk_image, memory, 0);
    if (bind_result != vk::Result::eSuccess) {
        std::printf("LSFG: binding the shared buffer failed: %s\n", vk::to_string(bind_result).c_str());
        return false;
    }
    return true;
}

void Lsfg::DestroySharedImage(SharedImage& image) {
    const vk::Device device = instance.GetDevice();
    if (image.image) {
        device.destroyImage(image.image);
    }
    if (image.memory) {
        device.freeMemory(image.memory);
    }
    if (image.ahb) {
        AHardwareBuffer_release(image.ahb);
    }
    image = {};
}

void Lsfg::DestroyContext(Context& ctx) {
    if (ctx.id >= 0) {
        api->destroy_context(ctx.id);
        ctx.id = -1;
    }
    for (auto& image : ctx.input) {
        DestroySharedImage(image);
    }
    for (auto& image : ctx.output) {
        DestroySharedImage(image);
    }
}

bool Lsfg::EnsureContext(u32 width, u32 height) {
    if (context && context->width == width && context->height == height) {
        return true;
    }
    const auto start = std::chrono::steady_clock::now();
    // The game device may still read the old buffers.
    static_cast<void>(instance.GetDevice().waitIdle());
    auto next = std::make_unique<Context>();
    next->width = width;
    next->height = height;
    bool ok = true;
    for (auto& image : next->input) {
        ok = ok && CreateSharedImage(image, width, height, api->info.input_ahb_usage, false);
    }
    std::array<AHardwareBuffer*, 3> outs{};
    for (u32 i = 0; i < multiplier - 1; ++i) {
        ok = ok && CreateSharedImage(next->output[i], width, height, api->info.output_ahb_usage, true);
        outs[i] = next->output[i].ahb;
    }
    if (ok) {
        char error[512] = {};
        next->id = api->create_context(next->input[0].ahb, next->input[1].ahb, outs.data(), multiplier - 1,
                                       width, height, static_cast<int32_t>(shared_format), error, sizeof error);
        if (next->id < 0) {
            std::printf("LSFG: creating the context failed: %s\n", error);
            ok = false;
        }
    }
    if (!ok) {
        DestroyContext(*next);
        return false;
    }
    // The new context exists: the old one goes (so the framegen runtime stays up in between).
    if (context) {
        DestroyContext(*context);
    }
    context = std::move(next);
    batch_pending = -1;
    LogPrecision();
    std::printf("LSFG: context %ux%u created in %.0f ms\n", width, height,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    std::fflush(stdout);
    return true;
}

vk::Semaphore Lsfg::Reuse(RingSemaphore& entry) {
    if (entry.tick != 0 && scheduler_used && !scheduler_used->IsFree(entry.tick)) {
        ++stats->ring_waits;
        scheduler_used->Wait(entry.tick);
    }
    entry.tick = 0;
    return entry.semaphore;
}

void Lsfg::OutputSubmitted(u32 index, u64 tick) {
    if (index < output_wait.size()) {
        output_wait[index][output_slot[index]].tick = tick;
    }
}

bool Lsfg::ImportSyncFile(vk::Semaphore semaphore, int fd) {
    const auto result = instance.GetDevice().importSemaphoreFdKHR({
        .semaphore = semaphore,
        .flags = vk::SemaphoreImportFlagBits::eTemporary,
        .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd,
        .fd = fd,
    });
    if (result != vk::Result::eSuccess) {
        close(fd); // not taken over by the driver
        return false;
    }
    return true;
}

int Lsfg::ProcessFrame(Scheduler& scheduler, vk::Image image, u32 width, u32 height, vk::Semaphore ready,
                       u64 ready_tick, u64 latency_id) {
    if (failed) {
        return -1;
    }
    using Clock = std::chrono::steady_clock;
    const auto begin = Clock::now();
    if (last_frame != Clock::time_point{}) {
        const double ms = std::chrono::duration<double, std::milli>(begin - last_frame).count();
        if (ms > 0.0 && ms < 200.0) {
            interval_ms += (ms - interval_ms) * 0.2;
        }
    }
    last_frame = begin;
    if (!EnsureContext(width, height)) {
        Disable("the frame generation context could not be created");
        return -1;
    }
    const vk::Device device = instance.GetDevice();
    Context& ctx = *context;
    const u32 count = ctx.frame < WarmupFrames ? 0 : multiplier - 1;

    scheduler_used = &scheduler;
    const bool chained = Chained();
    // Not chained: the presents of the last cycle have finished reading the output buffers (the
    // framegen device writes them again), and the last copy has finished (the fence below is
    // reset). Chained: nothing to wait for here (vk_lsfg.h).
    if (!chained) {
        std::array<vk::Fence, 4> fences{copy_done};
        u32 fence_count = 1;
        for (u32 i = 0; i < multiplier - 1; ++i) {
            fences[fence_count++] = output_done[i];
        }
        if (device.waitForFences(fence_count, fences.data(), VK_TRUE, WaitNs) != vk::Result::eSuccess) {
            Disable("a shown frame did not finish in 2 s");
            return -1;
        }
        static_cast<void>(device.resetFences(copy_done));
    }

    FrameLatency::Mark(latency_id, FrameLatency::Stage::CopyStart);
    // 1. Copy the real frame into the input buffer of this parity, in a submission of its own.
    SharedImage& input = ctx.input[ctx.frame & 1];
    const vk::ImageSubresourceRange color_range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    const u32 graphics_family = instance.GetGraphicsQueueFamilyIndex();
    {
        const auto cmdbuf = scheduler.CommandBuffer();
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = input.used ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
                .dstQueueFamilyIndex = graphics_family,
                .image = input.image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barriers);
        const vk::ImageSubresourceLayers layers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        const vk::ImageBlit blit{
            .srcSubresource = layers,
            .srcOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(width), s32(height), 1}},
            .dstSubresource = layers,
            .dstOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(width), s32(height), 1}},
        };
        cmdbuf.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, input.image,
                         vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest);
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = graphics_family,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
                .image = input.image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        input.used = true;
    }
    RingSemaphore& input_entry = input_ready[cycle % RingSize];
    const vk::Semaphore input_semaphore = Reuse(input_entry);
    {
        SubmitInfo info{};
        info.AddWait(ready, ready_tick);
        // The framegen device must be done with the input buffers (the last batch) before this copy.
        RingSemaphore* batch_entry = batch_pending >= 0 ? &batch_wait[batch_pending] : nullptr;
        if (batch_entry) {
            info.AddWait(batch_entry->semaphore);
        }
        if (!sync_host) {
            info.AddSignal(input_semaphore);
        }
        if (!chained) {
            info.AddSignal(copy_done);
        }
        scheduler.Flush(info);
        const u64 copy_tick = scheduler.CurrentTick() - 1;
        input_entry.tick = copy_tick;
        if (batch_entry) {
            batch_entry->tick = copy_tick;
        }
        // The sync file of the semaphore can only be exported once its signal is submitted (the
        // submission may go out from a recording thread).
        scheduler.WaitSubmitted(copy_tick);
        FrameLatency::WatchTimeline(latency_id, FrameLatency::Stage::CopyDone, device,
                                    scheduler.GetWorkSemaphore()->Handle(), copy_tick);
    }
    batch_pending = -1;

    // 2. The sync file of that copy goes to the framegen device.
    int input_fd = -1;
    if (!sync_host) {
        const auto [result, fd] = device.getSemaphoreFdKHR({
            .semaphore = input_semaphore,
            .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd,
        });
        if (result == vk::Result::eSuccess) {
            input_fd = fd; // -1 is possible too: already signaled
        } else {
            std::printf("LSFG: exporting a sync file failed (%s): waiting on the CPU from now on\n",
                        vk::to_string(result).c_str());
            sync_host = true;
        }
    }
    if (sync_host) {
        if (chained) {
            // Fell back to the CPU in this call: the copy had no fence to wait on.
            scheduler.Wait(scheduler.CurrentTick() - 1);
        } else if (device.waitForFences(copy_done, VK_TRUE, WaitNs) != vk::Result::eSuccess) {
            Disable("the copy of a frame did not finish in 2 s");
            return -1;
        }
        ++stats->host_waits;
    }
    const auto present_start = Clock::now();
    BbLsfgPresent out{};
    char error[512] = {};
    if (api->present(ctx.id, input_fd, count, &out, error, sizeof error) != 0) {
        Disable(error[0] ? error : "the framegen failed");
        return -1;
    }
    ++ctx.frame;
    ++cycle;
    if (latency_id && count > 0 && out.out_ready_fd[0] >= 0) {
        FrameLatency::WatchFd(latency_id, FrameLatency::Stage::FramegenDone, dup(out.out_ready_fd[0]));
    }

    // 3. What the framegen device hands back: a sync file per generated frame, one for the batch.
    bool host_wait = !out.gpu_synced || sync_host;
    const auto take = [&](int fd, vk::Semaphore semaphore) -> bool {
        if (fd < 0) {
            return false;
        }
        if (host_wait || !ImportSyncFile(semaphore, fd)) {
            if (!host_wait) {
                std::printf("LSFG: importing a sync file failed: waiting on the CPU from now on\n");
                sync_host = true;
                host_wait = true;
            } else {
                close(fd);
            }
            return false;
        }
        return true;
    };
    std::array<bool, 3> output_waits{};
    std::array<vk::Semaphore, 3> output_semaphores{};
    for (u32 i = 0; i < count; ++i) {
        output_slot[i] = static_cast<u32>(cycle % RingSize);
        output_semaphores[i] = Reuse(output_wait[i][output_slot[i]]);
        output_waits[i] = take(out.out_ready_fd[i], output_semaphores[i]);
    }
    if (out.batch_complete_fd >= 0) {
        const u32 slot = static_cast<u32>(cycle % RingSize);
        if (take(out.batch_complete_fd, Reuse(batch_wait[slot]))) {
            batch_pending = static_cast<int>(slot);
        }
    }
    if (host_wait) {
        // Some dependency is not a GPU one: the batch is complete once this returns.
        if (api->wait(ctx.id, WaitNs) != 0) {
            Disable("the framegen did not finish in 2 s");
            return -1;
        }
        ++stats->host_waits;
        batch_pending = -1;
    }
    for (u32 i = 0; i < count; ++i) {
        // Chained: no fence; what reads the output is ordered before the next copy (vk_lsfg.h).
        outputs[i] = {ctx.output[i].image, output_waits[i] && !host_wait ? output_semaphores[i] : vk::Semaphore{},
                      chained ? vk::Fence{} : output_done[i]};
    }

    // Statistics.
    stats->process_ms += std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    stats->present_ms += std::chrono::duration<double, std::milli>(Clock::now() - present_start).count();
    ++stats->real;
    stats->generated += count;
    static const bool print_stats = EnvFlag("BB_LSFG_STATS", false);
    const double window = std::chrono::duration<double>(Clock::now() - stats->window_start).count();
    if (print_stats && window >= 5.0 && stats->real) {
        std::printf("LSFG: %.1f real + %.1f generated frames/s (x%u, %s); per real frame %.2f ms in the "
                    "copy and framegen call (%.2f ms of it the framegen call), %u CPU waits, %s%s\n",
                    stats->real / window, stats->generated / window, multiplier, shader_set_name,
                    stats->process_ms / stats->real, stats->present_ms / stats->real, stats->host_waits,
                    chained ? "chained" : "fence waits",
                    stats->ring_waits ? (", " + std::to_string(stats->ring_waits) + " ring waits").c_str() : "");
        std::fflush(stdout);
        *stats = Stats{};
    }
    return static_cast<int>(count);
}

} // namespace Vulkan
#endif // __ANDROID__
