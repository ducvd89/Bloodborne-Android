// SPDX-License-Identifier: GPL-3.0-or-later
// The Hunter's Dream backend's shared pieces: settings, the account, the session service client,
// the dispatcher that runs the game's callbacks, the peer table and the P2P port's STUN, relay
// and hole punching. Adapted from droogie/bbhost src/net/{http,account,session,stun}.h (566184a).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/types.h"
#include "bbnet_guest.h" // bbport-android: the game's callbacks run on the guest CPU

extern "C" int32_t runtime_thread_spawn(void** out, void* PS4_SYSV_ABI (*entry)(void*),
                                        void* argument, uint64_t stack, const char* name);

namespace Core::Loader {
class SymbolsResolver;
}

namespace BbThd {

using Json = nlohmann::json;

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
// BB_THD_TRACE=1: every datagram and callback in the log, not only the first.
bool Trace();

struct Config {
    std::string np_base;   // BB_THD_SERVER: the session service, https://thehuntersdream.com:18671
    std::string auth_base; // BB_THD_AUTH: the account service, https://thehuntersdream.com
    std::string stun;      // BB_THD_STUN: host[:port]; empty: the server's host on 3478; "off"
    std::string ruleset;   // BB_THD_RULESET: X-BBHost-Ruleset, "vanilla"
    std::string client;    // what the User-Agent names this client by (BB_CLIENT_VERSION)
    std::string p2p_addr;  // BB_THD_P2P_ADDR: our LAN address for peers (default: detected)
    int p2p_port = 3658;   // BB_P2P_PORT
    bool verify_tls = true;
};
const Config& Cfg();

// The account the launcher signed in with Discord. Without one the game stays signed out.
std::string AccountName();
std::string AccountToken();
bool SignedIn();
// The server refused the token (HTTP 401): the poller slows down until the next launch.
bool Refused();
void MarkRefused(const std::string& reason);
// The name the game and the server know this player by.
std::string OnlineId();

std::string StrOf(const Json& o, const char* key, const std::string& dflt = "");
long long IntOf(const Json& o, const char* key, long long dflt = 0);

std::string NpBase();
bool NpPost(const std::string& path, const Json& body, Json& out, std::string& error,
            int timeout_ms = 8000);
bool NpGet(const std::string& path, Json& out, std::string& error, int timeout_ms = 8000);

// ---- the dispatcher: one guest thread, so the callbacks run where the game expects them.
enum class Prio { Context = 0, Request = 1, Signaling = 2, RoomEvent = 3 };
void DispatchAfter(int delay_ms, Prio prio, std::function<void()> fn);
std::int64_t NowMs();

// A guest callback (the SysV ABI, integer arguments).
template <typename... A>
inline std::int64_t CallGuest(void* fn, A... a) {
    using F = std::int64_t(PS4_SYSV_ABI*)(A...);
    return fn ? BbGuestCall(reinterpret_cast<F>(fn), a...) : 0; // bbport-android: through the guest CPU
}

// ---- our endpoint
std::string LocalAddrText();
int P2pPort();
void SetP2pPortBound(int port);
int StunRttUs();

// ---- the peer table
struct Peer {
    std::uint16_t member_id = 0;
    std::string online_id;
    std::uint32_t addr = 0;       // network order: where the peer is reached
    std::uint16_t port = 0;       // host order
    std::uint32_t local_addr = 0; // network order: its LAN address
    std::uint16_t local_port = 0; // host order
    unsigned conn_id = 0;
    int sig_status = 0;
};
void PeersClear();
void PeersUpsert(const Peer& p);
bool PeersGet(std::uint16_t member_id, Peer* out);
bool PeersFindOnline(const std::string& id, Peer* out);
void PeersSetConn(std::uint16_t member_id, unsigned conn_id, int sig_status);
void PeersErase(std::uint16_t member_id);
std::vector<Peer> PeersAll();
int PeersFromMembers(const Json& members);
Peer PeersProvisional(const std::string& id, std::uint32_t addr, std::uint16_t port);

// ---- the session service
bool ServerContextStart(Json& reply, std::string& error);
bool ServerCreateRoom(int max_members, const Json& extra, Json& reply, std::string& error);
bool ServerJoinRoom(std::uint64_t room_id, Json& reply, std::string& error);
bool ServerLeaveRoom(const std::string& session_id, int member_id, Json& reply, std::string& error);
int ServerHeartbeat(const std::string& session_id, int member_id); // 1 in the room, 0 not, -1 no answer
bool ServerKickMember(const std::string& session_id, int member_id, int kicker_id,
                      const std::uint8_t* opt, std::size_t opt_len, std::string& error);
bool ServerSignalingResolve(const std::string& id, std::uint32_t* addr, std::uint16_t* port,
                            std::string& error);
void RoomSet(const std::string& session_id, int member_id);
void RoomClear();
void PollerStart(std::function<void(const Json& event)> handler);
void PollerStop();

// ---- the P2P port (thd_net.cpp)
constexpr std::size_t kStunTokenLen = 8;
struct Relay {
    bool present = false;
    std::uint8_t token[kStunTokenLen] = {};
    std::uint16_t vport = 0;         // host order
    std::uint32_t observed_addr = 0; // network order
    std::uint16_t observed_port = 0; // host order
};
// A STUN Binding exchange from the P2P port (opened if the game has not bound it); with `relay`,
// the relay HELLO, and the relay turned on or off by the answer.
bool P2pStun(const char* host, std::uint16_t port, int timeout_ms, std::uint32_t* mapped_addr,
             std::uint16_t* mapped_port, Relay* relay);
// Probes from the P2P port so our NAT lets the peer in (none for a relayed peer).
void P2pPunch(const char* label, std::uint32_t addr, std::uint16_t port, std::uint32_t local_addr,
              std::uint16_t local_port);
// The address of this machine that reaches `host` (the LAN address peers on it use).
std::string DetectLanAddress(const std::string& host);

// ---- game memory (1.09 only)
void SetSignalingGate();
void HostExtra(Json& extra);

// ---- SceNpId / SceNpOnlineId
void FillNpId(void* out, const char* online);
std::string NpIdText(const void* npid);

// ---- signaling (thd_np.cpp), driven by matching2
void SignalingPeerKnown(std::uint16_t member_id);
void SignalingPeerDead(const std::string& online_id);
void SignalingReset();

void RegisterNet(Core::Loader::SymbolsResolver* sym);
void RegisterMatching2(Core::Loader::SymbolsResolver* sym);
void RegisterNp(Core::Loader::SymbolsResolver* sym);

} // namespace BbThd
