// SPDX-License-Identifier: GPL-3.0-or-later
// Party phantoms as full players (see party_phantom.h; the RE is docs/party/phantom_limits.md).
//
// Offsets are ours (raw ELF VA of eboot.elf 1.09), each read from the disassembly:
//   0x14DACE0 auto-replenish (no arguments; GameDataMan slot 0x553B130 -> +8 PlayerGameData,
//     +0x5B0 inventory list, +0x328 storage) and 0x14DBB70 (no arguments; +0x390 / +0x3A0), the
//     pair MoveMapStep calls at 0x19398D9 / 0x19398DE.
//   SpEffect apply: PlayerIns vfunc +0x3F0 (chr, id, source chr, 0, 0, 0, [rsp] 0, xmm0..4 1.0f),
//     the convention of BlockClear2 0x13852B1 (SpEffect 4680) and MultiDoping 0x138BD48.
//   WorldTransitionState slot 0x5556678: +0x08 stage request, +0x1528 last lamp {id, aux},
//     +0x1592 own world.
#include "party_phantom.h"
#include "party_async_io.h"
#include "party_perf.h"
#include "party_bossfog_rules.h"

#include "../../party_env.h"

#include "net/json.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <stdexcept>

#ifndef BB_PARTY_PHANTOM_NO_GAME
#include "coop_hooks.h"
#include "game_state.h"
#include "party_balance.h"
#include "party_director.h"
#include "party_link.h"
#include "party_guestvals_rules.h"
#include "party_progress.h"
#include "party_status.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <deque>
#include <mutex>
#endif

namespace coop {

// ---------------------------------------------------------------------------------------------
// Pure part (unit-tested)
// ---------------------------------------------------------------------------------------------

namespace {

std::string Trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) {
        ++a;
    }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) {
        --b;
    }
    return s.substr(a, b - a);
}

/// A whole decimal / 0x-hex integer in [lo, hi].
bool ParseInt(const std::string& s, long lo, long hi, std::int32_t* out) {
    if (s.empty()) {
        return false;
    }
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 0);
    if (!end || *end || v < lo || v > hi) {
        return false;
    }
    *out = static_cast<std::int32_t>(v);
    return true;
}

} // namespace

std::size_t ParseEmevdSkipRules(const std::string& text, std::vector<EmevdSkipRule>* out,
                                std::vector<std::string>* errors) {
    std::size_t added = 0, at = 0;
    while (at <= text.size()) {
        std::size_t comma = text.find(',', at);
        if (comma == std::string::npos) {
            comma = text.size();
        }
        const std::string item = Trim(text.substr(at, comma - at));
        at = comma + 1;
        if (item.empty()) {
            continue;
        }
        EmevdSkipRule r;
        std::string body = item;
        bool ok = true;
        if (const std::size_t amp = body.find('@'); amp != std::string::npos) {
            ok = ParseInt(Trim(body.substr(amp + 1)), 0, 0xffff, &r.index);
            body = body.substr(0, amp);
        }
        const std::size_t c1 = body.find(':');
        const std::size_t c2 = c1 == std::string::npos ? c1 : body.find(':', c1 + 1);
        if (ok && c2 != std::string::npos && body.find(':', c2 + 1) == std::string::npos) {
            const std::string ev = Trim(body.substr(0, c1));
            if (ev == "*") {
                r.event = -1;
            } else {
                ok = ParseInt(ev, 0, 0x7fffffff, &r.event);
            }
            ok = ok && ParseInt(Trim(body.substr(c1 + 1, c2 - c1 - 1)), 0, 0xffff, &r.bank) &&
                 ParseInt(Trim(body.substr(c2 + 1)), 0, 0xffff, &r.id);
        } else {
            ok = false;
        }
        if (!ok) {
            if (errors) {
                errors->push_back("'" + item + "' is not event:bank:id[@index]");
            }
            continue;
        }
        out->push_back(r);
        ++added;
    }
    return added;
}

bool EmevdSkipMatches(const EmevdSkipRule& r, std::int32_t event, std::int32_t bank, std::int32_t id,
                      std::int32_t index) {
    return (r.event < 0 || r.event == event) && r.bank == bank && r.id == id && (r.index < 0 || r.index == index);
}

std::string DescribeSkipRule(const EmevdSkipRule& r) {
    char text[128];
    char ev[16] = "*", idx[16] = "";
    if (r.event >= 0) {
        std::snprintf(ev, sizeof ev, "%d", r.event);
    }
    if (r.index >= 0) {
        std::snprintf(idx, sizeof idx, "@%d", r.index);
    }
    std::snprintf(text, sizeof text, "%s:%d:%d%s%s%s%s", ev, r.bank, r.id, idx, r.needs_travel ? " (with B1 travel)" : "",
                  r.client_only ? " (on a client)" : "", r.open_world ? " (open world rule)" : "");
    return text;
}

std::vector<EmevdSkipRule> ConfinementWallRules() {
    // Every co-op wall event of the game (bossfog::ConfinementWalls, an EMEVD scan): common 7600
    // (args X0 object, X1 SFX: 1 2005[3](X0, 0) off ... 5 wait for multiplayer ... 6 2005[3](X0, 1)
    // on, 7 2006[2](X1) SFX on ... 11 restart), its Chalice Dungeons copy m29 12907600 (without it
    // the host's dungeon put a wall at the first doorway while a member stayed in its session, live
    // beta 6) and m34 13400010, the wall at the back of Ludwig's arena (7 / 8; player report
    // 2026-10-22: it stood after his death until the guest left). The wall and its SFX, needs_travel.
    std::vector<EmevdSkipRule> out;
    std::size_t n = 0;
    const coop::bossfog::ConfinementWall* walls = coop::bossfog::ConfinementWalls(&n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(EmevdSkipRule{walls[i].event, 2005, 3, walls[i].on_index, true});
        out.push_back(EmevdSkipRule{walls[i].event, 2006, 2, walls[i].sfx_index, true});
    }
    return out;
}

std::vector<EmevdSkipRule> BossRoomRules() {
    std::vector<EmevdSkipRule> out;
    std::size_t n = 0;
    // A wall that vanilla takes down at a boss kill only once the co-op is over (13400010: Ludwig
    // dead AND not in multiplayer): the "not in multiplayer" condition is skipped (any role), so it
    // goes at the kill while the party stays - with the open world off too.
    const coop::bossfog::ConfinementWall* walls = coop::bossfog::ConfinementWalls(&n);
    for (std::size_t i = 0; i < n; ++i) {
        if (walls[i].alone_index >= 0) {
            out.push_back(EmevdSkipRule{walls[i].event, 0, 0, walls[i].alone_index, false});
        }
    }
    // A boss whose first fight starts by the host's action in the room (Laurence, Ebrietas, Lady
    // Maria): the guest's room stays open until the fight starts, as the host's (the re-fight
    // event's `1003[5] skip 2 if client` skipped on a client, so its fog is off until then).
    const coop::bossfog::FirstFight* ff = coop::bossfog::FirstFights(&n);
    for (std::size_t i = 0; i < n; ++i) {
        if (ff[i].start == coop::bossfog::FirstStart::action) {
            out.push_back(EmevdSkipRule{ff[i].refight_event, 1003, 5, ff[i].client_skip_index, false, true});
        }
    }
    // The Moon Presence's boss music (m21 event 12104853, "boss BGM on"): outside the single-player
    // states its wait also needs flag 12104851 ([8] 3[0] IfEventFlag), which only the guest-entry
    // event 12104881 sets (a press at the fog gate). In a party nobody presses it: the guests are
    // in the arena through the host's cutscene, and the host's own instruction [7] 1003[5] does
    // not skip [8] either, so neither heard the music (CL-76fd20, parity B6). Skipped for every
    // role: the music starts once the fight is started (12101852) and the player stands in the
    // fight area (region 2102801), as when playing alone.
    out.push_back(EmevdSkipRule{12104853, 3, 0, 8, false});
    // Gehrman's boss music (event 12104803, same layout): its wait [8] needs flag 12104801, set only
    // by the guest fog-entry event 12104811, and Gehrman's fight starts from his talk with no fog
    // for anyone (CL-19ed70). Skipped for every role, as the Moon Presence's above.
    out.push_back(EmevdSkipRule{12104803, 3, 0, 8, false});
    // Rom's lake jump (CL-393160, 0.7.7): the fall from the Byrgenwerth terrace into Moonside Lake
    // is only survivable through an invincibility the landing events switch on while the player
    // crosses region 3202800. The host's copy 13204820 ([4] on, [8] off 5 s later) skips its "on"
    // on a client ([3] 1003[105]) but still runs its "off" ([8] 2004[15]); the client's own copy
    // 13204821 does its own on / off ([4] / [7]). On a client entity 10000 in 13204820's region test
    // is also met by the host's landing, so a guest who jumped a few seconds after the host had
    // its invincibility switched off by that copy's stale [8] while still falling. A client skips
    // the stale "off"; 13204821 ends the guest's own invincibility.
    {
        EmevdSkipRule lake_off;
        lake_off.event = 13204820, lake_off.bank = 2004, lake_off.id = 15, lake_off.index = 8;
        lake_off.client_only = true;
        out.push_back(lake_off);
    }
    return out;
}

const char* PhantomEventKindName(PhantomEventKind k) {
    switch (k) {
    case PhantomEventKind::Lamp: return "lamp";
    case PhantomEventKind::Rested: return "rested";
    case PhantomEventKind::Insight: return "insight";
    case PhantomEventKind::BossBonus: return "boss_bonus";
    case PhantomEventKind::Unknown: break;
    }
    return "unknown";
}

std::string PhantomEventToJsonText(const PhantomEvent& e) {
    json::Value v = json::Value::make_object();
    v.set("kind", PhantomEventKindName(e.kind));
    v.set("seq", json::hex(e.seq));
    if (e.kind == PhantomEventKind::Lamp) {
        v.set("lamp", e.lamp);
    }
    if (e.kind == PhantomEventKind::Insight) {
        v.set("n", e.insight);
        v.set("event", e.source_event);
        if (!e.boss) {
            v.set("boss", false);
        }
    }
    if (e.kind == PhantomEventKind::BossBonus) {
        v.set("echoes", e.bonus);
        v.set("event", e.source_event);
    }
    return json::dump(v, 0);
}

bool PhantomEventFromJsonText(const std::string& text, PhantomEvent* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) {
        if (error) {
            *error = err;
        }
        return false;
    }
    try {
        if (v.type != json::Value::Type::Object) {
            throw std::runtime_error("not an object");
        }
        PhantomEvent e;
        const std::string& kind = json::str(v, "kind");
        for (PhantomEventKind k : {PhantomEventKind::Lamp, PhantomEventKind::Rested, PhantomEventKind::Insight,
                                   PhantomEventKind::BossBonus}) {
            if (kind == PhantomEventKindName(k)) {
                e.kind = k;
            }
        }
        if (e.kind == PhantomEventKind::Unknown) {
            throw std::runtime_error("unknown kind '" + kind + "'");
        }
        e.seq = json::u64(v, "seq");
        if (e.kind == PhantomEventKind::Lamp) {
            e.lamp = json::u32(v, "lamp");
        }
        if (e.kind == PhantomEventKind::Insight) {
            const double n = json::num(v, "n");
            if (n < 0 || n > 99 || n != static_cast<double>(static_cast<int>(n))) {
                throw std::runtime_error("n out of range");
            }
            e.insight = static_cast<int>(n);
            e.source_event = json::u32(v, "event");
            const json::Value* b = v.find("boss");  // absent (older hosts): a boss kill
            if (b && b->type != json::Value::Type::Bool) {
                throw std::runtime_error("boss not a bool");
            }
            e.boss = !b || b->boolean;
        }
        if (e.kind == PhantomEventKind::BossBonus) {
            const double n = json::num(v, "echoes");
            if (n < 0 || n > 10000000 || n != static_cast<double>(static_cast<int>(n))) {
                throw std::runtime_error("echoes out of range");
            }
            e.bonus = static_cast<int>(n);
            e.source_event = json::u32(v, "event");
        }
        *out = e;
        return true;
    } catch (const std::exception& ex) {
        if (error) {
            *error = ex.what();
        }
        return false;
    }
}

bool SanitizePeerPhantom(const PhantomEvent& e, std::string* why) {
    if (e.kind == PhantomEventKind::Lamp && e.lamp != kTravelNone && !TravelLampKnown(e.lamp)) {
        if (why) {
            *why = "lamp " + std::to_string(e.lamp) + " is not a ReturnPointParam row";
        }
        return false;
    }
    return true;
}

bool RestedFromTravel(const TravelIntent& t) {
    switch (t.kind) {
    case TravelKind::Lamp:
        return t.respawn_mode == 2; // a world lamp into the Hunter's Dream (0x13CDF30 mode 2)
    case TravelKind::HostDeath:
    case TravelKind::HuntersMark:
        return true; // respawn-type loads at the last lamp
    default:
        return false;
    }
}

InsightMode InsightModeFromString(const char* s) {
    if (!s || !s[0]) {
        return InsightMode::Parity;
    }
    const std::string v(s);
    if (v == "0" || v == "off") {
        return InsightMode::Off;
    }
    if (v == "full") {
        return InsightMode::Full;
    }
    return InsightMode::Parity;
}

const char* InsightModeName(InsightMode m) {
    switch (m) {
    case InsightMode::Off: return "off";
    case InsightMode::Parity: return "parity";
    case InsightMode::Full: return "full";
    }
    return "?";
}

int InsightTopUp(int grant, int want, int native_gain) {
    if (grant <= 0) {
        return 0;
    }
    const int missing = want - (native_gain > 0 ? native_gain : 0);
    return missing <= 0 ? 0 : (missing < grant ? missing : grant);
}

int GuestInsightGrant(int host_n, InsightMode mode) {
    if (host_n <= 0) {
        return 0;
    }
    switch (mode) {
    case InsightMode::Off: return 0;
    case InsightMode::Parity: return host_n - 1; // the native BlockClear2 +1 makes it N
    case InsightMode::Full: return host_n;
    }
    return 0;
}

void BossInsightTracker::OnBossDefeat(std::int32_t event, double now) {
    defeats_[event] = now;
    // Keep it small: drop stale entries.
    for (auto it = defeats_.begin(); it != defeats_.end();) {
        it = now - it->second > kWindow ? defeats_.erase(it) : std::next(it);
    }
}

std::optional<int> BossInsightTracker::OnInitializeEvent(std::int32_t event, std::int32_t target, std::int32_t n,
                                                         double now) {
    if (target != kInsightEvent) {
        return std::nullopt;
    }
    const auto it = defeats_.find(event);
    if (it == defeats_.end()) {
        return std::nullopt; // first encounter, story Insight, ...: not a boss kill
    }
    const bool fresh = now - it->second <= kWindow;
    defeats_.erase(it); // once per defeat
    if (!fresh || n <= 0 || n > 99) {
        return std::nullopt;
    }
    return n;
}

bool BossInsightTracker::DefeatedRecently(std::int32_t event, double now) const {
    const auto it = defeats_.find(event);
    return it != defeats_.end() && now - it->second <= kWindow && now >= it->second;
}

bool DirectInsight::Allowed(std::int32_t event) {
    switch (event) {
    case 9422:     // common: the first kidnap's waking scene (Hypogean Gaol)
    case 12901588: // m29: layer boss defeat (+2)
    case 12901589: // m29: two-boss defeat
    case 12901591: // m29: three-boss defeat
    case 12901708: // m29: boss reward, kill Insight table
    case 12901712: // m29: boss reward, kill Insight table (pre-boss)
    case 12901718: // m29: boss reward, first-encounter Insight table
    case 12901722: // m29: boss reward, first-encounter Insight table (pre-boss)
        return true;
    default:
        return false;
    }
}

bool DirectInsight::OnSetSpEffect(std::int32_t event, std::int32_t entity, std::int32_t speffect, double now) {
    if (entity != kPlayer || speffect != kSpEffect || !Allowed(event)) {
        return false;
    }
    Pending& p = pending_[event];
    if (p.n < kMaxPerEvent) {
        ++p.n;
    }
    p.last = now;
    return true;
}

std::vector<DirectInsight::Grant> DirectInsight::Due(double now) {
    std::vector<Grant> out;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now - it->second.last >= kQuiet) { // never early: a tick's `now` may predate the count
            out.push_back({it->first, it->second.n});
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

void InsightDripper::Add(int n) {
    if (n <= 0) {
        return;
    }
    pending_ = pending_ + n > kMaxPending ? kMaxPending : pending_ + n;
}

bool InsightDripper::Step(bool allowed) {
    if (wait_ > 0) {
        --wait_;
    }
    if (pending_ <= 0 || !allowed || wait_ > 0) {
        return false;
    }
    --pending_;
    wait_ = kInterval;
    return true;
}

int InsightBudget::Take(int n, double now) {
    grants_.erase(std::remove_if(grants_.begin(), grants_.end(),
                                 [now](const Grant& g) { return now - g.at >= kWindow || now < g.at; }),
                  grants_.end());
    if (n <= 0) {
        return 0;
    }
    int used = 0;
    for (const Grant& g : grants_) {
        used += g.n;
    }
    int give = n > kMaxPerEvent ? kMaxPerEvent : n;
    if (give > kMaxPerWindow - used) {
        give = kMaxPerWindow - used;
    }
    if (give <= 0) {
        return 0;
    }
    grants_.push_back({now, give});
    return give;
}

const char* RefillReasonName(RefillReason r) {
    switch (r) {
    case RefillReason::HostRested: return "host rested";
    case RefillReason::GuestRespawn: return "guest respawn";
    case RefillReason::Probe: return "probe";
    }
    return "?";
}

void RefillScheduler::Request(RefillReason why, bool expect_load, double now) {
    // A newer request restarts the wait (a second warp: refill after the last load).
    pending_ = true;
    why_ = why;
    expect_load_ = expect_load;
    load_seen_ = false;
    since_ = now;
    stable_ = 0;
}

bool RefillScheduler::Step(const ReplayState& s, double now) {
    if (!pending_) {
        return false;
    }
    if (s.loading || !s.world_up) {
        load_seen_ = true;
    }
    stable_ = ReplayAllowedNow(s) ? stable_ + 1 : 0;
    if (now - since_ > kTimeout) {
        pending_ = false;
        return false;
    }
    const bool load_done = !expect_load_ || load_seen_ || now - since_ > kLoadWait;
    if (load_done && stable_ >= kStableTicks) {
        pending_ = false;
        return true;
    }
    return false;
}

bool LampAnnouncer::ShouldSend(std::uint32_t lamp, std::uint64_t members_key) {
    if (lamp == kTravelNone || lamp == 0) {
        return false;
    }
    if (sent_ && lamp == lamp_ && members_key == members_) {
        return false;
    }
    sent_ = true;
    lamp_ = lamp;
    members_ = members_key;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Game part
// ---------------------------------------------------------------------------------------------
#ifndef BB_PARTY_PHANTOM_NO_GAME

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using ull = unsigned long long;
using Clock = std::chrono::steady_clock;

constexpr u64 kWts = 0x5556678;
constexpr u64 kWtsRequested = 0x08, kWtsLastLamp = 0x1528, kWtsOwnWorld = 0x1592;
constexpr u64 kWorldChrMan = 0x553e878; // +0x60 PlayerIns
constexpr u64 kGameDataMan = 0x553b130; // +8 PlayerGameData
constexpr u64 kReplenish = 0x14dace0;   // void(): vials / bullets from storage
constexpr u64 kBloodBullets = 0x14dbb70; // void(): Blood Bullets reset (MoveMapStep calls it first)
constexpr u64 kSpEffectVfunc = 0x3f0;
constexpr u32 kInsightSpEffect = 4680; // heroPointDamage -1: +1 Insight
constexpr double kParticipation = 90.0; // s: a guest that was a client this recently was in the fight

using VoidFn = void(BB_COOP_SYSV*)();
using SpEffectFn = u64(BB_COOP_SYSV*)(u64 chr, u64 id, u64 source, u64 c, u64 r8, u64 r9, u64 stack0, float, float,
                                      float, float, float);

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    perf::Scope z(perf::Zone::Log);
    coop::asyncio::Print(std::string("Party phantom: ") + PartyLogStamp() + line + "\n"); // never blocks the caller (party_async_io.h)
}

bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0' && !v[1];
}

bool PartyMode() {
    return bb_party_enabled();
}

double Now() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

struct Config {
    bool init = false;
    bool respawn = true, refill = true, open_world = true;
    InsightMode insight = InsightMode::Parity;
    bool probe_insight = false, probe_refill = false;
    bool probe_host9350 = false; // live G2 check: synthetic host encounter + boss Insight starts
    bool probe_host4680 = false; // live G2 check: synthetic direct SpEffect 4680 (chalice events)
};
Config g_cfg;
// 0.7.4: the host's session rules, live (party_balance Tick sets them: the host's on a guest).
bool RespawnOn() {
    return g_cfg.init && party::status::HostRuleOn(party::status::HostRule::GuestRespawn);
}
bool RefillOn() {
    return g_cfg.init && party::status::HostRuleOn(party::status::HostRule::GuestRefill);
}

std::atomic<u64> g_seq{0};
u64 NextSeq() {
    u64 cur = g_seq.load();
    if (cur == 0) {
        const u64 seed = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
        g_seq.compare_exchange_strong(cur, seed);
    }
    return g_seq.fetch_add(1) + 1;
}

// Host.
std::mutex g_host_mu;
BossInsightTracker g_tracker;       // under g_host_mu
DirectInsight g_direct;             // under g_host_mu
std::deque<PhantomEvent> g_outbox;  // under g_host_mu
LampAnnouncer g_lamp_announcer;     // main thread
double g_last_lamp_read = 0;        // main thread

// Guest.
std::mutex g_guest_mu;
RefillScheduler g_refill;           // under g_guest_mu
InsightDripper g_drip;              // under g_guest_mu
u64 g_last_rested = 0, g_last_insight = 0; // under g_guest_mu
InsightBudget g_insight_budget;            // under g_guest_mu
std::atomic<double> g_last_client{-1e9};

// Finding 4 (statsaudit): the boss clear on this game (BlockClear2's bonus 0x1335400 at 0x1384B92).
std::atomic<double> g_native_clear_at{-1e9};
std::atomic<int> g_native_clear_bonus{0};
struct BossWait {
    bool active = false;
    double at = 0;
    u32 event = 0;
};
BossWait g_boss_wait;           // under g_guest_mu
// pard4: relayed Insight grants settled against the native gain (InsightTopUp); main thread only.
struct InsightSettle {
    int grant = 0, want = 0;
    double at = 0;
    u32 event = 0;
    std::string what;
};
std::vector<InsightSettle> g_settle;     // under g_guest_mu
struct InsightSample {
    double t;
    int insight;
    long long drips;
};
std::deque<InsightSample> g_ins_hist;    // main thread (GuestTick)
std::atomic<long long> g_drips_applied{0};
constexpr double kSettleAfter = 5.0, kSettleBase = 2.0;  // live B3: a native first-encounter +1 came 2.7 s after the relay
// Finding 9: a first-encounter event's 9350 start on this guest's own game (its copy of the
// event, which 9350's End-if-client stops), granted by GuestTick (main thread).
struct FirstEncounter {
    u32 event = 0;
    int n = 0;
};
std::deque<FirstEncounter> g_first_enc;  // under g_guest_mu
int g_host_bonus = 0;           // under g_guest_mu
double g_host_bonus_at = -1e9;  // under g_guest_mu

// Probe (main thread).
bool g_world_seen = false;
double g_world_at = 0;
bool g_probe_done = false;

void Queue(const PhantomEvent& e) {
    std::lock_guard<std::mutex> lk(g_host_mu);
    if (g_outbox.size() >= 32) {
        g_outbox.pop_front();
    }
    g_outbox.push_back(e);
}

u64 Wts() {
    u64 p = 0;
    return SafeGet(Guest(kWts), &p) ? p : 0;
}

/// The host's own last lamp id (own world only), else kTravelNone.
u32 OwnLastLamp() {
    const u64 wts = Wts();
    u8 own = 0;
    u64 rec = ~0ull;
    if (!wts || !SafeGet(wts + kWtsOwnWorld, &own) || !own || !SafeGet(wts + kWtsLastLamp, &rec)) {
        return kTravelNone;
    }
    const u32 id = static_cast<u32>(rec);
    return id == 0 ? kTravelNone : id;
}

u64 PlayerGameData() {
    u64 gdm = 0, rec = 0;
    return SafeGet(Guest(kGameDataMan), &gdm) && gdm && SafeGet(gdm + 8, &rec) ? rec : 0;
}

u64 LocalPlayer() {
    u64 wcm = 0, player = 0;
    return SafeGet(Guest(kWorldChrMan), &wcm) && wcm && SafeGet(wcm + 0x60, &player) ? player : 0;
}

bool InImage(u64 address) {
    const u64 base = Guest(0);
    return base && address >= base && address < base + ImageSize();
}

/// The game's own auto-replenish, as MoveMapStep runs it after a respawn-type load.
bool Refill(RefillReason why) {
    const u64 rec = PlayerGameData();
    u64 inv = 0, a = 0, b = 0;
    if (!rec || !SafeGet(rec + 0x5b0, &inv) || !inv || !SafeGet(rec + 0x390, &a) || !a || !SafeGet(rec + 0x3a0, &b) ||
        !b) {
        Log("refill (%s): no player inventory; skipped", RefillReasonName(why));
        return false;
    }
    reinterpret_cast<VoidFn>(Guest(kBloodBullets))();
    reinterpret_cast<VoidFn>(Guest(kReplenish))();
    // A rest also heals (a lamp / the Dream): HP to max. The guest's chr data module (chr +0x3B0
    // -> +0x20): HP +0xf8, max +0xFC.
    int hp = -1, mx = -1;
    const u64 player = LocalPlayer();
    u64 cm = 0, dm = 0;
    if (why == RefillReason::HostRested && player && SafeGet(player + 0x3b0, &cm) && cm && SafeGet(cm + 0x20, &dm) &&
        dm && SafeGet(dm + 0xf8, &hp) && SafeGet(dm + 0xfc, &mx) && hp > 0 && mx > 0 && hp < mx) {
        SafeWrite(dm + 0xf8, &mx, 4);
    }
    Log("refill (%s): vials and bullets topped up from storage (0x14DBB70, 0x14DACE0)%s", RefillReasonName(why),
        hp > 0 && mx > 0 && hp < mx ? (", HP " + std::to_string(hp) + " -> " + std::to_string(mx)).c_str() : "");
    return true;
}

/// SpEffect 4680 on the local player (BlockClear2's call at 0x13852B1).
bool ApplyInsightSpEffect() {
    const u64 player = LocalPlayer();
    u64 vt = 0, fn = 0;
    if (!player || !SafeGet(player, &vt) || !vt || !SafeGet(vt + kSpEffectVfunc, &fn) || !InImage(fn)) {
        return false;
    }
    reinterpret_cast<SpEffectFn>(fn)(player, kInsightSpEffect, player, 0, 0, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    return true;
}

/// BlockClear2 0x13849A0 computes the boss clear bonus at 0x1384B92 (0x1335400(ctx, boss entity)
/// into PlayerGameData +0xF8, paid by 0x13356C0): this game ran its own boss defeat. The host
/// tells its guests its bonus (EVENT phantom boss_bonus) for a guest whose defeat does not run.
BB_COOP_SYSV void BossBonusHook(u64 a0, u64 entity) {
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64)>(Guest(0x1335400))(a0, entity);
    float v = 0.0f;
    const u64 pgd = PlayerGameData();
    if (pgd) {
        SafeGet(pgd + 0xf8, &v);
    }
    static const bool hide = std::getenv("BB_PARTY_TEST_HIDE_CLEAR") && std::getenv("BB_PARTY_TEST_HIDE_CLEAR")[0] == '1';
    if (hide && PartyDirector::Get().Role() == PartyRole::Guest) {
        // Test aid (finding 4): this guest's own boss defeat "did not run": no bonus, not seen.
        const float zero = 0.0f;
        if (pgd) {
            SafeWrite(pgd + 0xf8, &zero, 4);
        }
        Log("test: boss clear on this game hidden (BB_PARTY_TEST_HIDE_CLEAR): boss %u, its bonus %d dropped", u32(entity),
            int(v));
        return;
    }
    g_native_clear_at = Now();
    g_native_clear_bonus = int(v);
    // 0.7.2 rot: a boss's death cleanses it (and is a killer's death; party_rot_rules.h).
    balance::NoteBossDefeated(u32(entity), "the boss defeat on this game");
    Log("boss clear on this game (BlockClear2): boss %u, clear bonus %d Blood Echoes", u32(entity), int(v));
    if (PartyDirector::Get().Role() == PartyRole::Host && v > 0.0f) {
        PhantomEvent e;
        e.kind = PhantomEventKind::BossBonus;
        e.seq = NextSeq();
        e.bonus = int(v);
        e.source_event = u32(entity);
        Queue(e);
    }
}

/// AddSoul on the local player (PlayerIns vfunc +0x3D0, as 0x13356C0 pays the bonus).
bool AddOwnEchoes(int n) {
    const u64 player = LocalPlayer();
    u64 vt = 0, fn = 0;
    if (!player || n <= 0 || !SafeGet(player, &vt) || !vt || !SafeGet(vt + 0x3d0, &fn) || !InImage(fn)) {
        return false;
    }
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, int, int, int)>(fn)(player, n, 0, 1);
    return true;
}

/// Finding 9: a first-encounter Insight for this guest, once per its own guard flag (own save):
/// `why` names the source (its own event, or the host's mirror). False: already had it.
bool GrantFirstEncounter(u32 event, int n, const char* why) {
    const u32 guard = firstenc::Guard(event);
    bool own = false;
    const bool known = progress::ReadFlagOwnSave(guard, &own);
    if (!firstenc::Grant(guard, known && own)) {
        Log("first encounter (event %u, %d Insight, %s): this player had it already (own guard %u set): nothing", event, n,
            why, guard);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        g_settle.push_back({n, n, Now(), event, "first encounter"});
    }
    const bool wrote = progress::WriteFlagOwnSave(guard, true);
    Log("first encounter (event %u, %d Insight, %s): +%d here (settled against the native gain), own guard %u %s",
        event, n, why, n, guard,
        wrote ? "set in the own save" : "NOT written");
    RequestGameSave("first-encounter Insight");
    return true;
}

void FirstEncounterTick() {
    std::deque<FirstEncounter> due;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        due.swap(g_first_enc);
    }
    for (const FirstEncounter& f : due) {
        GrantFirstEncounter(f.event, f.n, "this guest's own event");
    }
}

/// Guest: a host boss kill whose defeat did not run here within bossfall::kWaitS still pays the
/// host's boss clear bonus and the native cooperator +1 Insight (once). BB_PARTY_BOSS_FALLBACK=0: off.
void BossFallbackTick(const GameSnapshot& g, double now) {
    static const bool on = !EnvOff("BB_PARTY_BOSS_FALLBACK");
    BossWait w;
    int bonus = 0;
    double bonus_at = -1e9;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        if (!g_boss_wait.active) {
            return;
        }
        w = g_boss_wait;
        bonus = g_host_bonus;
        bonus_at = g_host_bonus_at;
    }
    const bool native = g_native_clear_at.load() >= w.at - 10.0;
    bossfall::Seen seen;
    seen.echoes = seen.insight = native;
    const bossfall::Pay pay = bossfall::Plan(now - w.at, true, seen);
    if (pay == bossfall::Pay::Wait) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        g_boss_wait.active = false;
    }
    if (pay == bossfall::Pay::Nothing) {
        Log("host boss kill (event %u): the boss defeat ran on this game too (clear bonus %d): nothing to make up",
            w.event, g_native_clear_bonus.load());
        return;
    }
    if (!on) {
        Log("host boss kill (event %u): no boss defeat on this game within %.0f s; fallback off (BB_PARTY_BOSS_FALLBACK=0)",
            w.event, bossfall::kWaitS);
        return;
    }
    const bool have_bonus = bonus_at >= w.at - 10.0 && bonus > 0;
    const int before = g.echoes;
    const bool paid = have_bonus && (pay == bossfall::Pay::Both || pay == bossfall::Pay::Echoes) && AddOwnEchoes(bonus);
    if (pay == bossfall::Pay::Both || pay == bossfall::Pay::Insight) {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        g_drip.Add(1);
    }
    std::string what = paid ? "the host's clear bonus " + std::to_string(bonus) + " Blood Echoes paid"
                            : (have_bonus ? "the clear bonus NOT paid (no player)" : "no clear bonus from the host");
    Log("host boss kill (event %u): no boss defeat on this game within %.0f s: fallback (%s): %s, +1 Insight (the "
        "native cooperator one); echoes were %d",
        w.event, bossfall::kWaitS, bossfall::PayName(pay), what.c_str(), before);
}

void OnGuestRedirect(TravelKind kind, GuestRedirect how, std::uint32_t lamp) {
    Log("guest %s redirected to %s (lamp %d); the bell brings it back", TravelKindName(kind), GuestRedirectName(how),
        static_cast<int>(lamp));
    if (RefillOn()) {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        g_refill.Request(RefillReason::GuestRespawn, true, Now());
    }
}

std::uint64_t MembersKey(party::PartyLink* link) {
    std::uint64_t h = 1469598103934665603ull;
    for (const party::RosterEntry& e : link->roster()) {
        if (!e.connected) {
            continue;
        }
        h = (h ^ static_cast<std::uint64_t>(e.slot + 1)) * 1099511628211ull;
        for (char c : e.name) {
            h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        }
    }
    return h;
}

// BB_PARTY_PHANTOM_PROBE=host9350 (live G2 check, host): 6 s after a cooperator is in the world,
// feed the observer what the host's EMEVD runs for the clinic's story Insight (12410995 -> 9350(1)),
// then 3 s later a boss kill (12411800: 2003[12], 9350(3)). A guest must get +1 and then +2 (plus
// the native +1 on a real kill); the old 30 s one-event limit dropped the second.
void Host9350ProbeTick(double now) {
    static double coop_at = -1.0;
    static int step = 0;
    if (step >= 2) {
        return;
    }
    if (coop_at < 0.0) {
        const GameSnapshot g = ReadGameState(true);
        if (g.cooperators > 0 && g.world_up && !g.loading) {
            coop_at = now;
            Log("probe host9350: a cooperator is in the world; synthetic Insight starts in 6 s");
        }
        return;
    }
    auto start9350 = [](std::int32_t event, std::int32_t n) {
        std::uint8_t args[12] = {};
        const std::int32_t target = BossInsightTracker::kInsightEvent;
        std::memcpy(args + 4, &target, 4);
        std::memcpy(args + 8, &n, 4);
        PhantomEmevdInstruction(event, 2000, 0, args, sizeof(args));
    };
    if (step == 0 && now - coop_at >= 6.0) {
        step = 1;
        Log("probe host9350: story Insight 12410995 -> 9350(1)");
        start9350(12410995, 1);
    } else if (step == 1 && now - coop_at >= 9.0) {
        step = 2;
        Log("probe host9350: boss kill 12411800 -> 2003[12], 9350(3)");
        PhantomEmevdInstruction(12411800, 2003, 12, nullptr, 0);
        start9350(12411800, 3);
    }
}

// BB_PARTY_PHANTOM_PROBE=host4680 (live G2 / G1 check, host): 6 s after a cooperator is in the
// world, feed the observer what the host's m29 Chalice Dungeon events run (DirectInsight): the
// first-encounter table 12901718 (3 x 2004[8] 4680), 4 s later the layer boss kill 12901588 (2 x
// 4680 then 2003[12]) and the kill table 12901708 (3 x 4680); also two that must not count (the
// summon refund 9183, SpEffect 4682 of an NPC summon). A guest must get +3, +1 (+ the native +1 of
// a real kill, absent here) and +3.
void Host4680ProbeTick(double now) {
    static double coop_at = -1.0;
    static int step = 0;
    if (step >= 2) {
        return;
    }
    if (coop_at < 0.0) {
        const GameSnapshot g = ReadGameState(true);
        if (g.cooperators > 0 && g.world_up && !g.loading) {
            coop_at = now;
            Log("probe host4680: a cooperator is in the world; synthetic SpEffect 4680 starts in 6 s");
        }
        return;
    }
    auto sp = [](std::int32_t event, std::int32_t entity, std::int32_t id) {
        std::uint8_t args[12] = {};
        std::memcpy(args, &entity, 4);
        std::memcpy(args + 4, &id, 4);
        PhantomEmevdInstruction(event, 2004, 8, args, sizeof(args));
    };
    if (step == 0 && now - coop_at >= 6.0) {
        step = 1;
        Log("probe host4680: chalice encounter 12901718 -> 3 x 4680; refund 9183 and NPC summon 12404410 (not counted)");
        for (int i = 0; i < 3; ++i) {
            sp(12901718, DirectInsight::kPlayer, DirectInsight::kSpEffect);
        }
        sp(9183, DirectInsight::kPlayer, DirectInsight::kSpEffect);
        sp(12404410, DirectInsight::kPlayer, 4682);
    } else if (step == 1 && now - coop_at >= 10.0) {
        step = 2;
        Log("probe host4680: chalice layer boss kill 12901588 -> 2 x 4680, 2003[12]; kill table 12901708 -> 3 x 4680");
        sp(12901588, DirectInsight::kPlayer, DirectInsight::kSpEffect);
        sp(12901588, DirectInsight::kPlayer, DirectInsight::kSpEffect);
        PhantomEmevdInstruction(12901588, 2003, 12, nullptr, 0);
        for (int i = 0; i < 3; ++i) {
            sp(12901708, DirectInsight::kPlayer, DirectInsight::kSpEffect);
        }
    }
}

void FlushDirectInsight(double now) {
    std::vector<DirectInsight::Grant> due;
    std::vector<bool> boss;
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        due = g_direct.Due(now);
        for (const DirectInsight::Grant& d : due) {
            boss.push_back(g_tracker.DefeatedRecently(d.event, now));
        }
    }
    for (std::size_t i = 0; i < due.size(); ++i) {
        if (due[i].n <= 0) {
            continue;
        }
        PhantomEvent e;
        e.kind = PhantomEventKind::Insight;
        e.seq = NextSeq();
        e.insight = due[i].n;
        e.source_event = static_cast<u32>(due[i].event);
        e.boss = boss[i];
        Queue(e);
        Log("host %s: event %d gave the host %d Insight by SpEffect 4680; telling the guests",
            boss[i] ? "boss kill" : "encounter / story Insight", due[i].event, due[i].n);
    }
}

void HostTick(party::PartyLink* link, double now) {
    FlushDirectInsight(now);
    if (RespawnOn() && link && now - g_last_lamp_read >= 1.0) {
        g_last_lamp_read = now;
        const u32 lamp = OwnLastLamp();
        if (g_lamp_announcer.ShouldSend(lamp, MembersKey(link))) {
            PhantomEvent e;
            e.kind = PhantomEventKind::Lamp;
            e.seq = NextSeq();
            e.lamp = lamp;
            Queue(e);
        }
    }
    std::deque<PhantomEvent> out;
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        out.swap(g_outbox);
    }
    if (!link) {
        return; // nobody to tell
    }
    for (const PhantomEvent& e : out) {
        link->post_event(party::kBroadcast, kPhantomEventName, PhantomEventToJsonText(e));
        Log("sent %s #%llu (lamp %d, Insight %d, event %u)", PhantomEventKindName(e.kind), ull(e.seq),
            static_cast<int>(e.lamp), e.insight, e.source_event);
    }
}

void GuestTick(const GameSnapshot& g, double now) {
    if (g.session_role == RoleClient) {
        g_last_client = now;
    }
    ReplayState s;
    s.world_up = g.world_up;
    s.loading = g.loading;
    s.session_role = g.session_role;
    u8 requested = 1;
    const u64 wts = Wts();
    s.transition_requested = !wts || !SafeGet(wts + kWtsRequested, &requested) || requested != 0;
    bool refill_now, insight_now;
    RefillReason why;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        refill_now = g_refill.Step(s, now);
        why = g_refill.Reason();
        insight_now = g_drip.Step(ReplayAllowedNow(s));
    }
    if (refill_now) {
        Refill(why);
    }
    BossFallbackTick(g, now);
    FirstEncounterTick();
    // pard4: Insight history (value and our own drips) and the settling of relayed grants.
    if (g.insight >= 0) {
        g_ins_hist.push_back({now, g.insight, g_drips_applied.load()});
        while (g_ins_hist.size() > 2 && g_ins_hist.front().t < now - 15.0) {
            g_ins_hist.pop_front();
        }
    }
    {
        std::vector<InsightSettle> due;
        {
            std::lock_guard<std::mutex> lk(g_guest_mu);
            for (auto it = g_settle.begin(); it != g_settle.end();) {
                if (now - it->at >= kSettleAfter) {
                    due.push_back(*it);
                    it = g_settle.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const InsightSettle& p : due) {
            const InsightSample* base = nullptr;
            for (const InsightSample& h : g_ins_hist) {
                if (h.t <= p.at - kSettleBase) {
                    base = &h;
                }
            }
            if (!base && !g_ins_hist.empty()) {
                base = &g_ins_hist.front();
            }
            const int gain = base && g.insight >= 0
                                 ? int(g.insight - base->insight - (g_drips_applied.load() - base->drips))
                                 : 0;
            const int give = InsightTopUp(p.grant, p.want, gain);
            if (give > 0) {
                std::lock_guard<std::mutex> lk(g_guest_mu);
                g_drip.Add(give);
            }
            Log("%s (event %u): this game paid +%d Insight on its own since just before it; the relay adds %d of its %d "
                "(this player gets %d from the moment)", p.what.c_str(), p.event, gain, give, p.grant, p.want);
        }
    }
    if (insight_now) {
        const int before = g.insight;
        g_drips_applied.fetch_add(1);
        const bool ok = ApplyInsightSpEffect();
        Log("boss Insight: SpEffect %u %s (Insight was %d)", kInsightSpEffect, ok ? "applied" : "NOT applied (no player)",
            before);
    }
}

void ProbeTick(const GameSnapshot& g, double now) {
    if (!g.world_up || g.loading) {
        g_world_seen = false;
        return;
    }
    if (!g_world_seen) {
        g_world_seen = true;
        g_world_at = now;
    }
    if (g_probe_done || now - g_world_at < 5.0) {
        return;
    }
    g_probe_done = true;
    if (g_cfg.probe_insight) {
        const int before = g.insight;
        const bool ok = ApplyInsightSpEffect();
        Log("probe R5: SpEffect 4680 %s; Insight before %d (read again in the next state lines)", ok ? "applied" : "NOT applied",
            before);
    }
    if (g_cfg.probe_refill) {
        Refill(RefillReason::Probe);
    }
}

} // namespace

void PhantomInit() {
    if (g_cfg.init) {
        return;
    }
    g_cfg.init = true;
    // 0.7.4: respawn, refill and open world are the host's session rules (party_status HostRuleOn:
    // the host's values on a guest, this game's own bbport.ini on a host), read live; these are only
    // the start values for the log.
    g_cfg.respawn = !EnvOff("BB_PARTY_GUEST_RESPAWN");
    g_cfg.refill = !EnvOff("BB_PARTY_GUEST_REFILL");
    g_cfg.open_world = !EnvOff("BB_PARTY_OPEN_WORLD");
    g_cfg.insight = InsightModeFromString(std::getenv("BB_PARTY_GUEST_INSIGHT"));
    if (const char* p = std::getenv("BB_PARTY_PHANTOM_PROBE")) {
        g_cfg.probe_insight = std::strstr(p, "insight") != nullptr;
        g_cfg.probe_refill = std::strstr(p, "refill") != nullptr;
        g_cfg.probe_host9350 = std::strstr(p, "host9350") != nullptr;
        g_cfg.probe_host4680 = std::strstr(p, "host4680") != nullptr;
    }
    if (PartyMode() && Guest(0x1384b92) && CallsTo(0x1384b92, 0x1335400)) {
        HookCallSite(0x1384b92, 0x1335400, reinterpret_cast<const void*>(&BossBonusHook), "party boss clear watch");
    }
    SetGuestDeathRedirectEnabled(g_cfg.respawn);
    SetGuestRedirectCallback(&OnGuestRedirect);
    Log("guest death -> host lamp %s, refill %s, open world %s, boss Insight %s%s%s", g_cfg.respawn ? "on" : "off",
        g_cfg.refill ? "on" : "off", g_cfg.open_world ? (TravelEnabled() ? "on" : "on (inactive: no B1 travel)") : "off",
        InsightModeName(g_cfg.insight), g_cfg.probe_insight ? ", probe insight" : "",
        g_cfg.probe_refill ? ", probe refill" : "");
}

bool PhantomWantsEmevd() {
    return PartyMode() && InsightModeFromString(std::getenv("BB_PARTY_GUEST_INSIGHT")) != InsightMode::Off;
}

std::vector<EmevdSkipRule> PhantomSkipRules() {
    // 0.7.4: always in a party; each rule applies while the party's Open world rule is on (the host's).
    if (!PartyMode() || EnvOff("BB_PARTY_TRAVEL")) {
        return {};
    }
    std::vector<EmevdSkipRule> rules = ConfinementWallRules();
    for (EmevdSkipRule& r : rules) r.open_world = true;
    return rules;
}

void PhantomEmevdInstruction(std::int32_t event, std::int32_t bank, std::int32_t id, const std::uint8_t* args,
                             std::size_t size) {
    if (g_cfg.insight == InsightMode::Off) {
        return;
    }
    if (PartyDirector::Get().Role() == PartyRole::Guest) {
        // Finding 9: the guest's own copy of a boss's first-encounter event starts 9350 (which
        // ends at once on a client): the guest's own first-encounter Insight, once (GuestTick).
        if (bank == 2000 && id == 0 && args && size >= 12 && LocalSessionRole() == RoleClient &&
            firstenc::Guard(u32(event)) != 0) {
            std::int32_t target = 0, n = 0;
            std::memcpy(&target, args + 4, 4);
            std::memcpy(&n, args + 8, 4);
            if (target == BossInsightTracker::kInsightEvent && n > 0 && n <= 99) {
                std::lock_guard<std::mutex> lk(g_guest_mu);
                if (g_first_enc.size() < 8) {
                    g_first_enc.push_back({u32(event), n});
                }
            }
        }
        return;
    }
    if (PartyDirector::Get().Role() != PartyRole::Host) {
        return;
    }
    const double now = Now();
    if (bank == 2003 && (id == 12 || id == 15 || id == 53)) {
        std::lock_guard<std::mutex> lk(g_host_mu);
        g_tracker.OnBossDefeat(event, now);
        return;
    }
    if (bank == 2004 && id == 8) {
        // SetSpEffect(entity, id, ...): Insight given straight to the player (DirectInsight).
        if (!DirectInsight::Allowed(event) || !args || size < 8) {
            return;
        }
        std::int32_t entity = 0, sp = 0;
        std::memcpy(&entity, args, 4);
        std::memcpy(&sp, args + 4, 4);
        std::lock_guard<std::mutex> lk(g_host_mu);
        g_direct.OnSetSpEffect(event, entity, sp, now);
        return;
    }
    if (bank != 2000 || id != 0 || !args || size < 12) {
        return;
    }
    std::int32_t target = 0, n = 0;
    std::memcpy(&target, args + 4, 4);
    std::memcpy(&n, args + 8, 4);
    std::optional<int> grant;
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        grant = g_tracker.OnInitializeEvent(event, target, n, now);
    }
    bool boss = true;
    if (!grant) {
        // G2: Insight that is not a boss kill (a boss's first encounter, story moments): everyone
        // present gets it too - the guest's game has no native +1 for it.
        if (target != BossInsightTracker::kInsightEvent || n <= 0 || n > 99) {
            return;
        }
        // The game runs an (event, slot) only once at a time: a second InitializeEvent(9350, slot)
        // while the first still gives its Insight (n x 10 frames) is ignored on the host. Live
        // 2026-10-15 (parab4 b6b): the Hunter's Nightmare arrival starts 9350 slot 0 from 13401000
        // and 13400100 in the same frame; the host got +2, the guest +4.
        std::int32_t slot = 0;
        std::memcpy(&slot, args, 4);
        {
            static std::mutex mu;
            static std::int32_t last_slot = -1;
            static double last_until = -1;
            std::lock_guard<std::mutex> lk(mu);
            if (slot == last_slot && now < last_until) {
                Log("host encounter / story Insight: event %d starts 9350 slot %d again while it runs: ignored by the "
                    "game, not sent",
                    event, slot);
                return;
            }
            last_slot = slot;
            last_until = now + n * 10.0 / 30.0 + 0.5;
        }
        grant = n;
        boss = false;
    }
    PhantomEvent e;
    e.kind = PhantomEventKind::Insight;
    e.seq = NextSeq();
    e.insight = *grant;
    e.source_event = static_cast<u32>(event);
    e.boss = boss;
    Queue(e);
    Log("host %s: event %d starts 9350 with %d Insight; telling the guests", boss ? "boss kill" : "encounter / story Insight",
        event, *grant);
}

void PhantomNoteHostTravel(const TravelIntent& t) {
    if (!RefillOn() || !RestedFromTravel(t)) {
        return;
    }
    PhantomEvent e;
    e.kind = PhantomEventKind::Rested;
    e.seq = NextSeq();
    Queue(e);
}

void PhantomTick(party::PartyLink* link) {
    if (!g_cfg.init) {
        return;
    }
    const PartyRole role = PartyDirector::Get().Role();
    const double now = Now();
    {   // 0.7.4: the host's respawn / refill rules in force (a guest follows the host's).
        static int last = -1;
        const int now_rules = (RespawnOn() ? 1 : 0) | (RefillOn() ? 2 : 0);
        if (now_rules != last) {
            if (last >= 0 || now_rules != 3)
                Log("party rules: guest death -> host lamp %s, refill %s (%s)", (now_rules & 1) ? "on" : "off",
                    (now_rules & 2) ? "on" : "off", role == PartyRole::Guest ? "the host's" : "own");
            last = now_rules;
            SetGuestDeathRedirectEnabled((now_rules & 1) != 0);
        }
    }
    const GameSnapshot g = ReadGameState(false);
    if (role == PartyRole::Host) {
        if (g_cfg.probe_host9350) {
            Host9350ProbeTick(now);
        }
        if (g_cfg.probe_host4680) {
            Host4680ProbeTick(now);
        }
        HostTick(link, now);
    } else if (role == PartyRole::Guest) {
        GuestTick(g, now);
    }
    if (g_cfg.probe_insight || g_cfg.probe_refill) {
        ProbeTick(g, now);
    }
}

void PhantomOnEvent(const std::string& body) {
    PhantomEvent e;
    std::string err;
    if (!PhantomEventFromJsonText(body, &e, &err)) {
        Log("bad phantom event: %s", err.c_str());
        return;
    }
    const double now = Now();
    switch (e.kind) {
    case PhantomEventKind::Lamp:
        // A lamp id the guest's game will warp to on death (0x13CDF30): a ReturnPointParam row.
        if (!SanitizePeerPhantom(e, &err)) {
            Log("host %s; ignored", err.c_str());
            break;
        }
        if (RespawnOn()) {
            SetGuestDeathRedirect(e.lamp);
        }
        break;
    case PhantomEventKind::Rested: {
        if (!RefillOn()) {
            break;
        }
        std::lock_guard<std::mutex> lk(g_guest_mu);
        if (e.seq <= g_last_rested) {
            break;
        }
        g_last_rested = e.seq;
        // The guest follows the host's warp (B1): refill after that load.
        g_refill.Request(RefillReason::HostRested, TravelEnabled(), now);
        Log("host rested #%llu: refill after %s", ull(e.seq), TravelEnabled() ? "the follow load" : "a steady moment");
        break;
    }
    case PhantomEventKind::Insight: {
        const bool present = now - g_last_client.load() <= kParticipation;
        if (!e.boss && firstenc::Guard(e.source_event) != 0 && present && g_cfg.insight != InsightMode::Off) {
            {
                std::lock_guard<std::mutex> lk(g_guest_mu);
                if (e.seq <= g_last_insight) {
                    break;
                }
                g_last_insight = e.seq;
            }
            GrantFirstEncounter(e.source_event, e.insight, "the host's first encounter");
            break;
        }
        const int grant = e.boss ? GuestInsightGrant(e.insight, g_cfg.insight)
                                 : (g_cfg.insight == InsightMode::Off ? 0 : e.insight);
        std::lock_guard<std::mutex> lk(g_guest_mu);
        if (e.seq <= g_last_insight) {
            break;
        }
        g_last_insight = e.seq;
        const char* what = e.boss ? "host boss kill" : "host encounter / story Insight";
        if (!present) {
            Log("%s (event %u, %d Insight): this guest was not in the host's world; nothing", what, e.source_event,
                e.insight);
            break;
        }
        if (e.boss) {
            // 0.7.2 rot: every member in the host's world, alive or dead in the fight.
            balance::NoteBossDefeated(0, "the host's boss kill");
            // Finding 4: did the boss defeat run on this game too? (BossFallbackTick)
            g_boss_wait.active = true;
            g_boss_wait.at = now;
            g_boss_wait.event = e.source_event;
        }
        // A host's stream of Insight events is capped (bbport security pass), by a rolling budget
        // so a first-encounter +1 and the kill a few seconds later both count (G2).
        if (grant <= 0) {
            Log("%s (event %u, %d Insight): nothing on top%s (mode %s)", what, e.source_event, e.insight,
                e.boss ? " of the native cooperator +1" : "", InsightModeName(g_cfg.insight));
            break;
        }
        const int give = g_insight_budget.Take(grant, now);
        if (give <= 0) {
            Log("%s (event %u, %d Insight): over the Insight budget (%d per %.0f s); ignored", what, e.source_event,
                e.insight, InsightBudget::kMaxPerWindow, InsightBudget::kWindow);
            break;
        }
        if (e.boss) {
            g_settle.push_back({give, give + 1, now, e.source_event, "host boss kill"});
        } else {
            g_drip.Add(give);
        }
        Log("%s (event %u, %d Insight): +%d here%s (mode %s)", what, e.source_event, e.insight, give,
            e.boss ? " plus the native cooperator +1" : "", InsightModeName(g_cfg.insight));
        break;
    }
    case PhantomEventKind::BossBonus: {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        g_host_bonus = e.bonus;
        g_host_bonus_at = now;
        Log("host boss clear bonus: %d Blood Echoes (boss %u)", e.bonus, e.source_event);
        // 0.7.2 rot: the boss entity too (it may be this guest's killer); only in the host's world.
        if (now - g_last_client.load() <= kParticipation) balance::NoteBossDefeated(e.source_event, "the host's boss clear");
        break;
    }
    case PhantomEventKind::Unknown:
        break;
    }
}

#endif // BB_PARTY_PHANTOM_NO_GAME

} // namespace coop
