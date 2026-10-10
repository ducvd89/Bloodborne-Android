// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne (CUSA03173, update 1.09): what the translator knows about its code. Addresses are
// those of the 1.09 executable (game_check.py makes sure that is the one running).
#include "game_profile.h"
#include "bbport_guest_hooks.h"

namespace Game {
namespace {
constexpr std::uint64_t ImageBase = 0x800000000ull;

constexpr const char* Serials[] = {"CUSA03173"};

/// Reverse engineering: 0x2ab7350 computes vertices on the CPU (cloth) into a ~128 MiB ring, each
/// block written now and then; 0x20858a0 is the game's allocator, its bookkeeping in GPU-mapped
/// pages.
constexpr CodeRange DynamicWriters[] = {
    {ImageBase + 0x2ab7350, ImageBase + 0x2aba310},
    {ImageBase + 0x20858a0, ImageBase + 0x2085e20},
};

void InstallHooks(const CoreServices& services) {
    // The resource loaders' copies, the GPU memory range allocator and the heap (bbport_guest_hooks).
    BbGuestHooks::Install(services.fresh_range);
}
} // namespace

extern const Profile BloodborneProfile = {
    .name = "Bloodborne",
    .serials = Serials,
    .install_hooks = InstallHooks,
    .dynamic_writers = DynamicWriters,
    // The game's buffer-copy compute shader (not the export-stage copy shader).
    .buffer_copy_shader = 0xfefebf9f,
};

} // namespace Game
