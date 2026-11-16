// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for party_chalice_glyphdb.h: the cache parser (strict validation), the save record -> FormData
// conversion and the pick list rows. Synthetic records only (no data of the community list).
#include "party/party_chalice_glyphdb.h"

#include <cstdio>
#include <cstdlib>
#include <set>

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
                            Row("cccc3333", "weird", "x", rec) + "," +              // unknown category: kept as "other"
                            Row("dddd4444", "testing", "", rec) + "," +             // empty desc
                            Row("eeee5555", "testing", "x", rec.substr(2)) + "," +  // short record
                            Row("aaaa1111", "bloodgems", "dup", rec) + "," +        // duplicate glyph
                            Row("ffff6666", "bloodgems", "Gems", Hex(Synth(0x1d000001u, 0))) + "," +  // no chalice goods
                            Row("gggg7777", "bloodgems", "Gems", rec) + "]}";
    std::vector<glyphdb::Entry> db;
    std::string err;
    int skipped = 0;
    CHECK(glyphdb::ParseCache(doc, &db, &err, &skipped));
    CHECK(db.size() == 4 && skipped == 5);
    CHECK(db.size() == 4 && db[1].glyph == "bbbb2222" && db[1].category == "equipment" && db[2].category == "other");
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
    // Tab order: Blood gems, Equipment, Farming, (Other), Testing last.
    CHECK(rows[0].glyph == "gggg2222" && rows[1].glyph == "eeee3333" && rows[2].glyph == "ffff1111" && rows[3].glyph == "tttt0000");
    CHECK(rows[0].label == "Gems" && rows[0].cat == "Blood gems" && rows[2].cat == "Farming" && rows[3].cat == "Testing");
    CHECK(!rows[0].form_on_board && rows[0].form.size() == 0x7c && rows[0].source == "glyphdb");
    // BuildList: the community rows follow the party's, a glyph the board has already is not repeated, no cap at 40.
    std::vector<glyphs::BoardItem> board(1);
    board[0].glyph = "gggg2222";
    board[0].owner = "me";
    board[0].goods = 6109;
    board[0].has_form = true;
    const auto list = glyphs::BuildList("me", board, {}, rows);
    CHECK(list.size() == 4 && list[0].glyph == "gggg2222" && list[0].source == "own");
    CHECK(glyphs::EncodeChoices({rows[0]}) == "gggg2222\tGems\tBlood gems\n");  // the tab name travels as a third field
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

static glyphdb::Entry Db(const std::string& glyph, const std::string& cat, const std::string& desc, std::uint32_t goods = 6109) {
    glyphdb::Entry e;
    e.glyph = glyph;
    e.category = cat;
    e.desc = desc;
    e.rec = Synth(0x1d123456u, goods);
    return e;
}

// Crash guard: only rows whose chalice goods id the game's loaded EquipParamGoods holds reach the list.
static void TestGoodsGuard() {
    const std::vector<glyphdb::Entry> db = {Db("aaaa0001", "farming", "ok", 6100), Db("aaaa0002", "farming", "bad", 6809),
                                            Db("aaaa0003", "equipment", "ok", 6500), Db("aaaa0004", "bloodgems", "bad", 0x12345)};
    const std::set<std::uint32_t> valid = {6100, 6109, 6500};  // injected: the repo has no game data
    std::vector<std::string> dropped;
    const auto rows = glyphdb::ToRows(db, Raw, [&](std::uint32_t g) { return valid.count(g) != 0; }, &dropped);
    CHECK(rows.size() == 2 && rows[0].glyph == "aaaa0003" && rows[1].glyph == "aaaa0001");
    CHECK(dropped.size() == 2 && dropped[0] == "aaaa0004" && dropped[1] == "aaaa0002");
    CHECK(glyphdb::ToRows(db, Raw).size() == 4);  // no predicate: nothing is checked
    auto goods_of = [](const std::string& f) { return static_cast<std::uint32_t>(static_cast<unsigned char>(f[12])) | static_cast<std::uint32_t>(static_cast<unsigned char>(f[13])) << 8; };
    CHECK(goods_of(rows[0].form) == 6500 && goods_of(rows[1].form) == 6100);
}

// Tabs: Blood gems sorted by the gem type named in the description, then the description; unknown categories go to Other.
static void TestTabsAndGems() {
    CHECK(glyphdb::GemTypeRank("Radials | L1: boss drops Triangles") == 0);
    CHECK(glyphdb::GemTypeRank("Triangles | L1: Radial") == 1 && glyphdb::GemTypeRank("Abyssal Fire (Waning)") == 2);
    CHECK(glyphdb::GemTypeRank("Circle/Out of Shape") == 3 && glyphdb::GemTypeRank("Coffin treasure") == 9);
    CHECK(glyphdb::GemTypeRank("Gem Farm | Radials") == 9);  // only the first segment names the type
    CHECK(glyphdb::NormalizeCategory("") == "other" && glyphdb::NormalizeCategory("weird") == "other" &&
          glyphdb::NormalizeCategory("farming") == "farming");
    const std::vector<glyphdb::Entry> db = {Db("tt000001", "testing", "t"), Db("oo000001", "other", "o"), Db("ff000001", "farming", "f"),
                                            Db("eq000001", "equipment", "e"), Db("gm000001", "bloodgems", "Waning b"),
                                            Db("gm000002", "bloodgems", "Radials | z"), Db("gm000003", "bloodgems", "Coffin"),
                                            Db("gm000004", "bloodgems", "Radials | a"), Db("gm000005", "bloodgems", "Triangle x")};
    const auto rows = glyphdb::ToRows(db, Raw);
    std::string order;
    for (const auto& r : rows) order += r.glyph.substr(0, 2) + (r.glyph.substr(6)) + " ";
    CHECK(order == "gm04 gm02 gm05 gm01 gm03 eq01 ff01 oo01 tt01 ");
    CHECK(rows[5].cat == "Equipment" && rows[7].cat == "Other" && rows.back().cat == "Testing");
}

// The glyph box flag: a pick after any wait still counts (the list was offered in that box); it ends with the box.
static void TestSlowPick() {
    glyphs::GlyphBox box;
    CHECK(!box.Offered() && !box.Close());           // nothing offered: the close is not ours
    box.Offer();                                     // the box opens with the list
    // 60 s, 10 min pass: there is no clock in the flag
    CHECK(box.Offered());
    CHECK(box.Close() && !box.Offered());            // the pick comes: it was the offered box, and the flag is cleared
    box.Offer();
    box.Clear();                                     // another text box opened
    CHECK(!box.Close());
}

int main() {
    TestGoodsGuard();
    TestTabsAndGems();
    TestSlowPick();
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
