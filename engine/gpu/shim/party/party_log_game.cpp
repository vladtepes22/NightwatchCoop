// SPDX-License-Identifier: GPL-3.0-or-later
// Party logging, the game side (party_log_game.h, docs/party/logging.md).
#include "party_log_game.h"

#include "party_log.h"

#include "game_state.h"
#include "party_async_io.h"
#include "party_director.h"
#include "party_guestsvc.h"
#include "party_link.h"
#include "party_runtime.h"
#include "net/json.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace coop::plog {
namespace {

using party::LinkState;
using party::MemberVitals;

void Line(const std::string& text) { asyncio::Print("Party log: " + text + "\n"); }

std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

std::string Excerpt(const std::string& body, std::size_t max) {
    std::string out;
    out.reserve(std::min(body.size(), max) + 8);
    for (std::size_t i = 0; i < body.size() && out.size() < max; ++i) {
        const char c = body[i];
        out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    if (body.size() > max) out += "...";
    return out;
}

std::string SlotText(int slot) { return slot < 0 ? std::string("all") : "slot " + std::to_string(slot); }

std::string LocalName() {
    const char* n = std::getenv("BB_PARTY_NAME");
    return n && *n ? std::string(n) : std::string();
}

// ---- Load reasons ----
std::mutex g_reason_mu;
std::string g_reason;
std::int64_t g_reason_ms = -1;

std::string RecentReason(std::int64_t now, std::int64_t within_ms) {
    std::lock_guard<std::mutex> lk(g_reason_mu);
    if (g_reason_ms < 0 || now - g_reason_ms > within_ms) return {};
    return g_reason;
}

// ---- Event trace rate limits (per direction + event name) ----
std::mutex g_rate_mu;
std::map<std::string, RateLimit> g_rates;

bool RateOk(const std::string& key, int* suppressed) {
    std::lock_guard<std::mutex> lk(g_rate_mu);
    auto it = g_rates.find(key);
    if (it == g_rates.end()) it = g_rates.emplace(key, RateLimit(20, 10000)).first;
    return it->second.Allow(MonoMs(), suppressed);
}

void TraceEvent(const char* dir, int slot, const std::string& name, const std::string& body) {
    const unsigned area = AreaOfEvent(name);
    if (!On(kNet | area)) return;
    int suppressed = 0;
    if (!RateOk(std::string(dir) + name, &suppressed)) return;
    std::string text = Fmt("net: %s '%s' %s %s (%zu B)", dir, name.c_str(), dir[0] == 's' ? "->" : "<-",
                           SlotText(slot).c_str(), body.size());
    if (suppressed) text += Fmt(" [%d more '%s' lines suppressed]", suppressed, name.c_str());
    // Bodies: flag / item / travel / summon events at verbose (short), everything at trace.
    if (TraceOn()) text += ": " + Excerpt(body, 800);
    else if (area != kNet) text += ": " + Excerpt(body, 200);
    Line(text);
}

// ---- Run-id exchange and marks over the link ----
void SendLogEvent(int slot, const json::Value& v) {
    if (party::PartyLink* link = party::runtime_link()) link->post_event(slot, kLogEventName, json::dump(v, 0));
}

json::Value HelloBody() {
    json::Value v = json::Value::make_object();
    v.set("k", "hello");
    v.set("run", RunId());
    v.set("who", LocalName());
    v.set("level", LevelName(Get().level));
    v.set("areas", AreasText(Get().areas));
    return v;
}

void RelayMark(int n) {
    party::PartyLink* link = party::runtime_link();
    if (!link) return;
    json::Value v = json::Value::make_object();
    v.set("k", "mark");
    v.set("n", n);
    v.set("o", link->local_slot());
    v.set("who", LocalName());
    v.set("pc", static_cast<long long>(PartyClockMs()));
    // Host: to every member; guest: to the host, which passes it on to the others.
    link->post_event(link->is_host() ? party::kBroadcast : party::kHostSlot, kLogEventName, json::dump(v, 0));
}

std::string Str(const json::Value& v, const char* key) {
    const json::Value* m = v.find(key);
    return m && m->type == json::Value::Type::String ? m->string : std::string();
}
long long Num(const json::Value& v, const char* key, long long def) {
    const json::Value* m = v.find(key);
    return m && m->type == json::Value::Type::Number ? static_cast<long long>(m->number) : def;
}

// ---- The tracker (main thread) ----
struct PeerSeen {
    bool known = false, alive = false;
    std::uint32_t map = 0;
};

struct Tracker {
    bool init = false;
    bool world_up = false, loading = false;
    std::uint32_t map = 0xffffffffu;
    int session_role = -1;
    int item_get = -1;
    std::int64_t item_get_since = 0;
    bool prompt_up = false;
    std::uint32_t prompt_id = 0;
    bool svc_open = false;
    std::int64_t load_start = 0;
    std::string load_reason;
    std::uint32_t load_from = 0xffffffffu;
    std::array<PeerSeen, party::kMaxPartyPlayers> peers{};
    std::int64_t own_death_ms = -1;
    int echoes = -1;
    std::int64_t last_poll = -1000000, last_link = -1000000, last_snapshot = 0;
    LinkState link_state = LinkState::Idle;
    bool have_link = false;
    std::string hello_sig;  // the connected slots the last hello went to
    std::uint64_t resends = 0;
    std::int64_t world_up_since = -1;
    bool test_mark_done = false;
    RateLimit prompt_rate{6, 10000};
    RateLimit rtt_rate{1, 10000};
};
Tracker& T() {
    static Tracker t;
    return t;
}

const char* Yn(bool b) { return b ? "yes" : "no"; }

std::string VitalsText(const MemberVitals& v) {
    if (!(v.flags & party::kVitalsValid)) return Fmt("s%d no vitals (age %u ms)", v.slot, v.age_ms);
    std::string t = Fmt("s%d hp %u/%u %s map %s", v.slot, v.hp, v.max_hp, (v.flags & party::kVitalsAlive) ? "alive" : "DEAD",
                        MapName(v.map_id ? v.map_id : 0xffffffffu).c_str());
    if (v.flags & party::kVitalsHasPos)
        t += Fmt(" pos (%.1f, %.1f, %.1f)", double(v.pos[0]), double(v.pos[1]), double(v.pos[2]));
    t += Fmt(" lvl %u deaths %u age %u ms", unsigned(v.level), unsigned(v.deaths), v.age_ms);
    return t;
}

void Snapshot(const char* why, const GameSnapshot& s) {
    Tracker& t = T();
    const PartyRole role = PartyDirector::Get().Role();
    std::uint32_t prompt = 0;
    const bool prompt_up = GuestSvcPromptUp(&prompt);
    Line(Fmt("snapshot (%s): me %s map %s world %s loading %s session role %d echoes %d insight %d level %d; "
             "start ready %s prologue %s; item-get box %d entr%s, action prompt %s, guest service menu %s",
             why, role == PartyRole::Host ? "host" : role == PartyRole::Guest ? "guest" : "solo",
             MapName(s.map_id).c_str(), Yn(s.world_up), Yn(s.loading), s.session_role, s.echoes, s.insight, s.level,
             Yn(LocalStartReady()), Yn(LocalStartInPrologue()), ItemGetEntries(), ItemGetEntries() == 1 ? "y" : "ies",
             prompt_up ? Fmt("text %u", prompt).c_str() : "none", Yn(GuestServicesOpen())));
    party::PartyLink* link = party::runtime_link();
    if (!link) {
        Line("snapshot link: no party link");
        return;
    }
    const std::uint64_t resends = link->udp_resends();
    std::string roster;
    for (const party::RosterEntry& e : link->roster()) {
        roster += Fmt(" %s(s%d %s %s ping %u ms%s)", e.name.c_str(), e.slot, party::member_state_name(e.state),
                      MapName(e.map_id ? e.map_id : 0xffffffffu).c_str(), e.ping_ms, e.connected ? "" : " LOST");
    }
    Line(Fmt("snapshot link: %s slot %d state %s, rtt %u ms, %s, udp resends %llu (+%llu), party clock %lld ms; "
             "roster:%s",
             link->is_host() ? "host" : "guest", link->local_slot(), party::link_state_name(link->state()), link->rtt_ms(),
             link->connected_over_udp() ? "udp" : "tcp/none", static_cast<unsigned long long>(resends),
             static_cast<unsigned long long>(resends - std::min(resends, t.resends)),
             static_cast<long long>(PartyClockMs()), roster.empty() ? " (empty)" : roster.c_str()));
    t.resends = resends;
    MemberVitals v[party::kMaxPartyPlayers];
    const int n = link->vitals(v, party::kMaxPartyPlayers);
    std::string vit;
    for (int i = 0; i < n; ++i) vit += (vit.empty() ? " " : "; ") + VitalsText(v[i]);
    Line("snapshot vitals:" + (vit.empty() ? std::string(" none") : vit));
}

void LinkTick(std::int64_t now) {
    Tracker& t = T();
    party::PartyLink* link = party::runtime_link();
    const PartyRole role = PartyDirector::Get().Role();
    if (!link) {
        SetPeer(role == PartyRole::Host ? 'H' : role == PartyRole::Guest ? 'G' : '-', -1);
        SetPartyClockOffset(false, 0);
        t.have_link = false;
        return;
    }
    const bool host = link->is_host();
    const int slot = link->local_slot();
    const LinkState st = link->state();
    SetPeer(host ? 'H' : 'G', slot);
    const bool up = host ? st == LinkState::Hosting : st == LinkState::Connected;
    if (up) SetPartyClockOffset(true, link->host_clock_ms() - MonoMs());
    if (!t.have_link || st != t.link_state) {
        Event(kNet, Level::Verbose, "link state %s -> %s", t.have_link ? party::link_state_name(t.link_state) : "none",
              party::link_state_name(st));
        t.link_state = st;
        t.have_link = true;
    }
    // Run ids: one hello to each peer that came up (every level: one line per join).
    std::string sig;
    if (host) {
        for (const party::RosterEntry& e : link->roster())
            if (e.connected && e.slot != party::kHostSlot) sig += std::to_string(e.slot) + ",";
    } else if (up) {
        sig = "host";
    }
    if (sig != t.hello_sig) {
        if (!sig.empty()) SendLogEvent(host ? party::kBroadcast : party::kHostSlot, HelloBody());
        t.hello_sig = sig;
    }
    if (On(kNet) && !host && up && link->rtt_ms() > 300) {
        int sup = 0;
        if (t.rtt_rate.Allow(now, &sup)) Event(kNet, Level::Verbose, "high rtt to the host: %u ms", link->rtt_ms());
    }
}

void PollTick(std::int64_t now) {
    Tracker& t = T();
    const GameSnapshot s = ReadGameState(false);
    const int item_get = ItemGetEntries();
    std::uint32_t prompt = 0;
    const bool prompt_up = GuestSvcPromptUp(&prompt);
    const bool svc_open = GuestServicesOpen();
    if (!t.init) {
        t.init = true;
        t.world_up = s.world_up;
        t.loading = s.loading;
        t.map = s.map_id;
        t.session_role = s.session_role;
        t.item_get = item_get;
        t.prompt_up = prompt_up;
        t.prompt_id = prompt;
        t.svc_open = svc_open;
        t.echoes = s.echoes;
        return;
    }
    // Loads (travel area): start / end with the map, the reason hint and the time taken.
    if (s.loading != t.loading) {
        if (s.loading) {
            t.load_start = now;
            t.load_from = t.map;
            std::string why = RecentReason(now, 15000);
            if (why.empty() && t.own_death_ms >= 0 && now - t.own_death_ms < 30000) why = "after the own death";
            t.load_reason = why.empty() ? "no party reason known (own menu / game)" : why;
            Event(kTravel, Level::Verbose, "load start in %s (%s)", MapName(t.map).c_str(), t.load_reason.c_str());
        } else {
            Event(kTravel, Level::Verbose, "load end: %s -> %s after %.1f s (%s)", MapName(t.load_from).c_str(),
                  MapName(s.map_id).c_str(), double(now - t.load_start) / 1000.0, t.load_reason.c_str());
        }
        t.loading = s.loading;
    }
    // Test aid (BB_PARTY_LOG_MARK_AT=S): one user mark S s after the world first came up with the
    // party clock known (the mark relay is checked without a key press).
    static const long mark_at = [] {
        const char* v = std::getenv("BB_PARTY_LOG_MARK_AT");
        return v && *v ? std::strtol(v, nullptr, 10) : -1L;
    }();
    if (mark_at >= 0 && !t.test_mark_done && s.world_up && !s.loading && PartyClockMs() >= 0) {
        if (t.world_up_since < 0) t.world_up_since = now;
        if (now - t.world_up_since >= mark_at * 1000L) {
            t.test_mark_done = true;
            UserMark("test aid BB_PARTY_LOG_MARK_AT");
        }
    }
    if (s.world_up != t.world_up) {
        Event(kTravel, Level::Verbose, "world %s (map %s)", s.world_up ? "up" : "down", MapName(s.map_id).c_str());
        t.world_up = s.world_up;
    }
    if (s.map_id != t.map) {
        if (s.map_id != 0xffffffffu && t.map != 0xffffffffu)
            Event(kTravel, Level::Verbose, "map %s -> %s%s", MapName(t.map).c_str(), MapName(s.map_id).c_str(),
                  s.loading ? " (loading)" : " (no loading screen)");
        if (s.map_id != 0xffffffffu) t.map = s.map_id;
    }
    // Session role (summon area): 6 = in another player's world.
    if (s.session_role != t.session_role) {
        Event(kSummon, Level::Verbose, "session role %d -> %d%s", t.session_role, s.session_role,
              s.session_role == 6 ? " (in the host's world)" : "");
        t.session_role = s.session_role;
    }
    // The item-get box (items + dialog areas): a pickup or a party grant shows here.
    // -1 = no item-get list yet (title, loading): not a change of the box.
    if (item_get != t.item_get && item_get >= 0 && t.item_get >= 0) {
        if (t.item_get <= 0 && item_get > 0) {
            t.item_get_since = now;
            Event(kItems | kDialog, Level::Verbose, "item-get box open (%d entr%s) in %s", item_get,
                  item_get == 1 ? "y" : "ies", MapName(s.map_id).c_str());
        } else if (item_get <= 0 && t.item_get > 0) {
            Event(kItems | kDialog, Level::Verbose, "item-get box closed after %.1f s", double(now - t.item_get_since) / 1000.0);
        } else if (item_get > t.item_get) {
            Event(kItems | kDialog, Level::Verbose, "item-get box +%d entr%s (%d now)", item_get - t.item_get,
                  item_get - t.item_get == 1 ? "y" : "ies", item_get);
        }
        t.item_get = item_get;
    } else if (item_get != t.item_get) {
        t.item_get = item_get;
    }
    // Action-button prompts ("Examine", "Open", a lamp...) and the guest service menu (dialog area).
    if (prompt_up != t.prompt_up || (prompt_up && prompt != t.prompt_id)) {
        int sup = 0;
        if (On(kDialog) && t.prompt_rate.Allow(now, &sup)) {
            Event(kDialog, Level::Verbose, "action prompt %s%s", prompt_up ? Fmt("up: text %u", prompt).c_str() : "gone",
                  sup ? Fmt(" (%d prompt changes not logged)", sup).c_str() : "");
        }
        t.prompt_up = prompt_up;
        t.prompt_id = prompt;
    }
    if (svc_open != t.svc_open) {
        Event(kDialog, Level::Verbose, "guest service menu %s", svc_open ? "open" : "closed");
        t.svc_open = svc_open;
    }
    // Echoes (trace: every change; a stain pickup or a death shows as a jump).
    if (s.echoes != t.echoes && s.echoes >= 0 && t.echoes >= 0)
        Event(kDeath | kItems, Level::Trace, "blood echoes %d -> %d", t.echoes, s.echoes);
    if (s.echoes >= 0) t.echoes = s.echoes;
    // Deaths and revivals, own and the peers' (death area; from the party's VITALS).
    party::PartyLink* link = party::runtime_link();
    if (!link || !On(kDeath)) return;
    MemberVitals v[party::kMaxPartyPlayers];
    const int n = link->vitals(v, party::kMaxPartyPlayers);
    const int me = link->local_slot();
    for (int i = 0; i < n; ++i) {
        if (v[i].slot < 0 || v[i].slot >= party::kMaxPartyPlayers) continue;
        if (!(v[i].flags & party::kVitalsValid) || v[i].age_ms > party::kVitalsStaleMs) continue;
        PeerSeen& p = t.peers[static_cast<std::size_t>(v[i].slot)];
        const bool alive = (v[i].flags & party::kVitalsAlive) != 0;
        if (p.known && alive != p.alive) {
            const bool mine = v[i].slot == me;
            if (mine && !alive) t.own_death_ms = now;
            Event(kDeath, Level::Verbose, "%s %s: %s", mine ? "own player" : Fmt("slot %d", v[i].slot).c_str(),
                  alive ? "revived / respawned" : "DIED", VitalsText(v[i]).c_str());
        }
        p.known = true;
        p.alive = alive;
        p.map = v[i].map_id;
    }
}

}  // namespace

void GameInit() {
    InstallTagger();
    SetLocalName(LocalName());
    SetMarkRelay(&RelayMark);
}

void GameTick() {
    Tracker& t = T();
    const std::int64_t now = MonoMs();
    if (now - t.last_link >= 1000) {
        t.last_link = now;
        LinkTick(now);
    }
    const bool mark_snapshot = TakeSnapshotRequest();
    if (!Verbose()) {
        if (mark_snapshot) Snapshot("user mark", ReadGameState(false));
        return;
    }
    if (now - t.last_poll >= 100) {
        t.last_poll = now;
        PollTick(now);
    }
    const int every = Get().snapshot_s;
    if (mark_snapshot) {
        Snapshot("user mark", ReadGameState(false));
    } else if (every > 0 && now - t.last_snapshot >= every * 1000LL) {
        t.last_snapshot = now;
        Snapshot("periodic", ReadGameState(false));
    }
}

void NoteLoadReason(const std::string& why) {
    {
        std::lock_guard<std::mutex> lk(g_reason_mu);
        g_reason = why;
        g_reason_ms = MonoMs();
    }
    Event(kTravel, Level::Verbose, "load reason noted: %s", why.c_str());
}

void OnEventSent(int to_slot, const std::string& name, const std::string& body) {
    if (name == kLogEventName) return;
    TraceEvent("send", to_slot, name, body);
}

bool OnEventReceived(int from_slot, const std::string& name, const std::string& body) {
    if (name != kLogEventName) {
        TraceEvent("recv", from_slot, name, body);
        if (name == "travel") NoteLoadReason("the host's travel event");
        return false;
    }
    json::Value v;
    std::string err;
    if (!json::parse(body, v, err) || v.type != json::Value::Type::Object) {
        Line("peer log event from slot " + std::to_string(from_slot) + " not readable: " + err);
        return true;
    }
    const std::string k = Str(v, "k");
    party::PartyLink* link = party::runtime_link();
    if (k == "hello") {
        // Every level: which log run of the peer goes with this one.
        const std::string level = Str(v, "level");
        Line("peer slot " + std::to_string(from_slot) + " (" + Str(v, "who") + "): log run " + Str(v, "run") +
             ", level " + level + ", areas " + Str(v, "areas") + "; this log: run " + RunId() + ", level " +
             LevelName(Get().level) +
             (level != LevelName(Get().level) ? " (levels differ: set the same Party logging level on both PCs)" : ""));
        return true;
    }
    if (k == "mark") {
        const int n = static_cast<int>(Num(v, "n", 0));
        const int origin = static_cast<int>(Num(v, "o", from_slot));
        if (link && origin == link->local_slot()) return true;  // our own mark, relayed back
        PeerMark(n, Str(v, "who"), origin, Num(v, "pc", -1));
        // Host: pass a guest's mark on to the other guests.
        if (link && link->is_host() && from_slot != party::kHostSlot) {
            for (const party::RosterEntry& e : link->roster())
                if (e.connected && e.slot != party::kHostSlot && e.slot != from_slot)
                    link->post_event(e.slot, kLogEventName, body);
        }
        return true;
    }
    Line("peer log event '" + k + "' from slot " + std::to_string(from_slot) + " ignored");
    return true;
}

}  // namespace coop::plog
