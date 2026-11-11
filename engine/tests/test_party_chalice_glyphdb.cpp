// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for party_chalice_glyphdb.h: the cache parser (strict validation), the save record -> FormData
// conversion and the pick list rows. Synthetic records only (no data of the community list).
#include "party/party_chalice_glyphdb.h"

#include <cstdio>
#include <cstdlib>

using namespace coop::chalice;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

using Rec = std::array<std::uint8_t, glyphdb::kRecordBytes>;

static void PutBe(Rec& r, std::size_t w, std::uint32_t v) {
    r[4 * w] = static_cast<std::uint8_t>(v >> 24);
    r[4 * w + 1] = static_cast<std::uint8_t>(v >> 16);
    r[4 * w + 2] = static_cast<std::uint8_t>(v >> 8);
    r[4 * w + 3] = static_cast<std::uint8_t>(v);
}

// A made-up record: map 0x1d123456, goods 6109, pairs 0xA0+i, name field 0x4e4e4e4e (never copied).
static Rec Synth(std::uint32_t map = 0x1d123456u, std::uint32_t goods = 6109) {
    Rec r{};
    PutBe(r, 0, map);
    PutBe(r, 1, 77);
    PutBe(r, 3, map);
    PutBe(r, 4, goods);
    for (std::size_t i = 0; i < 18; ++i) PutBe(r, 5 + i, 0xA0 + static_cast<std::uint32_t>(i));
    for (std::size_t i = 23; i < 31; ++i) PutBe(r, i, 0x4e4e4e4eu);
    return r;
}

static std::string Hex(const Rec& r) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::uint8_t b : r) {
        s += d[b >> 4];
        s += d[b & 15];
    }
    return s;
}

static std::string Row(const std::string& glyph, const std::string& cat, const std::string& desc, const std::string& rec) {
    return "{\"glyph\":\"" + glyph + "\",\"cat\":\"" + cat + "\",\"desc\":\"" + desc + "\",\"rec\":\"" + rec + "\"}";
}

static std::uint32_t Le(const std::vector<std::uint8_t>& v, std::size_t w) {
    return static_cast<std::uint32_t>(v[4 * w]) | static_cast<std::uint32_t>(v[4 * w + 1]) << 8 |
           static_cast<std::uint32_t>(v[4 * w + 2]) << 16 | static_cast<std::uint32_t>(v[4 * w + 3]) << 24;
}

static std::string Raw(const std::uint8_t* d, std::size_t n) {
    return std::string(reinterpret_cast<const char*>(d), n);
}

static void TestConversion() {
    const Rec r = Synth();
    CHECK(glyphdb::RecordMap(r) == 0x1d123456u && glyphdb::RecordGoods(r) == 6109 && glyphdb::RecordUsable(r));
    const std::vector<std::uint8_t> f = glyphdb::RecordToFormData(r);
    CHECK(f.size() == 0x7c);
    CHECK(Le(f, 0) == 4 && Le(f, 1) == 0x7c);            // version, size
    CHECK(Le(f, 2) == 0x1d123456u && Le(f, 3) == 6109);  // helper +0xe0, +0xe8
    for (std::size_t i = 0; i < 18; ++i) CHECK(Le(f, 4 + i) == 0xA0 + i);
    for (std::size_t i = 22; i < 31; ++i) CHECK(Le(f, i) == 0);  // the name field and the rest stay zero
    CHECK(!glyphdb::RecordUsable(Synth(0x1c123456u)) && !glyphdb::RecordUsable(Synth(0x1d123456u, 0)));
}

static void TestValidation() {
    CHECK(glyphdb::ValidDbGlyph("abcd1234") && glyphdb::ValidDbGlyph("abcd12345"));
    CHECK(!glyphdb::ValidDbGlyph("abcd123") && !glyphdb::ValidDbGlyph("abcd123456") && !glyphdb::ValidDbGlyph("abcd 234") &&
          !glyphdb::ValidDbGlyph("ABCD1234"));
    CHECK(glyphdb::ValidDesc("Blood Echoes farm") && !glyphdb::ValidDesc("") && !glyphdb::ValidDesc(std::string(201, 'x')) &&
          !glyphdb::ValidDesc("tab\there") && glyphdb::ValidDesc(std::string(200, 'x')));
    Rec r{};
    CHECK(!glyphdb::HexToRecord("00", &r) && !glyphdb::HexToRecord(std::string(248, 'g'), &r));
    CHECK(glyphdb::HexToRecord(Hex(Synth()), &r) && r == Synth());
}

static void TestCache() {
    const std::string rec = Hex(Synth());
    const std::string doc = "{\"version\":1,\"glyphs\":[" + Row("aaaa1111", "farming", "Echoes", rec) + "," +
                            Row("BBBB2222", "equipment", "Gear", rec) + "," +      // upper case: folded
                            Row("short", "farming", "x", rec) + "," +               // bad glyph
                            Row("cccc3333", "weird", "x", rec) + "," +              // bad category
                            Row("dddd4444", "testing", "", rec) + "," +             // empty desc
                            Row("eeee5555", "testing", "x", rec.substr(2)) + "," +  // short record
                            Row("aaaa1111", "bloodgems", "dup", rec) + "," +        // duplicate glyph
                            Row("ffff6666", "bloodgems", "Gems", Hex(Synth(0x1d000001u, 0))) + "," +  // no chalice goods
                            Row("gggg7777", "bloodgems", "Gems", rec) + "]}";
    std::vector<glyphdb::Entry> db;
    std::string err;
    int skipped = 0;
    CHECK(glyphdb::ParseCache(doc, &db, &err, &skipped));
    CHECK(db.size() == 3 && skipped == 6);
    CHECK(db.size() == 3 && db[1].glyph == "bbbb2222" && db[1].category == "equipment");
    CHECK(!glyphdb::ParseCache("[]", &db, &err) && !glyphdb::ParseCache("{\"version\":2,\"glyphs\":[]}", &db, &err) &&
          !glyphdb::ParseCache("{\"version\":1}", &db, &err) && !glyphdb::ParseCache("garbage", &db, &err));
    CHECK(glyphdb::ParseCache("{\"version\":1,\"glyphs\":[]}", &db, &err) && db.empty());
}

static void TestRows() {
    const std::string rec = Hex(Synth());
    const std::string doc = "{\"version\":1,\"glyphs\":[" + Row("tttt0000", "testing", "Last", rec) + "," +
                            Row("ggg111111", "bloodgems", "Nine chars", rec) + "," +
                            Row("ffff1111", "farming", "First", rec) + "," +
                            Row("gggg2222", "bloodgems", "Gems", rec) + "," +
                            Row("eeee3333", "equipment", "Gear", rec) + "]}";
    std::vector<glyphdb::Entry> db;
    CHECK(glyphdb::ParseCache(doc, &db));
    const auto rows = glyphdb::ToRows(db, Raw);
    CHECK(rows.size() == 4);  // the 9-character glyph cannot be typed into the altar's box
    CHECK(rows[0].glyph == "ffff1111" && rows[1].glyph == "eeee3333" && rows[2].glyph == "gggg2222" && rows[3].glyph == "tttt0000");
    CHECK(rows[0].label == "Farming: First" && rows[2].label == "Blood gems: Gems");
    CHECK(!rows[0].form_on_board && rows[0].form.size() == 0x7c && rows[0].source == "glyphdb");
    // BuildList: the community rows follow the party's, a glyph the board has already is not repeated, no cap at 40.
    std::vector<glyphs::BoardItem> board(1);
    board[0].glyph = "gggg2222";
    board[0].owner = "me";
    board[0].goods = 6109;
    board[0].has_form = true;
    const auto list = glyphs::BuildList("me", board, {}, rows);
    CHECK(list.size() == 4 && list[0].glyph == "gggg2222" && list[0].source == "own");
    std::vector<glyphs::Entry> many;
    for (int i = 0; i < 100; ++i) {
        glyphs::Entry e;
        e.glyph = "g" + std::to_string(10000000 + i);
        many.push_back(e);
    }
    CHECK(glyphs::BuildList("me", {}, {}, many).size() == 100);
    // A long description is cut at a character boundary.
    glyphdb::Entry u;
    u.glyph = "uuuu1111";
    u.category = "farming";
    u.desc = std::string(66, 'a') + "\xC3\xA9\xC3\xA9\xC3\xA9";
    u.rec = Synth();
    const auto cut = glyphdb::ToRows({u}, Raw);
    CHECK(cut.size() == 1 && cut[0].label.size() <= 80 && cut[0].label.back() == '.');
}

int main() {
    TestConversion();
    TestValidation();
    TestCache();
    TestRows();
    if (g_fail) {
        std::printf("party-chalice-glyphdb-test: %d failed\n", g_fail);
        return 1;
    }
    std::printf("party-chalice-glyphdb-test: all passed\n");
    return 0;
}
