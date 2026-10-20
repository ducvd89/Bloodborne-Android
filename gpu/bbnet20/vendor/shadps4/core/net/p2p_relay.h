// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (from BBSC, seamless co-op): the UDP relay fallback of the P2P transport.
//
// Direct P2P needs one side reachable: CGNAT, strict routers and several windows on one PC (no
// hairpin) leave both sides unreachable and the connection goes DEAD after 30 s. Every peer whose
// online id we know (shadNet lookups) is also reachable through the co-op server's UDP relay
// (bbhost's relay, port 3478 on the shadNet server's address; Seamless Hunt's runs it):
//   "BBR1" 0x01 id[16]                  register, every 5 s (keeps our NAT mapping open); ack 0x02
//   "BBR1" 0x03 dst[16] src[16] ep[6] frame   frame for dst
//   "BBR1" 0x04 src[16] ep[6] frame           frame from src
//   "BBR1" 0x05 dst[16]                       dst is not on the relay (direct only for 10 s)
// ep = the sender's own public address and port as shadNet sees them (its STUN echo), so the
// receiver files the frame under the same endpoint its signaling code uses for that peer. A frame
// is one wire packet of the transport, untouched. A peer that reaches us through the relay is
// answered through it at once.
// Every known peer goes through the relay from the first frame (direct copies still go out until
// the peer answers, and a peer without the relay is answered directly).
// SHADPS4_P2P_RELAY=0 turns it off, =auto (or direct) = "direct first, relay after 2 s",
// =a.b.c.d:port another relay; SHADPS4_P2P_RELAY_PORT changes the port.
#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "core/net/p2p_transport.h"

namespace Core::Net::P2P {

class Relay {
public:
    static Relay& Instance();

    // ---- configuration (the NP libraries; any thread) ---------------------------------------
    /// The relay is on the shadNet server's address (network order); 0 turns it off.
    void SetServer(u32 addr_nbo);
    /// Our own public endpoint as shadNet sees it (STUN echo).
    void SetSelfEndpoint(u32 addr_nbo, u16 port_nbo);
    /// An online id of ours (registered with the relay).
    void SetLocalId(std::string_view online_id);
    /// A peer and the endpoint signaling uses for it.
    void RememberPeer(std::string_view online_id, u32 addr_nbo, u16 port_nbo);

    // ---- the transport ----------------------------------------------------------------------
    using SendRaw = std::function<void(std::span<const u8>, const Endpoint&)>;
    /// Direct = the caller sends directly only; Relay = sent through the relay, nothing else to
    /// do; Both = through the relay and the caller sends it directly too.
    enum class Path { Direct, Relay, Both };
    Path Route(const Endpoint& dest, std::span<const u8> wire, const SendRaw& send);
    struct Received {
        bool consumed = false;  // a relay control packet: nothing to process
        bool unwrapped = false; // a frame from a peer: process `frame` as received from `from`
        Endpoint from;
        std::vector<u8> frame;
    };
    Received OnReceive(std::span<const u8> data, const Endpoint& from);
    /// Registration with the relay; call every few hundred ms.
    void KeepAlive(const SendRaw& send);
    bool Enabled();

private:
    struct Peer {
        std::string id;
        u32 addr{};
        u16 port{};
        bool relayed{false};
        bool direct_rx{false};
        bool tx_started{false};
        bool relay_seen{false}; // it sent to us through the relay (it has the relay too)
        std::chrono::steady_clock::time_point first_tx{};
        std::chrono::steady_clock::time_point relayed_since{};
        // The relay said it is not registered: direct only until then.
        std::chrono::steady_clock::time_point missing_until{};
        bool missing_logged{false};
        std::vector<std::vector<u8>> early; // sent directly before the switch (sent again then)
    };

    static std::string PadId(std::string_view online_id);
    static u64 EndpointKey(u32 addr_nbo, u16 port_nbo);
    static Endpoint ToEndpoint(u32 addr_nbo, u16 port_nbo);
    void AddAliasLocked(const std::string& id, u32 addr_nbo, u16 port_nbo, const char* from);
    Peer* FindPeerLocked(u32 addr_nbo, u16 port_nbo);
    Endpoint ServerEndpointLocked() const;
    void SendRelayedLocked(const Peer& peer, std::span<const u8> wire, const SendRaw& send);

    std::mutex mutex_;
    u32 server_addr_{0};
    u16 server_port_{0};
    bool force_{false};
    bool acked_{false};
    u32 self_addr_{0};
    u16 self_port_{0};
    std::chrono::steady_clock::time_point last_register_{};
    std::vector<std::string> local_ids_;
    std::unordered_map<std::string, Peer> peers_;  // by padded online id
    std::unordered_map<u64, std::string> alias_;   // every known endpoint -> online id
    u32 next_pseudo_{1};
    u64 sent_{0};
    u64 received_{0};
    std::chrono::steady_clock::time_point last_status_{};
    u64 status_sent_{0};
    u64 status_received_{0};
};

} // namespace Core::Net::P2P
