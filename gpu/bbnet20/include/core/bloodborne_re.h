// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Core::Bloodborne {

std::optional<std::string> GetSeamlessHostPlacementHeader();
bool SetSeamlessHostPlacementHeader(std::string_view value);
void ClearSeamlessHostPlacementHeader();
// BBSC connect-first cross-map summon: the host map a pending sign should advertise, if any.
std::optional<std::uint32_t> GetBbscConnectFirstAreaId();
void TraceMatching2LeaveRoom(std::uintptr_t return_address, std::uint64_t room_id);
// BBSC warp together: a "the host is in this map" packet arrived from the signaling peer. Called
// on the signaling receive thread, so it only records the message; TickBbscFollowHost acts on it.
void OnBbscHostFollowPacket(std::uint32_t packed_map, float x, float y, float z, float heading,
                            std::uint32_t sequence, std::int32_t lamp_point);
// BBSC world state: one chunk of the host's event flags for its map block arrived (a whole
// SignalingBbscWorldFlags packet). Signaling receive thread; only stores it.
void OnBbscWorldFlagsPacket(const void* packet, std::uint32_t size);
// BBSC life state: another player says whether it is dead (spectating) or alive. Relay receive
// thread; only stores it.
void OnBbscLifePacket(std::uint32_t player, std::uint8_t dead, std::uint32_t sequence);
// BBSC scaling: the host's five scaling percentages (enemy HP, boss HP, damage, enemy stagger, boss
// stagger). Relay receive thread; only stores them.
void OnBbscScalePacket(const std::uint16_t (&percent)[5]);
// BBSC PvP (Duel Bell): kind 0 = the host's PvP state, kind 1 = a guest asks the host for `on`.
// Relay receive thread; only stores it.
void OnBbscPvpPacket(std::uint8_t kind, std::uint8_t on);
// BBSC relay sessions (mod): a hosting player's follow packet (relay thread; only stored), the host
// a guest takes packets from (0 = none known yet), and the session state as of the last game tick.
// BBSC shared progress: the host's boss-defeat flags that are on (relay "SHAD" 0x26). Relay thread.
void OnBbscProgressPacket(const std::uint32_t* flags, std::size_t count);
// BBSC boss line: the boss on the host's boss bar (name id, -1 none; relay "SHAD" 0x27).
void OnBbscHostBossPacket(std::int32_t boss);
void NoteBbscRelayHost(std::uint32_t sender, std::uint32_t packed_map, float x, float y, float z);
std::uint32_t GetBbscRelayHost();
std::int32_t GetBbscRelaySessionState();
void InstallSeamlessCoopPatches();
void InstallReverseEngineeringTrace();

// BBSC Discord status: what the game is doing, gathered once a second on the game tick.
struct BbscPresence {
    bool in_game = false;
    std::string area;  // "Central Yharnam", "Chalice Dungeon", ...
    std::string boss;  // boss bar name during a boss fight, else empty
    int role = 0;      // 0 solo, 1 host, 2 guest
    int hunters = 1;   // players in the session
    int max_hunters = 3;
    std::int64_t boss_since_s = 0; // unix time the boss bar appeared
};
BbscPresence GetBbscPresence();

} // namespace Core::Bloodborne
