// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for party_chalice_glyphs.h: the glyph pick list (own, guests', curated), the data file
// parser, chalice goods -> family / depth, and the unknown-glyph text. Synthetic data only.
#include "party/party_chalice_glyphs.h"

#include <cstdio>
#include <cstdlib>

using namespace coop::chalice::glyphs;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

static BoardItem Item(const char* glyph, const char* owner, std::uint32_t goods, std::uint64_t touched, bool form = true) {
    BoardItem b;
    b.glyph = glyph;
    b.owner = owner;
    b.goods = goods;
    b.touched = touched;
    b.has_form = form;
    return b;
}

static void TestGlyphRules() {
    CHECK(ValidGlyph("abcd1234") && ValidGlyph("ABcd12zz"));
    CHECK(!ValidGlyph("abcd123") && !ValidGlyph("abcd12345") && !ValidGlyph("abcd 234") && !ValidGlyph(""));
    CHECK(LowerGlyph("ABcd1Z34") == "abcd1z34");
    CHECK(DepthOfChalice(6109) == 1 && DepthOfChalice(6219) == 2 && DepthOfChalice(6539) == 5);
    CHECK(DepthOfChalice(0) == 0 && DepthOfChalice(7000) == 0 && DepthOfChalice(6000) == 0);
    CHECK(std::string(FamilyOfChalice(6109)) == "Pthumerian" && std::string(FamilyOfChalice(6219)) == "Hintertomb");
    CHECK(std::string(FamilyOfChalice(6429)) == "Isz" && std::string(FamilyOfChalice(6539)) == "Loran");
    CHECK(std::string(FamilyOfChalice(6210)) == "" && std::string(FamilyOfChalice(1)) == "");
}

static void TestGoodsOfForm() {
    // 16 bytes: words 0..3 = 4, 0x7c, 0, 6219 (0x184b) little endian
    const std::uint8_t raw[16] = {4, 0, 0, 0, 0x7c, 0, 0, 0, 0, 0, 0, 0, 0x4b, 0x18, 0, 0};
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    for (int i = 0; i < 16; i += 3) {
        std::uint32_t v = raw[i] << 16 | (i + 1 < 16 ? raw[i + 1] : 0) << 8 | (i + 2 < 16 ? raw[i + 2] : 0);
        b64 += tab[v >> 18 & 63];
        b64 += tab[v >> 12 & 63];
        b64 += i + 1 < 16 ? tab[v >> 6 & 63] : '=';
        b64 += i + 2 < 16 ? tab[v & 63] : '=';
    }
    CHECK(GoodsOfForm(b64) == 6219);
    CHECK(GoodsOfForm("") == 0 && GoodsOfForm("AAAA") == 0 && GoodsOfForm("!!!!!!!!!!!!!!!!!!!!!!") == 0);
}

static void TestParseCurated() {
    std::vector<Curated> c;
    std::string err;
    int skipped = -1;
    CHECK(!ParseCurated("not json", &c, &err) && !err.empty());
    CHECK(!ParseCurated("{\"x\":1}", &c, &err));
    CHECK(ParseCurated("{\"glyphs\":[]}", &c, &err, &skipped) && c.empty() && skipped == 0);
    const char* doc = "{\"_doc\":\"x\",\"glyphs\":["
                      "{\"glyph\":\"ABCD1234\",\"name\":\"Test one\",\"depth\":3,\"area\":\"Lower Hintertomb\",\"notes\":\"n\",\"form\":\"AAAA\"},"
                      "{\"glyph\":\"short\",\"name\":\"bad\"},"
                      "{\"glyph\":\"abcd5678\",\"depth\":9},"
                      "7]}";
    CHECK(ParseCurated(doc, &c, &err, &skipped));
    CHECK(c.size() == 2 && skipped == 2);
    CHECK(c[0].glyph == "abcd1234" && c[0].name == "Test one" && c[0].depth == 3 && c[0].form == "AAAA" && c[0].area == "Lower Hintertomb");
    CHECK(c[1].glyph == "abcd5678" && c[1].depth == 0 && c[1].form.empty());
}

static void TestBuildList() {
    std::vector<BoardItem> board = {
        Item("aaaaaaa1", "Guest1", 6219, 5),
        Item("bbbbbbb2", "Host", 6109, 1),
        Item("ccccccc3", "Guest2", 6309, 9),
        Item("ddddddd4", "Host", 6209, 7),
        Item("eeeeeee5", "Guest1", 6109, 3, false),  // no FormData: cannot be entered
        Item("bad", "Guest1", 6109, 3),              // not a glyph
        Item("AAAAAAA1", "Guest2", 6219, 2),         // same glyph again, other case
    };
    Curated cur;
    cur.glyph = "fffffff6";
    cur.name = "Public";
    cur.area = "Isz";
    cur.depth = 4;
    cur.notes = "boss";
    cur.form = "AAAA";
    Curated nof = cur;
    nof.glyph = "ggggggg7";
    nof.form.clear();
    Curated dup = cur;
    dup.glyph = "ccccccc3";
    const std::vector<Entry> list = BuildList("Host", board, {cur, nof, dup});
    CHECK(list.size() == 5);
    if (list.size() == 5) {
        CHECK(list[0].glyph == "ddddddd4" && list[0].source == "own");  // the host's own, newest first
        CHECK(list[1].glyph == "bbbbbbb2" && list[1].source == "own");
        CHECK(list[2].glyph == "ccccccc3" && list[2].source == "member");  // then guests', newest first
        CHECK(list[3].glyph == "aaaaaaa1" && list[3].source == "member");
        CHECK(list[4].glyph == "fffffff6" && list[4].source == "curated" && !list[4].form_on_board && list[4].form == "AAAA");
        CHECK(list[0].label == "Pthumerian, depth 2 - yours");
        CHECK(list[2].label == "Pthumerian, depth 3 - Guest2");
        CHECK(list[4].label == "Public - Isz, depth 4 (boss)");
    }
    CHECK(BuildList("Host", {}, {}).empty());
    // the list is capped
    std::vector<BoardItem> many;
    for (int i = 0; i < 100; ++i) {
        char g[16];
        std::snprintf(g, sizeof g, "g%07d", i);
        many.push_back(Item(g, "Guest1", 6109, static_cast<std::uint64_t>(i)));
    }
    CHECK(BuildList("Host", many, {}).size() == kMaxList);
}

static void TestEncodeAndText() {
    Entry e;
    e.glyph = "abcd1234";
    e.label = "Hintertomb, depth 2 - yours";
    CHECK(EncodeChoices({e, e}) == "abcd1234\tHintertomb, depth 2 - yours\nabcd1234\tHintertomb, depth 2 - yours\n");
    CHECK(EncodeChoices({}).empty());
    CHECK(UnknownGlyphText("abcd1234").find("abcd1234") != std::string::npos);
    CHECK(UnknownGlyphText("abcd1234").find("not in this party's list") != std::string::npos);
    CHECK(UnknownGlyphText("xx").find("8 letters and digits") != std::string::npos);
}

int main() {
    TestGlyphRules();
    TestGoodsOfForm();
    TestParseCurated();
    TestBuildList();
    TestEncodeAndText();
    if (g_fail) {
        std::printf("party-chalice-glyphs-test: %d failed\n", g_fail);
        return 1;
    }
    std::printf("party-chalice-glyphs-test: all passed\n");
    return 0;
}
