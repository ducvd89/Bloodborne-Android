// SPDX-License-Identifier: GPL-3.0-or-later
// The Hunter's Dream backend: sceNpMatching2 over its session service. Adapted from droogie/bbhost
// src/hle/np_matching2.cpp (566184a). The timings and their order are
// the co-op contract bbhost found the game needs.
#include "thd_internal.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "core/libraries/libs.h"

namespace BbThd {
namespace {

struct RoomDataInternal {
    std::uint16_t publicSlots, privateSlots, openPublicSlots, openPrivateSlots, maxSlot, serverId;
    std::uint32_t worldId;
    std::uint64_t lobbyId;
    std::uint64_t roomId;
    std::uint64_t passwdSlotMask;
    std::uint64_t joinedSlotMask;
    const void* roomGroup;
    std::uint64_t roomGroups;
    std::uint32_t flags;
    std::uint32_t pad;
    const void* roomBinAttrInternal;
    std::uint64_t roomBinAttrInternalNum;
};
static_assert(offsetof(RoomDataInternal, roomId) == 0x18 && sizeof(RoomDataInternal) == 0x58);

struct RoomMemberDataInternal {
    RoomMemberDataInternal* next;
    std::uint64_t joinDate;
    std::uint8_t npId[36];
    std::uint8_t pad[4];
    std::uint16_t memberId;
    std::uint8_t teamId;
    std::uint8_t natType;
    std::uint32_t flagAttr;
    void* roomGroup;
    void* roomMemberBinAttrInternal;
    std::uint64_t roomMemberBinAttrInternalNum;
};
static_assert(offsetof(RoomMemberDataInternal, memberId) == 0x38 && sizeof(RoomMemberDataInternal) == 0x58);

struct RoomMemberDataInternalList {
    RoomMemberDataInternal* members;
    std::uint64_t membersNum;
    RoomMemberDataInternal* me;
    RoomMemberDataInternal* owner;
};
struct CreateJoinRoomResponse {
    const RoomDataInternal* roomData;
    RoomMemberDataInternalList members;
};
struct World {
    World* next;
    std::uint32_t worldId, numOfLobby, curNumOfTotalLobby, maxNumOfTotalLobby, curNumOfRoom, maxNumOfRoom,
        curNumOfTotalRoomMember, numOfRoom;
};
static_assert(sizeof(World) == 0x28);
struct GetWorldInfoListResponse {
    World* world;
    std::uint32_t worldNum;
    std::uint32_t pad;
};
struct RoomMemberUpdateInfo {
    RoomMemberDataInternal* roomMemberDataInternal;
    std::uint8_t eventCause;
    std::uint8_t pad[7];
    std::uint8_t optData[16];
    std::uint64_t optDataLen;
};
struct RequestOptParam {
    void* cbFunc;
    void* cbFuncArg;
    std::uint32_t timeout;
    std::uint16_t appReqId;
    std::uint16_t pad;
};
// KICKEDOUT and ROOM_DESTROYED: the game reads optData only when it is 4 bytes long.
struct RoomUpdateInfo {
    std::uint8_t eventCause;
    std::uint8_t pad[3];
    std::int32_t errorCode;
    std::uint8_t optData[16];
    std::uint64_t optDataLen;
};
static_assert(offsetof(RoomUpdateInfo, optData) == 8 && offsetof(RoomUpdateInfo, optDataLen) == 0x18);
struct KickoutRequest {
    std::uint64_t roomId;
    std::uint16_t memberId;
    std::uint8_t blockKickFlag;
    std::uint8_t pad[5];
    std::uint8_t optData[16];
    std::uint64_t optDataLen;
};
static_assert(offsetof(KickoutRequest, optData) == 0x10 && offsetof(KickoutRequest, optDataLen) == 0x20);
struct SignalingGetPingInfoResponse {
    std::uint16_t serverId;
    std::uint16_t pad;
    std::uint32_t worldId;
    std::uint64_t roomId;
    std::uint32_t rtt; // microseconds
    std::uint32_t pad2;
};

constexpr std::uint16_t kEvGetWorldInfoList = 0x0002, kEvSetRoomDataExternal = 0x0004, kEvCreateJoinRoom = 0x0101,
                        kEvJoinRoom = 0x0102, kEvLeaveRoom = 0x0103, kEvKickoutRoomMember = 0x0104,
                        kEvGrantRoomOwner = 0x0105, kEvSearchRoom = 0x0106, kEvSetRoomDataInternal = 0x0109,
                        kEvSetRoomMemberDataInternal = 0x010B, kEvSignalingGetPingInfo = 0x0E01,
                        kEvMemberJoined = 0x1101, kEvMemberLeft = 0x1102, kEvKickedOut = 0x1103,
                        kEvRoomDestroyed = 0x1104, kEvSignalingDead = 0x5101, kEvSignalingEstablished = 0x5102,
                        kEvContextStarted = 0x6F02;
constexpr std::uint8_t kCauseLeave = 1, kCauseKickout = 2, kCauseServerOperation = 4, kCauseMemberDisappeared = 5;
constexpr int kErrInvalidArg = static_cast<int>(0x80550C03);
constexpr int kErrContext = static_cast<int>(0x80550C06);
constexpr int kErrServerNotAvailable = static_cast<int>(0x80550C28);
constexpr int kErrConnection = static_cast<int>(0x80550C0B);
constexpr int kErrTerminatedByPeer = static_cast<int>(0x80550E10);
constexpr int kErrTerminatedByMyself = static_cast<int>(0x80550E18);

// A response the game reads during its callback; the last few stay alive.
struct Held {
    std::vector<std::uint8_t> bytes;
};

struct M2 {
    std::mutex mu;
    unsigned ctx_id = 0;
    bool started = false;
    void *ctx_cb = nullptr, *ctx_arg = nullptr;
    void *room_cb = nullptr, *room_arg = nullptr;
    void *sig_cb = nullptr, *sig_arg = nullptr;
    void *lobby_cb = nullptr, *lobby_arg = nullptr;
    RequestOptParam dflt{};
    std::atomic<unsigned> next_req{1};
    std::string session_id;
    std::uint64_t room_id = 0;
    std::uint16_t member_id = 0, owner_id = 0, max_slot = 0;
    bool is_host = false, in_room = false;
    std::uint64_t joining_room = 0; // its member events can arrive before JoinRoom's answer
    std::int64_t room_since_ms = 0;
    bool leave_pending = false;
    unsigned leave_req = 0;
    RequestOptParam leave_opt{};
    std::vector<std::unique_ptr<Held>> held;
};
M2 g;

template <typename T>
T* Hold(std::size_t extra = 0) {
    auto held = std::make_unique<Held>();
    held->bytes.assign(sizeof(T) + extra, 0);
    T* p = reinterpret_cast<T*>(held->bytes.data());
    g.held.push_back(std::move(held));
    if (g.held.size() > 8) g.held.erase(g.held.begin());
    return p;
}

void fire_request(void* fn, void* arg, unsigned req, std::uint16_t event, int error, void* data) {
    if (!fn) return;
    if (Trace() || event != kEvGetWorldInfoList) {
        Log("np: REQUEST_CB event 0x%x req %u error 0x%x", event, req, static_cast<unsigned>(error));
    }
    CallGuest(fn, static_cast<std::int64_t>(g.ctx_id), static_cast<std::int64_t>(req),
              static_cast<std::int64_t>(event), static_cast<std::int64_t>(error), data, arg);
}
// A callback read under g.mu and made once it is released: the game's handlers take their own
// locks and call back into sceNpMatching2.
struct M2Call {
    void* fn = nullptr;
    std::int64_t a[6] = {};
    void run() const {
        if (fn) CallGuest(fn, a[0], a[1], a[2], a[3], a[4], a[5]);
    }
};
M2Call request_call(void* fn, void* arg, unsigned req, std::uint16_t event, int error, void* data) {
    if (!fn) return {};
    if (Trace() || event != kEvGetWorldInfoList) {
        Log("np: REQUEST_CB event 0x%x req %u error 0x%x", event, req, static_cast<unsigned>(error));
    }
    return {fn, {static_cast<std::int64_t>(g.ctx_id), static_cast<std::int64_t>(req), static_cast<std::int64_t>(event),
                 error, reinterpret_cast<std::int64_t>(data), reinterpret_cast<std::int64_t>(arg)}};
}
M2Call context_call(std::uint16_t event, int cause, int error) {
    if (!g.ctx_cb) return {};
    Log("np: CONTEXT_CB event 0x%x", event);
    return {g.ctx_cb, {static_cast<std::int64_t>(g.ctx_id), static_cast<std::int64_t>(event), cause, error,
                       reinterpret_cast<std::int64_t>(g.ctx_arg), 0}};
}
M2Call room_event_call(std::uint16_t event, void* data, std::uint64_t room_id) {
    if (!g.room_cb) return {};
    Log("np: ROOM_EVENT_CB event 0x%x room %llu", event, static_cast<unsigned long long>(room_id));
    return {g.room_cb, {static_cast<std::int64_t>(g.ctx_id), static_cast<std::int64_t>(room_id),
                        static_cast<std::int64_t>(event), reinterpret_cast<std::int64_t>(data),
                        reinterpret_cast<std::int64_t>(g.room_arg), 0}};
}
M2Call room_event_call(std::uint16_t event, void* data) { return room_event_call(event, data, g.room_id); }
void fire_signaling(std::uint16_t member, std::uint16_t event, int error, std::uint64_t room_id) {
    if (!g.sig_cb) return;
    if (Trace()) Log("np: SIGNALING_CB event 0x%x member %u", event, member);
    CallGuest(g.sig_cb, static_cast<std::int64_t>(g.ctx_id), static_cast<std::int64_t>(room_id),
              static_cast<std::int64_t>(member), static_cast<std::int64_t>(event), static_cast<std::int64_t>(error),
              g.sig_arg);
}
void fire_signaling(std::uint16_t member, std::uint16_t event, int error) {
    fire_signaling(member, event, error, g.room_id);
}

// The room's real members (provisional 0xff00+ entries are the same people, resolved earlier).
std::vector<Peer> room_peers() {
    std::vector<Peer> v;
    for (const Peer& p : PeersAll()) {
        if (p.member_id < 0xff00) v.push_back(p);
    }
    return v;
}

// ESTABLISHED for every member, self included, before the game's signaling poll.
void schedule_established_for_all(int delay_ms) {
    std::vector<std::uint16_t> ids;
    for (const Peer& p : room_peers()) ids.push_back(p.member_id);
    ids.push_back(g.member_id);
    DispatchAfter(delay_ms, Prio::Signaling, [ids] {
        for (std::uint16_t m : ids) fire_signaling(m, kEvSignalingEstablished, 0);
    });
}
// DEAD for each member first, then the room event.
void schedule_dead(const std::vector<std::uint16_t>& ids, int error, std::uint64_t room_id, int delay_ms) {
    if (ids.empty()) return;
    DispatchAfter(delay_ms, Prio::Signaling, [ids, error, room_id] {
        for (std::uint16_t m : ids) fire_signaling(m, kEvSignalingDead, error, room_id);
    });
}

CreateJoinRoomResponse* build_room_response(std::uint64_t room_id, std::uint16_t self_id, std::uint16_t owner_id,
                                            std::uint16_t max_slot) {
    const std::vector<Peer> peers = room_peers();
    const std::size_t n = peers.size() + 1;
    auto* resp = Hold<CreateJoinRoomResponse>(sizeof(RoomDataInternal) + n * sizeof(RoomMemberDataInternal));
    auto* room = reinterpret_cast<RoomDataInternal*>(resp + 1);
    auto* members = reinterpret_cast<RoomMemberDataInternal*>(room + 1);
    room->maxSlot = max_slot ? max_slot : 5;
    room->publicSlots = room->maxSlot;
    room->openPublicSlots = static_cast<std::uint16_t>(room->maxSlot > n ? room->maxSlot - n : 0);
    room->serverId = 1;
    room->worldId = 1;
    room->lobbyId = 1;
    room->roomId = room_id;
    std::uint64_t joined = 0;
    std::size_t i = 0;
    auto put = [&](std::uint16_t member_id, const std::string& online) {
        RoomMemberDataInternal& m = members[i];
        m.next = i + 1 < n ? &members[i + 1] : nullptr;
        FillNpId(m.npId, online.c_str());
        m.memberId = member_id;
        m.natType = 1;
        if (member_id && member_id <= 64) joined |= 1ull << (member_id - 1);
        if (member_id == self_id) resp->members.me = &m;
        if (member_id == owner_id) {
            resp->members.owner = &m;
            m.flagAttr |= 0x80000000u; // OWNER
        }
        ++i;
    };
    put(self_id, OnlineId());
    for (const Peer& p : peers) put(p.member_id, p.online_id);
    room->joinedSlotMask = joined;
    resp->roomData = room;
    resp->members.members = members;
    resp->members.membersNum = n;
    if (!resp->members.owner) resp->members.owner = resp->members.me;
    return resp;
}

RoomMemberUpdateInfo* build_member_update(const Peer& p, std::uint8_t cause) {
    auto* info = Hold<RoomMemberUpdateInfo>(sizeof(RoomMemberDataInternal));
    auto* m = reinterpret_cast<RoomMemberDataInternal*>(info + 1);
    FillNpId(m->npId, p.online_id.c_str());
    m->memberId = p.member_id;
    m->natType = 1;
    if (p.member_id == g.owner_id) m->flagAttr |= 0x80000000u;
    info->roomMemberDataInternal = m;
    info->eventCause = cause;
    return info;
}

RoomUpdateInfo* build_room_update(std::uint8_t cause, const std::uint8_t* opt, std::size_t opt_len) {
    auto* info = Hold<RoomUpdateInfo>();
    info->eventCause = cause;
    opt_len = std::min(opt_len, sizeof(info->optData));
    if (opt && opt_len) std::memcpy(info->optData, opt, opt_len);
    info->optDataLen = opt_len;
    return info;
}

RequestOptParam opt_or_default(const RequestOptParam* opt) {
    return opt && opt->cbFunc ? *opt : g.dflt;
}

void forget_room_locked() {
    g.in_room = false;
    g.session_id.clear();
    g.room_id = 0;
    g.owner_id = 0;
    g.max_slot = 0;
    PeersClear();
    RoomClear();
    SignalingReset();
}

// Our LeaveRoom (or the context going away): out of the server's room, DEAD for every member,
// then 0x103 at +300 ms.
void end_session_locked(const char* why) {
    if (!g.in_room && !g.leave_pending) return;
    Log("np: session over (%s): room %llu", why, static_cast<unsigned long long>(g.room_id));
    if (!g.session_id.empty()) {
        Json reply;
        std::string err;
        ServerLeaveRoom(g.session_id, g.member_id, reply, err);
    }
    std::vector<std::uint16_t> ids;
    for (const Peer& p : room_peers()) ids.push_back(p.member_id);
    schedule_dead(ids, kErrTerminatedByMyself, g.room_id, 0);
    if (g.leave_pending) {
        const unsigned req = g.leave_req;
        const RequestOptParam opt = g.leave_opt;
        g.leave_pending = false;
        DispatchAfter(300, Prio::Request, [req, opt] { fire_request(opt.cbFunc, opt.cbFuncArg, req, kEvLeaveRoom, 0, nullptr); });
    }
    forget_room_locked();
}

// The server ended our membership: the game hears KICKEDOUT or ROOM_DESTROYED after a DEAD per
// member, or it goes on summoning into a room the server no longer has.
void room_gone_locked(std::uint16_t event, std::uint8_t cause, const std::uint8_t* opt, std::size_t opt_len,
                      const std::string& why) {
    if (!g.in_room) return;
    const std::uint64_t room = g.room_id;
    Log("np: room %llu gone (%s): the game hears 0x%x", static_cast<unsigned long long>(room), why.c_str(), event);
    std::vector<std::uint16_t> ids;
    for (const Peer& p : room_peers()) {
        ids.push_back(p.member_id);
        SignalingPeerDead(p.online_id);
    }
    schedule_dead(ids, kErrTerminatedByPeer, room, 0);
    RoomUpdateInfo* info = build_room_update(cause, opt, opt_len);
    DispatchAfter(100, Prio::RoomEvent, [event, info, room] {
        M2Call c;
        {
            std::lock_guard lk(g.mu);
            c = room_event_call(event, info, room);
        }
        c.run();
    });
    forget_room_locked();
}

// The server drops a member whose heartbeat is 15 s old: one every 5 s while in a room.
void heartbeat_loop() {
    std::string sid;
    int mid = 0;
    {
        std::lock_guard lk(g.mu);
        if (g.in_room && !g.session_id.empty()) {
            sid = g.session_id;
            mid = g.member_id;
        }
    }
    if (!sid.empty() && ServerHeartbeat(sid, mid) == 0) {
        std::lock_guard lk(g.mu);
        if (g.in_room && g.session_id == sid) {
            room_gone_locked(kEvRoomDestroyed, kCauseServerOperation, nullptr, 0, "the server has no room for us");
        }
    }
    DispatchAfter(5000, Prio::Request, heartbeat_loop);
}

std::vector<std::uint8_t> b64_decode(const std::string& in) {
    std::vector<std::uint8_t> out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char ch : in) {
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+') v = 62;
        else if (ch == '/') v = 63;
        else continue;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>(acc >> bits));
        }
    }
    return out;
}

bool is_loopback(std::uint32_t addr_nbo) { return (addr_nbo & 0xff) == 127; }
bool server_is_local() {
    const std::string base = NpBase();
    return base.find("://127.") != std::string::npos || base.find("://localhost") != std::string::npos;
}

void on_server_event(const Json& ev);

bool defer_for_join_locked(const Json& ev) {
    const auto room = static_cast<std::uint64_t>(IntOf(ev, "RoomId", 0));
    if (g.in_room || !room || room != g.joining_room) return false;
    const long long tries = IntOf(ev, "_Deferred", 0);
    if (tries >= 20) return false;
    Json again = ev;
    again["_Deferred"] = tries + 1;
    DispatchAfter(250, Prio::RoomEvent, [again] { on_server_event(again); });
    return true;
}

void on_server_event(const Json& ev) {
    const std::string name = StrOf(ev, "Name");
    if (name == "guest_invite") {
        // The host's addresses as the server knows them: the punch opens our NAT toward the host
        // before its first datagram. The game delivers the summon itself over P2P.
        std::lock_guard lk(g.mu);
        Json rec = {{"MemberId", 1},
                    {"OnlineId", StrOf(ev, "HostOnlineId")},
                    {"Addr", StrOf(ev, "HostAddr")},
                    {"Port", IntOf(ev, "HostPort", 0)},
                    {"LocalAddr", StrOf(ev, "HostLocalAddr")},
                    {"LocalPort", IntOf(ev, "HostLocalPort", 0)},
                    {"MappedAddr", StrOf(ev, "HostMappedAddr")},
                    {"MappedPort", IntOf(ev, "HostMappedPort", 0)}};
        Peer before;
        const bool had = PeersGet(1, &before);
        PeersFromMembers(Json::array({rec}));
        Peer seeded;
        if (PeersGet(1, &seeded) && is_loopback(seeded.addr) && !server_is_local()) {
            if (had) PeersUpsert(before);
            else PeersErase(1);
        }
        Log("np: guest_invite: room %lld from %s (%s:%lld)", IntOf(ev, "RoomId", 0), StrOf(ev, "HostOnlineId").c_str(),
            StrOf(ev, "HostAddr").c_str(), IntOf(ev, "HostPort", 0));
        return;
    }
    if (name == "room_member_joined") {
        std::lock_guard lk(g.mu);
        const auto mid = static_cast<std::uint16_t>(IntOf(ev, "MemberId", 0));
        if (defer_for_join_locked(ev)) return;
        if (!g.in_room || !mid || mid == g.member_id) return;
        if (static_cast<std::uint64_t>(IntOf(ev, "RoomId", 0)) != g.room_id) return;
        Peer known;
        const bool already = PeersGet(mid, &known) && known.online_id == StrOf(ev, "OnlineId");
        if (PeersFromMembers(Json::array({ev})) == 0) return;
        Peer p;
        if (!PeersGet(mid, &p)) return;
        SignalingPeerKnown(p.member_id);
        if (already) return; // the address is news, the member is not
        Log("np: member %u (%s) joined room %llu", p.member_id, p.online_id.c_str(),
            static_cast<unsigned long long>(g.room_id));
        DispatchAfter(300, Prio::RoomEvent, [p] {
            M2Call c;
            {
                std::lock_guard lk2(g.mu);
                c = room_event_call(kEvMemberJoined, build_member_update(p, 0));
            }
            c.run();
        });
        DispatchAfter(800, Prio::Signaling, [mid] {
            fire_signaling(mid, kEvSignalingEstablished, 0);
            fire_signaling(g.member_id, kEvSignalingEstablished, 0);
        });
        return;
    }
    if (name == "room_member_left") {
        std::lock_guard lk(g.mu);
        const auto mid = static_cast<std::uint16_t>(IntOf(ev, "MemberId", 0));
        if (defer_for_join_locked(ev)) return;
        if (static_cast<std::uint64_t>(IntOf(ev, "RoomId", 0)) != g.room_id) return;
        Peer p;
        if (!PeersGet(mid, &p)) return;
        const std::string reason = StrOf(ev, "Reason");
        const std::uint8_t cause = reason == "kicked" ? kCauseKickout
                                   : (reason == "leave_room" || reason.empty()) ? kCauseLeave
                                                                                : kCauseMemberDisappeared;
        Log("np: member %u (%s) left (%s)", mid, p.online_id.c_str(), reason.c_str());
        // DEAD for that member, then MEMBER_LEFT; the room stays ours.
        schedule_dead({mid}, kErrTerminatedByPeer, g.room_id, 0);
        DispatchAfter(100, Prio::RoomEvent, [p, cause] {
            M2Call c;
            {
                std::lock_guard lk2(g.mu);
                c = room_event_call(kEvMemberLeft, build_member_update(p, cause));
            }
            c.run();
        });
        SignalingPeerDead(p.online_id);
        PeersErase(mid);
        for (const Peer& q : PeersAll()) {
            if (q.member_id >= 0xff00 && q.online_id == p.online_id) PeersErase(q.member_id);
        }
        return;
    }
    if (name == "peer_deactivated") {
        std::string who = StrOf(ev, "OnlineId");
        if (who.empty()) who = StrOf(ev, "PeerOnlineId");
        if (!who.empty()) SignalingPeerDead(who);
        return;
    }
    if (name == "room_closed") {
        std::lock_guard lk(g.mu);
        const auto room = static_cast<std::uint64_t>(IntOf(ev, "RoomId", 0));
        if (room && room != g.room_id) return;
        const std::string reason = StrOf(ev, "Reason");
        room_gone_locked(kEvRoomDestroyed, reason == "host_left" ? kCauseLeave : kCauseServerOperation, nullptr, 0,
                         "room closed: " + reason);
        return;
    }
    if (name == "room_member_kicked") {
        std::lock_guard lk(g.mu);
        const auto room = static_cast<std::uint64_t>(IntOf(ev, "RoomId", 0));
        const auto mid = static_cast<std::uint16_t>(IntOf(ev, "MemberId", 0));
        if (room != g.room_id || mid != g.member_id) return;
        const std::vector<std::uint8_t> opt = b64_decode(StrOf(ev, "OptData"));
        room_gone_locked(kEvKickedOut, kCauseKickout, opt.data(), opt.size(), "kicked by the host");
        return;
    }
    if (Trace()) Log("np: event %s ignored", name.c_str());
}

// ---- the imports -----------------------------------------------------------------------------------

s32 PS4_SYSV_ABI m2_initialize(const void*) {
    Log("sceNpMatching2Initialize");
    return 0;
}
s32 PS4_SYSV_ABI m2_terminate() {
    std::lock_guard lk(g.mu);
    PollerStop();
    g.started = false;
    return 0;
}
// Bloodborne's two-argument form: (const CreateContextParam*, ctxId*).
s32 PS4_SYSV_ABI m2_create_context(const void*, unsigned* ctx) {
    if (!ctx) return kErrInvalidArg;
    std::lock_guard lk(g.mu);
    if (!g.ctx_id) g.ctx_id = 1;
    *ctx = g.ctx_id;
    Log("sceNpMatching2CreateContext -> %u", g.ctx_id);
    return 0;
}
s32 PS4_SYSV_ABI m2_register_context_cb(void* fn, void* arg) {
    std::lock_guard lk(g.mu);
    g.ctx_cb = fn;
    g.ctx_arg = arg;
    return 0;
}
s32 PS4_SYSV_ABI m2_context_start(unsigned ctx, unsigned) {
    std::lock_guard lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    if (g.started) return 0;
    g.started = true;
    Log("sceNpMatching2ContextStart -> server %s", NpBase().c_str());
    SetSignalingGate();
    PollerStart(on_server_event);
    static bool heartbeat_started = false;
    if (!heartbeat_started) {
        heartbeat_started = true;
        DispatchAfter(5000, Prio::Request, heartbeat_loop);
    }
    // Registered off the game's thread; STARTED follows whether or not the server answered, so
    // an unreachable server reads as online but alone.
    DispatchAfter(0, Prio::Context, [] {
        Json reply;
        std::string err;
        if (!ServerContextStart(reply, err)) Log("np: context_start failed: %s", err.c_str());
    });
    DispatchAfter(200, Prio::Context, [] {
        M2Call c;
        {
            std::lock_guard lk2(g.mu);
            c = context_call(kEvContextStarted, 0, 0);
        }
        c.run();
    });
    return 0;
}
s32 PS4_SYSV_ABI m2_context_stop(unsigned) {
    std::lock_guard lk(g.mu);
    PollerStop();
    g.started = false;
    return 0;
}
s32 PS4_SYSV_ABI m2_destroy_context(unsigned) {
    std::lock_guard lk(g.mu);
    PollerStop();
    end_session_locked("context destroyed");
    g.started = false;
    g.ctx_cb = g.room_cb = g.sig_cb = g.lobby_cb = nullptr;
    return 0;
}
s32 PS4_SYSV_ABI m2_get_server_id(unsigned ctx, std::uint16_t* id) {
    if (!id) return kErrInvalidArg;
    if (ctx != g.ctx_id) return kErrContext;
    *id = 1;
    return 0;
}
s32 PS4_SYSV_ABI m2_set_default_opt(unsigned, const RequestOptParam* opt) {
    std::lock_guard lk(g.mu);
    if (opt) g.dflt = *opt;
    return 0;
}
s32 PS4_SYSV_ABI m2_register_room_event_cb(unsigned, void* fn, void* arg) {
    std::lock_guard lk(g.mu);
    g.room_cb = fn;
    g.room_arg = arg;
    return 0;
}
s32 PS4_SYSV_ABI m2_register_signaling_cb(unsigned, void* fn, void* arg) {
    std::lock_guard lk(g.mu);
    g.sig_cb = fn;
    g.sig_arg = arg;
    return 0;
}
s32 PS4_SYSV_ABI m2_register_lobby_event_cb(unsigned, void* fn, void* arg) {
    std::lock_guard lk(g.mu);
    g.lobby_cb = fn;
    g.lobby_arg = arg;
    return 0;
}

s32 PS4_SYSV_ABI m2_get_world_info_list(unsigned ctx, const void*, const RequestOptParam* opt, unsigned* req_out) {
    std::lock_guard lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    const RequestOptParam o = opt_or_default(opt);
    DispatchAfter(100, Prio::Request, [req, o] {
        M2Call c;
        {
            std::lock_guard lk2(g.mu);
            auto* resp = Hold<GetWorldInfoListResponse>(sizeof(World));
            auto* w = reinterpret_cast<World*>(resp + 1);
            w->worldId = 1;
            w->numOfLobby = 1;
            w->maxNumOfTotalLobby = 1;
            w->maxNumOfRoom = 1000;
            w->numOfRoom = 1000;
            resp->world = w;
            resp->worldNum = 1;
            c = request_call(o.cbFunc, o.cbFuncArg, req, kEvGetWorldInfoList, 0, resp);
        }
        c.run();
    });
    return 0;
}

// CreateJoinRoomRequest: maxSlot u16 at +0.
s32 PS4_SYSV_ABI m2_create_join_room(unsigned ctx, const std::uint8_t* reqp, const RequestOptParam* opt,
                                     unsigned* req_out) {
    std::lock_guard lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    std::uint16_t max_slot = 5; // the host, two cooperators, two invaders
    if (reqp) std::memcpy(&max_slot, reqp, 2);
    const RequestOptParam o = opt_or_default(opt);
    Log("sceNpMatching2CreateJoinRoom req %u maxSlot %u", req, max_slot);
    DispatchAfter(0, Prio::Request, [req, o, max_slot] {
        Json extra = Json::object();
        HostExtra(extra);
        Json reply;
        std::string err;
        std::uint64_t room = 0;
        std::string sid;
        int error = 0;
        if (ServerCreateRoom(max_slot, extra, reply, err) && IntOf(reply, "ResKind", -1) == 0) {
            room = static_cast<std::uint64_t>(IntOf(reply, "RoomId", 0));
            sid = StrOf(reply, "SessionId");
        } else {
            Log("np: create_room failed: %s", err.empty() ? StrOf(reply, "Message").c_str() : err.c_str());
            error = kErrConnection;
        }
        CreateJoinRoomResponse* resp = nullptr;
        {
            std::lock_guard lk2(g.mu);
            if (!error) {
                g.session_id = sid;
                g.room_id = room;
                g.member_id = static_cast<std::uint16_t>(IntOf(reply, "MemberId", 1));
                g.owner_id = g.member_id;
                g.max_slot = static_cast<std::uint16_t>(IntOf(reply, "MaxMembers", max_slot));
                g.is_host = true;
                g.in_room = true;
                g.room_since_ms = NowMs();
                PeersClear();
                RoomSet(sid, g.member_id);
                resp = build_room_response(room, g.member_id, g.member_id, max_slot);
                Log("np: room %llu created, we are member %u", static_cast<unsigned long long>(room), g.member_id);
            }
        }
        DispatchAfter(300, Prio::Request, [req, o, error, resp] {
            fire_request(o.cbFunc, o.cbFuncArg, req, kEvCreateJoinRoom, error, resp);
        });
        if (!error) {
            std::lock_guard lk2(g.mu);
            schedule_established_for_all(800);
        }
    });
    return 0;
}

// JoinRoomRequest: roomId u64 at +0.
s32 PS4_SYSV_ABI m2_join_room(unsigned ctx, const std::uint8_t* reqp, const RequestOptParam* opt, unsigned* req_out) {
    std::lock_guard lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    std::uint64_t room = 0;
    if (reqp) std::memcpy(&room, reqp, 8);
    const RequestOptParam o = opt_or_default(opt);
    Log("sceNpMatching2JoinRoom req %u room %llu", req, static_cast<unsigned long long>(room));
    g.joining_room = room;
    DispatchAfter(0, Prio::Request, [req, o, room] {
        Json reply;
        std::string err;
        int error = 0;
        if (!(ServerJoinRoom(room, reply, err) && IntOf(reply, "ResKind", -1) == 0)) {
            Log("np: join_room failed: %s", err.empty() ? StrOf(reply, "Message").c_str() : err.c_str());
            error = kErrConnection;
        }
        CreateJoinRoomResponse* resp = nullptr;
        {
            std::lock_guard lk2(g.mu);
            if (g.joining_room == room) g.joining_room = 0;
            if (!error) {
                g.session_id = StrOf(reply, "SessionId");
                g.room_id = room;
                g.member_id = static_cast<std::uint16_t>(IntOf(reply, "MemberId", 2));
                g.owner_id = static_cast<std::uint16_t>(IntOf(reply, "OwnerMemberId", 1));
                g.max_slot = static_cast<std::uint16_t>(IntOf(reply, "MaxMembers", 5));
                g.is_host = false;
                g.in_room = true;
                g.room_since_ms = NowMs();
                // The member list is the room (provisional entries stay for their connections).
                for (const Peer& p : PeersAll()) {
                    if (p.member_id < 0xff00) PeersErase(p.member_id);
                }
                if (const auto it = reply.find("Members"); it != reply.end()) PeersFromMembers(*it);
                for (const Peer& p : PeersAll()) SignalingPeerKnown(p.member_id);
                RoomSet(g.session_id, g.member_id);
                resp = build_room_response(room, g.member_id, g.owner_id, g.max_slot);
                Log("np: joined room %llu as member %u with %zu peers", static_cast<unsigned long long>(room),
                    g.member_id, room_peers().size());
            }
        }
        DispatchAfter(300, Prio::Request, [req, o, error, resp] {
            fire_request(o.cbFunc, o.cbFuncArg, req, kEvJoinRoom, error, resp);
        });
        if (!error) {
            std::lock_guard lk2(g.mu);
            schedule_established_for_all(500);
        }
    });
    return 0;
}

// The game calls this when its session is over: leave the server's room now, 0x103 at +300 ms.
s32 PS4_SYSV_ABI m2_leave_room(unsigned ctx, const void*, const RequestOptParam* opt, unsigned* req_out) {
    std::lock_guard lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    Log("sceNpMatching2LeaveRoom req %u", req);
    g.leave_pending = true;
    g.leave_req = req;
    g.leave_opt = opt_or_default(opt);
    end_session_locked("LeaveRoom");
    return 0;
}

// (ctx, roomId, memberId, status*, SceNetInAddr*, SceInPort_t*)
s32 PS4_SYSV_ABI m2_signaling_get_connection_status(unsigned ctx, std::uint64_t, std::uint16_t member, int* status,
                                                    std::uint32_t* addr, std::uint16_t* port) {
    if (ctx != g.ctx_id) return kErrContext;
    Peer p;
    const bool known = PeersGet(member, &p) && p.addr != 0;
    if (status) *status = known ? 2 : 0;
    if (addr) *addr = known ? p.addr : 0;
    if (port) *port = known ? static_cast<std::uint16_t>((p.port << 8) | (p.port >> 8)) : 0;
    return 0;
}

// The requests the summon path does not need still answer: the game runs nothing else until they do.
s32 answer_request(unsigned ctx, const RequestOptParam* opt, unsigned* req_out, std::uint16_t event, int error,
                   const char* what) {
    std::lock_guard lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    const RequestOptParam o = opt_or_default(opt);
    if (Trace()) Log("sceNpMatching2%s req %u: answered here", what, req);
    DispatchAfter(100, Prio::Request, [o, req, event, error] { fire_request(o.cbFunc, o.cbFuncArg, req, event, error, nullptr); });
    return 0;
}

s32 PS4_SYSV_ABI m2_kickout(unsigned ctx, const KickoutRequest* rq, const RequestOptParam* opt, unsigned* req_out) {
    std::lock_guard lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    if (!rq) return kErrInvalidArg;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    const RequestOptParam o = opt_or_default(opt);
    const KickoutRequest r = *rq;
    Log("sceNpMatching2KickoutRoomMember req %u member %u", req, r.memberId);
    const std::string sid = g.session_id;
    const int me = g.member_id;
    DispatchAfter(0, Prio::Request, [o, req, r, sid, me] {
        std::string err;
        const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(r.optDataLen), sizeof(r.optData));
        if (sid.empty() || !ServerKickMember(sid, r.memberId, me, r.optData, n, err)) {
            // Without the server the room hears it here.
            Json ev = {{"Name", "room_member_left"}, {"MemberId", static_cast<int>(r.memberId)}, {"Reason", "kicked"}};
            {
                std::lock_guard lk2(g.mu);
                ev["RoomId"] = static_cast<long long>(g.room_id);
            }
            on_server_event(ev);
        }
        DispatchAfter(100, Prio::Request, [o, req] { fire_request(o.cbFunc, o.cbFuncArg, req, kEvKickoutRoomMember, 0, nullptr); });
    });
    return 0;
}
s32 PS4_SYSV_ABI m2_grant_room_owner(unsigned ctx, const void*, const RequestOptParam* opt, unsigned* req_out) {
    return answer_request(ctx, opt, req_out, kEvGrantRoomOwner, 0, "GrantRoomOwner");
}
s32 PS4_SYSV_ABI m2_set_room_data_internal(unsigned ctx, const void*, const RequestOptParam* opt, unsigned* req_out) {
    return answer_request(ctx, opt, req_out, kEvSetRoomDataInternal, 0, "SetRoomDataInternal");
}
s32 PS4_SYSV_ABI m2_set_room_data_external(unsigned ctx, const void*, const RequestOptParam* opt, unsigned* req_out) {
    return answer_request(ctx, opt, req_out, kEvSetRoomDataExternal, 0, "SetRoomDataExternal");
}
s32 PS4_SYSV_ABI m2_set_room_member_data_internal(unsigned ctx, const void*, const RequestOptParam* opt,
                                                  unsigned* req_out) {
    return answer_request(ctx, opt, req_out, kEvSetRoomMemberDataInternal, 0, "SetRoomMemberDataInternal");
}
// No room list: summons go through the game's own sign server. An error reads as nothing found.
s32 PS4_SYSV_ABI m2_search_room(unsigned ctx, const void*, const RequestOptParam* opt, unsigned* req_out) {
    return answer_request(ctx, opt, req_out, kEvSearchRoom, kErrServerNotAvailable, "SearchRoom");
}
// The round trip through the relay, from the keepalive's STUN exchange.
s32 PS4_SYSV_ABI m2_signaling_get_ping_info(unsigned ctx, const std::uint64_t* rq, const RequestOptParam* opt,
                                            unsigned* req_out) {
    std::lock_guard lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    const RequestOptParam o = opt_or_default(opt);
    auto* resp = Hold<SignalingGetPingInfoResponse>();
    resp->serverId = 1;
    resp->worldId = 1;
    resp->roomId = rq ? *rq : g.room_id;
    const int rtt = StunRttUs();
    resp->rtt = static_cast<std::uint32_t>(rtt > 0 ? 2 * rtt : 60000);
    DispatchAfter(50, Prio::Request, [o, req, resp] { fire_request(o.cbFunc, o.cbFuncArg, req, kEvSignalingGetPingInfo, 0, resp); });
    return 0;
}
// Lobbies: never called by this game.
s32 PS4_SYSV_ABI m2_unused(unsigned, const void*, const RequestOptParam*, unsigned* req_out) {
    if (req_out) *req_out = g.next_req++;
    return 0;
}

} // namespace

void RegisterMatching2(Core::Loader::SymbolsResolver* sym) {
#define M2(nid, fn) LIB_FUNCTION(nid, "libSceNpMatching2", 1, "libSceNpMatching2", fn)
    M2("10t3e5+JPnU", m2_initialize);
    M2("Mqp3lJ+sjy4", m2_terminate);
    M2("YfmpW719rMo", m2_create_context);
    M2("fQQfP87I7hs", m2_register_context_cb);
    M2("7vjNQ6Z1op0", m2_context_start);
    M2("-f6M4caNe8k", m2_context_stop);
    M2("Nz-ZE7ur32I", m2_destroy_context);
    M2("LhCPctIICxQ", m2_get_server_id);
    M2("+8e7wXLmjds", m2_set_default_opt);
    M2("p+2EnxmaAMM", m2_register_room_event_cb);
    M2("0UMeWRGnZKA", m2_register_signaling_cb);
    M2("4Nj7u5B5yCA", m2_register_lobby_event_cb);
    M2("rJNPJqDCpiI", m2_get_world_info_list);
    M2("zCWZmXXN600", m2_create_join_room);
    M2("CSIMDsVjs-g", m2_join_room);
    M2("BD6kfx442Do", m2_leave_room);
    M2("tHD5FPFXtu4", m2_signaling_get_connection_status);
    M2("VqZX7POg2Mk", m2_search_room);
    M2("n5JmImxTiZU", m2_unused); // JoinLobby
    M2("BBbJ92uUdCg", m2_unused); // LeaveLobby
    M2("wyvlEgZ-55w", m2_unused); // GetLobbyInfoList
    M2("AUVfU6byg3c", m2_kickout);
    M2("NCP3bLGPt+o", m2_grant_room_owner);
    M2("S9D8JSYIrjE", m2_set_room_data_internal);
    M2("q7GK98-nYSE", m2_set_room_data_external);
    M2("HoqTrkS9c5Q", m2_set_room_member_data_internal);
    M2("wUmwXZHaX1w", m2_signaling_get_ping_info);
#undef M2
}

} // namespace BbThd
