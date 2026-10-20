// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (from BBSC, seamless co-op): the UDP relay fallback; see p2p_relay.h.

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "common/logging/log.h"
#include "core/net/p2p_relay.h"

namespace Core::Net::P2P {

namespace {
constexpr size_t kIdSize = 16;
constexpr size_t kEpSize = 6;
constexpr u8 kMagic[4] = {'B', 'B', 'R', '1'};
constexpr size_t kEarlyFrames = 32;

std::string IdText(const std::string& id) {
    return std::string(id.c_str());
}

bool V4(const Endpoint& e, u32* addr_nbo, u16* port_nbo) {
    if (e.Family() != AF_INET) {
        return false;
    }
    const auto* in = reinterpret_cast<const sockaddr_in*>(&e.addr);
    *addr_nbo = in->sin_addr.s_addr;
    *port_nbo = in->sin_port;
    return true;
}
} // namespace

Relay& Relay::Instance() {
    static Relay relay;
    return relay;
}

std::string Relay::PadId(std::string_view online_id) {
    std::string id(online_id.substr(0, kIdSize));
    id.resize(kIdSize, static_cast<char>(0));
    return id;
}

u64 Relay::EndpointKey(u32 addr_nbo, u16 port_nbo) {
    return (static_cast<u64>(addr_nbo) << 16) | port_nbo;
}

Endpoint Relay::ToEndpoint(u32 addr_nbo, u16 port_nbo) {
    sockaddr_in in{};
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = addr_nbo;
    in.sin_port = port_nbo;
    return Endpoint::FromSockaddr(reinterpret_cast<const sockaddr*>(&in), sizeof(in));
}

Endpoint Relay::ServerEndpointLocked() const {
    return ToEndpoint(server_addr_, server_port_);
}

bool Relay::Enabled() {
    std::scoped_lock lock{mutex_};
    return server_addr_ != 0;
}

void Relay::SetServer(u32 addr_nbo) {
    std::scoped_lock lock{mutex_};
    const char* env = std::getenv("SHADPS4_P2P_RELAY");
    const std::string mode = env != nullptr ? env : "";
    force_ = mode != "auto" && mode != "direct";
    u16 port = 3478;
    if (const char* p = std::getenv("SHADPS4_P2P_RELAY_PORT"); p != nullptr && *p != 0) {
        const long parsed = std::strtol(p, nullptr, 10);
        if (parsed > 0 && parsed < 65536) {
            port = static_cast<u16>(parsed);
        }
    }
    if (mode == "0" || mode == "off") {
        addr_nbo = 0;
    } else if (const size_t colon = mode.find(':'); colon != std::string::npos) {
        // Test override "a.b.c.d:port" (a relay other than the shadNet server's).
        in_addr parsed{};
        const long parsed_port = std::strtol(mode.c_str() + colon + 1, nullptr, 10);
        if (addr_nbo != 0 && ::inet_pton(AF_INET, mode.substr(0, colon).c_str(), &parsed) == 1 &&
            parsed_port > 0 && parsed_port < 65536) {
            addr_nbo = parsed.s_addr;
            port = static_cast<u16>(parsed_port);
        }
    }
    const u16 port_nbo = addr_nbo != 0 ? htons(port) : 0;
    if (server_addr_ == addr_nbo && server_port_ == port_nbo) {
        return;
    }
    server_addr_ = addr_nbo;
    server_port_ = port_nbo;
    acked_ = false;
    last_register_ = {};
    if (addr_nbo != 0) {
        LOG_INFO(Lib_Net, "P2P relay fallback: server {:#x}:{}{}", addr_nbo, port,
                 force_ ? " (forced for every peer)" : "");
    } else {
        LOG_INFO(Lib_Net, "P2P relay fallback: off");
    }
}

void Relay::SetSelfEndpoint(u32 addr_nbo, u16 port_nbo) {
    std::scoped_lock lock{mutex_};
    if (self_addr_ != addr_nbo || self_port_ != port_nbo) {
        LOG_INFO(Lib_Net, "P2P relay fallback: our public endpoint {:#x}:{}", addr_nbo,
                 ntohs(port_nbo));
    }
    self_addr_ = addr_nbo;
    self_port_ = port_nbo;
}

void Relay::SetLocalId(std::string_view online_id) {
    if (online_id.empty()) {
        return;
    }
    std::scoped_lock lock{mutex_};
    const std::string id = PadId(online_id);
    if (std::find(local_ids_.begin(), local_ids_.end(), id) == local_ids_.end()) {
        local_ids_.push_back(id);
        last_register_ = {};
    }
}

void Relay::RememberPeer(std::string_view online_id, u32 addr_nbo, u16 port_nbo) {
    if (online_id.empty() || addr_nbo == 0 || port_nbo == 0) {
        return;
    }
    std::scoped_lock lock{mutex_};
    const std::string id = PadId(online_id);
    // Peers are kept by online id; every endpoint signaling or the relay ever gave for it stays
    // an alias (a peer filed under one endpoint that moved when the relay brought another one
    // made signaling's sends find no relay entry: a one-way relay, DEAD after 30 s behind CGNAT).
    Peer& peer = peers_[id];
    peer.id = id;
    peer.addr = addr_nbo; // the endpoint signaling uses: relayed frames arrive "from" it
    peer.port = port_nbo;
    AddAliasLocked(id, addr_nbo, port_nbo, "signaling");
}

void Relay::AddAliasLocked(const std::string& id, u32 addr_nbo, u16 port_nbo, const char* from) {
    if (addr_nbo == 0 || port_nbo == 0) {
        return;
    }
    const u64 key = EndpointKey(addr_nbo, port_nbo);
    const auto it = alias_.find(key);
    if (it != alias_.end() && it->second == id) {
        return;
    }
    alias_[key] = id;
    LOG_INFO(Lib_Net, "P2P relay fallback: '{}' known at {:#x}:{} (from {})", IdText(id), addr_nbo,
             ntohs(port_nbo), from);
}

Relay::Peer* Relay::FindPeerLocked(u32 addr_nbo, u16 port_nbo) {
    const auto alias = alias_.find(EndpointKey(addr_nbo, port_nbo));
    if (alias == alias_.end()) {
        return nullptr;
    }
    const auto it = peers_.find(alias->second);
    return it == peers_.end() ? nullptr : &it->second;
}

void Relay::SendRelayedLocked(const Peer& peer, std::span<const u8> wire, const SendRaw& send) {
    const std::string& self = local_ids_.front();
    std::vector<u8> packet;
    packet.reserve(5 + 2 * kIdSize + kEpSize + wire.size());
    packet.insert(packet.end(), std::begin(kMagic), std::end(kMagic));
    packet.push_back(0x03);
    packet.insert(packet.end(), peer.id.begin(), peer.id.end());
    packet.insert(packet.end(), self.begin(), self.end());
    u8 ep[kEpSize];
    std::memcpy(ep, &self_addr_, 4);
    std::memcpy(ep + 4, &self_port_, 2);
    packet.insert(packet.end(), ep, ep + kEpSize);
    packet.insert(packet.end(), wire.begin(), wire.end());
    send(packet, ServerEndpointLocked());
    ++sent_;
}

void Relay::KeepAlive(const SendRaw& send) {
    std::scoped_lock lock{mutex_};
    if (server_addr_ == 0 || local_ids_.empty()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto interval = acked_ ? std::chrono::seconds(5) : std::chrono::seconds(2);
    if (last_register_.time_since_epoch().count() != 0 && now - last_register_ < interval) {
        return;
    }
    last_register_ = now;
    if (now - last_status_ >= std::chrono::seconds(30) &&
        (sent_ != status_sent_ || received_ != status_received_)) {
        last_status_ = now;
        status_sent_ = sent_;
        status_received_ = received_;
        std::string peers;
        for (const auto& [id, peer] : peers_) {
            if (!peers.empty()) {
                peers += ", ";
            }
            peers += IdText(peer.id);
            peers += !peer.relayed              ? " direct"
                     : now < peer.missing_until ? " not on the relay"
                     : peer.relay_seen          ? " relay"
                                                : " relay (no answer yet)";
        }
        LOG_INFO(Lib_Net,
                 "P2P relay fallback: {} frame(s) sent / {} received through the relay so far; "
                 "peers: {}",
                 sent_, received_, peers.empty() ? "none" : peers);
    }
    for (const std::string& id : local_ids_) {
        std::vector<u8> packet(std::begin(kMagic), std::end(kMagic));
        packet.push_back(0x01);
        packet.insert(packet.end(), id.begin(), id.end());
        send(packet, ServerEndpointLocked());
    }
}

Relay::Path Relay::Route(const Endpoint& dest, std::span<const u8> wire, const SendRaw& send) {
    u32 dest_addr = 0;
    u16 dest_port = 0;
    if (!V4(dest, &dest_addr, &dest_port)) {
        return Path::Direct;
    }
    std::scoped_lock lock{mutex_};
    if (server_addr_ == 0 || local_ids_.empty() || dest_addr == server_addr_) {
        return Path::Direct;
    }
    Peer* const found = FindPeerLocked(dest_addr, dest_port);
    if (found == nullptr) {
        return Path::Direct;
    }
    Peer& peer = *found;
    const auto now = std::chrono::steady_clock::now();
    if (peer.relayed && now < peer.missing_until) {
        return Path::Direct;
    }
    if (!peer.relayed) {
        if (force_) {
            peer.relayed = true;
        } else if (!peer.tx_started) {
            peer.tx_started = true;
            peer.first_tx = now;
        } else if (!peer.direct_rx && now - peer.first_tx >= std::chrono::seconds(2)) {
            peer.relayed = true;
        }
        if (!peer.relayed) {
            // Kept for the switch: signaling sends some packets only once (the ActivationRequest);
            // lost directly, the peer ignores everything after it and the connection dies.
            if (!peer.direct_rx && peer.early.size() < kEarlyFrames) {
                peer.early.emplace_back(wire.begin(), wire.end());
            }
            return Path::Direct;
        }
        peer.relayed_since = now;
        LOG_INFO(Lib_Net,
                 "P2P relay fallback: '{}' at {:#x}:{} {} - through the relay ({} earlier "
                 "frame(s) sent again)",
                 IdText(peer.id), peer.addr, ntohs(peer.port),
                 force_ ? "(forced)" : "sent nothing back directly in 2 s", peer.early.size());
        for (const std::vector<u8>& early : peer.early) {
            SendRelayedLocked(peer, early, send);
        }
        peer.early.clear();
        peer.early.shrink_to_fit();
    }
    SendRelayedLocked(peer, wire, send);
    // After the switch, frames still go directly too until the peer answers either way: an
    // emulator without the relay can only be reached directly (and NAT holes can take longer
    // than 2 s to open). A peer that uses the relay gets direct copies for 10 s more.
    const bool direct_copy =
        !peer.direct_rx && (!peer.relay_seen || now - peer.relayed_since < std::chrono::seconds(10));
    return direct_copy ? Path::Both : Path::Relay;
}

Relay::Received Relay::OnReceive(std::span<const u8> data, const Endpoint& from) {
    Received out;
    u32 from_addr = 0;
    u16 from_port = 0;
    if (!V4(from, &from_addr, &from_port)) {
        return out;
    }
    std::scoped_lock lock{mutex_};
    if (server_addr_ == 0) {
        return out;
    }
    if (!(from_addr == server_addr_ && from_port == server_port_)) {
        // A frame from a peer, directly.
        if (!peers_.empty()) {
            if (Peer* const found = FindPeerLocked(from_addr, from_port);
                found != nullptr && !found->direct_rx) {
                Peer& peer = *found;
                peer.direct_rx = true;
                peer.early.clear();
                if (peer.relayed && !peer.relay_seen) {
                    // It answers directly and never used the relay: direct from now on.
                    peer.relayed = false;
                    LOG_INFO(Lib_Net,
                             "P2P relay fallback: '{}' reached us directly - back to direct",
                             IdText(peer.id));
                } else {
                    LOG_INFO(Lib_Net, "P2P relay fallback: '{}' reached us directly{}",
                             IdText(peer.id), peer.relayed ? " (staying on the relay)" : "");
                }
            }
        }
        return out;
    }

    // From the relay itself.
    out.consumed = true;
    if (data.size() < 5 || std::memcmp(data.data(), kMagic, 4) != 0) {
        return out;
    }
    if (data[4] == 0x02) {
        if (!acked_) {
            acked_ = true;
            LOG_INFO(Lib_Net, "P2P relay fallback: registered with the relay");
        }
        return out;
    }
    if (data[4] == 0x05 && data.size() >= 5 + kIdSize) {
        const std::string id(reinterpret_cast<const char*>(data.data() + 5), kIdSize);
        const auto known = peers_.find(id);
        if (known != peers_.end()) {
            Peer& peer = known->second;
            peer.missing_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            if (!peer.missing_logged) {
                peer.missing_logged = true;
                LOG_WARNING(Lib_Net,
                            "P2P relay fallback: '{}' is not on the relay (it has no relay, or it "
                            "is not running) - direct only; if direct does not work either, it "
                            "cannot connect until it updates",
                            IdText(peer.id));
            }
        }
        return out;
    }
    if (data[4] != 0x04 || data.size() < 5 + kIdSize + kEpSize) {
        return out;
    }
    const std::string id(reinterpret_cast<const char*>(data.data() + 5), kIdSize);
    u32 ep_addr = 0;
    u16 ep_port = 0;
    std::memcpy(&ep_addr, data.data() + 5 + kIdSize, 4);
    std::memcpy(&ep_port, data.data() + 5 + kIdSize + 4, 2);
    if (!peers_.contains(id)) {
        // A peer signaling never looked up: the endpoint it gave, else a made-up one
        // (169.254.x.x, never sent to directly) so the answers go back through the relay.
        Peer& made = peers_[id];
        made.id = id;
        if (ep_addr != 0 && ep_port != 0) {
            made.addr = ep_addr;
            made.port = ep_port;
        } else {
            made.addr = htonl(0xA9FE0000u | (next_pseudo_++ & 0xFFFFu));
            made.port = htons(3173);
        }
        AddAliasLocked(id, made.addr, made.port, "relay, new peer");
    }
    // Its own endpoint is one more alias; the peer keeps the endpoint signaling knows, so the
    // frame arrives from where signaling expects it.
    AddAliasLocked(id, ep_addr, ep_port, "relay");
    Peer& peer = peers_[id];
    peer.relay_seen = true;
    peer.missing_until = {};
    if (!peer.relayed) {
        peer.relayed = true;
        peer.relayed_since = std::chrono::steady_clock::now();
        LOG_INFO(Lib_Net,
                 "P2P relay fallback: '{}' reached us through the relay - answering the same way",
                 IdText(peer.id));
    }
    ++received_;
    const size_t head = 5 + kIdSize + kEpSize;
    out.unwrapped = true;
    out.from = ToEndpoint(peer.addr, peer.port);
    out.frame.assign(data.begin() + head, data.end());
    return out;
}

} // namespace Core::Net::P2P
