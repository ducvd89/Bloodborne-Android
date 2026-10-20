// bbport online module: what BBSC (bloodborne_re.cpp, bbsc_persistence.cpp) reads of shadPS4's
// memory patcher: where the game's image is and which game it is. Set by bbnet_init (bbnet.h).
#pragma once
#include <cstdint>
#include <string>

namespace MemoryPatcher {
inline std::uint64_t g_eboot_address = 0;
inline std::uint64_t g_eboot_image_size = 0;
inline std::string g_game_serial;
} // namespace MemoryPatcher
