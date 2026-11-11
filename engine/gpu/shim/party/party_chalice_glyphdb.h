// SPDX-License-Identifier: GPL-3.0-or-later
// Community Chalice glyphs (bbport.ini chalice_glyphdb): Noxde's list "Add dungeons to Bloodborne save"
// (https://gist.github.com/Noxde/a29f699f4175bf315d9bd4baeebefb66, Nexus mod 121). The list is fetched at run
// time by the launcher into the player's own data folder and is never part of the repository or a release:
// the gist carries no licence. Pure rules, no game access: the cache parser (strict, bad entries dropped),
// and the conversion of the gist's 124-byte save record to the FormData the game's glyph search uses.
//
// The record is the serialized altar slot of the save (the in-game slot record, docs/party/chalice.md 3),
// big-endian dwords: w0 copy of w3, w1 / w2 ChannelId low / high, w3 the map (event-flag +96, helper +0xe0),
// w4 chalice goods id (+128, helper +0xe8), w5..w22 the nine layout pairs (+160, helper +0xf8), w23..w26 the
// creator's name field (+736, helper +0x140), then the rest of the name. FormData version 4 (0x7c bytes,
// reader 0x1a9c490) holds the same fields as little-endian dwords: {4, 0x7c, w3, w4, w5..w22, 4 name dwords}.
// The creator's name is never copied: the FormData's name field stays zero.
#pragma once

#include "party_chalice_glyphs.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::chalice::glyphdb {

constexpr std::size_t kRecordBytes = 124;      ///< one altar slot record (0x7c)
constexpr std::size_t kFormBytes = 0x7c;       ///< FormData version 4
constexpr std::size_t kMaxEntries = 400;
constexpr std::size_t kMaxDesc = 200;

struct Entry {
    std::string glyph;                          ///< lower case, 8 or 9 letters / digits
    std::string category;                       ///< farming / equipment / bloodgems / testing
    std::string desc;
    std::array<std::uint8_t, kRecordBytes> rec{};
};

/// A glyph of the database: [a-z0-9]{8,9} (case folded by the caller).
inline bool ValidDbGlyph(const std::string& g) {
    if (g.size() < 8 || g.size() > 9) return false;
    for (char c : g) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'))) return false;
    }
    return true;
}

/// A description: 1..200 bytes, no control characters (UTF-8 text above 0x7f passes).
inline bool ValidDesc(const std::string& d) {
    if (d.empty() || d.size() > kMaxDesc) return false;
    for (unsigned char c : d) {
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

inline bool ValidCategory(const std::string& c) {
    return c == "farming" || c == "equipment" || c == "bloodgems" || c == "testing";
}

/// 248 hex digits -> the record; false on any other text.
inline bool HexToRecord(const std::string& hex, std::array<std::uint8_t, kRecordBytes>* out) {
    if (hex.size() != kRecordBytes * 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < kRecordBytes; ++i) {
        const int a = nib(hex[2 * i]), b = nib(hex[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        (*out)[i] = static_cast<std::uint8_t>(a << 4 | b);
    }
    return true;
}

inline std::uint32_t BeWord(const std::array<std::uint8_t, kRecordBytes>& r, std::size_t w) {
    const std::size_t o = w * 4;
    return static_cast<std::uint32_t>(r[o]) << 24 | static_cast<std::uint32_t>(r[o + 1]) << 16 |
           static_cast<std::uint32_t>(r[o + 2]) << 8 | r[o + 3];
}

/// The record's map word (area 29 = 0x1d......) and chalice goods id.
inline std::uint32_t RecordMap(const std::array<std::uint8_t, kRecordBytes>& r) { return BeWord(r, 3); }
inline std::uint32_t RecordGoods(const std::array<std::uint8_t, kRecordBytes>& r) { return BeWord(r, 4); }

/// A record the game can generate a dungeon from: a map of area 29 and a chalice goods id.
inline bool RecordUsable(const std::array<std::uint8_t, kRecordBytes>& r) {
    return (RecordMap(r) >> 24) == 0x1d && RecordGoods(r) != 0;
}

/// The record -> FormData version 4 (0x7c bytes, little-endian dwords; the name field zero).
inline std::vector<std::uint8_t> RecordToFormData(const std::array<std::uint8_t, kRecordBytes>& r) {
    std::uint32_t w[kFormBytes / 4] = {};
    w[0] = 4;
    w[1] = static_cast<std::uint32_t>(kFormBytes);
    w[2] = BeWord(r, 3);
    w[3] = BeWord(r, 4);
    for (std::size_t i = 0; i < 18; ++i) w[4 + i] = BeWord(r, 5 + i);
    std::vector<std::uint8_t> out(kFormBytes);
    for (std::size_t i = 0; i < kFormBytes / 4; ++i) {
        out[4 * i] = static_cast<std::uint8_t>(w[i]);
        out[4 * i + 1] = static_cast<std::uint8_t>(w[i] >> 8);
        out[4 * i + 2] = static_cast<std::uint8_t>(w[i] >> 16);
        out[4 * i + 3] = static_cast<std::uint8_t>(w[i] >> 24);
    }
    return out;
}

/// The cache file the launcher writes: {"version":1,"glyphs":[{"glyph","cat","desc","rec"}]}. Entries that fail
/// the checks are dropped (counted in *skipped); false when the text is not that document.
inline bool ParseCache(const std::string& text, std::vector<Entry>* out, std::string* error = nullptr, int* skipped = nullptr) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err) || v.type != json::Value::Type::Object) {
        if (error) *error = err.empty() ? "not an object" : err;
        return false;
    }
    const json::Value* ver = v.find("version");
    const json::Value* l = v.find("glyphs");
    if (!ver || ver->type != json::Value::Type::Number || ver->number != 1 || !l || l->type != json::Value::Type::Array) {
        if (error) *error = "not a version 1 glyph cache";
        return false;
    }
    out->clear();
    int bad = 0;
    auto str = [](const json::Value& o, const char* key) {
        const json::Value* s = o.find(key);
        return s && s->type == json::Value::Type::String ? s->string : std::string();
    };
    for (const json::Value& e : l->array) {
        if (out->size() >= kMaxEntries) break;
        Entry d;
        d.glyph = glyphs::LowerGlyph(str(e, "glyph"));
        d.category = str(e, "cat");
        d.desc = str(e, "desc");
        if (e.type != json::Value::Type::Object || !ValidDbGlyph(d.glyph) || !ValidCategory(d.category) || !ValidDesc(d.desc) ||
            !HexToRecord(str(e, "rec"), &d.rec) || !RecordUsable(d.rec)) {
            ++bad;
            continue;
        }
        bool dup = false;
        for (const Entry& o : *out) dup = dup || o.glyph == d.glyph;
        if (dup) {
            ++bad;
            continue;
        }
        out->push_back(std::move(d));
    }
    if (skipped) *skipped = bad;
    return true;
}

inline const char* CategoryTitle(const std::string& c) {
    if (c == "farming") return "Farming";
    if (c == "equipment") return "Equipment";
    if (c == "bloodgems") return "Blood gems";
    return "Testing";
}

inline int CategoryRank(const std::string& c) {
    return c == "farming" ? 0 : c == "equipment" ? 1 : c == "bloodgems" ? 2 : 3;
}

/// The pick list rows of the database: grouped by category (farming, equipment, blood gems, testing), in
/// file order inside a group; only 8-character glyphs (the altar's glyph box takes 8); `form` = base64 FormData.
/// `b64(const uint8_t*, size_t)` encodes bytes (party_util.h b64_encode).
template <class B64>
inline std::vector<glyphs::Entry> ToRows(const std::vector<Entry>& db, B64 b64) {
    std::vector<glyphs::Entry> rows;
    for (int rank = 0; rank < 4; ++rank) {
        for (const Entry& d : db) {
            if (CategoryRank(d.category) != rank || d.glyph.size() != glyphs::kGlyphLength) continue;
            glyphs::Entry e;
            e.glyph = d.glyph;
            e.source = "glyphdb";
            e.form_on_board = false;
            const std::vector<std::uint8_t> fd = RecordToFormData(d.rec);
            e.form = b64(fd.data(), fd.size());
            std::size_t cut = d.desc.size() > 70 ? 67 : d.desc.size();
            while (cut > 0 && cut < d.desc.size() && (static_cast<unsigned char>(d.desc[cut]) & 0xC0) == 0x80) --cut;
            const std::string desc = cut < d.desc.size() ? d.desc.substr(0, cut) + "..." : d.desc;
            e.label = std::string(CategoryTitle(d.category)) + ": " + desc;
            rows.push_back(std::move(e));
        }
    }
    return rows;
}

}  // namespace coop::chalice::glyphdb
