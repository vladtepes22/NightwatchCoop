// SPDX-License-Identifier: GPL-3.0-or-later
// derived from droogie/bbhost src/hle/np_matching2.cpp @8f2746c
//
// sceNpMatching2 over the session layer (np_session.h) and the party transport.
//
// The game takes three values from this library - connection status 2, the peer's address,
// the peer's port - and its own session state machines do the rest, driven by the callbacks
// below. The layouts are the SDK's; what the game reads out of each callback was read from the
// eboot's own dispatch (bbhost: sub_10c3210 - the roomId at RoomDataInternal+0x18, the member
// list, the owner's member id; sub_10c3da0 - MEMBER_JOINED 0x1101 with the member record at
// data[0]). Timings and order are the co-op contract: CONTEXT_STARTED +200 ms, the request
// answers +300 ms, signaling ESTABLISHED +800 ms after a create (+500 after a join, +800 after
// a member joined), LeaveRoom's 0x103 +300 ms.
//
// bbport: guest callbacks go through the A1 dispatcher (bbnet::post_guest_call) - ordered, on
// a thread attached to the runtime; the server is the party host (PartyTransport); plugins,
// rulesets and accounts are gone; the host's own type-1 invite item stays the game's (the
// invite hook is A5's). Kept from bbhost's 2026-10-06 fixes: no auto-leave of the room when
// peers leave, no seeding of the host's address from a loopback invite, room_closed /
// kicked / heartbeat "not in room" end the room for the game, the STUN keepalive.
#include "bbnet_internal.h"
#include "np_hle.h"
#include "np_session.h"
#include "party_transport.h"
#include "party_util.h"

#include <atomic>
#include <chrono>
#include "../../party_env.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace bbnet::np {

namespace {

using party::int_of;
using party::str_of;

// ---- SDK layouts (the game's build of libSceNpMatching2) ---------------------------------------

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
static_assert(offsetof(RoomDataInternal, roomId) == 0x18);
static_assert(sizeof(RoomDataInternal) == 0x58);

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
static_assert(offsetof(RoomMemberDataInternal, memberId) == 0x38);
static_assert(sizeof(RoomMemberDataInternal) == 0x58);

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
    std::uint32_t worldId;
    std::uint32_t numOfLobby;
    std::uint32_t curNumOfTotalLobby;
    std::uint32_t maxNumOfTotalLobby;
    std::uint32_t curNumOfRoom;
    std::uint32_t maxNumOfRoom;
    std::uint32_t curNumOfTotalRoomMember;
    std::uint32_t numOfRoom;
};
static_assert(sizeof(World) == 0x28);  // the game walks the list at this stride

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

// RoomUpdateInfo, the data of KICKEDOUT and ROOM_DESTROYED: the game reads optData only when it
// is 4 bytes long - the reason the kicking host passed.
struct RoomUpdateInfo {
    std::uint8_t eventCause;
    std::uint8_t pad[3];
    std::int32_t errorCode;
    std::uint8_t optData[16];
    std::uint64_t optDataLen;
};
static_assert(offsetof(RoomUpdateInfo, optData) == 8 && offsetof(RoomUpdateInfo, optDataLen) == 0x18);

// KickoutRoomMemberRequest: roomId, the member, blockKickFlag, optData.
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
    std::uint32_t rtt;  // microseconds
    std::uint32_t pad2;
};

// Events (SceNpMatching2Event).
constexpr std::uint16_t kEvGetWorldInfoList = 0x0002;
constexpr std::uint16_t kEvSetRoomDataExternal = 0x0004;
constexpr std::uint16_t kEvCreateJoinRoom = 0x0101;
constexpr std::uint16_t kEvJoinRoom = 0x0102;
constexpr std::uint16_t kEvLeaveRoom = 0x0103;
constexpr std::uint16_t kEvKickoutRoomMember = 0x0104;
constexpr std::uint16_t kEvGrantRoomOwner = 0x0105;
constexpr std::uint16_t kEvSearchRoom = 0x0106;
constexpr std::uint16_t kEvSetRoomDataInternal = 0x0109;
constexpr std::uint16_t kEvSetRoomMemberDataInternal = 0x010B;
constexpr std::uint16_t kEvSignalingGetPingInfo = 0x0E01;
constexpr std::uint16_t kEvMemberJoined = 0x1101;
constexpr std::uint16_t kEvMemberLeft = 0x1102;
constexpr std::uint16_t kEvKickedOut = 0x1103;
constexpr std::uint16_t kEvRoomDestroyed = 0x1104;
constexpr std::uint16_t kEvSignalingDead = 0x5101;
constexpr std::uint16_t kEvSignalingEstablished = 0x5102;
constexpr std::uint16_t kEvContextStarted = 0x6F02;

// SceNpMatching2EventCause.
constexpr std::uint8_t kCauseLeave = 1, kCauseKickout = 2, kCauseServerOperation = 4, kCauseMemberDisappeared = 5;

constexpr int kErrInvalidArg = static_cast<int>(0x80550C03);
constexpr int kErrContext = static_cast<int>(0x80550C06);
constexpr int kErrServerNotAvailable = static_cast<int>(0x80550C28);
constexpr int kErrRequestFailed = static_cast<int>(0x80550C0B);
// The signaling DEAD event's error: the peer left, or we did.
constexpr int kErrTerminatedByPeer = static_cast<int>(0x80550E10);
constexpr int kErrTerminatedByMyself = static_cast<int>(0x80550E18);

// ---- state --------------------------------------------------------------------------------------

// A response the game reads during its callback. The callbacks run on the A1 dispatcher a
// little after they are posted, so the last few dozen stay alive.
struct Held {
    std::vector<std::uint8_t> bytes;
};
constexpr std::size_t kHeldKeep = 64;

struct M2 {
    std::mutex mu;
    unsigned ctx_id = 0;
    bool started = false;
    void* ctx_cb = nullptr;
    void* ctx_arg = nullptr;
    void* room_cb = nullptr;
    void* room_arg = nullptr;
    void* sig_cb = nullptr;
    void* sig_arg = nullptr;
    void* lobby_cb = nullptr;
    void* lobby_arg = nullptr;
    RequestOptParam dflt{};
    std::atomic<unsigned> next_req{1};
    // the room
    std::string session_id;
    std::uint64_t room_id = 0;
    std::uint16_t member_id = 0;
    std::uint16_t owner_id = 0;
    std::uint16_t max_slot = 0;
    bool is_host = false;
    bool in_room = false;
    // The room a JoinRoom is on its way into: its member events can come in before the answer.
    std::uint64_t joining_room = 0;
    std::int64_t room_since_ms = 0;
    // LeaveRoom, answered once the session ends
    bool leave_pending = false;
    unsigned leave_req = 0;
    RequestOptParam leave_opt{};
    std::vector<std::unique_ptr<Held>> held;
    std::string npid;  // our handle
    bool heartbeat_started = false;
};
M2 g;
// F5: when a member last left our room (steady ms; 0 = never).
std::atomic<std::int64_t> g_member_left_ms{0};
std::atomic<HostClosedRoomHook> g_host_closed_room_hook{nullptr};
std::int64_t steady_ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool trace() { return party_trace(); }

// BB_PARTY_FAST_JOIN (default on; docs/party/join_timing.md): the SDK-like latency of the room
// request answers and the signaling ESTABLISHED events shrinks (same order, shorter gaps).
int join_ms(int normal, int fast) {
    static const bool on = bb_party_switch_on("BB_PARTY_FAST_JOIN") != 0;
    return on ? fast : normal;
}

void keep(std::unique_ptr<Held> h) {
    g.held.push_back(std::move(h));
    if (g.held.size() > kHeldKeep) g.held.erase(g.held.begin());
}

std::uint64_t u(std::int64_t v) { return static_cast<std::uint64_t>(v); }
std::uint64_t p(const void* v) { return reinterpret_cast<std::uint64_t>(v); }

// A guest callback, built under g.mu and posted once it is released (the game's handlers take
// their own locks and call back into sceNpMatching2).
struct M2Call {
    void* fn = nullptr;
    std::uint64_t a[6] = {};
    void run() const {
        if (fn) post_guest_call(reinterpret_cast<std::uintptr_t>(fn), a[0], a[1], a[2], a[3], a[4], a[5]);
    }
};

// (ctxId, reqId, event, errorCode, data, arg)
void fire_request(void* fn, void* arg, unsigned req, std::uint16_t event, int error, void* data) {
    if (!fn) return;
    if (trace() || event != kEvGetWorldInfoList) {
        log("np: REQUEST_CB event 0x%x req %u error 0x%x", event, req, static_cast<unsigned>(error));
    }
    post_guest_call(reinterpret_cast<std::uintptr_t>(fn), g.ctx_id, req, event, u(error), p(data), p(arg));
}
M2Call request_call(void* fn, void* arg, unsigned req, std::uint16_t event, int error, void* data) {
    if (!fn) return {};
    if (trace() || event != kEvGetWorldInfoList) {
        log("np: REQUEST_CB event 0x%x req %u error 0x%x", event, req, static_cast<unsigned>(error));
    }
    return {fn, {g.ctx_id, req, event, u(error), p(data), p(arg)}};
}
// (ctxId, event, eventCause, errorCode, arg)
M2Call context_call(std::uint16_t event, int cause, int error) {
    if (!g.ctx_cb) return {};
    log("np: CONTEXT_CB event 0x%x", event);
    return {g.ctx_cb, {g.ctx_id, event, u(cause), u(error), p(g.ctx_arg), 0}};
}
// (ctxId, roomId, event, data, arg)
M2Call room_event_call(std::uint16_t event, void* data, std::uint64_t room_id) {
    if (!g.room_cb) return {};
    log("np: ROOM_EVENT_CB event 0x%x room %llu", event, static_cast<unsigned long long>(room_id));
    return {g.room_cb, {g.ctx_id, room_id, event, p(data), p(g.room_arg), 0}};
}
M2Call room_event_call(std::uint16_t event, void* data) { return room_event_call(event, data, g.room_id); }
// (ctxId, roomId, peerMemberId, event, errorCode, arg)
void fire_signaling(std::uint16_t member, std::uint16_t event, int error, std::uint64_t room_id) {
    void* fn;
    void* arg;
    unsigned ctx;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        fn = g.sig_cb;
        arg = g.sig_arg;
        ctx = g.ctx_id;
    }
    if (!fn) return;
    if (trace()) log("np: SIGNALING_CB event 0x%x member %u", event, member);
    post_guest_call(reinterpret_cast<std::uintptr_t>(fn), ctx, room_id, member, event, u(error), p(arg));
}

// The room's real members (provisional 0xff00+ entries are the same people, resolved before
// they joined).
std::vector<session::Peer> room_peers() {
    std::vector<session::Peer> v;
    for (const session::Peer& peer : session::peers_all()) {
        if (peer.member_id < 0xff00) v.push_back(peer);
    }
    return v;
}

// The ESTABLISHED event for every member the game must see before signalingPoll runs, self
// included.
void schedule_established_for_all(int delay_ms) {
    std::vector<std::uint16_t> ids;
    for (const session::Peer& peer : room_peers()) ids.push_back(peer.member_id);
    ids.push_back(g.member_id);
    const std::uint64_t room = g.room_id;
    session::dispatch_after(delay_ms, session::Prio::Signaling, [ids, room] {
        for (std::uint16_t m : ids) fire_signaling(m, kEvSignalingEstablished, 0, room);
    });
}

// Members gone for good: the signaling DEAD for each first, then the room event (DEAD 0x5101
// before MEMBER_LEFT, as the SDK delivers them).
void schedule_dead(const std::vector<std::uint16_t>& ids, int error, std::uint64_t room_id, int delay_ms) {
    if (ids.empty()) return;
    session::dispatch_after(delay_ms, session::Prio::Signaling, [ids, error, room_id] {
        for (std::uint16_t m : ids) fire_signaling(m, kEvSignalingDead, error, room_id);
    });
}

// A RoomDataInternal + member list (self + every peer) in one Held.
CreateJoinRoomResponse* build_room_response(std::uint64_t room_id, std::uint16_t self_id, std::uint16_t owner_id,
                                            std::uint16_t max_slot) {
    const std::vector<session::Peer> peers = room_peers();
    const std::size_t n = peers.size() + 1;
    auto held = std::make_unique<Held>();
    held->bytes.assign(sizeof(CreateJoinRoomResponse) + sizeof(RoomDataInternal) + n * sizeof(RoomMemberDataInternal),
                       0);
    auto* resp = reinterpret_cast<CreateJoinRoomResponse*>(held->bytes.data());
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
        fill_npid(m.npId, online.c_str());
        m.memberId = member_id;
        m.natType = 1;
        if (member_id && member_id <= 64) joined |= 1ull << (member_id - 1);
        if (member_id == self_id) resp->members.me = &m;
        if (member_id == owner_id) {
            resp->members.owner = &m;
            m.flagAttr |= 0x80000000u;  // SCE_NP_MATCHING2_ROOMMEMBER_FLAG_ATTR_OWNER
        }
        ++i;
    };
    put(self_id, session::online_id());
    for (const session::Peer& peer : peers) put(peer.member_id, peer.online_id);
    room->joinedSlotMask = joined;
    resp->roomData = room;
    resp->members.members = members;
    resp->members.membersNum = n;
    if (!resp->members.owner) resp->members.owner = resp->members.me;
    keep(std::move(held));
    return resp;
}

RoomMemberUpdateInfo* build_member_update(const session::Peer& peer, std::uint8_t cause) {
    auto held = std::make_unique<Held>();
    held->bytes.assign(sizeof(RoomMemberUpdateInfo) + sizeof(RoomMemberDataInternal), 0);
    auto* info = reinterpret_cast<RoomMemberUpdateInfo*>(held->bytes.data());
    auto* m = reinterpret_cast<RoomMemberDataInternal*>(info + 1);
    fill_npid(m->npId, peer.online_id.c_str());
    m->memberId = peer.member_id;
    m->natType = 1;
    if (peer.member_id == g.owner_id) m->flagAttr |= 0x80000000u;
    info->roomMemberDataInternal = m;
    info->eventCause = cause;
    keep(std::move(held));
    return info;
}

RoomUpdateInfo* build_room_update(std::uint8_t cause, const std::uint8_t* opt, std::size_t opt_len) {
    auto held = std::make_unique<Held>();
    held->bytes.assign(sizeof(RoomUpdateInfo), 0);
    auto* info = reinterpret_cast<RoomUpdateInfo*>(held->bytes.data());
    info->eventCause = cause;
    opt_len = opt_len > sizeof(info->optData) ? sizeof(info->optData) : opt_len;
    if (opt && opt_len) std::memcpy(info->optData, opt, opt_len);
    info->optDataLen = opt_len;
    keep(std::move(held));
    return info;
}

RequestOptParam opt_or_default(const RequestOptParam* opt) { return opt && opt->cbFunc ? *opt : g.dflt; }

void forget_room_locked() {
    g.in_room = false;
    g.session_id.clear();
    g.room_id = 0;
    g.owner_id = 0;
    g.max_slot = 0;
    session::peers_clear();
    session::room_clear();
    signaling_reset();
}

// Tells the host's service we left its room. F5: a leave the link could not carry (our game ended
// the session during a network outage) is retried until the link is back (every 2 s, up to 2 min):
// otherwise the host's room keeps us until its heartbeat timeout, and its game our old connection.
void leave_room_retry(const std::string& sid, int mid, int attempt) {
    session::dispatch_after(attempt ? 2000 : 0, session::Prio::Request, [sid, mid, attempt] {
        json::Value body = json::Value::make_object();
        body.set("SessionId", sid);
        body.set("MemberId", mid);
        json::Value reply;
        std::string err;
        // rpc() false: the host was not reached (the link is down); any answer ends it.
        if (session::transport().rpc(party::call::kLeaveRoom, body, reply, err, 4000)) {
            if (attempt) log("np: leave_room delivered after %d retries (the link is back)", attempt);
            else if (int_of(reply, "ResKind", 0) != 0) log("np: leave_room: %s", json::dump(reply, 0).c_str());
            return;
        }
        if (attempt == 0) log("np: leave_room: %s (retried until the host is reached)", err.c_str());
        if (attempt < 60) leave_room_retry(sid, mid, attempt + 1);
        else log("np: leave_room: the host was not reached in 2 min; its heartbeat timeout ends our membership");
    });
}

// Our LeaveRoom (or the context going away): out of the host's room, DEAD for every member,
// then 0x103 at +300 ms.
void end_session_locked(const char* why) {
    if (!g.in_room && !g.leave_pending) return;
    log("np: session over (%s): room %llu", why, static_cast<unsigned long long>(g.room_id));
    if (!g.session_id.empty()) leave_room_retry(g.session_id, g.member_id, 0);
    std::vector<std::uint16_t> ids;
    for (const session::Peer& peer : room_peers()) ids.push_back(peer.member_id);
    schedule_dead(ids, kErrTerminatedByMyself, g.room_id, 0);
    if (g.leave_pending) {
        const unsigned req = g.leave_req;
        const RequestOptParam opt = g.leave_opt;
        g.leave_pending = false;
        session::dispatch_after(300, session::Prio::Request,
                                [req, opt] { fire_request(opt.cbFunc, opt.cbFuncArg, req, kEvLeaveRoom, 0, nullptr); });
    }
    forget_room_locked();
}

// The host ended our membership (it left or timed out, we were kicked, it lost the room): the
// game still holds the room until it hears so - KICKEDOUT or ROOM_DESTROYED, after a DEAD per
// member.
void room_gone_locked(std::uint16_t event, std::uint8_t cause, const std::uint8_t* opt, std::size_t opt_len,
                      const std::string& why) {
    if (!g.in_room) return;
    const std::uint64_t room = g.room_id;
    log("np: room %llu gone (%s): the game hears 0x%x", static_cast<unsigned long long>(room), why.c_str(), event);
    std::vector<std::uint16_t> ids;
    for (const session::Peer& peer : room_peers()) {
        ids.push_back(peer.member_id);
        signaling_peer_dead(peer.online_id);
    }
    schedule_dead(ids, kErrTerminatedByPeer, room, 0);
    RoomUpdateInfo* info = build_room_update(cause, opt, opt_len);
    session::dispatch_after(100, session::Prio::RoomEvent, [event, info, room] {
        M2Call c;
        {
            std::lock_guard<std::mutex> lk2(g.mu);
            c = room_event_call(event, info, room);
        }
        c.run();
    });
    forget_room_locked();
}

// The host drops a member whose heartbeat is 15 s old; one every 5 s while we are in a room.
// A host that answers we are not in the room any more ends the room for the game.
void heartbeat_loop() {
    std::string sid;
    int mid = 0;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        if (g.in_room && !g.session_id.empty()) {
            sid = g.session_id;
            mid = g.member_id;
        }
    }
    if (!sid.empty()) {
        const int r = session::server_heartbeat(sid, mid);
        if (r < 0 && trace()) log("np: heartbeat not acknowledged");
        if (r == 0) {
            std::lock_guard<std::mutex> lk(g.mu);
            if (g.in_room && g.session_id == sid) {
                room_gone_locked(kEvRoomDestroyed, kCauseServerOperation, nullptr, 0, "the host has no room for us");
            }
        }
    }
    session::dispatch_after(5000, session::Prio::Request, heartbeat_loop);
}

bool is_loopback(std::uint32_t addr_nbo) { return (addr_nbo & 0xff) == 127; }

// The host's answers and events are peer input (bbport security pass): a room's size and member
// ids as the game's room structures can hold them, finite positions.
std::uint16_t clamp_slots(long long v, std::uint16_t def) {
    if (v < 2) return def;
    return static_cast<std::uint16_t>(v > 8 ? 8 : v);
}
std::uint16_t clamp_member(long long v, std::uint16_t def) {
    return v > 0 && v <= 64 ? static_cast<std::uint16_t>(v) : def;
}
float clamp_pos(const json::Value& v) {
    if (v.type != json::Value::Type::Number) return 0.0f;
    const double d = v.number;
    return d > -100000.0 && d < 100000.0 ? static_cast<float>(d) : 0.0f;
}

// ---- events from the host ------------------------------------------------------------------------

void on_server_event(const json::Value& ev);

// An event about the room a JoinRoom is still answering for: handled again once the answer is
// in (the event stream can be quicker than the join's reply).
bool defer_for_join_locked(const json::Value& ev) {
    const auto room = static_cast<std::uint64_t>(int_of(ev, "RoomId", 0));
    if (g.in_room || !room || room != g.joining_room) return false;
    const int tries = static_cast<int>(int_of(ev, "_Deferred", 0));
    if (tries >= 20) return false;
    json::Value again = ev;
    again.set("_Deferred", tries + 1);
    session::dispatch_after(250, session::Prio::RoomEvent, [again] { on_server_event(again); });
    return true;
}

void on_server_event(const json::Value& ev) {
    const std::string name = str_of(ev, "Name");
    if (name == "guest_invite") {
        std::lock_guard<std::mutex> lk(g.mu);
        const auto room = static_cast<std::uint64_t>(int_of(ev, "RoomId", 0));
        session::Peer host;
        host.member_id = 1;
        host.online_id = str_of(ev, "HostOnlineId");
        // The host's addresses: the peer table takes them now so JoinRoom's answer only
        // confirms, and the punch opens our NAT toward the host before its first datagram. A
        // loopback address (an older host's self-report) points us at ourselves: such an
        // invite seeds nothing, and the host is resolved when the game asks for it.
        json::Value one = json::Value::make_array();
        json::Value rec = json::Value::make_object();
        rec.set("MemberId", 1);
        rec.set("OnlineId", host.online_id);
        rec.set("Addr", str_of(ev, "HostAddr"));
        rec.set("Port", int_of(ev, "HostPort", 0));
        rec.set("LocalAddr", str_of(ev, "HostLocalAddr"));
        rec.set("LocalPort", int_of(ev, "HostLocalPort", 0));
        rec.set("MappedAddr", str_of(ev, "HostMappedAddr"));
        rec.set("MappedPort", int_of(ev, "HostMappedPort", 0));
        one.push(rec);
        session::Peer before;
        const bool had = session::peers_get(1, &before);
        session::peers_from_members(one);
        session::Peer seeded;
        if (session::peers_get(1, &seeded) && is_loopback(seeded.addr) && !session::transport().host_is_local()) {
            if (had) session::peers_upsert(before);
            else session::peers_erase(1);
            log("np: guest_invite: the host's address is loopback; not seeded");
        }
        session::SummonInvite inv{};
        inv.room_id = room;
        inv.member_tag = static_cast<std::uint32_t>(int_of(ev, "MemberTag", 0));
        inv.host_area = static_cast<std::uint32_t>(int_of(ev, "HostArea", 0));
        inv.host_level = static_cast<std::uint32_t>(int_of(ev, "HostLevel", 0));
        const json::Value* pos = ev.find("HostPos");
        if (pos && pos->type == json::Value::Type::Array) {
            for (std::size_t i = 0; i < 3 && i < pos->array.size(); ++i) {
                inv.host_pos[i] = clamp_pos(pos->array[i]);
            }
        }
        inv.host_online_id = host.online_id;
        log("np: guest_invite: room %llu from %s (%s:%lld)", static_cast<unsigned long long>(room),
            host.online_id.c_str(), str_of(ev, "HostAddr").c_str(), int_of(ev, "HostPort", 0));
        session::dispatch_after(0, session::Prio::RoomEvent, [inv] { session::deliver_invite(inv); });
        return;
    }
    if (name == "room_member_joined") {
        std::lock_guard<std::mutex> lk(g.mu);
        const auto mid = static_cast<std::uint16_t>(int_of(ev, "MemberId", 0));
        if (defer_for_join_locked(ev)) return;
        if (!g.in_room || !mid || mid == g.member_id) return;
        if (static_cast<std::uint64_t>(int_of(ev, "RoomId", 0)) != g.room_id) return;
        // A member already known (an address refresh): the address is news, the member is not -
        // a second MEMBER_JOINED would make the game build it twice.
        session::Peer known;
        const bool already = session::peers_get(mid, &known) && known.online_id == str_of(ev, "OnlineId");
        json::Value one = json::Value::make_array();
        one.push(ev);
        if (session::peers_from_members(one) == 0) return;
        session::Peer peer;
        if (!session::peers_get(mid, &peer)) return;
        signaling_peer_known(peer.member_id);
        if (already) {
            if (trace()) log("np: member %u (%s) already known; address refreshed", mid, peer.online_id.c_str());
            return;
        }
        log("np: member %u (%s) joined room %llu", peer.member_id, peer.online_id.c_str(),
            static_cast<unsigned long long>(g.room_id));
        session::dispatch_after(join_ms(300, 100), session::Prio::RoomEvent, [peer] {
            M2Call c;
            {
                std::lock_guard<std::mutex> lk2(g.mu);
                c = room_event_call(kEvMemberJoined, build_member_update(peer, 0));
            }
            c.run();
        });
        const std::uint64_t room = g.room_id;
        session::dispatch_after(join_ms(800, 300), session::Prio::Signaling, [mid, room] {
            std::uint16_t self;
            {
                std::lock_guard<std::mutex> lk2(g.mu);
                self = g.member_id;
            }
            fire_signaling(mid, kEvSignalingEstablished, 0, room);
            fire_signaling(self, kEvSignalingEstablished, 0, room);
        });
        return;
    }
    if (name == "room_member_left") {
        std::lock_guard<std::mutex> lk(g.mu);
        const auto mid = static_cast<std::uint16_t>(int_of(ev, "MemberId", 0));
        if (defer_for_join_locked(ev)) return;
        if (static_cast<std::uint64_t>(int_of(ev, "RoomId", 0)) != g.room_id) return;
        session::Peer peer;
        if (!session::peers_get(mid, &peer)) return;
        const std::string reason = str_of(ev, "Reason");
        const std::uint8_t cause = reason == "kicked"                           ? kCauseKickout
                                   : (reason == "leave_room" || reason.empty()) ? kCauseLeave
                                                                                : kCauseMemberDisappeared;
        log("np: member %u (%s) left (%s)", mid, peer.online_id.c_str(), reason.c_str());
        g_member_left_ms = steady_ms_now();
        // DEAD for that member, then MEMBER_LEFT; the room stays ours (no auto-leave: with three
        // in a room one leaving must not take the third out with it - bbhost 2026-10-05).
        schedule_dead({mid}, kErrTerminatedByPeer, g.room_id, 0);
        session::dispatch_after(100, session::Prio::RoomEvent, [peer, cause] {
            M2Call c;
            {
                std::lock_guard<std::mutex> lk2(g.mu);
                c = room_event_call(kEvMemberLeft, build_member_update(peer, cause));
            }
            c.run();
        });
        signaling_peer_dead(peer.online_id);
        session::peers_erase(mid);
        // Its provisional entry (resolved before it joined) goes too: a return resolves it afresh.
        for (const session::Peer& q : session::peers_all()) {
            if (q.member_id >= 0xff00 && q.online_id == peer.online_id) session::peers_erase(q.member_id);
        }
        return;
    }
    if (name == "peer_deactivated") {
        std::string who = str_of(ev, "OnlineId");
        if (who.empty()) who = str_of(ev, "PeerOnlineId");
        if (!who.empty()) signaling_peer_dead(who);
        return;
    }
    if (name == "room_closed") {
        std::unique_lock<std::mutex> lk(g.mu);
        const auto room = static_cast<std::uint64_t>(int_of(ev, "RoomId", 0));
        if (room && room != g.room_id) return;
        const std::string reason = str_of(ev, "Reason");
        const HostClosedRoomHook hook = g_host_closed_room_hook.load();
        if (reason == "host_left" && hook && g.in_room && !g.is_host) {
            log("np: room %llu closed by its host (it left): the party sends this guest home",
                static_cast<unsigned long long>(g.room_id));
            lk.unlock();
            hook("the host left its room");
            return;
        }
        room_gone_locked(kEvRoomDestroyed, reason == "host_left" ? kCauseLeave : kCauseServerOperation, nullptr, 0,
                         "room closed: " + reason);
        return;
    }
    if (name == "room_member_kicked") {
        std::lock_guard<std::mutex> lk(g.mu);
        const auto room = static_cast<std::uint64_t>(int_of(ev, "RoomId", 0));
        const auto mid = static_cast<std::uint16_t>(int_of(ev, "MemberId", 0));
        if (room != g.room_id || mid != g.member_id) return;
        const std::vector<std::uint8_t> opt = party::b64_decode(str_of(ev, "OptData"));
        room_gone_locked(kEvKickedOut, kCauseKickout, opt.data(), opt.size(), "kicked by the host");
        return;
    }
    if (trace()) log("np: event %s ignored", name.c_str());
}

// ---- the imports ----------------------------------------------------------------------------------

BBNET_ABI int m2_initialize(const void*) {
    BBNET_GUEST_RETURN();
    log("sceNpMatching2Initialize");
    return 0;
}
BBNET_ABI int m2_terminate() {
    BBNET_GUEST_RETURN();
    log("sceNpMatching2Terminate");
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.started = false;
    }
    session::events_stop();  // not under g.mu: the event thread may be waiting for it
    return 0;
}
// Bloodborne's two-argument form: (const CreateContextParam*, ctxId*). The param's first field
// is the SceNpId pointer.
BBNET_ABI int m2_create_context(const void* param, unsigned* ctx) {
    BBNET_GUEST_RETURN();
    if (!ctx) return kErrInvalidArg;
    std::lock_guard<std::mutex> lk(g.mu);
    if (param) {
        const void* npid = nullptr;
        std::memcpy(&npid, param, sizeof(npid));
        if (npid) g.npid = npid_text(npid);
    }
    if (!g.ctx_id) g.ctx_id = 1;
    *ctx = g.ctx_id;
    log("sceNpMatching2CreateContext (%s) -> %u", g.npid.c_str(), g.ctx_id);
    return 0;
}
BBNET_ABI int m2_register_context_cb(void* fn, void* arg) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lk(g.mu);
    g.ctx_cb = fn;
    g.ctx_arg = arg;
    return 0;
}
BBNET_ABI int m2_context_start(unsigned ctx, unsigned) {
    BBNET_GUEST_RETURN();
    {
        std::lock_guard<std::mutex> lk(g.mu);
        if (ctx != g.ctx_id) return kErrContext;
        if (g.started) return 0;
        g.started = true;
    }
    log("sceNpMatching2ContextStart -> party %s", session::transport().name());
    session::set_signaling_gate();
    session::events_start(on_server_event);  // not under g.mu (it may stop an event thread)
    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.heartbeat_started) {
        g.heartbeat_started = true;
        session::dispatch_after(5000, session::Prio::Request, heartbeat_loop);
    }
    // Register with the host off the game's thread; the started event follows at the contract's
    // delay whether or not the host answered: an unreachable host reads as "online but alone".
    session::dispatch_after(0, session::Prio::Context, [] {
        json::Value reply;
        std::string err;
        if (!session::server_context_start(reply, err)) log("np: context_start failed: %s", err.c_str());
    });
    session::dispatch_after(200, session::Prio::Context, [] {
        M2Call c;
        {
            std::lock_guard<std::mutex> lk2(g.mu);
            c = context_call(kEvContextStarted, 0, 0);
        }
        c.run();
    });
    return 0;
}
BBNET_ABI int m2_context_stop(unsigned) {
    BBNET_GUEST_RETURN();
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.started = false;
    }
    session::events_stop();
    return 0;
}
BBNET_ABI int m2_destroy_context(unsigned) {
    BBNET_GUEST_RETURN();
    session::events_stop();
    std::lock_guard<std::mutex> lk(g.mu);
    end_session_locked("context destroyed");
    g.started = false;
    g.ctx_cb = g.room_cb = g.sig_cb = g.lobby_cb = nullptr;
    return 0;
}
BBNET_ABI int m2_abort_context_start(unsigned) {
    BBNET_GUEST_RETURN();
    return 0;
}
BBNET_ABI int m2_get_server_id(unsigned ctx, std::uint16_t* id) {
    BBNET_GUEST_RETURN();
    if (!id) return kErrInvalidArg;
    if (ctx != g.ctx_id) return kErrContext;
    *id = 1;
    return 0;
}
BBNET_ABI int m2_set_default_opt(unsigned, const RequestOptParam* opt) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lk(g.mu);
    if (opt) g.dflt = *opt;
    return 0;
}
BBNET_ABI int m2_register_room_event_cb(unsigned, void* fn, void* arg) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lk(g.mu);
    g.room_cb = fn;
    g.room_arg = arg;
    return 0;
}
BBNET_ABI int m2_register_signaling_cb(unsigned, void* fn, void* arg) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lk(g.mu);
    g.sig_cb = fn;
    g.sig_arg = arg;
    return 0;
}
BBNET_ABI int m2_register_lobby_event_cb(unsigned, void* fn, void* arg) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lk(g.mu);
    g.lobby_cb = fn;
    g.lobby_arg = arg;
    return 0;
}

BBNET_ABI int m2_get_world_info_list(unsigned ctx, const void*, const RequestOptParam* opt, unsigned* req_out) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    const RequestOptParam o = opt_or_default(opt);
    session::dispatch_after(100, session::Prio::Request, [req, o] {
        M2Call c;
        {
            std::lock_guard<std::mutex> lk2(g.mu);
            auto held = std::make_unique<Held>();
            held->bytes.assign(sizeof(GetWorldInfoListResponse) + sizeof(World), 0);
            auto* resp = reinterpret_cast<GetWorldInfoListResponse*>(held->bytes.data());
            auto* w = reinterpret_cast<World*>(resp + 1);
            w->worldId = 1;
            w->numOfLobby = 1;
            w->maxNumOfTotalLobby = 1;
            w->maxNumOfRoom = 1000;
            w->numOfRoom = 1000;
            resp->world = w;
            resp->worldNum = 1;
            keep(std::move(held));
            c = request_call(o.cbFunc, o.cbFuncArg, req, kEvGetWorldInfoList, 0, resp);
        }
        c.run();
    });
    return 0;
}

// CreateJoinRoomRequest: maxSlot u16 at +0, worldId u32 at +0xc.
BBNET_ABI int m2_create_join_room(unsigned ctx, const std::uint8_t* reqp, const RequestOptParam* opt,
                                  unsigned* req_out) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    std::uint16_t max_slot = 5;  // Bloodborne asks 5: host, two cooperators, two invaders
    if (reqp) std::memcpy(&max_slot, reqp, 2);
    const RequestOptParam o = opt_or_default(opt);
    log("sceNpMatching2CreateJoinRoom req %u maxSlot %u%s", req, max_slot,
        g.in_room ? " (the game left its room without LeaveRoom; the host closes it)" : "");
    session::dispatch_after(0, session::Prio::Request, [req, o, max_slot] {
        json::Value extra = json::Value::make_object();
        session::host_extra(extra);
        json::Value reply;
        std::string err;
        std::uint64_t room = 0;
        std::string sid;
        int error = 0;
        if (session::server_create_room(max_slot, extra, reply, err)) {
            room = static_cast<std::uint64_t>(int_of(reply, "RoomId", 0));
            sid = str_of(reply, "SessionId");
        } else {
            log("np: create_room failed: %s", err.c_str());
            error = kErrRequestFailed;
        }
        CreateJoinRoomResponse* resp = nullptr;
        {
            std::lock_guard<std::mutex> lk2(g.mu);
            if (!error) {
                g.session_id = sid;
                g.room_id = room;
                g.member_id = clamp_member(int_of(reply, "MemberId", 1), 1);
                g.owner_id = g.member_id;
                g.max_slot = clamp_slots(int_of(reply, "MaxMembers", max_slot), max_slot);
                g.is_host = true;
                g.in_room = true;
                g.room_since_ms = session::now_ms();
                session::peers_clear();
                session::room_set(sid, g.member_id);
                resp = build_room_response(room, g.member_id, g.member_id, max_slot);
                log("np: room %llu created, we are member %u", static_cast<unsigned long long>(room), g.member_id);
            }
        }
        session::dispatch_after(join_ms(300, 100), session::Prio::Request, [req, o, error, resp] {
            fire_request(o.cbFunc, o.cbFuncArg, req, kEvCreateJoinRoom, error, resp);
        });
        if (!error) {
            std::lock_guard<std::mutex> lk2(g.mu);
            schedule_established_for_all(join_ms(800, 300));
        }
    });
    return 0;
}

// JoinRoomRequest: roomId u64 at +0.
BBNET_ABI int m2_join_room(unsigned ctx, const std::uint8_t* reqp, const RequestOptParam* opt, unsigned* req_out) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    std::uint64_t room = 0;
    if (reqp) std::memcpy(&room, reqp, 8);
    const RequestOptParam o = opt_or_default(opt);
    log("sceNpMatching2JoinRoom req %u room %llu", req, static_cast<unsigned long long>(room));
    g.joining_room = room;
    session::dispatch_after(0, session::Prio::Request, [req, o, room] {
        json::Value reply;
        std::string err;
        int error = 0;
        if (!session::server_join_room(room, reply, err)) {
            log("np: join_room failed: %s", err.c_str());
            error = kErrRequestFailed;
        }
        CreateJoinRoomResponse* resp = nullptr;
        {
            std::lock_guard<std::mutex> lk2(g.mu);
            if (g.joining_room == room) g.joining_room = 0;
            if (!error) {
                g.session_id = str_of(reply, "SessionId");
                g.room_id = room;
                g.member_id = clamp_member(int_of(reply, "MemberId", 2), 2);
                // The room's size and owner as the host has them.
                g.owner_id = clamp_member(int_of(reply, "OwnerMemberId", 1), 1);
                g.max_slot = clamp_slots(int_of(reply, "MaxMembers", 5), 5);
                g.is_host = false;
                g.in_room = true;
                g.room_since_ms = session::now_ms();
                // The member list is the room: entries from an invite or an earlier room go
                // (provisional ones stay for their connections).
                for (const session::Peer& peer : session::peers_all()) {
                    if (peer.member_id < 0xff00) session::peers_erase(peer.member_id);
                }
                if (const json::Value* members = reply.find("Members")) session::peers_from_members(*members);
                for (const session::Peer& peer : session::peers_all()) signaling_peer_known(peer.member_id);
                session::room_set(g.session_id, g.member_id);
                resp = build_room_response(room, g.member_id, g.owner_id, g.max_slot);
                log("np: joined room %llu as member %u with %zu peers", static_cast<unsigned long long>(room),
                    g.member_id, room_peers().size());
            }
        }
        session::dispatch_after(join_ms(300, 100), session::Prio::Request, [req, o, error, resp] {
            fire_request(o.cbFunc, o.cbFuncArg, req, kEvJoinRoom, error, resp);
        });
        if (!error) {
            std::lock_guard<std::mutex> lk2(g.mu);
            schedule_established_for_all(join_ms(500, 200));
        }
    });
    return 0;
}

BBNET_ABI int m2_leave_room(unsigned ctx, const void*, const RequestOptParam* opt, unsigned* req_out) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    const RequestOptParam o = opt_or_default(opt);
    // The game calls this only when its session is over: leave the host's room now, answer
    // 0x103 at +300 ms (bbhost: deferring it left the SessionOwner waiting and broke rejoin).
    log("sceNpMatching2LeaveRoom req %u%s", req,
        g.in_room && session::now_ms() - g.room_since_ms < 2000 ? " (2 s after the room was made!)" : "");
    g.leave_pending = true;
    g.leave_req = req;
    g.leave_opt = o;
    end_session_locked("LeaveRoom");
    return 0;
}

// (ctx, roomId, memberId, status*, SceNetInAddr*, SceInPort_t*): the three values.
BBNET_ABI int m2_signaling_get_connection_status(unsigned ctx, std::uint64_t room, std::uint16_t member, int* status,
                                                 std::uint32_t* addr, std::uint16_t* port) {
    BBNET_GUEST_RETURN();
    if (ctx != g.ctx_id) return kErrContext;
    session::Peer peer;
    const bool known = session::peers_get(member, &peer) && peer.addr != 0;
    if (status) *status = known ? 2 : 0;
    if (addr) *addr = known ? peer.addr : 0;
    if (port) *port = known ? static_cast<std::uint16_t>((peer.port << 8) | (peer.port >> 8)) : 0;
    if (trace()) {
        log("sceNpMatching2SignalingGetConnectionStatus room %llu member %u -> %d", static_cast<unsigned long long>(room),
            member, known ? 2 : 0);
    }
    return 0;
}

// The requests the summon path does not need still answer, as the SDK's always do: the game's
// room object marks a request pending until its callback and runs nothing else meanwhile.
int answer_request(unsigned ctx, const RequestOptParam* opt, unsigned* req_out, std::uint16_t event, int error,
                   const char* what) {
    std::lock_guard<std::mutex> lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    const RequestOptParam o = opt_or_default(opt);
    log("sceNpMatching2%s req %u: answered 0x%x%s", what, req, event, error ? " with an error" : "");
    session::dispatch_after(100, session::Prio::Request,
                            [o, req, event, error] { fire_request(o.cbFunc, o.cbFuncArg, req, event, error, nullptr); });
    return 0;
}

// The host sends a member away (a 4-byte reason in optData, which the kicked game reads from
// KICKEDOUT). The host service takes the member out and tells everyone, the kicked one
// included; without it the room hears it here.
BBNET_ABI int m2_kickout(unsigned ctx, const KickoutRequest* rq, const RequestOptParam* opt, unsigned* req_out) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    if (!rq) return kErrInvalidArg;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    const RequestOptParam o = opt_or_default(opt);
    const KickoutRequest r = *rq;
    std::uint32_t why = 0;  // 0xFF000019: the session lock after a boss clear (party_b2 K10b)
    if (r.optDataLen >= 4) std::memcpy(&why, r.optData, 4);
    log("sceNpMatching2KickoutRoomMember req %u room %llu member %u (%llu-byte reason 0x%08x)", req,
        static_cast<unsigned long long>(r.roomId), r.memberId, static_cast<unsigned long long>(r.optDataLen), why);
    const std::string sid = g.session_id;
    const int me = g.member_id;
    session::dispatch_after(0, session::Prio::Request, [o, req, r, sid, me] {
        std::string err;
        const std::size_t n =
            r.optDataLen > sizeof(r.optData) ? sizeof(r.optData) : static_cast<std::size_t>(r.optDataLen);
        const bool sent = !sid.empty() && session::server_kick_member(sid, r.memberId, me, r.optData, n, err);
        if (!sent) {
            log("np: kick of member %u not sent (%s); the room hears it here", r.memberId,
                sid.empty() ? "no room" : err.c_str());
            json::Value ev = json::Value::make_object();
            ev.set("Name", "room_member_left");
            {
                std::lock_guard<std::mutex> lk2(g.mu);
                ev.set("RoomId", static_cast<long long>(g.room_id));
            }
            ev.set("MemberId", static_cast<int>(r.memberId));
            ev.set("Reason", "kicked");
            on_server_event(ev);
        }
        session::dispatch_after(100, session::Prio::Request,
                                [o, req] { fire_request(o.cbFunc, o.cbFuncArg, req, kEvKickoutRoomMember, 0, nullptr); });
    });
    return 0;
}
BBNET_ABI int m2_grant_room_owner(unsigned ctx, const void*, const RequestOptParam* opt, unsigned* req_out) {
    BBNET_GUEST_RETURN();
    return answer_request(ctx, opt, req_out, kEvGrantRoomOwner, 0, "GrantRoomOwner");
}
BBNET_ABI int m2_set_room_data_internal(unsigned ctx, const void*, const RequestOptParam* opt, unsigned* req_out) {
    BBNET_GUEST_RETURN();
    return answer_request(ctx, opt, req_out, kEvSetRoomDataInternal, 0, "SetRoomDataInternal");
}
BBNET_ABI int m2_set_room_data_external(unsigned ctx, const void*, const RequestOptParam* opt, unsigned* req_out) {
    BBNET_GUEST_RETURN();
    return answer_request(ctx, opt, req_out, kEvSetRoomDataExternal, 0, "SetRoomDataExternal");
}
BBNET_ABI int m2_set_room_member_data_internal(unsigned ctx, const void*, const RequestOptParam* opt,
                                               unsigned* req_out) {
    BBNET_GUEST_RETURN();
    return answer_request(ctx, opt, req_out, kEvSetRoomMemberDataInternal, 0, "SetRoomMemberDataInternal");
}
// No room list: summons go through the game's own sign server (FromApi). The game's handler
// reads an error as "nothing found".
BBNET_ABI int m2_search_room(unsigned ctx, const void*, const RequestOptParam* opt, unsigned* req_out) {
    BBNET_GUEST_RETURN();
    return answer_request(ctx, opt, req_out, kEvSearchRoom, kErrServerNotAvailable, "SearchRoom");
}
// (ctx, {roomId}, opt, reqId): the round trip, from the keepalive's STUN exchange with the host.
BBNET_ABI int m2_signaling_get_ping_info(unsigned ctx, const std::uint64_t* rq, const RequestOptParam* opt,
                                        unsigned* req_out) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    const RequestOptParam o = opt_or_default(opt);
    auto held = std::make_unique<Held>();
    held->bytes.assign(sizeof(SignalingGetPingInfoResponse), 0);
    auto* resp = reinterpret_cast<SignalingGetPingInfoResponse*>(held->bytes.data());
    resp->serverId = 1;
    resp->worldId = 1;
    resp->roomId = rq ? *rq : g.room_id;
    const int rtt = session::stun_rtt_us();
    resp->rtt = static_cast<std::uint32_t>(rtt > 0 ? 2 * rtt : 60000);
    keep(std::move(held));
    session::dispatch_after(50, session::Prio::Request,
                            [o, req, resp] { fire_request(o.cbFunc, o.cbFuncArg, req, kEvSignalingGetPingInfo, 0, resp); });
    return 0;
}
// Lobbies: never called by this game; bound so a call is visible.
BBNET_ABI int m2_unused(unsigned, const void*, const RequestOptParam*, unsigned* req_out) {
    BBNET_GUEST_RETURN();
    static std::atomic<int> logs{0};
    if (logs.fetch_add(1) < 4) log("np: an unused NpMatching2 lobby request was called");
    if (req_out) *req_out = g.next_req++;
    return 0;
}

}  // namespace

std::string npid_text(const void* npid) {
    if (!npid) return "";
    char buf[17]{};
    std::memcpy(buf, npid, 16);
    return buf;
}

bool party_host_lost(const std::string& why) {
    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.in_room || g.is_host) return false;
    room_gone_locked(kEvRoomDestroyed, kCauseLeave, nullptr, 0, "the party host is gone: " + why);
    return true;
}

std::int64_t ms_since_member_left() {
    const std::int64_t at = g_member_left_ms.load();
    return at ? steady_ms_now() - at : (std::int64_t(1) << 40);
}

void set_host_closed_room_hook(HostClosedRoomHook hook) { g_host_closed_room_hook = hook; }

bool party_guest_in_room() {
    std::lock_guard<std::mutex> lk(g.mu);
    return g.in_room && !g.is_host;
}

bool matching2_context_started() {
    std::lock_guard<std::mutex> lk(g.mu);
    return g.started;
}

std::string matching2_status() {
    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.in_room) return "no room";
    return "room " + std::to_string(g.room_id) + " as member " + std::to_string(g.member_id) +
           (g.is_host ? " (host)" : " (guest)") + ", " + std::to_string(room_peers().size()) + " peer(s)";
}

}  // namespace bbnet::np

namespace bbnet {

#define M2(name, fn) {"sceNpMatching2" name, reinterpret_cast<void*>(np::fn)}
const Export kMatching2Exports[] = {
    M2("Initialize", m2_initialize),
    M2("Terminate", m2_terminate),
    M2("CreateContext", m2_create_context),
    M2("RegisterContextCallback", m2_register_context_cb),
    M2("ContextStart", m2_context_start),
    M2("ContextStop", m2_context_stop),
    M2("DestroyContext", m2_destroy_context),
    M2("AbortContextStart", m2_abort_context_start),
    M2("GetServerId", m2_get_server_id),
    M2("SetDefaultRequestOptParam", m2_set_default_opt),
    M2("RegisterRoomEventCallback", m2_register_room_event_cb),
    M2("RegisterSignalingCallback", m2_register_signaling_cb),
    M2("RegisterLobbyEventCallback", m2_register_lobby_event_cb),
    M2("GetWorldInfoList", m2_get_world_info_list),
    M2("CreateJoinRoom", m2_create_join_room),
    M2("JoinRoom", m2_join_room),
    M2("LeaveRoom", m2_leave_room),
    M2("SignalingGetConnectionStatus", m2_signaling_get_connection_status),
    M2("SearchRoom", m2_search_room),
    M2("JoinLobby", m2_unused),
    M2("LeaveLobby", m2_unused),
    M2("GetLobbyInfoList", m2_unused),
    M2("KickoutRoomMember", m2_kickout),
    M2("GrantRoomOwner", m2_grant_room_owner),
    M2("SetRoomDataInternal", m2_set_room_data_internal),
    M2("SetRoomDataExternal", m2_set_room_data_external),
    M2("SetRoomMemberDataInternal", m2_set_room_member_data_internal),
    M2("SignalingGetPingInfo", m2_signaling_get_ping_info),
    {nullptr, nullptr},
};
#undef M2

}  // namespace bbnet
