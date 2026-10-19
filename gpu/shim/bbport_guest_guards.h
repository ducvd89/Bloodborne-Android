// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: guards in the game's code against its own failure paths, from upstream bloodborne_pc 0.6
// (bbport_guest_guards.cpp): the blur-kernel sample count (#207), the GX per-frame command node
// (#162) and the effect targets' initialization on the main thread (Bad PM4 at save load).
#pragma once

#include <cstdint>

namespace BbGuestGuards {
/// Installs the guards (at load, before the game runs: bbgpu_patch_image). Each site is checked
/// first; one that is not the expected code is left alone (logged).
void PatchImage(unsigned char* image, std::uint64_t size);
/// How often they acted, for the runtime's report at exit.
void Report();
} // namespace BbGuestGuards
