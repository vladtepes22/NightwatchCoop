// SPDX-License-Identifier: GPL-3.0-or-later
// start/host_only.h: the mods' host-only boss events, merged into out/party_host_only.txt as scripts/mods.py does
// (mod_host_only_events, write_party_host_only_file; its cases in tests/test_mods.py PartyHostOnlyTests).
// Build and run: ninja -C out/gpu start-host-only-test && out/gpu/start-host-only-test.exe
#include "start/host_only.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace hostonly;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
        }                                                                                                              \
    } while (0)

using Ids = std::vector<std::uint32_t>;

static bool Parse(const std::string& text, Ids* ids, std::string* error = nullptr) {
    std::string e;
    return ParseModList("mod", text, ids, error ? error : &e);
}

static Ids Good(const std::string& text) {
    Ids ids;
    CHECK(Parse(text, &ids));
    return ids;
}

static bool Bad(const std::string& text) {
    Ids ids{1};
    const bool ok = Parse(text, &ids);
    return !ok && ids.empty();
}

static Merged Merge(const std::vector<std::pair<std::string, std::string>>& layers) {
    Merged merged;
    for (const auto& [name, text] : layers) {
        Ids own;
        std::string error;
        if (ParseModList(name, text, &own, &error)) Add(&merged, name, own);
    }
    return merged;
}

static Ids Written(const std::string& text) {
    Ids ids;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t end = text.find('\n', pos);
        const std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line[0] != '#') ids.push_back(std::uint32_t(std::stoul(line)));
        pos = end + 1;
    }
    return ids;
}

int main() {
    // The PR's own cases (test_mods.py test_lists_merge_sorted_without_duplicates): comments, spaces, BOM, CRLF.
    CHECK((Good("# copies of phase events\n12345678\n  12345679  # second body\n") == Ids{12345678, 12345679}));
    CHECK((Good("\xEF\xBB\xBF" "12345678\r\n13000001\r\n") == Ids{12345678, 13000001}));
    {
        const Merged m = Merge({{"nocturne", "# copies of phase events\n12345678\n  12345679  # second body\n"},
                                {"plain-has-none", ""},
                                {"second", "\xEF\xBB\xBF" "12345678\r\n13000001\r\n"}});
        CHECK((Ids(m.ids.begin(), m.ids.end()) == Ids{12345678, 12345679, 13000001}));
        CHECK((m.names == std::vector<std::string>{"nocturne", "second"}));
        const std::string text = FileText(m);
        CHECK(text == "# Boss phase events only the party host decides (bb-start), from: nocturne, second\n"
                      "12345678\n12345679\n13000001\n");
        CHECK((Written(text) == Ids{12345678, 12345679, 13000001}));
    }
    // No list or an empty one (comments and blank lines only): no ids, no file.
    CHECK(Good("").empty());
    CHECK(Good("# none yet\n\n").empty());
    CHECK(FileText(Merge({{"empty", "# none yet\n\n"}})).empty());
    CHECK(Merge({{"empty", "# none yet\n"}}).names.empty());
    // A bad list is left out whole (test_a_bad_list_is_left_out), the others stay.
    for (const char* text : {"12345679\nx\n", "-5\n", "0\n", "4294967296\n", "12345679 12345680\n", "0x12\n",
                             "\xD9\xA1\n" /* Arabic-Indic one: a digit to Python, not ASCII */, "+5\n", "1.5\n",
                             "12345678;\n", "99999999999999999999999\n"}) {
        CHECK(Bad(text));
        const Merged m = Merge({{"good", "12345678\n"}, {"bad", text}});
        CHECK((Ids(m.ids.begin(), m.ids.end()) == Ids{12345678}));
        CHECK((m.names == std::vector<std::string>{"good"}));
    }
    CHECK(FileText(Merge({{"bad", "x\n"}})).empty());
    {
        Ids ids;
        std::string error;
        CHECK(!ParseModList("World", "1\n# c\n  12x # why\n", &ids, &error));
        CHECK(error == "World: party_host_only.txt line 3 is not an event id: 12x # why");
    }
    // The bounds: 1 and 4294967295 are ids, leading zeros too (Python int()).
    CHECK((Good("1\n4294967295\n007\n") == Ids{1, 4294967295u, 7}));
    // Lines as Python's splitlines after universal newlines: lone CR, VT, FF, FS/GS/RS, NEL, LS, PS end a line.
    CHECK((Good("1\r2\v3\f4\x1c" "5\x1d" "6\x1e" "7\xC2\x85" "8\xE2\x80\xA8" "9\xE2\x80\xA9" "10") ==
           Ids{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
    CHECK((Good("1\r\n\r\n2") == Ids{1, 2}));
    // Only spaces, tabs and CR are stripped; other blanks make the line bad (as str.strip(' \t\r')).
    CHECK(Bad("\xC2\xA0" "12\n"));
    CHECK(Bad("12\x01\n"));
    // A BOM only at the very start; bytes that are not UTF-8 refuse the list (UnicodeDecodeError).
    CHECK(Bad("1\n\xEF\xBB\xBF" "2\n"));
    CHECK(Bad("1\n\xFF\n"));
    CHECK(Bad("# \xC0\xAF overlong\n1\n"));
    CHECK(Bad("# \xED\xA0\x80 surrogate\n1\n"));
    CHECK(Bad("# \xF4\x90\x80\x80 above U+10FFFF\n1\n"));
    CHECK(Bad("# \xE2\x82 cut\n"));
    CHECK((Good("# \xE2\x82\xAC fine\n5\n") == Ids{5}));
    // test_bounded: 4096 ids are written; one more over all mods and none are (no file).
    {
        std::string full;
        for (std::uint32_t i = 0; i < kMax; ++i) full += std::to_string(20000000 + i) + "\n";
        const Merged m = Merge({{"full", full}});
        CHECK(m.ids.size() == kMax);
        CHECK(Written(FileText(m)).size() == kMax);
        CHECK(FileText(Merge({{"full", full}, {"more", "30000000\n"}})).empty());
        // the same ids twice count once
        CHECK(Written(FileText(Merge({{"full", full}, {"again", "20000000\n"}}))).size() == kMax);
    }
    std::printf("start host-only: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
