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
    // C5: a spectating member reports its body's spot; the chr matched by its name here is the copy
    // carried along with the member it follows (the panel showed "0.2 m"): not read.
    std::snprintf(chr.name, sizeof chr.name, "Hunter0");
    v[0].flags = kVitalsValid | kVitalsHasPos | kVitalsSpectating;
    v[0].hp = 0;
    v[0].pos[0] = 30;
    hud::Compose(in, &b);
    assert(b.c[0].spectating && !b.c[0].local_read && b.c[0].chr == 0 && !b.c[0].alive);
    assert(Near(b.c[0].pos[0], 30) && b.c[0].have_pos && b.c[1].spectating == false);
    v[0].flags = kVitalsValid | kVitalsAlive | kVitalsHasPos;  // up again: the local read as before
    v[0].hp = 500;
    hud::Compose(in, &b);
    assert(!b.c[0].spectating && b.c[0].local_read && Near(b.c[0].pos[0], 3));
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

// Tags behind this player's own character (user decision 2026-10-21, arenaguest A1: three in a
// line, the near member's tag on our own back): hidden; beside us, in front of us or close behind
// us (within kOwnCoverDepthM): shown. Identity view, 90 degree projection, a 1280 x 720 display.
static void OwnBodyCover() {
    const float view[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const float proj[4] = {1, 1, 0, 0};
    const auto box = [&](float x, float z, float half) {
        const float feet[3] = {x, -0.9f, z};
        return hud::BodyBox(view, proj, feet, 1280, 720, half);
    };
    const hud::ScreenBox me = box(0, 3, hud::kOwnBodyHalfWidth);
    assert(me.valid && Near(me.y0, 252) && Near(me.y1, 462) && Near(me.x0, 598) && Near(me.x1, 682) && Near(me.depth, 3));
    const auto alpha = [&](float x, float z) { return hud::OwnBodyTagAlpha(box(x, z, hud::kMemberBodyHalfWidth), me); };
    assert(Near(alpha(0, 8), 0.0f));     // straight behind us: hidden
    assert(Near(alpha(0.4f, 8), 0.0f));  // still all behind our coat
    const float half_out = alpha(0.5f, 8);  // 57 % covered: fading
    assert(half_out > 0.5f && half_out < 0.8f);
    assert(Near(alpha(0.6f, 8), 1.0f));  // mostly beside us: shown
    assert(Near(alpha(3, 8), 1.0f));     // beside us: shown
    assert(Near(alpha(0, 3.3f), 1.0f));  // right behind our back (as close as a hug): shown
    assert(Near(alpha(0, 2), 1.0f));     // in front of us: shown
    assert(Near(alpha(0, 60), 0.0f));    // far behind us, small: hidden too
    assert(!box(0, -2, hud::kMemberBodyHalfWidth).valid && Near(alpha(0, -2), 1.0f));  // behind the camera
    assert(Near(hud::OwnBodyTagAlpha(box(0, 8, hud::kMemberBodyHalfWidth), hud::ScreenBox{}), 1.0f));      // our position unknown
    hud::ScreenBox a, b;
    a.valid = b.valid = true;
    a.x1 = a.y1 = 10;
    b.x0 = 5;
    b.x1 = b.y1 = 20;
    assert(Near(hud::CoveredFraction(a, b), 0.5f) && Near(hud::CoveredFraction(b, a), 50.0f / 300.0f));
    b.x0 = 10;
    assert(Near(hud::CoveredFraction(a, b), 0.0f));
}

// Overhead tags: only a live chr here, alive, on our map, from a fresh board (user report: health
// bars floating in midair).
// The overhead health bar: only "Name and health" with data; red while PvP is in force.
static void OverheadBar() {
    assert(hud::OverheadBarShown(3, true, 100));
    assert(!hud::OverheadBarShown(1, true, 100));
    assert(!hud::OverheadBarShown(3, false, 100));
    assert(!hud::OverheadBarShown(3, true, 0));
    assert(hud::OverheadBarRed(true) && !hud::OverheadBarRed(false));
    assert(!hud::GameEnemyBarHidden(1000, 0));      // no bar of ours: the game's stays (tags off)
    assert(hud::GameEnemyBarHidden(1000, 800));     // drawn this frame or two ago: hidden
    assert(!hud::GameEnemyBarHidden(1000, 600));    // tags went off 400 ms ago: back
}

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

// Party notices / alerts: lines wrapped at spaces inside the box, each centred.
static void NoticeLayout() {
    // A fixed advance per character (UTF-8 aware: a Cyrillic letter is 2 bytes, one advance).
    const auto measure = [](const std::string& s) {
        float n = 0;
        for (unsigned char c : s) n += (c & 0xC0) != 0x80 ? 10.0f : 0.0f;
        return n;
    };
    using hud::WrapNoticeLines;
    // Short: one line, unchanged.
    std::vector<std::string> l = WrapNoticeLines("The host is in a Chalice Dungeon", 1000, measure);
    assert(l.size() == 1 && l[0] == "The host is in a Chalice Dungeon");
    // The box's own breaks stay; each paragraph wraps at spaces, never past the width.
    l = WrapNoticeLines("Party: the host did not come back within 2 min\nYou play on alone in your own world.", 200,
                        measure);
    assert(l.size() >= 4);
    for (const std::string& s : l) assert(measure(s) <= 200 && !s.empty() && s.front() != ' ' && s.back() != ' ');
    assert(l[0] == "Party: the host did");
    // No word is lost or split.
    std::string joined;
    for (const std::string& s : l) joined += (joined.empty() ? "" : " ") + s;
    assert(joined == "Party: the host did not come back within 2 min You play on alone in your own world.");
    // A word wider than the box keeps its own line (never cut).
    l = WrapNoticeLines("a Supercalifragilistic b", 50, measure);
    assert(l.size() == 3 && l[1] == "Supercalifragilistic");
    // Russian: split only at spaces, so no UTF-8 sequence is cut; every line within the box.
    const std::string ru = "\xD0\xA5\xD0\xBE\xD1\x81\xD1\x82 \xD0\xB2 \xD0\x9F\xD0\xBE\xD0\xB4\xD0\xB7\xD0\xB5\xD0\xBC"
                           "\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xB5 \xD0\xA7\xD0\xB0\xD1\x88\xD0\xB8";  // Хост в Подземелье Чаши
    l = WrapNoticeLines(ru, 100, measure);
    assert(l.size() == 3);
    for (const std::string& s : l) {
        assert(measure(s) <= 100 || s.find(' ') == std::string::npos);
        assert((static_cast<unsigned char>(s[0]) & 0xC0) != 0x80);  // starts on a character
    }
    // Empty text: one empty line; an empty paragraph stays as a gap.
    assert(WrapNoticeLines("", 100, measure).size() == 1);
    l = WrapNoticeLines("a\n\nb", 100, measure);
    assert(l.size() == 3 && l[1].empty());
    // Centring: the middle of the box; a too-wide line starts at the box's left edge.
    assert(Near(hud::CenteredLineX(10, 200, 100), 60));
    assert(Near(hud::CenteredLineX(10, 200, 200), 10));
    assert(Near(hud::CenteredLineX(10, 200, 260), 10));
    // Every wrapped line of the long Chalice notice centres inside a 712 px box.
    l = WrapNoticeLines("The host is in a Chalice Dungeon: Party > Rejoin to go back", 330, measure);
    for (const std::string& s : l) {
        const float x = hud::CenteredLineX(24, 330, measure(s));
        assert(x >= 24 && x + measure(s) <= 24 + 330 + 0.01f);
        assert(Near((x - 24) * 2 + measure(s), 330));  // equal margins
    }
}

// The party panel and the notices keep clear of the game's boss health bar (bbport_overlay.cpp
// PartyHud / PartyToast: the same steps with the overlay's sizes) at every output we support.
static void BossBarLayout() {
    using hud::HudRect;
    // Measured (relWL s1 shot, 833 x 469, Father Gascoigne): name from y 385, bar to y 411,
    // x 142 .. 697. The area holds it with a margin.
    {
        const HudRect stage = hud::HudStage(833, 469, 16.0f / 9.0f);
        assert(Near(stage.x0, 0) && Near(stage.x1, 833) && Near(stage.y1 - stage.y0, 833 * 9.0f / 16.0f, 0.01f));
        const HudRect a = hud::BossBarArea(stage, 1);
        assert(a.x0 < 142 && a.x1 > 697 && a.y0 < 385 && a.y1 > 411);
        assert(a.x0 > 120 && a.y0 > 365);  // not much more: the corner stays free
        // Three bars stack higher; never above the stage.
        const HudRect a3 = hud::BossBarArea(stage, 3);
        assert(a3.y0 < a.y0 - 50 && Near(a3.y1, a.y1) && a3.y0 > 0);
        assert(hud::BossBarArea(stage, 40).y0 >= stage.y0);
    }
    // 21:9: the HUD stage is the 16:9 middle at full height; the bar starts right of the old corner.
    {
        const HudRect stage = hud::HudStage(2560, 1080, 2560.0f / 1080.0f);
        assert(Near(stage.x0, 320) && Near(stage.x1, 2240) && Near(stage.y0, 0) && Near(stage.y1, 1080));
    }
    // 16:10 display, a 16:9 frame letterboxed: the stage is the frame (bars above and below).
    {
        const HudRect stage = hud::HudStage(1920, 1200, 16.0f / 9.0f);
        assert(Near(stage.x0, 0) && Near(stage.y0, 60) && Near(stage.y1, 1140));
        // A 21:9 frame in a 16:9 window: letterboxed, the stage its 16:9 middle.
        const HudRect st2 = hud::HudStage(1920, 1080, 21.0f / 9.0f);
        assert(st2.y0 > 0 && Near(st2.y1 - st2.y0, 1920 * 9.0f / 21.0f, 0.01f) &&
               Near((st2.x1 - st2.x0) / (st2.y1 - st2.y0), 16.0f / 9.0f, 1e-3f));
    }
    // BottomClearOf: unchanged when clear, else just above the area.
    {
        const HudRect avoid{100, 800, 900, 900};
        assert(Near(hud::BottomClearOf(0, 90, 1000, 300, avoid, 6), 1000));    // left of it
        assert(Near(hud::BottomClearOf(0, 150, 1000, 50, avoid, 6), 1000));    // below it
        assert(Near(hud::BottomClearOf(0, 150, 1000, 150, avoid, 6), 794));   // reaches into it
        assert(Near(hud::BottomClearOf(0, 150, 790, 100, avoid, 6), 790));     // above it already
    }
    // Every output, 1-3 members, with and without the rot line, short and long notices, 1-3 bars:
    // the panel and the notice never touch the bar area, each other or the screen's top.
    struct Display {
        float w, h, frame_aspect;
    };
    const Display displays[] = {
        {833, 469, 16.0f / 9.0f},     {1280, 720, 16.0f / 9.0f},   {1920, 1080, 16.0f / 9.0f},
        {2560, 1440, 16.0f / 9.0f},   {3840, 2160, 16.0f / 9.0f},  {1680, 1050, 16.0f / 9.0f},
        {1920, 1200, 16.0f / 9.0f},   {2560, 1600, 16.0f / 9.0f},  {2560, 1080, 2560.0f / 1080.0f},
        {3440, 1440, 3440.0f / 1440.0f}, {3840, 1600, 3840.0f / 1600.0f}, {5120, 2160, 5120.0f / 2160.0f},
        {3840, 1080, 3840.0f / 1080.0f}, {1920, 1080, 2560.0f / 1080.0f},
    };
    int moved = 0, kept = 0;
    for (const Display& d : displays) {
        const float s = d.h / 1080.0f > 0.75f ? d.h / 1080.0f : 0.75f;  // the overlay's base_scale
        const HudRect stage = hud::HudStage(d.w, d.h, d.frame_aspect);
        for (int rows = 1; rows <= 3; ++rows) {
            const HudRect boss = hud::BossBarArea(stage, rows);
            for (int members = 1; members <= 3; ++members) {
                for (int rot = 0; rot < 2; ++rot) {
                    for (int chars : {20, 40, 70}) {  // "Joiners wait: you are in a boss fight" is 37
                        const float pad = 12 * s, gap = 6 * s;
                        // The panel: 210 wide + padding, ~30 per member, the short rot line.
                        const float pw = 230 * s, ph = members * 30 * s + 14 * s + rot * 20 * s;
                        float pb = hud::BottomClearOf(pad, pad + pw, d.h - pad, ph, boss, gap);
                        const HudRect panel{pad, pb - ph, pad + pw, pb};
                        assert(!hud::Overlaps(panel, boss) && panel.y0 >= 0);
                        (pb < d.h - pad ? moved : kept)++;
                        // The notice above the panel (19 px serif: ~10 px a character, 24 a line).
                        const float tw = chars * 10 * s + 32 * s, th = 24 * s + 24 * s;
                        const float tb = hud::BottomClearOf(pad, pad + tw, panel.y0 - gap, th, boss, gap);
                        const HudRect toast{pad, tb - th, pad + tw, tb};
                        assert(!hud::Overlaps(toast, boss) && !hud::Overlaps(toast, panel) && toast.y0 >= 0);
                    }
                }
            }
        }
    }
    // Small windows move the panel up; 1080p+ 16:9 and 21:9 mostly keep the corner.
    assert(moved > 0 && kept > moved);
}

void PingFormat() {
    char b[24];
    assert(party::hud::FormatPing(0, false, b, sizeof b) == 0 && b[0] == 0);
    party::hud::FormatPing(11, false, b, sizeof b);
    assert(std::strcmp(b, "11 ms") == 0);
    party::hud::FormatPing(250, true, b, sizeof b);
    {
        const unsigned char want[] = {0x32, 0x35, 0x30, 0x20, 0xd0, 0xbc, 0xd1, 0x81, 0};
        assert(std::strcmp(b, reinterpret_cast<const char*>(want)) == 0);
    }
    char tiny[4];
    party::hud::FormatPing(1234, false, tiny, sizeof tiny);
    assert(std::strlen(tiny) == 3);
    assert(party::hud::PingQuality(1) == 0 && party::hud::PingQuality(79) == 0);
    assert(party::hud::PingQuality(80) == 1 && party::hud::PingQuality(159) == 1);
    assert(party::hud::PingQuality(160) == 2 && party::hud::PingQuality(900) == 2);
}

int main() {
    PingFormat();
    BossBarLayout();
    NoticeLayout();
    AreaBanner();
    GameMenuRule();
    StaleChr();
    Tags();
    OverheadBar();
    OwnBodyCover();
    GamePlate();
    SameWorldRules();
    ComposeBoard();
    Geometry();
    Handover();
    std::printf("party hud: ok\n");
    return 0;
}
