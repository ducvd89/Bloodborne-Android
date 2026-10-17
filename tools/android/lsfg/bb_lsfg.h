// Bloodframe: the C API of libbloodframe.so, Lossless Scaling frame generation for a native Android (Bionic)
// process (android/bb_lsfg.cpp over the framegen backend of lsfg-vk-android). Bloodframe is based on
// lsfg-vk 1.0 by PancakeTAS (MIT) via GameNative's Android fork of it.
//
// A host can dlopen() the library only when frame generation is on, so there is no link-time
// dependency. The library makes its own VkInstance/VkDevice on the same physical device as the host's,
// through the host's own vkGetInstanceProcAddr (the Vulkan driver the process loaded, e.g. a Turnip
// zip), and shares images with the host's device as AHardwareBuffers.
// Nothing here throws; failures return -1 (or NULL) and write a message into `err`.
// One initialized runtime at a time; every call is serialized inside the library.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AHardwareBuffer;

#define BB_LSFG_API __attribute__((visibility("default")))

typedef struct BbLsfgConfig {
    void* vk_get_instance_proc_addr; ///< PFN_vkGetInstanceProcAddr of the host's driver
    const char* dll_path;            ///< the player's Lossless.dll
    uint8_t device_uuid[16];         ///< VkPhysicalDeviceIDProperties of the host's device
    uint8_t driver_uuid[16];
    float flow_scale;                ///< user-visible, 0.25 .. 1.0 (lower: faster, coarser motion)
    int32_t performance;             ///< 1: the light model (LSFG_3_1P), 0: the quality model
    int32_t generation_count;        ///< generated frames per real one: multiplier - 1 (1 .. 3)
    int32_t shared_format;           ///< VkFormat of the shared images (37 RGBA8, 97 RGBA16F)
    int32_t hdr;                     ///< the frames are HDR (the host has SDR frames: 0)
    int32_t fp16;                    ///< 1: the half-precision SPIR-V set of the Lossless.dll when it and the
                                     ///< GPU have it; else (and for 0) its full-precision SPIR-V set; the
                                     ///< DXBC set translated only as the last resort. bb_lsfg_shader_set
                                     ///< says which runs and why.
} BbLsfgConfig;

typedef struct BbLsfgInfo {
    uint64_t input_ahb_usage;  ///< AHARDWAREBUFFER_USAGE_* the input buffers must be allocated with
    uint64_t output_ahb_usage; ///< ... the output buffers
    int32_t sync_fd_ok;        ///< the framegen device exports/imports sync files
    int32_t input_copy;        ///< the framegen device copies the input buffers (not sampled directly)
    int32_t output_copy;       ///< ... copies its result into the output buffers
} BbLsfgInfo;

typedef struct BbLsfgPresent {
    int32_t out_ready_fd[3];  ///< sync file per generated frame (-1: ready, or the host waited)
    int32_t batch_complete_fd; ///< sync file: the framegen device is done with the input buffers
    int32_t gpu_synced;        ///< 1: the fds above are the dependencies; 0: the call waited (host)
} BbLsfgPresent;

/// Starts the runtime: the framegen instance and device, the shaders from the Lossless.dll.
BB_LSFG_API int bb_lsfg_init(const BbLsfgConfig* config, BbLsfgInfo* info, char* err, size_t err_len);

/// Makes a context over two input buffers and `out_count` output buffers (the config's
/// generation_count), all allocated with the usage bb_lsfg_init reported and `format`.
/// Returns the context id (>= 0) or -1.
BB_LSFG_API int bb_lsfg_create_context(struct AHardwareBuffer* in0, struct AHardwareBuffer* in1,
                                       struct AHardwareBuffer* const* outs, uint32_t out_count,
                                       uint32_t width, uint32_t height, int32_t format, char* err,
                                       size_t err_len);

/// Hands the next real frame to the context. `in_sem_fd` is the sync file of the host's copy of the
/// frame into the input buffer (ownership passes to the library; -1: already complete).
/// `count` generated frames (0: only the temporal history advances). The sync files in `out` are
/// owned by the caller. Returns 0 or -1.
BB_LSFG_API int bb_lsfg_present(int ctx, int in_sem_fd, uint32_t count, BbLsfgPresent* out, char* err,
                                size_t err_len);

/// Waits (host) for the context's last present; 0 when it completed, -1 on timeout.
BB_LSFG_API int bb_lsfg_wait(int ctx, uint64_t timeout_ns);

BB_LSFG_API void bb_lsfg_destroy_context(int ctx);
/// Ends the runtime (after the last context).
BB_LSFG_API void bb_lsfg_shutdown(void);

/// The shader set the session runs: BB_LSFG_SET_*. `reason` (when not NULL) gets a short text for the
/// log: why the sets before this one are not used, or what this one is. It can change at the first
/// bb_lsfg_create_context: the shaders are compiled there, and a driver that rejects a set makes the
/// library use the next one for the rest of the process. Returns -1 before bb_lsfg_init.
BB_LSFG_API int bb_lsfg_shader_set(char* reason, size_t len);
#define BB_LSFG_SET_FP16_SPIRV 0 ///< the Lossless.dll's half-precision SPIR-V (needs shaderFloat16)
#define BB_LSFG_SET_FP32_SPIRV 1 ///< the Lossless.dll's full-precision SPIR-V
#define BB_LSFG_SET_FP32_DXBC 2  ///< the dll's DXBC translated to SPIR-V by the library (the last resort)

/// The precision the session runs: 1 half (FP16 SPIR-V), 0 full (either full-precision set); `reason`
/// as for bb_lsfg_shader_set. Kept for hosts that only ask for the precision.
BB_LSFG_API int bb_lsfg_precision(char* reason, size_t len);

/// One line about the backend (driver, AHB transport mode, sync path, capabilities) for the log.
BB_LSFG_API int bb_lsfg_diagnostics(char* buf, size_t len);

#ifdef __cplusplus
}
#endif
