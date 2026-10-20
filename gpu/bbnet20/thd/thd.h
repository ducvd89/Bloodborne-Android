// SPDX-License-Identifier: GPL-3.0-or-later
// The Hunter's Dream backend, beside the shadNet one. BB_NET_BACKEND=thd (online_pool=huntersdream)
// replaces the game's NP and sceNet imports with droogie/bbhost's (566184a; see SHADNET-PORT.md):
// matching and signaling over The Hunter's Dream's session service, its STUN and relay, the PS4
// P2P framing bbhost players use, and WebAPI answered here. The game's own HTTP stays shadPS4's
// sceHttp, with the account token and ruleset added. Without BB_NET_BACKEND=thd nothing here runs
// and the shadNet backend is as it was.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Core::Loader {
class SymbolsResolver;
}

namespace BbThd {

// BB_NET_BACKEND=thd.
bool Enabled();
// Once the game is loaded: the account from the launcher (BB_THD_TOKEN, BB_THD_ONLINE_ID).
void Initialize();
// Every NP and sceNet import the game has, registered ahead of shadPS4's.
void Register(Core::Loader::SymbolsResolver* sym);
// The game's image (Binary Ninja 0x400000 at image[0]), for the NpMatching2 signaling gate and the
// host's area, level and position on create_room. Ignored unless the executable is 1.09.
void SetImage(std::uint8_t* image, std::uint64_t size);
// sceHttp: the headers the game server binds a request to the account and ruleset with.
void AddGameHeaders(std::vector<std::pair<std::string, std::string>>& headers);

} // namespace BbThd
