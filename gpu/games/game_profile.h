// SPDX-License-Identifier: GPL-2.0-or-later
// Game profiles: what the translator knows about one game, kept out of its core. The core (the
// command processor, the caches, the shader recompiler) runs any game on its generic paths and
// asks the active profile only for the rest: hooks in the game's own code that announce its
// writes earlier than a write trap would, guest code known to write per-frame data, shaders with
// a known purpose. A game without a profile runs on the generic paths alone. One file per game
// (games/<game>.cpp) defines its profile; game_profile.cpp lists them.
#pragma once
#include <cstdint>
#include <span>

namespace Game {

/// Guest addresses [begin, end).
struct CodeRange {
    std::uint64_t begin = 0, end = 0;
};

/// What the core offers a profile's hooks.
struct CoreServices {
    /// The game handed out GPU memory [address, address + size): what the GPU had there is dead.
    void (*fresh_range)(std::uint64_t address, std::uint64_t size) = nullptr;
};

struct Profile {
    const char* name = nullptr;
    /// The param.sfo TITLE_IDs it is for.
    std::span<const char* const> serials;
    /// Hooks in the game's code (each checks the bytes it replaces: another build of the game is
    /// left alone). They make the core learn of writes sooner; it does not depend on them for
    /// what it shows (write traps catch what nobody announces).
    void (*install_hooks)(const CoreServices& services) = nullptr;
    /// Guest code known to write per-frame data into GPU memory: the blocks it writes stay in
    /// the game's memory from its first write instead of after a few frames.
    std::span<const CodeRange> dynamic_writers;
    /// A compute shader (GCN program hash) that only copies buffers, into memory the translator
    /// reads on the CPU: its stores go through to the game's memory; 0 = none known.
    std::uint64_t buffer_copy_shader = 0;
};

/// Chooses the profile for the game by its TITLE_ID (bbgpu_init). BB_GAME_PROFILE=none runs the
/// game as an unknown one (no profile), for checking the generic paths.
void Select(const char* serial);

/// The active profile, or nullptr (an unknown game, or BB_GAME_PROFILE=none).
const Profile* Active();

/// The active profile's buffer copy shader, or 0.
inline std::uint64_t BufferCopyShader() {
    const Profile* profile = Active();
    return profile ? profile->buffer_copy_shader : 0;
}

/// Whether guest_rip is in code the active profile knows to write per-frame data.
bool IsDynamicWriter(std::uint64_t guest_rip);

} // namespace Game
