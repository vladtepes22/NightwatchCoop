// SPDX-License-Identifier: GPL-3.0-or-later
// Pings (I2), the game side (party_ping.h): the ping button -> the spot looked at (the GPU depth
// probe, bbport_camera.h) -> EVENT map_ping; incoming pings -> the marker board the overlay draws.
// Main thread (the director's tick, once a frame); OnEvent on the link's callback thread.
#include "party_ping.h"
#include "party_hud.h"
#include "party_link.h"
#include "party_status.h"
#include "party_async_io.h"
#include "game_state.h"
#include "../bbport_camera.h"
#include "../bbport_settings.h"

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace coop::ping {

namespace {

namespace P = party::ping;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    asyncio::Print(std::string("Party: ping: ") + line + "\n");
}

bool EnvFlag(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    return v[0] != '0';
}

std::atomic<std::uint32_t> g_requests{0};
std::atomic<const char*> g_source{"?"};
std::mutex g_mu;
std::deque<std::pair<int, std::string>> g_in;

// A map marker asked for by the map view (present thread), taken by the tick.
struct MarkReq {
    bool pending = false, remove = false;
    std::uint32_t map = 0;
    float x = 0, z = 0;
};
MarkReq g_mark_req;  // under g_mu

struct State {
    std::uint32_t seen_requests = 0;
    std::int64_t last_ping_ms = -1000000;
    std::uint32_t probe_seq = 0;      // a probe in flight (0: none)
    std::int64_t probe_deadline = 0;
    std::uint32_t next_id = 1;
    bool had_link = false;
    P::RelayRate rate;
    // What the ping is sent with (taken when the button was pressed).
    float me[3] = {0, 0, 0};
    std::uint32_t map = 0;
    BbCamera::State cam;
    // This player's own map marker (resent every kMarkResendMs), and the housekeeping clock.
    P::MapMark mymark;
    std::int64_t mark_sent_ms = 0, mark_check_ms = 0;
};
State& S() {
    static State s;
    return s;
}

std::string SlotName(party::PartyLink* link, int slot) {
    if (link) {
        for (const party::RosterEntry& r : link->roster())
            if (r.slot == slot) return r.name;
    }
    return std::string();
}

void PlaceMarker(const P::PingEvent& e, std::int64_t left_ms, const char* from) {
    if (e.mark) {  // a map marker: no expiry (I7/E3)
        if (e.clear) {
            P::ClearMark(e.slot);
            Log("%s: %s (slot %d) took its map marker down", from, e.who.c_str(), e.slot);
            return;
        }
        P::MapMark m;
        m.slot = e.slot;
        std::snprintf(m.name, sizeof m.name, "%s", e.who.c_str());
        m.map = e.map;
        m.x = e.pos[0];
        m.z = e.pos[2];
        m.id = e.id;
        m.mine = std::strcmp(from, "local") == 0;
        P::PlaceMark(m);
        if (m.mine || m.id != 0) Log("%s: %s (slot %d) map marker %u at (%.1f, %.1f) map %s", from, e.who.c_str(),
                                     e.slot, e.id, double(m.x), double(m.z), MapName(e.map).c_str());
        return;
    }
    if (e.clear) {
        P::Clear(e.slot);
        Log("%s: %s (slot %d) took its ping down", from, e.who.c_str(), e.slot);
        return;
    }
    if (left_ms <= 0) {
        Log("%s: %s (slot %d) ping %u dropped: older than %lld ms", from, e.who.c_str(), e.slot, e.id,
            static_cast<long long>(P::kLifeMs));
        return;
    }
    P::Marker m;
    m.slot = e.slot;
    std::snprintf(m.name, sizeof m.name, "%s", e.who.c_str());
    m.map = e.map;
    std::memcpy(m.pos, e.pos, sizeof m.pos);
    m.hit = e.hit;
    m.id = e.id;
    m.until_ms = party::hud::NowMs() + left_ms;
    m.mine = std::strcmp(from, "local") == 0;
    P::Place(m);
    Log("%s: %s (slot %d) ping %u at (%.2f, %.2f, %.2f) map %s, %s, shows %lld ms", from, e.who.c_str(), e.slot,
        e.id, double(e.pos[0]), double(e.pos[1]), double(e.pos[2]), MapName(e.map).c_str(),
        e.hit ? "on a surface" : "fallback point", static_cast<long long>(left_ms));
}

/// The camera's forward axis (view row 2: view z > 0 in front), or the player's facing.
void Forward(const BbCamera::State& cam, float yaw, float out[3]) {
    if (cam.valid) {
        out[0] = cam.view[8], out[1] = cam.view[9], out[2] = cam.view[10];
        return;
    }
    out[0] = std::sin(yaw), out[1] = 0.0f, out[2] = std::cos(yaw);
}

void Send(party::PartyLink* link, State& st, const float pos[3], bool hit, const char* how) {
    P::PingEvent e;
    e.id = st.next_id++;
    e.map = st.map;
    std::memcpy(e.pos, pos, sizeof e.pos);
    e.hit = hit;
    e.t = link->host_clock_ms();
    e.slot = link->local_slot();
    e.who = SlotName(link, e.slot);
    if (e.who.empty()) e.who = link->is_host() ? "Host" : "Hunter";
    const float d = party::hud::Distance(st.me, pos);
    Log("sent ping %u at (%.2f, %.2f, %.2f), %.1f m from me, %s (%s), map %s", e.id, double(pos[0]), double(pos[1]),
        double(pos[2]), double(d), hit ? "on a surface" : "fallback point", how, MapName(e.map).c_str());
    if (link->is_host()) {
        link->post_event(party::kBroadcast, P::kEventName, P::ToJson(e));
    } else {
        // The host fills in the slot and the name from its roster (never trusted from a guest).
        P::PingEvent up = e;
        up.slot = -1;
        up.who.clear();
        link->post_event(party::kHostSlot, P::kEventName, P::ToJson(up));
    }
    PlaceMarker(e, P::kLifeMs, "local");
}

void SendEvent(party::PartyLink* link, const P::PingEvent& e) {
    if (link->is_host()) {
        link->post_event(party::kBroadcast, P::kEventName, P::ToJson(e));
    } else {
        P::PingEvent up = e;  // the host fills in the slot and the name from its roster
        up.slot = -1;
        up.who.clear();
        link->post_event(party::kHostSlot, P::kEventName, P::ToJson(up));
    }
}

/// The map view's request and the owner's resend: this player's map marker out to the party.
void MarkTick(party::PartyLink* link, State& st, std::int64_t now) {
    MarkReq req;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        req = g_mark_req;
        g_mark_req.pending = false;
    }
    P::PingEvent e;
    e.mark = true;
    e.slot = link->local_slot();
    e.who = SlotName(link, e.slot);
    if (e.who.empty()) e.who = link->is_host() ? "Host" : "Hunter";
    if (req.pending && MarksEnabled()) {
        e.id = st.next_id++;
        e.t = link->host_clock_ms();
        if (req.remove) {
            e.clear = true;
            st.mymark.live = false;
        } else {
            e.map = req.map;
            e.pos[0] = req.x;
            e.pos[2] = req.z;
            st.mymark.live = true;
            st.mymark.map = req.map;
            st.mymark.x = req.x;
            st.mymark.z = req.z;
        }
        st.mark_sent_ms = now;
        SendEvent(link, e);
        PlaceMarker(e, 0, "local");
        return;
    }
    if (st.mymark.live && now - st.mark_sent_ms >= P::kMarkResendMs && MarksEnabled()) {
        st.mark_sent_ms = now;
        e.id = st.next_id++;
        e.map = st.mymark.map;
        e.pos[0] = st.mymark.x;
        e.pos[2] = st.mymark.z;
        SendEvent(link, e);
    }
}

/// Every two seconds: the marker of a member who is gone goes with it.
void MarkSweep(party::PartyLink* link, State& st, std::int64_t now) {
    if (now - st.mark_check_ms < 2000) return;
    st.mark_check_ms = now;
    P::MapMark marks[P::kMaxSlots];
    const int n = P::ReadMarks(marks);
    if (n == 0) return;
    const std::vector<party::RosterEntry> roster = link->roster();
    for (int i = 0; i < n; ++i) {
        if (marks[i].slot == link->local_slot() || marks[i].slot == party::kHostSlot) continue;
        bool here = false;
        for (const party::RosterEntry& r : roster) here = here || (r.slot == marks[i].slot && r.connected);
        if (!here) {
            P::ClearMark(marks[i].slot);
            Log("map marker of slot %d dropped: it left", marks[i].slot);
        }
    }
}

void Incoming(party::PartyLink* link, State& st) {
    std::deque<std::pair<int, std::string>> in;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        in.swap(g_in);
    }
    const double now_s = double(party::hud::NowMs()) / 1000.0;
    for (const auto& [slot, body] : in) {
        P::PingEvent e;
        std::string err;
        if (!P::FromJson(body, &e, &err)) {
            Log("event from slot %d refused: %s", slot, err.c_str());
            continue;
        }
        if (link->is_host()) {
            if (slot == party::kHostSlot || slot < 0 || slot >= P::kMaxSlots) continue;
            if (!st.rate.Take(slot, now_s)) {
                Log("ping from slot %d dropped: more than %d in %.0f s", slot, P::kRelayBurst, P::kRelayWindowS);
                continue;
            }
            e.slot = slot;  // the host names the pinger
            e.who = SlotName(link, slot);
            if (e.who.empty()) continue;  // not in the roster (any more)
            const std::string relay = P::ToJson(e);
            for (const party::RosterEntry& r : link->roster()) {
                if (r.slot != slot && r.slot != party::kHostSlot && r.connected) {
                    link->post_event(r.slot, P::kEventName, relay);
                }
            }
        } else {
            if (slot != party::kHostSlot) continue;
            if (e.slot < 0) e.slot = party::kHostSlot;
            if (e.slot == link->local_slot()) continue;  // our own back
            if (e.who.empty()) e.who = SlotName(link, e.slot);
        }
        PlaceMarker(e, e.mark ? 0 : P::LifeLeft(e.t, link->host_clock_ms()), link->is_host() ? "from guest" : "from host");
    }
}

}  // namespace

bool Enabled() {
    static const char* const env = std::getenv("BB_PARTY_PINGS");  // over the setting (tests)
    const bool on = env && *env ? env[0] != '0' : BbSettings::Get().party_pings.load();
    return on && party::status::Enabled();
}

bool MarksEnabled() {
    static const char* const env = std::getenv("BB_PARTY_MAP_MARKS");  // over the setting (tests)
    const bool on = env && *env ? env[0] != '0' : BbSettings::Get().party_map_marks.load();
    return on && party::status::Enabled();
}

void RequestMapMark(std::uint32_t map, float x, float z, bool remove) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_mark_req = MarkReq{true, remove, map, x, z};
}

void Request(const char* source) {
    g_source.store(source ? source : "?");
    g_requests.fetch_add(1, std::memory_order_acq_rel);
}

void OnEvent(int slot, const std::string& body) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_in.size() < 64) g_in.emplace_back(slot, body);
}

void Tick(const GameSnapshot& s, party::PartyLink* link, bool in_world) {
    State& st = S();
    if (!link) {
        if (st.had_link) P::ClearAll();
        st.mymark = P::MapMark();
        std::lock_guard<std::mutex> lk0(g_mu);
        g_mark_req.pending = false;
        st.had_link = false;
        st.probe_seq = 0;
        st.seen_requests = g_requests.load();
        g_in.clear();
        return;
    }
    st.had_link = true;
    Incoming(link, st);
    const std::int64_t now = party::hud::NowMs();
    MarkTick(link, st, now);
    MarkSweep(link, st, now);

    // A probe in flight: the depth's answer, or the fallback point once it is late.
    if (st.probe_seq) {
        BbCamera::Probe probe;
        const bool answered = BbCamera::ProbeFor(st.probe_seq, &probe);
        if (answered || now >= st.probe_deadline) {
            float at[3];
            bool hit = false;
            const char* how = "no depth answer";
            if (answered && probe.n > 0) {
                P::Sample samples[BbCamera::kProbeSamples];
                int hits = 0;
                for (int i = 0; i < probe.n && i < BbCamera::kProbeSamples; ++i) {
                    samples[i].hit = probe.hit[std::size_t(i)] != 0;
                    std::memcpy(samples[i].p, probe.pts[std::size_t(i)].data(), sizeof samples[i].p);
                    hits += samples[i].hit;
                }
                hit = P::PickAim(probe.cam.data(), st.me, samples, probe.n, probe.centre, at);
                how = hit ? "depth" : "depth: sky or nothing past the player";
                if (EnvFlag("BB_PARTY_PING_LOG", false)) {
                    for (int i = probe.centre; i >= 0 && i > probe.centre - 24; --i)
                        Log("probe %d%s: %s (%.2f, %.2f, %.2f)", i, i == probe.centre ? " (centre)" : "",
                            samples[i].hit ? "hit" : "sky", double(samples[i].p[0]), double(samples[i].p[1]),
                            double(samples[i].p[2]));
                }
                Log("depth probe %u: %d samples, %d surfaces, centre %d", probe.seq, probe.n, hits, probe.centre);
            } else if (answered) {
                how = "no scene depth this frame";
            }
            if (!hit) {
                float fwd[3];
                party::hud::Board b;
                party::hud::Read(&b);
                Forward(st.cam, b.me_yaw, fwd);
                P::FallbackAim(fwd, st.me, at);
            }
            st.probe_seq = 0;
            Send(link, st, at, hit, how);
        }
    }

    // The button.
    const std::uint32_t req = g_requests.load(std::memory_order_acquire);
    if (req == st.seen_requests) return;
    st.seen_requests = req;
    const char* source = g_source.load();
    if (!Enabled()) return;
    party::hud::Board b;
    party::hud::Read(&b);
    if (!in_world || !s.world_up || s.loading || !b.me_have_pos || s.map_id == 0 || s.map_id == 0xffffffffu) {
        Log("ping (%s) ignored: not in a world", source);
        return;
    }
    if (party::hud::GameMenu()) {
        Log("ping (%s) ignored: a game menu is open", source);
        return;
    }
    if (st.probe_seq) return;  // one at a time
    if (now - st.last_ping_ms < P::kCooldownMs) {
        Log("ping (%s) ignored: %lld ms after the last one", source, static_cast<long long>(now - st.last_ping_ms));
        return;
    }
    st.last_ping_ms = now;
    std::memcpy(st.me, b.me_pos, sizeof st.me);
    st.map = s.map_id;
    st.cam = BbCamera::Latest();
    st.probe_seq = BbCamera::RequestProbe();
    st.probe_deadline = now + P::kProbeWaitMs;
    Log("ping (%s): depth probe %u asked", source, st.probe_seq);
}

}  // namespace coop::ping

extern "C" int bbgpu_ping_combo_enabled(void) {
    return coop::ping::Enabled() ? 1 : 0;
}

extern "C" void bbgpu_ping_request(void) {
    coop::ping::Request("touchpad + R3");
}
