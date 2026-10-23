// SPDX-License-Identifier: GPL-2.0-or-later
// The map page's rules (gpu/shim/map/map_page_rules.h).
#include "map/map_page_rules.h"

#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

int main() {
    using namespace BbMapPage;
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
    std::printf(failures ? "map-page-test: %d failures\n" : "map-page-test: ok\n", failures);
    return failures ? 1 : 0;
}
