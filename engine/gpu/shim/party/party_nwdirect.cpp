// SPDX-License-Identifier: GPL-3.0-or-later
// The opt-in Nightwatch direct path's driver (party_nwdirect.h). Rules: net/party_nw_direct_rules.h.
#include "party/party_nwdirect.h"

#include "bbport_settings.h"
#include "party/upnp_win.h"
#include "../net/bbnet_internal.h"
#include "../net/json.h"
#include "../net/party_nw_direct_rules.h"
#include "../net/party_util.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace party::nwdirect {

namespace {

using Clock = std::chrono::steady_clock;
namespace nwd = bbnet::nwd;
using bbnet::party::int_of;
using bbnet::party::ip_parse;
using bbnet::party::ip_text;
using bbnet::party::str_of;

constexpr int kOptWaitMs = 20000;      // the peer never said it allows a direct path: not opted in
constexpr int kDoneWaitMs = 30000;     // responder: the decider never confirmed after we switched

std::uint16_t sw16(std::uint16_t v) { return static_cast<std::uint16_t>((v << 8) | (v >> 8)); }
std::uint32_t sw32(std::uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24);
}
std::int64_t ms_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
}

struct Peer {
    int slot = -1;
    nwd::Pair pair;
    bool opt_sent = false;
    bool peer_known = false;            // its flag arrived
    std::uint32_t tun = 0;              // its tunnel address, network order
    std::uint16_t port = 0;             // its party port, network order
    bool have_cand = false;
    std::uint32_t cand = 0;             // its public candidate, network order
    std::uint16_t cport = 0;
    bool cand_sent = false;
    Clock::time_point up{};             // when we first saw it
    Clock::time_point next_probe{}, last_probe{}, switched{}, next_keep{};
    int probes_sent = 0;
    std::uint32_t base = 0;             // seq >> 8 of this pair
    nwd::PathStats st[2];               // 0 tunnel, 1 direct
    std::map<std::uint32_t, Clock::time_point> sent_at;
    bool judged = false;
    nwd::Verdict verdict = nwd::Verdict::NoProbes;
    double tms = 0, dms = 0;
    bool res_sent = false, peer_res = false, peer_ok = false;
    bool go_sent = false, go_recv = false, ready_recv = false, done_recv = false;
    bool applied = false, logged = false;
    int keep_ctr = 0;
    int peer_state = 0;                 // its roster state (party::MemberState)
    std::int64_t heard_ms = 0;          // last keepalive ack, ms since epoch
};

struct Out {
    int slot;
    std::string body;
};

struct State {
    std::mutex mu;
    Env env;
    bool started = false;
    std::atomic<bool> stop{false};
    std::thread worker;
    PartyLink* link = nullptr;          // the link the peers belong to
    bool enabled = false;               // read when the link came up
    bool active = false;                // enabled and over the tunnel
    std::map<int, Peer> peers;
    Clock::time_point epoch = Clock::now();
    // The local candidate.
    int gather = 0;                     // 0 not started, 1 running, 2 ok, 3 failed
    std::uint32_t cand = 0;
    std::uint16_t cport = 0;
    std::unique_ptr<UpnpMapper> upnp;
    std::thread gatherer;
    std::mt19937 rng{std::random_device{}()};
};

State& S() {
    static State* s = new State;
    return *s;
}

void log(const std::string& m) {
    std::function<void(const std::string&)> f;
    {
        std::lock_guard<std::mutex> lk(S().mu);
        f = S().env.log;
    }
    if (f) f(m);
}

std::int64_t now_ms() { return ms_between(S().epoch, Clock::now()); }

// ---- wire ----

std::atomic<int> g_me{-1};  // our link slot, stamped as "From" (the host re-stamps a guest's)

std::string body_of(const char* type, int to, int from_host_stamp, const std::map<std::string, std::string>& num,
                    const std::map<std::string, std::string>& str) {
    json::Value v = json::Value::make_object();
    v.set("T", type);
    v.set("To", to);
    if (from_host_stamp >= 0) v.set("From", from_host_stamp);
    for (const auto& [k, x] : num) v.set(k, std::atoll(x.c_str()));
    for (const auto& [k, x] : str) v.set(k, x);
    return json::dump(v, 0);
}

// The fields every message carries: our tunnel endpoint (a guest's is re-stamped by the host).
std::map<std::string, std::string> self_str() { return {{"Tun", ip_text(bbnet::local_ipv4())}}; }
std::map<std::string, std::string> self_num() { return {{"Port", std::to_string(bbnet::settings().party_port)}}; }

void queue(std::vector<Out>& out, const char* type, int to, std::map<std::string, std::string> num = {},
           std::map<std::string, std::string> str = {}) {
    auto sn = self_num();
    sn.insert(num.begin(), num.end());
    auto ss = self_str();
    ss.insert(str.begin(), str.end());
    out.push_back({to, body_of(type, to, g_me.load(), sn, ss)});
}

void flush(PartyLink* link, std::vector<Out>& out) {
    for (const Out& o : out) link->send_event(o.slot, kEventName, o.body);
    out.clear();
}

// ---- candidate ----

void gather_thread() {
    std::uint32_t addr = 0;
    std::uint16_t port = 0;
    bool ok = false;
    const std::uint16_t own = bbnet::settings().party_port;
    std::string how;
    if (bbnet::nwd_test_mode()) {
        addr = 0x0200007fu;  // 127.0.0.2: the test aid's "WAN" address, no STUN / UPnP query
        port = sw16(own);
        ok = true;
        how = "test address";
    } else {
        // UPnP first: a mapping opens the game's party port on the router, so the peer's probes get in whatever the
        // router does with unsolicited packets (the plain UPnP party path works where the STUN one lost 20/20, 12 Nov).
        // Only a public external address counts (a router behind carrier NAT reports 100.64.x or a private one).
        if (UpnpMapper::enabled_by_env() && !S().stop.load()) {
            auto m = std::make_unique<UpnpMapper>();
            m->start(own);
            const UpnpResult u = m->wait(3000);
            const std::uint32_t ip = u.ok && !u.external_ip.empty() ? ip_parse(u.external_ip) : 0;
            if (ip && nwd::candidate_ok(sw32(ip), own, false)) {
                addr = ip;
                port = sw16(own);
                ok = true;
                how = "UPnP";
                std::lock_guard<std::mutex> lk(S().mu);
                S().upnp = std::move(m);
            } else {
                log(u.ok ? "direct path: the UPnP mapping gives no public address (none reported, or carrier NAT); trying STUN"
                         : "direct path: no UPnP answer from the router; trying STUN");
            }
        }
        // Then the existing STUN code, from the game's party port (so the mapping is the one the peer must hit).
        // Without a port mapping, the direct path only works if both routers let the punched packets through.
        const char* e = std::getenv("BB_PARTY_STUN");
        const std::string server = e && *e ? e : "stun.l.google.com:19302";
        if (!ok && server != "off" && server != "0") {
            std::string h = server;
            std::uint16_t sp = 19302;
            if (const auto c = server.rfind(':'); c != std::string::npos) {
                h = server.substr(0, c);
                sp = static_cast<std::uint16_t>(std::strtoul(server.c_str() + c + 1, nullptr, 10));
            }
            std::uint32_t ma = 0;
            std::uint16_t mp = 0;
            for (int i = 0; i < 3 && !ok && !S().stop.load(); ++i) ok = bbnet::p2p_stun(h.c_str(), sp, 2000, &ma, &mp);
            if (ok) {
                addr = ma;
                port = mp;  // as p2p_stun reports it (network order, like the party code's)
                how = "STUN";
            }
        }
    }
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.cand = addr;
        s.cport = port;
        s.gather = ok ? 2 : 3;
    }
    log(ok ? "direct path: public address for the direct test found (" + how + ")"
           : "direct path: no public address found (no STUN or UPnP answer)");
}

// ---- helpers (under the lock) ----

bool safe_now(State& s, const Peer& p) {
    const int st = s.env.local_state ? s.env.local_state() : 0;
    const int me = g_me.load();
    return nwd::safe_pair(st, me == 0, p.peer_state, p.slot == 0);
}

void say_final(Peer& p, nwd::Verdict v, std::vector<std::string>& lines) {
    if (p.logged) return;
    p.logged = true;
    lines.push_back(nwd::log_line(v, p.tms, p.dms, p.st[1].lost(), p.st[1].sent));
}

void fallback(State& s, Peer& p, const char* why, std::vector<Out>& out, std::vector<std::string>& lines, bool tell) {
    bbnet::p2p_nwd_alias(p.tun, p.port, p.cand, p.cport, false);
    p.pair.peer_fell_back();
    if (tell) queue(out, "fb", p.slot);
    lines.push_back(std::string("direct path: ") + why + " -> tunnel (for the rest of the session)");
    (void)s;
}

void apply(State& s, Peer& p, std::vector<Out>& out, std::vector<std::string>& lines, bool decider) {
    bbnet::p2p_nwd_alias(p.tun, p.port, p.cand, p.cport, true);
    p.pair.switch_now(true);
    p.applied = true;
    p.switched = Clock::now();
    p.heard_ms = now_ms();
    p.next_keep = Clock::now();
    if (!p.logged) {
        p.logged = true;
        lines.push_back(nwd::log_line(nwd::Verdict::Direct, p.tms, p.dms, 0, nwd::kProbes));
    }
    queue(out, decider ? "done" : "ready", p.slot);
    (void)s;
}

void tick_peer(State& s, Peer& p, bool decider, Clock::time_point now, std::vector<Out>& out,
               std::vector<std::string>& lines) {
    if (!p.opt_sent) {
        p.opt_sent = true;
        queue(out, "opt", p.slot, {{"On", "1"}});
    }
    if (p.pair.final_tunnel()) return;
    // The peer never answered with its own flag.
    if (!p.peer_known) {
        if (ms_between(p.up, now) > kOptWaitMs) {
            p.pair.flags(true, false);
            if (!p.logged) {
                p.logged = true;
                lines.push_back(nwd::not_opted_line());
            }
        }
        return;
    }
    switch (p.pair.phase()) {
    case nwd::Phase::Wait: {
        if (s.gather == 0) {
            s.gather = 1;
            if (s.gatherer.joinable()) s.gatherer.join();
            s.gatherer = std::thread(gather_thread);
        }
        if (s.gather == 3) {  // we have no public address: tell the peer, stay on the tunnel
            p.pair.peer_fell_back();
            queue(out, "res", p.slot, {{"Ok", "0"}}, {{"Why", "nocand"}});
            say_final(p, nwd::Verdict::NoCandidate, lines);
            return;
        }
        if (s.gather == 2 && !p.cand_sent) {
            p.cand_sent = true;
            queue(out, "cand", p.slot, {{"CPort", std::to_string(sw16(s.cport))}}, {{"Ip", ip_text(s.cand)}});
        }
        if (p.cand_sent && p.have_cand) {
            p.pair.start_probing();
            p.base = (static_cast<std::uint32_t>(s.rng()) & 0xffffffu);
            p.next_probe = now;
        }
        return;
    }
    case nwd::Phase::Probing: {
        if (p.probes_sent < nwd::kProbes && now >= p.next_probe) {
            const std::uint32_t seq0 = (p.base << 8) | static_cast<std::uint32_t>(p.probes_sent * 2);
            for (std::uint8_t tag = 0; tag < 2; ++tag) {
                const std::uint32_t seq = seq0 | tag;
                p.sent_at[seq] = now;
                ++p.st[tag].sent;
                if (tag == 0) bbnet::p2p_nwd_probe(p.tun, p.port, 0, seq);
                else bbnet::p2p_nwd_probe(p.cand, p.cport, 1, seq);
            }
            ++p.probes_sent;
            p.last_probe = now;
            p.next_probe = now + std::chrono::milliseconds(nwd::kProbeIntervalMs);
        }
        if (!p.judged && p.probes_sent >= nwd::kProbes &&
            ms_between(p.last_probe, now) >= nwd::kReplyGraceMs) {
            p.judged = true;
            p.verdict = nwd::judge(p.st[0], p.st[1], &p.tms, &p.dms);
            queue(out, "res", p.slot,
                  {{"Ok", p.verdict == nwd::Verdict::Direct ? "1" : "0"},
                   {"TunMs", std::to_string(static_cast<long long>(p.tms))},
                   {"DirMs", std::to_string(static_cast<long long>(p.dms))}});
            p.res_sent = true;
        }
        if (p.judged) {
            if (p.verdict != nwd::Verdict::Direct) {
                p.pair.decide(p.verdict, false);
                say_final(p, p.verdict, lines);
            } else if (p.peer_res) {
                if (!p.pair.decide(p.verdict, p.peer_ok)) say_final(p, nwd::Verdict::PeerDeclined, lines);
            }
        }
        return;
    }
    case nwd::Phase::Decided: {
        if (decider) {
            if (!p.go_sent) {
                p.go_sent = true;
                queue(out, "go", p.slot);
            }
            if (p.ready_recv && !p.applied && safe_now(s, p)) apply(s, p, out, lines, true);
        } else if (p.go_recv && !p.applied && safe_now(s, p)) {
            apply(s, p, out, lines, false);
        }
        return;
    }
    case nwd::Phase::Direct: {
        if (now >= p.next_keep) {
            p.next_keep = now + std::chrono::milliseconds(nwd::kKeepaliveMs);
            const std::uint32_t seq = (p.base << 8) | static_cast<std::uint32_t>(128 + (p.keep_ctr++ & 127));
            bbnet::p2p_nwd_probe(p.cand, p.cport, 1, seq);
        }
        if (p.pair.lost(now_ms(), ms_between(s.epoch, p.switched))) {
            // lost() already moved the pair to the final tunnel state.
            bbnet::p2p_nwd_alias(p.tun, p.port, p.cand, p.cport, false);
            queue(out, "fb", p.slot);
            lines.push_back("direct path: lost (no answer for 2 s) -> tunnel (for the rest of the session)");
        } else if (!decider && !p.done_recv && ms_between(p.switched, now) > kDoneWaitMs) {
            fallback(s, p, "the other side never confirmed", out, lines, true);
        }
        return;
    }
    default:
        return;
    }
}

void worker() {
    State& s = S();
    while (!s.stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        PartyLink* link = s.env.link ? s.env.link() : nullptr;
        std::vector<Out> out;
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> lk(s.mu);
            if (link != s.link) {  // a new party (or none): forget the old pairs
                for (auto& [slot, p] : s.peers)
                    if (p.applied && !p.pair.final_tunnel())
                        bbnet::p2p_nwd_alias(p.tun, p.port, p.cand, p.cport, false);
                s.peers.clear();
                s.link = link;
                s.active = false;
                bbnet::p2p_nwd_arm(false);
                bbnet::p2p_nwd_set_ack_handler(nullptr);
                s.enabled = false;
            }
            if (!link) continue;
            const LinkState ls = link->state();
            if (ls != LinkState::Connected && ls != LinkState::Hosting) continue;
            if (!s.active) {
                s.enabled = BbSettings::Get().party_nw_direct.load();
                // Only over the tunnel: the PC's own address is a Nightwatch tunnel address.
                if (!s.enabled || !bbnet::nwd_tunnel_mode() || (ls == LinkState::Connected && link->local_slot() < 0)) {
                    continue;
                }
                s.active = true;
                bbnet::p2p_nwd_arm(true);
                bbnet::p2p_nwd_set_ack_handler([](std::uint8_t tag, std::uint32_t seq) {
                    State& st = S();
                    std::lock_guard<std::mutex> lk2(st.mu);
                    for (auto& [slot, p] : st.peers) {
                        (void)slot;
                        if (p.base != (seq >> 8)) continue;
                        const std::uint32_t idx = seq & 0xff;
                        if (idx >= 128) {
                            if (tag == 1) p.pair.heard(now_ms()), p.heard_ms = now_ms();
                            return;
                        }
                        auto it = p.sent_at.find(seq);
                        if (it == p.sent_at.end() || tag > 1) return;
                        const double rtt = std::chrono::duration<double, std::milli>(Clock::now() - it->second).count();
                        p.sent_at.erase(it);
                        ++p.st[tag].answered;
                        p.st[tag].rtt_ms.push_back(rtt);
                        return;
                    }
                });
                lines.push_back("direct path: allowed here; tested with party members who allow it too");
            }
            // The members to pair with.
            const int me = link->local_slot();
            g_me.store(me);
            std::vector<int> slots;
            const std::vector<RosterEntry> roster = link->roster();
            for (const RosterEntry& e : roster)
                if (e.connected && e.slot != me && !e.invader) slots.push_back(e.slot);
            if (ls == LinkState::Connected) {  // a guest: the host is a peer too
                bool has0 = false;
                for (int x : slots) has0 = has0 || x == 0;
                if (!has0 && me != 0) slots.push_back(0);
            }
            const auto now = Clock::now();
            for (int slot : slots) {
                auto [it, fresh] = s.peers.try_emplace(slot);
                Peer& p = it->second;
                if (fresh) {
                    p.slot = slot;
                    p.up = now;
                }
                for (const RosterEntry& e : roster)
                    if (e.slot == slot) p.peer_state = static_cast<int>(e.state);
                tick_peer(s, p, me < slot, now, out, lines);
            }
        }
        for (const std::string& l : lines) log(l);
        if (link && !out.empty()) flush(link, out);
    }
}

// Under the lock: applies one message; returns a log line (or empty).
std::string handle(State& s, bool host, int slot, PartyLink* link, const json::Value& v, const std::string& type) {
    const int from = host ? slot : static_cast<int>(int_of(v, "From", 0));
    if (from < 0 || from > 8) return {};
    std::uint32_t tun = ip_parse(str_of(v, "Tun"));
    if (host) {
        const std::uint32_t seen = link->member_ip(slot);
        if (seen) tun = seen;  // the link's own view wins over the claim
    }
    const long long port = int_of(v, "Port", 0);
    std::string note;
    Peer& p = s.peers[from];
    if (p.slot < 0) {
        p.slot = from;
        p.up = Clock::now();
    }
    if (type == "opt") {
        if (!tun || !bbnet::nwd_tunnel_peer(tun) || port < 1024 || port > 65535) {
            return "direct path: flag refused (not a tunnel address)";
        }
        p.tun = tun;
        p.port = sw16(static_cast<std::uint16_t>(port));
        p.peer_known = true;
        p.pair.flags(true, int_of(v, "On", 1) != 0);
        return {};
    }
    if (!p.peer_known) return {};  // everything else needs its flag first
    if (type == "cand") {
        const std::uint32_t ip = ip_parse(str_of(v, "Ip"));
        const long long cp = int_of(v, "CPort", 0);
        if (!ip || cp < 1024 || cp > 65535 || !nwd::candidate_ok(sw32(ip), static_cast<std::uint16_t>(cp), bbnet::nwd_test_mode())) {
            p.pair.peer_fell_back();
            return "direct path: candidate refused (not a public address)";
        }
        p.cand = ip;
        p.cport = sw16(static_cast<std::uint16_t>(cp));
        p.have_cand = true;
    } else if (type == "res") {
        p.peer_res = true;
        p.peer_ok = int_of(v, "Ok", 0) != 0;
        if (!p.peer_ok && str_of(v, "Why") == "nocand" && !p.logged) {
            p.logged = true;
            p.pair.peer_fell_back();
            note = nwd::log_line(nwd::Verdict::NoCandidate, 0, 0, 0, 0);
        }
    } else if (type == "go") {
        p.go_recv = true;
    } else if (type == "ready") {
        p.ready_recv = true;
    } else if (type == "done") {
        p.done_recv = true;
    } else if (type == "fb") {
        if (p.applied && p.pair.direct()) {
            bbnet::p2p_nwd_alias(p.tun, p.port, p.cand, p.cport, false);
            note = "direct path: the other side lost the direct path -> tunnel (for the rest of the session)";
        }
        p.pair.peer_fell_back();
    }
    return note;
}

}  // namespace

bool Enabled() { return BbSettings::Get().party_nw_direct.load(); }

void Configure(Env env) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    s.env = std::move(env);
    if (s.started) return;
    s.started = true;
    s.worker = std::thread(worker);
}

void Stop() {
    State& s = S();
    s.stop.store(true);
    std::thread w, g;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        w = std::move(s.worker);
        g = std::move(s.gatherer);
        for (auto& [slot, p] : s.peers)
            if (p.applied) bbnet::p2p_nwd_alias(p.tun, p.port, p.cand, p.cport, false);
        s.peers.clear();
    }
    bbnet::p2p_nwd_arm(false);
    bbnet::p2p_nwd_set_ack_handler(nullptr);
    if (w.joinable()) w.join();
    if (g.joinable()) g.join();
    std::unique_ptr<UpnpMapper> up;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        up = std::move(s.upnp);
    }
    if (up) up->stop();
}

bool OnEvent(bool host, int slot, const std::string& name, const std::string& body) {
    if (name != kEventName) return false;
    State& s = S();
    json::Value v;
    std::string err;
    if (!json::parse(body, v, err) || v.type != json::Value::Type::Object) {
        log("direct path: event refused: " + err);
        return true;
    }
    PartyLink* link = s.env.link ? s.env.link() : nullptr;
    if (!link) return true;
    const std::string type = str_of(v, "T");
    const int to = static_cast<int>(int_of(v, "To", -1));
    // The host: a guest's message for another guest is forwarded, stamped with the sender's slot and the tunnel
    // address the host sees its link on (a guest cannot name someone else's address).
    if (host && to != 0) {
        const std::uint32_t ip = link->member_ip(slot);
        if (to <= 0 || to > 8 || to == slot || !ip) return true;
        json::Value f = v;
        f.set("From", slot);
        f.set("Tun", ip_text(ip));
        link->send_event(to, kEventName, json::dump(f, 0));
        return true;
    }
    std::string note;  // logged after the lock is released (log() takes it)
    {
        std::lock_guard<std::mutex> lk(s.mu);
        // Setting off: ignore everything. (Not gated on `active`: the peer's flag can arrive before our worker saw
        // the link come up; it is kept, and nothing is sent or tested before the worker is active.)
        if (!Enabled()) return true;
        note = handle(s, host, slot, link, v, type);
    }
    if (!note.empty()) log(note);
    return true;
}

}  // namespace party::nwdirect
