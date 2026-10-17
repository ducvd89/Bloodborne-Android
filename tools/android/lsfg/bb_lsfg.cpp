// Bloodframe: C API over the framegen backend of GameNative's lsfg-vk fork (bb_lsfg.h).
// Bloodframe is based on lsfg-vk 1.0 by PancakeTAS (MIT) via GameNative's Android fork of it.
//
// This is the part of the fork's Vulkan layer (src/context.cpp) that does not need to be a layer: it
// starts the framegen runtime (its own VkInstance/VkDevice, made through the game's vkGetInstanceProcAddr),
// reads the shaders from the player's Lossless.dll (src/extract), creates a context over
// AHardwareBuffers and runs one present per real frame. What the game does around it (copy the frame
// into the input buffer, ownership transfers, showing the outputs) is libbbgpu's
// (vk_lsfg.cpp); the layer's adaptive scheduler, Adreno workarounds and config file are not needed.
#include "bb_lsfg.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

#include "bb_half.hpp"
#include "extract/extract.hpp"
#include "extract/trans.hpp"
#include "lsfg_3_1.hpp"
#include "lsfg_3_1p.hpp"
#include "lsfg_backend.hpp"

namespace {

std::mutex g_mutex;
bool g_initialized = false;
bool g_performance = false;
BbLsfgConfig g_config{};
std::string g_dll_path;
LSFG::BackendDiagnostics g_diag{};
// Precision: what the host asked for, what the session runs and why (one reason for the log).
bool g_fp16_requested = false;
bool g_fp16 = false;
std::string g_fp16_reason;
// A driver that cannot do half precision cannot later in this process either: said once, kept.
std::string g_fp16_driver_failure;

void set_error(char* err, size_t len, const std::string& message) {
    __android_log_print(ANDROID_LOG_WARN, "bb_lsfg", "%s", message.c_str());
    if (err != nullptr && len > 0) {
        std::snprintf(err, len, "%s", message.c_str());
    }
}

LSFG::DeviceIdentity identity_of(const BbLsfgConfig& config) {
    LSFG::DeviceIdentity identity{};
    std::copy_n(config.device_uuid, VK_UUID_SIZE, identity.deviceUUID.begin());
    std::copy_n(config.driver_uuid, VK_UUID_SIZE, identity.driverUUID.begin());
    return identity;
}

// Whether the session can run the half-precision shaders: asked for, and the Lossless.dll has the set.
// The GPU's side (shaderFloat16) is known once the device exists: start_runtime().
void choose_precision() {
    g_fp16 = false;
    if (!g_fp16_requested) {
        g_fp16_reason = "turned off in the settings";
        return;
    }
    if (!g_fp16_driver_failure.empty()) {
        g_fp16_reason = g_fp16_driver_failure;
        return;
    }
    const std::string why = BbHalf::Prepare();
    if (!why.empty()) {
        g_fp16_reason = why;
        return;
    }
    g_fp16 = true;
    g_fp16_reason = "half-precision shaders of the Lossless.dll";
}

// Full precision for the rest of the process, said once (the host logs the reason it gets from
// bb_lsfg_precision; this is the library's own line).
void fall_back_to_fp32(const std::string& why) {
    g_fp16 = false;
    g_fp16_reason = why;
    g_fp16_driver_failure = why;
    __android_log_print(ANDROID_LOG_WARN, "bb_lsfg", "Bloodframe: FP16 off (%s)", why.c_str());
}

void initialize_runtime() {
    // The shaders: half precision is SPIR-V as the dll has it, full precision is the DXBC translated.
    const auto loader = [](const std::string& name) {
        return g_fp16 ? BbHalf::Get(name) : Extract::translateShader(Extract::getShader(name));
    };
    const auto identity = identity_of(g_config);
    const auto format = static_cast<VkFormat>(g_config.shared_format);
    const bool hdr = g_config.hdr != 0;
    // The framegen takes the flow scale as the factor the images are divided by.
    const float inverse_scale = 1.0F / g_config.flow_scale;
    const auto count = static_cast<uint64_t>(g_config.generation_count);
    // The device enables shaderFloat16 only when the half-precision shaders are going to use it.
    LSFG::setShaderFloat16Requested(g_fp16);
    if (g_performance) {
        LSFG_3_1P::initialize(identity, format, hdr, inverse_scale, count, loader);
        g_diag = LSFG_3_1P::getBackendDiagnostics();
    } else {
        LSFG_3_1::initialize(identity, format, hdr, inverse_scale, count, loader);
        g_diag = LSFG_3_1::getBackendDiagnostics();
    }
}

void finalize_runtime() {
    if (g_performance) {
        LSFG_3_1P::finalize();
    } else {
        LSFG_3_1::finalize();
    }
}

// (Re)starts the framegen runtime. A no-op while it runs with the same settings; after the last
// context was deleted the runtime is gone and this builds it again (instance, device, pools).
void start_runtime() {
    initialize_runtime();
    if (g_fp16 && !g_diag.supportDecision.fp16) {
        // The shaders are chosen before the device exists: this GPU or driver has no shaderFloat16, so
        // the same runtime again with the full-precision set (the shader pool keeps what it loaded).
        finalize_runtime();
        fall_back_to_fp32("the GPU driver does not support shaderFloat16");
        initialize_runtime();
    }
}

uint64_t ahb_usage(bool output) {
    uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    // The framegen writes the outputs with a compute shader (storage) in these transport modes.
    if (LSFG::ahbStorageUsageRequired(g_diag.ahbTransportMode,
                                      output ? LSFG::AhbImageRole::Output : LSFG::AhbImageRole::Input)) {
        usage |= AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
    }
    return usage;
}

} // namespace

extern "C" {

int bb_lsfg_init(const BbLsfgConfig* config, BbLsfgInfo* info, char* err, size_t err_len) {
    std::scoped_lock lock{g_mutex};
    try {
        if (config == nullptr || config->vk_get_instance_proc_addr == nullptr ||
            config->dll_path == nullptr || config->generation_count < 1 ||
            config->generation_count > 3 || !(config->flow_scale >= 0.25F && config->flow_scale <= 1.0F)) {
            set_error(err, err_len, "bb_lsfg_init: invalid configuration");
            return -1;
        }
        if (g_initialized) {
            set_error(err, err_len, "bb_lsfg_init: already initialized");
            return -1;
        }
        g_config = *config;
        g_dll_path = config->dll_path;
        g_config.dll_path = g_dll_path.c_str();
        g_performance = config->performance != 0;
        g_fp16_requested = config->fp16 != 0;
        LSFG::setInstanceProcAddr(reinterpret_cast<PFN_vkGetInstanceProcAddr>(config->vk_get_instance_proc_addr));
        Extract::setDllPath(g_dll_path);
        // The shaders first: a missing or old Lossless.dll is the likeliest failure and costs no GPU
        // work to find out.
        Extract::extractShaders();
        choose_precision();
        start_runtime();
        if (g_diag.ahbTransportMode == LSFG::AhbTransportMode::Unsupported) {
            const std::string reason = g_diag.supportDecision.rejectionReason.empty()
                                           ? "the Vulkan driver supports no AHardwareBuffer transport for the frame format"
                                           : g_diag.supportDecision.rejectionReason;
            finalize_runtime();
            set_error(err, err_len, reason);
            return -1;
        }
        if (info != nullptr) {
            *info = {};
            info->input_ahb_usage = ahb_usage(false);
            info->output_ahb_usage = ahb_usage(true);
            info->sync_fd_ok = g_diag.externalSemaphoreSyncFd ? 1 : 0;
            info->input_copy = LSFG::ahbInputCopyRequired(g_diag.ahbTransportMode) ? 1 : 0;
            info->output_copy = LSFG::ahbOutputCopyRequired(g_diag.ahbTransportMode) ? 1 : 0;
        }
        g_initialized = true;
        return 0;
    } catch (const std::exception& e) {
        set_error(err, err_len, e.what());
    } catch (...) {
        set_error(err, err_len, "unknown error");
    }
    return -1;
}

int bb_lsfg_create_context(struct AHardwareBuffer* in0, struct AHardwareBuffer* in1,
                           struct AHardwareBuffer* const* outs, uint32_t out_count, uint32_t width,
                           uint32_t height, int32_t format, char* err, size_t err_len) {
    std::scoped_lock lock{g_mutex};
    try {
        if (!g_initialized) {
            set_error(err, err_len, "bb_lsfg_create_context: not initialized");
            return -1;
        }
        if (in0 == nullptr || in1 == nullptr || outs == nullptr ||
            out_count != static_cast<uint32_t>(g_config.generation_count) || format != g_config.shared_format) {
            set_error(err, err_len, "bb_lsfg_create_context: invalid buffers");
            return -1;
        }
        // After the last context was deleted the runtime was torn down: start it again.
        start_runtime();
        const std::vector<AHardwareBuffer*> outputs(outs, outs + out_count);
        const VkExtent2D extent{width, height};
        const auto vk_format = static_cast<VkFormat>(format);
        const auto make = [&]() {
            return g_performance ? LSFG_3_1P::createContextFromAHB(in0, in1, outputs, extent, vk_format)
                                 : LSFG_3_1::createContextFromAHB(in0, in1, outputs, extent, vk_format);
        };
        if (!g_fp16) {
            return make();
        }
        try {
            return make();
        } catch (const std::exception& e) {
            // The shaders are compiled with the context: the driver may refuse a half-precision one.
            // Full precision from here on.
            fall_back_to_fp32(std::string("the context failed to build with half-precision shaders: ") + e.what());
        }
        finalize_runtime();
        start_runtime();
        return make();
    } catch (const std::exception& e) {
        set_error(err, err_len, e.what());
    } catch (...) {
        set_error(err, err_len, "unknown error");
    }
    return -1;
}

int bb_lsfg_present(int ctx, int in_sem_fd, uint32_t count, BbLsfgPresent* out, char* err, size_t err_len) {
    std::scoped_lock lock{g_mutex};
    if (out != nullptr) {
        *out = {};
        out->out_ready_fd[0] = out->out_ready_fd[1] = out->out_ready_fd[2] = -1;
        out->batch_complete_fd = -1;
    }
    try {
        if (!g_initialized || out == nullptr || count > static_cast<uint32_t>(g_config.generation_count)) {
            set_error(err, err_len, "bb_lsfg_present: invalid call");
            return -1;
        }
        // The input semaphore is a sync file (or -1, already signaled); the framegen imports it.
        constexpr auto handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        const LSFG::AndroidFrameSyncFds sync =
            g_performance ? LSFG_3_1P::presentContextWithCountExportSyncFd(ctx, in_sem_fd, count, handle_type)
                          : LSFG_3_1::presentContextWithCountExportSyncFd(ctx, in_sem_fd, count, handle_type);
        out->gpu_synced = sync.gpuDependenciesExported ? 1 : 0;
        for (size_t i = 0; i < sync.outputReadyFds.size() && i < 3; ++i) {
            out->out_ready_fd[i] = sync.outputReadyFds[i];
        }
        out->batch_complete_fd = sync.batchCompleteFd;
        return 0;
    } catch (const std::exception& e) {
        set_error(err, err_len, e.what());
    } catch (...) {
        set_error(err, err_len, "unknown error");
    }
    return -1;
}

int bb_lsfg_wait(int ctx, uint64_t timeout_ns) {
    std::scoped_lock lock{g_mutex};
    try {
        if (!g_initialized) {
            return -1;
        }
        const bool done = g_performance ? LSFG_3_1P::waitContext(ctx, timeout_ns)
                                        : LSFG_3_1::waitContext(ctx, timeout_ns);
        return done ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

void bb_lsfg_destroy_context(int ctx) {
    std::scoped_lock lock{g_mutex};
    try {
        if (!g_initialized) {
            return;
        }
        if (g_performance) {
            LSFG_3_1P::deleteContext(ctx);
        } else {
            LSFG_3_1::deleteContext(ctx);
        }
    } catch (...) {
    }
}

void bb_lsfg_shutdown(void) {
    std::scoped_lock lock{g_mutex};
    try {
        if (g_initialized) {
            finalize_runtime();
        }
    } catch (...) {
    }
    g_initialized = false;
}

int bb_lsfg_precision(char* reason, size_t len) {
    std::scoped_lock lock{g_mutex};
    if (reason != nullptr && len > 0) {
        std::snprintf(reason, len, "%s", g_fp16_reason.c_str());
    }
    return g_initialized && g_fp16 ? 1 : 0;
}

int bb_lsfg_diagnostics(char* buf, size_t len) {
    std::scoped_lock lock{g_mutex};
    if (buf == nullptr || len == 0) {
        return -1;
    }
    if (!g_initialized) {
        std::snprintf(buf, len, "not initialized");
        return -1;
    }
    const auto& d = g_diag;
    std::snprintf(buf, len,
                  "model=%s driver=\"%s\" (id %u, %s) api=%u.%u.%u ahb_transport=%s "
                  "input_copy=%d output_copy=%d vulkan_path=%s sync_path=%s spirv=0x%x sync_fd=%d "
                  "opaque_fd=%d precision=%s null_descriptor=%d",
                  g_performance ? "performance" : "quality", d.driverName.c_str(),
                  static_cast<unsigned>(d.driverId), d.driverInfo.c_str(), VK_API_VERSION_MAJOR(d.apiVersion),
                  VK_API_VERSION_MINOR(d.apiVersion), VK_API_VERSION_PATCH(d.apiVersion),
                  LSFG::ahbTransportModeName(d.ahbTransportMode),
                  LSFG::ahbInputCopyRequired(d.ahbTransportMode) ? 1 : 0,
                  LSFG::ahbOutputCopyRequired(d.ahbTransportMode) ? 1 : 0, d.vulkanPath.c_str(),
                  d.synchronizationPath.c_str(), d.spirvTargetVersion, d.externalSemaphoreSyncFd ? 1 : 0,
                  d.externalSemaphoreOpaqueFd ? 1 : 0, g_fp16 ? "fp16" : "fp32",
                  d.supportDecision.nullDescriptor ? 1 : 0);
    return 0;
}

} // extern "C"
