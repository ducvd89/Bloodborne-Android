// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: fixes to the game's own code (play time at high frame rates, hit collector cap, save
// character id guard), from upstream bloodborne_pc 0.7 (the SrShadowy fork's engine fixes).
// BB_FIX_PLAY_TIME / BB_FIX_HIT_COLLECTOR / BB_FIX_CHARA_ID=0 turn one off.
#pragma once

#include <cstdint>

namespace BbEngineFixes {
/// Installs the hooks (at load, before the game runs: bbgpu_patch_image), as native hook slots only: a
/// libbbaot.so built before these sites were in the recompiler's catalog has no slot there, and the
/// game then runs as it did (one line says so).
void PatchImage(unsigned char* image, std::uint64_t size);
} // namespace BbEngineFixes
