// SPDX-License-Identifier: GPL-3.0-or-later
// Party HUD board (gpu/shim/party/party_hud.cpp): who shares this player's world, the companions
// from the roster + VITALS + local PlayerIns reads, same-map distance / bearing, the camera
// projection, the publish / read hand-over.
// Build and run: cmake --build out/gpu --target party-hud-test && out/gpu/party-hud-test.exe
#include "party/party_hud.h"

#include <cassert>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace party;
using party::hud::Board;
using party::hud::ComposeIn;

static RosterEntry Entry(int slot, const char* name, MemberState st, std::uint32_t map, bool connected = true) {
    RosterEntry e;
    e.slot = slot;
    e.name = name;
    e.state = st;
    e.map_id = map;
    e.connected = connected;
    return e;
}

static bool Near(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) < eps;
}

static void SameWorldRules() {
    using MS = MemberState;
    // The host shares its world with summoned guests only.
    assert(hud::SameWorld(0, MS::Home, 1, MS::InHostWorld));
    assert(!hud::SameWorld(0, MS::Home, 1, MS::Home));
    assert(!hud::SameWorld(0, MS::Loading, 1, MS::InHostWorld));
    assert(!hud::SameWorld(0, MS::Prologue, 1, MS::InHostWorld));
    // A summoned guest: the host and the other summoned guests.
    assert(hud::SameWorld(1, MS::InHostWorld, 0, MS::Home));
    assert(hud::SameWorld(1, MS::InHostWorld, 2, MS::InHostWorld));
    assert(!hud::SameWorld(1, MS::InHostWorld, 2, MS::Home));
    assert(!hud::SameWorld(1, MS::InHostWorld, 0, MS::Loading));
    // A guest at home: nobody.
    assert(!hud::SameWorld(1, MS::Home, 0, MS::Home));
    assert(!hud::SameWorld(1, MS::Home, 2, MS::InHostWorld));
    assert(!hud::SameWorld(1, MS::InHostWorld, 1, MS::InHostWorld));
}

static void ComposeBoard() {
    const std::uint32_t map = 0x18000000, other = 0x15010000;
    std::vector<RosterEntry> roster = {Entry(0, "Hunter0", MemberState::Home, map),
                                       Entry(1, "Hunter1", MemberState::InHostWorld, map),
                                       Entry(2, "Hunter2", MemberState::Home, other),
                                       Entry(3, "Hunter3", MemberState::Loading, 0, false)};
    MemberVitals v[3];
    v[0].slot = 0;  // the host's own VITALS (what a guest gets relayed)
    v[0].flags = kVitalsValid | kVitalsAlive | kVitalsHasPos;
    v[0].hp = 500;
    v[0].max_hp = 1000;
    v[0].map_id = map;
    v[0].pos[0] = 10;
    v[0].age_ms = 200;
    v[1].slot = 2;
    v[1].flags = kVitalsValid | kVitalsHasPos;  // dead in its own world
    v[1].hp = 0;
    v[1].max_hp = 800;
    v[1].stamina = 0;
    v[1].max_stamina = 90;
    v[1].map_id = other;
    v[1].pos[2] = 5;
    v[2].slot = 1;  // ourselves: never a companion
    v[2].flags = kVitalsValid | kVitalsAlive;
    v[2].hp = 1;
    v[2].max_hp = 1;
    hud::LocalChr chr;
    std::snprintf(chr.name, sizeof chr.name, "Hunter0");
    chr.have_hp = true;
    chr.hp = 420;
    chr.max_hp = 1000;
    chr.stamina = 50;
    chr.max_stamina = 100;
    chr.have_pos = true;
    chr.pos[0] = 3;
    chr.pos[2] = 4;
    chr.chr = 0x1234500;  // the overlay re-reads this PlayerIns every frame

    ComposeIn in;
    in.local_slot = 1;
    in.local_state = MemberState::InHostWorld;
    in.local_map = map;
    in.me_have_pos = true;
    in.roster = roster.data();
    in.roster_n = int(roster.size());
    in.vitals = v;
    in.vitals_n = 3;
    in.chrs = &chr;
    in.chrs_n = 1;
    Board b;
    hud::Compose(in, &b);
    assert(b.active && b.count == 3);
    // The host: in our world; its position from its PlayerIns here, HP from its VITALS (a remote
    // data module lags).
    const hud::Companion& h = b.c[0];
    assert(std::strcmp(h.name, "Hunter0") == 0 && h.slot == 0 && h.same_world && h.local_read);
    assert(h.have_hp && h.hp == 500 && h.max_hp == 1000 && h.alive && !h.have_stamina);
    assert(h.chr == 0x1234500 && b.c[1].chr == 0);
    assert(h.have_pos && h.same_map && Near(h.pos[0], 3) && h.age_ms == 0);
    assert(Near(hud::Distance(b.me_pos, h.pos), 5));
    // The other guest at home: from its VITALS, greyed (not our world), other map, dead.
    const hud::Companion& g = b.c[1];
    assert(g.slot == 2 && !g.same_world && !g.local_read && g.have_hp && !g.alive && g.max_hp == 800);
    assert(g.map_id == other && g.have_pos && !g.same_map);
    // A lost member: roster only, no position.
    const hud::Companion& l = b.c[2];
    assert(l.slot == 3 && !l.connected && !l.same_world && !l.have_hp && !l.have_pos);
    // E2 "name and ping": a guest's view of the host shows its own link's round trip (the host's
    // measure in its roster entry); another member shows the host's measure of that member.
    {
        std::vector<RosterEntry> r2 = roster;
        r2[1].ping_ms = 140;  // Hunter1 = this guest
        r2[2].ping_ms = 90;   // Hunter2
        ComposeIn in2 = in;
        in2.roster = r2.data();
        in2.roster_n = int(r2.size());
        Board b2;
        hud::Compose(in2, &b2);
        assert(b2.c[0].slot == 0 && b2.c[0].ping_ms == 140);
        assert(b2.c[1].slot == 2 && b2.c[1].ping_ms == 90);
    }

    // Without VITALS HP, the local read's HP counts.
    v[0].flags = kVitalsHasPos;
    hud::Compose(in, &b);
    assert(b.c[0].have_hp && b.c[0].hp == 420 && b.c[0].have_stamina && b.c[0].stamina == 50);
    v[0].flags = kVitalsValid | kVitalsAlive | kVitalsHasPos;
    // A chr that did not match by name changes nothing; without the local read the host comes
    // from its VITALS (same map: distance).
    std::snprintf(chr.name, sizeof chr.name, "Nobody");
    hud::Compose(in, &b);
    assert(!b.c[0].local_read && b.c[0].hp == 500 && b.c[0].age_ms == 200 && b.c[0].same_map);
    assert(Near(hud::Distance(b.me_pos, b.c[0].pos), 10));
    // Positions are not compared across maps, nor without our own.
    in.me_have_pos = false;
    hud::Compose(in, &b);
    assert(!b.c[0].same_map);
    in.me_have_pos = true;
    // Not across worlds either (live: "Hunter0 0.0 m" while both stood at the same lamp of one map,
    // each in its own world): this guest at home, the host on the same map at the same spot.
    in.local_state = MemberState::Home;
    std::memcpy(v[0].pos, in.me_pos, sizeof in.me_pos);
    hud::Compose(in, &b);
    assert(!b.c[0].same_world && b.c[0].map_id == map && b.c[0].have_pos && !b.c[0].same_map);
    in.local_state = MemberState::InHostWorld;
    hud::Compose(in, &b);
    assert(b.c[0].same_world && b.c[0].same_map);
}

static void Geometry() {
    const float o[3] = {0, 0, 0}, ahead[3] = {0, 0, 5}, right[3] = {5, 0, 0}, behind[3] = {0, 0, -5};
    // Forward +z, right +x.
    assert(Near(hud::Bearing(o, ahead, 0, 1, 1, 0), 0.0f));
    assert(Near(hud::Bearing(o, right, 0, 1, 1, 0), 3.14159265f / 2));
    assert(Near(std::fabs(hud::Bearing(o, behind, 0, 1, 1, 0)), 3.14159265f));
    // Identity view, 90 degree projection: a point ahead lands mid-screen, one up-right of it
    // right of the centre (and below it: ndc +y is down for a positive y scale).
    const float view[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const float proj[4] = {1, 1, 0, 0};
    float u = 0, v = 0, d = 0;
    assert(hud::Project(view, proj, ahead, &u, &v, &d) && Near(u, 0.5f) && Near(v, 0.5f) && Near(d, 5));
    const float p[3] = {2.5f, 2.5f, 5};
    assert(hud::Project(view, proj, p, &u, &v, &d) && Near(u, 0.75f) && Near(v, 0.75f));
    assert(!hud::Project(view, proj, behind, &u, &v, &d));
    // WorldToScreen: pixels on a 1280 x 720 display; off screen (in front) and behind.
    hud::ScreenPoint sp = hud::WorldToScreen(view, proj, p, 1280, 720);
    assert(sp.in_front && sp.visible && Near(sp.x, 960) && Near(sp.y, 540) && Near(sp.depth, 5));
    const float far_right[3] = {20, 0, 5};
    sp = hud::WorldToScreen(view, proj, far_right, 1280, 720);
    assert(sp.in_front && !sp.visible && sp.x > 1280);
    assert(!hud::WorldToScreen(view, proj, behind, 1280, 720).in_front);
    // Member colours: distinct per slot, cream outside 0..3.
    float c0[3], c1[3], cx[3];
    hud::MemberColour(0, c0);
    hud::MemberColour(1, c1);
    hud::MemberColour(9, cx);
    assert(!(Near(c0[0], c1[0]) && Near(c0[1], c1[1]) && Near(c0[2], c1[2])) && Near(cx[0], 0.96f));
}

// Overhead tags: only a live chr here, alive, on our map, from a fresh board (user report: health
// bars floating in midair).
static void Tags() {
    hud::Companion c;
    c.connected = c.same_world = c.local_read = c.have_pos = c.same_map = true;
    c.have_hp = true;
    c.hp = 100;
    c.max_hp = 100;
    const char* why = nullptr;
    assert(hud::TagVisible(c, 100, &why) && why[0] == 0);
    assert(!hud::TagVisible(c, hud::kTagFreshMs + 1, &why) && std::string(why) == "stale");
    hud::Companion d = c;
    d.local_read = false;  // only a network report: never a tag
    assert(!hud::TagVisible(d, 0, &why) && std::string(why) == "no chr here");
    d = c;
    d.alive = false;
    assert(!hud::TagVisible(d, 0, &why) && std::string(why) == "dead");
    d = c;
    d.local_dead = true;  // its chr here is a corpse
    assert(!hud::TagVisible(d, 0, &why) && std::string(why) == "dead");
    d = c;
    d.same_map = false;
    assert(!hud::TagVisible(d, 0, &why));
    d = c;
    d.connected = false;
    assert(!hud::TagVisible(d, 0, &why));
}

// Our tag's anchor height and layout (the game's plate: bar on the anchor, name above).
static void GamePlate() {
    assert(hud::kTagBarHeightM > 1.6f && hud::kTagBarHeightM < 2.1f);
    // Bar bottom on the anchor (y 100), 3 px bar, 2 px gap, 10 px text: text top 85; no bar: 90.
    assert(Near(hud::TagTextTop(100.0f, 10.0f, true, 3.0f, 2.0f), 85.0f));
    assert(Near(hud::TagTextTop(100.0f, 10.0f, false, 3.0f, 2.0f), 90.0f));
    // The plate scale: the game's measured size at every distance (fixed, as the game's own plate).
    assert(Near(hud::PlateScale(10.0f), 1.0f) && Near(hud::PlateScale(24.0f), 1.0f));
    assert(Near(hud::PlateScale(8.0f), 1.0f) && Near(hud::kPlateMaxScale, 1.0f));
    assert(Near(hud::PlateScale(5.0f), hud::kPlateMaxScale) && Near(hud::PlateScale(0.5f), hud::kPlateMaxScale));
    assert(Near(hud::PlateScale(0.0f), hud::kPlateMaxScale) && Near(hud::PlateScale(std::nanf("")), hud::kPlateMaxScale));
    // The tuned 833x469 plate (live, side by side with the game's): bar 83 px centred, 2 px, its
    // top on the anchor; caps 1 px on the bar ends, 2 px above to 1 px below; name ~9.8 px font,
    // baseline 6 px above the bar, 1 px in from the left cap.
    const hud::PlateLayout far = hud::LayoutPlate(336.0f, 139.0f, 833.0f, 469.0f, 20.0f);
    assert(Near(far.bar_x1 - far.bar_x0, 83.0f, 1.5f) && Near((far.bar_x0 + far.bar_x1) * 0.5f, 336.0f, 1.0f));
    assert(Near(far.bar_y0, 139.0f) && Near(far.bar_y1 - far.bar_y0, 2.0f));
    assert(Near(far.cap_w, 1.0f) && Near(far.cap_l, far.bar_x0) && Near(far.cap_r, far.bar_x1 - 1.0f));
    assert(Near(far.bar_y0 - far.cap_y0, 2.0f) && Near(far.cap_y1 - far.bar_y1, 1.0f));
    assert(Near(far.font, 9.85f, 0.1f) && Near(far.bar_y0 - far.name_baseline, 6.0f));
    assert(Near(far.name_x - far.bar_x0, 1.0f));
    // Near, the same size as far (the game's plate does not scale); 1080p: font ~22.7 px, bar 191 px.
    const hud::PlateLayout near = hud::LayoutPlate(336.0f, 139.0f, 833.0f, 469.0f, 5.0f);
    assert(Near(near.font / far.font, hud::kPlateMaxScale, 0.01f));
    assert(Near((near.bar_x1 - near.bar_x0) / (far.bar_x1 - far.bar_x0), hud::kPlateMaxScale, 0.05f));
    assert(Near(near.bar_y0 - near.name_baseline, far.bar_y0 - far.name_baseline, 0.01f));
    const hud::PlateLayout hd = hud::LayoutPlate(960.0f, 500.0f, 1920.0f, 1080.0f, 30.0f);
    assert(Near(hd.font, 22.7f, 0.2f) && Near(hd.bar_x1 - hd.bar_x0, 191.0f, 1.5f));
    // The live tune file (BB_PARTY_HUD_TUNE_FILE): key=value lines, colours 0..255, others ignored.
    hud::PlateParams p;
    assert(hud::ParsePlateTune("# c\nfont_h = 0.03\r\nbar_top=255,0,51\nbogus=1\nrim=x\n", &p) == 2);
    assert(Near(p.font_h, 0.03f) && Near(p.bar_top[0], 1.0f) && Near(p.bar_top[2], 0.2f) && Near(p.rim, 0.0f));
    assert(Near(hud::LayoutPlate(0, 0, 833.0f, 469.0f, 20.0f, p).font, 14.07f, 0.05f));
}

static void Handover() {
    hud::ResetForTest();
    Board b;
    hud::Read(&b);
    assert(!b.active && !hud::Shown());
    Board in;
    in.active = true;
    hud::Publish(in);
    assert(!hud::Shown());  // nobody to show
    in.count = 1;
    std::snprintf(in.c[0].name, sizeof in.c[0].name, "Hunter1");
    hud::Publish(in);
    hud::Read(&b);
    assert(hud::Shown() && b.count == 1 && b.seq == 2 && std::strcmp(b.c[0].name, "Hunter1") == 0);
    hud::Publish(Board{});
    assert(!hud::Shown());
}

static void StaleChr() {
    assert(!hud::StaleChrDue(false, 600, 100));             // near a report
    assert(!hud::StaleChrDue(true, 0, 100));                // still being built: (0, 0, 0) HP 0/0
    assert(!hud::StaleChrDue(true, 600, 1.0));              // a warp / re-creation settles within ~1 s
    assert(hud::StaleChrDue(true, 600, hud::kStaleChrHoldS));  // really left behind
}

static void GameMenuRule() {
    int f[hud::kFullscreenMenuFields] = {};
    assert(!hud::FullscreenMenuOpen(f, -1));
    assert(hud::FullscreenMenuOpen(f, 1));  // debug force on
    f[4] = 3;                               // +0x120 (e.g. the inventory)
    assert(hud::FullscreenMenuOpen(f, -1));
    assert(!hud::FullscreenMenuOpen(f, 0));  // debug force off
    assert(hud::FullscreenMenuOpen(f, 2));   // another value: as the fields say
    f[4] = 0;
    assert(!hud::FullscreenMenuOpen(f, 2));
    assert(hud::GameMenuUp(true, f, -1));    // the menu flow's open menu (inventory, warp menu)
    assert(!hud::GameMenuUp(false, f, -1));
    {
        hud::FlowMenuKind k;  // the inventory / warp menu: full-screen after the grace, a box inside keeps it
        assert(!k.Update(true, false, 10.0));
        assert(k.Update(true, false, 10.3));
        assert(k.Update(true, true, 11.0));
        assert(!k.Update(false, false, 12.0));
        // the lamp's YES/NO box: the flow menu first, the box 35 ms later: never full-screen
        assert(!k.Update(true, false, 20.0));
        assert(!k.Update(true, true, 20.035));
        assert(k.box() && !k.Update(true, true, 21.0));
        // the box closes, the flow menu stays (a menu opened from it): looked at again
        assert(!k.Update(true, false, 22.0));
        assert(k.Update(true, false, 22.3));
        assert(!k.Update(false, false, 23.0));
    }
    hud::SetGameMenu(true);
    assert(hud::GameMenu());
    hud::SetGameMenu(false);
    assert(!hud::GameMenu());
    hud::SetGameDialog(true);
    assert(hud::GameDialog() && !hud::GameMenu());
    hud::SetGameDialog(false);
    assert(!hud::GameDialog());
}

// 0.6.2: the performance HUD (bottom right) lifts above the game's area-name banner for a while
// after each arrival (a new local PlayerIns once the loading screen is gone) and map block change.
static void AreaBanner() {
    hud::AreaBannerWatch w;
    assert(!w.Update(false, 0, 0xffffffffu, 0.0));          // loading / title
    assert(w.Update(true, 0x1000, 0x18010000, 1.0));         // arrival: Central Yharnam
    assert(w.Update(true, 0x1000, 0x18010000, 10.9));
    assert(!w.Update(true, 0x1000, 0x18010000, 11.1));       // banner gone
    assert(w.Update(true, 0x1000, 0x18000000, 30.0));        // walked into Cathedral Ward (m24_00)
    assert(!w.Update(true, 0x1000, 0x18000000, 40.5));
    assert(!w.Update(true, 0x1000, 0xffffffffu, 41.0));      // unknown map: no new banner
    assert(!w.Update(true, 0x1000, 0x18000000, 41.1));       // ... nor when it reads again
    assert(!w.Update(false, 0x1000, 0x18000000, 50.0));      // a loading screen (lamp warp)
    assert(w.Update(true, 0x1000, 0x18000000, 60.0));        // back in the world: same map, same chr - an arrival
    assert(!w.Update(true, 0x1000, 0x18000000, 70.5));
    assert(w.Update(true, 0x2000, 0x18000000, 80.0));        // a re-created PlayerIns (join)
    assert(!w.Update(true, 0, 0x18000000, 81.0));            // no player: nothing
    // The HUD's bottom edge: the corner normally; at most 80 % of the height while the banner may
    // be up (the banner spans ~82 .. 91 %).
    assert(Near(hud::PerfHudBottom(1068.0f, 1080.0f, false), 1068.0f));
    assert(Near(hud::PerfHudBottom(1068.0f, 1080.0f, true), 864.0f));
    assert(Near(hud::PerfHudBottom(500.0f, 1080.0f, true), 500.0f));  // already above it
    assert(Near(hud::PerfHudBottom(2148.0f, 2160.0f, true), 1728.0f));
    assert(hud::PerfHudBottom(1068.0f, 1080.0f, true) < 0.82f * 1080.0f);
}

int main() {
    AreaBanner();
    GameMenuRule();
    StaleChr();
    Tags();
    GamePlate();
    SameWorldRules();
    ComposeBoard();
    Geometry();
    Handover();
    std::printf("party hud: ok\n");
    return 0;
}
