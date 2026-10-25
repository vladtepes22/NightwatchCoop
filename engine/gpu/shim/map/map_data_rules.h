// SPDX-License-Identifier: GPL-2.0-or-later
// What the map shows besides its floors, from the map generator's own data on the player's PC
// (<maps>\atlas\maps: atlas.json, features.json, catalog.json, <area>.json): the areas, the lamps
// and whether they are lit, pickups still lying there, ladders / gates / NPCs, the areas hidden
// until their boss is dead ("boss reveal", fog of war), and the place names for the title. Pure:
// the game's event flags come in through a lookup (1 on, 0 off, -1 unknown), so
// tests/test_map_page.cpp covers it. Adapted from Hunter's Atlas
// source/native/gpu/shim/bbport_map_features.cpp and source/compact/bbport_map.cpp (MikeTat1988,
// GPL-2.0-or-later). Map travel is deliberately not here.
#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace BbMapData {

using Json = nlohmann::json;
/// An event flag's value: 1 on, 0 off, -1 unknown (not read yet, no save loaded).
using FlagFn = std::function<int(std::uint32_t)>;

/// The game the generator's data was made from, as features.json records it: the SHA-256 of the
/// 1.09 executable's loaded image (game_image_sha256), the same for every dump of the game.
inline constexpr const char* kGameImageSha256 = "071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a";
/// What generators before 0.2.5 recorded instead (game_eboot_sha256): the hash of one dump's
/// eboot.bin, the only one they accepted. Still valid data (docs/party/version_check.md,
/// "Game identity").
inline constexpr const char* kLegacyEbootSha256 = "d65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9";

/// Whether features.json was made from Bloodborne 1.09.
inline bool FeaturesMatchGame(const Json& features) {
    return features.value("game_image_sha256", "") == kGameImageSha256 ||
           features.value("game_eboot_sha256", "") == kLegacyEbootSha256;
}

struct Tri {
    float v[6];  // x0 z0 x1 z1 x2 z2
};

struct Lamp {
    std::string name;
    float x = 0, y = 0, z = 0;
    std::uint32_t unlock = 0;  // lit flag; 0: none (the Hunter's Dream: its conditions decide)
    std::uint32_t return_point = 0;  // ReturnPointParam row the lamp warp goes to (map travel); 0: none
    std::vector<std::uint32_t> on, off;
};

struct Item {
    std::string name, category;  // category: healing, insight, equipment, echoes
    int quantity = 1;
    std::uint32_t collected = 0;
};

struct Pickup {
    float x = 0, y = 0, z = 0;
    std::vector<std::uint32_t> on, off;
    std::vector<Item> items;
};

struct Reveal {
    float min_y = -1e9f, max_y = 1e9f;
    bool has_bounds = false, has_centre = false;
    float bounds[4] = {}, cx = 0, cz = 0, radius = 0;
    std::vector<Tri> tris;
    std::vector<std::uint32_t> on, off, boss;
    Json when;  // null: none
};

struct Marker {
    float x = 0, y = 0, z = 0;
    std::string kind, name;  // kind: ladder, gate, npc
    int entity = 0;
};

struct Place {
    std::string name;
    float min_y = -1e9f, max_y = 1e9f;
    bool main = false;
    std::vector<Tri> tris;
};

struct Landmark {
    std::string name, prefix = "Near ";
    float x = 0, y = 0, z = 0, max_h = 60, max_v = 6;
};

struct Area {
    std::string id, name;  // id: the region, m24_00_00_00
    float bounds[4] = {};  // min x, min z, max x, max z
    std::vector<Lamp> lamps;
    std::vector<Pickup> pickups;
    std::vector<Reveal> reveals;
    std::map<int, Json> npc_rules;
    bool always_visible = false;
    std::vector<Place> places;
    std::vector<Landmark> landmarks;
};

// ---- Reading the generator's files ----

inline bool Finite(float v) { return std::isfinite(v); }

inline void Point3(const Json& p, float* x, float* y, float* z) {
    if (!p.is_array() || p.size() != 3) throw std::runtime_error("map data: bad position");
    *x = p[0].get<float>();
    *y = p[1].get<float>();
    *z = p[2].get<float>();
    if (!Finite(*x) || !Finite(*y) || !Finite(*z)) throw std::runtime_error("map data: bad position");
}

inline std::vector<std::uint32_t> Flags(const Json& j, const char* key) {
    std::vector<std::uint32_t> out;
    for (const auto& id : j.value(key, Json::array())) out.push_back(id.get<std::uint32_t>());
    return out;
}

inline std::vector<Tri> Tris(const Json& list) {
    std::vector<Tri> out;
    for (const auto& t : list) {
        if (!t.is_array() || t.size() != 6) continue;
        Tri tri{};
        bool ok = true;
        for (int k = 0; k < 6; ++k) ok = ok && Finite(tri.v[k] = t[k].get<float>());
        if (ok) out.push_back(tri);
    }
    return out;
}

/// atlas.json: the areas with names and bounds, in the generator's order.
inline std::vector<Area> ParseAtlas(const Json& atlas) {
    if (atlas.at("version") != 1) throw std::runtime_error("map data: atlas.json version");
    std::vector<Area> out;
    for (const auto& m : atlas.at("maps")) {
        Area a;
        a.id = m.at("id").get<std::string>();
        a.name = m.at("name").get<std::string>();
        if (a.id.size() != 12 || a.id.find_first_not_of("m0123456789_") != std::string::npos) continue;
        const auto& b = m.at("bounds");
        for (int k = 0; k < 4; ++k) a.bounds[k] = b.at(k).get<float>();
        if (a.bounds[2] > a.bounds[0] && a.bounds[3] > a.bounds[1]) out.push_back(std::move(a));
    }
    return out;
}

inline Area* Find(std::vector<Area>& areas, const std::string& id) {
    for (auto& a : areas)
        if (a.id == id) return &a;
    return nullptr;
}
inline const Area* Find(const std::vector<Area>& areas, const std::string& id) {
    for (const auto& a : areas)
        if (a.id == id) return &a;
    return nullptr;
}

/// features.json: lamps, pickups, reveal regions, NPC rules. Throws when it was made from another
/// game version.
inline void ApplyFeatures(const Json& features, std::vector<Area>& areas) {
    if (features.at("schema") != 1 || !FeaturesMatchGame(features))
        throw std::runtime_error("map data: features.json is for another game version");
    for (auto it = features.at("areas").begin(); it != features.at("areas").end(); ++it) {
        Area* a = Find(areas, it.key());
        if (!a) continue;
        const Json& f = it.value();
        for (const auto& l : f.value("lamps", Json::array())) {
            Lamp lamp;
            lamp.name = l.at("name").get<std::string>();
            Point3(l.at("pos"), &lamp.x, &lamp.y, &lamp.z);
            lamp.unlock = l.value("unlock_flag", 0u);
            lamp.return_point = l.value("return_point", 0u);
            lamp.on = Flags(l, "available_on");
            lamp.off = Flags(l, "available_off");
            a->lamps.push_back(std::move(lamp));
        }
        for (const auto& p : f.value("pickups", Json::array())) {
            Pickup pickup;
            Point3(p.at("pos"), &pickup.x, &pickup.y, &pickup.z);
            pickup.on = Flags(p, "available_on");
            pickup.off = Flags(p, "available_off");
            for (const auto& c : p.at("contents")) {
                Item item;
                item.name = c.at("name").get<std::string>();
                item.category = c.at("category").get<std::string>();
                item.quantity = c.value("quantity", 1);
                item.collected = c.at("collected_flag").get<std::uint32_t>();
                pickup.items.push_back(std::move(item));
            }
            if (!pickup.items.empty()) a->pickups.push_back(std::move(pickup));
        }
        for (const auto& r : f.value("reveal_regions", Json::array())) {
            Reveal reveal;
            reveal.min_y = r.value("min_y", -1e9f);
            reveal.max_y = r.value("max_y", 1e9f);
            if (r.contains("bounds") && r["bounds"].size() == 4) {
                reveal.has_bounds = true;
                for (int k = 0; k < 4; ++k) reveal.bounds[k] = r["bounds"][k].get<float>();
            }
            if (r.contains("centre") && r["centre"].size() == 3) {
                reveal.has_centre = true;
                reveal.cx = r["centre"][0].get<float>();
                reveal.cz = r["centre"][2].get<float>();
                reveal.radius = r.value("radius", 0.0f);
            }
            if (r.contains("triangles")) reveal.tris = Tris(r["triangles"]);
            reveal.on = Flags(r, "available_on");
            reveal.off = Flags(r, "available_off");
            reveal.boss = Flags(r, "boss_flags");
            if (r.contains("reveal_when")) reveal.when = r["reveal_when"];
            // Kept even when unusable: the compiled maps index the regions by position.
            a->reveals.push_back(std::move(reveal));
        }
        const Json npc = f.value("npc_rules", Json::object());
        for (auto n = npc.begin(); n != npc.end(); ++n) a->npc_rules[std::stoi(n.key())] = n.value();
        a->always_visible = f.value("always_visible", false);
    }
}

/// catalog.json: the named places (title) and landmarks ("Near ...", subtitle) of each area.
inline void ApplyCatalog(const Json& catalog, std::vector<Area>& areas) {
    for (auto& a : areas) {
        if (!catalog.contains(a.id)) continue;
        const Json& entry = catalog.at(a.id);
        for (const auto& item : entry.value("landmarks", Json::array())) {
            if (!item.contains("name") || !item.contains("pos")) continue;
            Landmark m;
            m.name = item.at("name").get<std::string>();
            try {
                Point3(item.at("pos"), &m.x, &m.y, &m.z);
            } catch (const std::exception&) {
                continue;
            }
            m.max_h = item.value("max_horizontal_distance", 60.0f);
            m.max_v = item.value("max_vertical_distance", 6.0f);
            m.prefix = item.value("subtitle_prefix", std::string("Near "));
            if (m.max_h > 0 && m.max_v >= 0) a.landmarks.push_back(std::move(m));
        }
        for (const auto& item : entry.value("regions", Json::array())) {
            if (!item.contains("name") || item.value("shape", std::string("footprint")) != "footprint") continue;
            Place p;
            p.name = item.at("name").get<std::string>();
            p.min_y = item.value("min_y", -1e9f);
            p.max_y = item.value("max_y", 1e9f);
            p.main = item.value("main_location", false);
            if (item.contains("triangles")) p.tris = Tris(item["triangles"]);
            if (!p.tris.empty() && p.min_y <= p.max_y) a.places.push_back(std::move(p));
        }
    }
}

/// <area>.json's markers: ladders, gates and NPC conversation sites (its floors are the baked maps').
inline std::vector<Marker> ParseMarkers(const Json& area_file) {
    std::vector<Marker> out;
    for (const auto& m : area_file.value("markers", Json::array())) {
        Marker marker;
        marker.kind = m.value("kind", std::string());
        if (marker.kind != "ladder" && marker.kind != "gate" && marker.kind != "npc") continue;
        try {
            Point3(m.at("pos"), &marker.x, &marker.y, &marker.z);
        } catch (const std::exception&) {
            continue;
        }
        marker.name = m.value("name", std::string());
        marker.entity = m.value("entity_id", 0);
        out.push_back(std::move(marker));
    }
    return out;
}

/// Every event flag the areas' rules read (the game tick reads these, and only these).
inline void CollectFlags(const Json& expr, std::vector<std::uint32_t>& out) {
    if (!expr.is_array() || expr.empty()) return;
    if (expr[0].is_string() && expr[0] == "flag" && expr.size() == 2 && expr[1].is_number_integer()) {
        out.push_back(expr[1].get<std::uint32_t>());
        return;
    }
    for (const auto& e : expr) CollectFlags(e, out);
}
inline std::vector<std::uint32_t> CollectFlags(const std::vector<Area>& areas) {
    std::vector<std::uint32_t> out;
    const auto add = [&](const std::vector<std::uint32_t>& ids) { out.insert(out.end(), ids.begin(), ids.end()); };
    for (const auto& a : areas) {
        for (const auto& l : a.lamps) {
            if (l.unlock) out.push_back(l.unlock);
            add(l.on);
            add(l.off);
        }
        for (const auto& p : a.pickups) {
            add(p.on);
            add(p.off);
            for (const auto& i : p.items) out.push_back(i.collected);
        }
        for (const auto& r : a.reveals) {
            add(r.on);
            add(r.off);
            add(r.boss);
            CollectFlags(r.when, out);
        }
        for (const auto& [entity, rule] : a.npc_rules) CollectFlags(rule, out);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ---- Rules ----

/// A rule expression: true/false, a number, ["flag", id], ["and"|"or"|"=="|"!=", a, b]. -1 unknown.
inline int Evaluate(const Json& e, const FlagFn& flag, int depth = 0) {
    if (depth > 32) return -1;
    if (e.is_boolean()) return e.get<bool>() ? 1 : 0;
    if (e.is_number_integer()) return e.get<int>();
    if (!e.is_array() || e.empty() || !e[0].is_string()) return -1;
    const std::string op = e[0].get<std::string>();
    if (op == "flag" && e.size() == 2 && e[1].is_number_integer()) return flag(e[1].get<std::uint32_t>());
    if (e.size() != 3) return -1;
    const int a = Evaluate(e[1], flag, depth + 1), b = Evaluate(e[2], flag, depth + 1);
    if (op == "and") return a == 0 || b == 0 ? 0 : (a < 0 || b < 0 ? -1 : 1);
    if (op == "or") return a > 0 || b > 0 ? 1 : (a < 0 || b < 0 ? -1 : 0);
    if (a < 0 || b < 0) return -1;
    if (op == "==") return a == b;
    if (op == "!=") return a != b;
    return -1;
}

/// All of `on` set and all of `off` clear (an unknown flag fails).
inline bool Requirements(const std::vector<std::uint32_t>& on, const std::vector<std::uint32_t>& off, const FlagFn& flag) {
    for (auto id : on)
        if (flag(id) != 1) return false;
    for (auto id : off)
        if (flag(id) != 0) return false;
    return true;
}

inline bool InTriangle(const Tri& t, float x, float z) {
    if (x < std::min({t.v[0], t.v[2], t.v[4]}) || x > std::max({t.v[0], t.v[2], t.v[4]}) ||
        z < std::min({t.v[1], t.v[3], t.v[5]}) || z > std::max({t.v[1], t.v[3], t.v[5]}))
        return false;
    const auto cross = [&](int u, int v) { return (t.v[v] - t.v[u]) * (z - t.v[u + 1]) - (t.v[v + 1] - t.v[u + 1]) * (x - t.v[u]); };
    const float ab = cross(0, 2), bc = cross(2, 4), ca = cross(4, 0);
    return (ab >= -.0001f && bc >= -.0001f && ca >= -.0001f) || (ab <= .0001f && bc <= .0001f && ca <= .0001f);
}

inline bool Inside(const Reveal& r, float x, float y, float z) {
    if (y < r.min_y || y > r.max_y) return false;
    if (r.has_bounds && (x < r.bounds[0] || z < r.bounds[1] || x > r.bounds[2] || z > r.bounds[3])) return false;
    if (r.has_centre) {
        const float dx = x - r.cx, dz = z - r.cz;
        if (dx * dx + dz * dz > r.radius * r.radius) return false;
    }
    if (!r.tris.empty()) {
        bool hit = false;
        for (const auto& t : r.tris)
            if ((hit = InTriangle(t, x, z))) break;
        if (!hit) return false;
    }
    return true;
}

inline bool RegionShown(const Reveal& r, const FlagFn& flag) {
    if (!Requirements(r.on, r.off, flag)) return false;
    if (!r.when.is_null() && Evaluate(r.when, flag) != 1) return false;
    for (auto id : r.boss)
        if (flag(id) != 1) return false;
    return true;
}

/// Per reveal region (the compiled maps' order), whether it is shown: boss reveal.
inline std::vector<bool> RegionVisibility(const Area& a, const FlagFn& flag) {
    std::vector<bool> out;
    out.reserve(a.reveals.size());
    for (const auto& r : a.reveals) out.push_back(a.always_visible || RegionShown(r, flag));
    return out;
}

/// Whether a point is shown: always without boss reveal; with it, inside its first reveal region
/// and that region revealed (outside every region: hidden).
inline bool Revealed(const Area& a, bool fog, float x, float y, float z, const FlagFn& flag) {
    if (!fog || a.always_visible) return true;
    for (const auto& r : a.reveals)
        if (Inside(r, x, y, z)) return RegionShown(r, flag);
    return false;
}

struct LampState {
    bool discovered = false;  // lit: shown on the map
    bool available = false;   // lit and usable now
};
inline LampState StateOf(const Lamp& l, const FlagFn& flag) {
    LampState s;
    s.discovered = !l.unlock || flag(l.unlock) == 1;
    s.available = s.discovered && Requirements(l.on, l.off, flag);
    // The Hunter's Dream has no lit flag: its own conditions decide.
    if (!l.unlock) s.discovered = s.available;
    return s;
}

enum class NpcMode { All, NewDialogue, Hidden };
inline bool NpcShown(const Area& a, NpcMode mode, int entity, const FlagFn& flag) {
    if (mode == NpcMode::Hidden) return false;
    if (mode == NpcMode::All) return true;
    const auto it = a.npc_rules.find(entity);
    return it != a.npc_rules.end() && Evaluate(it->second, flag) == 1;
}

/// Pickup categories, as bits.
enum : unsigned { kHealing = 1, kInsight = 2, kEquipment = 4, kEchoes = 8, kAllItems = 15 };
inline unsigned CategoryBit(const std::string& c) {
    return c == "healing" ? kHealing : c == "insight" ? kInsight : c == "equipment" ? kEquipment : c == "echoes" ? kEchoes : 0;
}

struct ShownPickup {
    float x = 0, y = 0, z = 0;
    std::string category;  // the first shown item's
    std::string label;     // "name xN" per shown item, one per line
};
/// The pickups still lying there in the chosen categories (an unknown collected flag hides it).
inline std::vector<ShownPickup> Pickups(const Area& a, unsigned categories, bool fog, const FlagFn& flag) {
    std::vector<ShownPickup> out;
    if (!categories) return out;
    for (const auto& p : a.pickups) {
        if (!Requirements(p.on, p.off, flag) || !Revealed(a, fog, p.x, p.y, p.z, flag)) continue;
        ShownPickup s{p.x, p.y, p.z, {}, {}};
        for (const auto& i : p.items) {
            if (!(CategoryBit(i.category) & categories) || flag(i.collected) != 0) continue;
            if (s.category.empty()) s.category = i.category;
            if (!s.label.empty()) s.label += '\n';
            s.label += i.name + " x" + std::to_string(i.quantity);
        }
        if (!s.category.empty()) out.push_back(std::move(s));
    }
    return out;
}

/// The title over the map: the main named place the point stands in, else the area's name.
inline std::string Title(const Area& a, float x, float y, float z) {
    for (const auto& p : a.places)
        if (p.main && y >= p.min_y - 1 && y <= p.max_y + 2)
            for (const auto& t : p.tris)
                if (InTriangle(t, x, z)) return p.name;
    return a.name;
}

/// The subtitle: another named place there, else the nearest landmark in reach ("Near ..."), else "".
inline std::string Subtitle(const Area& a, float x, float y, float z) {
    const std::string title = Title(a, x, y, z);
    for (const auto& p : a.places)
        if (p.name != title && y >= p.min_y - 1 && y <= p.max_y + 2)
            for (const auto& t : p.tris)
                if (InTriangle(t, x, z)) return p.name;
    const Landmark* nearest = nullptr;
    float best = std::numeric_limits<float>::infinity();
    for (const auto& m : a.landmarks) {
        const float h = std::hypot(m.x - x, m.z - z), v = std::abs(m.y - y);
        if (h > m.max_h || v > m.max_v) continue;
        const float d = std::hypot(h, v);
        if (d < best) {
            best = d;
            nearest = &m;
        }
    }
    return nearest ? nearest->prefix + nearest->name : std::string();
}

/// The next lamp to show (L1 / R1, Q / E): `list` holds the shown lamps in order, `current` an
/// index into it or -1; `step` +1 / -1. -1 when there is none.
inline int CycleLamp(int count, int current, int step) {
    if (count <= 0) return -1;
    if (current < 0 || current >= count) return step > 0 ? 0 : count - 1;
    return ((current + step) % count + count) % count;
}

}  // namespace BbMapData
