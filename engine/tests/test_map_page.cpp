// SPDX-License-Identifier: GPL-2.0-or-later
// The map page's rules (gpu/shim/map/map_page_rules.h).
#include "map/map_data_rules.h"
#include "map/map_page_rules.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unordered_map>

static int failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

// The map data rules on a small hand-made area (map_data_rules.h).
static void DataRules() {
    using namespace BbMapData;
    std::unordered_map<std::uint32_t, int> flags;
    const FlagFn flag = [&](std::uint32_t id) {
        const auto it = flags.find(id);
        return it == flags.end() ? -1 : it->second;
    };
    const Json atlas = Json::parse(R"({"version": 1, "maps": [
        {"id": "m24_00_00_00", "name": "Cathedral Ward", "bounds": [-100, -100, 100, 100]},
        {"id": "bad", "name": "x", "bounds": [0, 0, 1, 1]},
        {"id": "m21_00_00_00", "name": "Hunter's Dream", "bounds": [0, 0, 0, 1]}]})");
    auto areas = ParseAtlas(atlas);
    CHECK(areas.size() == 1 && areas[0].name == "Cathedral Ward");  // bad id and empty bounds dropped
    const Json features = Json::parse(std::string(R"({"schema": 1, "game_eboot_sha256": ")") + kEbootSha256 + R"(",
      "areas": {"m24_00_00_00": {
        "lamps": [{"name": "Cathedral Ward", "pos": [1, 2, 3], "unlock_flag": 100, "return_point": 2402950},
                  {"name": "Dream", "pos": [0, 0, 0], "available_on": [101]}],
        "pickups": [{"pos": [10, 0, 10], "contents": [
                        {"name": "Blood Vial", "category": "healing", "quantity": 2, "collected_flag": 200},
                        {"name": "Coldblood", "category": "echoes", "quantity": 1, "collected_flag": 201}]},
                    {"pos": [50, 0, 50], "available_off": [102], "contents": [
                        {"name": "Madman's Knowledge", "category": "insight", "quantity": 1, "collected_flag": 202}]}],
        "reveal_regions": [{"min_y": -10, "max_y": 10, "bounds": [0, 0, 20, 20], "boss_flags": [300]},
                           {"triangles": [[30, 30, 70, 30, 30, 70]], "reveal_when": ["==", ["flag", 301], 1]}],
        "npc_rules": {"7": ["and", ["==", ["flag", 400], 1], ["==", ["flag", 401], 0]]},
        "always_visible": false}}})");
    ApplyFeatures(features, areas);
    const Area& a = areas[0];
    CHECK(a.lamps.size() == 2 && a.pickups.size() == 2 && a.reveals.size() == 2 && a.npc_rules.size() == 1);
    CHECK(a.lamps[0].return_point == 2402950 && a.lamps[1].return_point == 0);  // where map travel goes; 0: none given
    bool threw = false;
    try {
        Json other = features;
        other["game_eboot_sha256"] = "00";
        ApplyFeatures(other, areas);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);  // made from another game version: refused
    const auto ids = CollectFlags(areas);
    CHECK((ids == std::vector<std::uint32_t>{100, 101, 102, 200, 201, 202, 300, 301, 400, 401}));

    // Expressions: unknown stays unknown unless the other side decides.
    CHECK(Evaluate(Json::parse(R"(["and", ["flag", 1], false])"), flag) == 0);
    CHECK(Evaluate(Json::parse(R"(["and", ["flag", 1], true])"), flag) == -1);
    CHECK(Evaluate(Json::parse(R"(["or", ["flag", 1], true])"), flag) == 1);
    CHECK(Evaluate(Json::parse(R"(["bogus", 1, 2])"), flag) == -1);

    // Lamps: unlit hidden; the Dream (no lit flag) follows its conditions.
    CHECK(!StateOf(a.lamps[0], flag).discovered);
    flags[100] = 1;
    CHECK(StateOf(a.lamps[0], flag).discovered && StateOf(a.lamps[0], flag).available);
    CHECK(!StateOf(a.lamps[1], flag).discovered);
    flags[101] = 1;
    CHECK(StateOf(a.lamps[1], flag).discovered);

    // Pickups: collected or unknown items hidden, categories filter, conditions apply.
    CHECK(Pickups(a, kAllItems, false, flag).empty());  // nothing known yet
    flags[200] = 0;
    flags[201] = 0;
    auto shown = Pickups(a, kAllItems, false, flag);
    CHECK(shown.size() == 1 && shown[0].category == "healing" && shown[0].label == "Blood Vial x2\nColdblood x1");
    CHECK(Pickups(a, kEchoes, false, flag).at(0).label == "Coldblood x1");
    CHECK(Pickups(a, 0, false, flag).empty());
    flags[200] = 1;
    CHECK(Pickups(a, kHealing, false, flag).empty());  // the vial is collected
    flags[202] = 0;
    flags[102] = 0;
    CHECK(Pickups(a, kInsight, false, flag).size() == 1);
    flags[102] = 1;
    CHECK(Pickups(a, kInsight, false, flag).empty());  // its condition is off

    // Boss reveal: hidden until the boss flag; outside every region hidden; off shows all.
    CHECK(Revealed(a, false, 999, 0, 999, flag));
    CHECK(!Revealed(a, true, 5, 0, 5, flag));
    CHECK(!Revealed(a, true, 999, 0, 999, flag));
    flags[300] = 1;
    CHECK(Revealed(a, true, 5, 0, 5, flag) && !Revealed(a, true, 5, 50, 5, flag));  // above its height
    CHECK(!Revealed(a, true, 35, 0, 35, flag));
    flags[301] = 1;
    CHECK(Revealed(a, true, 35, 0, 35, flag) && !Revealed(a, true, 65, 0, 65, flag));  // outside the triangle
    CHECK((RegionVisibility(a, flag) == std::vector<bool>{true, true}));
    flags[300] = 0;
    CHECK((RegionVisibility(a, flag) == std::vector<bool>{false, true}));

    // NPC sites.
    CHECK(NpcShown(a, NpcMode::All, 99, flag) && !NpcShown(a, NpcMode::Hidden, 7, flag));
    CHECK(!NpcShown(a, NpcMode::NewDialogue, 7, flag));
    flags[400] = 1;
    flags[401] = 0;
    CHECK(NpcShown(a, NpcMode::NewDialogue, 7, flag) && !NpcShown(a, NpcMode::NewDialogue, 8, flag));

    // Place names: main place, then another place, then the nearest landmark in reach.
    ApplyCatalog(Json::parse(R"({"m24_00_00_00": {
        "regions": [{"name": "Oedon Chapel", "shape": "footprint", "min_y": -5, "max_y": 5, "main_location": true,
                     "triangles": [[0, 0, 10, 0, 0, 10]]},
                    {"name": "Chapel Roof", "min_y": -5, "max_y": 5, "triangles": [[0, 0, 10, 0, 0, 10]]}],
        "landmarks": [{"name": "the Lamp", "pos": [50, 0, 50], "max_horizontal_distance": 10},
                      {"name": "the Gate", "pos": [55, 0, 50], "max_horizontal_distance": 10, "subtitle_prefix": "By "}]}})"),
                 areas);
    CHECK(Title(a, 1, 0, 1) == "Oedon Chapel" && Subtitle(a, 1, 0, 1) == "Chapel Roof");
    CHECK(Title(a, 1, 50, 1) == "Cathedral Ward");  // above the place
    CHECK(Title(a, 51, 0, 50) == "Cathedral Ward" && Subtitle(a, 51, 0, 50) == "Near the Lamp");
    CHECK(Subtitle(a, 55, 0, 50) == "By the Gate" && Subtitle(a, 90, 0, 90).empty());

    // Markers: only ladders, gates and NPCs.
    const auto markers = ParseMarkers(Json::parse(R"({"markers": [
        {"kind": "ladder", "name": "Ladder", "pos": [1, 2, 3]}, {"kind": "chest", "pos": [0, 0, 0]},
        {"kind": "npc", "name": "Gilbert", "pos": [4, 5, 6], "entity_id": 7}, {"kind": "gate", "pos": [1, 2]}]})"));
    CHECK(markers.size() == 2 && markers[1].entity == 7 && markers[1].name == "Gilbert");

    CHECK(CycleLamp(0, -1, 1) == -1 && CycleLamp(3, -1, 1) == 0 && CycleLamp(3, -1, -1) == 2);
    CHECK(CycleLamp(3, 2, 1) == 0 && CycleLamp(3, 0, -1) == 2 && CycleLamp(3, 1, 1) == 2);
}

// `map-page-test <atlas maps dir>`: parses a real generator output and prints what it found.
static int RealData(const char* dir) {
    using namespace BbMapData;
    const auto t0 = std::chrono::steady_clock::now();
    const auto load = [&](const std::string& name) {
        std::ifstream f(std::string(dir) + "/" + name, std::ios::binary);
        return Json::parse(f);
    };
    auto areas = ParseAtlas(load("atlas.json"));
    ApplyFeatures(load("features.json"), areas);
    ApplyCatalog(load("catalog.json"), areas);
    size_t lamps = 0, pickups = 0, reveals = 0, places = 0, landmarks = 0, markers = 0;
    for (const auto& a : areas) {
        lamps += a.lamps.size();
        pickups += a.pickups.size();
        reveals += a.reveals.size();
        places += a.places.size();
        landmarks += a.landmarks.size();
        markers += ParseMarkers(load(a.id + ".json")).size();
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%zu areas, %zu lamps, %zu pickups, %zu reveal regions, %zu places, %zu landmarks, %zu markers, %zu flags; %.0f ms\n",
                areas.size(), lamps, pickups, reveals, places, landmarks, markers, CollectFlags(areas).size(), ms);
    return areas.empty() || !lamps ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc == 2) return RealData(argv[1]);
    using namespace BbMapPage;
    DataRules();
    CHECK(Region(0x18000000u) == "m24_00_00_00");  // Cathedral Ward
    CHECK(Region(0x15000000u) == "m21_00_00_00");  // Hunter's Dream
    CHECK(Region(0x18010000u) == "m24_01_00_00");
    CHECK(Region(0x22000000u) == "m34_00_00_00");  // The Old Hunters
    CHECK(Region(0xffffffffu).empty());           // between loads
    CHECK(Region(0x1d000000u).empty());           // a Chalice Dungeon
    CHECK(Region(0x1d0a5a00u).empty());
    CHECK(WheelZoom(4.0f, 0) == 4.0f);
    CHECK(WheelZoom(4.0f, 1) > 4.0f && WheelZoom(4.0f, -1) < 4.0f);
    CHECK(WheelZoom(24.0f, 10) == kMaxZoom && WheelZoom(0.2f, -10) == kMinZoom);
    CHECK(WheelZoom(-1.0f, 0) == kDefaultZoom && WheelZoom(NAN, 0) == kDefaultZoom);
    CHECK(WheelZoom(4.0f, NAN) == 4.0f);
    // bind.map / pad.map values: base names, modifiers dropped, blanks skipped.
    CHECK(InputNames("M") == std::vector<std::string>{"M"});
    CHECK((InputNames(" M, Ctrl+Shift+Mouse4 ,,Left Ctrl\r\n") ==
           std::vector<std::string>{"M", "Mouse4", "Left Ctrl"}));
    CHECK(InputNames("").empty() && InputNames(" , ").empty());
    CHECK(InputNames("Ctrl+") == std::vector<std::string>{"Ctrl+"});  // a key named so, not a modifier

    // The controller pointer: speed in pixels a second, kept inside the canvas; bad input resets.
    float u = .5f, v = .5f;
    MovePointer(&u, &v, 1, 0, .5f, 500, 1000, 500);
    CHECK(std::abs(u - .75f) < 1e-5f && v == .5f);
    MovePointer(&u, &v, 1, -1, 10, 500, 1000, 500);
    CHECK(u == 1.0f && v == 0.0f);
    MovePointer(&u, &v, NAN, 0, .1f, 500, 1000, 500);
    CHECK(u == 1.0f && v == 0.0f);  // a bad stick moves nothing
    u = NAN;
    MovePointer(&u, &v, 0, 0, .1f, 500, 0, 0);
    CHECK(u == .5f && v == .5f);
    // Panning: right moves the centre east (+x), down moves it south (-z); a drag follows the pointer.
    float cx = 10, cz = 20;
    Pan(&cx, &cz, 1, 1, .5f, 400, 4);
    CHECK(cx == 60 && cz == -30);
    Pan(&cx, &cz, 1, 1, .5f, 400, 0);
    CHECK(cx == 60 && cz == -30);  // no zoom: no move
    Drag(&cx, &cz, 40, -40, 4);
    CHECK(cx == 50 && cz == -40);
    // The icon under the pointer: the nearest within its radius.
    const std::vector<Hit> hits = {{0, 0, 10}, {8, 0, 10}, {100, 100, 5}};
    CHECK(Nearest(hits, 5, 0) == 1 && Nearest(hits, 3, 0) == 0 && Nearest(hits, 50, 50) == -1);
    CHECK(Nearest(hits, 103, 100) == 2 && Nearest(hits, 106, 100) == -1 && Nearest({}, 0, 0) == -1);
    // Floors: wraps; one floor has nothing to step to.
    // Floors by height: up is the next higher one (floor 1 lowest, then 2, 0), wrapping.
    const std::vector<float> heights = {5, -3, 1};
    CHECK(StepFloor(heights, 1, 1) == 2 && StepFloor(heights, 2, 1) == 0 && StepFloor(heights, 0, 1) == 1);
    CHECK(StepFloor(heights, 0, -1) == 2 && StepFloor(heights, 1, -1) == 0);
    CHECK(StepFloor(heights, -1, 1) == 1 && StepFloor(heights, -1, -1) == 0);  // none drawn: lowest / highest
    CHECK(StepFloor({0, 0, 0}, 0, 1) == 1 && StepFloor({0, 0, 0}, 2, 1) == 0);  // equal heights: floor order
    CHECK(StepFloor({2, NAN}, 0, 1) == 1 && StepFloor({2}, 0, 1) == -1 && StepFloor(heights, 1, 0) == -1);
    // Cross / Enter / click: the first press selects a lamp, the second on it travels.
    CHECK(OnConfirm(2, -1, true, false) == Confirm::Select && OnConfirm(2, 1, true, true) == Confirm::Select);
    CHECK(OnConfirm(2, 2, true, false) == Confirm::Travel && OnConfirm(2, 2, true, true) == Confirm::Travel);
    CHECK(OnConfirm(-1, 2, true, false) == Confirm::Travel);  // Cross with the lamp shown, pointer elsewhere
    CHECK(OnConfirm(-1, 2, true, true) == Confirm::None);     // a click on empty map never travels
    CHECK(OnConfirm(2, 2, false, false) == Confirm::None && OnConfirm(-1, -1, true, false) == Confirm::None);
    CHECK(OnConfirm(3, 2, false, true) == Confirm::Select);   // selecting works with travel off
    std::printf(failures ? "map-page-test: %d failures\n" : "map-page-test: ok\n", failures);
    return failures ? 1 : 0;
}
