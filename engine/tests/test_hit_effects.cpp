// SPDX-License-Identifier: GPL-3.0-or-later
// Nightwatch 0.3.0 mod platform: start/features.h (manifest, hit_effects.json, merge, out file) and
// gpu/shim/party/hit_effects_rules.h (the engine's reading of that file and its pick).
// Build and run: ninja -C out/gpu hit-effects-test && out/gpu/hit-effects-test.exe
#include "start/features.h"
#include "gpu/shim/party/hit_effects_rules.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace nwfeatures;
namespace he = coop::hiteffects;

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

static const char* kGold =
    R"({"id":"gold-bolt-paper","name":"Gold Bolt Paper","version":"1.0.0","author":"x","requires":"0.3.0","features":["hit_effects"]})";
static const char* kGoldTable =
    R"({"version":1,"rows":[{"attacker_speffect":90002,"base_fx":880,"fx":900880},{"attacker_speffect":90002,"base_fx":10880,"fx":910880}]})";

static void Versions() {
    Version v;
    CHECK(ParseVersion("0.3.0", &v) && v.major == 0 && v.minor == 3 && v.patch == 0);
    CHECK(ParseVersion("0.3.0-alpha.1", &v) && v.minor == 3);
    CHECK(ParseVersion("1.2", &v) && v.major == 1 && v.minor == 2 && v.patch == 0);
    CHECK(ParseVersion("10.20.30+meta", &v) && v.patch == 30);
    CHECK(!ParseVersion("", &v));
    CHECK(!ParseVersion("3", &v));
    CHECK(!ParseVersion("0.3.", &v));
    CHECK(!ParseVersion("0.3.0.1", &v));
    CHECK(!ParseVersion("v0.3.0", &v));
    CHECK(!ParseVersion("0.x.0", &v));
    Version a, b;
    ParseVersion("0.3.0", &a);
    ParseVersion("0.2.9", &b);
    CHECK(Compare(a, b) > 0 && Compare(b, a) < 0 && Compare(a, a) == 0);
}

static void Manifests() {
    Manifest m;
    std::string e;
    CHECK(ParseManifest(kGold, &m, &e));
    CHECK(m.id == "gold-bolt-paper" && m.name == "Gold Bolt Paper" && m.version == "1.0.0" && m.min_version == "0.3.0");
    CHECK(m.features.size() == 1 && m.Uses("hit_effects") && !m.Uses("other"));
    CHECK(Refusal(m, "0.3.0") == "");
    CHECK(Refusal(m, "0.3.0-alpha.1") == "");  // a 0.3.0 pre-release offers the 0.3.0 features
    CHECK(Refusal(m, "0.4.1") == "");
    CHECK(Refusal(m, "0.2.9-beta.1") == "needs Nightwatch 0.3.0 or newer (this is 0.2.9-beta.1)");
    // BOM, extra keys, no features
    CHECK(ParseManifest("\xEF\xBB\xBF{\"id\":\"a\",\"requires\":\"0.3\",\"homepage\":\"x\"}", &m, &e));
    CHECK(m.features.empty() && Refusal(m, "0.3.0") == "");
    // unknown feature
    CHECK(ParseManifest(R"({"id":"a","requires":"0.3.0","features":["hit_effects","frostbite"]})", &m, &e));
    CHECK(Refusal(m, "0.3.0") == "uses the feature \"frostbite\", which Nightwatch 0.3.0 does not have");
    // bad manifests
    CHECK(!ParseManifest("not json", &m, &e) && e == "nightwatch.json is not a JSON object");
    CHECK(!ParseManifest("[]", &m, &e));
    CHECK(!ParseManifest(R"({"requires":"0.3.0"})", &m, &e) && e == "nightwatch.json has no \"id\"");
    CHECK(!ParseManifest(R"({"id":"a"})", &m, &e) && e == "nightwatch.json has no \"requires\"");
    CHECK(!ParseManifest(R"({"id":"","requires":"0.3.0"})", &m, &e));
    CHECK(!ParseManifest(R"({"id":"a","requires":"soon"})", &m, &e));
    CHECK(!ParseManifest(R"({"id":"a","requires":3})", &m, &e));
    CHECK(!ParseManifest(R"({"id":"a","requires":"0.3.0","features":"hit_effects"})", &m, &e));
    CHECK(!ParseManifest(R"({"id":"a","requires":"0.3.0","features":[1]})", &m, &e));
    CHECK(!ParseManifest(R"({"id":"a","requires":"0.3.0","name":5})", &m, &e));
}

static void Tables() {
    std::vector<HitRow> rows;
    std::string e;
    CHECK(ParseHitEffects(kGoldTable, &rows, &e));
    CHECK(rows.size() == 2 && rows[0] == (HitRow{90002, 880, 900880}) && rows[1] == (HitRow{90002, 10880, 910880}));
    CHECK(ParseHitEffects(R"({"version":1,"rows":[]})", &rows, &e) && rows.empty());
    CHECK(ParseHitEffects(R"({"version":1,"rows":[{"attacker_speffect":1,"base_fx":2,"fx":3,"note":"x"}],"x":1})", &rows,
                          &e) &&
          rows.size() == 1);  // unknown fields ignored
    CHECK(!ParseHitEffects(R"({"version":2,"rows":[]})", &rows, &e) && e == "nightwatch/hit_effects.json: \"version\" is not 1");
    CHECK(!ParseHitEffects(R"({"rows":[]})", &rows, &e));
    CHECK(!ParseHitEffects(R"({"version":1})", &rows, &e));
    CHECK(!ParseHitEffects("{", &rows, &e));
    const char* bad[] = {
        R"({"version":1,"rows":[{"attacker_speffect":1,"base_fx":2,"fx":3},{"attacker_speffect":1,"base_fx":2}]})",
        R"({"version":1,"rows":[{"attacker_speffect":0,"base_fx":2,"fx":3}]})",
        R"({"version":1,"rows":[{"attacker_speffect":-1,"base_fx":2,"fx":3}]})",
        R"({"version":1,"rows":[{"attacker_speffect":1,"base_fx":2,"fx":2147483648}]})",
        R"({"version":1,"rows":[{"attacker_speffect":1,"base_fx":2.5,"fx":3}]})",
        R"({"version":1,"rows":[{"attacker_speffect":"1","base_fx":2,"fx":3}]})",
        R"({"version":1,"rows":[5]})",
    };
    for (const char* b : bad) CHECK(!ParseHitEffects(b, &rows, &e) && rows.empty());
    CHECK(!ParseHitEffects(bad[0], &rows, &e) &&
          e == "nightwatch/hit_effects.json: row 2 needs whole numbers 1..2147483647 for attacker_speffect, base_fx and fx");
    std::string many = R"({"version":1,"rows":[)";
    for (std::size_t i = 0; i <= kMaxHitRows; ++i) many += std::string(i ? "," : "") + R"({"attacker_speffect":1,"base_fx":2,"fx":3})";
    many += "]}";
    CHECK(!ParseHitEffects(many, &rows, &e));
}

static void MergeAndEngine() {
    HitMerged merged;
    CHECK(HitEffectsFileText(merged).empty());
    AddHitEffects(&merged, "A", {{90002, 880, 900880}, {90002, 10880, 910880}, {5, 806, 806000}});
    AddHitEffects(&merged, "empty", {});
    AddHitEffects(&merged, "B", {{90002, 880, 111}, {7, 880, 222}});  // B wins 90002/880; adds 7/880
    CHECK(merged.names.size() == 2 && merged.rows.size() == 4);
    const std::string text = HitEffectsFileText(merged);
    CHECK(text.rfind("# hit_effects", 0) == 0 && text.find("from: A, B\n") != std::string::npos);
    std::vector<he::Row> rows;
    std::size_t bad = 0;
    CHECK(he::ParseTable(text, &rows, &bad));
    CHECK(rows.size() == 4);
    CHECK(rows[0].attacker_speffect == 90002 && rows[0].base_fx == 880 && rows[0].fx == 111);  // newest mod first
    CHECK(rows[1].attacker_speffect == 7 && rows[1].fx == 222);
    // the engine's pick
    std::set<int> on = {90002};
    const auto has = [&](int id) { return on.count(id) > 0; };
    int sp = 0;
    CHECK(he::Pick(rows, 880, has, &sp) == 111 && sp == 90002);
    CHECK(he::Pick(rows, 10880, has) == 910880);
    CHECK(he::Pick(rows, 10980, has) == 0);  // no row: keep the game's id
    CHECK(he::Pick(rows, 806, has) == 0);    // row, but SpEffect 5 is not on
    on = {7, 90002};
    CHECK(he::Pick(rows, 880, has) == 111);  // both B rows match: B's first listed row
    on = {7};
    CHECK(he::Pick(rows, 880, has) == 222);
    on = {};
    CHECK(he::Pick(rows, 880, has) == 0);
    CHECK(he::Watched(rows, 880) && he::Watched(rows, 806) && !he::Watched(rows, 881));
    // the gold table end to end
    HitMerged gold;
    std::vector<HitRow> own;
    std::string e;
    CHECK(ParseHitEffects(kGoldTable, &own, &e));
    AddHitEffects(&gold, "Gold Bolt Paper", own);
    CHECK(he::ParseTable(HitEffectsFileText(gold), &rows));
    on = {90002};
    CHECK(he::Pick(rows, 880, has) == 900880 && he::Pick(rows, 10880, has) == 910880 && he::Pick(rows, 881, has) == 0);
}

static void EngineFile() {
    std::vector<he::Row> rows;
    std::size_t bad = 0;
    CHECK(he::ParseTable("", &rows) && rows.empty());
    CHECK(he::ParseTable("# c\n\n  1 2 3  # x\r\n4\t5\t6", &rows) && rows.size() == 2 && rows[1].fx == 6);
    CHECK(!he::ParseTable("1 2 3\n1 2\n", &rows, &bad) && bad == 2 && rows.empty());
    CHECK(!he::ParseTable("1 2 3 4\n", &rows, &bad) && bad == 1);
    CHECK(!he::ParseTable("0 2 3\n", &rows));
    CHECK(!he::ParseTable("1 2 2147483648\n", &rows));
    CHECK(!he::ParseTable("1 -2 3\n", &rows));
    CHECK(!he::ParseTable("a b c\n", &rows));
    std::string many;
    for (std::size_t i = 0; i <= he::kMaxRows; ++i) many += "1 2 3\n";
    CHECK(!he::ParseTable(many, &rows));
}

int main() {
    Versions();
    Manifests();
    Tables();
    MergeAndEngine();
    EngineFile();
    std::printf("hit-effects-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
