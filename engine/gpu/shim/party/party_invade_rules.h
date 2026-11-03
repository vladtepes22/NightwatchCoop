// SPDX-License-Identifier: GPL-3.0-or-later
// Invasions into open public games (0.7.8, CL-4cc63c): the pure rules, header only (unit test
// tests/test_party_invade.cpp, ninja -C out/gpu party-invade-test). Game side: party_invade.cpp; the link's part
// (the HELLO flag, the gate, RejectCode::Invasion) is party_link.cpp; the public-list flag is net/party_discovery.h
// and net/party_vpnlobby.h. Design and decisions: docs/party/invasions.md.
//
// Owner decision (community poll, 88 %): invasions are HOST-CONTROLLED. The host's "Allow invaders" switch
// (bbport.ini party_invaders, env BB_PARTY_INVADERS, System > Party > Allow invaders, launcher Party tab) is off by
// default, and only the host's value counts. An invader is a player who joins a listed host that allows them, as a
// hostile phantom: the host and the guests are its enemies, map enemies are not (the red phantom semantics). The
// invader's side is a normal party link (so the version / build / patch check refuses an incompatible invader with a
// named reason) that says "invader" in its HELLO.
//
// What decides, on the host, at the HELLO and again every second until the invader stands in the world (Admit):
// the switch, the session's block list, the invader's rate limit, the host's own state (not in the world, loading,
// a cutscene, a boss fight, dead, in its prologue, travelling), one invader at a time, and a free game slot.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace coop::invade {

constexpr const char* kEventName = "invade";
constexpr int kGameSlots = 4;               ///< the game's player slots: the host and three phantoms (an invader is one)
constexpr int kMaxInvaders = 1;             ///< one invader at a time
constexpr double kRateWindowS = 600.0;      ///< an invader (address) may START this many invasions per window ...
constexpr int kRateMax = 3;                 ///< ... then it waits for the oldest to age out
constexpr int kSpamMax = 20;                ///< and may knock (a refused or admitted HELLO) this many times per window
constexpr double kPendingMaxS = 120.0;      ///< an invader that is not in the world after this long is sent away
constexpr double kReportEveryS = 5.0;       ///< the host repeats the state while an invader is there

/// Why an invader is refused (also the plain words of the REJECT reason and the log).
enum class Deny : std::uint8_t {
    None = 0,
    Off,            ///< the host does not allow invaders
    Blocked,        ///< the host blocked this invader for the session
    RateLimited,    ///< too many attempts
    InvaderStays,   ///< the invader's own "Stay in my world" is on (checked on its side; also here when told)
    HostStays,      ///< the host's "Stay in my world" is on
    NotInWorld,     ///< the host is at the title / in its own prologue / not in a world yet
    Loading,        ///< the host is on a loading screen
    Cutscene,       ///< a cutscene plays
    BossFight,      ///< a boss fight (fog) is on
    HostDead,       ///< the host is dead / spectating
    Travelling,     ///< the party is travelling (a lamp, a gather)
    InvaderPresent, ///< another invader is already in
    NoSlot,         ///< no free game slot (host + guests already fill 4)
};

inline const char* DenyWord(Deny d) {
    switch (d) {
    case Deny::None: return "none";
    case Deny::Off: return "off";
    case Deny::Blocked: return "blocked";
    case Deny::RateLimited: return "rate_limited";
    case Deny::InvaderStays: return "invader_stays";
    case Deny::HostStays: return "host_stays";
    case Deny::NotInWorld: return "not_in_world";
    case Deny::Loading: return "loading";
    case Deny::Cutscene: return "cutscene";
    case Deny::BossFight: return "boss_fight";
    case Deny::HostDead: return "host_dead";
    case Deny::Travelling: return "travelling";
    case Deny::InvaderPresent: return "invader_present";
    case Deny::NoSlot: return "no_slot";
    }
    return "?";
}

/// The named reason a refused invader reads (the REJECT text and the invader's party status).
inline std::string DenyText(Deny d) {
    switch (d) {
    case Deny::None: return "";
    case Deny::Off: return "the host does not allow invaders";
    case Deny::Blocked: return "the host blocked you for this session";
    case Deny::RateLimited: return "too many invasion attempts: try again later";
    case Deny::InvaderStays: return "turn off Stay in my world to invade";
    case Deny::HostStays: return "the host stays in its own world";
    case Deny::NotInWorld: return "the host is not in a world you can enter now";
    case Deny::Loading: return "the host is on a loading screen";
    case Deny::Cutscene: return "a cutscene is playing in the host's world";
    case Deny::BossFight: return "a boss fight is on in the host's world";
    case Deny::HostDead: return "the host is dead";
    case Deny::Travelling: return "the party is travelling";
    case Deny::InvaderPresent: return "another invader is already in that world";
    case Deny::NoSlot: return "no free slot in the host's world (4 players)";
    }
    return "";
}

/// What the host reads about itself and its party now.
struct HostView {
    bool allow = false;             ///< the host's setting "Allow invaders"
    bool host_stays = false;        ///< the host's "Stay in my world" is on
    bool world_up = true;           ///< in a world (not at the title)
    bool prologue = false;          ///< the host is in its campaign start
    bool loading = false;
    bool cutscene = false;
    bool boss_fight = false;        ///< a boss health bar is up
    bool alive = true;
    bool travelling = false;        ///< a party travel / gather is running
    int world_players = 1;          ///< the host + the guests in its world or on the way in (not invaders)
    int invaders = 0;               ///< invaders already admitted (pending or in)
    int party_max = kGameSlots;     ///< the party's "max players" setting
};

/// Can another game slot take an invader? The game has four player slots (host + three phantoms) and an invader is one
/// of them; a party set to fewer players than that counts that number.
inline int Capacity(int party_max) {
    return std::max(2, std::min(party_max, kGameSlots));
}

/// The one decision. `blocked` / `rate_limited` come from the books below. The order is the order a player is told
/// about: the host's switch first, then who the invader is, then the host's moment, then room.
inline Deny Admit(const HostView& h, bool blocked, bool rate_limited, bool invader_stays = false) {
    if (!h.allow) return Deny::Off;
    if (blocked) return Deny::Blocked;
    if (rate_limited) return Deny::RateLimited;
    if (invader_stays) return Deny::InvaderStays;
    if (h.host_stays) return Deny::HostStays;
    if (!h.world_up || h.prologue) return Deny::NotInWorld;
    if (h.loading) return Deny::Loading;
    if (h.cutscene) return Deny::Cutscene;
    if (h.boss_fight) return Deny::BossFight;
    if (!h.alive) return Deny::HostDead;
    if (h.travelling) return Deny::Travelling;
    if (h.invaders >= kMaxInvaders) return Deny::InvaderPresent;
    if (h.world_players + h.invaders >= Capacity(h.party_max)) return Deny::NoSlot;
    return Deny::None;
}

/// Is the host listed as invadable (the public list's flag, announced by the host)? A host that allows invaders,
/// has a free slot and no invader. The lobby and the LAN list show only what the host announces; the host
/// announces false whenever its switch is off.
inline bool Invadable(bool allow, int world_players, int invaders, int party_max) {
    return allow && invaders < kMaxInvaders && world_players + invaders < Capacity(party_max);
}

// ---- abuse limits ----

/// Attempts per key (an address) in a sliding window.
class RateBook {
public:
    /// Records an attempt at `now`; false (and not recorded) when the key already used kRateMax in the window.
    bool Allow(const std::string& key, double now, int max = kRateMax) {
        auto& v = at_[key];
        v.erase(std::remove_if(v.begin(), v.end(), [&](double t) { return now - t >= kRateWindowS; }), v.end());
        if (static_cast<int>(v.size()) >= max) return false;
        v.push_back(now);
        if (at_.size() > 256) Trim(now);
        return true;
    }
    /// True when the next Allow(key) would refuse (nothing is recorded).
    bool Limited(const std::string& key, double now, int max = kRateMax) const {
        const auto it = at_.find(key);
        if (it == at_.end()) return false;
        int n = 0;
        for (double t : it->second) n += now - t < kRateWindowS ? 1 : 0;
        return n >= max;
    }
    void Clear() { at_.clear(); }

private:
    void Trim(double now) {
        for (auto it = at_.begin(); it != at_.end();) {
            auto& v = it->second;
            v.erase(std::remove_if(v.begin(), v.end(), [&](double t) { return now - t >= kRateWindowS; }), v.end());
            it = v.empty() ? at_.erase(it) : std::next(it);
        }
    }
    std::map<std::string, std::vector<double>> at_;
};

/// The invaders the host blocked for this session (by name and by address; a name is cheap to change).
class BlockBook {
public:
    void Block(const std::string& name, const std::string& addr) {
        if (!name.empty()) names_.insert(name);
        if (!addr.empty()) addrs_.insert(addr);
    }
    bool Blocked(const std::string& name, const std::string& addr) const {
        return (!name.empty() && names_.count(name)) || (!addr.empty() && addrs_.count(addr));
    }
    void Clear() { names_.clear(), addrs_.clear(); }
    std::size_t Size() const { return names_.size(); }

private:
    std::set<std::string> names_, addrs_;
};

// ---- how an invasion ends ----

/// Why an invader leaves the host's world (the words are in the wire and the logs).
enum class End : std::uint8_t {
    None = 0,
    InvaderDied,  ///< the invader's HP reached 0: it goes home (the host's side won)
    HostDied,     ///< the host died: the invader goes home (the invader's side won)
    Travel,       ///< the host loads another place (a lamp, a death respawn, a story warp): the session ends
    BossFight,    ///< a boss fight started
    Disabled,     ///< the host switched "Allow invaders" off
    Blocked,      ///< the host blocked or kicked the invader
    HostGone,     ///< the host left / the link dropped
    Timeout,      ///< the invader never got into the world
};

inline const char* EndWord(End e) {
    switch (e) {
    case End::None: return "none";
    case End::InvaderDied: return "invader_died";
    case End::HostDied: return "host_died";
    case End::Travel: return "travel";
    case End::BossFight: return "boss_fight";
    case End::Disabled: return "disabled";
    case End::Blocked: return "blocked";
    case End::HostGone: return "host_gone";
    case End::Timeout: return "timeout";
    }
    return "?";
}

inline bool ParseEnd(const char* w, End* out) {
    for (int i = 1; i <= static_cast<int>(End::Timeout); ++i) {
        if (std::strcmp(EndWord(static_cast<End>(i)), w) == 0) {
            *out = static_cast<End>(i);
            return true;
        }
    }
    return false;
}

/// What the host (and the guests) are told, `name` already the display name.
inline std::string EndTextForHost(End e, const std::string& name) {
    switch (e) {
    case End::InvaderDied: return "The invader " + name + " was defeated and goes home";
    case End::HostDied: return "The invader " + name + " won: you fell";
    case End::Travel: return "The invader " + name + " is sent home: the world changed";
    case End::BossFight: return "The invader " + name + " is sent home: a boss fight began";
    case End::Disabled: return "The invader " + name + " is sent home: invaders are off";
    case End::Blocked: return "The invader " + name + " is blocked for this session";
    case End::HostGone: return "The invader " + name + " left";
    case End::Timeout: return "The invader " + name + " never arrived";
    case End::None: break;
    }
    return "";
}

/// What the invader reads (the host's name already the display name).
inline std::string EndTextForInvader(End e, const std::string& host) {
    switch (e) {
    case End::InvaderDied: return "You were defeated in " + host + "'s world: back home";
    case End::HostDied: return "You defeated " + host + ": back home";
    case End::Travel: return host + " left that place: back home";
    case End::BossFight: return "A boss fight began in " + host + "'s world: back home";
    case End::Disabled: return host + " turned invaders off: back home";
    case End::Blocked: return host + " blocked you for this session";
    case End::HostGone: return host + " left the game: back home";
    case End::Timeout: return "You could not get into " + host + "'s world: back home";
    case End::None: break;
    }
    return "";
}

/// What the host's game sees of an invasion in progress (its own state; checked every frame).
struct Live {
    bool allow = true;        ///< the host's switch is still on
    bool blocked = false;
    bool in_world = true;     ///< the invader stands in the host's world (else pending)
    double pending_s = 0;     ///< how long it has been pending
    bool host_alive = true;
    bool invader_alive = true;
    bool host_loading = false;
    bool boss_fight = false;
    bool link_up = true;      ///< the invader's link is connected
};

/// The reason this invasion must end now, or End::None. The order: the host's own decisions, then the world, then the
/// fight.
inline End CheckEnd(const Live& l) {
    if (l.blocked) return End::Blocked;
    if (!l.allow) return End::Disabled;
    if (!l.link_up) return End::HostGone;
    if (!l.in_world) return l.pending_s > kPendingMaxS ? End::Timeout : End::None;
    if (l.host_loading) return End::Travel;
    if (l.boss_fight) return End::BossFight;
    if (!l.host_alive) return End::HostDied;
    if (!l.invader_alive) return End::InvaderDied;
    return End::None;
}

// ---- the wire: EVENT "invade", host -> every member ----
// {"s":<0 leave, 1 pending, 2 in>,"n":"<name>","w":"<end word>"}  `n` is a link member name (A-Za-z0-9_-, <= 16).

struct Msg {
    int state = 0;      ///< 0 over, 1 pending (admitted, on the way), 2 in the world
    std::string name;
    End why = End::None;
};

inline bool NameOk(const std::string& n) {
    if (n.empty() || n.size() > 16) return false;
    for (char c : n)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-')) return false;
    return true;
}

inline std::string Encode(const Msg& m) {
    char b[160];
    if (m.state == 0 && m.why != End::None)
        std::snprintf(b, sizeof b, "{\"s\":0,\"n\":\"%s\",\"w\":\"%s\"}", m.name.c_str(), EndWord(m.why));
    else
        std::snprintf(b, sizeof b, "{\"s\":%d,\"n\":\"%s\"}", m.state, m.name.c_str());
    return b;
}

/// Strict: false for anything that is not exactly what Encode makes.
inline bool Decode(const std::string& body, Msg* out) {
    int s = -1;
    char name[40] = {}, word[40] = {};
    int end = 0;
    if (std::sscanf(body.c_str(), "{\"s\":%d,\"n\":\"%32[^\"]\",\"w\":\"%32[^\"]\"}%n", &s, name, word, &end) == 3 &&
        body[static_cast<std::size_t>(end)] == '\0') {
        End e = End::None;
        if (s != 0 || !NameOk(name) || !ParseEnd(word, &e)) return false;
        *out = Msg{0, name, e};
        return true;
    }
    end = 0;
    if (std::sscanf(body.c_str(), "{\"s\":%d,\"n\":\"%32[^\"]\"}%n", &s, name, &end) == 2 && end > 0 &&
        body[static_cast<std::size_t>(end)] == '\0') {
        if (s < 0 || s > 2 || !NameOk(name)) return false;
        *out = Msg{s, name, End::None};
        return true;
    }
    return false;
}

/// The invader tells the host it fell (its HP reached 0 on its own game). The host's own read of the invader's
/// copy is the second opinion; either one ends the invasion.
constexpr const char* kFellEvent = "invade_fell";

// ---- the smoke's script, BB_PARTY_INVADE_TEST="name[=arg]@S[?cond][#N],..." ----
// Steps run in order, each S seconds after the previous one finished (the first S seconds after this game's world is up).
// names: allow=0|1 (the host's Allow invaders), view=boss|cutscene|loading|full|none (the host's moment, forced),
// rejoin (an invader asks the host again), die (this player dies now). `?in` starts the delay when an invasion is on
// (host: an invader is in the world; invader: it stands in the host's world), `?home` when there is none (the invader in
// its own world, its link down); `#N` waits for the Nth time that condition came true.
struct TestStep {
    enum class Kind { Allow, View, Rejoin, Die } kind = Kind::Die;
    int arg = 0;
    std::string word;
    double delay = 0;
    int cond = 0;  // 0 none, 1 in, 2 home
    int nth = 0;
};

inline bool ParseTest(const char* text, std::vector<TestStep>* out, std::string* bad = nullptr) {
    out->clear();
    if (!text) return true;
    const std::string s(text);
    std::size_t at = 0;
    while (at <= s.size()) {
        std::size_t end = s.find(',', at);
        if (end == std::string::npos) end = s.size();
        std::string item = s.substr(at, end - at);
        at = end + 1;
        if (item.empty()) {
            if (end >= s.size()) break;
            continue;
        }
        TestStep st;
        const auto fail = [&] {
            if (bad) *bad = item;
            return false;
        };
        std::string head = item;
        if (const std::size_t h = head.find('#'); h != std::string::npos) {
            st.nth = std::atoi(head.c_str() + h + 1);
            if (st.nth < 1 || st.nth > 20) return fail();
            head = head.substr(0, h);
        }
        if (const std::size_t q = head.find('?'); q != std::string::npos) {
            const std::string c = head.substr(q + 1);
            if (c == "in") st.cond = 1;
            else if (c == "home") st.cond = 2;
            else return fail();
            head = head.substr(0, q);
        }
        if (st.nth && !st.cond) return fail();
        if (const std::size_t amp = head.find('@'); amp != std::string::npos) {
            char* e = nullptr;
            st.delay = std::strtod(head.c_str() + amp + 1, &e);
            if (!e || *e || st.delay < 0) return fail();
            head = head.substr(0, amp);
        }
        std::string arg;
        if (const std::size_t eq = head.find('='); eq != std::string::npos) {
            arg = head.substr(eq + 1);
            head = head.substr(0, eq);
        }
        if (head == "allow" && (arg == "0" || arg == "1")) st.kind = TestStep::Kind::Allow, st.arg = arg == "1";
        else if (head == "view" && (arg == "boss" || arg == "cutscene" || arg == "loading" || arg == "full" || arg == "none"))
            st.kind = TestStep::Kind::View, st.word = arg;
        else if (head == "rejoin" && arg.empty()) st.kind = TestStep::Kind::Rejoin;
        else if (head == "die" && arg.empty()) st.kind = TestStep::Kind::Die;
        else return fail();
        out->push_back(st);
        if (end >= s.size()) break;
    }
    return true;
}


// ---- the invader's look (SpEffects) ----
// The phantom look is a SpEffect: the chr's effect update plays SpEffectVfxParam row `stateInfo` of every SpEffect that has
// useSpEffectEffect. A cooperator carries 9006 (stateInfo 272, SFX 211: the blue white-phantom glow); the game's invader
// effect is 9026 (the Sinister Bell's 9025 + 1; stateInfo 276, SFX 212). The invader's chr is a cooperator on the other
// games (9006), so its look is 9006 -> 9026, per chr, and back at the end.
constexpr int kCoopLookEffect = 9006;
constexpr int kInvaderLookEffect = 9026;

struct LookStep {
    bool remove_coop = false;   ///< take 9006 off the chr (and remember to put it back)
    bool add_invader = false;   ///< apply 9026
};
inline LookStep LookNow(bool has_coop, bool has_invader) {
    return {has_coop, !has_invader};
}
struct LookBack {
    bool remove_invader = false;  ///< take 9026 off (only if this code put it on)
    bool add_coop = false;        ///< put 9006 back (only if this code took it off)
};
inline LookBack LookEnd(bool added_invader, bool removed_coop, bool has_invader, bool has_coop) {
    return {added_invader && has_invader, removed_coop && !has_coop};
}

// ---- the invader's name: hostile red on the other players' screens, whatever the coloured-tags setting says ----
// The game's own phantom plate is off in party mode (our tag replaces it, in the plate's blue); the game has no hostile colour
// to borrow (its plate colour is one fixed blue), so the tag, the party panel's row and the through-walls marker of the member
// the host named as the invader use this red until the invasion ends (the name is cleared with it).
constexpr float kInvaderNameRgb[3] = {0.95f, 0.17f, 0.15f};
inline bool IsInvaderName(const std::string& member, const std::string& invader) {
    return !invader.empty() && member == invader;
}
/// The colour of a member's name: the invader's red, else `normal`.
inline const float* NameRgb(const std::string& member, const std::string& invader, const float* normal) {
    return IsInvaderName(member, invader) ? kInvaderNameRgb : normal;
}

// ---- rewards: the game's own rules (global_event.lua of eventcommon.luabnd, OnDeadEvent_*; EquipParamGoods) ----
// Cost: none. The Sinister Resonant Bell (goods 225) has consumeHeroPoint 0 and isConsume 0: it is not used up and costs
// no Insight (the Beckoning Bell, goods 200, costs 1 Insight; the party layer already zeroes that).
// Payout, by the game's CalcExcuteMultiBonus(rate, player, insight) = 0x131d470 (Lua: SOUL_RATE_S 0.1, _L 0.5):
//   the host died (the invader won)        the invader: Blood Echoes at rate 0.1 and +1 Insight   (OnDeadEvent_HostDead)
//   the invader died (the host's side won) the host: echoes at rate 0.5, no Insight               (OnDeadEvent_NormalBlackDead)
//                                          each guest: echoes at rate 0.5 * 0.5 = 0.25, no Insight (same, "client")
// Nobody else is paid and no other ending pays. The echoes are the game's own amount for the receiver's level.
enum class Role : std::uint8_t { Host, Guest, Invader };
// The result banners (the game's text effects: Lua SetTextEffect = native 0x13360B0, a queue the HUD shows): global_event.lua shows
// TEXT_TYPE_TargetClear (5) to a hostile phantom that killed the host (PlayerKill) and TEXT_TYPE_BlackClear (7) to whoever killed a
// hostile phantom (and counts it, AddKillBlackGhost). Vanilla shows the second to the killer only; the party shows it to the host
// and every guest (any of them may have dealt the blow). Nothing is shown for the other endings.
constexpr int kTextTargetClear = 5, kTextBlackClear = 7;
struct Reward {
    bool pay = false;
    float rate = 0;    ///< CalcExcuteMultiBonus' first argument
    int insight = 0;   ///< its last argument
    int banner = 0;    ///< the text effect type to show (0 none)
};
constexpr float kRateS = 0.1f, kRateL = 0.5f;
inline Reward RewardFor(Role role, End end) {
    if (end == End::HostDied && role == Role::Invader) return {true, kRateS, 1, kTextTargetClear};
    if (end == End::InvaderDied && role == Role::Host) return {true, kRateL, 0, kTextBlackClear};
    if (end == End::InvaderDied && role == Role::Guest) return {true, kRateL * 0.5f, 0, kTextBlackClear};
    return {};
}

/// One payout per invasion outcome on this game: the same (invader name, ending) within kRewardOnceS is paid once, whichever
/// path reports it (the host's own decision, the broadcast, a resent event).
constexpr double kRewardOnceS = 120.0;
class RewardOnce {
public:
    bool Take(const std::string& name, End end, double now) {
        if (have_ && name == name_ && end == end_ && now - at_ < kRewardOnceS) return false;
        have_ = true, name_ = name, end_ = end, at_ = now;
        return true;
    }
private:
    bool have_ = false;
    std::string name_;
    End end_ = End::None;
    double at_ = 0;
};

// ---- the line the invader reads in its party status when a join is refused ----

inline std::string RefusedText(const std::string& reason) {
    return "invasion refused: " + reason;
}

}  // namespace coop::invade
