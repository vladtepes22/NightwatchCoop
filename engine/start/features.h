// SPDX-License-Identifier: GPL-3.0-or-later
// Nightwatch 0.3.0 mod platform (work plan F1, F2): a mod's manifest `nightwatch.json` beside its dvdroot_ps4, and its
// feature tables `nightwatch/<feature>.json`, merged by bb-start into one file per feature in out/ that the engine reads
// at start. Public docs: docs/modding/manifest.md, docs/modding/hit_effects.md.
//
// Rules (principles of work/0.3.0/ITEMS.md):
// - A mod without nightwatch.json loads as before and uses no feature table.
// - A manifest that is not valid, needs a newer Nightwatch (`requires`, compared as major.minor.patch; a pre-release
//   suffix like -beta.1 is ignored on both sides) or names a feature this build does not have: the mod is not loaded,
//   with one log line saying why.
// - A table is used only when the manifest lists its feature. A bad table (not JSON, wrong version, a bad row) drops
//   that mod's whole table with one log line; the mod itself still loads. Unknown fields are ignored (a table format
//   only grows by new optional fields).
// - Tables merge in load order; a later mod wins per key.
#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nwfeatures {

constexpr const char* kManifest = "nightwatch.json";
constexpr const char* kTablesDir = "nightwatch";
constexpr const char* kHitEffects = "hit_effects";
constexpr const char* kHitEffectsOut = "nightwatch_hit_effects.txt";  // in out/, read by gpu/shim/party/hit_effects.cpp
constexpr std::size_t kMaxFileBytes = 1u << 20;
constexpr std::size_t kMaxHitRows = 4096;  // after the merge; hit_effects_rules.h kMaxRows
constexpr const char* kWeaponClipGroups = "weapon_clip_groups";
constexpr const char* kWeaponClipGroupsOut = "nightwatch_weapon_clip_groups.txt";  // gpu/shim/party/weapon_clip_groups.cpp
constexpr std::size_t kMaxClipRows = 4096;  // after the merge; weapon_clip_groups_rules.h kMaxRows

/// The features this build offers (a manifest naming any other is refused).
inline const std::vector<std::string>& Known() {
    static const std::vector<std::string> known = {kHitEffects, kWeaponClipGroups};
    return known;
}

struct Version {
    int major = 0, minor = 0, patch = 0;
};

/// "X.Y.Z" with an optional "-suffix" (ignored); also "X.Y" (patch 0). False for anything else.
inline bool ParseVersion(std::string_view text, Version* v) {
    const std::size_t dash = text.find_first_of("-+");
    if (dash != std::string_view::npos) text = text.substr(0, dash);
    int parts[3] = {0, 0, 0};
    int count = 0;
    std::size_t i = 0;
    while (true) {
        if (count == 3 || i >= text.size() || text[i] < '0' || text[i] > '9') return false;
        long long n = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            n = n * 10 + (text[i] - '0');
            if (n > 99999) return false;
            ++i;
        }
        parts[count++] = int(n);
        if (i == text.size()) break;
        if (text[i] != '.') return false;
        ++i;
    }
    if (count < 2) return false;
    *v = {parts[0], parts[1], parts[2]};
    return true;
}

inline int Compare(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

struct Manifest {
    std::string id, name, version, author, min_version;  // min_version = "requires"
    std::vector<std::string> features;
    bool Uses(std::string_view feature) const {
        return std::find(features.begin(), features.end(), feature) != features.end();
    }
};

inline std::string_view StripBom(std::string_view bytes) {
    if (bytes.substr(0, 3) == "\xEF\xBB\xBF") bytes.remove_prefix(3);
    return bytes;
}

/// nightwatch.json: an object with string `id` (required), `requires` (required, a version), optional strings `name`,
/// `version`, `author` and an optional array of strings `features`. Other keys are ignored. False with `error`.
inline bool ParseManifest(std::string_view bytes, Manifest* m, std::string* error) {
    *m = {};
    if (bytes.size() > kMaxFileBytes) {
        *error = std::string(kManifest) + " is larger than 1 MiB";
        return false;
    }
    const nlohmann::json j = nlohmann::json::parse(StripBom(bytes), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        *error = std::string(kManifest) + " is not a JSON object";
        return false;
    }
    const auto text = [&](const char* key, std::string* out, bool required) {
        const auto it = j.find(key);
        if (it == j.end()) {
            if (required) *error = std::string(kManifest) + " has no \"" + key + "\"";
            return !required;
        }
        if (!it->is_string() || (required && it->get<std::string>().empty())) {
            *error = std::string(kManifest) + ": \"" + key + "\" is not a" + (required ? " non-empty" : "") + " string";
            return false;
        }
        *out = it->get<std::string>();
        return true;
    };
    if (!text("id", &m->id, true) || !text("requires", &m->min_version, true) || !text("name", &m->name, false) ||
        !text("version", &m->version, false) || !text("author", &m->author, false)) {
        return false;
    }
    Version v;
    if (!ParseVersion(m->min_version, &v)) {
        *error = std::string(kManifest) + ": \"requires\" is not a version like 0.3.0: " + m->min_version;
        return false;
    }
    if (const auto it = j.find("features"); it != j.end()) {
        if (!it->is_array()) {
            *error = std::string(kManifest) + ": \"features\" is not a list";
            return false;
        }
        for (const auto& f : *it) {
            if (!f.is_string() || f.get<std::string>().empty()) {
                *error = std::string(kManifest) + ": \"features\" holds something other than a feature name";
                return false;
            }
            m->features.push_back(f.get<std::string>());
        }
    }
    return true;
}

/// Why this build (`nightwatch`, its VERSION text) cannot load the mod, or "" when it can.
inline std::string Refusal(const Manifest& m, std::string_view nightwatch) {
    Version need, have;
    if (!ParseVersion(m.min_version, &need)) return "\"requires\" is not a version: " + m.min_version;
    if (!ParseVersion(nightwatch, &have)) return "this Nightwatch has no version (" + std::string(nightwatch) + ")";
    if (Compare(have, need) < 0) {
        return "needs Nightwatch " + m.min_version + " or newer (this is " + std::string(nightwatch) + ")";
    }
    for (const std::string& f : m.features) {
        if (std::find(Known().begin(), Known().end(), f) == Known().end()) {
            return "uses the feature \"" + f + "\", which Nightwatch " + std::string(nightwatch) + " does not have";
        }
    }
    return "";
}

// ---- hit_effects (docs/modding/hit_effects.md) ----

struct HitRow {
    std::uint32_t attacker_speffect = 0, base_fx = 0, fx = 0;
    std::uint32_t sp_attribute = 0, sound = 0;  // a draw row when sp_attribute != 0
    bool operator==(const HitRow& o) const {
        return attacker_speffect == o.attacker_speffect && base_fx == o.base_fx && fx == o.fx &&
               sp_attribute == o.sp_attribute && sound == o.sound;
    }
    /// Rows with the same key replace each other in the merge.
    bool SameKey(const HitRow& o) const {
        return sp_attribute ? sp_attribute == o.sp_attribute
                            : !o.sp_attribute && attacker_speffect == o.attacker_speffect && base_fx == o.base_fx;
    }
};

/// nightwatch/hit_effects.json: {"version": 1, "rows": [...]}; a row is either a swap {"attacker_speffect": N,
/// "base_fx": N, "fx": N} or a draw {"sp_attribute": A, "fx": N, "sound": N} (A 1..255 but not 2 / 6, which the game
/// draws itself), each N a whole number 1..2147483647. False with `error` naming the first bad row (rows count from 1).
inline bool ParseHitEffects(std::string_view bytes, std::vector<HitRow>* rows, std::string* error) {
    rows->clear();
    const std::string file = std::string(kTablesDir) + "/" + kHitEffects + ".json";
    if (bytes.size() > kMaxFileBytes) {
        *error = file + " is larger than 1 MiB";
        return false;
    }
    const nlohmann::json j = nlohmann::json::parse(StripBom(bytes), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        *error = file + " is not a JSON object";
        return false;
    }
    const auto ver = j.find("version");
    if (ver == j.end() || !ver->is_number_integer() || ver->get<long long>() != 1) {
        *error = file + ": \"version\" is not 1";
        return false;
    }
    const auto list = j.find("rows");
    if (list == j.end() || !list->is_array()) {
        *error = file + ": \"rows\" is not a list";
        return false;
    }
    if (list->size() > kMaxHitRows) {
        *error = file + ": more than " + std::to_string(kMaxHitRows) + " rows";
        return false;
    }
    std::size_t number = 0;
    for (const auto& r : *list) {
        ++number;
        HitRow row;
        const auto field = [&](const char* key, std::uint32_t* out) {
            const auto it = r.is_object() ? r.find(key) : r.end();
            if (it == r.end() || !it->is_number_integer()) return false;
            const long long v = it->get<long long>();
            if (v < 1 || v > 0x7FFFFFFFLL) return false;
            *out = std::uint32_t(v);
            return true;
        };
        if (r.is_object() && r.contains("sp_attribute")) {
            if (!field("sp_attribute", &row.sp_attribute) || row.sp_attribute > 255 || row.sp_attribute == 2 ||
                row.sp_attribute == 6 || !field("fx", &row.fx) || !field("sound", &row.sound) ||
                r.contains("attacker_speffect") || r.contains("base_fx")) {
                *error = file + ": row " + std::to_string(number) +
                         " (a draw row) needs sp_attribute 1..255 but not 2 or 6, and whole numbers 1..2147483647 for "
                         "fx and sound (no attacker_speffect or base_fx)";
                rows->clear();
                return false;
            }
        } else if (!field("attacker_speffect", &row.attacker_speffect) || !field("base_fx", &row.base_fx) ||
                   !field("fx", &row.fx)) {
            *error = file + ": row " + std::to_string(number) +
                     " needs whole numbers 1..2147483647 for attacker_speffect, base_fx and fx";
            rows->clear();
            return false;
        }
        rows->push_back(row);
    }
    return true;
}

/// The merge in load order: a later mod's row replaces an earlier one with the same (attacker_speffect, base_fx).
/// `rows` keeps the newest mod first, so when one hit matches rows of several SpEffects the latest mod wins too.
struct HitMerged {
    std::vector<std::string> names;  // the mods that gave rows, in load order
    std::vector<std::pair<HitRow, std::size_t>> rows;  // row, index of its mod in `names`
};

inline void AddHitEffects(HitMerged* merged, std::string_view name, const std::vector<HitRow>& own) {
    if (own.empty()) return;
    const std::size_t index = merged->names.size();
    merged->names.emplace_back(name);
    for (const HitRow& row : own) {
        auto& rows = merged->rows;
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const auto& r) { return r.first.SameKey(row); }),
                   rows.end());
        rows.emplace_back(row, index);
    }
}

/// out/nightwatch_hit_effects.txt (hit_effects_rules.h ParseTable): a comment line, then "<attacker_speffect>
/// <base_fx> <fx>" per row, the newest mod first. "" when there are no rows (no file).
inline std::string HitEffectsFileText(const HitMerged& merged) {
    if (merged.rows.empty()) return "";
    std::vector<std::pair<HitRow, std::size_t>> rows = merged.rows;
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (rows.size() > kMaxHitRows) rows.resize(kMaxHitRows);
    std::string text =
        "# hit_effects (bb-start; attacker_speffect base_fx fx | attr sp_attribute fx sound, newest mod first), from: ";
    for (std::size_t i = 0; i < merged.names.size(); ++i) text += (i ? ", " : "") + merged.names[i];
    text += "\n";
    for (const auto& [row, index] : rows) {
        (void)index;
        if (row.sp_attribute) {
            text += "attr " + std::to_string(row.sp_attribute) + " " + std::to_string(row.fx) + " " +
                    std::to_string(row.sound) + "\n";
        } else {
            text += std::to_string(row.attacker_speffect) + " " + std::to_string(row.base_fx) + " " +
                    std::to_string(row.fx) + "\n";
        }
    }
    return text;
}

// ---- weapon_clip_groups (docs/modding/weapon_clip_groups.md) ----

struct ClipRow {
    std::uint32_t weapon = 0, clip_group = 0;  // weapon: a multiple of 10000 (covers its +0..+10 rows)
    bool operator==(const ClipRow& o) const { return weapon == o.weapon && clip_group == o.clip_group; }
};

/// nightwatch/weapon_clip_groups.json: {"version": 1, "rows": [{"weapon": N, "clip_group": G}, ...]}: weapon a whole
/// number 10000..2147480000 and a multiple of 10000, clip_group 1..999 (the aGGG of the clips). False with `error`
/// naming the first bad row (rows count from 1); a weapon listed twice in one table is a bad row too.
inline bool ParseWeaponClipGroups(std::string_view bytes, std::vector<ClipRow>* rows, std::string* error) {
    rows->clear();
    const std::string file = std::string(kTablesDir) + "/" + kWeaponClipGroups + ".json";
    if (bytes.size() > kMaxFileBytes) {
        *error = file + " is larger than 1 MiB";
        return false;
    }
    const nlohmann::json j = nlohmann::json::parse(StripBom(bytes), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        *error = file + " is not a JSON object";
        return false;
    }
    const auto ver = j.find("version");
    if (ver == j.end() || !ver->is_number_integer() || ver->get<long long>() != 1) {
        *error = file + ": \"version\" is not 1";
        return false;
    }
    const auto list = j.find("rows");
    if (list == j.end() || !list->is_array()) {
        *error = file + ": \"rows\" is not a list";
        return false;
    }
    if (list->size() > kMaxClipRows) {
        *error = file + ": more than " + std::to_string(kMaxClipRows) + " rows";
        return false;
    }
    std::size_t number = 0;
    for (const auto& r : *list) {
        ++number;
        const auto field = [&](const char* key, long long lo, long long hi, long long* out) {
            const auto it = r.is_object() ? r.find(key) : r.end();
            if (it == r.end() || !it->is_number_integer()) return false;
            *out = it->get<long long>();
            return *out >= lo && *out <= hi;
        };
        long long weapon = 0, group = 0;
        const bool ok = field("weapon", 10000, 2147480000LL, &weapon) && weapon % 10000 == 0 &&
                        field("clip_group", 1, 999, &group);
        const bool twice = ok && std::any_of(rows->begin(), rows->end(),
                                             [&](const ClipRow& c) { return c.weapon == std::uint32_t(weapon); });
        if (!ok || twice) {
            *error = file + ": row " + std::to_string(number) +
                     (twice ? " lists a weapon a second time"
                            : " needs \"weapon\" (a multiple of 10000) and \"clip_group\" (1..999)");
            rows->clear();
            return false;
        }
        rows->push_back({std::uint32_t(weapon), std::uint32_t(group)});
    }
    return true;
}

/// The merge in load order: a later mod's row replaces an earlier one for the same weapon.
struct ClipMerged {
    std::vector<std::string> names;  // the mods that gave rows, in load order
    std::vector<ClipRow> rows;       // sorted by weapon
};

inline void AddWeaponClipGroups(ClipMerged* merged, std::string_view name, const std::vector<ClipRow>& own) {
    if (own.empty()) return;
    merged->names.emplace_back(name);
    for (const ClipRow& row : own) {
        auto& rows = merged->rows;
        const auto it = std::lower_bound(rows.begin(), rows.end(), row.weapon,
                                         [](const ClipRow& r, std::uint32_t w) { return r.weapon < w; });
        if (it != rows.end() && it->weapon == row.weapon) *it = row;
        else rows.insert(it, row);
    }
    if (merged->rows.size() > kMaxClipRows) merged->rows.resize(kMaxClipRows);
}

/// out/nightwatch_weapon_clip_groups.txt (weapon_clip_groups_rules.h ParseTable): a comment line, then "<weapon>
/// <clip_group>" per row, by weapon. "" when there are no rows (no file). Gameplay: bb-start hashes this text into
/// the party version check.
inline std::string WeaponClipGroupsFileText(const ClipMerged& merged) {
    if (merged.rows.empty()) return "";
    std::string text = "# weapon_clip_groups (bb-start; weapon clip_group), from: ";
    for (std::size_t i = 0; i < merged.names.size(); ++i) text += (i ? ", " : "") + merged.names[i];
    text += "\n";
    for (const ClipRow& row : merged.rows)
        text += std::to_string(row.weapon) + " " + std::to_string(row.clip_group) + "\n";
    return text;
}

}  // namespace nwfeatures
