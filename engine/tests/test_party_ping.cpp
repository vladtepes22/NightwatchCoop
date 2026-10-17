// SPDX-License-Identifier: GPL-3.0-or-later
// Pings (I2, gpu/shim/party/party_ping.cpp): EVENT map_ping JSON, the host's relay rate, the
// marker board, the aim pick from the depth probe and the screen-edge clamp.
// cmake --build out/gpu --target party-ping-test && out/gpu/party-ping-test.exe
#include "party/party_ping.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace P = party::ping;

static bool Near(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}

static void TestJson() {
    P::PingEvent e;
    e.id = 3;
    e.map = 0x18010000;
    e.pos[0] = 1.234f, e.pos[1] = -2.5f, e.pos[2] = 300.0f;
    e.hit = true;
    e.t = 987654321;
    P::PingEvent back;
    std::string err;
    assert(P::FromJson(P::ToJson(e), &back, &err));
    assert(back.id == 3 && back.map == e.map && back.hit && !back.clear && back.t == e.t && back.slot == -1 &&
           back.who.empty());
    assert(Near(back.pos[0], 1.23f) && Near(back.pos[1], -2.5f) && Near(back.pos[2], 300.0f));
    e.slot = 2;
    e.who = "Hunter_1";
    assert(P::FromJson(P::ToJson(e), &back, &err) && back.slot == 2 && back.who == "Hunter_1");
    // Refused: no map, a bad name, a slot out of range, a non-finite / absurd position, not JSON.
    assert(!P::FromJson(R"({"map":0,"pos":[1,2,3]})", &back, &err));
    assert(!P::FromJson(R"({"map":4294967295,"pos":[1,2,3]})", &back, &err));
    assert(!P::FromJson(R"({"map":402718720,"pos":[1,2,3],"who":"a b"})", &back, &err));
    assert(!P::FromJson(R"({"map":402718720,"pos":[1,2,3],"who":"ABCDEFGHIJKLMNOPQ"})", &back, &err));
    assert(!P::FromJson(R"({"map":402718720,"pos":[1,2,3],"slot":8})", &back, &err));
    assert(!P::FromJson(R"({"map":402718720,"pos":[1,2,3],"slot":-2})", &back, &err));
    assert(!P::FromJson(R"({"map":402718720,"pos":[1,2,1e6]})", &back, &err));
    assert(!P::FromJson(R"({"map":402718720,"pos":[1,2]})", &back, &err));
    assert(!P::FromJson(R"({"map":402718720})", &back, &err));
    assert(!P::FromJson(R"({"map":402718720,"pos":[1,2,3],"hit":2})", &back, &err));
    assert(!P::FromJson(R"({"map":1.5,"pos":[1,2,3]})", &back, &err));
    assert(!P::FromJson("[1,2,3]", &back, &err) && !P::FromJson("{", &back, &err));
    // A clear needs no map or position.
    assert(P::FromJson(R"({"clear":1,"slot":1})", &back, &err) && back.clear && back.slot == 1);
}

static void TestRate() {
    P::RelayRate r;
    for (int i = 0; i < P::kRelayBurst; ++i) assert(r.Take(1, 100.0));
    assert(!r.Take(1, 100.0));
    assert(r.Take(2, 100.0));  // per member
    assert(!r.Take(1, 100.5));
    assert(r.Take(1, 100.0 + P::kRelayWindowS / P::kRelayBurst + 0.01));  // one back per window / burst
    assert(!r.Take(-1, 1.0) && !r.Take(P::kMaxSlots, 1.0));
}

static void TestBoard() {
    P::ResetForTest();
    P::Marker m;
    m.slot = 1;
    std::snprintf(m.name, sizeof m.name, "Hunter1");
    m.map = 7;
    m.until_ms = 5000;
    P::Place(m);
    m.slot = 0;
    m.until_ms = 3000;
    P::Place(m);
    P::Marker out[P::kMaxSlots];
    assert(P::Read(out, 1000) == 2);
    assert(P::Read(out, 3500) == 1 && out[0].slot == 1);  // slot 0's ran out
    m.slot = 1;
    m.map = 9;
    m.until_ms = 9000;
    P::Place(m);  // one per player: replaces
    assert(P::Read(out, 4000) == 1 && out[0].map == 9);
    P::Clear(1);
    assert(P::Read(out, 4000) == 0);
    m.slot = 99;  // ignored
    P::Place(m);
    assert(P::Read(out, 0) == 0);
    // Fade over the last kFadeMs; life left from the host clock.
    assert(P::Alpha(P::kLifeMs) == 1.0f && P::Alpha(0) == 0.0f && Near(P::Alpha(P::kFadeMs / 2), 0.5f));
    assert(P::LifeLeft(10000, 10000) == P::kLifeMs);
    assert(P::LifeLeft(10000, 13000) == P::kLifeMs - 3000);
    assert(P::LifeLeft(10000, 10000 + P::kLifeMs) == 0);   // replayed after a reconnect: old
    assert(P::LifeLeft(10000, 9000) == P::kLifeMs);        // clocks a little apart
    assert(P::LifeLeft(20000, 10000) == 0);                // absurd
    assert(P::LifeLeft(0, 10000) == P::kLifeMs);           // no clock
    char d[16];
    P::FormatDistance(3.44f, d, sizeof d);
    assert(std::strcmp(d, "3.4 m") == 0);
    P::FormatDistance(123.6f, d, sizeof d);
    assert(std::strcmp(d, "124 m") == 0);
}

static void TestAim() {
    // Camera 4 m behind and 2 m above the player at the origin, looking along +z.
    const float cam[3] = {0, 2.0f, -4.0f}, player[3] = {0, 0, 0};
    P::Sample s[8];
    // centre (index 6) hits the player's back, 5 its head, 4 a wall 20 m ahead, above that sky.
    s[7] = {true, {0, 0.5f, -0.3f}};
    s[6] = {true, {0, 1.0f, -0.2f}};
    s[5] = {true, {0, 1.7f, 0.1f}};
    s[4] = {true, {0, 3.0f, 20.0f}};
    s[3] = {false, {0, 0, 0}};
    float at[3];
    assert(P::PickAim(cam, player, s, 8, 6, at) && Near(at[2], 20.0f));
    // Something between the camera and the player (a branch): skipped.
    s[5] = {true, {0, 2.0f, -3.0f}};
    assert(P::PickAim(cam, player, s, 8, 6, at) && Near(at[2], 20.0f));
    // Only sky past the player: nothing; too far (the sky dome at 3 km): nothing.
    s[4] = {false, {0, 0, 0}};
    assert(!P::PickAim(cam, player, s, 8, 6, at));
    s[4] = {true, {0, 50.0f, 3000.0f}};
    assert(!P::PickAim(cam, player, s, 8, 6, at));
    // Looking down at the ground ahead: the centre itself is past the player.
    P::Sample g[3] = {{true, {0, 0, 9.0f}}, {true, {0, 0, 6.0f}}, {true, {0, 0, 4.0f}}};
    assert(P::PickAim(cam, player, g, 3, 2, at) && Near(at[2], 4.0f));
    assert(!P::PickAim(cam, player, g, 3, 3, at) && !P::PickAim(cam, player, nullptr, 0, 0, at));
    // The fallback: 15 m along the view from the chest.
    const float fwd[3] = {0, 0, 2.0f};
    P::FallbackAim(fwd, player, at);
    assert(Near(at[0], 0) && Near(at[1], 1.0f) && Near(at[2], P::kFallbackDistance));
    const float bad[3] = {0, 0, 0};
    P::FallbackAim(bad, player, at);
    assert(std::isfinite(at[0]) && std::isfinite(at[2]));
}

static void TestEdge() {
    // Identity view (camera at the origin looking along +z), 90 degree fov, ndc +y down.
    const float view[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const float proj[4] = {1, 1, 1, -0.1f};
    const float ahead[3] = {0, 0, 10};
    P::EdgePoint e = P::ToScreenOrEdge(view, proj, ahead, 800, 600, 20);
    assert(e.on_screen && Near(e.x, 400) && Near(e.y, 300) && Near(e.depth, 10));
    const float right[3] = {30, 0, 10};  // off to the right: clamped to the right edge
    e = P::ToScreenOrEdge(view, proj, right, 800, 600, 20);
    assert(!e.on_screen && Near(e.x, 780) && Near(e.y, 300) && Near(e.angle, 0));
    const float behind_left[3] = {-5, 0, -10};  // behind, to the left: the left edge
    e = P::ToScreenOrEdge(view, proj, behind_left, 800, 600, 20);
    assert(!e.on_screen && Near(e.x, 20) && e.depth < 0);
    const float behind[3] = {0, 0, -10};  // straight behind: the bottom edge
    e = P::ToScreenOrEdge(view, proj, behind, 800, 600, 20);
    assert(!e.on_screen && Near(e.x, 400) && Near(e.y, 580));
    const float below[3] = {0, 50, 10};  // far down: the bottom edge
    e = P::ToScreenOrEdge(view, proj, below, 800, 600, 20);
    assert(!e.on_screen && Near(e.y, 580) && e.x >= 20 && e.x <= 780);
    const float nan_p[3] = {NAN, 0, 1};
    e = P::ToScreenOrEdge(view, proj, nan_p, 800, 600, 20);
    assert(!e.on_screen && std::isfinite(e.x) && std::isfinite(e.y));
}

int main() {
    TestJson();
    TestRate();
    TestBoard();
    TestAim();
    TestEdge();
    std::printf("party-ping-test: ok\n");
    return 0;
}
