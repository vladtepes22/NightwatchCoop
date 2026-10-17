// SPDX-License-Identifier: GPL-3.0-or-later
// The party host service (party_host_service.h): Matching2 rooms, signaling resolve and
// per-member event queues, in the host's process.
#include "party_host_service.h"

#include "bbnet_internal.h"
#include "party_util.h"
#include "np_hle.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>

namespace bbnet::party {

int party_max_members() {
    static const int n = [] {
        const char* e = std::getenv("BB_PARTY_MAX");
        int v = e && *e ? std::atoi(e) : 3;
        if (v < 2) v = 2;
        if (v > 8) v = 8;
        return v;
    }();
    return n;
}

namespace {

std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Endpoint {
    std::string local_addr, mapped_addr, public_addr;
    int local_port = 0, mapped_port = 0, public_port = 0;
    int relay_port = 0;
};

// A party member's signaling registration (context_start).
struct Context {
    std::string online_id;
    Endpoint ep;
    std::uint32_t link_addr = 0;
    std::int64_t at_ms = 0;
};

struct Member {
    std::uint16_t id = 0;
    std::string online_id;
    Endpoint ep;  // what its create/join carried
    std::int64_t last_heartbeat_ms = 0;
};

struct Room {
    std::uint64_t id = 0;
    std::string session_id;
    std::uint16_t owner = 0;
    int max_members = 0;
    std::uint16_t next_member = 1;
    std::map<std::uint16_t, Member> members;
    json::Value extra = json::Value::make_object();
};

struct Queue {
    std::deque<json::Value> events;  // each carries EventId
    std::uint64_t next_id = 1;
};

// A member's address fields are peer input (bbport security pass): a dotted IPv4 address or
// nothing, a port 1..65535 or 0.
std::string addr_of(const json::Value& r, const char* key) {
    const std::string s = str_of(r, key);
    return s.size() <= 15 && ip_parse(s) ? s : std::string();
}
int port_of(const json::Value& r, const char* key) {
    const long long v = int_of(r, key, 0);
    return v > 0 && v <= 65535 ? static_cast<int>(v) : 0;
}

Endpoint endpoint_of(const json::Value& r) {
    Endpoint e;
    e.local_addr = addr_of(r, "LocalAddr");
    e.local_port = port_of(r, "LocalPort");
    e.public_addr = addr_of(r, "PublicAddr");
    e.public_port = port_of(r, "PublicPort");
    e.mapped_addr = addr_of(r, "MappedAddr");
    e.mapped_port = port_of(r, "MappedPort");
    e.relay_port = port_of(r, "RelayPort");
    return e;
}

// create_room's extra members, as guests' invites carry them: numbers, and HostPos as three
// finite coordinates.
void copy_extra(const json::Value& rq, json::Value& extra) {
    for (const char* k : {"HostArea", "HostLevel", "MemberTag"}) {
        const json::Value* v = rq.find(k);
        if (v && v->type == json::Value::Type::Number && v->number >= 0 && v->number <= 4294967295.0)
            extra.set(k, *v);
    }
    const json::Value* p = rq.find("HostPos");
    if (p && p->type == json::Value::Type::Array && p->array.size() == 3) {
        json::Value pos = json::Value::make_array();
        for (const json::Value& c : p->array) {
            const bool ok = c.type == json::Value::Type::Number && c.number > -100000.0 && c.number < 100000.0;
            pos.push(ok ? c.number : 0.0);
        }
        extra.set("HostPos", std::move(pos));
    }
}

json::Value error_reply(int kind, const std::string& text) {
    json::Value r = json::Value::make_object();
    r.set("ResKind", kind);
    r.set("Error", text);
    return r;
}

// ResKind 7: the request names someone other than the caller PartyLink authenticated.
constexpr int kResImpersonation = 7;

// The identity a call acts as. A call that came over PartyLink carries the member's
// authenticated name in `who` (party_runtime's on_rpc refuses a slot with no roster name), and
// that name wins: a body OnlineId naming anyone else is refused. Only an in-process caller with
// no name of its own (tests, tools) is taken at the body's word.
bool caller_identity(const Caller& who, const json::Value& rq, const char* kind, std::string* id, json::Value* err) {
    const std::string body = str_of(rq, "OnlineId");
    if (!who.online_id.empty()) {
        if (!body.empty() && body != who.online_id) {
            log("party host: refused %s from %s claiming OnlineId %s", kind, who.online_id.c_str(), body.c_str());
            *err = error_reply(kResImpersonation, "OnlineId does not match the caller");
            return false;
        }
        *id = who.online_id;
        return true;
    }
    if (who.link_addr) {  // a remote caller with no authenticated name
        log("party host: refused %s from an unnamed remote caller", kind);
        *err = error_reply(kResImpersonation, "caller not authenticated");
        return false;
    }
    if (body.empty()) {
        *err = error_reply(1, "no OnlineId");
        return false;
    }
    *id = body;
    return true;
}

// May `who` act as member `m`? Its own record only (an unnamed in-process caller: anyone).
bool caller_is(const Caller& who, const std::string& member_online_id) {
    if (who.online_id.empty()) return who.link_addr == 0;
    return who.online_id == member_online_id;
}

json::Value ok_reply() {
    json::Value r = json::Value::make_object();
    r.set("ResKind", 0);
    return r;
}

}  // namespace

struct PartyHostService::Impl {
    mutable std::mutex mu;
    std::condition_variable cv;
    std::uint64_t wake_gen = 0;
    std::map<std::string, Context> contexts;
    std::vector<std::string> restarted;  // take_restarted()
    std::map<std::uint64_t, Room> rooms;
    std::map<std::string, Queue> queues;
    std::set<std::string> loading;
    // Members whose heartbeat timeout was held by the loading query (an outage, a load): when the
    // hold ends their clock starts over (they could not beat meanwhile).
    std::set<std::string> held_late;
    std::function<bool(const std::string&)> loading_query;
    std::function<std::int64_t()> clock = steady_ms;
    int heartbeat_timeout_ms = 15000;
    std::uint64_t next_room = 1001;
    std::string host_online, host_addr;
    int host_port = 0;
    std::mt19937_64 rng{std::random_device{}() ^ static_cast<std::uint64_t>(steady_ms())};
    std::atomic<bool> reaper{false};
    std::atomic<bool> stop{false};

    std::int64_t now() const { return clock(); }

    std::uint64_t push_locked(const std::string& online_id, json::Value ev) {
        Queue& q = queues[online_id];
        const std::uint64_t id = q.next_id++;
        ev.set("EventId", static_cast<long long>(id));
        if (party_trace()) log("party host: event #%llu for %s: %s", static_cast<unsigned long long>(id),
                               online_id.c_str(), json::dump(ev, 0).c_str());
        q.events.push_back(std::move(ev));
        while (q.events.size() > 256) q.events.pop_front();
        ++wake_gen;
        cv.notify_all();
        return id;
    }

    // The record peers get for `m` of `room` (signaling_resolve's shape plus MemberId).
    json::Value record_locked(const std::string& online_id, const Endpoint* member_ep, std::uint16_t member_id) const {
        Endpoint ep = member_ep ? *member_ep : Endpoint{};
        std::uint32_t link = 0;
        auto c = contexts.find(online_id);
        if (c != contexts.end()) {
            // What it registered at context_start wins.
            const Endpoint& r = c->second.ep;
            if (!r.local_addr.empty()) {
                ep.local_addr = r.local_addr;
                ep.local_port = r.local_port;
            }
            if (!r.mapped_addr.empty() && r.mapped_port > 0) {
                ep.mapped_addr = r.mapped_addr;
                ep.mapped_port = r.mapped_port;
            }
            if (r.relay_port) ep.relay_port = r.relay_port;
            link = c->second.link_addr;
        }
        if (online_id == host_online && !host_addr.empty() && ep.mapped_addr.empty()) {
            ep.mapped_addr = host_addr;
            ep.mapped_port = host_port;
        }
        // The address peers reach it at: a mapped address that differs from the local one
        // (a NAT in between), else the local one; the link's address with the local port
        // when it registered nothing reachable.
        std::string addr = ep.local_addr;
        int port = ep.local_port;
        if (!ep.mapped_addr.empty() && ep.mapped_port > 0 &&
            (ep.mapped_addr != ep.local_addr || ep.mapped_port != ep.local_port)) {
            addr = ep.mapped_addr;
            port = ep.mapped_port;
        }
        if ((addr.empty() || ip_is_loopback(ip_parse(addr))) && link && !ip_is_loopback(link)) {
            addr = ip_text(link);
            if (!port) port = ep.local_port;
        }
        if (!ep.relay_port && !ep.mapped_addr.empty()) {
            std::uint16_t vp = 0;
            if (p2p_relay_vport_for(ip_parse(ep.mapped_addr), static_cast<std::uint16_t>(ep.mapped_port), &vp))
                ep.relay_port = vp;
        }
        json::Value r = json::Value::make_object();
        if (member_id) r.set("MemberId", static_cast<int>(member_id));
        r.set("OnlineId", online_id);
        r.set("Addr", addr);
        r.set("Port", port);
        r.set("LocalAddr", ep.local_addr);
        r.set("LocalPort", ep.local_port);
        r.set("MappedAddr", ep.mapped_addr);
        r.set("MappedPort", ep.mapped_port);
        r.set("RelayPort", ep.relay_port);
        return r;
    }

    Room* find_session_locked(const std::string& sid, std::uint16_t member_id, Member** m) {
        for (auto& [id, room] : rooms) {
            if (room.session_id != sid) continue;
            auto it = room.members.find(member_id);
            if (it == room.members.end()) return nullptr;
            if (m) *m = &it->second;
            return &room;
        }
        return nullptr;
    }

    // Takes `member_id` out of `room`: the owner closes the room, anyone else leaves it.
    // `reason` is the room_member_left Reason; `kicked_opt` set for a kick.
    void remove_member_locked(std::uint64_t room_id, std::uint16_t member_id, const std::string& reason,
                              const std::string* kicked_opt) {
        auto rit = rooms.find(room_id);
        if (rit == rooms.end()) return;
        Room& room = rit->second;
        auto mit = room.members.find(member_id);
        if (mit == room.members.end()) return;
        const Member gone = mit->second;
        room.members.erase(mit);
        loading.erase(gone.online_id);
        if (kicked_opt) {
            json::Value ev = json::Value::make_object();
            ev.set("Name", "room_member_kicked");
            ev.set("RoomId", static_cast<long long>(room.id));
            ev.set("MemberId", static_cast<int>(gone.id));
            ev.set("OptData", *kicked_opt);
            push_locked(gone.online_id, std::move(ev));
        }
        if (member_id == room.owner) {
            const std::string why = reason == "timeout" ? "host_timeout" : reason == "leave_room" ? "host_left" : "server";
            log("party host: room %llu closed (%s left: %s)", static_cast<unsigned long long>(room.id),
                gone.online_id.c_str(), reason.c_str());
            for (const auto& [id, m] : room.members) {
                (void)id;
                json::Value ev = json::Value::make_object();
                ev.set("Name", "room_closed");
                ev.set("RoomId", static_cast<long long>(room.id));
                ev.set("Reason", why);
                push_locked(m.online_id, std::move(ev));
            }
            rooms.erase(rit);
            return;
        }
        log("party host: member %u (%s) left room %llu (%s)", gone.id, gone.online_id.c_str(),
            static_cast<unsigned long long>(room.id), reason.c_str());
        for (const auto& [id, m] : room.members) {
            (void)id;
            json::Value ev = json::Value::make_object();
            ev.set("Name", "room_member_left");
            ev.set("RoomId", static_cast<long long>(room.id));
            ev.set("MemberId", static_cast<int>(gone.id));
            ev.set("OnlineId", gone.online_id);
            ev.set("Reason", reason);
            push_locked(m.online_id, std::move(ev));
        }
    }

    // Every room `online_id` is in (normally one).
    std::vector<std::pair<std::uint64_t, std::uint16_t>> memberships_locked(const std::string& online_id) const {
        std::vector<std::pair<std::uint64_t, std::uint16_t>> v;
        for (const auto& [rid, room] : rooms) {
            for (const auto& [mid, m] : room.members) {
                if (m.online_id == online_id) v.emplace_back(rid, mid);
            }
        }
        return v;
    }

    // The next member id: never 0 and never one in use (a u16 wraps after 65535 joins).
    static std::uint16_t next_member_id(Room& room) {
        for (int i = 0; i < 0x10000; ++i) {
            const std::uint16_t id = room.next_member++;
            if (id && id < 0xff00 && !room.members.count(id)) return id;
            if (room.next_member >= 0xff00) room.next_member = 1;
        }
        return 0;  // unreachable: a room holds a handful of members
    }

    std::string new_session_id() {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(rng()));
        return buf;
    }

    // --- the calls ---

    json::Value context_start(const Caller& who, const json::Value& rq) {
        std::string id;
        json::Value err;
        if (!caller_identity(who, rq, "context_start", &id, &err)) return err;
        Context c;
        c.online_id = id;
        c.ep.local_addr = addr_of(rq, "SignalingAddr");
        c.ep.local_port = port_of(rq, "SignalingPort");
        c.ep.mapped_addr = addr_of(rq, "MappedAddr");
        c.ep.mapped_port = port_of(rq, "MappedPort");
        c.ep.relay_port = port_of(rq, "RelayPort");
        c.link_addr = who.link_addr;
        c.at_ms = now();
        std::lock_guard<std::mutex> lk(mu);
        // A member registering a context while it is still in a room is a new game process (a
        // crash restart): its old membership ends now, not at the heartbeat timeout, so the other
        // games drop the dead peer (signaling DEAD, MEMBER_LEFT) before it is summoned again.
        const auto old_rooms = memberships_locked(id);
        if (contexts.count(id) || !old_rooms.empty()) restarted.push_back(id);
        for (const auto& [rid, mid] : old_rooms) {
            log("party host: %s started a new game while in room %llu; its old membership ends", id.c_str(),
                static_cast<unsigned long long>(rid));
            remove_member_locked(rid, mid, "restarted", nullptr);
        }
        contexts[id] = c;
        queues[id];  // the queue exists from now on
        log("party host: context %s at %s:%d (mapped %s:%d)", id.c_str(), c.ep.local_addr.c_str(), c.ep.local_port,
            c.ep.mapped_addr.c_str(), c.ep.mapped_port);
        json::Value r = ok_reply();
        r.set("OnlineId", id);
        // Our game's connections to it that are still resolving (it left and rejoined): now.
        np::signaling_peer_registered(id);
        return r;
    }

    json::Value create_room(const Caller& who, const json::Value& rq) {
        std::string id;
        json::Value err;
        if (!caller_identity(who, rq, "create_room", &id, &err)) return err;
        std::lock_guard<std::mutex> lk(mu);
        // A room it still has (the game left without LeaveRoom) closes first.
        for (const auto& [rid, mid] : memberships_locked(id)) remove_member_locked(rid, mid, "leave_room", nullptr);
        Room room;
        room.id = next_room++;
        room.session_id = new_session_id();
        const int asked = static_cast<int>(int_of(rq, "MaxMembers", 0));
        room.max_members = asked > 0 ? asked : party_max_members();
        Member m;
        m.id = room.next_member++;
        m.online_id = id;
        m.ep = endpoint_of(rq);
        m.last_heartbeat_ms = now();
        room.owner = m.id;
        copy_extra(rq, room.extra);
        room.members[m.id] = m;
        queues[id];
        const std::uint64_t rid = room.id;
        json::Value r = ok_reply();
        r.set("RoomId", static_cast<long long>(rid));
        r.set("SessionId", room.session_id);
        r.set("MemberId", static_cast<int>(m.id));
        r.set("MaxMembers", room.max_members);
        r.set("OwnerMemberId", static_cast<int>(room.owner));
        rooms[rid] = std::move(room);
        log("party host: room %llu created by %s (max %d)", static_cast<unsigned long long>(rid), id.c_str(),
            rooms[rid].max_members);
        return r;
    }

    json::Value join_room(const Caller& who, const json::Value& rq) {
        std::string id;
        json::Value err;
        if (!caller_identity(who, rq, "join_room", &id, &err)) return err;
        const auto rid = static_cast<std::uint64_t>(int_of(rq, "RoomId", 0));
        std::lock_guard<std::mutex> lk(mu);
        auto it = rooms.find(rid);
        if (it == rooms.end()) return error_reply(2, "Room not found");
        // Out of any other room; a second join of the same room replaces the first membership.
        for (const auto& [orid, omid] : memberships_locked(id)) remove_member_locked(orid, omid, "leave_room", nullptr);
        it = rooms.find(rid);
        if (it == rooms.end()) return error_reply(2, "Room not found");  // it was its own room
        Room& room = it->second;
        const int cap = std::min(room.max_members, party_max_members());
        if (static_cast<int>(room.members.size()) >= cap) return error_reply(3, "Room full");
        Member m;
        m.id = next_member_id(room);
        m.online_id = id;
        m.ep = endpoint_of(rq);
        m.last_heartbeat_ms = now();
        queues[id];
        json::Value members = json::Value::make_array();
        for (const auto& [mid, other] : room.members) members.push(record_locked(other.online_id, &other.ep, mid));
        room.members[m.id] = m;
        // Everyone else hears of the new member, with its addresses.
        const json::Value rec = record_locked(id, &m.ep, m.id);
        for (const auto& [mid, other] : room.members) {
            if (mid == m.id) continue;
            json::Value ev = rec;
            ev.set("Name", "room_member_joined");
            ev.set("RoomId", static_cast<long long>(room.id));
            push_locked(other.online_id, std::move(ev));
        }
        json::Value r = ok_reply();
        r.set("RoomId", static_cast<long long>(room.id));
        r.set("SessionId", room.session_id);
        r.set("MemberId", static_cast<int>(m.id));
        r.set("MaxMembers", room.max_members);
        r.set("OwnerMemberId", static_cast<int>(room.owner));
        r.set("Members", std::move(members));
        log("party host: %s joined room %llu as member %u (%zu members)", id.c_str(),
            static_cast<unsigned long long>(room.id), m.id, room.members.size());
        return r;
    }

    // A member record named by SessionId + MemberId that is not the caller's own: refused.
    json::Value not_yours(const Caller& who, const char* kind, const Member& m) const {
        log("party host: refused %s from %s for member %u (%s)", kind, who.online_id.c_str(), m.id,
            m.online_id.c_str());
        return error_reply(kResImpersonation, "that member is not the caller");
    }

    json::Value leave_room(const Caller& who, const json::Value& rq) {
        const std::string sid = str_of(rq, "SessionId");
        const auto mid = static_cast<std::uint16_t>(int_of(rq, "MemberId", 0));
        std::lock_guard<std::mutex> lk(mu);
        Member* m = nullptr;
        if (Room* room = find_session_locked(sid, mid, &m)) {
            if (!caller_is(who, m->online_id)) return not_yours(who, "leave_room", *m);
            remove_member_locked(room->id, mid, "leave_room", nullptr);
        }
        return ok_reply();
    }

    json::Value heartbeat(const Caller& who, const json::Value& rq) {
        const std::string sid = str_of(rq, "SessionId");
        const auto mid = static_cast<std::uint16_t>(int_of(rq, "MemberId", 0));
        std::lock_guard<std::mutex> lk(mu);
        Member* m = nullptr;
        json::Value r = ok_reply();
        if (find_session_locked(sid, mid, &m)) {
            if (!caller_is(who, m->online_id)) return not_yours(who, "heartbeat", *m);
            m->last_heartbeat_ms = now();
            r.set("InRoom", 1);
        } else {
            r.set("InRoom", 0);
        }
        return r;
    }

    json::Value kick_member(const Caller& who, const json::Value& rq) {
        const std::string sid = str_of(rq, "SessionId");
        const auto mid = static_cast<std::uint16_t>(int_of(rq, "MemberId", 0));
        const auto kicker = static_cast<std::uint16_t>(int_of(rq, "KickerMemberId", 0));
        std::lock_guard<std::mutex> lk(mu);
        Member* k = nullptr;
        Room* room = find_session_locked(sid, kicker, &k);
        if (!room) return error_reply(4, "not in that room");
        if (!caller_is(who, k->online_id)) return not_yours(who, "kick_member", *k);
        if (room->owner != kicker) return error_reply(5, "only the owner kicks");
        if (!room->members.count(mid) || mid == kicker) return error_reply(6, "no such member");
        std::string opt = str_of(rq, "OptData");
        if (opt.size() > 64) opt.clear();  // base64 of the game's 16 bytes
        remove_member_locked(room->id, mid, "kicked", &opt);
        return ok_reply();
    }

    json::Value signaling_resolve(const json::Value& rq) const {
        const std::string id = str_of(rq, "OnlineId");
        std::lock_guard<std::mutex> lk(mu);
        if (!contexts.count(id)) return error_reply(1, "not registered");
        // A room membership's endpoint fills what the context lacks.
        const Endpoint* mep = nullptr;
        for (const auto& [rid, room] : rooms) {
            (void)rid;
            for (const auto& [mid, m] : room.members) {
                (void)mid;
                if (m.online_id == id) mep = &m.ep;
            }
        }
        json::Value r = record_locked(id, mep, 0);
        if (str_of(r, "Addr").empty() || int_of(r, "Port", 0) <= 0) return error_reply(1, "no address");
        r.set("ResKind", 0);
        return r;
    }

    json::Value signaling_update(const Caller& who, const json::Value& rq) {
        std::string id;
        json::Value err;
        if (!caller_identity(who, rq, "signaling_update", &id, &err)) return err;
        std::lock_guard<std::mutex> lk(mu);
        Context& c = contexts[id];
        c.online_id = id;
        c.ep.mapped_addr = addr_of(rq, "MappedAddr");
        c.ep.mapped_port = port_of(rq, "MappedPort");
        if (rq.find("RelayPort")) c.ep.relay_port = port_of(rq, "RelayPort");
        if (who.link_addr) c.link_addr = who.link_addr;
        c.at_ms = now();
        log("party host: %s is now at %s:%d", id.c_str(), c.ep.mapped_addr.c_str(), c.ep.mapped_port);
        // Its rooms' other members get the new address (a member they know: an address refresh).
        for (auto& [rid, room] : rooms) {
            for (auto& [mid, m] : room.members) {
                if (m.online_id != id) continue;
                m.ep.mapped_addr = c.ep.mapped_addr;
                m.ep.mapped_port = c.ep.mapped_port;
                m.last_heartbeat_ms = now();
                const json::Value rec = record_locked(id, &m.ep, mid);
                for (const auto& [oid, other] : room.members) {
                    if (oid == mid) continue;
                    json::Value ev = rec;
                    ev.set("Name", "room_member_joined");
                    ev.set("RoomId", static_cast<long long>(rid));
                    push_locked(other.online_id, std::move(ev));
                }
            }
        }
        return ok_reply();
    }
};

PartyHostService::PartyHostService() : d_(std::make_shared<Impl>()) {}
PartyHostService::~PartyHostService() {
    d_->stop = true;
    wake_all();
}

PartyHostService& PartyHostService::instance() {
    static PartyHostService* s = [] {
        auto* p = new PartyHostService;  // lives as long as the process
        p->start_reaper();
        return p;
    }();
    return *s;
}

void PartyHostService::handle(const Caller& caller, const std::string& kind, const json::Value& rq, json::Value& reply) {
    if (kind == "context_start") reply = d_->context_start(caller, rq);
    else if (kind == "create_room") reply = d_->create_room(caller, rq);
    else if (kind == "join_room") reply = d_->join_room(caller, rq);
    else if (kind == "leave_room") reply = d_->leave_room(caller, rq);
    else if (kind == "heartbeat") reply = d_->heartbeat(caller, rq);
    else if (kind == "kick_member") reply = d_->kick_member(caller, rq);
    else if (kind == "signaling_resolve") reply = d_->signaling_resolve(rq);
    else if (kind == "signaling_update") reply = d_->signaling_update(caller, rq);
    else reply = error_reply(9, "unknown call " + kind);
    if (party_trace()) {
        log("party host: %s %s %s -> %s", caller.online_id.c_str(), kind.c_str(), json::dump(rq, 0).c_str(),
            json::dump(reply, 0).c_str());
    }
}

std::uint64_t PartyHostService::push_event(const std::string& online_id, json::Value event) {
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->push_locked(online_id, std::move(event));
}

std::vector<json::Value> PartyHostService::wait_events(const std::string& online_id, std::uint64_t cursor, int wait_ms,
                                                       std::size_t max) {
    std::vector<json::Value> out;
    std::unique_lock<std::mutex> lk(d_->mu);
    auto collect = [&] {
        auto it = d_->queues.find(online_id);
        if (it == d_->queues.end()) return false;
        for (const json::Value& ev : it->second.events) {
            if (static_cast<std::uint64_t>(int_of(ev, "EventId", 0)) > cursor) {
                out.push_back(ev);
                if (out.size() >= max) break;
            }
        }
        return !out.empty();
    };
    if (collect() || wait_ms <= 0) return out;
    const std::uint64_t gen = d_->wake_gen;
    d_->cv.wait_for(lk, std::chrono::milliseconds(wait_ms), [&] { return d_->wake_gen != gen || d_->stop; });
    collect();
    return out;
}

void PartyHostService::ack_events(const std::string& online_id, std::uint64_t cursor) {
    std::lock_guard<std::mutex> lk(d_->mu);
    auto it = d_->queues.find(online_id);
    if (it == d_->queues.end()) return;
    auto& q = it->second.events;
    while (!q.empty() && static_cast<std::uint64_t>(int_of(q.front(), "EventId", 0)) <= cursor) q.pop_front();
}

void PartyHostService::wake_all() {
    std::lock_guard<std::mutex> lk(d_->mu);
    ++d_->wake_gen;
    d_->cv.notify_all();
}

void PartyHostService::set_member_loading(const std::string& online_id, bool loading) {
    std::lock_guard<std::mutex> lk(d_->mu);
    if (loading) {
        d_->loading.insert(online_id);
    } else if (d_->loading.erase(online_id)) {
        // Back: its heartbeat clock starts over, it had no way to beat while loading.
        for (auto& [rid, room] : d_->rooms) {
            (void)rid;
            for (auto& [mid, m] : room.members) {
                (void)mid;
                if (m.online_id == online_id) m.last_heartbeat_ms = d_->now();
            }
        }
    }
}

void PartyHostService::set_loading_query(std::function<bool(const std::string&)> query) {
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->loading_query = std::move(query);
}

void PartyHostService::member_crashed(const std::string& online_id) {
    std::lock_guard<std::mutex> lk(d_->mu);
    const auto rooms = d_->memberships_locked(online_id);
    for (const auto& [rid, mid] : rooms) {
        log("party host: %s's game died while in room %llu; its membership ends", online_id.c_str(),
            static_cast<unsigned long long>(rid));
        d_->remove_member_locked(rid, mid, "restarted", nullptr);
    }
    if (!rooms.empty()) d_->restarted.push_back(online_id);
}

bool PartyHostService::member_timed_out(const std::string& online_id) {
    std::lock_guard<std::mutex> lk(d_->mu);
    const auto rooms = d_->memberships_locked(online_id);
    for (const auto& [rid, mid] : rooms) {
        log("party host: %s dropped by our game during an outage; its membership of room %llu ends now", online_id.c_str(),
            static_cast<unsigned long long>(rid));
        d_->remove_member_locked(rid, mid, "timeout", nullptr);
    }
    return !rooms.empty();
}

void PartyHostService::context_gone(const std::string& online_id) {
    std::lock_guard<std::mutex> lk(d_->mu);
    if (!d_->contexts.erase(online_id)) return;
    log("party host: %s's context is gone", online_id.c_str());
    for (const auto& [id, c] : d_->contexts) {
        (void)c;
        json::Value ev = json::Value::make_object();
        ev.set("Name", "peer_deactivated");
        ev.set("OnlineId", online_id);
        d_->push_locked(id, std::move(ev));
    }
}

std::vector<std::string> PartyHostService::take_restarted() {
    std::lock_guard<std::mutex> lk(d_->mu);
    std::vector<std::string> out;
    out.swap(d_->restarted);
    return out;
}

void PartyHostService::set_host_endpoint(const std::string& online_id, const std::string& addr, int port) {
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->host_online = online_id;
    d_->host_addr = addr;
    d_->host_port = port;
}

void PartyHostService::set_clock(std::function<std::int64_t()> now_ms) {
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->clock = now_ms ? std::move(now_ms) : steady_ms;
}

void PartyHostService::set_heartbeat_timeout_ms(int ms) {
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->heartbeat_timeout_ms = ms;
}

void PartyHostService::tick() {
    struct Late {
        std::uint64_t room;
        std::uint16_t member;
        std::string online_id;
    };
    std::vector<Late> late;
    std::function<bool(const std::string&)> query;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        const std::int64_t now = d_->now();
        for (const auto& [rid, room] : d_->rooms) {
            for (const auto& [mid, m] : room.members) {
                if (now - m.last_heartbeat_ms > d_->heartbeat_timeout_ms) late.push_back({rid, mid, m.online_id});
            }
        }
        query = d_->loading_query;
    }
    if (late.empty()) return;
    std::vector<bool> held(late.size(), false);
    for (std::size_t i = 0; i < late.size(); ++i) held[i] = query && query(late[i].online_id);
    std::lock_guard<std::mutex> lk(d_->mu);
    const std::int64_t now = d_->now();
    for (std::size_t i = 0; i < late.size(); ++i) {
        auto rit = d_->rooms.find(late[i].room);
        if (rit == d_->rooms.end()) continue;
        auto mit = rit->second.members.find(late[i].member);
        if (mit == rit->second.members.end() || now - mit->second.last_heartbeat_ms <= d_->heartbeat_timeout_ms) continue;
        if (held[i] || d_->loading.count(late[i].online_id)) {
            if (d_->held_late.insert(late[i].online_id).second || party_trace())
                log("party host: %s is loading or silent; its heartbeat timeout is held", late[i].online_id.c_str());
            continue;
        }
        if (d_->held_late.erase(late[i].online_id)) {
            // F5: back from an outage (or a load) that held its timeout: 0.6.1 removed it here, 0.6 s
            // after the link resumed, before its next heartbeat could arrive.
            log("party host: %s is back from a held heartbeat timeout; its clock starts over", late[i].online_id.c_str());
            mit->second.last_heartbeat_ms = now;
            continue;
        }
        d_->remove_member_locked(late[i].room, late[i].member, "timeout", nullptr);
    }
}

void PartyHostService::start_reaper() {
    if (d_->reaper.exchange(true)) return;
    std::shared_ptr<Impl> keep = d_;
    PartyHostService* self = this;
    std::thread([keep, self] {
        while (!keep->stop.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!keep->stop.load()) self->tick();
        }
    }).detach();
}

bool PartyHostService::room_of(const std::string& online_id, RoomView* out, bool owned_only) const {
    std::lock_guard<std::mutex> lk(d_->mu);
    for (const auto& [rid, room] : d_->rooms) {
        for (const auto& [mid, m] : room.members) {
            if (m.online_id != online_id || (owned_only && mid != room.owner)) continue;
            if (out) {
                out->room_id = rid;
                out->session_id = room.session_id;
                out->owner_id = room.owner;
                out->max_members = room.max_members;
                out->extra = room.extra;
                out->members.clear();
                for (const auto& [id2, m2] : room.members)
                    out->members.push_back({id2, m2.online_id, d_->loading.count(m2.online_id) != 0});
            }
            return true;
        }
    }
    return false;
}

bool PartyHostService::room(std::uint64_t room_id, RoomView* out) const {
    std::lock_guard<std::mutex> lk(d_->mu);
    auto it = d_->rooms.find(room_id);
    if (it == d_->rooms.end()) return false;
    if (out) {
        out->room_id = room_id;
        out->session_id = it->second.session_id;
        out->owner_id = it->second.owner;
        out->max_members = it->second.max_members;
        out->extra = it->second.extra;
        out->members.clear();
        for (const auto& [id2, m2] : it->second.members)
            out->members.push_back({id2, m2.online_id, d_->loading.count(m2.online_id) != 0});
    }
    return true;
}

bool PartyHostService::resolve(const std::string& online_id, json::Value* record) const {
    json::Value rq = json::Value::make_object();
    rq.set("OnlineId", online_id);
    json::Value r = d_->signaling_resolve(rq);
    if (int_of(r, "ResKind", -1) != 0) return false;
    if (record) *record = std::move(r);
    return true;
}

std::string PartyHostService::status_line() const {
    std::lock_guard<std::mutex> lk(d_->mu);
    std::size_t members = 0;
    for (const auto& [rid, room] : d_->rooms) {
        (void)rid;
        members += room.members.size();
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "host service: %zu context(s), %zu room(s), %zu member(s)", d_->contexts.size(),
                  d_->rooms.size(), members);
    return buf;
}

}  // namespace bbnet::party
