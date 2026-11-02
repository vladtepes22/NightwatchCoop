// SPDX-License-Identifier: GPL-3.0-or-later
// Unit test of the navmesh test walker (gpu/shim/party/nav_walk.h): planner, funnel, stick math, pure pursuit,
// replan and stall recovery on synthetic meshes. ninja -C out/gpu nav-walk-test
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>

#include "party/nav_walk.h"

using namespace navwalk;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

// A grid of 2 m squares over nx x nz cells, minus the blocked ones.
static NavMesh Grid(int nx, int nz, bool (*blocked)(int, int)) {
    std::vector<std::vector<V3>> polys;
    for (int i = 0; i < nx; ++i)
        for (int j = 0; j < nz; ++j) {
            if (blocked && blocked(i, j)) continue;
            auto v = [&](int a, int b) { return V3{float(a * 2), 0.0f, float(b * 2)}; };
            polys.push_back({v(i, j), v(i, j + 1), v(i + 1, j + 1), v(i + 1, j)});
        }
    NavMesh m;
    m.FromPolys(polys);
    return m;
}

static bool WallWithGap(int i, int j) { return i == 5 && j < 17; }
static bool Island(int i, int) { return i == 5; }
static bool ShortWall(int i, int j) { return i == 4 && j < 8; }

// Kinematic player: moves 4 m/s along cam_off + stick angle, scaled by the tilt. Optionally blocked.
struct Sim {
    V3 pos;
    double cam_off;
    double speed = 4.0;
    bool blocked = false;
    void Step(const WalkOut& o, double dt) {
        const double lx = (o.stick.lx - 128) / 127.0, ly = -(o.stick.ly - 128) / 127.0;
        const double tilt = std::hypot(lx, ly);
        if (tilt < 0.05 || blocked) return;
        const double yaw = cam_off + std::atan2(lx, ly);
        pos.x += float(std::sin(yaw) * speed * tilt * dt);
        pos.z += float(std::cos(yaw) * speed * tilt * dt);
    }
};

static void TestPlan() {
    NavMesh m = Grid(10, 10, nullptr);
    std::vector<V3> p;
    PlanInfo pi;
    CHECK(m.Plan({1, 0, 1}, {17, 0, 17}, &p, &pi) == PlanResult::Ok);
    CHECK(p.size() <= 6);
    CHECK(pi.length < std::sqrt(2.0) * 16 * 1.04);
    p.clear();
    CHECK(m.Plan({1, 0, 1}, {50, 0, 50}, &p, &pi) == PlanResult::GoalOff);
    CHECK(m.Plan({-30, 0, 1}, {5, 0, 5}, &p, &pi) == PlanResult::StartOff);
    p.clear();
    CHECK(m.Plan({1, 0, 1}, {21.5f, 0, 10}, &p, &pi) == PlanResult::Ok);
    CHECK(pi.snap_goal > 1.4 && pi.snap_goal < 1.6);
    CHECK(std::fabs(p.back().x - 20) < 0.01);
    std::vector<std::vector<V3>> two = {{{0, 0, 0}, {0, 0, 4}, {4, 0, 4}, {4, 0, 0}},
                                        {{0, 10, 0}, {0, 10, 4}, {4, 10, 4}, {4, 10, 0}}};
    NavMesh t;
    t.FromPolys(two);
    CHECK(t.FaceAt({2, 9.5f, 2}) == 1);
    CHECK(t.FaceAt({2, 0.4f, 2}) == 0);
    CHECK(t.FaceAt({2, 5, 2}) == -1);
}

static void TestFunnelDetour() {
    NavMesh m = Grid(10, 10, ShortWall);
    std::vector<V3> p;
    PlanInfo pi;
    CHECK(m.Plan({1, 0, 1}, {19, 0, 1}, &p, &pi) == PlanResult::Ok);
    CHECK(p.size() >= 3);
    CHECK(pi.length > 18 + 8);
    bool top = false;
    for (const V3& v : p) top |= v.z >= 15.5f;
    CHECK(top);
    for (const V3& v : p) CHECK(!(v.x > 8.01f && v.x < 9.99f && v.z < 15.99f));
    bool corner = false;
    for (const V3& v : p)
        if (v.z > 16.3f && v.z < 17.0f) corner = true;
    CHECK(corner);  // the agent margin keeps it off the wall's end
}

static void TestSmooth() {
    NavMesh m = Grid(10, 10, ShortWall);
    // a zig-zag over open floor collapses to the straight line; around the wall's end the corner stays
    std::vector<V3> z = {{1, 0, 1}, {4, 0, 5}, {3, 0, 3}, {6, 0, 4}, {5, 0, 10}, {7, 0, 4}, {10, 0, 6}};
    std::vector<V3> o = m.Smooth(z);
    CHECK(o.size() <= 3);
    std::vector<V3> around = {{1, 0, 1}, {7, 0, 18}, {9, 0, 18}, {19, 0, 1}};
    o = m.Smooth(around);
    CHECK(o.size() >= 3);
    CHECK(!m.Walkable({7, 0, 2}, {11, 0, 2}));  // through the wall
    CHECK(m.Walkable({1, 0, 1}, {7, 0, 18}));
    NavMesh g = Grid(10, 10, nullptr);
    CHECK(!g.Walkable({1, 0, 1}, {1, 0, 23}));  // off the mesh
    CHECK(!g.Walkable({0.2f, 0, 5}, {0.2f, 0, 9}));  // closer than the clearance to the edge
}

static void TestIsland() {
    NavMesh m = Grid(10, 10, Island);
    std::vector<V3> p;
    CHECK(m.Plan({1, 0, 1}, {17, 0, 5}, &p) == PlanResult::NoRoute);
}

static void TestStick() {
    Stick s = MakeStick(0, 1);
    CHECK(s.lx == 128 && s.ly == 1);
    s = MakeStick(kPi / 2, 1);
    CHECK(s.lx == 255 && s.ly == 128);
    s = MakeStick(kPi, 0.5);
    CHECK(s.lx == 128 && s.ly == 128 + 64);
    CamModel c;
    c.FromCamera({0, 0, -5}, {0, 0, 0});
    CHECK(c.have && std::fabs(c.off) < 1e-9);
    CHECK(std::fabs(c.StickAngle(kPi / 2) - kPi / 2) < 1e-9);
    c.FromCamera({-5, 0, 0}, {0, 0, 0});
    CHECK(std::fabs(c.off - kPi / 2) < 1e-9);
    CHECK(std::fabs(c.StickAngle(kPi / 2)) < 1e-9);
    CHECK(std::fabs(c.StickAngle(-3 * kPi / 4) - Wrap(-3 * kPi / 4 - kPi / 2)) < 1e-9);
    c.FromForward(-0.732, -0.681);  // a real block: yaw -133 degrees
    CHECK(std::fabs(c.off * 180 / kPi + 132.9) < 0.1);
    c.FromForward(0.0, 0.05);  // looking straight down: ignored
    CHECK(std::fabs(c.off * 180 / kPi + 132.9) < 0.1);
    CamModel o;
    o.Observe(0.2, 1.0);
    o.Observe(0.2, 1.0);
    CHECK(o.have && std::fabs(o.off - 0.8) < 1e-6);
    // a constant 150 degree disagreement between the matrix and the motion is learned, and the swings still come
    // from the matrix: off = raw + resid
    CamModel w;
    w.FromForward(0.0, 1.0);  // raw 0
    for (int i = 0; i < 40; ++i) w.Observe(0.0, 2.6, 4.0);  // the motion says 2.6 rad
    CHECK(w.mismatch && std::fabs(w.resid - 2.6) < 0.05);
    w.FromForward(1.0, 0.0);  // the camera swings 90 degrees: off follows at once
    CHECK(std::fabs(Wrap(w.off - (kPi / 2 + 2.6))) < 0.05);
    for (int i = 0; i < 40; ++i) w.Observe(0.0, kPi / 2, 4.0);  // and when the motion agrees with the matrix again
    CHECK(!w.mismatch && w.recovered == 1 && std::fabs(w.resid) < 0.45);
}

static void TestFollower() {
    PathFollower f;
    f.Set({{0, 0, 0}, {10, 0, 0}, {10, 0, 10}});
    CHECK(std::fabs(f.Total() - 20) < 1e-9);
    f.Update({3, 0, 1});
    CHECK(std::fabs(f.Progress() - 3) < 1e-6 && std::fabs(f.Cross() - 1) < 1e-6);
    V3 l = f.Lookahead(9);
    CHECK(std::fabs(l.x - 10) < 1e-5 && std::fabs(l.z - 2) < 1e-5);
    f.Update({2, 0, 0});
    CHECK(f.Progress() >= 3 - 1e-6);
    f.Update({10, 0, 9}, 30);
    CHECK(f.Progress() > 18.9);
}

struct RunResult {
    bool done = false, failed = false;
    double t = 0;
    int warps = 0;
    int zero_ticks = 0;
};

static RunResult Run(Walker& w, Sim& sim, bool use_cam, double limit = 120, int block_from = -1, int block_to = -1) {
    RunResult r;
    const double dt = 0.05;
    double now = 0;
    int tick = 0;
    for (; now < limit; now += dt, ++tick) {
        if (use_cam)
            w.cam().FromCamera({float(sim.pos.x - std::sin(sim.cam_off) * 6), 3, float(sim.pos.z - std::cos(sim.cam_off) * 6)},
                               sim.pos);
        sim.blocked = tick >= block_from && tick < block_to;
        WalkOut o = w.Tick(sim.pos, now, dt);
        if (o.warp) {
            ++r.warps;
            sim.pos = o.warp_to;
            sim.pos.y -= 1;
        }
        if (o.state == WalkState::Walking && o.stick.lx == 128 && o.stick.ly == 128) ++r.zero_ticks;
        sim.Step(o, dt);
        if (w.state() == WalkState::Done) {
            r.done = true;
            break;
        }
        if (w.state() == WalkState::Failed) {
            r.failed = true;
            break;
        }
    }
    r.t = now;
    return r;
}

static void TestWalkOpen() {
    NavMesh m = Grid(20, 20, nullptr);
    for (double off : {0.0, 1.3, -2.5, 3.0}) {
        Walker w;
        Sim sim{{2, 0, 2}, off};
        CHECK(w.Start(&m, sim.pos, {{36, 0, 30}}, {true}, 1.0, 0) == PlanResult::Ok);
        const RunResult r = Run(w, sim, true);
        CHECK(r.done && r.warps == 0 && w.stats.replans == 0 && w.stats.backups == 0);
        CHECK(Dist2(sim.pos, {36, 0, 30}) <= 1.05);
        CHECK(r.zero_ticks == 0);
        CHECK(r.t < 16);
    }
}

static void TestWalkDetourWithoutCamera() {
    NavMesh m = Grid(12, 20, WallWithGap);
    Walker w;
    Sim sim{{3, 0, 3}, 2.2};
    CHECK(w.Start(&m, sim.pos, {{20, 0, 3}}, {true}, 1.0, 0) == PlanResult::Ok);
    const RunResult r = Run(w, sim, false);
    CHECK(r.done);
    CHECK(r.warps == 0);
    CHECK(w.cam().have);
}

static void TestWaypointHints() {
    NavMesh m = Grid(12, 20, WallWithGap);
    Walker w;
    Sim sim{{3, 0, 3}, 0.4};
    CHECK(w.Start(&m, sim.pos, {{9, 0, 36}, {20, 0, 36}, {20, 0, 3}}, {false, false, false}, 1.0, 0) == PlanResult::Ok);
    CHECK(w.stats.path_len > 60);
    // every route waypoint (known walkable) stays on the path: the shortcut never skips one
    for (const V3& g : std::vector<V3>{{9, 0, 36}, {20, 0, 36}}) {
        double best = 1e9;
        for (const V3& p : w.path().Points()) best = std::min(best, Dist2(p, g));
        CHECK(best < 0.5);
    }
    const RunResult r = Run(w, sim, true);
    CHECK(r.done && r.warps == 0);
}

static void TestUnreachable() {
    NavMesh m = Grid(12, 20, Island);
    Walker w;
    Sim sim{{3, 0, 3}, 0};
    CHECK(w.Start(&m, sim.pos, {{20, 0, 3}}, {true}, 1.0, 0) == PlanResult::NoRoute);
    CHECK(w.state() == WalkState::Failed && !w.fail_reason.empty());
    Walker w2;
    CHECK(w2.Start(&m, sim.pos, {{200, 0, 3}}, {true}, 1.0, 0) == PlanResult::GoalOff);
}

static void TestOffMeshTail() {
    NavMesh m = Grid(20, 20, nullptr);  // 40 x 40 m
    {
        Walker w;
        Sim sim{{2, 0, 2}, 0.3};
        // a route whose last waypoint is a lift car 6 m off the mesh
        CHECK(w.Start(&m, sim.pos, {{38, 0, 20}, {46, 0, 20}}, {true, true}, 1.0, 0, true, true) == PlanResult::Ok);
        const RunResult r = Run(w, sim, true);
        CHECK(r.done && r.warps == 0);
        CHECK(Dist2(sim.pos, {46, 0, 20}) <= 1.05);
    }
    {
        Walker w;
        Sim sim{{2, 0, 2}, 0.3};
        CHECK(w.Start(&m, sim.pos, {{46, 0, 20}}, {true}, 1.0, 0, true, false) == PlanResult::GoalOff);  // a single goal 6 m off: early failure
    }
    {
        // a stick goal 1.5 m off the mesh (a lift trigger): walked to, to within the radius
        Walker w;
        Sim sim{{2, 0, 2}, 0.3};
        CHECK(w.Start(&m, sim.pos, {{41.5f, 0, 20}}, {true}, 0.3, 0, true, false) == PlanResult::Ok);
        const RunResult r = Run(w, sim, true);
        CHECK(r.done && Dist2(sim.pos, {41.5f, 0, 20}) <= 0.35);
    }
}

static void TestCornerAim() {
    NavMesh m = Grid(10, 10, ShortWall);  // the wall x 8..10 (m), z 0..16
    Walker w;
    Sim sim{{7, 0, 12}, 0.0};
    CHECK(w.Start(&m, sim.pos, {{14, 0, 2}}, {true}, 1.0, 0) == PlanResult::Ok);
    double now = 0;
    for (int i = 0; i < 400 && w.state() == WalkState::Walking; ++i, now += 0.05) {
        w.cam().FromForward(0, 1);
        WalkOut o = w.Tick(sim.pos, now, 0.05);
        // every aim point is reachable in a straight line over the mesh (never across the wall)
        if (o.state == WalkState::Walking) {
            for (int k = 1; k < 10; ++k) {  // the aim line stays out of the wall block (x 8..10, z 0..16)
                const double x = sim.pos.x + (w.last_target().x - sim.pos.x) * k / 10.0;
                const double z = sim.pos.z + (w.last_target().z - sim.pos.z) * k / 10.0;
                CHECK(!(x > 8.05 && x < 9.95 && z < 15.95));
            }
        }
        sim.Step(o, 0.05);
    }
    CHECK(w.state() == WalkState::Done);
}

static void TestReplanKeepsDirection() {
    NavMesh m = Grid(20, 20, nullptr);
    Walker w;
    Sim sim{{2, 0, 2}, 0.0};
    CHECK(w.Start(&m, sim.pos, {{10, 0, 2}, {20, 0, 2}, {30, 0, 2}, {38, 0, 2}}, {true, true, true, true}, 1.0, 0) == PlanResult::Ok);
    double now = 0;
    int tick = 0;
    double len_at_stall = -1;
    for (; tick < 600 && w.state() == WalkState::Walking; ++tick, now += 0.05) {
        w.cam().FromForward(0, 1);
        sim.blocked = sim.pos.x > 24 && sim.pos.x < 25.5 && w.stats.replans == 0;  // stuck at x 24 for a while
        WalkOut o = w.Tick(sim.pos, now, 0.05);
        if (w.stats.replans == 1 && len_at_stall < 0) len_at_stall = w.path().Total();
        sim.Step(o, 0.05);
    }
    CHECK(w.state() == WalkState::Done);
    CHECK(len_at_stall > 0 && len_at_stall < 16.5);  // from x 24 to 38 (14 m), not back through the passed goals
}

static void TestPause() {
    NavMesh m = Grid(20, 20, nullptr);
    Walker w;
    Sim sim{{2, 0, 2}, 0.0};
    CHECK(w.Start(&m, sim.pos, {{36, 0, 30}}, {true}, 1.0, 0) == PlanResult::Ok);
    w.Tick(sim.pos, 0.0, 0.05);
    w.Tick(sim.pos, 0.05, 0.05);
    WalkOut o = w.Tick(sim.pos, 20.0, 0.25);  // 20 s with no tick (a load): not a stall, not a timeout
    CHECK(w.stats.stalls == 0 && w.state() == WalkState::Walking);
    o = w.Tick(sim.pos, 20.05, 0.05);
    CHECK(w.stats.stalls == 0);
}

static void TestStallRecovery() {
    NavMesh m = Grid(20, 20, nullptr);
    {
        Walker w;
        Sim sim{{2, 0, 2}, 0.5};
        CHECK(w.Start(&m, sim.pos, {{36, 0, 30}}, {true}, 1.0, 0) == PlanResult::Ok);
        const RunResult r = Run(w, sim, true, 120, 60, 130);
        CHECK(r.done);
        CHECK(w.stats.stalls >= 1);
        CHECK(r.warps == 0);
    }
    {
        Walker w;
        Sim sim{{2, 0, 2}, 0.5};
        CHECK(w.Start(&m, sim.pos, {{36, 0, 30}}, {true}, 1.0, 0) == PlanResult::Ok);
        const RunResult r = Run(w, sim, true, 200, 60, 1000000);
        CHECK(r.failed);
        CHECK(r.warps == w.cfg.max_warps && w.stats.warps == w.cfg.max_warps);
        CHECK(w.stats.backups >= 2 && w.stats.rolls >= 1);
        CHECK(w.fail_reason == "blocked: no way past");
    }
}

static void TestOffCorridorReplan() {
    NavMesh m = Grid(20, 20, nullptr);
    Walker w;
    Sim sim{{2, 0, 2}, 0.0};
    CHECK(w.Start(&m, sim.pos, {{36, 0, 4}}, {true}, 1.0, 0) == PlanResult::Ok);
    w.Tick(sim.pos, 0.0, 0.05);
    sim.pos = {10, 0, 25};
    w.Tick(sim.pos, 0.05, 0.05);
    CHECK(w.stats.replans == 1);
    const RunResult r = Run(w, sim, true);
    CHECK(r.done && r.warps == 0);
}

static void TestTurnRateAndEase() {
    NavMesh m = Grid(20, 20, nullptr);
    Walker w;
    Sim sim{{2, 0, 20}, 0.0};
    CHECK(w.Start(&m, sim.pos, {{36, 0, 20}}, {true}, 0.5, 0) == PlanResult::Ok);
    double now = 0, last = 0, max_step = 0;
    bool first = true;
    int slow = 0;
    for (int i = 0; i < 400 && w.state() == WalkState::Walking; ++i, now += 0.05) {
        w.cam().FromCamera({sim.pos.x, 3, sim.pos.z - 6}, sim.pos);
        WalkOut o = w.Tick(sim.pos, now, 0.05);
        const double a = std::atan2(o.stick.lx - 128.0, -(o.stick.ly - 128.0));
        if (!first && o.state == WalkState::Walking) max_step = std::max(max_step, std::fabs(Wrap(a - last)));
        first = false;
        last = a;
        if (std::hypot(o.stick.lx - 128, o.stick.ly - 128) < 100) ++slow;
        sim.Step(o, 0.05);
    }
    CHECK(max_step <= 4.5 * 0.05 + 0.05);
    CHECK(slow > 0);
    CHECK(w.state() == WalkState::Done);
}

int main() {
    TestPlan();
    TestFunnelDetour();
    TestSmooth();
    TestIsland();
    TestStick();
    TestFollower();
    TestWalkOpen();
    TestWalkDetourWithoutCamera();
    TestWaypointHints();
    TestUnreachable();
    TestOffMeshTail();
    TestPause();
    TestReplanKeepsDirection();
    TestCornerAim();
    TestStallRecovery();
    TestOffCorridorReplan();
    TestTurnRateAndEase();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("nav-walk-test: ok\n");
    return 0;
}
