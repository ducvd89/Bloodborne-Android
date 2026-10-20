// SPDX-License-Identifier: GPL-2.0-or-later
// bbport-android: BBSC (Bloodborne seamless co-op, bbsc/) is not in this build (BB_NET_BBSC=OFF): the
// places in the network and NP libraries that report to it (summon placement header, host follow and world
// flag packets) find these no-ops, and nothing of it patches the game.
#include "core/bloodborne_re.h"

namespace Core::Bloodborne {

std::optional<std::string> GetSeamlessHostPlacementHeader() {
    return std::nullopt;
}
bool SetSeamlessHostPlacementHeader(std::string_view) {
    return true;
}
void ClearSeamlessHostPlacementHeader() {}
std::optional<std::uint32_t> GetBbscConnectFirstAreaId() {
    return std::nullopt;
}
void TraceMatching2LeaveRoom(std::uintptr_t, std::uint64_t) {}
void OnBbscHostFollowPacket(std::uint32_t, float, float, float, float, std::uint32_t, std::int32_t) {}
void OnBbscWorldFlagsPacket(const void*, std::uint32_t) {}

} // namespace Core::Bloodborne
