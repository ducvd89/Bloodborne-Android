// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: which part of the port wrote the game's code where. The runtime (src/runtime_patches.c) keeps the
// notes; a game module whose own hook finds other bytes than it expects (BBSC, gpu/bbnet20) asks who changed
// them. Weak: absent in a test or a build without the runtime, then nothing is noted.
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" void runtime_code_patch_note(std::uintptr_t address, std::size_t size, const char* owner)
    __attribute__((weak));

/// The port is about to write [address, address + size) of the game's code (`owner` is a string with static
/// storage: what patches there).
inline void BbPatchNote(std::uintptr_t address, std::size_t size, const char* owner) {
    if (runtime_code_patch_note) {
        runtime_code_patch_note(address, size, owner);
    }
}
