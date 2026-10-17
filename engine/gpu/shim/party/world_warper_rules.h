// SPDX-License-Identifier: GPL-3.0-or-later
// The world warper (world_warper.h): the pure rules. Header only, no game dependency (unit tests:
// tests/test_world_warper.cpp).
//
//   Route    every ReturnPointParam row with a map and a spot (party_ids: kPartyReturnPoints /
//            ReturnSpot), the Chalice Dungeons (area 29, generated layouts) left out: the Hunter's
//            Dream first, then the lamps in the headstones' order (lamp_travel_rules.h Lamps()), then
//            the other rows (boss-arena and event respawn points) by id. `lamps=A+B` replaces it.
//   Progress a text file (BB_WORLD_WARPER_STATE), one line per event, appended and flushed at once
//            so a crash keeps everything before it:
//              start <rp>                     the warp to <rp> began
//              done <rp> <new> <hitches> ...  the area's routine finished (the numbers for the log)
//              skip <rp> <reason>             no load / stuck: never tried again
//              finished                       the whole route is done (the launcher exports)
//            A row started `kMaxAttempts` times without a done (the game crashed or hung there
//            each time) is skipped on the next start: resume after a crash.
//   Options  BB_WORLD_WARPER = tour | replay | measure (replay, tour, replay again: the stutter
//            check in one session), then ",key=value" items: lamps=A+B (the tour's route),
//            replay=A+B (the replay's route), max=N (the first N rows of the route), noact (no
//            weapon / item actions), noenemy (no enemies), nokill, enemies=N (new enemy kinds per
//            row, 6), chalice (the Chalice Dungeon rows too: the save's current dungeon), notod (no
//            time-of-day pass), nogear (no armor / weapon rotation), noremo (no cutscenes), bosses (idle bosses too),
//            noitems (no item-effect pass), dump (log the player record), remoonly (only the
//            cutscene pass), remos=A+B (the cutscene pass's cutscenes), remoskip=S (a cutscene is
//            skipped S s after it started; 150).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "lamp_travel_rules.h"
#include "party_gamedata.h"

namespace coop::warper {

// The gear lists come from the player's own params (party_gamedata.h; scripts/partydata/warper_tool.py): the
// right-hand and left-hand weapon base rows and the armor head rows, one per model. Empty without the tables.
inline const std::vector<std::uint32_t>& RightWeapons() { return gamedata::Get().warper_right; }
inline const std::vector<std::uint32_t>& LeftWeapons() { return gamedata::Get().warper_left; }
inline const std::vector<std::uint32_t>& ArmorSets() { return gamedata::Get().warper_armor; }

enum class Mode : std::uint8_t { Off, Tour, Replay, Measure };

struct Options {
    Mode mode = Mode::Off;
    std::vector<std::uint32_t> lamps;   ///< lamps=: the tour's route (empty: every row)
    std::vector<std::uint32_t> replay;  ///< replay=: the replay route (empty: kDefaultReplay)
    int max = 0;                        ///< max=N: the first N rows only (0: all)
    bool act = true, enemies = true, kill = true;
    bool chalice = false;  ///< chalice: the Chalice Dungeon rows too (the save's current dungeon)
    bool tod = true;       ///< notod: no time-of-day pass (day / evening / night / Blood Moon)
    bool gear = true;      ///< nogear: no armor set / weapons a row
    bool remo = true;      ///< noremo: no cutscenes
    bool bosses = false;   ///< bosses: loaded bosses are fought too (opt-in: an idle boss outside its fight
                           ///< stands in its closed arena; going there put the player in the walls)
    bool items = true;     ///< noitems: no item-effect pass (SpEffects of every goods)
    // Work in progress, opt-in (not verified live: the gesture request did not play a gesture on
    // 2026-10-16; the tools' use is not checked yet): `gestures`, `tools`, `remopass`.
    bool gestures = false; ///< gestures: the gesture pass
    bool tools = false;    ///< tools: the hunter-tool / throwable pass
    bool remo_pass = false; ///< remopass: the cutscene pass after the tour
    int gestures_max = 0, tools_max = 0;  ///< gestures=N / tools=N: at most N of them (0: all)
    bool dump = false;     ///< dump: log the player record (equipment research)
    int enemies_per_row = 6;  ///< enemies=N: at most N new enemy kinds per row
    bool remo_only = false;   ///< remoonly: only the cutscene pass (no tour, no time of day)
    std::vector<std::uint32_t> remos;  ///< remos=: the cutscene pass's list (empty: every known one)
    int remo_skip = 150;      ///< remoskip=S: skip a playing cutscene after S s
};

/// A word option ("noact", "chalice", ...): true when `item` is one (applied to `o`).
inline bool ApplyFlag(Options& o, const std::string& item) {
    if (item == "noact") o.act = false;
    else if (item == "noenemy") o.enemies = false;
    else if (item == "nokill") o.kill = false;
    else if (item == "chalice") o.chalice = true;
    else if (item == "notod") o.tod = false;
    else if (item == "nogear") o.gear = false;
    else if (item == "gear") o.gear = true;
    else if (item == "noremo") o.remo = false;
    else if (item == "remo") o.remo = true;
    else if (item == "nobosses") o.bosses = false;
    else if (item == "bosses") o.bosses = true;
    else if (item == "noitems") o.items = false;
    else if (item == "dump") o.dump = true;
    else if (item == "remoonly") o.remo_only = o.remo = true;
    else if (item == "nogestures") o.gestures = false;
    else if (item == "gestures") o.gestures = true;
    else if (item == "notools") o.tools = false;
    else if (item == "tools") o.tools = true;
    else if (item == "remopass") o.remo_pass = true;
    else return false;
    return true;
}

/// The replay route (the stutter check): four lamps in four areas that a save past the first boss
/// usually has, each a different kind of scene (the Dream, streets, a cathedral, a forest).
inline constexpr std::uint32_t kDefaultReplay[] = {2102950, 2412950, 2402950, 2202950};

inline constexpr int kMaxAttempts = 2;

/// How far from the row's spot an enemy may be to be fought (m): farther ones stand in parts of
/// the map that are not walkable from here.
inline constexpr float kEnemyRange = 45.0f;

inline std::vector<std::uint32_t> ParseIds(const std::string& s) {
    std::vector<std::uint32_t> out;
    std::size_t at = 0;
    while (at < s.size()) {
        std::size_t end = s.find_first_of("+ /", at);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(at, end - at);
        at = end + 1;
        if (item.empty()) continue;
        char* e = nullptr;
        const unsigned long v = std::strtoul(item.c_str(), &e, 10);
        if (e && *e == 0 && v > 0 && v < 100000000ul) out.push_back(static_cast<std::uint32_t>(v));
    }
    return out;
}

inline Options ParseOptions(const char* env) {
    Options o;
    if (!env || !env[0]) return o;
    std::string s = env;
    for (char& c : s) {
        if (c == ';') c = ',';
    }
    std::size_t at = 0;
    bool first = true;
    while (at <= s.size()) {
        std::size_t end = s.find(',', at);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(at, end - at);
        at = end + 1;
        if (first) {
            first = false;
            if (item == "0" || item == "off" || item == "false" || item == "no") return Options{};
            if (item == "replay") o.mode = Mode::Replay;
            else if (item == "measure") o.mode = Mode::Measure;
            else o.mode = Mode::Tour;  // "1", "on", "tour" (or an option first: a tour)
            Options probe;
            if (item.find('=') == std::string::npos && !ApplyFlag(probe, item)) continue;
        }
        if (item.rfind("lamps=", 0) == 0) o.lamps = ParseIds(item.substr(6));
        else if (item.rfind("replay=", 0) == 0) o.replay = ParseIds(item.substr(7));
        else if (item.rfind("max=", 0) == 0) o.max = std::max(0, std::atoi(item.c_str() + 4));
        else if (item.rfind("enemies=", 0) == 0) o.enemies_per_row = std::max(0, std::atoi(item.c_str() + 8));
        else if (item.rfind("remos=", 0) == 0) o.remos = ParseIds(item.substr(6));
        else if (item.rfind("gestures=", 0) == 0) o.gestures = true, o.gestures_max = std::max(0, std::atoi(item.c_str() + 9));
        else if (item.rfind("tools=", 0) == 0) o.tools = true, o.tools_max = std::max(0, std::atoi(item.c_str() + 6));
        else if (item.rfind("remoskip=", 0) == 0) o.remo_skip = std::max(5, std::atoi(item.c_str() + 9));
        else ApplyFlag(o, item);
    }
    return o;
}

/// Chalice Dungeon rows (area 29): generated layouts, no fixed spot, a ritual to enter.
constexpr bool ChaliceRow(std::uint32_t rp) {
    return rp / 100000 == 29;
}

/// The tour: `known` = every ReturnPointParam row (sorted or not); `has_spot(rp)`: the row has a
/// spot. Dream first, the lamps in the headstones' order, then the remaining rows by id; each once.
template <class SpotFn>
std::vector<std::uint32_t> BuildRoute(const std::vector<std::uint32_t>& known, SpotFn has_spot,
                                      const Options& o = Options{}) {
    std::vector<std::uint32_t> out;
    std::set<std::uint32_t> have(known.begin(), known.end()), used;
    auto add = [&](std::uint32_t rp) {
        if (!have.count(rp) || used.count(rp) || (ChaliceRow(rp) && !o.chalice) || !has_spot(rp)) return;
        used.insert(rp);
        out.push_back(rp);
    };
    if (!o.lamps.empty()) {
        for (std::uint32_t rp : o.lamps) add(rp);
    } else {
        add(lamptravel::kDreamReturnPoint);
        for (const lamptravel::Lamp& l : lamptravel::Lamps()) add(lamptravel::ReturnPoint(l));
        std::vector<std::uint32_t> rest(have.begin(), have.end());
        for (std::uint32_t rp : rest) add(rp);
    }
    if (o.max > 0 && out.size() > static_cast<std::size_t>(o.max)) out.resize(static_cast<std::size_t>(o.max));
    return out;
}

/// The packed map (AA << 24 | B << 16) of a ReturnPointParam row.
constexpr std::uint32_t MapOfRow(std::uint32_t rp) {
    return lamptravel::MapOf(rp);
}

struct Progress {
    std::map<std::uint32_t, int> starts;
    std::set<std::uint32_t> done, skipped;
    bool finished = false;
    // Content seen in earlier sessions of the run (resume: not fought / played again).
    std::set<std::uint32_t> kinds;      ///< "kind <model>": enemy models killed (or bosses fought)
    std::set<std::uint32_t> remos;      ///< "remo <id>": cutscenes played (or tried)
    std::map<std::uint32_t, int> remo_tries;  ///< "remo <id>" lines per cutscene
    std::set<std::uint32_t> remos_ok;   ///< "remook <id>": cutscenes that started and played
    std::set<std::uint64_t> tod_done;   ///< "tod <value> <rp> ...": time-of-day rows (value << 32 | rp)
    int tod_orig = -1;                  ///< "todorig <value>": the save's own time of day
    bool effects_done = false;          ///< "effects": the item-effect pass ran
    std::set<std::uint32_t> fx;         ///< "fx <id>": item effects applied (or crashed in)
    std::set<std::uint32_t> gestures;   ///< "gesture <n>": gestures played (1-based param id)
    std::set<std::uint32_t> tools;      ///< "tool <goods>": hunter tools / throwables tried
    std::set<std::uint32_t> tools_ok;   ///< "toolok <goods>": ... whose use was seen (its cost)
    std::set<std::uint32_t> gestures_ok; ///< "gestureok <n>": gestures seen playing
};

inline Progress ParseProgress(const std::string& text) {
    Progress p;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "finished") {
            p.finished = true;
            continue;
        }
        if (line == "effects") {
            p.effects_done = true;
            continue;
        }
        const std::size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        const std::string word = line.substr(0, sp);
        char* after = nullptr;
        const std::uint32_t rp = static_cast<std::uint32_t>(std::strtoul(line.c_str() + sp + 1, &after, 10));
        if (word == "todorig") {
            p.tod_orig = static_cast<int>(rp);
            continue;
        }
        if (word == "tod") {  // tod <value> <rp> ...
            const std::uint32_t row = static_cast<std::uint32_t>(std::strtoul(after, nullptr, 10));
            if (row) p.tod_done.insert(std::uint64_t(rp) << 32 | row);
            continue;
        }
        if (!rp) continue;
        if (word == "kind") p.kinds.insert(rp);
        else if (word == "fx") p.fx.insert(rp);
        else if (word == "remo") p.remos.insert(rp), ++p.remo_tries[rp];
        else if (word == "remook") p.remos_ok.insert(rp);
        else if (word == "gesture") p.gestures.insert(rp);
        else if (word == "tool") p.tools.insert(rp);
        else if (word == "toolok") p.tools_ok.insert(rp);
        else if (word == "gestureok") p.gestures_ok.insert(rp);
        else if (word == "start") ++p.starts[rp];
        else if (word == "done") p.done.insert(rp);
        else if (word == "skip") p.skipped.insert(rp);
    }
    return p;
}

/// Why the row is not visited again (nullptr: it is still to do).
inline const char* SkipReason(const Progress& p, std::uint32_t rp) {
    if (p.done.count(rp)) return "done";
    if (p.skipped.count(rp)) return "skipped";
    const auto it = p.starts.find(rp);
    if (it != p.starts.end() && it->second >= kMaxAttempts) return "crashed there";
    return nullptr;
}

/// The next route index to visit at or after `from` (route.size(): nothing left).
inline std::size_t NextIndex(const std::vector<std::uint32_t>& route, const Progress& p, std::size_t from) {
    for (std::size_t i = from; i < route.size(); ++i) {
        if (!SkipReason(p, route[i])) return i;
    }
    return route.size();
}

/// Rows of the route already handled (done, skipped or given up).
inline std::size_t HandledCount(const std::vector<std::uint32_t>& route, const Progress& p) {
    std::size_t n = 0;
    for (std::uint32_t rp : route) n += SkipReason(p, rp) ? 1 : 0;
    return n;
}

/// One step of a scripted input sequence: buttons and sticks held for `seconds`.
struct PadStep {
    float seconds = 0;
    std::uint32_t buttons = 0;
    int lx = -1, ly = -1, rx = -1, ry = -1;  ///< 0..255, -1 = released (the player's own)
    const char* what = "";                   ///< the action's name for the per-action counts
};

// PS4 pad bits (src/runtime_pad.c).
inline constexpr std::uint32_t kL3 = 0x2, kR3 = 0x4, kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80,
                               kL2 = 0x100, kR2 = 0x200, kL1 = 0x400, kR1 = 0x800, kTriangle = 0x1000,
                               kCircle = 0x2000, kSquare = 0x8000;

/// The camera sweep: a full turn right, then up and down (the sky, the ground), then back.
inline std::vector<PadStep> LookSteps() {
    return {
        {5.0f, 0, -1, -1, 255, 128, "look"}, {0.2f, 0, -1, -1, 128, 128, "look"},
        {1.2f, 0, -1, -1, 128, 0, "look"},   {2.0f, 0, -1, -1, 128, 255, "look"},
        {1.0f, 0, -1, -1, 128, 0, "look"},   {0.3f, 0, -1, -1, -1, -1, "look"},
    };
}

/// The walk loop: a circle forward while the camera turns, a dash (Circle held), back, sideways.
inline std::vector<PadStep> WalkSteps() {
    return {
        {3.0f, 0, 128, 0, 200, 128, "walk"},       {2.5f, kCircle, 128, 0, 60, 128, "walk"},
        {0.3f, 0, -1, -1, -1, -1, "walk"},        {2.0f, 0, 128, 255, -1, -1, "walk"},
        {1.5f, 0, 0, 128, -1, -1, "walk"},        {1.5f, 0, 255, 128, -1, -1, "walk"},
        {0.3f, 0, -1, -1, -1, -1, "walk"},
    };
}

/// Weapon actions, both forms: R1 combo, R2 (charged: held), the transform (L1) and its attacks,
/// the firearm (L2), a dodge (Circle) and a backstep, then the transform back.
inline std::vector<PadStep> ActionSteps() {
    auto tap = [](std::uint32_t b, const char* w, float hold = 0.15f, float after = 0.9f) {
        return std::vector<PadStep>{{hold, b, -1, -1, -1, -1, w}, {after, 0, -1, -1, -1, -1, w}};
    };
    std::vector<PadStep> v;
    auto add = [&](std::vector<PadStep> s) { v.insert(v.end(), s.begin(), s.end()); };
    add(tap(kR1, "r1"));
    add(tap(kR1, "r1"));
    add(tap(kR1, "r1", 0.15f, 1.3f));
    add(tap(kR2, "r2", 0.15f, 1.6f));
    add(tap(kR2, "r2 charged", 1.6f, 1.6f));
    add(tap(kL2, "firearm", 0.15f, 1.3f));
    add(tap(kL1, "transform", 0.15f, 1.6f));
    add(tap(kR1, "r1 transformed"));
    add(tap(kR1, "r1 transformed", 0.15f, 1.3f));
    add(tap(kR2, "r2 transformed", 0.15f, 1.8f));
    add(tap(kL1, "transform back", 0.15f, 1.6f));
    v.push_back({0.25f, kCircle, 128, 0, -1, -1, "dodge"});
    v.push_back({1.0f, 0, -1, -1, -1, -1, "dodge"});
    add(tap(kCircle, "backstep", 0.15f, 1.0f));
    return v;
}

/// Once per tour (the first area): the other weapons and items. Left / Right on the d-pad switch
/// the left-hand (firearm / torch / shield) and right-hand weapons, each followed by its attacks;
/// Up / Down cycle the quick items (and tools), Square uses one, Triangle a blood vial.
inline std::vector<PadStep> LoadoutSteps(bool switch_weapons = true) {
    auto tap = [](std::uint32_t b, const char* w, float hold = 0.15f, float after = 0.9f) {
        return std::vector<PadStep>{{hold, b, -1, -1, -1, -1, w}, {after, 0, -1, -1, -1, -1, w}};
    };
    std::vector<PadStep> v;
    auto add = [&](std::vector<PadStep> s) { v.insert(v.end(), s.begin(), s.end()); };
    for (int i = 0; switch_weapons && i < 2; ++i) {
        add(tap(kRight, "weapon switch", 0.15f, 1.8f));
        add(tap(kR1, "r1 (other weapon)", 0.15f, 1.2f));
        add(tap(kL1, "transform (other weapon)", 0.15f, 1.6f));
        add(tap(kR1, "r1 (other weapon)", 0.15f, 1.2f));
        add(tap(kL1, "transform (other weapon)", 0.15f, 1.6f));
        add(tap(kLeft, "left-hand switch", 0.15f, 1.8f));
        add(tap(kL2, "left-hand weapon", 0.15f, 1.4f));
    }
    for (int i = 0; i < 4; ++i) {
        add(tap(kUp, "quick item", 0.15f, 0.6f));
        add(tap(kSquare, "quick item", 0.15f, 2.6f));
    }
    for (int i = 0; i < 3; ++i) {
        add(tap(kDown, "tool / item", 0.15f, 0.6f));
        add(tap(kSquare, "tool / item", 0.15f, 2.6f));
    }
    add(tap(kTriangle, "blood vial", 0.15f, 2.0f));
    return v;
}

inline float Seconds(const std::vector<PadStep>& steps) {
    float s = 0;
    for (const PadStep& p : steps) s += p.seconds;
    return s;
}

/// Bosses (`bosses`): an idle boss stands in its closed arena, and a spot taken from the player's
/// side of it (as for other enemies) lands in the arena's wall. Candidate spots instead: first the
/// arena's own entry floor (the `bossArenas` table, read from the game's MSBs when the party tables are built: the
/// point the fog wall's stuck check warps a player to, just inside the fog, or the boss's entry region where a
/// map has no fog wall), from where the
/// fight starts as in the game; then 4 m in front of the boss (an idle boss faces into its arena),
/// to its right, left and back. Each is checked (ArenaSpotOk / BossSpotOk) before the fight, the
/// next one tried otherwise.
inline constexpr int kBossSpots = 5;  ///< the arena's entry floor, then four round the boss
inline constexpr float kBossSpotDistance = 4.0f;

/// The yaw that faces from `from` to `to` (the game's convention: forward = (-sin, -cos)).
inline float FaceYaw(const float from[3], const float to[3]) {
    return std::atan2(-(to[0] - from[0]), -(to[2] - from[2]));
}

/// The arena's entry floor of boss `entity` (gamedata `bossArenas`); false: none known (also without the tables).
inline bool BossArenaSpot(std::uint32_t entity, float out[3]) {
    const std::vector<gamedata::BossArenaRow>& rows = gamedata::Get().boss_arenas;
    const auto it = std::lower_bound(rows.begin(), rows.end(), entity,
                                     [](const gamedata::BossArenaRow& r, std::uint32_t e) { return r.boss < e; });
    if (it == rows.end() || it->boss != entity) {
        return false;
    }
    out[0] = it->x, out[1] = it->y, out[2] = it->z;
    return true;
}

/// Candidate spot `i` round a boss at `boss` facing `yaw` (0: in front, 1 right, 2 left, 3 back).
inline void BossSpot(const float boss[3], float yaw, int i, float out[3]) {
    static constexpr float kTurn[4] = {0.0f, -1.5707963f, 1.5707963f, 3.14159265f};
    const float a = yaw + kTurn[((i % 4) + 4) % 4];
    out[0] = boss[0] - std::sin(a) * kBossSpotDistance;
    out[1] = boss[1] + 0.3f;
    out[2] = boss[2] - std::cos(a) * kBossSpotDistance;
}

/// A boss spot is usable when, after settling (`settled`) and a short walk toward the boss
/// (`walked`), the player stands on the boss's floor (within 2.5 m of its height: not fallen
/// through, not stuck on a wall's top), near it (under 8 m) and could walk (came 0.4 m or more
/// closer: not stuck inside a wall).
inline bool BossSpotOk(const float boss[3], const float settled[3], const float walked[3]) {
    auto flat = [&](const float p[3]) {
        const float dx = p[0] - boss[0], dz = p[2] - boss[2];
        return std::sqrt(dx * dx + dz * dz);
    };
    const float d0 = flat(settled), d1 = flat(walked);
    return std::fabs(settled[1] - boss[1]) < 2.5f && std::fabs(walked[1] - boss[1]) < 2.5f && d0 < 8.0f &&
           d0 - d1 >= 0.4f;
}

/// The arena's entry floor checked against the spot itself (`spot`: where the player was put): on
/// its floor (within 2.5 m of its height: not fallen through) and the walk toward the boss moved
/// the player 0.4 m or more (not stuck in geometry).
inline bool ArenaSpotOk(const float spot[3], const float settled[3], const float walked[3]) {
    const float dx = walked[0] - settled[0], dz = walked[2] - settled[2];
    return std::fabs(settled[1] - spot[1]) < 2.5f && std::fabs(walked[1] - spot[1]) < 2.5f &&
           std::sqrt(dx * dx + dz * dz) >= 0.4f;
}

/// The title-stage screens the warper tells apart (from the texts on screen): the first-launch
/// brightness / controls screens (a save folder without the game's system data: confirmed with
/// their defaults), a title with New Game but no Continue (no character: the run ends, a Cross
/// would start a New Game), anything else (Cross until the save is continued).
/// Arguments: the last lookup times (ms, 0 = never) of a text of the brightness screen
/// ("brightness"), of the controls screen ("Camera X-Axis"), "Continue" and "New Game", and now.
/// A first-launch screen when its text is the latest; no character when the title's New Game was
/// built 3 s ago or more and Continue never was.
enum class TitleScreen : std::uint8_t { Other, Brightness, Controls, NoCharacter };
inline TitleScreen ClassifyTitle(std::int64_t brightness_ms, std::int64_t controls_ms, std::int64_t continue_ms,
                                 std::int64_t new_game_ms, std::int64_t now_ms) {
    const std::int64_t menu = std::max(continue_ms, new_game_ms);
    if (controls_ms > 0 && controls_ms >= brightness_ms && controls_ms >= menu) return TitleScreen::Controls;
    if (brightness_ms > 0 && brightness_ms >= menu) return TitleScreen::Brightness;
    if (new_game_ms > 0 && continue_ms == 0 && now_ms - new_game_ms >= 1000) return TitleScreen::NoCharacter;
    return TitleScreen::Other;
}

/// The title menu was built (New Game looked up) and Continue was not: wait, never Cross.
inline bool TitleUndecided(std::int64_t continue_ms, std::int64_t new_game_ms) {
    return new_game_ms > 0 && continue_ms == 0;
}

/// The presses on a first-launch screen, 1 s apart (its cursor starts on the top row): the
/// brightness screen Down to Next, Cross; the controls screen Up (wraps to Next), Cross.
inline std::uint32_t FirstLaunchPress(TitleScreen screen, int step) {
    const std::uint32_t move = screen == TitleScreen::Controls ? kUp : kDown;
    return step % 2 == 0 ? move : 0x4000u /* Cross */;
}

/// An enemy's kind for the coverage (one kill per kind): its model cAAAA, the NpcParam row id's
/// first four digits (100010, 10100080, 100007000: c1000, c1010, c1000).
constexpr std::uint32_t EnemyKind(std::uint32_t npc_param_id) {
    std::uint32_t k = npc_param_id;
    while (k >= 10000u) k /= 10u;
    return k;
}

/// A chr the warper fights: a real enemy's NpcParam row. The c9xxx team-23 chrs (the Hunter's
/// Dream's 902010.. set pieces with 999 HP) and the short player-type rows are left alone:
/// they are the Dream's own, not enemies to see die.
constexpr bool EnemyRowFightable(std::uint32_t npc_param_id) {
    return npc_param_id >= 10000u && EnemyKind(npc_param_id) / 1000u != 9u;
}

/// The packed map (AA << 24 | BB << 16) a cutscene belongs to: remo AABBnnnn.
constexpr std::uint32_t RemoMap(std::uint32_t remo) {
    return (remo / 1000000u) << 24 | (remo / 10000u % 100u) << 16;
}

/// A cutscene is asked for at most this many times in a run (the tour's try and the cutscene
/// pass's): one that did not start twice is left out.
inline constexpr int kMaxRemoTries = 2;

/// The cutscene pass (after the tour): every cutscene of `remos` that has not played yet (tried
/// fewer than kMaxRemoTries times), each with a row of its map from `tour` to warp to (a fresh
/// warp before every cutscene: two asked for in one visit ended the game). The rows of a map
/// take turns, so a retry starts from another row. Cutscenes of maps without a row are left out.
inline std::vector<std::pair<std::uint32_t, std::uint32_t>> CutsceneRoute(const std::vector<std::uint32_t>& tour,
                                                                         const std::vector<std::uint32_t>& remos,
                                                                         const Progress& p) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    std::map<std::uint32_t, std::size_t> turn;  // map -> how many cutscenes of it so far
    for (std::uint32_t id : remos) {
        if (p.remos_ok.count(id)) continue;
        const auto t = p.remo_tries.find(id);
        if (t != p.remo_tries.end() && t->second >= kMaxRemoTries) continue;
        std::vector<std::uint32_t> rows;
        for (std::uint32_t rp : tour) {
            if (!ChaliceRow(rp) && MapOfRow(rp) == RemoMap(id)) rows.push_back(rp);
        }
        if (rows.empty()) continue;
        std::size_t& n = turn[RemoMap(id)];
        const std::size_t tried = t == p.remo_tries.end() ? 0 : static_cast<std::size_t>(t->second);
        out.emplace_back(rows[(n + tried) % rows.size()], id);
        ++n;
    }
    return out;
}

/// The time-of-day values (event flags 9800..9802 as one 3-bit value, party_story kTodTable):
/// day, evening, night, Blood Moon.
inline constexpr int kTodValues[4] = {0, 4, 6, 7};
inline const char* TodName(int v) {
    return v == 0 ? "day" : v == 4 ? "evening" : v == 6 ? "night" : v == 7 ? "Blood Moon" : "?";
}

/// Areas whose look follows the time of day (Yharnam, the Dream, the woods, Byrgenwerth;
/// the Nightmares, the DLC and the Chalice Dungeons have their own fixed sky).
constexpr bool TodArea(std::uint32_t rp) {
    const std::uint32_t area = rp / 100000u;
    return area == 21 || area == 22 || area == 23 || area == 24 || area == 25 || area == 27 || area == 28 ||
           area == 32;
}

/// The time-of-day route: the first row of each map (AA_BB) of `tour` in a TodArea.
inline std::vector<std::uint32_t> TodRoute(const std::vector<std::uint32_t>& tour) {
    std::vector<std::uint32_t> out;
    std::set<std::uint32_t> maps;
    for (std::uint32_t rp : tour) {
        if (!TodArea(rp) || ChaliceRow(rp)) continue;
        if (maps.insert(rp / 10000u).second) out.push_back(rp);
    }
    return out;
}

/// The time-of-day passes: every value but the save's own, then back to it (`orig` < 0: all).
inline std::vector<int> TodPasses(int orig) {
    std::vector<int> v;
    for (int t : kTodValues) {
        if (t != orig) v.push_back(t);
    }
    return v;
}

/// The gear of the tour's n-th row (put on when the row's world is up, as the equipment menu
/// does): a right-hand and a left-hand weapon (the first slot of each
/// hand: the ones held) and an armor set, rotating through the gear lists so every
/// model is worn / held at least once in a full tour (34 sets, 26 + 15 weapons). The other weapon
/// slots are left as they are.
struct Gear {
    std::uint32_t right = 0, left = 0;
    std::uint32_t armor = 0;  ///< head row; + 1000 chest, + 2000 arms, + 3000 legs
};
inline Gear GearFor(std::size_t n) {
    Gear g;
    if (!RightWeapons().empty()) g.right = RightWeapons()[n % RightWeapons().size()];
    if (!LeftWeapons().empty()) g.left = LeftWeapons()[n % LeftWeapons().size()];
    if (!ArmorSets().empty()) g.armor = ArmorSets()[n % ArmorSets().size()];
    return g;
}

/// The player record's equipment slots (PlayerGameData, live 1.09 dump 2026-10-15): the inventory
/// index (+0x1F4), gaitem handle (+0x264) and item id (+0x2A4) of each slot, in this order.
enum EquipSlot : int { kSlotLeft1 = 0, kSlotRight1, kSlotLeft2, kSlotRight2, kSlotLeft3, kSlotRight3,
                       kSlotHead = 8, kSlotChest, kSlotArms, kSlotLegs };
inline constexpr std::uint64_t kRecEquipIndex = 0x1f4, kRecEquipHandle = 0x264, kRecEquipId = 0x2a4;

/// Goods whose effect is not shown by the item-effect pass: the warps (Hunter's Mark, Bold
/// Hunter's Mark), the bells (summons, resonance) and the test rows.
constexpr bool ItemEffectAllowed(std::uint32_t goods, std::uint32_t speffect) {
    switch (goods) {
    case 1: case 2: case 100: case 200: case 201: case 205: case 206: case 225: case 226: case 999: case 1400:
        return false;
    default: break;
    }
    // Only the effects with a look of their own (the item names from msg/engus item.msgbnd); the
    // others are cure / heal steps, Echoes gains (Coldblood), item gifts (the brooch, pendant and
    // stone hand out gems with an item box) or story items (Umbilical Cord: Insight and the
    // ending's condition).
    switch (speffect) {
    case 1120:  // Old Hunter Bone
    case 2010:  // Hand Lantern
    case 2014:  // Blue Elixir
    case 2016:  // Blue Elixir (DLC row)
    case 2017:  // Vermin
    case 2160:  // Madman's Knowledge / Blood Dreg
    case 2161:  // Great One's Wisdom
    case 2200:  // Fire Paper
    case 2210:  // Empty Phantasm Shell
    case 2220:  // Bolt Paper
    case 2230:  // Bone Marrow Ash
    case 3010:  // Blood Vial
    case 3020:  // Iosefka's Blood Vial
    case 3030:  // Blood of Arianna / Queenly Blood
    case 3040:  // Blood of Adeline
    case 3060:  // Blood of Adella
    case 3240:  // Beast Blood Pellet
        return true;
    default:
        return false;
    }
}

/// The gestures (the gesture menu's 23 entries; table 0x55526D0, entry p = 1..23: gesture index
/// p - 1, animation 104000 + 10 * (motion - 1), all in c0000's behaviour as Gesture<index>). The
/// game plays one when MenuMan (*0x5562878) +0x2BC holds its param id p (the player's pad update
/// 0x1526750 reads and clears it; no "learned" check), as the gesture menu asks for it.
inline constexpr int kGestureCount = 23;
inline constexpr float kGestureSeconds = 4.5f;  ///< played, then a dodge ends a looping one

/// The hunter tools and throwables (goods in the quick-item slots; EquipParamGoods refCategory 1:
/// a Bullet row) used once each per run, from quick slot 0 with Square: the throwables (Molotov
/// 1200, Delayed Molotov 1201, Poison Knife 1210, Oil Urn 1220, Pebble 1230, Throwing Knife 1240,
/// Rope Molotov 1250, Delayed Rope Molotov 1251, Pungent Blood Cocktail 1260, Numbing Mist 1270)
/// and the quicksilver tools (Tiny Music Box 700, Augur of Ebrietas 2000, A Call Beyond 2010,
/// Beast Roar 2020, Lead Elixir 2030, Choir Bell 2050, Tiny Tonitrus 2070, Executioner's Gloves
/// 2080, Shaman Bone Blade 2090, Messenger's Gift 2110, Blacksky Eye 2120).
inline constexpr std::uint32_t kWarperTools[] = {1200, 1201, 1210, 1220, 1230, 1240, 1250, 1251, 1260, 1270,
                                                 700,  2000, 2010, 2020, 2030, 2050, 2070, 2080, 2090, 2110, 2120};
inline constexpr std::uint32_t kQuicksilverBullets = 900;  ///< goods: the tools' ammunition
inline constexpr float kToolSeconds = 4.0f;

/// A gesture's name (param id 1..23, the gesture menu's order = goods 799 + id): the game's own text from the
/// party tables (message files), "Gesture <goods>" without them.
inline const char* GestureName(std::uint32_t id) {
    return id >= 1 && id <= static_cast<std::uint32_t>(kGestureCount) ? gamedata::GestureText(799u + id) : "?";
}

/// A gesture played: a gesture animation (a000_104000 .. a000_104259) in the player's recent
/// animations that was not among them before the request.
inline bool GestureSeen(const std::vector<int>& before, const std::vector<int>& after) {
    for (int a : after) {
        if (a >= 104000 && a < 104260 && std::find(before.begin(), before.end(), a) == before.end()) return true;
    }
    return false;
}

/// A hunter tool / throwable was used when its use cost something: one of the item (throwables,
/// counts before / after, -1 unknown) or quicksilver bullets (the tools).
inline bool ToolUseSeen(int held_before, int held_after, int bullets_before, int bullets_after) {
    return (held_before > 0 && held_after >= 0 && held_after < held_before) ||
           (bullets_before > 0 && bullets_after >= 0 && bullets_after < bullets_before);
}

/// The gestures / tools still to do in a run (resume: the ones in the progress file are done).
inline std::vector<std::uint32_t> GesturesLeft(const Progress& p) {
    std::vector<std::uint32_t> v;
    for (std::uint32_t g = 1; g <= static_cast<std::uint32_t>(kGestureCount); ++g) {
        if (!p.gestures.count(g)) v.push_back(g);
    }
    return v;
}
inline std::vector<std::uint32_t> ToolsLeft(const Progress& p) {
    std::vector<std::uint32_t> v;
    for (std::uint32_t t : kWarperTools) {
        if (!p.tools.count(t)) v.push_back(t);
    }
    return v;
}

/// Weapon actions after a gear change: the right-hand weapon in both forms (R1, R2, charged R2,
/// transform, R1, R2, back) and the left-hand one (L2); no weapon switch (see Gear).
inline std::vector<PadStep> GearSteps() {
    auto tap = [](std::uint32_t b, const char* w, float hold = 0.15f, float after = 0.9f) {
        return std::vector<PadStep>{{hold, b, -1, -1, -1, -1, w}, {after, 0, -1, -1, -1, -1, w}};
    };
    std::vector<PadStep> v;
    auto add = [&](std::vector<PadStep> s) { v.insert(v.end(), s.begin(), s.end()); };
    add(tap(kR1, "gear: r1", 0.15f, 1.1f));
    add(tap(kR1, "gear: r1", 0.15f, 1.1f));
    add(tap(kR2, "gear: r2", 0.15f, 1.5f));
    add(tap(kR2, "gear: r2 charged", 1.6f, 1.5f));
    add(tap(kL1, "gear: transform", 0.15f, 1.6f));
    add(tap(kR1, "gear: r1 transformed", 0.15f, 1.1f));
    add(tap(kR2, "gear: r2 transformed", 0.15f, 1.6f));
    add(tap(kL1, "gear: transform back", 0.15f, 1.6f));
    add(tap(kL2, "gear: left hand", 0.15f, 1.4f));
    add(tap(kL2, "gear: left hand", 0.15f, 1.4f));
    return v;
}

/// The area's name for the progress display (English, the map's usual name; "" unknown).
inline const char* AreaName(std::uint32_t rp) {
    switch (rp / 10000) {
    case 210: return "Hunter's Dream";
    case 211: return "Abandoned Old Workshop";
    case 220: return "Hemwick Charnel Lane";
    case 230: return "Old Yharnam";
    case 240: return "Cathedral Ward";
    case 241: return "Central Yharnam";
    case 242: return "Upper Cathedral Ward";
    case 250: return "Cainhurst";
    case 260: return "Nightmare of Mensis";
    case 270: return "Forbidden Woods";
    case 280: return "Yahar'gul";
    case 320: return "Byrgenwerth / Lecture Building";
    case 330: return "Nightmare Frontier";
    case 340: return "Hunter's Nightmare";
    case 350: return "Research Hall";
    case 360: return "Fishing Hamlet";
    default: return "";
    }
}

/// One area's line in the progress file: "done <rp> <new> <hitches> <worst ms> <seconds> <map>".
inline std::string DoneLine(std::uint32_t rp, std::uint64_t created, std::uint64_t hitches, double worst_ms,
                            double seconds, const std::string& map) {
    char b[160];
    std::snprintf(b, sizeof b, "done %u %llu %llu %.0f %.0f %s", rp, static_cast<unsigned long long>(created),
                  static_cast<unsigned long long>(hitches), worst_ms, seconds, map.c_str());
    return b;
}

} // namespace coop::warper
