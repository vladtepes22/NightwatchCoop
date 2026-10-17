// SPDX-License-Identifier: GPL-3.0-or-later
// Direct lamp travel (lamp_travel.h): the pure rules. Header only, no game dependency (unit tests:
// tests/test_lamp_travel.cpp).
//
// Bloodborne's Hunter's Dream headstones (talk scripts t210000..t210003, the DLC's t210721) list
// the lamps with the talk command AddWarpEntry (talk ESD c1_19(index, name flag, lit flag),
// command 0x13 of the switch 0x203FFD0) and open the game's warp menu (c1_41, command 0x29: the
// "WarpMenu" FrpgMenuDlgWarp); the chosen entry's index comes back in MenuMan +0xEC0 (talk test
// f23). Lamp AAB_n (area AA, block B, lamp n) is:
//   ReturnPointParam row  AAB*10000 + 2950 + n   (what the lamp warp 0x13CDF30 takes)
//   name / blocked flag   70000000 + AAB*10 + n  (EventText: the lamp's name; set = "cannot travel")
//   lit flag              10000000 + AAB*10000 + 7810 + 20*n  (the lamp object's flag base + 10)
//   Dream flag            70000100 + AAB*10000 + n  (the lamp's "Return to the Hunter's Dream" YES:
//                                                     common event 7200 runs the warp to 2102950)
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "party_gamedata.h"

namespace coop::lamptravel {

struct Lamp {
    std::uint16_t aab;  ///< area * 10 + block (241 = m24_01)
    std::uint8_t n;     ///< lamp index in its block
    std::uint8_t group; ///< the headstone that lists it (Group)
};

enum Group : std::uint8_t { Dream = 0, Yharnam = 1, Frontier = 2, Unseen = 3, Nightmare = 4, HuntersNightmare = 5 };

/// Every world lamp of the player's game, in the order the headstones list them (t210000, t210001, t210002, t210003,
/// t210721). Derived from the game's lamp flags when the party tables are built (gamedata section `lamps`,
/// docs/party/gamedata.md); empty without the tables. Built once, on first use.
inline const std::vector<Lamp>& Lamps() {
    static const std::vector<Lamp> lamps = [] {
        std::vector<Lamp> v;
        for (const gamedata::LampRow& r : gamedata::Get().lamps) v.push_back({r.aab, r.n, r.group});
        return v;
    }();
    return lamps;
}

inline constexpr std::uint32_t kDreamReturnPoint = 2102950; ///< the Hunter's Dream (lamp 210_0)
inline constexpr std::uint32_t kDreamNameFlag = 70002100;   ///< EventText "Hunter's Dream"
/// The lamp's Dream confirmation (talk ESD c1_17(2, 10010713, 3, 4, 2): OpenGenericDialog kind 2,
/// "Return to the Hunter's Dream?"), the command the world lamps' scripts t AAB 000..003 run on
/// "Return to the Hunter's Dream" (ActionButtonParam 6101).
inline constexpr std::uint32_t kCmdGenericDialog = 0x11;
inline constexpr std::uint32_t kCmdClearWarpList = 0x14;
inline constexpr std::uint32_t kCmdAddWarpEntry = 0x13;
inline constexpr std::uint32_t kCmdOpenWarpMenu = 0x29;
inline constexpr std::uint32_t kDreamConfirmText = 10010713;

constexpr std::uint32_t ReturnPoint(const Lamp& l) { return l.aab * 10000u + 2950u + l.n; }
constexpr std::uint32_t NameFlag(const Lamp& l) { return 70000000u + l.aab * 10u + l.n; }
constexpr std::uint32_t LitFlag(const Lamp& l) { return 10000000u + l.aab * 10000u + 7810u + 20u * l.n; }

/// The lamp of ReturnPointParam row `id` (false: not a world / Dream lamp of the table, or the Dream).
inline bool LampOf(std::uint32_t id, Lamp* out) {
    for (const Lamp& l : Lamps()) {
        if (ReturnPoint(l) == id) {
            if (out) *out = l;
            return true;
        }
    }
    return false;
}

/// The flag whose setting is the lamp's own "Return to the Hunter's Dream: YES" (its common event
/// 7200 warps); 0 for an id outside the table.
inline std::uint32_t DreamFlag(std::uint32_t here) {
    Lamp l{};
    return LampOf(here, &l) ? 70000100u + l.aab * 10000u + l.n : 0;
}

/// The packed map (AA << 24 | B << 16) a lamp's return point lies in.
constexpr std::uint32_t MapOf(std::uint32_t return_point) {
    return (return_point / 100000) << 24 | (return_point / 10000 % 10) << 16;
}

/// The talk command is the world lamp's "Return to the Hunter's Dream?" confirmation: the one
/// direct travel replaces. `args` are the command's integer arguments (count = number of args).
inline bool IsDreamConfirm(std::uint32_t cmd, int count, const std::int32_t* args) {
    return cmd == kCmdGenericDialog && count == 5 && args && args[0] == 2 &&
           static_cast<std::uint32_t>(args[1]) == kDreamConfirmText;
}

struct Entry {
    std::uint32_t index = 0;        ///< the warp menu's result value (1-based, unique)
    std::uint32_t name_flag = 0;    ///< AddWarpEntry's 2nd argument
    std::uint32_t lit_flag = 0;     ///< AddWarpEntry's 3rd argument (the game shows it when set)
    std::uint32_t return_point = 0; ///< where the choice travels
    std::uint8_t group = 0;
};

/// The warp list for a lamp: the Hunter's Dream first, then every lit lamp the player may travel
/// to, grouped and ordered as the headstones list them. Leaves out the lamp the player stands at
/// (`here`, 0 unknown), lamps whose "cannot travel" flag is set (the headstones refuse those too)
/// and ids `known` rejects. `flag(id)` reads an event flag; `known(return point)`: a ReturnPointParam
/// row the game has. Empty: nothing to choose but the Dream (the vanilla question stays).
/// `dream`: the Hunter's Dream as the list's first entry.
template <class FlagFn, class KnownFn>
std::vector<Entry> BuildList(std::uint32_t here, FlagFn flag, KnownFn known, bool dream = true) {
    std::vector<Entry> out;
    Lamp at{};
    const bool here_known = LampOf(here, &at);
    std::uint32_t next = 1;
    if (here_known && dream) {
        // The lamp the player stands at is lit: its flag shows the Dream entry.
        out.push_back({next++, kDreamNameFlag, LitFlag(at), kDreamReturnPoint, Dream});
    }
    for (const Lamp& l : Lamps()) {
        const std::uint32_t rp = ReturnPoint(l);
        if (rp == here || !known(rp) || !flag(LitFlag(l)) || flag(NameFlag(l))) {
            continue;
        }
        out.push_back({next++, NameFlag(l), LitFlag(l), rp, l.group});
    }
    if (out.empty() || (out.size() == 1 && out[0].return_point == kDreamReturnPoint)) {
        out.clear();  // only the Dream: the lamp's own question does that
    }
    return out;
}

/// The return point of the chosen warp-menu `index` (0: none / unknown).
inline std::uint32_t Chosen(const std::vector<Entry>& list, std::uint32_t index) {
    for (const Entry& e : list) {
        if (e.index == index) return e.return_point;
    }
    return 0;
}

/// The lamp nearest to `pos` among `spots` (return point, x, y, z) in map `map` (AA << 24 | B << 16),
/// within `max_m` metres; 0 none.
struct Spot {
    std::uint32_t id;
    float x, y, z;
};
inline std::uint32_t NearestLamp(const std::vector<Spot>& spots, std::uint32_t map, const float pos[3], float max_m) {
    std::uint32_t best = 0;
    float best_d = max_m * max_m;
    for (const Spot& s : spots) {
        if (MapOf(s.id) != (map & 0xffff0000u)) continue;
        const float dx = s.x - pos[0], dy = s.y - pos[1], dz = s.z - pos[2];
        const float d = dx * dx + dy * dy + dz * dz;
        if (d <= best_d) {
            best_d = d;
            best = s.id;
        }
    }
    return best;
}

/// Where the player stands for NearestLamp: its physics position when readable, else the drawn
/// one (chr +0xC0). An in-place warp (vtbl +0x5F8: the test aids, the party's no-load placements)
/// moves the physics position at once and the drawn one only once the player moves (lamprule run 5:
/// at lamp 2412951 with +0xC0 still at the Great Bridge lamp 2412952, 72 m away, so "here" was
/// 2412952 and that lamp, lit, was left out of the list).
inline const float* StandingPos(bool physics_ok, const float physics[3], const float drawn[3]) {
    return physics_ok ? physics : drawn;
}

/// BB_LAMP_TRAVEL: unset -> on; "0" / "off" / "false" -> off.
inline bool SwitchOn(const char* v) {
    if (!v || !v[0]) return true;
    return !(std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0 || std::strcmp(v, "false") == 0);
}

/// Whether a lamp may offer direct travel now: not in the Hunter's Dream (its headstones do it),
/// no cutscene, no boss fight (a party's hold), the world loaded.
struct Situation {
    bool world_up = false, loading = false, cutscene = false, boss = false;
    std::uint32_t map = 0;
};
inline bool Offer(const Situation& s) {
    const std::uint32_t area = (s.map >> 24) & 0xff;
    return s.world_up && !s.loading && !s.cutscene && !s.boss && area != 21 && s.map != 0xffffffffu;
}

/// Map travel (the map view's selected lamp, map/map_page.cpp): why it cannot start now. The same
/// lamp warp as a world lamp's list, from wherever the player stands, so it is refused where a
/// lamp's list would be (loading, cutscene, boss fight) and also in a Chalice Dungeon (area 29: no
/// lamps of the world there, and the dungeon's own exit is its lamp), while dead or spectating a
/// boss fight, and while a lamp's warp menu or another warp is under way. The Hunter's Dream
/// (area 21) is allowed: its headstones run the same warp.
enum class MapTravel { Ready, Off, NotInWorld, Cutscene, Boss, Chalice, Dead, Busy };
struct MapTravelSituation {
    bool on = false;  ///< lamp travel is installed (BB_LAMP_TRAVEL on, 1.09 code)
    bool world_up = false, loading = false, cutscene = false, boss = false;
    std::uint32_t map = 0xffffffffu;
    int hp = -1;              ///< the player's HP (-1: no chr)
    bool spectating = false;  ///< dead in a boss-fight limbo, following a member
    bool busy = false;        ///< a lamp's warp menu is open or a warp is on its way
};
inline MapTravel MapTravelState(const MapTravelSituation& s) {
    if (!s.on) return MapTravel::Off;
    if (!s.world_up || s.loading || s.map == 0xffffffffu) return MapTravel::NotInWorld;
    if (s.cutscene) return MapTravel::Cutscene;
    if (((s.map >> 24) & 0xff) == 29) return MapTravel::Chalice;
    if (s.hp <= 0 || s.spectating) return MapTravel::Dead;
    if (s.boss) return MapTravel::Boss;
    if (s.busy) return MapTravel::Busy;
    return MapTravel::Ready;
}

/// Whether the map may travel to lamp `rp`: a lamp of the table other than the Hunter's Dream (the
/// lamps' own Dream question does that), lit, travel not refused by its flag (as the headstones),
/// and a ReturnPointParam row the game has. `flag(id)`, `known(rp)` as BuildList's.
template <class FlagFn, class KnownFn>
bool MapTravelTarget(std::uint32_t rp, FlagFn flag, KnownFn known) {
    Lamp l{};
    return rp != kDreamReturnPoint && LampOf(rp, &l) && known(rp) && flag(LitFlag(l)) && !flag(NameFlag(l));
}

// ---- Chalice layer travel (CL-48ee07, BB_PARTY_LAYER_TRAVEL) ----
// A Chalice Dungeon (area 29) has up to 9 lamps, return points 2902950 + n (the lamp object is entity 2901950 + n).
// Vanilla offers only "Return to the Hunter's Dream" at them; the altar's "Select location to awaken in" lists the
// lit ones. With the rule on, the dungeon lamp's own Dream question is replaced by the same warp menu as a world
// lamp's: the Dream first, then the dungeon's other lit lamps (the layers). Ids as for the world lamps with block 290:
//   name / blocked flag  70002900 + n,  lit flag 12907810 + 20 n,  Dream flag 72900100 + n.
inline constexpr std::uint32_t kChaliceFirstReturn = 2902950;
inline constexpr int kChaliceLamps = 9;

constexpr bool IsChaliceLamp(std::uint32_t rp) { return rp >= kChaliceFirstReturn && rp < kChaliceFirstReturn + kChaliceLamps; }
constexpr std::uint32_t ChaliceLampIndex(std::uint32_t rp) { return rp - kChaliceFirstReturn; }
constexpr std::uint32_t ChaliceLitFlag(std::uint32_t n) { return 12907810u + 20u * n; }
constexpr std::uint32_t ChaliceNameFlag(std::uint32_t n) { return 70002900u + n; }
constexpr std::uint32_t ChaliceDreamFlag(std::uint32_t n) { return 72900100u + n; }

/// Lamp n of the dungeon may be travelled to: the first lamp (return point 2902950, "Chamber of the Seal", the
/// dungeon's start and the altar's first awaken point) always; the others once lit (the lamp object's lit flag or
/// its registration flag 12907800 + 20 n, as party_chalice_merge's ExpandLamps counts them).
template <class FlagFn>
bool ChaliceLampUnlocked(std::uint32_t n, FlagFn flag) {
    return n == 0 || flag(ChaliceLitFlag(n)) || flag(12907800u + 20u * n);
}

/// The warp list at dungeon lamp `here` (a return point of the 9): the Hunter's Dream first, then every other lit
/// lamp of the dungeon (never the one the player stands at), ascending. `flag(id)`: an event flag read in the
/// dungeon; `known(rp)`: a ReturnPointParam row the game has. Empty (the vanilla question stays): `here` is not a
/// dungeon lamp or no other lamp is lit.
template <class FlagFn, class KnownFn>
std::vector<Entry> BuildChaliceList(std::uint32_t here, FlagFn flag, KnownFn known) {
    std::vector<Entry> out;
    if (!IsChaliceLamp(here)) return out;
    std::uint32_t next = 1;
    out.push_back({next++, kDreamNameFlag, ChaliceLitFlag(ChaliceLampIndex(here)), kDreamReturnPoint, Dream});
    for (std::uint32_t n = 0; n < static_cast<std::uint32_t>(kChaliceLamps); ++n) {
        const std::uint32_t rp = kChaliceFirstReturn + n;
        if (rp == here || !known(rp) || !ChaliceLampUnlocked(n, flag) || flag(ChaliceNameFlag(n))) continue;
        // The game's AddWarpEntry takes an entry only when its 3rd argument flag is set (live: the unlit start lamp
        // 2902950 was dropped, "took 1 of 2"): a lamp unlocked without its own lit flag shows the standing lamp's.
        const std::uint32_t lit = flag(ChaliceLitFlag(n)) ? ChaliceLitFlag(n) : ChaliceLitFlag(ChaliceLampIndex(here));
        out.push_back({next++, ChaliceNameFlag(n), lit, rp, Yharnam});
    }
    if (out.size() == 1) out.clear();
    return out;
}

/// The dungeon lamp nearest to the player among `spots` (return point, x, y, z) within `max_m` metres (0 none).
inline std::uint32_t NearestChaliceLamp(const std::vector<Spot>& spots, const float pos[3], float max_m) {
    std::uint32_t best = 0;
    float best_d = max_m * max_m;
    for (const Spot& s : spots) {
        if (!IsChaliceLamp(s.id)) continue;
        const float dx = s.x - pos[0], dy = s.y - pos[1], dz = s.z - pos[2];
        const float d = dx * dx + dy * dy + dz * dz;
        if (d <= best_d) {
            best_d = d;
            best = s.id;
        }
    }
    return best;
}

/// Whether the host's rule lets a Chalice Dungeon lamp offer the list: the rule is on, the player leads (the host or
/// a solo game; a guest's lamp stays the vanilla question and the party's lamp request).
constexpr bool ChaliceListOffered(bool rule_on, bool guest) { return rule_on && !guest; }

/// CL-48ee07: a guest whose current slot already holds the host's dungeon (no dungeon of its own: the quick join wrote
/// it there, so its game never sends it home) runs the host's dungeon-lamp warp itself. nullptr: do it; else why not.
/// Every id must match: the lamp is one of 2902950..58, the guest's own current dungeon id (`local_channel`, never 0)
/// equals the host's (`host_channel`), the host's rule is on, and this guest is in the dungeon map, in the world and
/// alive, not loading, in no boss fight or cutscene and not staying in its own world.
inline const char* LayerFollowRefusal(bool rule_on, std::uint32_t lamp, std::uint64_t local_channel, std::uint64_t host_channel,
                                      bool in_dungeon_map, bool stay_home, bool loading, bool dead, bool boss) {
    if (!rule_on) return "the rule is off";
    if (!IsChaliceLamp(lamp)) return "not a dungeon lamp";
    if (local_channel == 0 || host_channel == 0 || local_channel != host_channel) return "this game's dungeon is not the host's";
    if (stay_home) return "staying in its own world";
    if (!in_dungeon_map) return "not in the dungeon";
    if (loading) return "loading";
    if (dead) return "dead";
    if (boss) return "boss fight or cutscene";
    return nullptr;
}

/// Other observers of the talk command switch 0x203FFD0, which lamp travel replaces (issue #4: the
/// shared NPC talk's choice observer could not hook it after lamp travel and never ran). They are
/// registered before the game runs and called first in every call, as a prologue hook would be.
template <typename Fn, std::size_t N = 4>
class ObserverList {
public:
    /// False: full, null, or already there (registered once).
    bool Add(Fn fn) {
        if (!fn || count_ >= N) return false;
        for (std::size_t i = 0; i < count_; ++i) {
            if (fns_[i] == fn) return false;
        }
        fns_[count_++] = fn;
        return true;
    }
    template <typename... A>
    void Dispatch(A... args) const {
        for (std::size_t i = 0; i < count_; ++i) fns_[i](args...);
    }
    std::size_t Size() const { return count_; }

private:
    Fn fns_[N] = {};
    std::size_t count_ = 0;
};

/// 0.7.6 (a guest who talked to a window resident near a lamp sat down at the lamp and got its travel
/// list): a guest's lamp answers "client" (stays shut) while the guest's own NPC conversation is on - its
/// talk playing, the item-get box of its gift - and for this many seconds after. The player's OK press of
/// that box and the next Cross presses (a player mashes the button) must not start a lamp in reach.
inline constexpr double kGuestLampAfterTalkGrace = 1.5;

/// A guest's world lamp may answer IsClientPlayer 0 (open) only when its conversation (talk or item box)
/// is not on and did not end within the grace. `talk_idle_s`: seconds since it was last on (0: on now).
constexpr bool GuestLampMayOpen(double talk_idle_s) { return talk_idle_s >= kGuestLampAfterTalkGrace; }

} // namespace coop::lamptravel
