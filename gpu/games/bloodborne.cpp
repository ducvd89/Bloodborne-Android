// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne (update 1.09): what the translator knows about its code. Addresses are those of the
// 1.09 executable (game_check.py makes sure that is the one running); every retail release runs
// the same one.
#include "game_profile.h"
#include "bbport_guest_hooks.h"

namespace Game {
namespace {
constexpr std::uint64_t ImageBase = 0x800000000ull;

/// The retail releases (scripts/game_check.py SUPPORTED_TITLES): Bloodborne in the US, EU, UK, JP
/// and Asia, then the editions with The Old Hunters (US, EU, JP, Asia). Without its serial here a
/// region ran on the generic paths alone, and the new memory model crashed on entering the world
/// (issue #119, the US release: a null read in the allocator, 0x2085xxx). Its bookkeeping is in
/// GPU-mapped pages; DynamicWriters below keeps them in place from the first write (the hooks
/// alone do not matter for it: BB_GUEST_HOOKS=0 runs; BB_GAME_PROFILE=none crashes).
constexpr const char* Serials[] = {"CUSA00900", "CUSA00207", "CUSA00208", "CUSA00299", "CUSA01363",
                                   "CUSA03179", "CUSA03173", "CUSA03014", "CUSA03023"};

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
