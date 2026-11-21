/* Minimal libSceJpegDec HLE for games that use the PS4 JPEG decoder.
 * Image decoding is provided by stb_image (public domain/MIT), built into this translation unit
 * so the Android runtime has no additional shared-library dependency.
 */
#define _GNU_SOURCE
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
/* stb_image carries unused parameters in some helpers; the Android NDK build uses
 * -Werror, so silence that diagnostic around the vendored header only. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#include "../third_party/stb/stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "runtime.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JPEG_INVALID_ADDR ((int32_t)0x80650001)
#define JPEG_INVALID_SIZE ((int32_t)0x80650002)
#define JPEG_INVALID_PARAM ((int32_t)0x80650003)
#define JPEG_INVALID_HANDLE ((int32_t)0x80650004)
#define JPEG_INVALID_WORK_MEMORY ((int32_t)0x80650005)
#define JPEG_EXCEED_MAX_WIDTH ((int32_t)0x80650006)
#define JPEG_INVALID_DATA ((int32_t)0x80650010)
#define JPEG_DECODE_ERROR ((int32_t)0x80650012)
#define JPEG_OK 0

typedef struct {
    uint32_t size;
    uint32_t decode_mode;
    uint32_t max_width;
} JpegCreateParam;

typedef struct {
    const void *jpeg_addr;
    uint32_t jpeg_size;
    uint16_t decode_mode;
    uint16_t downscale_ratio;
} JpegParseParam;

typedef struct {
    uint32_t image_width;
    uint32_t image_height;
    uint16_t color_space;
    uint16_t num_components;
    uint8_t component_id[4];
    uint8_t sampling_factor[4];
    uint32_t decode_mode;
    uint32_t required_decode_mode;
    uint32_t output_width;
    uint32_t output_height;
} JpegImageInfo;

typedef struct {
    const void *jpeg_addr;
    void *image_addr;
    void *aux_addr;
    uint32_t jpeg_size;
    uint32_t image_size;
    uint32_t aux_size;
    uint16_t decode_mode;
    uint16_t downscale_ratio;
    uint16_t pixel_format;
    uint16_t alpha_value;
    uint32_t image_pitch;
} JpegDecodeParam;

typedef struct {
    void *self;
    uint32_t handle_size;
    uint32_t decode_mode;
    uint32_t max_width;
    uint32_t stride_or_aux;
} JpegHandle;

_Static_assert(sizeof(JpegCreateParam) == 12, "JpegCreateParam ABI");
_Static_assert(sizeof(JpegParseParam) == 16, "JpegParseParam ABI");
_Static_assert(sizeof(JpegImageInfo) == 36, "JpegImageInfo ABI");
_Static_assert(sizeof(JpegDecodeParam) == 48, "JpegDecodeParam ABI");
_Static_assert(sizeof(JpegHandle) == 24, "JpegHandle ABI");

static int ratio_valid(uint16_t ratio) {
    return ratio == 1 || ratio == 2 || ratio == 4 || ratio == 8;
}

static int parse_sof(const uint8_t *data, uint32_t size, JpegImageInfo *info) {
    if (!data || size < 4 || data[0] != 0xff || data[1] != 0xd8) return JPEG_INVALID_DATA;
    uint32_t pos = 2;
    while (pos < size) {
        while (pos < size && data[pos] != 0xff) ++pos;
        while (pos < size && data[pos] == 0xff) ++pos;
        if (pos >= size) break;
        const uint8_t marker = data[pos++];
        if (marker == 0xd9 || marker == 0xda) break;
        if (marker == 0x00 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (pos + 2 > size) return JPEG_INVALID_DATA;
        const uint16_t seg_size = (uint16_t)((data[pos] << 8) | data[pos + 1]);
        if (seg_size < 2 || seg_size > size - pos) return JPEG_INVALID_DATA;
        if ((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
            (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf)) {
            if (seg_size < 8) return JPEG_INVALID_DATA;
            const uint8_t *sof = data + pos;
            const uint16_t height = (uint16_t)((sof[3] << 8) | sof[4]);
            const uint16_t width = (uint16_t)((sof[5] << 8) | sof[6]);
            const uint8_t components = sof[7];
            if (!width || !height || !components || components > 4 || seg_size < 8u + 3u * components)
                return JPEG_INVALID_DATA;
            memset(info, 0, sizeof(*info));
            info->image_width = width;
            info->image_height = height;
            info->num_components = components;
            for (uint8_t i = 0; i < components; ++i) {
                info->component_id[i] = sof[8 + 3 * i];
                info->sampling_factor[i] = sof[9 + 3 * i];
            }
            info->color_space = components == 1 ? 2 : components == 3 ? 1 : 0;
            return JPEG_OK;
        }
        pos += seg_size;
    }
    return JPEG_DECODE_ERROR;
}

static uint32_t required_mode(const JpegImageInfo *info) {
    if (info->color_space == 2) return 1;
    if (info->color_space != 1) return 0;
    uint32_t max_h = 1, density = 0;
    for (uint16_t i = 0; i < info->num_components; ++i) {
        const uint8_t h = info->sampling_factor[i] >> 4;
        const uint8_t v = info->sampling_factor[i] & 0xf;
        if (h > max_h) max_h = h;
        if (i < 3) density += (uint32_t)h * v;
    }
    const uint32_t scale = max_h ? 96 / (max_h * 8) : 12;
    return density * scale < 0x25 ? 1 : 2;
}

static void finish_info(JpegImageInfo *info, uint32_t mode, uint16_t ratio) {
    info->decode_mode = mode;
    info->required_decode_mode = required_mode(info);
    info->output_width = (info->image_width + ratio - 1) / ratio;
    info->output_height = (info->image_height + ratio - 1) / ratio;
}

static ABI int32_t jpeg_query_memory_size(const JpegCreateParam *param) {
    if (!param) return JPEG_INVALID_ADDR;
    if (param->size != sizeof(*param)) return JPEG_INVALID_SIZE;
    if (param->decode_mode < 1 || param->decode_mode > 2 || !param->max_width || param->max_width > 0xffff)
        return JPEG_INVALID_PARAM;
    uint32_t aligned = (param->max_width + 0x1f) & ~0x1f;
    aligned |= 0x10;
    const uint64_t size = (param->decode_mode == 2 ? (uint64_t)(aligned << 5) * 5 : (uint64_t)(aligned << 4) * 3)
                        + (uint64_t)aligned * 6 + 0x7360;
    return size > INT32_MAX ? JPEG_INVALID_SIZE : (int32_t)size;
}

static ABI int32_t jpeg_create(const JpegCreateParam *param, void *memory, uint32_t memory_size,
                               JpegHandle **out_handle) {
    if (!param || !memory || !out_handle) return JPEG_INVALID_ADDR;
    const int32_t required = jpeg_query_memory_size(param);
    if (required < 0) return required;
    if (memory_size < (uint32_t)required) return JPEG_INVALID_SIZE;
    const uintptr_t raw = (uintptr_t)memory;
    if (raw > UINTPTR_MAX - 31) return JPEG_INVALID_WORK_MEMORY;
    const uintptr_t aligned = (raw + 31) & ~(uintptr_t)31;
    if (aligned - raw > memory_size || sizeof(JpegHandle) > memory_size - (aligned - raw))
        return JPEG_INVALID_WORK_MEMORY;
    JpegHandle *handle = (JpegHandle *)aligned;
    handle->self = handle;
    handle->handle_size = sizeof(*param);
    handle->decode_mode = param->decode_mode;
    handle->max_width = (param->max_width + 0x1f) & ~0x1f;
    handle->stride_or_aux = 0;
    *out_handle = handle;
    return JPEG_OK;
}

static ABI int32_t jpeg_delete(JpegHandle *handle) {
    if (!handle || ((uintptr_t)handle & 31) || handle->self != handle) return JPEG_INVALID_HANDLE;
    handle->self = NULL;
    return JPEG_OK;
}

static ABI int32_t jpeg_parse_header(const JpegParseParam *param, JpegImageInfo *image_info) {
    if (!param || !param->jpeg_addr || !image_info) return JPEG_INVALID_ADDR;
    if (!param->jpeg_size || param->jpeg_size > INT_MAX) return JPEG_INVALID_SIZE;
    if (param->decode_mode > 1 || !ratio_valid(param->downscale_ratio)) return JPEG_INVALID_PARAM;
    JpegImageInfo result;
    const int32_t status = parse_sof((const uint8_t *)param->jpeg_addr, param->jpeg_size, &result);
    if (status) return status;
    finish_info(&result, param->decode_mode, param->downscale_ratio);
    *image_info = result;
    return JPEG_OK;
}

static uint8_t clamp_byte(int value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

static ABI int32_t jpeg_decode(JpegHandle *handle, const JpegDecodeParam *param, JpegImageInfo *image_info) {
    if (!handle || ((uintptr_t)handle & 31) || handle->self != handle) return JPEG_INVALID_HANDLE;
    if (!param || !param->jpeg_addr || !param->image_addr) return JPEG_INVALID_ADDR;
    if (!param->jpeg_size || !param->image_size || param->jpeg_size > INT_MAX) return JPEG_INVALID_SIZE;
    if (param->decode_mode > 1 || !ratio_valid(param->downscale_ratio) ||
        (param->pixel_format != 0 && param->pixel_format != 1 && param->pixel_format != 10 && param->pixel_format != 11) ||
        param->alpha_value > 255) return JPEG_INVALID_PARAM;

    int width = 0, height = 0, source_channels = 0;
    stbi_uc *decoded = stbi_load_from_memory((const stbi_uc *)param->jpeg_addr, (int)param->jpeg_size,
                                             &width, &height, &source_channels, 4);
    if (!decoded) return JPEG_DECODE_ERROR;
    if (width <= 0 || height <= 0 || (uint32_t)width > handle->max_width) {
        stbi_image_free(decoded);
        return JPEG_EXCEED_MAX_WIDTH;
    }
    const uint32_t ratio = param->downscale_ratio;
    const uint32_t out_w = ((uint32_t)width + ratio - 1) / ratio;
    const uint32_t out_h = ((uint32_t)height + ratio - 1) / ratio;
    const uint64_t min_pitch = param->pixel_format == 10
        ? ((uint64_t)(out_w + 1) / 2) * 4
        : (uint64_t)out_w * (param->pixel_format == 11 ? 1 : 4);
    const uint32_t pitch = param->image_pitch ? param->image_pitch : (uint32_t)min_pitch;
    if (pitch < min_pitch || (param->pixel_format != 11 && (pitch & 3)) ||
        (uint64_t)pitch * out_h > param->image_size) {
        stbi_image_free(decoded);
        return JPEG_INVALID_SIZE;
    }

    uint8_t *dst = (uint8_t *)param->image_addr;
    for (uint32_t y = 0; y < out_h; ++y) {
        uint8_t *row = dst + (size_t)y * pitch;
        const uint32_t sy = y * ratio;
        for (uint32_t x = 0; x < out_w; ++x) {
            const uint32_t sx = x * ratio;
            const stbi_uc *src = decoded + ((size_t)sy * width + sx) * 4;
            if (param->pixel_format == 0 || param->pixel_format == 1) {
                uint8_t *pixel = row + (size_t)x * 4;
                pixel[0] = src[param->pixel_format == 1 ? 2 : 0];
                pixel[1] = src[1];
                pixel[2] = src[param->pixel_format == 1 ? 0 : 2];
                pixel[3] = param->alpha_value ? (uint8_t)param->alpha_value : src[3];
            } else if (param->pixel_format == 11) {
                row[x] = (uint8_t)((src[0] * 77u + src[1] * 150u + src[2] * 29u) >> 8);
            } else {
                /* Y8U8Y8V8: packed 4:2:2 pairs (Y0, U, Y1, V). */
                const stbi_uc *pair = decoded + ((size_t)sy * width + (sx & ~1u)) * 4;
                const int r0 = pair[0], g0 = pair[1], b0 = pair[2];
                const stbi_uc *second = (sx + 1 < (uint32_t)width) ? pair + 4 : pair;
                const int r1 = second[0], g1 = second[1], b1 = second[2];
                const int yy = (sx & 1) ? (r1 * 77 + g1 * 150 + b1 * 29) >> 8 : (r0 * 77 + g0 * 150 + b0 * 29) >> 8;
                const int u = ((-43 * r0 - 85 * g0 + 128 * b0) >> 8) + 128;
                const int v = ((128 * r0 - 107 * g0 - 21 * b0) >> 8) + 128;
                const size_t base = (size_t)(x & ~1u) * 2;
                row[base + (x & 1 ? 2 : 0)] = clamp_byte(yy);
                if (!(x & 1)) { row[base + 1] = clamp_byte(u); row[base + 3] = clamp_byte(v); }
            }
        }
    }
    stbi_image_free(decoded);

    if (image_info) {
        JpegImageInfo result;
        memset(&result, 0, sizeof(result));
        result.image_width = (uint32_t)width;
        result.image_height = (uint32_t)height;
        result.color_space = source_channels == 1 ? 2 : 1;
        result.num_components = (uint16_t)source_channels;
        result.component_id[0] = 1; result.component_id[1] = 2; result.component_id[2] = 3;
        result.sampling_factor[0] = 0x22; result.sampling_factor[1] = 0x11; result.sampling_factor[2] = 0x11;
        finish_info(&result, param->decode_mode, param->downscale_ratio);
        *image_info = result;
    }
    return (int32_t)((out_w << 16) | (out_h & 0xffff));
}

static ABI int32_t jpeg_decode_input_control(JpegHandle *handle, const JpegDecodeParam *param,
                                              JpegImageInfo *image_info) {
    return jpeg_decode(handle, param, image_info);
}

uintptr_t runtime_jpeg_resolve(const char *name) {
    static const RuntimeExport exports[] = {
        {"sceJpegDecQueryMemorySize", jpeg_query_memory_size},
        {"sceJpegDecCreate", jpeg_create},
        {"sceJpegDecDelete", jpeg_delete},
        {"sceJpegDecParseHeader", jpeg_parse_header},
        {"sceJpegDecDecode", jpeg_decode},
        {"sceJpegDecDecodeWithInputControl", jpeg_decode_input_control},
    };
    return RUNTIME_LOOKUP(exports, name);
}
