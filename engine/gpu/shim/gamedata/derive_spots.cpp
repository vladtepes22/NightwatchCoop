// SPDX-License-Identifier: GPL-3.0-or-later
// Places inside the boss arenas, read from the map studio files (MSB) of the player's own game for the boss
// fog gates (bossGates, derive_worldact.cpp):
//
//  * bossArenas: the arena's entry floor of each boss character (world_warper_rules.h BossArenaSpot). Per gate,
//    the fog object (the PARTS entry of the gate's fog entity) fixes where the wall stands; the floor is the
//    map's POINT named `霧壁埋まりワープ先` ("warp target when stuck in the fog wall") within 5 m of it, which sits
//    just inside the fog, else the nearest POINT named `進入目標` / `侵入目標` ("room entry target") within 40 m
//    (maps whose fog wall has no stuck check). The boss characters of the gate: the characters its boss-defeat
//    event waits on (4[0]) or marks (2003 boss-defeat markers), and the character parts of the map whose entity
//    id differs from one of those only in the last digit (the other halves of the fight).
//  * firstFightSpots: where the host's character must stand for the host's first-fight event of a gate
//    (its `intro` event) to see its one position condition hold: 3[2] (player in a region): the region's POINT
//    raised 0.5 m (inside the box); 3[3] (player within some metres of a character): that character's PARTS entry
//    raised 0.5 m and moved 2 m along z. `scene`: the event plays a cutscene (2002); `chained`: it is the second
//    half (`後半`) of a two-part first fight. An event without exactly one position condition has no row (the
//    first fight starts by an action in the room).
#include "derive.h"

#include "container.h"
#include "msb.h"
#include "parallel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>

namespace coop::gamedata {
namespace {

bool Has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

constexpr std::int64_t kPlayer = 10000;  // the player's chr type in a condition
constexpr double kStuckRange = 5.0, kEntryRange = 40.0;

struct MapGeo {
    bool present = false;
    std::vector<bbdata::MsbPoint> points;
    std::vector<std::string> point_names;
    std::vector<bbdata::MsbPart> parts;
};

double Dist(const bbdata::MsbPoint& p, const bbdata::MsbPart& f) {
    const double dx = double(p.x) - f.x, dy = double(p.y) - f.y, dz = double(p.z) - f.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/// The part with `entity` (of part type `type`; 0 = any type, objects first).
const bbdata::MsbPart* FindPart(const MapGeo& g, std::int64_t entity, int type) {
    const bbdata::MsbPart* any = nullptr;
    for (const bbdata::MsbPart& p : g.parts) {
        if (!p.has_entity || p.entity != entity) continue;
        if (type == 0 || p.type == type) return &p;
        if (!any) any = &p;
    }
    return type == 0 ? any : nullptr;
}

/// The nearest point to `fog` whose name holds one of `keys`, within `range` m.
const bbdata::MsbPoint* NearestNamed(const MapGeo& m, const bbdata::MsbPart& fog, std::initializer_list<const char*> keys, double range) {
    const bbdata::MsbPoint* best = nullptr;
    double best_d = 0;
    for (std::size_t i = 0; i < m.points.size(); ++i) {
        bool named = false;
        for (const char* k : keys) named = named || Has(m.point_names[i], k);
        if (!named) continue;
        const double d = Dist(m.points[i], fog);
        if (d <= range && (!best || d < best_d)) best = &m.points[i], best_d = d;
    }
    return best;
}

}  // namespace

Sections DeriveSpots(const std::vector<FileEvents>& events, const Sections& worldact, const std::string& msb_dir, unsigned threads) {
    namespace fs = std::filesystem;
    struct Gate {
        std::int64_t entry, boss, intro, fog;
        const FileEvents* file = nullptr;
    };
    std::vector<Gate> gates;
    const auto wa = worldact.find("bossGates");
    if (wa != worldact.end()) {
        for (const Row& r : wa->second) {
            if (r.size() >= 7) gates.push_back({std::stoll(r[0]), std::stoll(r[3]), std::stoll(r[4]), std::stoll(r[5])});
        }
    }
    // the script file (= the map) each gate's events live in
    std::set<const FileEvents*> files;
    for (Gate& g : gates) {
        for (const FileEvents& fe : events) {
            const bool hit = std::any_of(fe.recs.begin(), fe.recs.end(), [&](const bbdata::EventRec& r) { return r.event == g.entry; });
            if (hit) {
                g.file = &fe;
                files.insert(&fe);
                break;
            }
        }
    }
    const std::vector<const FileEvents*> maps(files.begin(), files.end());
    std::vector<MapGeo> geo(maps.size());
    ParallelFor(maps.size(), threads, [&](std::size_t k) {
        const fs::path p = fs::path(msb_dir) / (maps[k]->stem + ".msb.dcx");
        std::error_code ec;
        if (!fs::exists(p, ec)) return;
        const bbdata::Bytes data = bbdata::ReadMaybeDcx(p.string());
        const bbdata::Span s(data);
        const auto lists = bbdata::ParseMsbLists(s);
        const auto* pts = bbdata::FindMsbList(lists, "POINT_PARAM_ST");
        const auto* parts = bbdata::FindMsbList(lists, "PARTS_PARAM_ST");
        if (!pts || !parts) throw bbdata::FormatError(p.string() + ": no POINT_PARAM_ST / PARTS_PARAM_ST");
        geo[k].present = true;
        geo[k].points = bbdata::ReadMsbPoints(s, *pts, false);
        geo[k].point_names = bbdata::ReadMsbNames(s, *pts);
        geo[k].parts = bbdata::ReadMsbParts(s, *parts);
    });

    std::map<std::int64_t, std::array<double, 3>> arenas;
    struct Fight {
        double x, y, z;
        bool chained, scene;
    };
    std::map<std::int64_t, Fight> fights;
    for (const Gate& g : gates) {
        if (!g.file) continue;
        const MapGeo& m = geo[static_cast<std::size_t>(std::find(maps.begin(), maps.end(), g.file) - maps.begin())];
        if (!m.present) continue;
        const FileEvents& fe = *g.file;
        // ---- the arena's entry floor ----
        const bbdata::MsbPart* fog = FindPart(m, g.fog, 1);
        if (!fog) fog = FindPart(m, g.fog, 0);
        const bbdata::MsbPoint* floor = nullptr;
        if (fog) {
            floor = NearestNamed(m, *fog, {"霧壁埋まりワープ先"}, kStuckRange);
            if (!floor) floor = NearestNamed(m, *fog, {"進入目標", "侵入目標"}, kEntryRange);
        }
        if (floor) {
            std::set<std::int64_t> named;
            for (const bbdata::EventRec& r : fe.recs) {
                if (r.event != g.boss) continue;
                for (const bbdata::Cond& c : r.conds) {
                    if (c.bank == 4 && c.id == 0 && c.v.size() >= 2 && c.v[1] > 0) named.insert(c.v[1]);
                }
                for (const bbdata::Marker& mk : r.markers) {
                    if ((mk.kind == bbdata::MarkerKind::BossDefeat || mk.kind == bbdata::MarkerKind::MidbossDefeat) && !mk.v.empty() && mk.v[0] > 0) {
                        named.insert(mk.v[0]);
                    }
                }
            }
            std::set<std::int64_t> decades;
            for (const std::int64_t e : named) decades.insert(e / 10);
            std::set<std::int64_t> chrs = named;
            for (const bbdata::MsbPart& p : m.parts) {
                if (p.type == 2 && p.has_entity && decades.count(p.entity / 10)) chrs.insert(p.entity);
            }
            for (const std::int64_t e : chrs) arenas.emplace(e, std::array<double, 3>{floor->x, floor->y, floor->z});
        }
        // ---- the host's first-fight spot ----
        if (!g.intro) continue;
        for (const bbdata::EventRec& r : fe.recs) {
            if (r.event != g.intro) continue;
            int positions = 0;
            std::int64_t region = 0, target = 0;
            for (const bbdata::Cond& c : r.conds) {
                if (c.bank == 3 && c.id == 2 && c.v.size() >= 4) {
                    ++positions;
                    region = c.v[3];
                } else if (c.bank == 3 && c.id == 3 && c.v.size() >= 4) {
                    ++positions;
                    target = c.v[2] == kPlayer ? c.v[3] : c.v[2];
                }
            }
            if (positions != 1) break;
            double x = 0, y = 0, z = 0;
            bool found = false;
            if (region) {
                for (std::size_t i = 0; i < m.points.size() && !found; ++i) {
                    if (m.points[i].has_entity && m.points[i].entity == region) {
                        x = m.points[i].x, y = double(m.points[i].y) + 0.5, z = m.points[i].z, found = true;
                    }
                }
            } else if (const bbdata::MsbPart* t = FindPart(m, target, 0)) {
                x = t->x, y = double(t->y) + 0.5, z = double(t->z) + 2.0, found = true;
            }
            if (!found) break;
            bool scene = false;
            for (const bbdata::Marker& mk : r.markers) scene = scene || (mk.kind == bbdata::MarkerKind::Cutscene && mk.sub != 8);
            fights.emplace(g.intro, Fight{x, y, z, Has(r.name, "後半"), scene});
            break;
        }
    }
    Sections s;
    auto& a = s["bossArenas"];
    for (const auto& [e, p] : arenas) a.push_back({std::to_string(e), FixedText(p[0], 2), FixedText(p[1], 2), FixedText(p[2], 2)});
    auto& f = s["firstFightSpots"];
    for (const auto& [e, p] : fights) {
        f.push_back({std::to_string(e), FixedText(p.x, 2), FixedText(p.y, 2), FixedText(p.z, 2), p.chained ? "1" : "0", p.scene ? "1" : "0"});
    }
    return s;
}

}  // namespace coop::gamedata
