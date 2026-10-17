// SPDX-License-Identifier: GPL-3.0-or-later
// guest_stick=X:Z[:R[:STEPS]] argument parsing (gpu/shim/party/party_stick_args.h).
// Build and run: ninja -C out/gpu party-stick-test && out/gpu/party-stick-test.exe
#include "party/party_stick_args.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

int main() {
    float x = 0, z = 0, r = 0;
    int steps = 0;
    CHECK(ParseStickArgs("-66.5:122", &x, &z, &r, &steps));
    CHECK(x == -66.5f && z == 122.0f && r == 1.0f && steps == 40);
    CHECK(ParseStickArgs("10:20:2.5", &x, &z, &r, &steps));
    CHECK(r == 2.5f && steps == 40);
    CHECK(ParseStickArgs("10:20:2:300", &x, &z, &r, &steps));
    CHECK(steps == 300);
    CHECK(!ParseStickArgs("", &x, &z, &r, &steps));
    CHECK(!ParseStickArgs("10", &x, &z, &r, &steps));
    CHECK(!ParseStickArgs("10:20:0", &x, &z, &r, &steps));
    CHECK(!ParseStickArgs("10:20:1:0", &x, &z, &r, &steps));
    CHECK(!ParseStickArgs("10:20:1:5000", &x, &z, &r, &steps));
    CHECK(!ParseStickArgs("10:20:1:5x", &x, &z, &r, &steps));
    int e = 0;
    float d = 0, a = 0;
    CHECK(!ParseStickArgs("obj:2411998:6:180", &x, &z, &r, &steps));
    CHECK(ParseStickObjArgs("obj:2411998:6:180", &e, &d, &a, &r, &steps));
    CHECK(e == 2411998 && d == 6.0f && a == 180.0f && r == 1.0f && steps == 40);
    CHECK(ParseStickObjArgs("obj:2411998:6:0:2:80", &e, &d, &a, &r, &steps));
    CHECK(r == 2.0f && steps == 80);
    CHECK(!ParseStickObjArgs("obj:2411998:6", &e, &d, &a, &r, &steps));
    CHECK(!ParseStickObjArgs("10:20", &e, &d, &a, &r, &steps));
    CHECK(!ParseStickObjArgs("obj:0:6:0", &e, &d, &a, &r, &steps));
    std::vector<std::array<float, 3>> route;
    CHECK(ParseRouteArgs("-122.6:-27:68.0/-120.1:72/-118.4:-26.8:76.4", &route));
    CHECK(route.size() == 3 && route[0][0] == -122.6f && route[0][1] == -27.0f && route[0][2] == 68.0f);
    CHECK(route[1][0] == -120.1f && std::isnan(route[1][1]) && route[1][2] == 72.0f);
    CHECK(route[2][2] == 76.4f);
    CHECK(ParseRouteArgs("1:2", &route) && route.size() == 1);
    CHECK(!ParseRouteArgs("", &route));
    CHECK(!ParseRouteArgs("1:2/", &route));
    CHECK(!ParseRouteArgs("1:2//3:4", &route));
    CHECK(!ParseRouteArgs("1:2/3", &route));
    CHECK(!ParseRouteArgs("1:2:3:4", &route));
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("party-stick-test: ok\n");
    return 0;
}
