// SPDX-License-Identifier: GPL-3.0-or-later
// Nightwatch 0.3.0 feature weapon_clip_groups: start/features.h (the json, merge, out file) and
// gpu/shim/party/weapon_clip_groups_rules.h (the engine's reading of that file and its lookup).
// Build and run: ninja -C out/gpu weapon-clip-groups-test && out/gpu/weapon-clip-groups-test.exe
#include "start/features.h"
#include "gpu/shim/party/weapon_clip_groups_rules.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace nwfeatures;
namespace cg = coop::clipgroups;

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

static const char* kDancer =
    R"({"version":1,"rows":[{"weapon":27330000,"clip_group":161},{"weapon":27340000,"clip_group":161},{"weapon":27350000,"clip_group":161}]})";

static void ManifestUse() {
    Manifest m;
    std::string error;
    CHECK(ParseManifest(R"({"id":"dancer","requires":"0.3.0","features":["weapon_clip_groups","hit_effects"]})", &m, &error));
    CHECK(m.Uses(kWeaponClipGroups));
    CHECK(Refusal(m, "0.3.0-alpha.2").empty());
    CHECK(!Refusal(m, "0.2.9").empty());
}

static void Json() {
    std::vector<ClipRow> rows;
    std::string error;
    CHECK(ParseWeaponClipGroups(kDancer, &rows, &error) && rows.size() == 3 && rows[0].weapon == 27330000 &&
          rows[0].clip_group == 161);
    CHECK(ParseWeaponClipGroups(R"({"version":1,"rows":[]})", &rows, &error) && rows.empty());
    CHECK(!ParseWeaponClipGroups(R"({"version":2,"rows":[]})", &rows, &error));
    CHECK(!ParseWeaponClipGroups(R"({"version":1})", &rows, &error));
    CHECK(!ParseWeaponClipGroups("[1]", &rows, &error));
    CHECK(!ParseWeaponClipGroups(R"({"version":1,"rows":[{"weapon":27330100,"clip_group":161}]})", &rows, &error) &&
          error.find("row 1") != std::string::npos && rows.empty());
    CHECK(!ParseWeaponClipGroups(R"({"version":1,"rows":[{"weapon":27330000,"clip_group":0}]})", &rows, &error));
    CHECK(!ParseWeaponClipGroups(R"({"version":1,"rows":[{"weapon":27330000,"clip_group":1000}]})", &rows, &error));
    CHECK(!ParseWeaponClipGroups(R"({"version":1,"rows":[{"weapon":0,"clip_group":5}]})", &rows, &error));
    CHECK(!ParseWeaponClipGroups(R"({"version":1,"rows":[{"weapon":"27330000","clip_group":5}]})", &rows, &error));
    CHECK(!ParseWeaponClipGroups(
              R"({"version":1,"rows":[{"weapon":10000,"clip_group":5},{"weapon":10000,"clip_group":6}]})", &rows, &error) &&
          error.find("second time") != std::string::npos);
    CHECK(ParseWeaponClipGroups(R"({"version":1,"rows":[{"weapon":10000,"clip_group":5,"note":"x"}]})", &rows, &error));
}

static void MergeAndEngine() {
    ClipMerged merged;
    CHECK(WeaponClipGroupsFileText(merged).empty());
    std::vector<ClipRow> a, b;
    std::string error;
    CHECK(ParseWeaponClipGroups(kDancer, &a, &error));
    CHECK(ParseWeaponClipGroups(R"({"version":1,"rows":[{"weapon":27340000,"clip_group":170},{"weapon":4000000,"clip_group":200}]})",
                                &b, &error));
    AddWeaponClipGroups(&merged, "dancer", a);
    AddWeaponClipGroups(&merged, "later", b);
    CHECK(merged.rows.size() == 4 && merged.names.size() == 2);
    const std::string text = WeaponClipGroupsFileText(merged);
    CHECK(text.find("# weapon_clip_groups") == 0 && text.find("dancer, later") != std::string::npos);
    CHECK(text.find("4000000 200\n27330000 161\n27340000 170\n27350000 161\n") != std::string::npos);
    std::vector<cg::Row> rows;
    std::size_t bad = 0;
    CHECK(cg::ParseTable(text, &rows, &bad) && rows.size() == 4);
    CHECK(cg::GroupOf(rows, 27330000) == 161);
    CHECK(cg::GroupOf(rows, 27331000) == 161);  // +10
    CHECK(cg::GroupOf(rows, 27340500) == 170);  // later mod wins
    CHECK(cg::GroupOf(rows, 4000000) == 200);
    CHECK(cg::GroupOf(rows, 27360000) == 0);
    CHECK(cg::GroupOf(rows, -1) == 0);
    CHECK(cg::GroupOf(rows, 0) == 0);
    CHECK(cg::GroupOf(rows, 9999) == 0);
}

static void EngineFile() {
    std::vector<cg::Row> rows;
    std::size_t bad = 0;
    CHECK(cg::ParseTable("", &rows) && rows.empty());
    CHECK(cg::ParseTable("# c\n\n 50000 7 # x\r\n20000 3\n", &rows) && rows.size() == 2 && rows[0].weapon == 20000);
    CHECK(!cg::ParseTable("20000 3\n20001 3\n", &rows, &bad) && bad == 2 && rows.empty());
    CHECK(!cg::ParseTable("20000\n", &rows, &bad) && bad == 1);
    CHECK(!cg::ParseTable("20000 3 4\n", &rows));
    CHECK(!cg::ParseTable("20000 0\n", &rows));
    CHECK(!cg::ParseTable("20000 1000\n", &rows));
    CHECK(!cg::ParseTable("2147490000 1\n", &rows));
    CHECK(!cg::ParseTable("-20000 1\n", &rows));
    std::string many;
    for (std::size_t i = 0; i <= cg::kMaxRows; ++i) many += std::to_string(10000 * (i + 1)) + " 1\n";
    CHECK(!cg::ParseTable(many, &rows));
}

int main() {
    ManifestUse();
    Json();
    MergeAndEngine();
    EngineFile();
    std::printf("weapon-clip-groups-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
