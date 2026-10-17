// SPDX-License-Identifier: GPL-3.0-or-later
// PartyTransport implementations (party_transport.h): LocalHost over the in-process host
// service, RemoteGuest over PartyLink (A4 plugs its RPC and EVENT frames in).
#include "party_transport.h"

#include "bbnet_internal.h"
#include "from_api.h"
#include "party_host_service.h"
#include "party_util.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace bbnet::party {

bool PartyTransport::ok_call(const char* kind, const json::Value& request, json::Value& reply, std::string& error,
                             int timeout_ms) {
    if (!rpc(kind, request, reply, error, timeout_ms)) return false;
    if (int_of(reply, "ResKind", -1) != 0) {
        error = str_of(reply, "Error");
        if (error.empty()) error = "refused (ResKind " + std::to_string(int_of(reply, "ResKind", -1)) + ")";
        return false;
    }
    return true;
}

// The one entry point for a call that reached the host (in-process or over PartyLink).
void host_handle(const Caller& caller, const std::string& kind, const json::Value& request, json::Value& reply) {
    if (kind == call::kHttp) {
        FromApi::instance().handle_json(caller, request, reply);
        return;
    }
    PartyHostService::instance().handle(caller, kind, request, reply);
}

// ---- LocalHost -----------------------------------------------------------------------------

struct LocalHost::Pump {
    std::mutex mu;
    std::atomic<bool> run{false};
    std::thread thread;
    EventHandler handler;
    std::string online_id;
};

LocalHost::LocalHost(PartyHostService& service, FromApi* api)
    : service_(service), api_(api), pump_(std::make_unique<Pump>()) {}

LocalHost::~LocalHost() { stop_events(); }

bool LocalHost::rpc(const char* kind, const json::Value& request, json::Value& reply, std::string& error, int) {
    Caller me{settings().online_id, 0};
    if (std::string(kind) == call::kHttp) {
        // The host's own game: its FROM API is answered here (http_hle calls FromApi directly;
        // this path serves tests and tools).
        if (!api_) {
            error = "no FromApi on this transport";
            return false;
        }
        api_->handle_json(me, request, reply);
        return true;
    }
    service_.handle(me, kind, request, reply);
    return true;
}

void LocalHost::start_events(const std::string& online_id, EventHandler handler) {
    stop_events();
    Pump& p = *pump_;
    {
        std::lock_guard<std::mutex> lk(p.mu);
        p.handler = std::move(handler);
        p.online_id = online_id;
    }
    p.run = true;
    PartyHostService* svc = &service_;
    Pump* pp = &p;
    p.thread = std::thread([svc, pp] {
        std::uint64_t cursor = 0;
        std::string id;
        {
            std::lock_guard<std::mutex> lk(pp->mu);
            id = pp->online_id;
        }
        while (pp->run.load()) {
            std::vector<json::Value> evs = svc->wait_events(id, cursor, 250);
            for (const json::Value& ev : evs) {
                if (!pp->run.load()) break;
                const auto eid = static_cast<std::uint64_t>(int_of(ev, "EventId", 0));
                EventHandler h;
                {
                    std::lock_guard<std::mutex> lk(pp->mu);
                    h = pp->handler;
                }
                if (party_trace()) log("party: event #%llu %s", static_cast<unsigned long long>(eid),
                                       str_of(ev, "Name").c_str());
                if (h) h(ev);
                cursor = eid > cursor ? eid : cursor;
                svc->ack_events(id, cursor);
            }
        }
    });
}

void LocalHost::stop_events() {
    Pump& p = *pump_;
    if (!p.run.exchange(false)) return;
    service_.wake_all();
    if (p.thread.joinable()) {
        if (p.thread.get_id() == std::this_thread::get_id()) p.thread.detach();
        else p.thread.join();
    }
}

// ---- RemoteGuest ---------------------------------------------------------------------------

struct RemoteGuest::State {
    std::mutex mu;
    RpcFn rpc;
    std::uint32_t host_addr = 0;
    std::uint16_t host_port = 0;
    bool host_loopback = false;
    std::string host_online;
    EventHandler handler;
    std::string online_id;
    std::uint64_t last_event = 0;
    bool events_on = false;
    std::shared_ptr<LocalHost> solo;  // set_solo(true)
};

RemoteGuest::RemoteGuest() : st_(std::make_shared<State>()) {}

void RemoteGuest::set_rpc(RpcFn fn) {
    std::lock_guard<std::mutex> lk(st_->mu);
    st_->rpc = std::move(fn);
}

void RemoteGuest::set_host_endpoint(std::uint32_t addr_nbo, std::uint16_t port, bool loopback) {
    std::lock_guard<std::mutex> lk(st_->mu);
    st_->host_addr = addr_nbo;
    st_->host_port = port;
    st_->host_loopback = loopback;
}

void RemoteGuest::set_host_online_id(const std::string& id) {
    std::lock_guard<std::mutex> lk(st_->mu);
    st_->host_online = id;
}

bool RemoteGuest::host_is_local() const {
    std::lock_guard<std::mutex> lk(st_->mu);
    return st_->host_loopback || st_->solo;  // solo: our own service is the host
}

// A record (or invite) naming the host with no address this guest can use gets the address
// PartyLink reached the host at.
void RemoteGuest::fix_host_addresses(json::Value& v) const {
    std::uint32_t addr;
    std::uint16_t port;
    bool loopback;
    {
        std::lock_guard<std::mutex> lk(st_->mu);
        addr = st_->host_addr;
        port = st_->host_port;
        loopback = st_->host_loopback;
    }
    if (!addr) return;
    auto unusable = [&](const std::string& a) {
        const std::uint32_t x = ip_parse(a);
        return !x || (ip_is_loopback(x) && !loopback);
    };
    // The link reached the host on a LAN / VPN address: game traffic takes that path too, never
    // the advertised public endpoint (a router without hairpin NAT drops it; live: the bell
    // join timed out after 48 s with 9 datagrams each way).
    const bool lan_link = ip_is_private(addr);
    auto fix_record = [&](json::Value& r, const char* addr_key, const char* port_key) {
        if (r.type != json::Value::Type::Object) return;
        const std::string have = str_of(r, addr_key);
        if (lan_link && ip_parse(have) != addr) {
            r.set(addr_key, ip_text(addr));
            r.set(port_key, static_cast<int>(port));
            return;
        }
        if (!unusable(have)) return;
        r.set(addr_key, ip_text(addr));
        if (int_of(r, port_key, 0) <= 0) r.set(port_key, static_cast<int>(port));
    };
    if (str_of(v, "Name").empty() && v.find("ResKind") && !v.find("Members") && !v.find("RoomId")) {  // signaling_resolve
        std::string host_online;
        {
            std::lock_guard<std::mutex> lk(st_->mu);
            host_online = st_->host_online;
        }
        if (!host_online.empty() && str_of(v, "OnlineId") == host_online) fix_record(v, "Addr", "Port");
        return;
    }
    if (str_of(v, "Name") == "guest_invite") {
        fix_record(v, "HostAddr", "HostPort");
        return;
    }
    // The host is the room's owner (member 1 in every room it made) and the only member whose
    // records the host service fills from its own endpoint.
    const long long owner = int_of(v, "OwnerMemberId", 1);
    if (json::Value* members = const_cast<json::Value*>(v.find("Members"))) {
        if (members->type == json::Value::Type::Array) {
            for (json::Value& m : members->array) {
                if (int_of(m, "MemberId", 0) == owner) fix_record(m, "Addr", "Port");
            }
        }
    }
    if (str_of(v, "Name") == "room_member_joined" && int_of(v, "MemberId", 0) == owner) fix_record(v, "Addr", "Port");
}

bool RemoteGuest::rpc(const char* kind, const json::Value& request, json::Value& reply, std::string& error,
                      int timeout_ms) {
    RpcFn fn;
    std::shared_ptr<LocalHost> solo;
    {
        std::lock_guard<std::mutex> lk(st_->mu);
        fn = st_->rpc;
        solo = st_->solo;
    }
    if (solo) return solo->rpc(kind, request, reply, error, timeout_ms);
    if (!fn) {
        error = "no party link";  // TODO(A4): PartyLink::rpc_call
        return false;
    }
    std::string reply_json;
    if (!fn(kind, json::dump(request, 0), reply_json, error, timeout_ms)) return false;
    if (!json::parse(reply_json, reply, error)) {
        error = "bad reply: " + error;
        return false;
    }
    if (std::string(kind) == call::kJoinRoom || std::string(kind) == call::kSignalingResolve)
        fix_host_addresses(reply);
    return true;
}

std::uint64_t RemoteGuest::on_link_event(const std::string& event_json) {
    json::Value ev;
    std::string err;
    if (!json::parse(event_json, ev, err) || ev.type != json::Value::Type::Object) return 0;
    const auto id = static_cast<std::uint64_t>(int_of(ev, "EventId", 0));
    EventHandler h;
    {
        std::lock_guard<std::mutex> lk(st_->mu);
        if (!st_->events_on) return id;  // nothing listens: acknowledged and dropped
        if (id && id <= st_->last_event) return id;  // a resend
        if (id) st_->last_event = id;
        h = st_->handler;
    }
    fix_host_addresses(ev);
    if (h) h(ev);
    return id;
}

void RemoteGuest::reset_event_cursor() {
    std::lock_guard<std::mutex> lk(st_->mu);
    st_->last_event = 0;
}

void RemoteGuest::start_events(const std::string& online_id, EventHandler handler) {
    std::shared_ptr<LocalHost> solo;
    {
        std::lock_guard<std::mutex> lk(st_->mu);
        st_->online_id = online_id;
        st_->handler = handler;
        st_->events_on = true;
        solo = st_->solo;
    }
    if (solo) solo->start_events(online_id, std::move(handler));
}

void RemoteGuest::stop_events() {
    std::shared_ptr<LocalHost> solo;
    {
        std::lock_guard<std::mutex> lk(st_->mu);
        st_->events_on = false;
        st_->handler = nullptr;
        solo = st_->solo;
    }
    if (solo) solo->stop_events();
}

bool RemoteGuest::set_solo(bool on) {
    std::shared_ptr<LocalHost> old, fresh;
    std::string id;
    EventHandler h;
    bool events = false;
    {
        std::lock_guard<std::mutex> lk(st_->mu);
        if (on == static_cast<bool>(st_->solo)) return false;
        if (on) {
            fresh = std::make_shared<LocalHost>(PartyHostService::instance(), &FromApi::instance());
            st_->solo = fresh;
            events = st_->events_on;
            id = st_->online_id;
            h = st_->handler;
        } else {
            old = std::move(st_->solo);
            st_->solo.reset();
        }
    }
    if (fresh && events && h) fresh->start_events(id, std::move(h));
    if (old) old->stop_events();
    log("party transport: %s", on ? "solo (this game's own host service: the party host refused us)"
                                  : "remote-guest again (the party link is up)");
    return true;
}

bool RemoteGuest::solo() const {
    std::lock_guard<std::mutex> lk(st_->mu);
    return static_cast<bool>(st_->solo);
}

// ---- the process's transport ---------------------------------------------------------------

namespace {
std::mutex g_tmu;
std::shared_ptr<PartyTransport> g_transport;
RemoteGuest* g_remote = nullptr;
}  // namespace

PartyTransport& transport() {
    std::lock_guard<std::mutex> lk(g_tmu);
    if (!g_transport) {
        if (settings().host) {
            g_transport = std::make_shared<LocalHost>(PartyHostService::instance(), &FromApi::instance());
        } else {
            auto rg = std::make_shared<RemoteGuest>();
            g_remote = rg.get();
            g_transport = rg;
        }
        log("party transport: %s", g_transport->name());
    }
    return *g_transport;
}

void set_transport(std::shared_ptr<PartyTransport> t) {
    std::lock_guard<std::mutex> lk(g_tmu);
    g_transport = std::move(t);
    g_remote = dynamic_cast<RemoteGuest*>(g_transport.get());
}

RemoteGuest* remote_guest() {
    transport();
    std::lock_guard<std::mutex> lk(g_tmu);
    return g_remote;
}

}  // namespace bbnet::party
