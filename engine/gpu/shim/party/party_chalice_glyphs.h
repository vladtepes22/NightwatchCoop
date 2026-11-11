// SPDX-License-Identifier: GPL-3.0-or-later
// The Chalice glyph pick list (docs/party/chalice.md 11): the host's Ritual Altar > "Search by Chalice
// Glyph" shows the party's known glyphs before the code keyboard. Pure rules, no game access:
// the list is built from the channel board (the host's own dungeons, the guests' dungeons sent over
// the party) and a shipped data file of curated glyphs; the text for an unknown glyph.
//
// A glyph is only a key: its layout (FormData) lives on the server the dungeon was made on. The party
// has no server, so a glyph can be entered only when its FormData is on the party's board. A curated
// entry therefore carries its FormData ("form"); one without cannot be entered and is not offered.
#pragma once

#include "net/json.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::chalice::glyphs {

constexpr std::size_t kGlyphLength = 8;
constexpr std::size_t kMaxList = 40;       ///< entries in the pick list
constexpr std::size_t kMaxCurated = 200;   ///< entries read from the data file
constexpr std::size_t kMaxFormB64 = 4096;

/// A Chalice Glyph is eight letters and digits (the board compares them without case).
inline bool ValidGlyph(const std::string& g) {
    if (g.size() != kGlyphLength) return false;
    return std::all_of(g.begin(), g.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); });
}

inline std::string LowerGlyph(std::string g) {
    for (char& c : g) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return g;
}

/// The depth (1..5) of a chalice goods id 6dff (6109 -> 1, 6219 -> 2, 6539 -> 5); 0 unknown.
constexpr int DepthOfChalice(std::uint32_t goods) {
    const std::uint32_t d = goods >= 6100 && goods < 6600 ? (goods - 6000) / 100 : 0;
    return d >= 1 && d <= 5 ? static_cast<int>(d) : 0;
}

/// The dungeon family of a chalice goods id by its last two digits (HolygrailExParam
/// holygrailTypeId: 09 Pthumerian, 19 Hintertomb, 29 Isz, 39 Loran); "" unknown.
constexpr const char* FamilyOfChalice(std::uint32_t goods) {
    if (DepthOfChalice(goods) == 0) return "";
    switch (goods % 100) {
        case 9: return "Pthumerian";
        case 19: return "Hintertomb";
        case 29: return "Isz";
        case 39: return "Loran";
        default: return "";
    }
}

/// The chalice goods id inside a FormData (base64; word 3 = helper +0xe8, reader 0x1a9c490); 0 when the
/// text is not usable base64 or too short.
inline std::uint32_t GoodsOfForm(const std::string& b64) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    std::uint8_t raw[16] = {};
    std::size_t n = 0;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : b64) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) return 0;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n < sizeof raw) raw[n++] = static_cast<std::uint8_t>(acc >> bits);
            acc &= (1u << bits) - 1;
        }
        if (n >= sizeof raw) break;
    }
    if (n < 16) return 0;
    return static_cast<std::uint32_t>(raw[12]) | static_cast<std::uint32_t>(raw[13]) << 8 | static_cast<std::uint32_t>(raw[14]) << 16 |
           static_cast<std::uint32_t>(raw[15]) << 24;
}

/// A glyph of the data file (patches/chalice_glyphs.json).
struct Curated {
    std::string glyph, name, area, notes, form;
    int depth = 0;
};

/// {"glyphs":[{"glyph","name","depth","area","notes","form"}]}. Entries with a bad glyph are skipped
/// (counted in `*skipped`); false (and `*error`) when the text is not that document.
inline bool ParseCurated(const std::string& text, std::vector<Curated>* out, std::string* error = nullptr, int* skipped = nullptr) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err) || v.type != json::Value::Type::Object) {
        if (error) *error = err.empty() ? "not an object" : err;
        return false;
    }
    const json::Value* l = v.find("glyphs");
    if (!l || l->type != json::Value::Type::Array) {
        if (error) *error = "no glyphs array";
        return false;
    }
    out->clear();
    int bad = 0;
    auto str = [](const json::Value& o, const char* key, std::size_t max) {
        const json::Value* s = o.find(key);
        return s && s->type == json::Value::Type::String && s->string.size() <= max ? s->string : std::string();
    };
    for (const json::Value& e : l->array) {
        if (out->size() >= kMaxCurated) break;
        if (e.type != json::Value::Type::Object) {
            ++bad;
            continue;
        }
        Curated c;
        c.glyph = str(e, "glyph", 16);
        if (!ValidGlyph(c.glyph)) {
            ++bad;
            continue;
        }
        c.glyph = LowerGlyph(c.glyph);
        c.name = str(e, "name", 60);
        c.area = str(e, "area", 60);
        c.notes = str(e, "notes", 120);
        c.form = str(e, "form", kMaxFormB64);
        const json::Value* d = e.find("depth");
        c.depth = d && d->type == json::Value::Type::Number && d->number >= 0 && d->number <= 5 ? static_cast<int>(d->number) : 0;
        out->push_back(std::move(c));
    }
    if (skipped) *skipped = bad;
    return true;
}

/// One dungeon the party's board holds.
struct BoardItem {
    std::string glyph;
    std::string owner;          ///< the member whose ritual or slot it is
    std::uint32_t goods = 0;    ///< chalice goods id (0 unknown)
    int depth = 0;              ///< ritual depth when the board knows it (0 unknown)
    bool has_form = false;      ///< the board holds its FormData: it can be entered
    std::uint64_t touched = 0;  ///< board clock (newest first within a group)
};

/// A row of the pick list.
struct Entry {
    std::string glyph;          ///< lower case
    std::string label;          ///< one line for the list
    std::string source;         ///< "own", "member", "curated"
    bool form_on_board = true;  ///< false: a curated entry whose FormData the board must take first
    std::string form;           ///< curated: the FormData to put on the board
};

inline std::string KindText(std::uint32_t goods, int depth) {
    const int d = depth > 0 ? depth : DepthOfChalice(goods);
    const char* fam = FamilyOfChalice(goods);
    std::string s = fam[0] ? fam : "Chalice dungeon";
    if (d > 0) s += ", depth " + std::to_string(d);
    return s;
}

/// The list: the host's own dungeons first, then the guests', then the curated ones the board does not
/// have already; at most kMaxList (the community rows come after, all of them); one row per glyph; board items without FormData are left out (they
/// cannot be entered). `me` is the host's name on the board.
inline std::vector<Entry> BuildList(const std::string& me, std::vector<BoardItem> board, const std::vector<Curated>& curated,
                                    const std::vector<Entry>& community = {}) {
    std::stable_sort(board.begin(), board.end(), [&](const BoardItem& a, const BoardItem& b) {
        const bool am = a.owner == me, bm = b.owner == me;
        if (am != bm) return am;
        return a.touched > b.touched;
    });
    std::vector<Entry> out;
    auto have = [&](const std::string& g) {
        return std::any_of(out.begin(), out.end(), [&](const Entry& e) { return e.glyph == g; });
    };
    for (const BoardItem& b : board) {
        const std::string g = LowerGlyph(b.glyph);
        if (!b.has_form || !ValidGlyph(g) || have(g) || out.size() >= kMaxList) continue;
        Entry e;
        e.glyph = g;
        e.source = b.owner == me ? "own" : "member";
        e.label = KindText(b.goods, b.depth) + " - " + (b.owner == me ? std::string("yours") : b.owner);
        out.push_back(std::move(e));
    }
    for (const Curated& c : curated) {
        if (c.form.empty() || have(c.glyph) || out.size() >= kMaxList) continue;
        Entry e;
        e.glyph = c.glyph;
        e.source = "curated";
        e.form_on_board = false;
        e.form = c.form;
        e.label = (c.name.empty() ? std::string("Public glyph") : c.name);
        if (!c.area.empty()) e.label += " - " + c.area;
        if (c.depth > 0) e.label += ", depth " + std::to_string(c.depth);
        if (!c.notes.empty()) e.label += " (" + c.notes + ")";
        out.push_back(std::move(e));
    }
    // The community list (party_chalice_glyphdb.h): already grouped, not held to kMaxList, below everything else.
    for (const Entry& e : community) {
        if (!have(e.glyph)) out.push_back(e);
    }
    return out;
}

/// The choices as the text box takes them: one "glyph<TAB>label" per line.
inline std::string EncodeChoices(const std::vector<Entry>& list) {
    std::string s;
    for (const Entry& e : list) {
        s += e.glyph + "\t" + e.label + "\n";
    }
    return s;
}

/// The notice for a glyph the party does not have (the game's own search then fails too).
inline std::string UnknownGlyphText(const std::string& glyph) {
    if (!ValidGlyph(glyph)) {
        return "A Chalice Glyph is 8 letters and digits. Pick one from the party's list.";
    }
    return "Glyph " + glyph + " is not in this party's list: it has no dungeon layout here. Pick one from the list, or have its maker's game join the party.";
}

}  // namespace coop::chalice::glyphs
