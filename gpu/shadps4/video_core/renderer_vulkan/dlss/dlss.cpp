// SPDX-License-Identifier: GPL-2.0-or-later
#include "dlss.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <utility>
#ifdef BB_DLSS_SDK
#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_vk.h"
#endif

namespace Dlss {
struct Upscaler::Impl {
  VkDevice device;
  std::function<void()> wait;
  bool available = false;
  std::string problem = "DLSS SDK not included in this build";
#ifdef BB_DLSS_SDK
  bool initialized = false;
  NVSDK_NGX_Parameter *params = nullptr;
  NVSDK_NGX_Handle *handle = nullptr;
  uint32_t width = 0, height = 0, ow = 0, oh = 0;
  int preset = -1;
  bool Check(NVSDK_NGX_Result r, const char *operation) {
    if (!NVSDK_NGX_FAILED(r))
      return true;
    char text[160];
    std::snprintf(text, sizeof(text), "%s failed (NGX 0x%x)", operation,
                  unsigned(r));
    problem = text;
    std::fprintf(stderr, "DLSS: %s\n", text);
    available = false;
    return false;
  }
#endif
  Impl(VkInstance instance, VkPhysicalDevice physical, VkDevice dev,
       std::function<void()> retire)
      : device(dev), wait(std::move(retire)) {
#ifdef BB_DLSS_SDK
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.vendorID != 0x10de) {
      problem = "DLSS requires an NVIDIA RTX GPU";
      return;
    }
    const char *env = std::getenv("BB_DLSS_DIR");
    const auto directory =
        std::filesystem::path(env && *env ? env : BB_DLSS_RUNTIME).wstring();
    const wchar_t *paths[] = {directory.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo = {paths, 1};
    std::error_code ec;
    std::filesystem::create_directories("out/ngx", ec);
    const auto data = std::filesystem::absolute("out/ngx").wstring();
    // Local custom-engine project identity; no NVIDIA application ID is
    // impersonated.
    if (!Check(NVSDK_NGX_VULKAN_Init_with_ProjectID(
                   "58d663b4-dd41-48a5-852f-6f5a67597f5c",
                   NVSDK_NGX_ENGINE_TYPE_CUSTOM, "bbport-experimental",
                   data.c_str(), instance, physical, device,
                   vkGetInstanceProcAddr, vkGetDeviceProcAddr, &info),
               "initialization"))
      return;
    initialized = true;
    if (!Check(NVSDK_NGX_VULKAN_GetCapabilityParameters(&params),
               "capability query"))
      return;
    int supported = 0;
    params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &supported);
    available = supported != 0;
    problem =
        available ? "" : "NGX reports DLSS unavailable on this device/driver";
#endif
  }
  ~Impl() {
#ifdef BB_DLSS_SDK
    if (handle)
      NVSDK_NGX_VULKAN_ReleaseFeature(handle);
    if (params)
      NVSDK_NGX_VULKAN_DestroyParameters(params);
    if (initialized)
      NVSDK_NGX_VULKAN_Shutdown1(device);
#endif
  }
  bool Record(const Frame &f) {
#ifdef BB_DLSS_SDK
    if (!available)
      return false;
    bool created = false;
    if (!handle || width != f.width || height != f.height ||
        ow != f.output.width || oh != f.output.height || preset != f.preset) {
      wait();
      if (handle) {
        NVSDK_NGX_VULKAN_ReleaseFeature(handle);
        handle = nullptr;
      }
      const auto model = f.preset == 4 ? NVSDK_NGX_DLSS_Hint_Render_Preset_L
                                       : NVSDK_NGX_DLSS_Hint_Render_Preset_M;
      for (const char *key :
           {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance})
        params->Set(key, unsigned(model));
      static const NVSDK_NGX_PerfQuality_Value qualities[] = {
          NVSDK_NGX_PerfQuality_Value_DLAA,
          NVSDK_NGX_PerfQuality_Value_MaxQuality,
          NVSDK_NGX_PerfQuality_Value_Balanced,
          NVSDK_NGX_PerfQuality_Value_MaxPerf,
          NVSDK_NGX_PerfQuality_Value_UltraPerformance};
      if (f.preset < 0 || f.preset > 4) {
        problem = "Invalid DLSS quality";
        return false;
      }
      NVSDK_NGX_DLSS_Create_Params create{};
      create.Feature.InWidth = f.width;
      create.Feature.InHeight = f.height;
      create.Feature.InTargetWidth = f.output.width;
      create.Feature.InTargetHeight = f.output.height;
      create.Feature.InPerfQualityValue = qualities[f.preset];
      create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                    NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                    NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
      if (!Check(NGX_VULKAN_CREATE_DLSS_EXT1(device, f.command, 1, 1, &handle,
                                             params, &create),
                 "feature creation"))
        return false;
      width = f.width;
      height = f.height;
      ow = f.output.width;
      oh = f.output.height;
      preset = f.preset;
      created = true;
      std::printf(
          "Upscaler: DLSS 4.5 Super Resolution, model %c, %ux%u -> %ux%u\n",
          model == NVSDK_NGX_DLSS_Hint_Render_Preset_L ? 'L' : 'M', width,
          height, ow, oh);
    }
    auto resource = [](const Image &i, bool output) {
      return NVSDK_NGX_Create_ImageView_Resource_VK(
          i.view, i.image, {i.aspect, 0, 1, 0, 1}, i.format, i.width, i.height,
          output);
    };
    auto color = resource(f.color, false), depth = resource(f.depth, false);
    auto motion = resource(f.motion, false), output = resource(f.output, true);
    // NGX samples inputs in SHADER_READ_ONLY_OPTIMAL; restore the renderer's
    // GENERAL contract.
    auto transition = [&](bool restore) {
      VkImageMemoryBarrier barriers[3]{};
      const Image inputs[] = {f.color, f.depth, f.motion};
      for (unsigned i = 0; i < 3; ++i) {
        auto &b = barriers[i];
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask =
            restore ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask =
            restore ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT
                    : VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = restore ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                              : VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = restore ? VK_IMAGE_LAYOUT_GENERAL
                              : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = inputs[i].image;
        b.subresourceRange = {inputs[i].aspect, 0, 1, 0, 1};
      }
      vkCmdPipelineBarrier(f.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 3, barriers);
    };
    transition(false);
    NVSDK_NGX_VK_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = &color;
    eval.Feature.pInOutput = &output;
    eval.pInDepth = &depth;
    eval.pInMotionVectors = &motion;
    eval.InJitterOffsetX = f.jitter_x;
    eval.InJitterOffsetY = f.jitter_y;
    eval.InRenderSubrectDimensions = {f.width, f.height};
    eval.InReset = f.reset || created;
    eval.InMVScaleX = eval.InMVScaleY = 1.0f;
    eval.InPreExposure = eval.InExposureScale = 1.0f;
    bool ok =
        Check(NGX_VULKAN_EVALUATE_DLSS_EXT(f.command, handle, params, &eval),
              "evaluation");
    transition(true);
    if (ok)
      problem.clear();
    return ok;
#else
    return false;
#endif
  }
};
Upscaler::Upscaler(VkInstance i, VkPhysicalDevice p, VkDevice d,
                   std::function<void()> wait)
    : impl(std::make_unique<Impl>(i, p, d, std::move(wait))) {}
Upscaler::~Upscaler() = default;
bool Upscaler::Available() const { return impl->available; }
bool Upscaler::Record(const Frame &f) { return impl->Record(f); }
const std::string &Upscaler::Problem() const { return impl->problem; }
} // namespace Dlss
