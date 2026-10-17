// SPDX-License-Identifier: GPL-3.0-or-later
// Party host harness: the party host's network side with no game - a PartyLink host, the
// PartyHostService + FromApi behind it (wired as gpu/shim/party/party_runtime.cpp wires them:
// guest RPCs -> host_handle, one event pump per member, context_gone on release, the loading
// query), the party UDP port (STUN responder + relay, hole-punch probes counted) and a small
// "host game" bot that plays what the host's game does for a summon: its NP context, its own
// Matching2 room (heartbeats), summon_messenger/get polling, summon_messenger/request for every
// new party sign (-> guest_invite), and its room events.
//
// For tools/party/simguest.py (a headless guest): run the harness, then point simguest at the
// printed port / party code. Build: ninja -C out/gpu party-host-harness
//
//   party-host-harness.exe [--port 9317] [--password PW] [--secret HEX16] [--name Host]
//                          [--max 3] [--eboot-sha256 HEX64] [--patches-hash HEX64]
//                          [--mods-hash HEX64] [--build-id TEXT] [--params-hash HEX64]
//                          [--hooks-hash HEX64] [--rules TEXT]   (A9 identity; default empty / zeros)
//                          [--advertise 127.0.0.1] [--duration SEC] [--no-udp] [--no-bot]
//                          [--fast] [--trace]
//
// stdout: "HARNESS READY port=N code=BBP1-... secret=HEX" once listening, then one line per
// link / service / bot event ("harness: ..."), "HARNESS STATUS ..." every 10 s and
// "HARNESS EXIT ..." at the end. --fast shrinks PartyLink's lost timeout (3 s) and slot keep
// (20 s) for quick tests.
#include "bbnet_internal.h"
#include "from_api.h"
#include "json.h"
#include "party_host_service.h"
#include "party_transport.h"
#include "party_util.h"

#include "party/party_code.h"
#include "party/party_crypto.h"
#include "party/party_link.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// --- Runtime stand-ins (src/probe.c, runtime_thread.c, runtime.c) ---
extern "C" {
BBNET_ABI void restore_guest_fs(void) {}
void runtime_thread_attach_host(const char*) {}
const char* runtime_symbol(const char*) { return nullptr; }
}

namespace {

using bbnet::party::Caller;
using bbnet::party::FromApi;
using bbnet::party::int_of;
using bbnet::party::PartyHostService;
using bbnet::party::str_of;
using party::LinkCallbacks;
using party::LinkConfig;
using party::PartyLink;
using party::RosterEntry;
using Clock = std::chrono::steady_clock;

std::mutex g_out_mu;
void say(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void say(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    for (std::size_t n = std::strlen(buf); n && (buf[n - 1] == '\n' || buf[n - 1] == '\r');) buf[--n] = 0;
    std::lock_guard<std::mutex> lk(g_out_mu);
    std::fprintf(stdout, "%s\n", buf);
    std::fflush(stdout);
}

void set_env(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

bool parse_hex(const std::string& s, std::uint8_t* out, std::size_t n) {
    if (s.size() != 2 * n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        unsigned v = 0;
        if (std::sscanf(s.c_str() + 2 * i, "%2x", &v) != 1) return false;
        out[i] = static_cast<std::uint8_t>(v);
    }
    return true;
}
std::string to_hex(const std::uint8_t* p, std::size_t n) {
    std::string s;
    char b[3];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof b, "%02x", p[i]);
        s += b;
    }
    return s;
}

json::Value obj(std::initializer_list<std::pair<const char*, json::Value>> kv) {
    json::Value o = json::Value::make_object();
    for (const auto& [k, v] : kv) o.set(k, v);
    return o;
}

struct Options {
    std::uint16_t port = 9317;
    std::string password;
    std::string secret_hex;
    std::string name = "Host";
    int max_players = 3;
    std::string eboot_hex, patches_hex, mods_hex;
    std::string build_id, params_hex, hooks_hex, rules;
    std::string advertise = "127.0.0.1";
    int duration_s = 0;
    bool udp = true;
    bool bot = true;
    bool fast = false;
    bool trace = false;
};

// ---- link wiring (party_runtime.cpp's, minus the game) -------------------------------------

struct Pump {
    std::string name;
    int slot = 0;
    std::atomic<bool> stop{false};
};

std::mutex g_mu;
PartyLink* g_link = nullptr;
std::map<std::string, std::shared_ptr<Pump>> g_pumps;
std::atomic<int> g_joins{0}, g_rejoins{0}, g_lost{0}, g_left{0}, g_rpcs{0};

void pump_main(std::shared_ptr<Pump> p) {
    PartyHostService& svc = PartyHostService::instance();
    std::uint64_t cursor = 0;
    while (!p->stop.load()) {
        std::vector<json::Value> evs = svc.wait_events(p->name, cursor, 1000);
        if (p->stop.load()) break;
        std::uint64_t last = cursor;
        for (const json::Value& ev : evs) {
            const auto id = static_cast<std::uint64_t>(int_of(ev, "EventId", 0));
            if (id && id <= cursor) continue;
            g_link->send_event(p->slot, str_of(ev, "Name"), json::dump(ev, 0));
            say("harness: event -> %s: %s", p->name.c_str(), json::dump(ev, 0).c_str());
            if (id > last) last = id;
        }
        if (last > cursor) {
            cursor = last;
            svc.ack_events(p->name, cursor);
        }
    }
}

void pump_start(const RosterEntry& m) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_pumps.find(m.name);
    if (it != g_pumps.end()) {
        if (it->second->slot == m.slot && !it->second->stop.load()) return;  // resumed: keep its cursor
        it->second->stop = true;
    }
    auto p = std::make_shared<Pump>();
    p->name = m.name;
    p->slot = m.slot;
    g_pumps[m.name] = p;
    std::thread(pump_main, p).detach();
}

void pump_stop(const std::string& name) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_pumps.find(name);
        if (it == g_pumps.end()) return;
        it->second->stop = true;
        g_pumps.erase(it);
    }
    PartyHostService::instance().wake_all();
}

LinkCallbacks make_callbacks(bool trace) {
    LinkCallbacks cb;
    cb.on_log = [](const std::string& line) { say("harness: link: %s", line.c_str()); };
    cb.on_member_joined = [](const RosterEntry& m, bool rejoined) {
        (rejoined ? g_rejoins : g_joins).fetch_add(1);
        say("harness: member %s %s (slot %d)", m.name.c_str(), rejoined ? "is back" : "joined", m.slot);
        pump_start(m);
    };
    cb.on_member_left = [](const RosterEntry& m, bool slot_kept) {
        FromApi::instance().drop_signs(m.name);  // as party_runtime.cpp
        if (slot_kept) {
            g_lost.fetch_add(1);
            say("harness: member %s (slot %d) lost; slot kept", m.name.c_str(), m.slot);
            return;
        }
        g_left.fetch_add(1);
        say("harness: member %s (slot %d) left", m.name.c_str(), m.slot);
        pump_stop(m.name);
        PartyHostService::instance().context_gone(m.name);
    };
    cb.on_rpc = [trace](int slot, const std::string& kind, const std::string& body) -> std::string {
        g_rpcs.fetch_add(1);
        Caller caller;
        if (g_link) {
            for (const RosterEntry& e : g_link->roster())
                if (e.slot == slot) caller.online_id = e.name;
            caller.link_addr = g_link->member_ip(slot);
        }
        json::Value req, reply;
        std::string err;
        if (caller.online_id.empty()) {  // no authenticated roster name: it cannot act as anyone
            reply = json::Value::make_object();
            reply.set("ResKind", 7);
            reply.set("Error", "caller not authenticated");
            return json::dump(reply, 0);
        }
        if (!json::parse(body, req, err)) {
            reply = json::Value::make_object();
            reply.set("ResKind", 1);
            reply.set("Error", "bad request: " + err);
            return json::dump(reply, 0);
        }
        bbnet::party::host_handle(caller, kind, req, reply);
        std::string out = json::dump(reply, 0);
        if (trace) say("harness: rpc %s %s %s -> %s", caller.online_id.c_str(), kind.c_str(), body.c_str(), out.c_str());
        return out;
    };
    cb.on_event = [](int from, std::uint64_t cursor, const std::string& name, const std::string& body) {
        say("harness: event from slot %d #%llu %s %s", from, static_cast<unsigned long long>(cursor), name.c_str(),
            body.c_str());
    };
    cb.on_party_cmd = [](int from, const std::string& cmd, const std::string& body) {
        say("harness: party cmd from slot %d: %s %s", from, cmd.c_str(), body.c_str());
    };
    return cb;
}

// ---- the host's game, in miniature ----------------------------------------------------------

struct HostBot {
    std::string name, advertise;
    int port = 0, max_members = 3;
    std::atomic<bool> stop{false};
    std::string session_id;
    int member_id = 0;
    std::uint64_t room_id = 0;
    std::uint64_t user_id = 0;
    std::set<std::uint64_t> requested;  // sign ids already summoned
    std::uint64_t cursor = 0;

    json::Value svc_call(const char* kind, const json::Value& rq) {
        json::Value reply;
        bbnet::party::host_handle(Caller{name, 0}, kind, rq, reply);
        return reply;
    }
    json::Value api_post(const std::string& path, const json::Value& body) {
        bbnet::party::HttpRequest rq;
        rq.method = "POST";
        rq.url = std::string("http://") + bbnet::party::kGameHost + ":18671" + path +
                 (user_id ? "?user_id=" + std::to_string(user_id) : std::string());
        rq.body = json::dump(body, 0);
        bbnet::party::HttpResponse r;
        FromApi::instance().handle(Caller{name, 0}, rq, r);
        json::Value out;
        std::string err;
        if (r.status != 200 || !json::parse(r.body, out, err)) return json::Value::make_object();
        return out;
    }

    bool make_room() {
        json::Value r = svc_call("create_room",
                                 obj({{"OnlineId", name},
                                      {"LocalAddr", advertise},
                                      {"LocalPort", port},
                                      {"PublicAddr", advertise},
                                      {"PublicPort", port},
                                      {"MaxMembers", max_members},
                                      {"HostArea", 0x18010000LL},
                                      {"HostLevel", 50},
                                      {"MemberTag", 1}}));
        if (int_of(r, "ResKind", -1) != 0) {
            say("harness: bot: create_room failed: %s", json::dump(r, 0).c_str());
            return false;
        }
        room_id = static_cast<std::uint64_t>(int_of(r, "RoomId", 0));
        session_id = str_of(r, "SessionId");
        member_id = static_cast<int>(int_of(r, "MemberId", 0));
        say("harness: bot: room %llu (session %s, member %d)", static_cast<unsigned long long>(room_id),
            session_id.c_str(), member_id);
        return true;
    }

    void run() {
        svc_call("context_start",
                 obj({{"OnlineId", name}, {"SignalingAddr", advertise}, {"SignalingPort", port}}));
        PartyHostService::instance().set_host_endpoint(name, advertise, port);
        json::Value login = api_post("/basic_utils/login", obj({{"MessageId", "LoginRequest"}}));
        user_id = static_cast<std::uint64_t>(int_of(login, "UserId", 0));
        say("harness: bot: logged in as user %llu", static_cast<unsigned long long>(user_id));
        make_room();
        auto last_hb = Clock::now();
        while (!stop.load()) {
            // Room events for the host (its game's Matching2 callbacks).
            std::vector<json::Value> evs = PartyHostService::instance().wait_events(name, cursor, 500);
            for (const json::Value& ev : evs) {
                const auto id = static_cast<std::uint64_t>(int_of(ev, "EventId", 0));
                if (id > cursor) cursor = id;
                const std::string n = str_of(ev, "Name");
                say("harness: bot: event %s: %s", n.c_str(), json::dump(ev, 0).c_str());
                if (n == "room_closed" && static_cast<std::uint64_t>(int_of(ev, "RoomId", 0)) == room_id) make_room();
            }
            if (!evs.empty()) PartyHostService::instance().ack_events(name, cursor);
            if (Clock::now() - last_hb >= std::chrono::seconds(2)) {
                last_hb = Clock::now();
                json::Value hb = svc_call("heartbeat", obj({{"SessionId", session_id}, {"MemberId", member_id}}));
                if (int_of(hb, "InRoom", 0) != 1) {
                    say("harness: bot: no longer in room %llu; making a new one",
                        static_cast<unsigned long long>(room_id));
                    make_room();
                }
            }
            // The sign board, as the host's game polls it.
            json::Value tl = json::Value::make_array();
            tl.push(obj({{"SummonType", 0}, {"GetLimitCount", 5}}));
            json::Value got = api_post("/summon_messenger/get",
                                       obj({{"MessageId", "SummonDataGetListRequest"},
                                            {"SessionId", "bbp-host"},
                                            {"UserId", static_cast<long long>(user_id)},
                                            {"SummonTypeList", tl},
                                            {"GetMaxCount", 20}}));
            const json::Value* list = got.find("SummonDataList");
            if (!list || list->type != json::Value::Type::Array) {
                say("harness: bot: BAD summon_messenger/get answer: %s", json::dump(got, 0).c_str());
                continue;
            }
            for (const json::Value& s : list->array) {
                const auto sid = static_cast<std::uint64_t>(int_of(s, "SummonDataId", 0));
                if (requested.count(sid)) continue;
                requested.insert(sid);
                const std::vector<std::uint8_t> blob = bbnet::party::b64_decode(str_of(s, "SummonData"));
                const long long ver = int_of(s, "SummonDataVersion", 0);
                const bool ok = ver == 3 && blob.size() == 0xE0;
                std::string owner;
                if (blob.size() == 0xE0) owner.assign(reinterpret_cast<const char*>(blob.data() + 0x40), strnlen(reinterpret_cast<const char*>(blob.data() + 0x40), 16));
                say("harness: bot: sign %llu from %s (user %lld, type %lld, version %lld, %zu bytes, blob owner '%s'): %s",
                    static_cast<unsigned long long>(sid), str_of(s, "OnlineId").c_str(), int_of(s, "UserId", 0),
                    int_of(s, "SummonType", -1), ver, blob.size(), owner.c_str(),
                    ok ? "valid" : "INVALID (the game would skip it)");
                if (!ok) continue;
                json::Value rq = api_post("/summon_messenger/request",
                                          obj({{"MessageId", "SummonDataSummonRequest"},
                                               {"SessionId", "bbp-host"},
                                               {"UserId", static_cast<long long>(user_id)},
                                               {"CharaId", 1},
                                               {"TargetUserId", int_of(s, "UserId", 0)},
                                               {"TargetCharaId", int_of(s, "CharaId", 0)},
                                               {"SummonDataId", static_cast<long long>(sid)}}));
                say("harness: bot: summon_messenger/request for sign %llu -> %s", static_cast<unsigned long long>(sid),
                    json::dump(rq, 0).c_str());
            }
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    // Loopback-only sockets: no Windows Firewall prompt for a test program (party_sock.h).
    if (!std::getenv("BB_PARTY_LOOPBACK")) {
#if defined(_WIN32)
        _putenv("BB_PARTY_LOOPBACK=1");
#else
        setenv("BB_PARTY_LOOPBACK", "1", 1);
#endif
    }
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--port") o.port = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        else if (a == "--password") o.password = next();
        else if (a == "--secret") o.secret_hex = next();
        else if (a == "--name") o.name = next();
        else if (a == "--max") o.max_players = std::atoi(next().c_str());
        else if (a == "--eboot-sha256") o.eboot_hex = next();
        else if (a == "--patches-hash") o.patches_hex = next();
        else if (a == "--mods-hash") o.mods_hex = next();
        else if (a == "--build-id") o.build_id = next();
        else if (a == "--params-hash") o.params_hex = next();
        else if (a == "--hooks-hash") o.hooks_hex = next();
        else if (a == "--rules") o.rules = next();
        else if (a == "--advertise") o.advertise = next();
        else if (a == "--duration") o.duration_s = std::atoi(next().c_str());
        else if (a == "--no-udp") o.udp = false;
        else if (a == "--no-bot") o.bot = false;
        else if (a == "--fast") o.fast = true;
        else if (a == "--trace") o.trace = true;
        else {
            std::fprintf(stderr, "unknown argument %s (see the header of tests/party_host_harness.cpp)\n", a.c_str());
            return 2;
        }
    }
    if (o.port == 0) {
        std::fprintf(stderr, "--port must be a fixed port (the UDP port shares it)\n");
        return 2;
    }
    char port_text[16];
    std::snprintf(port_text, sizeof port_text, "%u", o.port);
    // Before anything reads bbnet::settings().
    set_env("BB_PARTY", "host");
    set_env("BB_PARTY_PORT", port_text);
    set_env("BB_PARTY_NAME", o.name.c_str());
    if (o.trace) set_env("BB_PARTY_TRACE", "1");
    std::string mx = std::to_string(o.max_players);
    set_env("BB_PARTY_MAX", mx.c_str());

    LinkConfig cfg;
    cfg.name = o.name;
    cfg.port = o.port;
    cfg.password = o.password;
    cfg.max_players = o.max_players;
    if (!o.eboot_hex.empty() && !parse_hex(o.eboot_hex, cfg.eboot_sha256.data(), 32)) {
        std::fprintf(stderr, "--eboot-sha256 wants 64 hex digits\n");
        return 2;
    }
    if (!o.patches_hex.empty() && !parse_hex(o.patches_hex, cfg.patches_hash.data(), 32)) {
        std::fprintf(stderr, "--patches-hash wants 64 hex digits\n");
        return 2;
    }
    if (!o.mods_hex.empty() && !parse_hex(o.mods_hex, cfg.mods_hash.data(), 32)) {
        std::fprintf(stderr, "--mods-hash wants 64 hex digits\n");
        return 2;
    }
    if ((!o.params_hex.empty() && !parse_hex(o.params_hex, cfg.params_sha256.data(), 32)) ||
        (!o.hooks_hex.empty() && !parse_hex(o.hooks_hex, cfg.hooks_hash.data(), 32))) {
        std::fprintf(stderr, "--params-hash / --hooks-hash want 64 hex digits\n");
        return 2;
    }
    cfg.build_id = o.build_id;
    cfg.rules = o.rules;
    if (!o.secret_hex.empty()) {
        if (!parse_hex(o.secret_hex, cfg.secret.data(), 8)) {
            std::fprintf(stderr, "--secret wants 16 hex digits\n");
            return 2;
        }
    } else {
        party::crypto::random_bytes(cfg.secret.data(), cfg.secret.size());
    }
    if (o.fast) {
        cfg.lost_timeout_ms = 3000;
        cfg.slot_keep_ms = 20000;
    }

    if (o.udp && !bbnet::p2p_open()) say("harness: warning: cannot open the party UDP port %u", o.port);

    auto* link = new PartyLink(cfg, make_callbacks(o.trace));  // lives as long as the process
    g_link = link;
    std::string err;
    if (!link->start_host(&err)) {
        say("HARNESS FAIL cannot host on TCP port %u: %s", o.port, err.c_str());
        return 1;
    }
    PartyHostService& svc = PartyHostService::instance();
    svc.set_loading_query([](const std::string& id) {
        if (!g_link) return false;
        for (const RosterEntry& e : g_link->roster())
            if (e.name == id) return e.state == party::MemberState::Loading || !e.connected;
        return false;
    });
    link->set_local_state(party::MemberState::InHostWorld, 0x18010000u);

    std::array<std::uint8_t, 4> lo{127, 0, 0, 1};
    std::array<std::uint8_t, 4> adv{};
    if (!party::parse_ipv4(o.advertise, &adv)) adv = lo;
    const std::uint8_t flags = static_cast<std::uint8_t>(party::kCodeFlagLan |
                                                         (o.password.empty() ? 0 : party::kCodeFlagPasswordRequired));
    const std::string code = party::make_party_code(adv, link->bound_port(), cfg.secret, flags);
    say("HARNESS READY port=%u code=%s secret=%s name=%s udp=%u", link->bound_port(), code.c_str(),
        to_hex(cfg.secret.data(), 8).c_str(), o.name.c_str(), static_cast<unsigned>(bbnet::p2p_bound_port()));

    HostBot bot;
    bot.name = o.name;
    bot.advertise = o.advertise;
    bot.port = o.port;
    bot.max_members = o.max_players;
    std::thread bot_thread;
    if (o.bot) bot_thread = std::thread([&] { bot.run(); });

    const auto t0 = Clock::now();
    auto last_status = t0;
    while (o.duration_s <= 0 || Clock::now() - t0 < std::chrono::seconds(o.duration_s)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (Clock::now() - last_status >= std::chrono::seconds(10)) {
            last_status = Clock::now();
            std::string roster;
            for (const RosterEntry& e : link->roster()) {
                char b[128];
                std::snprintf(b, sizeof b, "%s%s(slot %d, %s%s, %u ms)", roster.empty() ? "" : ", ", e.name.c_str(),
                              e.slot, party::member_state_name(e.state), e.connected ? "" : ", lost", e.ping_ms);
                roster += b;
            }
            say("HARNESS STATUS roster=[%s] joins=%d rejoins=%d lost=%d left=%d rpcs=%d | %s | %s", roster.c_str(),
                g_joins.load(), g_rejoins.load(), g_lost.load(), g_left.load(), g_rpcs.load(),
                svc.status_line().c_str(), bbnet::p2p_status().c_str());
        }
    }
    bot.stop = true;
    svc.wake_all();
    if (bot_thread.joinable()) bot_thread.join();
    say("HARNESS EXIT joins=%d rejoins=%d lost=%d left=%d rpcs=%d | %s", g_joins.load(), g_rejoins.load(),
        g_lost.load(), g_left.load(), g_rpcs.load(), bbnet::p2p_status().c_str());
    link->stop(true);
    std::fflush(stdout);
    std::_Exit(0);  // detached pumps may still wait in the service
}
