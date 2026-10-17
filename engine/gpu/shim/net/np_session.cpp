// SPDX-License-Identifier: GPL-3.0-or-later
// derived from droogie/bbhost src/net/session.cpp @8f2746c
//
// The session layer (np_session.h). bbport: the dispatcher runs host closures only and posts
// guest callbacks to the A1 dispatcher; np_post() to the private server became PartyTransport
// calls; the STUN server is the party host (set by the party link), the relay is the host's.
#include "np_session.h"

#include "bbnet_internal.h"
#include "party_transport.h"
#include "party_udp.h"
#include "party_util.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace bbnet::session {

namespace {

using clock = std::chrono::steady_clock;

// ---- the scheduler ---------------------------------------------------------------------------

struct Due {
    clock::time_point at;
    int prio;
    std::uint64_t seq;
    std::function<void()> fn;
    bool operator<(const Due& o) const {
        if (at != o.at) return at < o.at;
        if (prio != o.prio) return prio < o.prio;
        return seq < o.seq;
    }
};

std::mutex g_dmu;
std::condition_variable g_dcv, g_idle_cv;
std::deque<Due> g_due;  // kept sorted on insert
std::uint64_t g_seq = 0;
bool g_drun = false;
bool g_running_one = false;
std::atomic<std::thread::id> g_dtid{};

void scheduler_main() {
    g_dtid.store(std::this_thread::get_id());
    std::unique_lock<std::mutex> lk(g_dmu);
    for (;;) {
        if (g_due.empty()) {
            g_dcv.wait(lk);
            continue;
        }
        const auto now = clock::now();
        if (g_due.front().at > now) {
            g_dcv.wait_until(lk, g_due.front().at);
            continue;
        }
        std::function<void()> fn = std::move(g_due.front().fn);
        g_due.pop_front();
        g_running_one = true;
        lk.unlock();
        if (fn) fn();
        lk.lock();
        g_running_one = false;
        g_idle_cv.notify_all();
    }
}

// ---- the peer table --------------------------------------------------------------------------

std::mutex g_pmu;
std::map<std::uint16_t, Peer> g_peers;
// Peer input caps (bbport security pass): other members in a room (BB_PARTY_MAX is at most 8).
constexpr std::size_t kMaxPeers = 8;
// A peer's port number: 0 unless 1..65535 (a cast would wrap 65536 + n to n).
std::uint16_t port_of(long long v) { return v > 0 && v <= 65535 ? static_cast<std::uint16_t>(v) : 0; }

// ---- our reflexive address -------------------------------------------------------------------

struct Mapped {
    std::mutex mu;
    std::string addr;
    int port = 0;
    int relay_port = 0;
    std::int64_t at_ms = 0;
    bool asked = false;
    std::string stun_host;
    std::uint16_t stun_port = 0;
    std::string override_addr;
    int override_port = 0;
};
Mapped g_mapped;
std::atomic<int> g_stun_rtt_us{0};

void mapped_refresh(bool force) {
    std::string host;
    std::uint16_t sport;
    {
        std::lock_guard<std::mutex> lk(g_mapped.mu);
        if (!g_mapped.override_addr.empty()) {
            g_mapped.addr = g_mapped.override_addr;
            g_mapped.port = g_mapped.override_port;
            return;
        }
        if (!force && g_mapped.asked && now_ms() - g_mapped.at_ms < 120000) return;
        g_mapped.asked = true;
        g_mapped.at_ms = now_ms();
        host = g_mapped.stun_host;
        sport = g_mapped.stun_port;
    }
    if (host.empty() || !sport) return;
    std::uint32_t addr = 0;
    std::uint16_t port = 0;
    RelayInfo relay;
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
        const auto t0 = clock::now();
        ok = p2p_stun(host.c_str(), sport, 1000, &addr, &port, true, &relay);
        if (ok) {
            g_stun_rtt_us = static_cast<int>(
                std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0).count());
        }
    }
    std::lock_guard<std::mutex> lk(g_mapped.mu);
    if (!ok) {
        if (g_mapped.addr.empty())
            log("stun: no answer from %s:%u; the host gets our LAN address only", host.c_str(), sport);
        return;  // keep the last answer
    }
    const std::string text = party::ip_text(addr);
    if (g_mapped.addr != text || g_mapped.port != port) {
        log("stun: %s:%u sees our P2P port %d as %s:%u%s", host.c_str(), sport, p2p_port(), text.c_str(), port,
            (text == local_addr_text() && port == p2p_port()) ? " (no NAT in between)" : "");
    }
    g_mapped.addr = text;
    g_mapped.port = port;
    g_mapped.relay_port = relay.present ? relay.vport : 0;
}

json::Value our_endpoint() {
    mapped_refresh(false);
    json::Value o = json::Value::make_object();
    o.set("OnlineId", online_id());
    o.set("LocalAddr", local_addr_text());
    o.set("LocalPort", p2p_port());
    o.set("PublicAddr", local_addr_text());
    o.set("PublicPort", p2p_port());
    std::lock_guard<std::mutex> lk(g_mapped.mu);
    o.set("MappedAddr", g_mapped.addr);
    o.set("MappedPort", g_mapped.port);
    o.set("RelayPort", g_mapped.relay_port);
    return o;
}

// A peer with a new address: probes toward it so our NAT lets its datagrams in, never toward
// ourselves.
void punch_if_new(const Peer& before, const Peer& after) {
    if (after.online_id == online_id() || !after.addr || !after.port) return;
    if (before.addr == after.addr && before.port == after.port && before.local_addr == after.local_addr &&
        before.local_port == after.local_port) {
        return;
    }
    p2p_punch(after.online_id.c_str(), after.addr, after.port, after.local_addr, after.local_port);
}

// Where a peer's address enters the table: the socket layer learns the peer and its relay port
// and picks the path per datagram (direct while the peer answers probes, else the host relay;
// always the relay with BB_PARTY_FORCE_RELAY=1). The game keeps the direct address.
void route_peer(Peer& p, int relay_port) {
    if (settings().host) return;
    const std::uint16_t rp = relay_port > 0 && relay_port <= 65535 ? static_cast<std::uint16_t>(relay_port) : 0;
    p2p_add_peer(p.online_id.c_str(), p.addr, p.port, rp);
}

std::atomic<bool> g_context_started{false};

std::mutex g_roommu;
std::string g_room_sid;
int g_room_mid = 0;

// Tells the host our new reflexive address.
void server_signaling_update() {
    json::Value body = json::Value::make_object();
    body.set("OnlineId", online_id());
    {
        std::lock_guard<std::mutex> lk(g_mapped.mu);
        body.set("MappedAddr", g_mapped.addr);
        body.set("MappedPort", g_mapped.port);
        body.set("RelayPort", g_mapped.relay_port);
    }
    {
        std::lock_guard<std::mutex> lk(g_roommu);
        body.set("SessionId", g_room_sid);
        body.set("MemberId", g_room_mid);
    }
    json::Value reply;
    std::string err;
    transport().rpc(party::call::kSignalingUpdate, body, reply, err, 4000);
}

// While the context runs: the STUN binding every 15 s keeps our NAT mapping to the host open
// and the relay port alive; a changed address goes to the host at once (bbhost 2026-10-06).
std::atomic<bool> g_keeprun{false};
std::condition_variable g_keepcv;
std::mutex g_keepmu;

void keepalive_main() {
    std::string last;
    {
        std::lock_guard<std::mutex> lk(g_mapped.mu);
        last = g_mapped.addr + ":" + std::to_string(g_mapped.port) + " relay " + std::to_string(g_mapped.relay_port);
    }
    while (g_keeprun.load()) {
        {
            std::unique_lock<std::mutex> lk(g_keepmu);
            g_keepcv.wait_for(lk, std::chrono::seconds(15), [] { return !g_keeprun.load(); });
        }
        if (!g_keeprun.load()) break;
        mapped_refresh(true);
        std::string now;
        {
            std::lock_guard<std::mutex> lk(g_mapped.mu);
            now = g_mapped.addr + ":" + std::to_string(g_mapped.port) + " relay " + std::to_string(g_mapped.relay_port);
        }
        // The relay port matters too: a restarted host may hand out another one.
        if (now != last && now.rfind(":0 ", 0) != 0) {
            log("stun: our address is now %s (was %s); telling the host", now.c_str(), last.c_str());
            server_signaling_update();
            last = now;
        }
    }
}

std::mutex g_hookmu;
std::function<void(json::Value&)> g_extra_hook;
std::function<void(const SummonInvite&)> g_invite_hook;

constexpr std::uint64_t kSignalingGate = 0x546cc09;  // bbhost 0x586cc09
constexpr std::uint64_t kFrpgNetManSlot = 0x553b120;  // bbhost 0x593b120
constexpr std::uint32_t kAreaOff = 0xa7c, kLevelOff = 0xaa8, kPosOff = 0xaac;

}  // namespace

void dispatch_after(int delay_ms, Prio prio, std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_dmu);
    if (!g_drun) {
        g_drun = true;
        std::thread(scheduler_main).detach();
    }
    Due d{clock::now() + std::chrono::milliseconds(delay_ms < 0 ? 0 : delay_ms), static_cast<int>(prio), g_seq++,
          std::move(fn)};
    auto it = g_due.begin();
    while (it != g_due.end() && *it < d) ++it;
    g_due.insert(it, std::move(d));
    g_dcv.notify_all();
}

bool on_session_thread() { return g_dtid.load() == std::this_thread::get_id(); }

void drain(int horizon_ms) {
    const auto deadline = clock::now() + std::chrono::milliseconds(horizon_ms);
    std::unique_lock<std::mutex> lk(g_dmu);
    g_idle_cv.wait_until(lk, deadline + std::chrono::seconds(5), [&] {
        return !g_running_one && (g_due.empty() || g_due.front().at > deadline);
    });
}

std::string online_id() {
    const std::string& id = settings().online_id;
    return id.empty() ? "Player" : id;
}

std::string local_addr_text() {
    const std::string t = party::ip_text(local_ipv4());
    return t.empty() ? "127.0.0.1" : t;
}

int p2p_port() {
    const int b = p2p_bound_port();
    return b > 0 ? b : settings().party_port;
}

void peers_clear() {
    std::lock_guard<std::mutex> lk(g_pmu);
    g_peers.clear();
}

void peers_upsert(const Peer& p) {
    Peer before;
    {
        std::lock_guard<std::mutex> lk(g_pmu);
        // A room has at most kMaxPeers other members: a host's flood of member records must not
        // grow the table (every entry becomes a member in the game's own room structures).
        if (!g_peers.count(p.member_id)) {
            std::size_t real = 0;
            for (const auto& [m, q] : g_peers) real += m < 0xff00 ? 1 : 0;
            if ((p.member_id < 0xff00 && real >= kMaxPeers) || g_peers.size() >= 2 * kMaxPeers) {
                log("np: peer table full; member %u (%s) dropped", p.member_id, p.online_id.c_str());
                return;
            }
        }
        Peer& slot = g_peers[p.member_id];
        before = slot;
        const unsigned conn = p.conn_id ? p.conn_id : slot.conn_id;
        const int sig = p.sig_status ? p.sig_status : slot.sig_status;
        slot = p;
        slot.conn_id = conn;
        slot.sig_status = sig;
    }
    punch_if_new(before, p);
}

bool peers_get(std::uint16_t member_id, Peer* out) {
    std::lock_guard<std::mutex> lk(g_pmu);
    auto it = g_peers.find(member_id);
    if (it == g_peers.end()) return false;
    if (out) *out = it->second;
    return true;
}

bool peers_find_online(const std::string& id, Peer* out) {
    std::lock_guard<std::mutex> lk(g_pmu);
    for (const auto& [m, p] : g_peers) {
        (void)m;
        if (p.online_id == id) {
            if (out) *out = p;
            return true;
        }
    }
    return false;
}

bool peers_find_conn(unsigned conn_id, Peer* out) {
    std::lock_guard<std::mutex> lk(g_pmu);
    for (const auto& [m, p] : g_peers) {
        (void)m;
        if (p.conn_id == conn_id && conn_id != 0) {
            if (out) *out = p;
            return true;
        }
    }
    return false;
}

void peers_set_conn(std::uint16_t member_id, unsigned conn_id, int sig_status) {
    std::lock_guard<std::mutex> lk(g_pmu);
    auto it = g_peers.find(member_id);
    if (it == g_peers.end()) return;
    it->second.conn_id = conn_id;
    it->second.sig_status = sig_status;
}

void peers_erase(std::uint16_t member_id) {
    std::lock_guard<std::mutex> lk(g_pmu);
    g_peers.erase(member_id);
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count();
}

std::vector<Peer> peers_all() {
    std::lock_guard<std::mutex> lk(g_pmu);
    std::vector<Peer> v;
    for (const auto& [m, p] : g_peers) {
        (void)m;
        v.push_back(p);
    }
    return v;
}

int peers_from_members(const json::Value& members) {
    using party::int_of;
    using party::ip_parse;
    using party::str_of;
    if (members.type != json::Value::Type::Array) return 0;
    int n = 0;
    for (const json::Value& m : members.array) {
        if (n >= static_cast<int>(kMaxPeers)) break;  // a room is never bigger
        Peer p;
        const long long mid = int_of(m, "MemberId", 0);
        if (mid <= 0 || mid >= 0xff00) continue;  // 0xff00+: our own provisional entries
        p.member_id = static_cast<std::uint16_t>(mid);
        p.online_id = str_of(m, "OnlineId");
        p.addr = ip_parse(str_of(m, "Addr"));
        p.port = port_of(int_of(m, "Port", 0));
        p.local_addr = ip_parse(str_of(m, "LocalAddr"));
        p.local_port = port_of(int_of(m, "LocalPort", 0));
        // A STUN-mapped address that differs from the peer's local one means a NAT in between,
        // and only the mapped one reaches the peer.
        const std::uint32_t mapped = ip_parse(str_of(m, "MappedAddr"));
        const int mapped_port = static_cast<int>(int_of(m, "MappedPort", 0));
        if (mapped && mapped_port > 0 && mapped_port <= 65535 && mapped != p.local_addr && mapped != p.addr) {
            p.addr = mapped;
            p.port = static_cast<std::uint16_t>(mapped_port);
        }
        if (!p.addr) {
            p.addr = p.local_addr;
            p.port = p.local_port;
        }
        route_peer(p, static_cast<int>(int_of(m, "RelayPort", 0)));
        peers_upsert(p);
        ++n;
    }
    return n;
}

Peer peers_provisional(const std::string& id, std::uint32_t addr, std::uint16_t port) {
    Peer before, after;
    {
        std::lock_guard<std::mutex> lk(g_pmu);
        bool found = false;
        for (auto& [m, p] : g_peers) {
            if (p.online_id == id && m >= 0xff00) {
                before = p;
                p.addr = addr;
                p.port = port;
                after = p;
                found = true;
                break;
            }
        }
        if (!found) {
            std::uint16_t m = 0xff00;
            while (g_peers.count(m)) ++m;
            after.member_id = m;
            after.online_id = id;
            after.addr = addr;
            after.port = port;
            g_peers[m] = after;
        }
    }
    punch_if_new(before, after);
    return after;
}

party::PartyTransport& transport() { return party::transport(); }

bool server_context_start(json::Value& reply, std::string& error) {
    mapped_refresh(true);
    json::Value body = json::Value::make_object();
    body.set("OnlineId", online_id());
    body.set("SignalingAddr", local_addr_text());
    body.set("SignalingPort", p2p_port());
    {
        std::lock_guard<std::mutex> lk(g_mapped.mu);
        body.set("MappedAddr", g_mapped.addr);
        body.set("MappedPort", g_mapped.port);
        body.set("RelayPort", g_mapped.relay_port);
    }
    const bool ok = transport().ok_call(party::call::kContextStart, body, reply, error);
    if (ok) g_context_started.store(true);
    return ok;
}

void host_session_reset() {
    {
        std::lock_guard<std::mutex> lk(g_mapped.mu);
        g_mapped.asked = false;  // the relay registration is the old host's
    }
    if (!g_context_started.load()) return;
    dispatch_after(0, Prio::Context, [] {
        json::Value reply;
        std::string err;
        if (server_context_start(reply, err)) log("np: context registered again with the host");
        else log("np: context_start after the host's restart failed: %s", err.c_str());
    });
}

bool server_create_room(int max_members, const json::Value& extra, json::Value& reply, std::string& error) {
    json::Value body = our_endpoint();
    body.set("MaxMembers", max_members);
    if (extra.type == json::Value::Type::Object) {
        for (const auto& [k, v] : extra.object) body.set(k, v);
    }
    return transport().ok_call(party::call::kCreateRoom, body, reply, error);
}

bool server_join_room(std::uint64_t room_id, json::Value& reply, std::string& error) {
    json::Value body = our_endpoint();
    body.set("RoomId", static_cast<long long>(room_id));
    return transport().ok_call(party::call::kJoinRoom, body, reply, error);
}

bool server_leave_room(const std::string& session_id, int member_id, json::Value& reply, std::string& error) {
    json::Value body = json::Value::make_object();
    body.set("SessionId", session_id);
    body.set("MemberId", member_id);
    return transport().ok_call(party::call::kLeaveRoom, body, reply, error);
}

int server_heartbeat(const std::string& session_id, int member_id) {
    json::Value body = json::Value::make_object();
    body.set("SessionId", session_id);
    body.set("MemberId", member_id);
    json::Value reply;
    std::string err;
    if (!transport().rpc(party::call::kHeartbeat, body, reply, err, 3000)) return -1;
    return party::int_of(reply, "InRoom", 1) == 0 ? 0 : 1;
}

bool server_kick_member(const std::string& session_id, int member_id, int kicker_id, const std::uint8_t* opt,
                        std::size_t opt_len, std::string& error) {
    json::Value body = json::Value::make_object();
    body.set("SessionId", session_id);
    body.set("MemberId", member_id);
    body.set("KickerMemberId", kicker_id);
    body.set("OptData", party::b64_encode(opt, opt_len > 16 ? 16 : opt_len));
    json::Value reply;
    return transport().ok_call(party::call::kKickMember, body, reply, error, 4000);
}

bool server_signaling_resolve(const std::string& id, std::uint32_t* addr, std::uint16_t* port, std::string& error) {
    json::Value body = json::Value::make_object();
    body.set("OnlineId", id);
    json::Value reply;
    if (!transport().ok_call(party::call::kSignalingResolve, body, reply, error)) return false;
    const std::uint32_t a = party::ip_parse(party::str_of(reply, "Addr"));
    const int p = static_cast<int>(party::int_of(reply, "Port", 0));
    if (!a || p <= 0 || p > 65535) {
        error = "no address";
        return false;
    }
    Peer route;
    route.online_id = id;
    route.addr = a;
    route.port = static_cast<std::uint16_t>(p);
    route_peer(route, static_cast<int>(party::int_of(reply, "RelayPort", 0)));
    if (addr) *addr = route.addr;
    if (port) *port = route.port;
    return true;
}

void events_start(std::function<void(const json::Value& event)> handler) {
    transport().start_events(online_id(), std::move(handler));
    if (!g_keeprun.exchange(true)) std::thread(keepalive_main).detach();
}

void events_stop() {
    {
        std::lock_guard<std::mutex> lk(g_keepmu);
        g_keeprun.store(false);
        g_keepcv.notify_all();
    }
    transport().stop_events();
}

void room_set(const std::string& session_id, int member_id) {
    std::lock_guard<std::mutex> lk(g_roommu);
    g_room_sid = session_id;
    g_room_mid = member_id;
}
void room_clear() { room_set("", 0); }
int stun_rtt_us() { return g_stun_rtt_us.load(); }

void set_stun_server(const std::string& host, std::uint16_t port) {
    std::lock_guard<std::mutex> lk(g_mapped.mu);
    g_mapped.stun_host = host;
    g_mapped.stun_port = port;
    g_mapped.asked = false;
}

void set_mapped_override(const std::string& addr, int port) {
    std::lock_guard<std::mutex> lk(g_mapped.mu);
    g_mapped.override_addr = addr;
    g_mapped.override_port = port;
    g_mapped.addr = addr;
    g_mapped.port = port;
}

bool set_signaling_gate() {
    void* at = guest_image_at(kSignalingGate, 1);
    if (!at) {
        log("np: no game image; the Matching2 signaling gate is not set");
        return false;
    }
    std::uint8_t v = 0;
    if (!guest_read(reinterpret_cast<std::uintptr_t>(at), &v, 1)) return false;
    if (v == 1) return true;
    const std::uint8_t one = 1;
    if (!guest_write(reinterpret_cast<std::uintptr_t>(at), &one, 1)) {
        log("np: the Matching2 signaling gate (0x%llx) is not writable", static_cast<unsigned long long>(kSignalingGate));
        return false;
    }
    log("np: signaling gate set (image+0x%llx = 1)", static_cast<unsigned long long>(kSignalingGate));
    return true;
}

void set_host_extra_hook(std::function<void(json::Value& extra)> hook) {
    std::lock_guard<std::mutex> lk(g_hookmu);
    g_extra_hook = std::move(hook);
}

void host_extra(json::Value& extra) {
    std::function<void(json::Value&)> hook;
    {
        std::lock_guard<std::mutex> lk(g_hookmu);
        hook = g_extra_hook;
    }
    if (hook) {
        hook(extra);
        return;
    }
    void* slot = guest_image_at(kFrpgNetManSlot, 8);
    std::uint64_t man = 0;
    if (!slot || !guest_read(reinterpret_cast<std::uintptr_t>(slot), &man, 8) || man < 0x10000) return;
    std::uint32_t area = 0, level = 0;
    float pos[3] = {0, 0, 0};
    if (!guest_read(static_cast<std::uintptr_t>(man) + kAreaOff, &area, 4) ||
        !guest_read(static_cast<std::uintptr_t>(man) + kLevelOff, &level, 4) ||
        !guest_read(static_cast<std::uintptr_t>(man) + kPosOff, pos, sizeof(pos))) {
        return;
    }
    extra.set("HostArea", static_cast<long long>(area));
    extra.set("HostLevel", static_cast<long long>(level));
    json::Value p = json::Value::make_array();
    for (float f : pos) p.push(static_cast<double>(f));
    extra.set("HostPos", std::move(p));
    extra.set("MemberTag", 1);
}

void set_invite_hook(std::function<void(const SummonInvite&)> hook) {
    std::lock_guard<std::mutex> lk(g_hookmu);
    g_invite_hook = std::move(hook);
}

void deliver_invite(const SummonInvite& inv) {
    std::function<void(const SummonInvite&)> hook;
    {
        std::lock_guard<std::mutex> lk(g_hookmu);
        hook = g_invite_hook;
    }
    if (hook) {
        hook(inv);
        return;
    }
    log("np: invite to room %llu from %s (no delivery hook; the host's game sends its own item)",
        static_cast<unsigned long long>(inv.room_id), inv.host_online_id.c_str());
}

}  // namespace bbnet::session
