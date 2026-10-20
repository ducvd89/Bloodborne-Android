// SPDX-License-Identifier: GPL-3.0-or-later
// The Hunter's Dream backend: sceNpSignaling over the peer table, and the NP manager, auth,
// lookup, score and WebAPI imports answered here (The Hunter's Dream has no PSN behind it).
// Adapted from droogie/bbhost src/hle/np_signaling.cpp and src/hle/system.cpp (566184a).
#include "thd_internal.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/libraries/libs.h"

namespace BbThd {
namespace {

// ---- sceNpSignaling ---------------------------------------------------------------------------------
// The game activates a connection to a peer's NpId and expects ESTABLISHED (0x01) then ACTIVE
// (0x0c); its status read wants 2 with the peer's address and port. 0x00 is dead.

constexpr int kSigErrInvalidArg = static_cast<int>(0x80550003);
constexpr std::uint16_t kEvDead = 0x00, kEvEstablished = 0x01, kEvActive = 0x0c;

struct SigCtx {
    void* cb = nullptr;
    void* arg = nullptr;
};
struct Conn {
    unsigned ctx = 0;
    std::string online_id;
    std::uint16_t member_id = 0;
    bool announced = false;
    bool dead = false;
};
std::mutex g_sig_mu;
std::map<unsigned, SigCtx> g_sig_ctx;
std::map<unsigned, Conn> g_conn;
unsigned g_next_sig_ctx = 1;
unsigned g_next_conn = 1;

void sig_fire(unsigned ctx, unsigned conn, std::uint16_t event, int error) {
    SigCtx c;
    {
        std::lock_guard lk(g_sig_mu);
        const auto it = g_sig_ctx.find(ctx);
        if (it == g_sig_ctx.end() || !it->second.cb) return;
        c = it->second;
    }
    if (Trace()) Log("np: NpSignaling event 0x%x conn %u", event, conn);
    CallGuest(c.cb, static_cast<std::int64_t>(ctx), static_cast<std::int64_t>(conn), static_cast<std::int64_t>(event),
              static_cast<std::int64_t>(error), c.arg);
}

// A connection with a known peer: ESTABLISHED at +200 ms, ACTIVE at +400.
void announce_locked(unsigned conn_id, Conn& c) {
    if (c.announced || !c.member_id) return;
    c.announced = true;
    PeersSetConn(c.member_id, conn_id, 2);
    const unsigned ctx = c.ctx;
    DispatchAfter(200, Prio::Signaling, [ctx, conn_id] { sig_fire(ctx, conn_id, kEvEstablished, 0); });
    DispatchAfter(400, Prio::Signaling, [ctx, conn_id] { sig_fire(ctx, conn_id, kEvActive, 0); });
}

s32 PS4_SYSV_ABI sig_init(u64, int, int, u64) { return 0; }
s32 PS4_SYSV_ABI sig_term() {
    std::lock_guard lk(g_sig_mu);
    g_sig_ctx.clear();
    g_conn.clear();
    return 0;
}
s32 PS4_SYSV_ABI sig_create_ctx(const void*, void* cb, void* arg, unsigned* ctx) {
    if (!ctx) return kSigErrInvalidArg;
    std::lock_guard lk(g_sig_mu);
    const unsigned id = g_next_sig_ctx++;
    g_sig_ctx[id] = SigCtx{cb, arg};
    *ctx = id;
    return 0;
}
s32 PS4_SYSV_ABI sig_delete_ctx(unsigned ctx) {
    std::lock_guard lk(g_sig_mu);
    g_sig_ctx.erase(ctx);
    for (auto it = g_conn.begin(); it != g_conn.end();) it = it->second.ctx == ctx ? g_conn.erase(it) : std::next(it);
    return 0;
}
// Idempotent per peer: the game re-activates.
s32 PS4_SYSV_ABI sig_activate(unsigned ctx, const void* npid, unsigned* conn_out) {
    if (!conn_out) return kSigErrInvalidArg;
    const std::string peer = NpIdText(npid);
    std::lock_guard lk(g_sig_mu);
    for (auto& [id, c] : g_conn) {
        if (c.ctx == ctx && c.online_id == peer && !c.dead) {
            *conn_out = id;
            if (!c.member_id) {
                Peer p;
                if (PeersFindOnline(peer, &p)) c.member_id = p.member_id;
            }
            announce_locked(id, c);
            return 0;
        }
    }
    const unsigned id = g_next_conn++;
    Conn c;
    c.ctx = ctx;
    c.online_id = peer;
    Peer p;
    if (PeersFindOnline(peer, &p)) c.member_id = p.member_id;
    std::string how;
    if (!c.member_id) {
        // The host activates its connection to the guest it summons before the guest is a room
        // member: the server resolves the address from the guest's context_start.
        std::uint32_t addr = 0;
        std::uint16_t port = 0;
        std::string error;
        if (ServerSignalingResolve(peer, &addr, &port, error)) {
            c.member_id = PeersProvisional(peer, addr, port).member_id;
            char text[64];
            std::snprintf(text, sizeof(text), " (resolved %u.%u.%u.%u:%u)", addr & 0xff, (addr >> 8) & 0xff,
                          (addr >> 16) & 0xff, addr >> 24, port);
            how = text;
        } else {
            how = " (not known and not resolved: " + error + ")";
        }
    }
    g_conn[id] = c;
    *conn_out = id;
    Log("sceNpSignalingActivateConnection %s -> conn %u%s", peer.c_str(), id, how.c_str());
    announce_locked(id, g_conn[id]);
    return 0;
}
s32 PS4_SYSV_ABI sig_deactivate(unsigned ctx, unsigned conn) {
    std::lock_guard lk(g_sig_mu);
    const auto it = g_conn.find(conn);
    if (it != g_conn.end() && it->second.ctx == ctx) {
        it->second.dead = true;
        if (it->second.member_id) PeersSetConn(it->second.member_id, conn, 0);
    }
    return 0;
}
// (ctxId, connId, status*, SceNetInAddr*, SceInPort_t*)
s32 PS4_SYSV_ABI sig_status(unsigned ctx, unsigned conn, int* st, std::uint32_t* addr, std::uint16_t* port) {
    int status = 0;
    std::uint32_t a = 0;
    std::uint16_t p = 0;
    {
        std::lock_guard lk(g_sig_mu);
        const auto it = g_conn.find(conn);
        if (it != g_conn.end() && it->second.ctx == ctx && !it->second.dead) {
            Peer peer;
            if (it->second.member_id && PeersGet(it->second.member_id, &peer) && peer.addr) {
                status = it->second.announced ? 2 : 1;
                a = peer.addr;
                p = static_cast<std::uint16_t>((peer.port << 8) | (peer.port >> 8));
            } else {
                status = 1;
            }
        }
    }
    if (st) *st = status;
    if (addr) *addr = a;
    if (port) *port = p;
    return 0;
}

// ---- NP manager: signed in as the account, no PSN behind it -----------------------------------------

constexpr int kNpErrInvalidArg = static_cast<int>(0x80550003);
std::atomic<int> g_np_req{1};

s32 PS4_SYSV_ABI np_ok() { return 0; }
s32 PS4_SYSV_ABI np_register_cb(void*, void*) { return 0; }
s32 PS4_SYSV_ABI np_get_state(int, int* state) {
    if (!state) return kNpErrInvalidArg;
    // Signed out without an account: the game plays offline rather than under a made-up name.
    *state = SignedIn() ? 2 : 1;
    return 0;
}
s32 PS4_SYSV_ABI np_online_id(int, void* id) {
    if (!id) return kNpErrInvalidArg;
    const std::string cur = OnlineId();
    std::memset(id, 0, 20);
    std::memcpy(id, cur.c_str(), std::min<std::size_t>(cur.size(), 16));
    return 0;
}
// The same handle as the online id: the session service matches peers by it.
s32 PS4_SYSV_ABI np_get_npid(int, void* id) {
    if (!id) return kNpErrInvalidArg;
    FillNpId(id, OnlineId().c_str());
    return 0;
}
s32 PS4_SYSV_ABI np_presence_status(int, int* st) {
    if (st) *st = 1;
    return 0;
}
s32 PS4_SYSV_ABI np_cmp_online(const void* a, const void* b) {
    if (!a || !b) return kNpErrInvalidArg;
    return std::memcmp(a, b, 16);
}
s32 PS4_SYSV_ABI np_cmp_npid(const void* a, const void* b) {
    if (!a || !b) return kNpErrInvalidArg;
    return std::memcmp(a, b, 36);
}
s32 PS4_SYSV_ABI np_create_request(const void*) { return g_np_req.fetch_add(1); }
s32 PS4_SYSV_ABI np_request_done(int) { return 0; }
s32 PS4_SYSV_ABI np_poll_async(int, int* result) {
    if (result) *result = 0;
    return 0; // finished
}
s32 PS4_SYSV_ABI np_check_availability(int, const void*, void*) { return 0; }
s32 PS4_SYSV_ABI np_parental(int, const void*, std::int8_t* age, std::uint8_t* info) {
    if (age) *age = 25;
    if (info) std::memset(info, 0, 3);
    return 0;
}
s32 PS4_SYSV_ABI np_check_plus(int, const void*, std::uint8_t* result) {
    if (result) result[0] = 1;
    return 0;
}
s32 PS4_SYSV_ABI np_plus_notify(int, u64) { return 0; }
// The game server binds the session to the account by X-BB-Token, not by this code.
s32 PS4_SYSV_ABI npauth_get_code(int, const void*, char* code, int* issuer) {
    if (code) {
        std::memset(code, 0, 128);
        std::strncpy(code, "DUMMY", 127);
    }
    if (issuer) *issuer = 10;
    return 0;
}

// ---- lookup ---------------------------------------------------------------------------------------------

std::atomic<int> g_lookup_id{1};
s32 PS4_SYSV_ABI lookup_title_ctx(const void*) { return g_lookup_id.fetch_add(1); }
s32 PS4_SYSV_ABI lookup_async(int, const void*) { return g_lookup_id.fetch_add(1); }
s32 PS4_SYSV_ABI lookup_npid(int, const void* online, void* npid, void*) {
    if (!online || !npid) return kNpErrInvalidArg;
    char name[17]{};
    std::memcpy(name, online, 16);
    FillNpId(npid, name);
    return 0;
}

// ---- score: no leaderboard server ------------------------------------------------------------------

constexpr int kScoreErrOffline = static_cast<int>(0x80550605u);
std::atomic<int> g_score_id{1};
s32 PS4_SYSV_ABI score_title_ctx(const void*, int) { return g_score_id.fetch_add(1); }
s32 PS4_SYSV_ABI score_create_request(int) { return g_score_id.fetch_add(1); }
s32 PS4_SYSV_ABI score_set_pc_id(int, int) { return 0; }
s32 PS4_SYSV_ABI score_offline() { return kScoreErrOffline; }
s32 PS4_SYSV_ABI score_comment_ok(int, const char*) { return 0; }

// ---- WebAPI: answered here, never sent anywhere -------------------------------------------------------

constexpr int kWebApiInvalid = static_cast<int>(0x80552902);
constexpr int kWebApiNoReq = static_cast<int>(0x80552906);
constexpr char kWebApiJsonType[] = "application/json; charset=utf-8";

struct WebApiReq {
    int status = 200;
    std::string body;
    std::size_t read_off = 0;
};
std::mutex g_web_mu;
std::atomic<int> g_web_id{1};
std::int64_t g_web_req = 1;
std::unordered_map<std::int64_t, WebApiReq> g_web_reqs;

std::string webapi_body_for_path(const char* path) {
    if (path && std::strstr(path, "friendList")) return "{\"totalResults\":0,\"friendList\":[]}";
    if (path && std::strstr(path, "blockList")) return "{\"totalResults\":0,\"blockList\":[]}";
    return "{\"totalResults\":0}";
}
s32 PS4_SYSV_ABI webapi_init(int, u64) { return g_web_id.fetch_add(1); }
s32 PS4_SYSV_ABI webapi_create_ctx(int, const void*) { return g_web_id.fetch_add(1); }
s32 PS4_SYSV_ABI webapi_create_filter(int, const void*, u64) { return g_web_id.fetch_add(1); }
s32 PS4_SYSV_ABI webapi_reg_push(int, int, void*, void*) { return g_web_id.fetch_add(1); }
s32 PS4_SYSV_ABI webapi_create_req(int, const char*, const char* path, int, const void*, std::int64_t* req_id) {
    if (!path || !req_id) return kWebApiInvalid;
    std::lock_guard lk(g_web_mu);
    const std::int64_t id = g_web_req++;
    g_web_reqs[id] = WebApiReq{200, webapi_body_for_path(path), 0};
    *req_id = id;
    return 0;
}
s32 PS4_SYSV_ABI webapi_delete_req(std::int64_t req) {
    std::lock_guard lk(g_web_mu);
    g_web_reqs.erase(req);
    return 0;
}
s32 PS4_SYSV_ABI webapi_send(std::int64_t req, const void*, u64) {
    std::lock_guard lk(g_web_mu);
    return g_web_reqs.count(req) ? 0 : kWebApiNoReq;
}
s32 PS4_SYSV_ABI webapi_status(std::int64_t req, int* code) {
    if (!code) return kWebApiInvalid;
    std::lock_guard lk(g_web_mu);
    const auto it = g_web_reqs.find(req);
    if (it == g_web_reqs.end()) return kWebApiNoReq;
    *code = it->second.status;
    return 0;
}
s32 PS4_SYSV_ABI webapi_read(std::int64_t req, void* data, u64 size) {
    std::lock_guard lk(g_web_mu);
    const auto it = g_web_reqs.find(req);
    if (it == g_web_reqs.end()) return kWebApiNoReq;
    WebApiReq& r = it->second;
    if (!data || size == 0 || r.read_off >= r.body.size()) return 0;
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(size), r.body.size() - r.read_off);
    std::memcpy(data, r.body.data() + r.read_off, n);
    r.read_off += n;
    return static_cast<int>(n);
}
bool is_content_type(const char* name) {
    return std::strcmp(name, "Content-Type") == 0 || std::strcmp(name, "content-type") == 0;
}
s32 PS4_SYSV_ABI webapi_hdr_len(std::int64_t req, const char* name, u64* len) {
    if (!name || !len) return kWebApiInvalid;
    std::lock_guard lk(g_web_mu);
    if (!g_web_reqs.count(req)) return kWebApiNoReq;
    if (!is_content_type(name)) return kWebApiInvalid;
    *len = sizeof(kWebApiJsonType);
    return 0;
}
s32 PS4_SYSV_ABI webapi_hdr_val(std::int64_t req, const char* name, char* buf, u64 n) {
    if (!name || !buf || n == 0) return kWebApiInvalid;
    std::lock_guard lk(g_web_mu);
    if (!g_web_reqs.count(req)) return kWebApiNoReq;
    if (!is_content_type(name)) return kWebApiInvalid;
    std::snprintf(buf, static_cast<std::size_t>(n), "%s", kWebApiJsonType);
    return 0;
}
s32 PS4_SYSV_ABI webapi_parse_npid(const char* json, void* npid) {
    if (!json || !npid) return kWebApiInvalid;
    const char* s = json;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
    char online[17]{};
    auto take_quoted = [&](const char* p) {
        const char* q = std::strchr(p, '"');
        if (!q) return false;
        ++q;
        const char* e = std::strchr(q, '"');
        if (!e) return false;
        const std::size_t n = static_cast<std::size_t>(e - q);
        if (n == 0 || n >= sizeof(online)) return false;
        std::memcpy(online, q, n);
        return true;
    };
    if (*s == '{') {
        const char* key = std::strstr(s, "onlineId");
        if (!key) key = std::strstr(s, "\"data\"");
        // The value after the key's colon.
        const char* colon = key ? std::strchr(key, ':') : nullptr;
        if (!colon || !take_quoted(colon)) return kWebApiInvalid;
    } else if (*s == '"') {
        if (!take_quoted(s)) return kWebApiInvalid;
    } else {
        std::snprintf(online, sizeof(online), "%s", s);
    }
    FillNpId(npid, online);
    return 0;
}

} // namespace

void SignalingPeerKnown(std::uint16_t member_id) {
    Peer p;
    if (!PeersGet(member_id, &p)) return;
    std::lock_guard lk(g_sig_mu);
    for (auto& [id, c] : g_conn) {
        if (!c.dead && c.online_id == p.online_id) {
            c.member_id = member_id;
            announce_locked(id, c);
        }
    }
}

void SignalingPeerDead(const std::string& online_id) {
    std::vector<std::pair<unsigned, unsigned>> dead;
    {
        std::lock_guard lk(g_sig_mu);
        for (auto& [id, c] : g_conn) {
            if (!c.dead && c.online_id == online_id) {
                c.dead = true;
                dead.emplace_back(c.ctx, id);
            }
        }
    }
    for (const auto& [ctx, conn] : dead) {
        DispatchAfter(100, Prio::Signaling, [ctx = ctx, conn = conn] { sig_fire(ctx, conn, kEvDead, 0); });
    }
}

void SignalingReset() {
    std::lock_guard lk(g_sig_mu);
    for (auto& [id, c] : g_conn) c.dead = true;
}

void RegisterNp(Core::Loader::SymbolsResolver* sym) {
#define SIG(nid, fn) LIB_FUNCTION(nid, "libSceNpSignaling", 1, "libSceNpSignaling", fn)
    SIG("3KOuC4RmZZU", sig_init);
    SIG("NPhw0UXaNrk", sig_term);
    SIG("5yYjEdd4t8Y", sig_create_ctx);
    SIG("hx+LIg-1koI", sig_delete_ctx);
    SIG("0UvTFeomAUM", sig_activate);
    SIG("6UEembipgrM", sig_deactivate);
    SIG("bD-JizUb3JM", sig_status);
#undef SIG
#define NP(nid, fn) LIB_FUNCTION(nid, "libSceNpManager", 1, "libSceNpManager", fn)
    NP("VfRSmPmj8Q8", np_register_cb);        // sceNpRegisterStateCallback
    NP("mjjTXh+NHWY", np_ok);                 // sceNpUnregisterStateCallback
    NP("uFJpaKNBAj4", np_register_cb);        // sceNpRegisterGamePresenceCallback
    NP("GImICnh+boA", np_register_cb);        // sceNpRegisterPlusEventCallback
    NP("xViqJdDgKl0", np_ok);                 // sceNpUnregisterPlusEventCallback
    NP("3Zl8BePTh9Y", np_ok);                 // sceNpCheckCallback
    NP("Ec63y59l9tw", np_ok);                 // sceNpSetNpTitleId
    NP("A2CQ3kgSopQ", np_ok);                 // sceNpSetContentRestriction
    NP("Gaxrp3EWY-M", np_plus_notify);        // sceNpNotifyPlusFeature
    NP("eQH7nWPcAgc", np_get_state);          // sceNpGetState
    NP("XDncXQIJUSk", np_online_id);          // sceNpGetOnlineId
    NP("p-o74CnoNzY", np_get_npid);           // sceNpGetNpId
    NP("IPb1hd1wAGc", np_presence_status);    // sceNpGetGamePresenceStatus
    NP("eiqMCt9UshI", np_create_request);     // sceNpCreateAsyncRequest
    NP("S7QTn72PrDw", np_request_done);       // sceNpDeleteRequest
    NP("OzKvTvg3ZYU", np_request_done);       // sceNpAbortRequest
    NP("uqcPJLWL08M", np_poll_async);         // sceNpPollAsync
    NP("2rsFmlGWleQ", np_check_availability); // sceNpCheckNpAvailability
    NP("ilwLM4zOmu4", np_parental);           // sceNpGetParentalControlInfo
    NP("r6MyYJkryz8", np_check_plus);         // sceNpCheckPlus
#undef NP
    LIB_FUNCTION("i8UmXTSq7N4", "libSceNpCommon", 1, "libSceNpCommon", np_cmp_npid);
    LIB_FUNCTION("dj+O5aD2a0Q", "libSceNpCommon", 1, "libSceNpCommon", np_cmp_online);
#define AUTH(nid, fn) LIB_FUNCTION(nid, "libSceNpAuth", 1, "libSceNpAuth", fn)
    AUTH("N+mr7GjTvr8", np_create_request);
    AUTH("H8wG9Bk-nPc", np_request_done);
    AUTH("gjSyfzSsDcE", np_poll_async);
    AUTH("KxGkOrQJTqY", npauth_get_code);
#undef AUTH
#define LOOKUP(nid, fn) LIB_FUNCTION(nid, "libSceNpManager", 1, "libSceNpManager", fn)
    LOOKUP("8533Q+LU7EQ", lookup_title_ctx);
    LOOKUP("mtqDK9zkoIE", np_request_done);
    LOOKUP("JA4+sS39GMs", lookup_async);
    LOOKUP("wLaxchvEEnk", np_request_done);
    LOOKUP("eYz4v5Uek9U", np_request_done);
    LOOKUP("V4EVrruHuy8", np_poll_async);
    LOOKUP("T6tnM1Uti4g", lookup_npid);
#undef LOOKUP
#define SCORE(nid, fn) LIB_FUNCTION(nid, "libSceNpScore", 1, "libSceNpScore", fn)
    SCORE("KnNA1TEgtBI", score_title_ctx);
    SCORE("G0pE+RNCwfk", np_request_done);
    SCORE("gW8qyjYrUbk", score_create_request);
    SCORE("dK8-SgYf6r4", np_request_done);
    SCORE("1i7kmKbX6hk", np_request_done);
    SCORE("bygbKdHmjn4", score_set_pc_id);
    SCORE("ETS-uM-vH9Q", score_offline); // GetRankingByNpIdPcId
    SCORE("KBHxDjyk-jA", score_offline); // GetRankingByRange
    SCORE("zKoVok6FFEI", score_offline); // GetGameData
    SCORE("zT0XBtgtOSI", score_offline); // RecordScore
    SCORE("LoVMVrijVOk", score_offline); // GetBoardInfo
    SCORE("bcoVwcBjQ9E", score_offline); // RecordGameData
    SCORE("2b3TI0mDYiI", score_comment_ok);
    SCORE("r4oAo9in0TA", score_comment_ok);
#undef SCORE
#define WEB(nid, fn) LIB_FUNCTION(nid, "libSceNpWebApi", 1, "libSceNpWebApi", fn)
    WEB("G3AnLNdRBjE", webapi_init);
    WEB("asz3TtIqGF8", np_request_done);      // Terminate
    WEB("x1Y7yiYSk7c", webapi_create_ctx);
    WEB("XUjdsSTTZ3U", np_request_done);      // DeleteContext
    WEB("y5Ta5JCzQHY", webapi_create_filter);
    WEB("zE+R6Rcx3W0", np_ok);                // DeletePushEventFilter
    WEB("PfSTDCgNMgc", webapi_reg_push);
    WEB("qK4o2656W4w", np_ok);                // UnregisterPushEventCallback
    WEB("rdgs5Z1MyFw", webapi_create_req);
    WEB("noQgleu+KLE", webapi_delete_req);
    WEB("JzhYTP2fG18", webapi_delete_req);    // AbortRequest
    WEB("kVbL4hL3K7w", webapi_send);
    WEB("k210oKgP80Y", webapi_status);
    WEB("CQtPRSF6Ds8", webapi_read);
    WEB("743ZzEBzlV8", webapi_hdr_len);
    WEB("VwJ5L0Higg0", webapi_hdr_val);
    WEB("or0e885BlXo", webapi_parse_npid);
#undef WEB
}

} // namespace BbThd
