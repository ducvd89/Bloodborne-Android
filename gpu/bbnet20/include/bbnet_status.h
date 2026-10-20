// SPDX-License-Identifier: GPL-2.0-or-later
// bbport-android: what the online session is doing, for the app (BB_NET_STATUS_FILE: one line `state|detail`,
// rewritten whole under another name and renamed on every change; the app shows it as a badge and in its menu)
// and for the log. The shadNet client and NpHandler, the Hunter's Dream backend and the signaling code report here.
#pragma once

#include <string>

namespace BbNetStatus {

enum class State { Connecting, Online, Offline, Rejected, Error };

/// The pool's name as the player knows it: "The Hunter's Dream" (BB_NET_BACKEND=thd), else "Seamless Hunt".
const char* PoolName();
/// `host` or `host:port` (the port left out when it is the default of shadNet's server, 31313).
std::string ServerLabel(const std::string& host, unsigned port);

/// The session's state and its text for the player. Rejected is final for the run (the server refused this client,
/// retrying does not change that): later calls other than another Rejected are ignored.
void Set(State state, std::string detail);
State Current();
/// Players connected over P2P (signaling/transports that are established); appended to the text while Online.
void SetPeers(int count);
/// shadNet's signaling: transport `id` is established (up) or is not (any other state, or gone).
void TransportUp(int id, bool up);
/// A short notice for the app's chip (BB_NET_BANNER_FILE): a friend request, an invitation, a server message.
void Notice(const std::string& text);

} // namespace BbNetStatus
