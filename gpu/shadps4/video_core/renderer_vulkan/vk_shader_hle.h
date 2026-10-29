// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <vector>

#include "common/types.h"

namespace AmdGpu {
struct ComputeProgram;
union Regs;
} // namespace AmdGpu

namespace Shader {
struct Info;
}

namespace Vulkan {

class Rasterizer;

/// Attempts to execute a shader using HLE if possible. `copy_control`: for the game's copy shader,
/// the control list entries (12 bytes each, one per work group) to run instead of the bound list
/// (the copies that change something, CopyFilter); empty: the bound list.
bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer,
                      std::span<const u8> copy_control = {});

/// Whether the game's copy shader runs as HLE (copies of our own) rather than as the translated
/// shader: the case in copy mode (BB_UMA, no BB_GUEST_IN_PLACE), BB_COPY_SHADER_NATIVE=0.
bool CopyShaderIsHle();

/// One copy of the game's copy shader: `size` bytes from guest `src` to guest `dst`, and its
/// control list entry (dst index, src index, last element).
struct CopyEntry {
    u64 src, dst, size;
    u32 control[3];
};
/// The copies its control list names for `groups` work groups, unmerged, in list order. False
/// (nothing added) for another shader, an unexpected shape or a list reaching past its buffers.
bool CopyShaderEntries(const Shader::Info& info, u32 groups, std::vector<CopyEntry>& out);

} // namespace Vulkan
