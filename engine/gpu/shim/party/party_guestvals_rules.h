// SPDX-License-Identifier: GPL-3.0-or-later
// A party member's OWN values (the statsaudit findings, 0.6.2): the pure rules, header only, no
// game dependency (unit test: tests/test_party_guestvals.cpp, ninja -C out/gpu party-guestvals-test).
//
//   ngguard     finding 3: a summoned guest keeps its own NG cycle for its own save (the game's
//               restore value GSM +0x159C / flag +0x1598, re-asserted while a client; the own cycle
//               put back at home when the host's leaked into it). In the host's world the live
//               cycle is the host's (LiveNg: enemy HP / damage / echoes as on the host's game).
//   gueststain  finding 1: a guest's bloodstain recoverable in the host's world (a party-side
//               stain: map, spot, echoes; paid when the guest walks onto it).
//   lampshare   finding 5: lamps are shared by the whole party (a guest's own lit lamps go to the
//               host's world; the host's reach every guest's own save through C2).
//   firstenc    finding 9: each player gets a boss's first-encounter Insight once (its own guard flag).
//   bossfall    finding 4: a host boss kill whose defeat did not run on the guest's game still pays
//               the guest its boss echoes and the native +1 Insight (once).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::ngguard {

/// What the guard does this tick.
enum class Act {
    None,
    Learn,         ///< remember the own cycle (from the live value at home, or the game's restore value)
    KeepRestore,   ///< while a client: GSM +0x1598 = 1, +0x159C = own (repeated summoned loads re-save the host's)
    RestoreHome,   ///< at home after a session: GameDataMan +0x68 = own (the host's leaked into it)
    AcceptChange,  ///< the own world's cycle changed for a reason of its own (an ending): own = live
};

struct View {
    bool steady = false;        ///< world up, no load, a moment after it
    bool client = false;        ///< summoned into another world (session role 6)
    bool own_world = false;     ///< GSM +0x1592 == 1 (the own world is loaded), not a client
    int live = -1;              ///< GameDataMan +0x68
    int restore_flag = 0;       ///< GSM +0x1598 (the game restores +0x159C at the next own-world load)
    int restore_value = -1;     ///< GSM +0x159C
    int own = -1;               ///< the own cycle learned so far (-1 unknown)
    int host = -1;              ///< the host's cycle seen while a client (-1 unknown)
    bool after_session = false; ///< the first steady own world after being a client in this process
    bool ending_since = false;  ///< an ending ran since the own cycle was learned (NG+ is legitimate)
    bool alone_pending = false; ///< an alone load is being set up (its descriptor uses the game's values)
    bool host_world = false;    ///< a guest in the host's world: GSM +0x1592 == 0 with the host's 0x19 state (+0x1593)
};

/// The NG cycle to write into GameDataMan +0x68 now, -1 for none. Vanilla (and ERSC): everyone in a
/// world runs its host's cycle (enemy HP, damage, kill echoes, boss bonus). A B2 load into the host's
/// world is built with GSM +0x1521 clear, so its descriptor (0x1943F10) took the restore value +0x159C
/// - the guest's own cycle - and the MoveMapStep ctor (0x1937570) made it live: the guest ran the
/// host's world at its own NG. The own cycle stays in +0x159C with +0x1598 set (what the save writer
/// 0x14EA560 stores). Applies while loading too (before the map's characters are made), once the own
/// cycle is known (a title join learns it first).
inline int LiveNg(const View& v) {
    if (!v.host_world || v.alone_pending || v.own < 0 || v.own > 7 || v.host < 0 || v.host > 7 || v.live < 0 ||
        v.live > 7 || v.live == v.host) {
        return -1;
    }
    return v.host;
}

/// The note shown to a guest summoned into a host at another NG cycle (0.7.6: before it was a log line
/// only, and a player whose save had run ahead of the party never learned why the worlds differed).
/// Empty when the cycles match or one is unknown.
inline std::string MixedNgNotice(int host, int own) {
    if (host < 0 || own < 0 || host == own) return std::string();
    char t[200];
    std::snprintf(t, sizeof t,
                  "Your save is in NG+%d, the host's world is NG+%d: you play the host's world at its NG+%d; "
                  "your own save keeps NG+%d", own, host, host, own);
    return t;
}

/// While in the host's world: the restore value / flag must hold the own cycle (so the save and the
/// next own-world load keep it) - checked also during a load, after the MoveMapStep ctor re-saved
/// the live value.
inline bool RestoreWrong(const View& v) {
    return v.host_world && !v.alone_pending && v.own >= 0 && v.own <= 7 &&
           (v.restore_flag != 1 || v.restore_value != v.own);
}

inline Act Plan(const View& v) {
    if (!v.steady || v.live < 0 || v.live > 7) {
        return Act::None;
    }
    if (v.client) {
        if (v.own < 0) {
            // A title join never had its own world up: the first summoned load saved the own
            // cycle (still the save's) into +0x159C with +0x1598 = 1 (0x1937570).
            return v.restore_flag == 1 && v.restore_value >= 0 && v.restore_value <= 7 ? Act::Learn : Act::None;
        }
        if (v.alone_pending) {
            return Act::None;
        }
        return v.restore_flag != 1 || v.restore_value != v.own ? Act::KeepRestore : Act::None;
    }
    if (!v.own_world) {
        return Act::None;
    }
    if (v.own < 0) {
        return Act::Learn;
    }
    if (v.live == v.own) {
        return Act::None;
    }
    if (v.after_session && !v.ending_since && v.host >= 0 && v.live == v.host) {
        return Act::RestoreHome;
    }
    return Act::AcceptChange;
}

inline const char* ActName(Act a) {
    switch (a) {
    case Act::None: return "none";
    case Act::Learn: return "learn";
    case Act::KeepRestore: return "keep restore";
    case Act::RestoreHome: return "restore at home";
    case Act::AcceptChange: return "accept change";
    }
    return "?";
}

}  // namespace coop::ngguard

namespace coop::gueststain {

constexpr float kPickupRadius = 1.6f;  ///< m, horizontal: standing on the stain
constexpr float kPickupHeight = 2.5f;  ///< m, vertical
constexpr float kArmDistance = 3.0f;   ///< m: the guest must have been this far from it once (not on its own body)

struct Stain {
    bool live = false;
    int slot = -1;           ///< the save slot of the character it belongs to
    std::uint32_t map = 0;   ///< packed map 0xAABB0000
    float pos[3] = {0, 0, 0};
    int echoes = 0;
    /// A Chalice Dungeon stain (map area 29): the ChannelId of the dungeon it lies in (the layout is
    /// generated from it, so the same id is the same dungeon); 0 unknown (a file from before 0.7.6).
    std::uint64_t dungeon = 0;
};

/// The stain counts in this map: live, with echoes, the same map (block). A Chalice Dungeon stain
/// (area 29) only in the same dungeon (`dungeon`, the game's current ChannelId there; 0 unknown
/// never matches): another dungeon has another layout, so the spot means nothing there.
inline bool InMap(const Stain& s, std::uint32_t map, std::uint64_t dungeon = 0) {
    if (!s.live || s.echoes <= 0 || map == 0xffffffffu) {
        return false;
    }
    if ((map >> 24) != 29) {
        return (s.map & 0xffff0000u) == (map & 0xffff0000u);
    }
    // A dungeon's pieces are blocks of one layout: the area and the dungeon id decide, not the block.
    return (s.map >> 24) == 29 && s.dungeon != 0 && s.dungeon == dungeon;
}

/// What the guest does with its party stain in the world it is in (`map`, `dungeon` as for InMap).
///  Show:   it lies here (recoverable by walking onto it).
///  Keep:   it lies elsewhere (another map, or another / no dungeon): kept as it is. A Chalice
///          stain waits for its own dungeon (the party back in that id); a death of the guest in
///          another place replaces it, the vanilla rule for a lost stain.
///  Refund: a Chalice stain from before 0.7.6 (no dungeon id: the spot cannot be matched to any
///          dungeon) is paid back at once instead of being lost silently.
enum class Fate : std::uint8_t { Show, Keep, Refund };
inline Fate FateOf(const Stain& s, std::uint32_t map, std::uint64_t dungeon) {
    if (s.live && s.echoes > 0 && (s.map >> 24) == 29 && s.dungeon == 0) {
        return Fate::Refund;
    }
    return InMap(s, map, dungeon) ? Fate::Show : Fate::Keep;
}

inline float Horizontal(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dz * dz);
}

/// Standing on it (the game's own stain is picked up from about there).
inline bool Touching(const float me[3], const float at[3]) {
    return Horizontal(me, at) <= kPickupRadius && std::fabs(me[1] - at[1]) <= kPickupHeight;
}

/// Armed once the living player stood kArmDistance away (a revive where it lies never pays at once).
inline bool Arms(const float me[3], const float at[3]) {
    return Horizontal(me, at) >= kArmDistance || std::fabs(me[1] - at[1]) > kPickupHeight;
}

/// One line per stain: "slot map x y z echoes" (party_stain.txt; one stain per character).
inline std::string Format(const Stain& s) {
    char b[160];
    std::snprintf(b, sizeof b, "%d %u %.3f %.3f %.3f %d", s.slot, unsigned(s.map), double(s.pos[0]), double(s.pos[1]),
                  double(s.pos[2]), s.echoes);
    std::string out = b;
    if (s.dungeon) {
        out += " #" + std::to_string(static_cast<unsigned long long>(s.dungeon));  // '#': hex bytes may start with any letter
    }
    return out;
}

/// `rest`: what follows the six fields (and the "#dungeon" token): the game's raw record, hex, trimmed.
inline bool Parse(const std::string& line, Stain* out, std::string* rest = nullptr) {
    Stain s;
    unsigned map = 0;
    double x = 0, y = 0, z = 0;
    int used = 0;
    if (std::sscanf(line.c_str(), "%d %u %lf %lf %lf %d%n", &s.slot, &map, &x, &y, &z, &s.echoes, &used) != 6) {
        return false;
    }
    std::size_t a = std::size_t(used), b = line.size();
    while (a < b && static_cast<unsigned char>(line[a]) <= ' ') {
        ++a;
    }
    while (b > a && static_cast<unsigned char>(line[b - 1]) <= ' ') {
        --b;
    }
    if (a < b && line[a] == '#') {
        std::size_t e = a + 1;
        unsigned long long d = 0;
        while (e < b && line[e] >= '0' && line[e] <= '9') {
            d = d * 10 + static_cast<unsigned>(line[e] - '0');
            ++e;
        }
        s.dungeon = d;
        a = e;
        while (a < b && static_cast<unsigned char>(line[a]) <= ' ') {
            ++a;
        }
    }
    if (rest) {
        *rest = line.substr(a, b - a);
    }
    if (s.slot < 0 || s.slot > 99 || s.echoes <= 0 || s.echoes > 999999999 || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(z) || std::fabs(x) > 1e5 || std::fabs(y) > 1e5 || std::fabs(z) > 1e5) {
        return false;
    }
    s.map = map;
    s.pos[0] = float(x), s.pos[1] = float(y), s.pos[2] = float(z);
    s.live = true;
    *out = s;
    return true;
}

}  // namespace coop::gueststain

namespace coop::lampshare {

/// The lamp's flags that make it lit for travel, for lamp `n` of map block AAB (area*10 + block):
/// the lamp object's base+10 / base+11 (base 1AAB7800 + 20 n: the object and the headstone list's
/// lit flag) and the lamp-lit flag 7AAB0200 + n (common event 7100+slot activates its warp object).
inline std::vector<std::uint32_t> LitFlags(std::uint32_t aab, std::uint32_t n) {
    const std::uint32_t base = 10000000u + aab * 10000u + 7800u + 20u * n;
    return {base + 10u, base + 11u, 70000000u + aab * 10000u + 200u + n};
}

/// The lamp of a lamp-lit flag 7AAB0200 + n: its LitFlags (empty for any other id). The host
/// lighting a guest's lamp sets all three at once, so a save right after it has the whole lamp
/// (lamprule run 5: the guest's 7AAB02xx came 1 s before its base+10 / +11; the host's save in
/// between kept only the lamp-lit flag).
inline std::vector<std::uint32_t> LitFlagsOfLampLit(std::uint32_t id) {
    if (id < 70000000u || id >= 80000000u || id % 10000u < 200u || id % 10000u >= 220u) return {};
    return LitFlags((id - 70000000u) / 10000u, id % 10000u - 200u);
}

/// The last-lamp guard (0.7.2 player report: "died in the research hall and respawned at the lamp
/// above the elevator, which I had not lit"). Lighting a lamp makes it the last lamp (GSM +0x1528
/// {return point, anim}, the respawn record +0x1538 / +0x153C) of every game that sees its lit flag
/// rise, also when another party member lit it: the host's game set its own last lamp to the
/// guest's 3502950 within a second of the guest's 73500200 (the lamp's own reaction to the flag;
/// our code writes no record there). Only the player's own lamp use may move it: a change of the
/// record to a lamp whose lit flag rose within kLitWindowS, while this player stands farther than
/// kNearM from that lamp and no load is under way, is put back. Every other change stands (a rest,
/// a travel, the party's own placements, a lamp this player lit itself).
constexpr double kLitWindowS = 5.0;
constexpr float kNearM = 8.0f;
/// `by_guest`: the host lit that lamp for a guest's lamp_lit event (it knows; then the distance does
/// not matter: a host standing at the lamp did not use it).
inline bool RestoreLastLamp(std::uint32_t was_rp, std::uint32_t now_rp, double lit_ago, float dist_m, bool loading,
                            bool by_guest = false) {
    return now_rp != was_rp && now_rp != 0 && !loading && lit_ago >= 0.0 && lit_ago <= kLitWindowS &&
           (by_guest || !(dist_m >= 0.0f && dist_m <= kNearM));
}

/// A lamp the guest's game shows lit in the host's world counts as lit BY the guest (EVENT lamp_lit to
/// the host) only after a real lamp use: the guest stands at that lamp (within kNearM of its spot)
/// and no load ended within kArrivalGraceS (a lamp warp's arrival lights its lamp by itself: the
/// 0.7.2 player report, an A5 cross-map join arriving at the host map's unlit lamp 3502950 made
/// the host "light" it and take it as its own last lamp). Lamps already lit when a load ends are
/// never sent for that map.
constexpr double kArrivalGraceS = 20.0;
inline bool CountsAsGuestLit(double since_load_s, float dist_m) {
    return since_load_s >= kArrivalGraceS && dist_m >= 0.0f && dist_m <= kNearM;
}

/// A guest's arrival lamp in the host's map (A5 cross-map join, gather, a lone respawn): never a
/// lamp the host's world has not lit. `host_last`: the host's last lamp (0 / unknown: none); each
/// candidate: a lamp of the host's map, lit there or not, and its squared distance to the host
/// (< 0 unknown). The host's last lamp when it is a lit candidate; else the lit lamp nearest the
/// host (unknown distances last); else 0: no lamp (the caller places the guest beside the host).
struct LampCandidate {
    std::uint32_t rp = 0;
    bool lit = false;
    float d2 = -1.0f;
};
inline std::uint32_t ArrivalLamp(std::uint32_t host_last, const std::vector<LampCandidate>& c) {
    for (const LampCandidate& l : c) {
        if (l.rp == host_last && l.lit) return l.rp;
    }
    std::uint32_t best = 0;
    float best_d = 0.0f;
    for (const LampCandidate& l : c) {
        if (!l.lit) continue;
        const float d = l.d2 < 0.0f ? 3.0e38f : l.d2;
        if (!best || d < best_d) best = l.rp, best_d = d;
    }
    return best;
}

/// The flags to send so the other world has this lamp lit: the ones `mine` has and `theirs` lacks,
/// only when the lamp counts as lit in `mine` (its headstone flag base+10 is set).
template <class MineFn, class TheirsFn>
std::vector<std::uint32_t> Missing(std::uint32_t aab, std::uint32_t n, MineFn mine, TheirsFn theirs) {
    std::vector<std::uint32_t> out;
    const std::vector<std::uint32_t> f = LitFlags(aab, n);
    if (!mine(f[0])) {
        return out;
    }
    for (std::uint32_t id : f) {
        if (mine(id) && !theirs(id)) {
            out.push_back(id);
        }
    }
    return out;
}

/// The host's lamps into the own save (0.7.6 report: a guest joined at the host's lamp, which its own
/// save lacked, and lighting it did nothing). A lamp counts as lit in `src` when its headstone flag
/// base+10 OR its lamp-lit flag 7AAB02xx is set (a host world can hold only the second: C2 and a
/// guest's lamp_lit event write 7AAB02xx on their own). Then EVERY one of its three flags `dst` lacks
/// is returned: with only 7AAB02xx in the own save the lamp is neither listed nor lit by its action.
template <class SrcFn, class DstFn>
std::vector<std::uint32_t> MissingFromLit(std::uint32_t aab, std::uint32_t n, SrcFn src, DstFn dst) {
    std::vector<std::uint32_t> out;
    const std::vector<std::uint32_t> f = LitFlags(aab, n);
    if (!src(f[0]) && !src(f[2])) {
        return out;
    }
    for (std::uint32_t id : f) {
        if (!dst(id)) {
            out.push_back(id);
        }
    }
    return out;
}

/// The main boss defeat flag of map block AAB (1AAB1800: 12401800 Vicar Amelia, 12301800 the
/// Blood-starved Beast, 13201800 Rom, ...).
constexpr std::uint32_t BlockBossFlag(std::uint32_t aab) { return 10000000u + aab * 10000u + 1800u; }

/// A guest's lamp past a boss the receiving world has not beaten stays out of that world: lamps
/// after a block's first one (n >= 1) are mostly the ones a boss's death lights (the Grand
/// Cathedral after Vicar Amelia; live lampL: travelling there with her alive put the party in her
/// arena beside the idle boss, past the fog). The block's first lamp (n == 0) is the way in.
inline bool HeldForBoss(std::uint32_t n, bool boss_dead_mine, bool boss_dead_theirs) {
    return n >= 1 && boss_dead_mine && !boss_dead_theirs;
}

/// Whose lamps light up where (user decision 2026-10-21, "host state saves lamps, anybody can light
/// them"; it replaced the 2026-10-15 union of every member's lamps: a high-level guest lit all of
/// its own lamps in a friend's world, past bosses the friend had not beaten).
enum class Origin {
    GuestOwnSave,      ///< lit in a guest's own world before the session (its save brings it)
    GuestInHostWorld,  ///< a guest lit it in the host's world during the session (EVENT lamp_lit)
    HostWorld,         ///< lit in the host's world (by the host, now or earlier)
};

/// The host's two lamp rules (party_balance Rules): Shared lamps (share_lamps, on by default) and
/// Guests bring their lamps (guests_bring_lamps, off by default: the 0.7.2 rule).
struct Rule {
    bool share = true;
    bool guests_bring = false;
};

/// host_world: the lamp is lit in the host's world (and saved there). member_saves: it reaches
/// every party member's own save (C2 and the guest's own-save writer). A guest's own lamps always
/// stay lit in its own save; only whether they also light up in the host's world is decided here.
struct Outcome {
    bool host_world = false;
    bool member_saves = false;
};

inline Outcome Decide(Origin o, const Rule& r) {
    switch (o) {
    case Origin::GuestOwnSave: {
        // Only with both rules on (the 2026-10-15 union); HeldForBoss still applies then.
        const bool in = r.share && r.guests_bring;
        return {in, in};
    }
    case Origin::GuestInHostWorld:
        // A lamp of the host's world, whoever lit it: the host's world keeps it (also with Shared
        // lamps off, as before: the host's world is the one it was lit in).
        return {true, r.share};
    case Origin::HostWorld:
        return {true, r.share};
    }
    return {};
}

/// Guest: send the own save's lamps the host's world lacks (EVENT lamp_lit with "own").
inline bool SendOwnLamps(const Rule& r) { return Decide(Origin::GuestOwnSave, r).host_world; }
/// Host: light a guest's lamp_lit event in its world (`own`: the guest's own-save lamps).
inline bool HostTakes(bool own, const Rule& r) {
    return Decide(own ? Origin::GuestOwnSave : Origin::GuestInHostWorld, r).host_world;
}
/// Guest: the host's world's lamps (the host's own and the ones guests lit there) into the own save.
inline bool IntoOwnSave(const Rule& r) { return Decide(Origin::HostWorld, r).member_saves; }

}  // namespace coop::lampshare

namespace coop::bossfall {

constexpr double kWaitS = 3.0;  ///< after the host's boss-kill notice: the guest's own defeat had this long

/// Whether the guest's own boss defeat ran: its boss bonus arrived (echoes went up by a boss-size
/// amount or PlayerGameData +0xF8 was set) or its native +1 Insight (SpEffect 4680) came.
struct Seen {
    bool echoes = false;
    bool insight = false;
};

enum class Pay { Wait, Nothing, Echoes, Insight, Both };

/// `waited`: s since the host's notice; `present`: the guest was in the host's world for the kill.
inline Pay Plan(double waited, bool present, const Seen& s) {
    if (!present) {
        return Pay::Nothing;
    }
    if (s.echoes && s.insight) {
        return Pay::Nothing;
    }
    if (waited < kWaitS) {
        return Pay::Wait;
    }
    if (!s.echoes && !s.insight) {
        return Pay::Both;
    }
    return s.echoes ? Pay::Insight : Pay::Echoes;
}

inline const char* PayName(Pay p) {
    switch (p) {
    case Pay::Wait: return "wait";
    case Pay::Nothing: return "nothing";
    case Pay::Echoes: return "echoes";
    case Pay::Insight: return "insight";
    case Pay::Both: return "echoes and insight";
    }
    return "?";
}

}  // namespace coop::bossfall

namespace coop::firstenc {

/// A boss's first-encounter events (AAB1802 & co., the 1.09 EMEVD: each reads a guard flag
/// 9300-9349, starts common 9350 with N only while it is off, then sets it) and their guards.
/// The guard is per player (a never_sync flag): a guest-started fight used to send the guest's
/// guard to the host's world first, so the host's event skipped its Insight and nobody got it
/// (live fogF2: Vicar Amelia, 12401802 / 9301).
inline std::uint32_t Guard(std::uint32_t event) {
    switch (event) {
    case 12101802: return 9343;  // Gehrman
    case 12101852: return 9307;  // Moon Presence
    case 12201802: return 9338;  // Witch of Hemwick
    case 12301702: return 9340;  // Old Yharnam (2nd)
    case 12301802: return 9339;  // Blood-starved Beast
    case 12401802: return 9301;  // Vicar Amelia
    case 12411702: return 9300;  // Cleric Beast
    case 12411802: return 9336;  // Father Gascoigne
    case 12414807: return 9337;
    case 12421802: return 9304;  // Darkbeast Paarl
    case 12501802: return 9341;  // Martyr Logarius
    case 12601802: return 9306;  // Mergo's Wet Nurse
    case 12601852: return 9342;  // Micolash
    case 12801802: return 9305;  // The One Reborn
    case 13201802: return 9303;  // Rom
    case 13301802: return 9302;  // Amygdala
    case 13401801: return 9344;
    case 13401851: return 9302;
    case 13501801: return 9346;
    case 13501851: return 9348;
    case 13601801: return 9347;
    default: return 0;
    }
}

/// Whether this player gets a first-encounter Insight: only while its OWN guard flag is off.
inline bool Grant(std::uint32_t guard, bool own_guard_set) {
    return guard != 0 && !own_guard_set;
}

}  // namespace coop::firstenc
