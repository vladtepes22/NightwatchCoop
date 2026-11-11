// SPDX-License-Identifier: GPL-3.0-or-later
// The party's tables of game data, read at run time from the player's own game (no game data is
// committed): which maps / lamps / cutscenes / NPCs exist, which item lots carry a pickup flag and
// which awards are host-only, the Beckoning-Bell helpers, the world warper's gear.
//
// The engine derives them in C++ (gpu/shim/gamedata/, started at engine start on a worker pool; the mods
// overlay is respected because the files read are the ones the game reads) and caches them in
// <user dir>/party_tables.txt, keyed by the inputs. Get() waits for that build; Preparing() / PreparingPercent()
// tell a caller that must not wait (an in-game hint). Without a build (tests, BB_PARTY_TABLES naming a cache
// file) Get() parses the cache file the first time a table is asked for. Missing or unreadable tables: every
// table is empty, Loaded() is false and the reason is logged once and kept in Status(); the consumers then say
// no (an unknown map, lamp or entity id is refused, no item is shared by lot) and never crash.
//
// File format (UTF-8, "\n" or "\r\n"): `BBPARTYTABLES 1`, `key <sha1>`, then per section `[name] <rows>`
// and that many lines of tab-separated fields. Section names and columns: gpu/shim/gamedata/build.cpp (RenderCache), docs/party/gamedata.md.
// Header only, no game dependency; tests install their own tables with Install().
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace coop::gamedata {

struct LotFlag {
    std::int32_t lot;
    std::int32_t flag;
};
struct LedgerRow {
    std::int32_t idx;
    std::int32_t lot;
    std::int32_t done_flag;
    std::string note;
};
struct AwardSpot {
    std::int32_t lot;
    std::int32_t spot_flag;
    std::string note;
};
struct SharedTreasure {
    std::int32_t lot;
    std::int32_t catchup;  // 1: the key catch-up re-gives it
    std::string note;
};
struct BossDropRow {
    std::int32_t lot;
    int area;
    std::string note;
};
struct AuditRowData {
    std::int32_t lot;
    bool required;     // true: ITEM_REQUIRE_SHARED (kinds), false: ITEM_PER_PLAYER (reason)
    std::string note;
};
struct KillAward {
    std::int32_t lot;
    int area;
    std::uint32_t entity;
    std::uint32_t event;
    std::uint32_t spot_flag;
};
/// {npc entity, join flag, leave flag, host-entered flag, arena entry region, x, y, z, yaw}
struct NpcFogRow {
    std::uint32_t npc, join, leave, host_flag, region;
    float x, y, z, yaw;
};

/// A world lamp: AAB = area * 10 + block (241 = m24_01), n = its index in the block, group = the Hunter's Dream
/// headstone that lists it (lamptravel::Group), in the order the headstones list them.
struct LampRow {
    std::uint16_t aab;
    std::uint8_t n;
    std::uint8_t group;
};

/// One event-flag category row {first id, last id, category name, note}; rows keep file order (first match wins).
struct FlagRow {
    std::uint32_t first, last;
    std::string category;
    std::string note;
};

/// A boss fog gate (party_worldact.h BossGate): {guest-entry event, host-entered flag, guest-entered flag, boss flag,
/// intro flag, fog entity, landing event or 0}.
struct BossGateRow {
    std::uint32_t guest_event, host_flag, guest_flag, boss_flag, intro_flag, fog_entity, landing_event;
};
/// An area gate rule (party_worldact.h GateRule): {boss-defeat event (= its flag), gate flag, second boss event or 0}.
struct GateRuleRow {
    std::uint32_t boss, gate, also;
};

/// A per-player weapon gate of the Messenger shop: the event flag and the weapon (base id) it means "got".
struct ShopGateData {
    std::int32_t flag, weapon;
};
/// A starting weapon of the Dream messengers: the stock flag of its shop row and the weapon id it sells.
struct StartWeaponData {
    std::int32_t flag, weapon;
};
/// The entry floor of a boss character's arena (MSB point): world_warper_rules.h BossArenaSpot.
struct BossArenaRow {
    std::uint32_t boss;
    float x, y, z;
};
/// Where the host's character must stand for a first-fight event to see its position condition hold, and whether the
/// event is the second half of a two-part first fight (`chained`) or plays an intro scene (`scene`).
struct FirstFightRow {
    std::uint32_t event;
    float x, y, z;
    bool chained, scene;
};

struct Tables {
    bool loaded = false;
    std::string status = "party tables not loaded";
    // ids (sorted, unique)
    std::vector<std::uint32_t> maps, return_points, entities, remos, npc_mirror;
    std::vector<std::uint32_t> return_spot_ids, warp_spot_ids;
    std::vector<float> return_spots, warp_spots;  // x, y, z, yaw per id
    // npc fog
    std::vector<NpcFogRow> npc_fog;
    // world warper gear
    std::vector<std::uint32_t> warper_right, warper_left, warper_armor;
    // items
    std::vector<LotFlag> lot_flags;
    std::vector<LedgerRow> ledger;
    std::vector<AwardSpot> award_spots;
    std::vector<std::int32_t> unshared;
    std::vector<SharedTreasure> shared_treasure;
    std::vector<BossDropRow> boss_drops;
    std::vector<AuditRowData> audit_rows;
    std::vector<KillAward> kill_awards;
    // event-flag categories (party_progress.cpp resolves the names)
    std::vector<FlagRow> flag_rows;
    std::vector<LampRow> lamps;
    std::vector<std::pair<std::uint32_t, std::string>> gesture_names;  // goods id -> the game's name (message files)
    std::vector<std::uint32_t> host_only_events;  // sorted: boss phase events only the host's copy decides
    std::vector<std::uint32_t> boss_entities, chalice_boss_entities;  // entities given the boss-doping SpEffects (sorted)
    std::vector<BossGateRow> boss_gates;          // file order
    std::vector<GateRuleRow> gate_rules;
    std::vector<std::uint32_t> state_ids, runtime_ids;  // sorted: flags whose state follows the host / flags never synced (counters)
    std::vector<ShopGateData> shop_gates;         // sorted by flag
    std::vector<StartWeaponData> start_weapons;   // file order: right hand (72101000) then left (72101011)
    std::vector<BossArenaRow> boss_arenas;        // sorted by boss entity
    std::vector<FirstFightRow> first_fights;      // sorted by event
};

namespace detail {

inline std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> out;
    std::size_t at = 0;
    for (;;) {
        const std::size_t t = line.find('\t', at);
        if (t == std::string::npos) {
            out.push_back(line.substr(at));
            return out;
        }
        out.push_back(line.substr(at, t - at));
        at = t + 1;
    }
}

inline bool ToU32(const std::string& s, std::uint32_t* v) {
    char* end = nullptr;
    const unsigned long long x = std::strtoull(s.c_str(), &end, 10);
    if (s.empty() || *end != '\0' || x > 0xffffffffull) {
        return false;
    }
    *v = static_cast<std::uint32_t>(x);
    return true;
}
inline bool ToI32(const std::string& s, std::int32_t* v) {
    char* end = nullptr;
    const long long x = std::strtoll(s.c_str(), &end, 10);
    if (s.empty() || *end != '\0' || x < INT32_MIN || x > INT32_MAX) {
        return false;
    }
    *v = static_cast<std::int32_t>(x);
    return true;
}
inline bool ToF(const std::string& s, float* v) {
    char* end = nullptr;
    const float x = std::strtof(s.c_str(), &end);
    if (s.empty() || *end != '\0') {
        return false;
    }
    *v = x;
    return true;
}

}  // namespace detail

/// Parses the cache text into `out` (replacing it). False with `err` set when the text is not a cache of
/// this format or a row is malformed (then `out` is left empty).
inline bool Parse(const std::string& text, Tables* out, std::string* err) {
    using detail::Split;
    *out = Tables{};
    // No iostreams: the fuzz harness replaces operator new, and a stream's buffers come from the C++ runtime's own.
    std::size_t pos = 0;
    std::string line;
    auto next_line = [&]() {
        if (pos >= text.size()) {
            return false;
        }
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            nl = text.size();
        }
        line.assign(text, pos, nl - pos);
        pos = nl + 1;
        return true;
    };
    auto fail = [&](const std::string& m) {
        *out = Tables{};
        out->status = m;
        if (err) {
            *err = m;
        }
        return false;
    };
    if (!next_line() || line.rfind("BBPARTYTABLES 1", 0) != 0) {
        return fail("not a party tables file (wrong header or format version)");
    }
    Tables t;
    std::string section;
    std::size_t left = 0;
    bool any = false;
    auto u32row = [&](std::vector<std::uint32_t>& v, const std::vector<std::string>& f) {
        std::uint32_t x = 0;
        if (f.size() < 1 || !detail::ToU32(f[0], &x)) {
            return false;
        }
        v.push_back(x);
        return true;
    };
    auto spot = [&](std::vector<std::uint32_t>& ids, std::vector<float>& xyz, const std::vector<std::string>& f) {
        std::uint32_t id = 0;
        float v[4];
        if (f.size() < 5 || !detail::ToU32(f[0], &id)) {
            return false;
        }
        for (int k = 0; k < 4; ++k) {
            if (!detail::ToF(f[static_cast<std::size_t>(k) + 1], &v[k])) {
                return false;
            }
        }
        ids.push_back(id);
        xyz.insert(xyz.end(), v, v + 4);
        return true;
    };
    while (next_line()) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (left == 0) {
            if (line.empty() || line.rfind("key ", 0) == 0) {
                continue;
            }
            if (line[0] != '[') {
                return fail("party tables: unexpected line outside a section");
            }
            const std::size_t close = line.find(']');
            if (close == std::string::npos) {
                return fail("party tables: bad section header");
            }
            section = line.substr(1, close - 1);
            std::uint32_t n = 0;
            if (!detail::ToU32(line.size() > close + 2 ? line.substr(close + 2) : std::string(), &n)) {
                return fail("party tables: bad row count in [" + section + "]");
            }
            left = n;
            any = true;
            continue;
        }
        --left;
        const std::vector<std::string> f = Split(line);
        bool ok = true;
        std::int32_t a = 0, b = 0, c = 0;
        std::uint32_t u[5];
        if (section == "maps") ok = u32row(t.maps, f);
        else if (section == "returnPoints") ok = u32row(t.return_points, f);
        else if (section == "entities") ok = u32row(t.entities, f);
        else if (section == "remos") ok = u32row(t.remos, f);
        else if (section == "npcMirror") ok = u32row(t.npc_mirror, f);
        else if (section == "warperRight") ok = u32row(t.warper_right, f);
        else if (section == "warperLeft") ok = u32row(t.warper_left, f);
        else if (section == "warperArmor") ok = u32row(t.warper_armor, f);
        else if (section == "returnSpots") ok = spot(t.return_spot_ids, t.return_spots, f);
        else if (section == "warpSpots") ok = spot(t.warp_spot_ids, t.warp_spots, f);
        else if (section == "npcFog") {
            NpcFogRow r{};
            ok = f.size() >= 9 && detail::ToU32(f[0], &r.npc) && detail::ToU32(f[1], &r.join) &&
                 detail::ToU32(f[2], &r.leave) && detail::ToU32(f[3], &r.host_flag) && detail::ToU32(f[4], &r.region) &&
                 detail::ToF(f[5], &r.x) && detail::ToF(f[6], &r.y) && detail::ToF(f[7], &r.z) && detail::ToF(f[8], &r.yaw);
            if (ok) t.npc_fog.push_back(r);
        } else if (section == "flagRows") {
            ok = f.size() >= 3 && detail::ToU32(f[0], &u[0]) && detail::ToU32(f[1], &u[1]) && !f[2].empty() &&
                 u[0] <= u[1];
            if (ok) t.flag_rows.push_back({u[0], u[1], f[2], f.size() > 3 ? f[3] : std::string()});
        } else if (section == "lamps") {
            ok = f.size() >= 3 && detail::ToU32(f[0], &u[0]) && detail::ToU32(f[1], &u[1]) && detail::ToU32(f[2], &u[2]) &&
                 u[0] < 65536 && u[1] < 256 && u[2] < 256;
            if (ok) t.lamps.push_back({static_cast<std::uint16_t>(u[0]), static_cast<std::uint8_t>(u[1]), static_cast<std::uint8_t>(u[2])});
        } else if (section == "gestureNames") {
            ok = f.size() >= 2 && detail::ToU32(f[0], &u[0]) && !f[1].empty();
            if (ok) t.gesture_names.emplace_back(u[0], f[1]);
        } else if (section == "hostOnlyEvents") {
            ok = u32row(t.host_only_events, f);
        } else if (section == "bossEntities") {
            ok = f.size() >= 2 && detail::ToU32(f[0], &u[0]) && (f[1] == "0" || f[1] == "1");
            if (ok) (f[1] == "1" ? t.chalice_boss_entities : t.boss_entities).push_back(u[0]);
        } else if (section == "bossGates") {
            std::uint32_t w[7];
            ok = f.size() >= 7;
            for (std::size_t k = 0; ok && k < 7; ++k) ok = detail::ToU32(f[k], &w[k]);
            if (ok) t.boss_gates.push_back({w[0], w[1], w[2], w[3], w[4], w[5], w[6]});
        } else if (section == "gateRules") {
            ok = f.size() >= 3 && detail::ToU32(f[0], &u[0]) && detail::ToU32(f[1], &u[1]) && detail::ToU32(f[2], &u[2]);
            if (ok) t.gate_rules.push_back({u[0], u[1], u[2]});
        } else if (section == "stateIds") {
            ok = u32row(t.state_ids, f);
        } else if (section == "runtimeIds") {
            ok = u32row(t.runtime_ids, f);
        } else if (section == "shopGates") {
            ok = f.size() >= 2 && detail::ToI32(f[0], &a) && detail::ToI32(f[1], &b);
            if (ok) t.shop_gates.push_back({a, b});
        } else if (section == "startWeapons") {
            ok = f.size() >= 2 && detail::ToI32(f[0], &a) && detail::ToI32(f[1], &b);
            if (ok) t.start_weapons.push_back({a, b});
        } else if (section == "bossArenas") {
            BossArenaRow r{};
            ok = f.size() >= 4 && detail::ToU32(f[0], &r.boss) && detail::ToF(f[1], &r.x) && detail::ToF(f[2], &r.y) &&
                 detail::ToF(f[3], &r.z);
            if (ok) t.boss_arenas.push_back(r);
        } else if (section == "firstFightSpots") {
            FirstFightRow r{};
            ok = f.size() >= 6 && detail::ToU32(f[0], &r.event) && detail::ToF(f[1], &r.x) && detail::ToF(f[2], &r.y) &&
                 detail::ToF(f[3], &r.z) && (f[4] == "0" || f[4] == "1") && (f[5] == "0" || f[5] == "1");
            if (ok) {
                r.chained = f[4] == "1";
                r.scene = f[5] == "1";
                t.first_fights.push_back(r);
            }
        } else if (section == "itemLotFlag") {
            ok = f.size() >= 2 && detail::ToI32(f[0], &a) && detail::ToI32(f[1], &b);
            if (ok) t.lot_flags.push_back({a, b});
        } else if (section == "itemLedger") {
            ok = f.size() >= 3 && detail::ToI32(f[0], &a) && detail::ToI32(f[1], &b) && detail::ToI32(f[2], &c);
            if (ok) t.ledger.push_back({a, b, c, f.size() > 3 ? f[3] : std::string()});
        } else if (section == "itemAwardSpot") {
            ok = f.size() >= 2 && detail::ToI32(f[0], &a) && detail::ToI32(f[1], &b);
            if (ok) t.award_spots.push_back({a, b, f.size() > 2 ? f[2] : std::string()});
        } else if (section == "itemUnshared") {
            ok = f.size() >= 1 && detail::ToI32(f[0], &a);
            if (ok) t.unshared.push_back(a);
        } else if (section == "itemSharedTreasure") {
            ok = f.size() >= 2 && detail::ToI32(f[0], &a) && detail::ToI32(f[1], &b);
            if (ok) t.shared_treasure.push_back({a, b, f.size() > 2 ? f[2] : std::string()});
        } else if (section == "itemBossDrop") {
            ok = f.size() >= 2 && detail::ToI32(f[0], &a) && detail::ToI32(f[1], &b);
            if (ok) t.boss_drops.push_back({a, b, f.size() > 2 ? f[2] : std::string()});
        } else if (section == "itemRequireShared" || section == "itemPerPlayer") {
            ok = f.size() >= 1 && detail::ToI32(f[0], &a);
            if (ok) t.audit_rows.push_back({a, section == "itemRequireShared", f.size() > 1 ? f[1] : std::string()});
        } else if (section == "itemKillAward") {
            ok = f.size() >= 5 && detail::ToI32(f[0], &a) && detail::ToI32(f[1], &b) && detail::ToU32(f[2], &u[0]) &&
                 detail::ToU32(f[3], &u[1]) && detail::ToU32(f[4], &u[2]);
            if (ok) t.kill_awards.push_back({a, b, u[0], u[1], u[2]});
        }  // an unknown section is skipped: a newer cache read by an older game
        if (!ok) {
            return fail("party tables: malformed row in [" + section + "]");
        }
    }
    if (!any || left != 0) {
        return fail("party tables: file is cut short");
    }
    // The consumers binary-search these: keep the contract even for a hand-edited file.
    for (auto* v : {&t.maps, &t.return_points, &t.entities, &t.remos, &t.npc_mirror, &t.host_only_events, &t.boss_entities, &t.chalice_boss_entities, &t.state_ids, &t.runtime_ids}) {
        std::sort(v->begin(), v->end());
        v->erase(std::unique(v->begin(), v->end()), v->end());
    }
    std::sort(t.unshared.begin(), t.unshared.end());
    std::sort(t.shop_gates.begin(), t.shop_gates.end(), [](const ShopGateData& x, const ShopGateData& y) { return x.flag < y.flag; });
    std::sort(t.boss_arenas.begin(), t.boss_arenas.end(), [](const BossArenaRow& x, const BossArenaRow& y) { return x.boss < y.boss; });
    std::sort(t.first_fights.begin(), t.first_fights.end(), [](const FirstFightRow& x, const FirstFightRow& y) { return x.event < y.event; });
    std::sort(t.lot_flags.begin(), t.lot_flags.end(), [](const LotFlag& x, const LotFlag& y) {
        return x.lot != y.lot ? x.lot < y.lot : x.flag < y.flag;
    });
    t.loaded = true;
    t.status = "party tables loaded";
    *out = std::move(t);
    return true;
}

/// <BB_PARTY_TABLES>, else <BB_USER_DIR or "user">/party_tables.txt.
inline std::string CachePath() {
    if (const char* e = std::getenv("BB_PARTY_TABLES")) {
        if (*e) {
            return e;
        }
    }
    std::string dir = "user";
    if (const char* u = std::getenv("BB_USER_DIR")) {
        if (*u) {
            dir = u;
        }
    }
    return dir + "/party_tables.txt";
}

/// Loads `path`; on failure `out` is empty and `err` says why.
inline bool LoadFile(const std::string& path, Tables* out, std::string* err) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        *out = Tables{};
        out->status = "party tables missing (" + path + ")";
        if (err) {
            *err = out->status;
        }
        return false;
    }
    std::string text;
    char buf[1 << 16];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) {
        text.append(buf, n);
    }
    std::fclose(f);
    return Parse(text, out, err);
}

namespace detail {
inline Tables& Store() {
    static Tables t;
    return t;
}
inline std::once_flag& Once() {
    static std::once_flag o;
    return o;
}
/// The background build's state: 0 none (Get() reads the cache file), 1 building, 2 done.
struct Async {
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<int> state{0};
    std::atomic<int> percent{0};
    std::atomic<bool> rebuilding{false};
    std::string label;
};
inline std::chrono::steady_clock::time_point& RebuildT0() {
    static std::chrono::steady_clock::time_point t;
    return t;
}
inline Async& A() {
    static Async a;
    return a;
}
/// Bumped whenever the store is replaced (build done, first load, Install): derived views of a table rebuild.
inline std::atomic<std::uint64_t>& Gen() {
    static std::atomic<std::uint64_t> g{1};
    return g;
}
}  // namespace detail

namespace detail {
inline std::string& GameDirStore() {
    static std::string d;
    return d;
}
}  // namespace detail
/// The game folder (dvdroot_ps4) the tables were built from, set by the builder; "" when unknown. For code that
/// reads other texts of the player's own game at run time (message files).
inline std::string GameDir() { return detail::GameDirStore(); }
inline void SetGameDir(const std::string& d) { detail::GameDirStore() = d; }

/// The builder (gamedata/build.cpp) marks the start; Get() then waits for FinishBuild().
inline void BeginBuild() { detail::A().state.store(1); }
inline void SetBuildProgress(int percent, const std::string& label) {
    std::lock_guard<std::mutex> lk(detail::A().mu);
    detail::A().percent.store(percent);
    detail::A().label = label;
}
/// Installs the built tables (empty with a status when the build failed) and wakes the waiters.
inline void FinishBuild(Tables t) {
    std::call_once(detail::Once(), [] {});
    {
        std::lock_guard<std::mutex> lk(detail::A().mu);
        detail::Store() = std::move(t);
        detail::Gen().fetch_add(1);
        detail::A().percent.store(100);
        detail::A().state.store(2);
    }
    detail::A().cv.notify_all();
}
/// A real rebuild (cache missing / key changed) started: the builder calls this after the key check. A cache hit
/// never does, so the in-game "preparing" notice can wait for it (the key check alone takes seconds on a big install).
inline void MarkRebuild() {
    detail::RebuildT0() = std::chrono::steady_clock::now();
    detail::A().rebuilding.store(true);
}
inline bool Rebuilding() { return detail::A().rebuilding.load(); }
/// Seconds since MarkRebuild() (0 when none).
inline double RebuildSeconds() {
    return Rebuilding() ? std::chrono::duration<double>(std::chrono::steady_clock::now() - detail::RebuildT0()).count() : 0.0;
}
/// Show the "preparing co-op data" notice only for a real rebuild that is taking a noticeable time.
inline bool ShouldShowRebuildNotice(bool rebuilding, double rebuild_seconds, double min_seconds = 1.0) {
    return rebuilding && rebuild_seconds >= min_seconds;
}
/// Test helper.
inline void ResetRebuildFlag() { detail::A().rebuilding.store(false); }
/// True while the background build runs (never blocks).
inline bool Preparing() { return detail::A().state.load() == 1; }
inline int PreparingPercent() { return detail::A().percent.load(); }
inline std::string PreparingLabel() {
    std::lock_guard<std::mutex> lk(detail::A().mu);
    return detail::A().label;
}

/// The tables. Waits for the background build when one runs; else loaded from the cache file on first use.
/// Never null, never throws; empty when the tables are missing.
inline const Tables& Get() {
    detail::Async& a = detail::A();
    if (a.state.load() != 0) {
        std::unique_lock<std::mutex> lk(a.mu);
        a.cv.wait(lk, [&] { return a.state.load() == 2; });
        return detail::Store();
    }
    std::call_once(detail::Once(), [] {
        std::string err;
        Tables t;
        if (!LoadFile(CachePath(), &t, &err)) {
            std::fprintf(stderr,
                         "[party] game-data tables unavailable: %s. Party features that need them (travel, "
                         "shared items, NPC fog, world warper) refuse until they are built from your game "
                         "files (the engine does it at start).\n",
                         err.c_str());
        }
        detail::Store() = std::move(t);
        detail::Gen().fetch_add(1);
    });
    return detail::Store();
}

/// Changes whenever the tables are replaced (see detail::Gen).
inline std::uint64_t Generation() { return detail::Gen().load(); }

inline bool Loaded() {
    return Get().loaded;
}

/// A gesture's name as the player's game spells it (goods id 800 ..; read from the message files when the tables are
/// built), "Gesture <n>" when the tables have none. The pointer stays valid for the program's life.
inline const char* GestureText(std::uint32_t goods) {
    static std::mutex mu;
    static std::map<std::uint32_t, std::string> cache;
    std::lock_guard<std::mutex> lk(mu);
    const auto it = cache.find(goods);
    if (it != cache.end()) return it->second.c_str();
    std::string name = "Gesture " + std::to_string(goods);
    for (const auto& g : Get().gesture_names) {
        if (g.first == goods) name = g.second;
    }
    return cache.emplace(goods, std::move(name)).first->second.c_str();
}

/// Replaces the tables (tests, or a caller that built them itself). Not thread safe with Get().
inline void Install(Tables t) {
    std::call_once(detail::Once(), [] {});
    detail::Store() = std::move(t);
    detail::Gen().fetch_add(1);
}

/// Test helper: loads the cache named by BB_PARTY_TABLES (or the default path) into the store. False
/// when it is absent: the caller prints "skipped" and returns.
inline bool InstallFromEnv() {
    Tables t;
    std::string err;
    if (!LoadFile(CachePath(), &t, &err)) {
        return false;
    }
    Install(std::move(t));
    return true;
}

}  // namespace coop::gamedata
