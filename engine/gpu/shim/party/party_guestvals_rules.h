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
};

/// The stain counts in this map: live, with echoes, the same map (block), not a Chalice Dungeon
/// (area 29: the layout is per dungeon, the game drops such a stain on its own).
inline bool InMap(const Stain& s, std::uint32_t map) {
    return s.live && s.echoes > 0 && (s.map & 0xffff0000u) == (map & 0xffff0000u) && (map >> 24) != 29 &&
           map != 0xffffffffu;
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
    char b[128];
    std::snprintf(b, sizeof b, "%d %u %.3f %.3f %.3f %d", s.slot, unsigned(s.map), double(s.pos[0]), double(s.pos[1]),
                  double(s.pos[2]), s.echoes);
    return b;
}

/// `rest`: what follows the six fields (the game's raw record, hex), trimmed.
inline bool Parse(const std::string& line, Stain* out, std::string* rest = nullptr) {
    Stain s;
    unsigned map = 0;
    double x = 0, y = 0, z = 0;
    int used = 0;
    if (std::sscanf(line.c_str(), "%d %u %lf %lf %lf %d%n", &s.slot, &map, &x, &y, &z, &s.echoes, &used) != 6) {
        return false;
    }
    if (rest) {
        std::size_t a = std::size_t(used), b = line.size();
        while (a < b && static_cast<unsigned char>(line[a]) <= ' ') {
            ++a;
        }
        while (b > a && static_cast<unsigned char>(line[b - 1]) <= ' ') {
            --b;
        }
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
