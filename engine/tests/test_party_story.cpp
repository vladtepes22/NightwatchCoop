// SPDX-License-Identifier: GPL-3.0-or-later
// Party story (gpu/shim/party/party_story, phase C4), the parts without the game: the data
// tables (sex variants, endings, story flags, time of day), the EVENT "story" JSON round trip,
// the host queue and GuestStory's mirror / replay / ending programs with an injected game state.
// Build and run: ninja -C out/gpu party-story-test && out/gpu/party-story-test.exe
#include "party/party_story.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace coop;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                              \
    } while (0)

// G3: a guest's 2002[6] the host plays as its own (the Cathedral Ward door, day -> evening).
static void test_guest_phase_change() {
    StoryIntent s;
    s.kind = StoryKind::Cutscene;
    s.instr = 6;
    s.id = 24000020;
    s.tod = 1;
    s.warp_point = 2402200;
    s.warp_map = 0x18000000;  // m24_00
    const std::uint32_t m24_00 = 0x18000000, m24_01 = 0x18010000;
    std::string why;
    CHECK(GuestPhaseChangeOk(s, 0, m24_00, &why));
    CHECK(!GuestPhaseChangeOk(s, 4, m24_00, &why) && why.find("already") != std::string::npos);  // host at evening
    CHECK(!GuestPhaseChangeOk(s, 0, m24_01, &why) && why.find("map") != std::string::npos);  // host elsewhere
    StoryIntent t = s;
    t.tod = -1;
    CHECK(!GuestPhaseChangeOk(t, 0, m24_00, &why));
    t = s;
    t.instr = 4;
    CHECK(!GuestPhaseChangeOk(t, 0, m24_00, &why));
    t = s;
    t.id = 12345678;  // not a remo of the game
    CHECK(!GuestPhaseChangeOk(t, 0, m24_00, &why));
    t = s;
    t.warp_point = kStoryNone;
    CHECK(!GuestPhaseChangeOk(t, 0, m24_00, &why));
    t = s;
    t.tod = 3;
    CHECK(GuestPhaseChangeOk(t, 6, m24_00, &why));   // night -> Blood Moon
    CHECK(!GuestPhaseChangeOk(t, 7, m24_00, &why));  // Blood Moon already
    // D5 / D10 (pard4): Amelia's skull pressed by a guest: 2002[6] with no warp (point -1).
    StoryIntent k;
    k.kind = StoryKind::Cutscene;
    k.instr = 6;
    k.id = 24000030;
    k.tod = 2;
    k.event_id = 12401803;
    k.warp_point = kStoryNone;
    k.warp_map = 0x18000000;
    CHECK(GuestPressedHostOnlyEvent(12401803, 24000030));
    CHECK(!GuestPressedHostOnlyEvent(12401803, 24000020));
    CHECK(!GuestPressedHostOnlyEvent(12400750, 24000020));
    CHECK(GuestPhaseChangeOk(k, 4, m24_00, &why));    // evening -> night
    CHECK(!GuestPhaseChangeOk(k, 6, m24_00, &why));   // night already
    CHECK(!GuestPhaseChangeOk(k, 4, m24_01, &why));   // the host in another block
    k.event_id = 12345;
    CHECK(!GuestPhaseChangeOk(k, 4, m24_00, &why));   // no warp only for the known events
}

static void test_tables() {
    CHECK(RemoForSex(21000010, false) == 21000010);
    CHECK(RemoForSex(21000010, true) == 21001010);
    CHECK(RemoForSex(21001010, false) == 21000010);
    CHECK(RemoForSex(21000030, true) == 21001030);
    CHECK(RemoForSex(24010005, true) == 24011005);
    CHECK(RemoForSex(21000020, true) == 21000020); // B has no sex variant
    CHECK(RemoForSex(32000000, true) == 32000000);

    CHECK(EndingTypeOfEvent(12100180) == 1);
    CHECK(EndingTypeOfEvent(12100000) == 2);
    CHECK(EndingTypeOfEvent(12100002) == 3);
    CHECK(EndingTypeOfEvent(12101802) == 0);
    CHECK(EndingTypeOfRemo(21001010) == 1);
    CHECK(EndingTypeOfRemo(21000020) == 2);
    CHECK(EndingTypeOfRemo(21001030) == 3);
    CHECK(EndingTypeOfRemo(21000040) == 0);

    CHECK(StoryFlagsForRemo(32000000) == std::vector<std::uint32_t>{70002802});
    CHECK(StoryFlagsForRemo(28001040) == std::vector<std::uint32_t>{12800434});
    CHECK(StoryFlagsForRemo(24000030).empty());

    CHECK(TodFlagValue(0) == 0 && TodFlagValue(1) == 4 && TodFlagValue(2) == 6 && TodFlagValue(3) == 7);
    CHECK(TodFlagValue(4) == -1 && TodFlagValue(-1) == -1);
    CHECK(TodFromFlagValue(0) == 0 && TodFromFlagValue(4) == 1 && TodFromFlagValue(6) == 2 && TodFromFlagValue(7) == 3);

    StoryIntent s;
    s.kind = StoryKind::Cutscene;
    s.id = 24101010; // a boss intro: mirror only
    CHECK(!ReplayWorthy(s));
    s.tod = 2;
    CHECK(ReplayWorthy(s));
    s.tod = -1;
    s.id = 24000030;
    CHECK(ReplayWorthy(s));
    s.id = 1;
    s.force = true;
    CHECK(ReplayWorthy(s));

    for (int k = 0; k <= static_cast<int>(StoryKind::TimeOfDay); ++k) {
        CHECK(StoryKindFromName(StoryKindName(static_cast<StoryKind>(k))) == static_cast<StoryKind>(k));
    }
}

static void test_json() {
    StoryIntent s;
    s.seq = 0x18f0000000123ull;
    s.kind = StoryKind::Cutscene;
    s.id = 24000030;
    s.mode = 0;
    s.instr = 6;
    s.event_id = 12401803;
    s.map = 0x18000000;
    s.tod = 2;
    s.warp_point = 2402200;
    s.warp_map = 0x18000000;
    s.flags = {70002802, 9423};
    const std::string text = StoryToJsonText(s);
    StoryIntent r;
    std::string err;
    CHECK(StoryFromJsonText(text, &r, &err));
    CHECK(r.seq == s.seq && r.kind == s.kind && r.id == s.id && r.instr == 6 && r.event_id == 12401803);
    CHECK(r.map == s.map && r.tod == 2 && r.warp_point == 2402200 && r.warp_map == s.warp_map);
    CHECK(r.flags == s.flags && !r.force);

    StoryIntent e;
    e.seq = 5;
    e.kind = StoryKind::Ending;
    e.id = 3;
    CHECK(StoryFromJsonText(StoryToJsonText(e), &r, &err) && r.kind == StoryKind::Ending && r.id == 3 && r.tod == -1);
    e.id = 4;
    CHECK(!StoryFromJsonText(StoryToJsonText(e), &r, &err));
    e.id = 3;
    e.seq = 0;
    CHECK(!StoryFromJsonText(StoryToJsonText(e), &r, &err));
    CHECK(!StoryFromJsonText("{\"seq\":\"0x1\",\"kind\":\"bogus\"}", &r, &err));
    CHECK(!StoryFromJsonText("[1,2]", &r, &err));
    CHECK(!StoryFromJsonText("{", &r, &err));
}

static void test_host_queue() {
    HostStoryQueue q;
    StoryIntent s;
    s.kind = StoryKind::Cutscene;
    s.seq = 10;
    CHECK(q.Push(s));
    CHECK(!q.Push(s)); // same seq
    s.seq = 9;
    CHECK(!q.Push(s));
    for (std::uint64_t i = 11; i < 11 + HostStoryQueue::kMax; ++i) {
        s.seq = i;
        q.Push(s);
    }
    CHECK(q.Size() == HostStoryQueue::kMax);
    StoryIntent out;
    CHECK(q.Pop(&out) && out.seq == 11);
}

static StoryState OwnWorld() {
    StoryState st;
    st.world_up = true;
    st.session_role = 0;
    st.own_world = true;
    st.map_id = 0x18000000; // m24_00
    return st;
}

static StoryState Phantom() {
    StoryState st = OwnWorld();
    st.session_role = 6;
    st.own_world = false;
    st.map_id = 0x20000000;  // m32_00: the host's map, where the tests' scene 32000000 plays (MirrorMapOk)
    return st;
}

/// Steps until an action comes (or `max` frames); `t` advances 1/60 s a frame.
static std::optional<StoryAction> Until(GuestStory& g, const StoryState& st, double& t, int max = 200) {
    for (int i = 0; i < max; ++i) {
        t += 1.0 / 60;
        if (auto a = g.Step(st, t)) {
            return a;
        }
    }
    return std::nullopt;
}

static void PlayToEnd(GuestStory& g, StoryState st, double& t) {
    st.remo_playing = true;
    for (int i = 0; i < 240; ++i) { // 4 s: longer than GuestStory::kMirrorMinSeen
        t += 1.0 / 60;
        CHECK(!g.Step(st, t));
    }
    st.remo_playing = false;
}

static void test_mirror_then_replay() {
    GuestStory g;
    double t = 100;
    StoryIntent s;
    s.seq = 1;
    s.kind = StoryKind::Cutscene;
    s.id = 32000000;
    s.mode = 0;
    s.instr = 7;
    s.tod = 3;
    s.flags = {70002802};
    CHECK(g.Offer(s, true, t));
    CHECK(!g.Offer(s, true, t)); // duplicate seq
    CHECK(g.MirrorPending() && g.QueueSize() == 1 && g.Busy());

    // Phantom: the mirror plays with mode 2 after the gate held kStableTicks frames.
    StoryState ph = Phantom();
    std::optional<StoryAction> a;
    int frames = 0;
    for (; frames < 100 && !a; ++frames) {
        t += 1.0 / 60;
        a = g.Step(ph, t);
    }
    CHECK(a && a->kind == StoryAction::PlayRemo && a->id == 32000000 && a->mode == 2 && a->mirror);
    CHECK(frames >= GuestStory::kStableTicks);
    CHECK(!g.ReplayBusy() || g.QueueSize() == 1);
    PlayToEnd(g, ph, t);
    t += 1.0 / 60;
    CHECK(!g.Step(ph, t)); // wait end -> finished
    CHECK(g.Seen(32000000) && !g.Running());

    // Still a phantom (seamless play: maybe for the whole session): no second remo (seen), the
    // time of day, then the flags - the game side writes both into the own save's pool.
    ph.tod_value = 6; // the own save's clock: night
    a = Until(g, ph, t);
    CHECK(a && a->kind == StoryAction::ApplyTod && a->tod == 3);
    a = Until(g, ph, t);
    CHECK(a && a->kind == StoryAction::SetFlags && a->on && a->flags == std::vector<std::uint32_t>{70002802});
    CHECK(!Until(g, ph, t, 60));
    CHECK(!g.Busy());
}

// kidnap071 (a player report): the host's Gaol waking scene (28000010, m28) reached guests that
// were still in the Cathedral Ward (m24_00) and played there without the cell's scenery. A live
// mirror plays only in its own map; on a guest in another map it is dropped (TakeMirrorOtherMap).
// kidnapall (user decision 2026-10-22, replaces 0.7.2's "guests skip it"): every member sees the
// waking scene, whoever was grabbed. The guest that arrives in the cell after the host's copy (its
// loading screen held through it) plays it as its summoned replay, then the scene's flag 9423.
static void test_guest_replays_gaol_scene() {
    GuestStory g;
    double t = 100;
    StoryIntent s;
    s.seq = 1;
    s.kind = StoryKind::Cutscene;
    s.id = 28000010;
    s.instr = 3;
    s.flags = {9423};
    CHECK(g.Offer(s, false, t));  // missed live (loading): the replay queue
    StoryState ph = Phantom();
    ph.map_id = 0x18000000;  // still in the Cathedral Ward: it waits for the cell
    CHECK(!Until(g, ph, t, 200) && g.QueueSize() == 1);
    ph.map_id = 0x1c000000;  // m28_00: arrived with the party
    auto a = Until(g, ph, t, 200);
    CHECK(a && a->kind == StoryAction::PlayRemo && a->id == 28000010 && a->mode == 2 && !a->mirror);
    PlayToEnd(g, ph, t);
    a = Until(g, ph, t);
    CHECK(a && a->kind == StoryAction::SetFlags && a->on &&
          std::find(a->flags.begin(), a->flags.end(), 9423u) != a->flags.end());
    // The grabbed guest arrives from its own death (not calm): the waking scene does not wait for
    // calm (live run 1: 9 s in the cell first); any other replay still does.
    CHECK(ReplayIgnoresCalm(28000010) && !ReplayIgnoresCalm(32000000) && !ReplayIgnoresCalm(28000040));
    {
        GuestStory h;
        double th = 200;
        StoryState hurt = Phantom();
        hurt.map_id = 0x1c000000;
        hurt.calm = false;
        StoryIntent o = s;
        o.seq = 5;
        o.id = 28000040;  // another m28 scene: waits for calm
        o.flags = {};
        o.tod = 1;
        CHECK(h.Offer(o, false, th));
        CHECK(!Until(h, hurt, th, 120));
        StoryIntent w = s;
        w.seq = 6;
        CHECK(h.Offer(w, false, th));
        auto b = Until(h, hurt, th, 120);
        CHECK(b && b->kind == StoryAction::PlayRemo && b->id == 28000010);
    }
    // A guest that watched it live (Seen) keeps only the flags.
    GuestStory w;
    w.NoteSeen(28000010);
    CHECK(w.Offer(s, false, t));
    a = Until(w, ph, t, 200);
    CHECK(a && a->kind == StoryAction::SetFlags);
}

static void test_mirror_needs_its_map() {
    CHECK(MirrorMapOk(28000010, 0x1c000000));   // m28_00
    CHECK(!MirrorMapOk(28000010, 0x18000000));  // m24_00
    CHECK(MirrorMapOk(24010010, 0x18010000));   // Gascoigne's intro in m24_01
    CHECK(!MirrorMapOk(24010010, 0x18000000));  // ... not over m24_00
    CHECK(MirrorMapOk(28001040, 0x1c000000));   // the +1000 sibling: the same map
    CHECK(MirrorMapOk(28000010, 0xffffffffu));  // unknown map: as before
    GuestStory g;
    double t = 100;
    StoryIntent s;
    s.seq = 1;
    s.kind = StoryKind::Cutscene;
    s.id = 28000010;
    s.instr = 3;
    CHECK(g.Offer(s, true, t));
    StoryState ph = Phantom();
    ph.map_id = 0x18000000;  // m24_00: the host went to the Gaol, this guest did not
    CHECK(!Until(g, ph, t, 100));
    std::uint32_t at = 0;
    CHECK(!g.MirrorPending() && g.TakeMirrorOtherMap(&at) == 28000010 && at == 0x18000000);
    CHECK(g.TakeMirrorOtherMap() == 0);
    // Its queued replay (a story scene) does not play there as a summoned replay either.
    CHECK(g.QueueSize() == 1 && !Until(g, ph, t, 200) && g.QueueSize() == 1);
    // In the Gaol (the party's load brought it there): the mirror plays.
    s.seq = 2;
    CHECK(g.Offer(s, true, t));
    ph.map_id = 0x1c000000;
    auto a = Until(g, ph, t, 100);
    CHECK(a && a->kind == StoryAction::PlayRemo && a->id == 28000010 && a->mirror);
    PlayToEnd(g, ph, t);
    t += 1.0 / 60;
    CHECK(!g.Step(ph, t));
    CHECK(g.Seen(28000010));
    // Still loading (no stable world): it waits for the arrival, not dropped.
    GuestStory w;
    CHECK(w.Offer(s, true, t));
    StoryState ld = Phantom();
    ld.loading = true;
    CHECK(!Until(w, ld, t, 100));
    CHECK(w.MirrorPending() && w.TakeMirrorOtherMap() == 0);
}

// parg (live): a live mirror the guest's own load cut after 1.4 s is not "seen"; the queued
// replay plays the scene again (phantom path) once the guest is steady, then the time of day.
static void test_cut_mirror_replayed() {
    GuestStory g;
    double t = 100;
    StoryIntent s;
    s.seq = 1;
    s.kind = StoryKind::Cutscene;
    s.id = 32000000;
    s.instr = 7;
    s.tod = 3;
    s.flags = {70002802};
    CHECK(g.Offer(s, true, t));
    StoryState ph = Phantom();
    auto a = Until(g, ph, t, 100);
    CHECK(a && a->kind == StoryAction::PlayRemo && a->mirror);
    ph.remo_playing = true;
    for (int i = 0; i < 84; ++i) { // 1.4 s
        t += 1.0 / 60;
        CHECK(!g.Step(ph, t));
    }
    ph.remo_playing = false;
    t += 1.0 / 60;
    CHECK(!g.Step(ph, t));
    CHECK(!g.Seen(32000000) && g.TakeMirrorCut() == 32000000 && g.TakeMirrorCut() == 0);
    ph.tod_value = 6;
    a = Until(g, ph, t);
    CHECK(a && a->kind == StoryAction::PlayRemo && a->id == 32000000 && a->mode == 2 && !a->mirror);
    PlayToEnd(g, ph, t);
    a = Until(g, ph, t);
    CHECK(a && a->kind == StoryAction::ApplyTod && a->tod == 3);
    CHECK(g.Seen(32000000));
}

// BB_PARTY_STORY_SUMMONED=0: the queue waits for the own world, as before seamless play.
static void test_replay_waits_home_when_summoned_off() {
    GuestStory g;
    g.summoned_replay = false;
    double t = 100;
    StoryIntent s;
    s.seq = 1;
    s.kind = StoryKind::Cutscene;
    s.id = 32000000;
    s.instr = 7;
    s.tod = 3;
    s.flags = {70002802};
    CHECK(g.Offer(s, true, t));
    StoryState ph = Phantom();
    auto a = Until(g, ph, t, 100);
    CHECK(a && a->kind == StoryAction::PlayRemo && a->mirror);
    PlayToEnd(g, ph, t);
    t += 1.0 / 60;
    CHECK(!g.Step(ph, t));
    // Still a phantom: the queued item waits.
    CHECK(!Until(g, ph, t, 60));
    // Home (session dropped): no second remo (seen), the time of day, then the flags.
    StoryState own = OwnWorld();
    own.tod_value = 6; // night
    a = Until(g, own, t);
    CHECK(a && a->kind == StoryAction::ApplyTod && a->tod == 3);
    a = Until(g, own, t);
    CHECK(a && a->kind == StoryAction::SetFlags && a->on && a->flags == std::vector<std::uint32_t>{70002802});
    CHECK(!Until(g, own, t, 60));
    CHECK(!g.Busy());
}

// A scene the guest missed (sent while it was not in the host's world) plays as a phantom at the
// next calm moment through the phantom path (mode 2); an ending queued before it keeps waiting
// for the own world.
static void test_missed_scene_replayed_while_summoned() {
    GuestStory g;
    g.SetAskEnding(false);  // the replay body (the question: below)
    double t = 0;
    StoryIntent end;
    end.seq = 1;
    end.kind = StoryKind::Ending;
    end.id = 2;
    CHECK(g.Offer(end, false, t));
    StoryIntent s;
    s.seq = 2;
    s.kind = StoryKind::Cutscene;
    s.id = 28000040; // Lecture Building grab: sex variant + flag
    CHECK(g.Offer(s, false, t));
    CHECK(g.QueueSize() == 2 && !g.MirrorPending());
    StoryState ph = Phantom();
    ph.map_id = 0x1c000000;  // in Yahar'gul (m28_00), the scene's map
    // Not calm (lost HP lately): nothing.
    ph.calm = false;
    CHECK(!Until(g, ph, t, 80));
    ph.calm = true;
    auto a = Until(g, ph, t);
    CHECK(a && a->kind == StoryAction::PlayRemo && a->id == 28000040 && a->mode == 2 && !a->mirror);
    CHECK(g.RunningSummoned());
    PlayToEnd(g, ph, t);
    a = Until(g, ph, t);
    CHECK(a && a->kind == StoryAction::SetFlags && a->flags == std::vector<std::uint32_t>{12800434});
    CHECK(g.Seen(28000040));
    // The ending comes last; summoned it starts with the warp home (out of the host's world).
    a = Until(g, ph, t);
    CHECK(a && a->kind == StoryAction::TravelDream && a->id == GuestStory::kDreamLamp);
    CHECK(g.QueueSize() == 0 && g.Busy() && g.ReplayBusy());
}

static void test_summoned_ending() {
    // Summoned in the host's Hunter's Dream (m21, not the own one): the warp still runs; the
    // program waits for the own Dream (load, own world, steady) and goes on there.
    {
        GuestStory g;
    g.SetAskEnding(false);  // the replay body (the question: below)
        double t = 0;
        StoryIntent e;
        e.seq = 7;
        e.kind = StoryKind::Ending;
        e.id = 2;
        CHECK(g.Offer(e, true, t));
        StoryState ph = Phantom();
        ph.map_id = 0x15000000;
        auto a = Until(g, ph, t);
        CHECK(a && a->kind == StoryAction::TravelDream);
        // Still the host's world (the warp has not started yet): nothing more.
        CHECK(!Until(g, ph, t, 60));
        StoryState load = ph;
        load.loading = true;
        load.world_up = false;
        load.session_role = 0;
        CHECK(!Until(g, load, t, 30));
        StoryState dream = OwnWorld();
        dream.map_id = 0x15000000;
        a = Until(g, dream, t);
        CHECK(a && a->kind == StoryAction::SetFlags && a->flags == (std::vector<std::uint32_t>{12101800, 9180}));
        a = Until(g, dream, t);
        CHECK(a && a->kind == StoryAction::PlayRemo && a->id == 21000020 && a->mode == 0);
        PlayToEnd(g, dream, t);
        a = Until(g, dream, t);
        CHECK(a && a->kind == StoryAction::SetFlags && !a->on);
        a = Until(g, dream, t);
        CHECK(a && a->kind == StoryAction::NgCycle);
    }
    // Not calm: the guest does not leave in a fight.
    {
        GuestStory g;
    g.SetAskEnding(false);  // the replay body (the question: below)
        double t = 0;
        StoryIntent e;
        e.seq = 1;
        e.kind = StoryKind::Ending;
        e.id = 1;
        g.Offer(e, true, t);
        StoryState ph = Phantom();
        ph.calm = false;
        CHECK(!Until(g, ph, t, 120));
        ph.calm = true;
        auto a = Until(g, ph, t);
        CHECK(a && a->kind == StoryAction::TravelDream);
    }
    // BB_PARTY_STORY_ENDING_SUMMONED=0: the old wait for the own world.
    {
        GuestStory g;
    g.SetAskEnding(false);  // the replay body (the question: below)
        g.summoned_ending = false;
        double t = 0;
        StoryIntent e;
        e.seq = 1;
        e.kind = StoryKind::Ending;
        e.id = 3;
        g.Offer(e, true, t);
        CHECK(!Until(g, Phantom(), t, 120));
        CHECK(g.QueueSize() == 1);
        auto a = Until(g, OwnWorld(), t);
        CHECK(a && a->kind == StoryAction::TravelDream);
    }
}

static void test_replay_unseen_with_sex_and_tod_guard() {
    GuestStory g;
    g.SetAskEnding(false);  // the replay body (the question: below)
    double t = 0;
    StoryIntent s;
    s.seq = 7;
    s.kind = StoryKind::Cutscene;
    s.id = 28000040; // Lecture Building grab: sex variant + flag
    CHECK(g.Offer(s, false, t));
    CHECK(!g.MirrorPending() && g.QueueSize() == 1);
    StoryState own = OwnWorld();
    own.sex_variant = true;
    // A loading screen / remo holds it.
    own.loading = true;
    CHECK(!Until(g, own, t, 60));
    own.loading = false;
    auto a = Until(g, own, t);
    CHECK(a && a->kind == StoryAction::PlayRemo && a->id == 28001040 && a->mode == 0 && !a->mirror);
    PlayToEnd(g, own, t);
    a = Until(g, own, t);
    CHECK(a && a->kind == StoryAction::SetFlags && a->flags == std::vector<std::uint32_t>{12800434});

    // Time of day never goes back: Blood Moon guest, evening intent.
    StoryIntent tod;
    tod.seq = 8;
    tod.kind = StoryKind::TimeOfDay;
    tod.id = 1;
    CHECK(g.Offer(tod, false, t));
    own.tod_value = 7;
    CHECK(!Until(g, own, t, 80));
    CHECK(!g.Busy());
}

static void test_remo_never_starts() {
    GuestStory g;
    g.SetAskEnding(false);  // the replay body (the question: below)
    double t = 0;
    StoryIntent s;
    s.seq = 1;
    s.kind = StoryKind::Cutscene;
    s.id = 24000020;
    s.tod = 1;
    g.Offer(s, false, t);
    StoryState own = OwnWorld();
    auto a = Until(g, own, t);
    CHECK(a && a->kind == StoryAction::PlayRemo);
    // The playing bit never rises: after kRemoStartTimeout the tod still comes.
    a = Until(g, own, t, int(GuestStory::kRemoStartTimeout * 60) + 30);
    CHECK(a && a->kind == StoryAction::ApplyTod && a->tod == 1);
}

static void test_mirror_dropped_when_sent_home() {
    GuestStory g;
    g.SetAskEnding(false);  // the replay body (the question: below)
    double t = 0;
    StoryIntent s;
    s.seq = 1;
    s.kind = StoryKind::Cutscene;
    s.id = 24101010; // boss intro: mirror only
    g.Offer(s, true, t);
    CHECK(g.MirrorPending() && g.QueueSize() == 0);
    StoryState own = OwnWorld(); // sent home before the gate held
    CHECK(!Until(g, own, t, 60));
    CHECK(!g.Busy());

    GuestStory off;
    off.mirror_enabled = false;
    off.Offer(s, true, t);
    CHECK(!off.MirrorPending());
}

static void test_endings() {
    // A: Dream warp, arrival, 72100130; the game's event does the rest (no forced ending).
    {
        GuestStory g;
    g.SetAskEnding(false);  // the replay body (the question: below)
        double t = 0;
        StoryIntent e;
        e.seq = 3;
        e.kind = StoryKind::Ending;
        e.id = 1;
        CHECK(g.Offer(e, true, t));
        e.seq = 4; // the flag poll reports it again
        CHECK(g.Offer(e, true, t));
        CHECK(g.QueueSize() == 1 && !g.MirrorPending());
        StoryIntent rs; // the ending remo itself is never mirrored or queued
        rs.seq = 5;
        rs.kind = StoryKind::Cutscene;
        rs.id = 21001010;
        g.Offer(rs, true, t);
        CHECK(g.QueueSize() == 1 && !g.MirrorPending());

        StoryState own = OwnWorld();
        auto a = Until(g, own, t);
        CHECK(a && a->kind == StoryAction::TravelDream && a->id == GuestStory::kDreamLamp);
        CHECK(g.ReplayBusy());
        StoryState load = own;
        load.loading = true;
        CHECK(!Until(g, load, t, 30));
        StoryState dream = own;
        dream.map_id = 0x15000000;
        a = Until(g, dream, t);
        CHECK(a && a->kind == StoryAction::SetFlags && a->flags == std::vector<std::uint32_t>{72100130});
        // The ending step loads: done.
        CHECK(!Until(g, load, t, 10));
        CHECK(!Until(g, dream, t, 10));
        CHECK(!g.Busy());
    }
    // C, already in the Dream: no warp; flags, remo by sex, 9180 off, NG+, unlocks, 23, forced
    // ending request when no load follows.
    {
        GuestStory g;
    g.SetAskEnding(false);  // the replay body (the question: below)
        double t = 0;
        StoryIntent e;
        e.seq = 3;
        e.kind = StoryKind::Ending;
        e.id = 3;
        g.Offer(e, false, t);
        StoryState dream = OwnWorld();
        dream.map_id = 0x15000000;
        dream.sex_variant = true;
        auto a = Until(g, dream, t);
        CHECK(a && a->kind == StoryAction::SetFlags &&
              a->flags == (std::vector<std::uint32_t>{12101800, 12101850, 9180}) && a->on);
        a = Until(g, dream, t);
        CHECK(a && a->kind == StoryAction::PlayRemo && a->id == 21001030 && a->mode == 0);
        PlayToEnd(g, dream, t);
        a = Until(g, dream, t);
        CHECK(a && a->kind == StoryAction::SetFlags && a->flags == std::vector<std::uint32_t>{9180} && !a->on);
        a = Until(g, dream, t);
        CHECK(a && a->kind == StoryAction::NgCycle);
        a = Until(g, dream, t);
        CHECK(a && a->kind == StoryAction::SetFlags && a->flags == (std::vector<std::uint32_t>{6604, 6602, 6603}));
        a = Until(g, dream, t);
        CHECK(a && a->kind == StoryAction::SetFlags && a->flags == std::vector<std::uint32_t>{23});
        a = Until(g, dream, t, int(GuestStory::kEndingWait * 60) + 30);
        CHECK(a && a->kind == StoryAction::ForceEnding && a->ending == 3);
        CHECK(!g.Busy());
        CHECK(!g.Seen(21000030) || true);
    }
    // B: ending check satisfied by the native load -> no forced request.
    {
        GuestStory g;
    g.SetAskEnding(false);  // the replay body (the question: below)
        double t = 0;
        StoryIntent e;
        e.seq = 3;
        e.kind = StoryKind::Ending;
        e.id = 2;
        g.Offer(e, false, t);
        StoryState dream = OwnWorld();
        dream.map_id = 0x15000000;
        std::vector<StoryAction::Kind> kinds;
        for (int i = 0; i < 6; ++i) {
            auto a = Until(g, dream, t);
            CHECK(a.has_value());
            if (!a) {
                break;
            }
            kinds.push_back(a->kind);
            if (a->kind == StoryAction::PlayRemo) {
                CHECK(a->id == 21000020);
                PlayToEnd(g, dream, t);
            }
            if (a->kind == StoryAction::SetFlags && a->flags == std::vector<std::uint32_t>{22}) {
                break;
            }
        }
        CHECK(kinds.size() == 6);
        StoryState load = dream;
        load.loading = true;
        CHECK(!Until(g, load, t, 10));
        CHECK(!Until(g, dream, t, int(GuestStory::kEndingWait * 60) + 30));
        CHECK(!g.Busy());
    }
}

static void test_persistence() {
    GuestStory g;
    double t = 0;
    StoryIntent s;
    s.seq = 41;
    s.kind = StoryKind::Cutscene;
    s.id = 24000030;
    s.tod = 2;
    g.Offer(s, false, t);
    StoryIntent e;
    e.seq = 42;
    e.kind = StoryKind::Ending;
    e.id = 2;
    g.Offer(e, false, t);
    CHECK(g.TakeDirty());
    CHECK(!g.TakeDirty());
    const std::string text = json::dump(g.ToJson(), 0);

    GuestStory h;
    json::Value v;
    std::string err;
    CHECK(json::parse(text, v, err));
    CHECK(h.FromJson(v, &err));
    CHECK(h.QueueSize() == 2 && h.LastSeq() == 42);
    CHECK(!h.Offer(e, false, t)); // already seen seq
    json::Value bad;
    CHECK(json::parse("{\"last_seq\":\"0x1\",\"queue\":[{\"kind\":\"cutscene\"}],\"seen\":[]}", bad, err));
    CHECK(!h.FromJson(bad, &err));
    CHECK(h.QueueSize() == 2); // untouched on error
}

static void test_dedupe_skip() {
    // On by default; only "0" turns it off.
    CHECK(StoryDedupeEnabled(nullptr));
    CHECK(StoryDedupeEnabled("1"));
    CHECK(!StoryDedupeEnabled("0"));
    // A client's own 2002 of a mirrored event is skipped; nothing else is.
    // illusion_wall / annalise_missing: the guest's wait of the castle display event passes once mirrored
    CHECK(StoryScenePassWait(true, 1000, 2, 12500810, true, true, false));
    CHECK(!StoryScenePassWait(false, 1000, 2, 12500810, true, true, false));  // switch off
    CHECK(!StoryScenePassWait(true, 1000, 2, 12500810, false, true, false));  // host
    CHECK(!StoryScenePassWait(true, 1000, 2, 12500810, true, false, false));  // not mirrored
    CHECK(!StoryScenePassWait(true, 1000, 2, 12500810, true, true, true));    // the scene still plays
    CHECK(!StoryScenePassWait(true, 1000, 2, 12601802, true, true, false));   // another event
    CHECK(StoryScenePassWait(true, 0, 0, 12500810, true, true, false));        // the condition wait
    CHECK(!StoryScenePassWait(true, 1001, 1, 12500810, true, true, false));   // another instruction
    CHECK(StoryDedupeSkip(true, 2002, 3, 12601802, true, true));
    CHECK(!StoryDedupeSkip(false, 2002, 3, 12601802, true, true));
    CHECK(!StoryDedupeSkip(true, 2002, 3, 12601802, false, true));  // host
    CHECK(!StoryDedupeSkip(true, 2002, 3, 12601802, true, false));  // not mirrored
    CHECK(!StoryDedupeSkip(true, 2003, 3, 12601802, true, true));   // other bank
    CHECK(!StoryDedupeSkip(true, 2002, 8, 12601802, true, true));   // 2002[8] = travel
    CHECK(!StoryDedupeSkip(true, 2002, 3, 0, true, true));
}

// Playtest 1 extra problem: a guest who watched its own first Dream (21000000) got the host's
// copy as a catch-up replay later. NoteSeen skips the scene, the flags still come.
static void test_own_world_scene_not_replayed() {
    GuestStory g;
    double t = 0;
    g.NoteSeen(28001040);  // the sex variant counts as its base scene
    CHECK(g.Seen(28000040));
    CHECK(g.TakeDirty());
    g.NoteSeen(28000040);
    CHECK(!g.TakeDirty());  // nothing new
    StoryIntent s;
    s.seq = 3;
    s.kind = StoryKind::Cutscene;
    s.id = 28000040;
    CHECK(g.Offer(s, false, t));
    auto a = Until(g, OwnWorld(), t);
    CHECK(a && a->kind == StoryAction::SetFlags && a->flags == std::vector<std::uint32_t>{12800434});
}

// Playtest 1 bug 14: the host replayed Gascoigne's intro (a guest's story_up) mid-fight.
static void test_host_story_up_guard() {
    HostStoryUpGuard h;
    CHECK(h.Refuse(24010010, false, 0) == nullptr);
    CHECK(h.Refuse(24010010, true, 0) != nullptr);  // boss fight: never
    h.NotePlayed(24010010, 100);
    CHECK(h.Refuse(24010010, false, 100 + 47) != nullptr);
    CHECK(h.Refuse(24010010, false, 100 + HostStoryUpGuard::kRecent - 1) != nullptr);
    CHECK(h.Refuse(24010010, false, 100 + HostStoryUpGuard::kRecent + 1) == nullptr);
    CHECK(h.Refuse(24000020, false, 150) == nullptr);  // another scene
    h.NotePlayed(28001040, 200);                        // sex variant: same scene
    CHECK(h.Refuse(28000040, false, 210) != nullptr);
}

// smoke068 s1: two guests went in at Gascoigne's fog; their forwarded intro played on the host,
// then the host's own event 12411802 played it again (2 x 48 s). The forwarded copy waits for the
// host's own play, which the guard then drops; without the host's event it plays after the wait.
static void test_guest_intro_once_on_host() {
    // Held while the wait runs, only for a first-fight intro.
    CHECK(HoldGuestIntroForHostEvent(true, 0.0));
    CHECK(HoldGuestIntroForHostEvent(true, kGuestIntroWait - 0.1));
    CHECK(!HoldGuestIntroForHostEvent(true, kGuestIntroWait));
    CHECK(!HoldGuestIntroForHostEvent(false, 0.0));  // any other scene: at once, as before
    // Two guests forward the same intro at t = 127.3; the host's own 2002[3] at 127.6.
    HostStoryUpGuard h;
    const double t0 = 127.3;
    bool played_from_guest = false;
    for (int k = 0; k < 200; ++k) {
        const double t = t0 + 0.1 * k;
        if (k == 3) h.NotePlayed(24010010, t);  // the host's own event, 0.3 s later
        if (h.Refuse(24010010, false, t) != nullptr) break;           // dropped: one play
        if (!HoldGuestIntroForHostEvent(true, t - t0)) {
            played_from_guest = true;
            break;
        }
    }
    CHECK(!played_from_guest);
    // The host's event never comes (its visit failed): the guest's copy plays after the wait.
    HostStoryUpGuard none;
    double played_at = -1;
    for (double t = t0; t < t0 + 20.0; t += 0.1) {
        if (none.Refuse(24010010, false, t) == nullptr && !HoldGuestIntroForHostEvent(true, t - t0)) {
            played_at = t;
            break;
        }
    }
    CHECK(played_at >= t0 + kGuestIntroWait - 0.11 && played_at < t0 + kGuestIntroWait + 0.2);
}

// User decision (2026-10-15): the host's ending asks the guest first (pard3).
static void test_ending_question() {
    CHECK(GuestEndingType(1, false) == 1 && GuestEndingType(1, true) == 1);  // the host submitted: A
    CHECK(GuestEndingType(2, false) == 2 && GuestEndingType(2, true) == 3);  // refused: the guest's cords
    CHECK(GuestEndingType(3, false) == 2 && GuestEndingType(3, true) == 3);
    auto ending = [](std::uint64_t seq) {
        StoryIntent e;
        e.seq = seq;
        e.kind = StoryKind::Ending;
        e.id = 2;
        return e;
    };
    {  // YES: the question, then the replay as before
        GuestStory g;
        double t = 0;
        CHECK(g.Offer(ending(5), true, t));
        StoryState ph = Phantom();
        auto a = Until(g, ph, t);
        CHECK(a && a->kind == StoryAction::AskEnding && a->ending == 2);
        CHECK(g.EndingAskPending() && !Until(g, ph, t, 50));  // waits for the answer
        g.AnswerEnding(true, 3);  // three cords in the own save
        a = Until(g, ph, t);
        CHECK(a && a->kind == StoryAction::TravelDream);
        StoryState dream = OwnWorld();
        dream.map_id = 0x15000000;
        StoryState load = ph;
        load.loading = true;
        load.world_up = false;
        CHECK(!Until(g, load, t, 5));
        a = Until(g, dream, t);
        CHECK(a && a->kind == StoryAction::SetFlags &&
              a->flags == (std::vector<std::uint32_t>{12101800, 12101850, 9180}));  // C, the guest's own
    }
    {  // NOT NOW: home to the own Dream only, no flags, no NG cycle
        GuestStory g;
        double t = 0;
        CHECK(g.Offer(ending(6), true, t));
        StoryState ph = Phantom();
        auto a = Until(g, ph, t);
        CHECK(a && a->kind == StoryAction::AskEnding);
        g.AnswerEnding(false, 2);
        a = Until(g, ph, t);
        CHECK(a && a->kind == StoryAction::TravelDream);
        StoryState load = ph;
        load.loading = true;
        load.world_up = false;
        CHECK(!Until(g, load, t, 5));
        StoryState dream = OwnWorld();
        dream.map_id = 0x15000000;
        for (int i = 0; i < 400; ++i) {
            t += 0.1;
            auto b = g.Step(dream, t);
            CHECK(!b || (b->kind != StoryAction::SetFlags && b->kind != StoryAction::NgCycle &&
                         b->kind != StoryAction::PlayRemo && b->kind != StoryAction::ForceEnding));
        }
        CHECK(!g.Busy());
    }
    {  // no answer in kEndingAskTimeout: not now
        GuestStory g;
        double t = 0;
        CHECK(g.Offer(ending(7), true, t));
        StoryState ph = Phantom();
        auto a = Until(g, ph, t);
        CHECK(a && a->kind == StoryAction::AskEnding);
        t += kEndingAskTimeout + 1;
        a = Until(g, ph, t);
        CHECK(a && a->kind == StoryAction::TravelDream && !g.EndingAskPending());
    }
}

// D10 race (player report 2026-10-22, cwscene): the guest stood 2 m from the host at Gascoigne's
// gate. Its own copy of event 12411802 ran 2002[3] (remo 24010010) at the moment the host's did,
// before the host's EVENT "story" came, so the host's copy was dropped and the guest's own play
// showed ~18 s late. The own play of a boss intro is now held for the host's copy.
static void test_own_intro_held_for_host() {
    // Which plays are held: a client's plain 2002[1]/[3] of a known remo from a first-fight intro.
    CHECK(HoldOwnSceneForHost(true, true, 2002, 3, true, true));
    CHECK(HoldOwnSceneForHost(true, true, 2002, 1, true, true));
    CHECK(!HoldOwnSceneForHost(false, true, 2002, 3, true, true));  // the host plays its own
    CHECK(!HoldOwnSceneForHost(true, false, 2002, 3, true, true));  // BB_PARTY_STORY_MIRROR=0
    CHECK(!HoldOwnSceneForHost(true, true, 2003, 3, true, true));
    CHECK(!HoldOwnSceneForHost(true, true, 2002, 2, true, true));   // a warp variant
    CHECK(!HoldOwnSceneForHost(true, true, 2002, 6, true, true));   // G3 phase change: as before
    CHECK(!HoldOwnSceneForHost(true, true, 2002, 7, true, true));   // time of day
    CHECK(!HoldOwnSceneForHost(true, true, 2002, 3, false, true));  // not a boss intro: D10 as before
    CHECK(!HoldOwnSceneForHost(true, true, 2002, 3, true, false));  // unknown remo
    CHECK(kOwnIntroHold > kGuestIntroWait);  // the host's hold of a forwarded intro fits in it

    OwnSceneHold::Item own;
    own.event_id = 12411802;
    own.remo = 24010010;
    own.mode = 2;
    own.instr = 3;
    own.map = 0x18010000;

    {  // The live race: the host's copy comes 0.2 s after the own play was held -> one live play.
        OwnSceneHold h;
        GuestStory g;
        double t = 100;
        own.at = t;
        CHECK(h.Hold(own) && h.Holding(24010010) && h.Size() == 1);
        t += 0.2;
        StoryIntent host;
        host.seq = 7;
        host.kind = StoryKind::Cutscene;
        host.id = 24010010;
        host.instr = 3;
        host.mode = 2;
        host.event_id = 12411802;
        host.map = 0x18010000;
        CHECK(g.Offer(host, true, t) && g.MirrorPending());
        const auto dropped = h.HostCopy(host.id);
        CHECK(dropped && dropped->event_id == 12411802 && dropped->at == 100);
        CHECK(h.Size() == 0 && !h.Holding(24010010));
        StoryState ph = Phantom();
        ph.map_id = 0x18010000;  // m24_01: both at the gate
        const double sent = t;
        auto a = Until(g, ph, t);
        CHECK(a && a->kind == StoryAction::PlayRemo && a->id == 24010010 && a->mode == 2 && a->mirror);
        CHECK(t - sent < 1.0);  // the guest's scene starts within 1 s of the host's copy
        PlayToEnd(g, ph, t);
        t += 1.0 / 60;
        CHECK(!g.Step(ph, t));
        CHECK(g.Seen(24010010) && !g.Running());
        // Never a second copy: nothing held is due later, and the game's own copy was skipped.
        CHECK(!h.Due(t + 1000));
        CHECK(StoryDedupeSkip(true, 2002, 3, 12411802, true, true));
    }
    {  // The host's copy never comes: the own copy plays after kOwnIntroHold, once.
        OwnSceneHold h;
        own.at = 0;
        CHECK(h.Hold(own));
        CHECK(!h.Due(kOwnIntroHold - 0.1));
        const auto due = h.Due(kOwnIntroHold);
        CHECK(due && due->remo == 24010010 && due->mode == 2 && due->event_id == 12411802);
        CHECK(h.Size() == 0 && !h.Due(1000));
        CHECK(!h.HostCopy(24010010));  // a late host copy finds nothing held (RequestGuestStory drops it)
    }
    {  // The same scene twice is held once; another scene is not touched by the host's copy.
        OwnSceneHold h;
        own.at = 0;
        CHECK(h.Hold(own));
        OwnSceneHold::Item again = own;
        again.at = 1;
        CHECK(h.Hold(again) && h.Size() == 1);
        OwnSceneHold::Item other = own;
        other.event_id = 12601802;
        other.remo = 26000010;  // Mergo's Wet Nurse
        CHECK(h.Hold(other) && h.Size() == 2);
        CHECK(h.HostCopy(24010010) && h.Size() == 1 && h.Holding(26000010));
        // The sex variant of a scene counts as its base scene.
        OwnSceneHold::Item grab = own;
        grab.remo = 28000040;
        CHECK(h.Hold(grab) && h.HostCopy(28001040) && !h.Holding(28000040));
    }
    {  // A hold that never got a steady world to play in is dropped, not played much later.
        OwnSceneHold h;
        own.at = 0;
        CHECK(h.Hold(own));
        CHECK(h.Prune(kOwnIntroMaxAge - 1) == 0 && h.Size() == 1);
        CHECK(h.Prune(kOwnIntroMaxAge + 1) == 1 && h.Size() == 0);
        for (std::size_t i = 0; i < OwnSceneHold::kMax; ++i) {
            OwnSceneHold::Item it = own;
            it.remo = 24010010 + static_cast<std::uint32_t>(i) * 100000;
            CHECK(h.Hold(it));
        }
        OwnSceneHold::Item more = own;
        more.remo = 26000010;
        CHECK(!h.Hold(more));  // full: the game plays it as before
    }
}

// Player report 2026-10-22 (cwscene.log 3149-3227): the guest stood 2-3 m behind the host at the
// Cathedral Ward door. The host's scene 24000020 (event 12400750) is m24_00, the guest's current
// block was m24_01: the mirror was dropped ("its map is not loaded here") and the scene came as a
// replay once the guest walked into m24_00. The same area with the scene's block loaded, or the
// host near, now plays it live; another area or a far guest in an unloaded block still does not.
static void test_mirror_at_block_boundary() {
    const std::uint32_t m24_00 = 0x18000000, m24_01 = 0x18010000, m28_00 = 0x1c000000;
    CHECK(MapOfRemo(24000020) == m24_00 && MapOfRemo(24010010) == m24_01);
    CHECK(MapOfEntity(2410800) == m24_01 && MapOfEntity(2400123) == m24_00 && MapOfEntity(3602805) == 0x24000000);
    CHECK(MapOfEntity(10000) == 0 && MapOfEntity(0) == 0 && MapOfEntity(24000020) == 0);
    // The door: m24_01, the scene's block m24_00 loaded, the host 2.5 m away.
    CHECK(!MirrorMapOk(24000020, m24_01));
    CHECK(MirrorSceneOk(24000020, m24_01, true, 2.5f));
    CHECK(MirrorSceneOk(24000020, m24_01, false, 2.5f));   // block not seen loaded, the host right there
    CHECK(MirrorSceneOk(24000020, m24_01, true, -1.0f));   // loaded, no host position
    CHECK(MirrorSceneOk(24000020, m24_01, false, kMirrorNearHost));
    // Far away in the same area with the scene's block not loaded: none (no scenery).
    CHECK(!MirrorSceneOk(24000020, m24_01, false, 180.0f));
    CHECK(!MirrorSceneOk(24000020, m24_01, false, -1.0f));
    // Another area (the Gaol waking scene for guests in the Cathedral Ward): none, whatever is loaded.
    CHECK(!MirrorSceneOk(28000010, m24_01, true, 2.0f));
    CHECK(!MirrorSceneOk(24000020, m28_00, true, 2.0f));
    CHECK(MirrorSceneOk(24000020, m24_00, false, -1.0f));  // its own block: as before
    StoryState ph = Phantom();
    ph.map_id = m24_01;
    ph.loaded_blocks = {m24_01, m24_00};
    CHECK(SceneBlockLoaded(ph, 24000020) && !SceneBlockLoaded(ph, 28000010));

    StoryIntent s;
    s.seq = 1;
    s.kind = StoryKind::Cutscene;
    s.id = 24000020;
    s.instr = 6;
    s.tod = 1;
    s.event_id = 12400750;
    s.map = m24_00;
    {  // The door: the live mirror plays within 1 s, flagged as played outside its block; no replay.
        GuestStory g;
        double t = 100;
        StoryState door = ph;
        door.host_dist = 2.5f;
        CHECK(g.Offer(s, true, t));
        const double sent = t;
        auto a = Until(g, door, t);
        CHECK(a && a->kind == StoryAction::PlayRemo && a->id == 24000020 && a->mirror && t - sent < 1.0);
        CHECK(g.TakeMirrorNearby() == 24000020 && g.TakeMirrorNearby() == 0);
        CHECK(g.TakeMirrorOtherMap() == 0);
        PlayToEnd(g, door, t);
        t += 1.0 / 60;
        CHECK(!g.Step(door, t));
        CHECK(g.Seen(24000020));
        // Its queued catch-up (a world phase change: tod 1) keeps the time of day, never the scene again.
        StoryState in00 = door;
        in00.map_id = m24_00;
        in00.calm = true;
        for (int i = 0; i < 400; ++i) {
            t += 1.0 / 60;
            if (auto b = g.Step(in00, t)) {
                CHECK(b->kind != StoryAction::PlayRemo);
                if (b->kind == StoryAction::PlayRemo) break;
            }
        }
    }
    {  // A far guest in m24_01 with m24_00 not loaded: dropped as before.
        GuestStory g;
        double t = 100;
        StoryState far = Phantom();
        far.map_id = m24_01;
        far.loaded_blocks = {m24_01};
        far.host_dist = 180.0f;
        CHECK(g.Offer(s, true, t));
        CHECK(!Until(g, far, t, 100));
        std::uint32_t at = 0;
        CHECK(g.TakeMirrorOtherMap(&at) == 24000020 && at == m24_01 && g.TakeMirrorNearby() == 0);
    }
}

// The host's scene ended or was skipped (its A11 cutscene hold is over): the guest's live mirror of
// it ends too and counts as seen even when that is before kMirrorMinSeen (no catch-up replay).
static void test_host_scene_end_ends_mirror() {
    StoryIntent s;
    s.seq = 1;
    s.kind = StoryKind::Cutscene;
    s.id = 32000000;
    s.instr = 3;
    StoryState ph = Phantom();
    {  // A host skip 2 s in: the mirror ends here, seen, no replay of it.
        GuestStory g;
        double t = 100;
        g.HostSceneEnded();  // nothing running: no effect
        CHECK(!g.TakeEndMirror());
        CHECK(g.Offer(s, true, t));
        auto a = Until(g, ph, t);
        CHECK(a && a->kind == StoryAction::PlayRemo && a->mirror && g.RunningMirror());
        StoryState on = ph;
        on.remo_playing = true;
        for (int i = 0; i < 120; ++i) {  // 2 s of the scene
            t += 1.0 / 60;
            CHECK(!g.Step(on, t));
        }
        g.HostSceneEnded();
        CHECK(g.TakeEndMirror() && !g.TakeEndMirror());  // once
        t += 0.2;
        CHECK(!g.Step(ph, t));  // the remo fell (our end)
        CHECK(!g.Running() && g.Seen(32000000) && g.TakeMirrorCut() == 0);
        // Its queued replay (a story scene) keeps only its flags: never the scene again.
        StoryState home = OwnWorld();
        for (int i = 0; i < 400; ++i) {
            t += 1.0 / 60;
            if (auto b = g.Step(home, t)) {
                CHECK(b->kind != StoryAction::PlayRemo);
            }
        }
    }
    {  // Cut by this guest's own load (no host end): not seen, replayed later as before.
        GuestStory g;
        double t = 100;
        CHECK(g.Offer(s, true, t));
        CHECK(Until(g, ph, t));
        StoryState on = ph;
        on.remo_playing = true;
        t += 1.0 / 60;
        g.Step(on, t);
        t += 1.0;
        CHECK(!g.Step(ph, t));
        CHECK(!g.Seen(32000000) && g.TakeMirrorCut() == 32000000);
    }
}

// 0.7.6: one party ending = one NG cycle in a guest's save (a guest was one cycle ahead of the party).
static void test_ending_one_cycle() {
    GuestStory g;
    double t = 100;
    // First run of the ending's cycle step: +1; the same ending again (the replay resumed from the
    // sidecar after the game closed in the staff roll) writes the same target, never one more.
    CHECK(g.EndingNgTarget(65, 3, 1, t) == 2);
    CHECK(g.EndingNgTarget(65, 3, 2, t + 5) == 2);   // already there: unchanged
    CHECK(g.EndingNgTarget(65, 3, 1, t + 9) == 2);   // save reverted to the old cycle: brought to 2, not 3
    CHECK(g.EndingNgTarget(65, 3, 7, t + 9) == 7);   // never turned back
    GuestStory h;
    CHECK(h.FromJson(g.ToJson()));                    // survives a restart through the sidecar
    CHECK(h.EndingNgTarget(65, 3, 2, 500) == 2);
    CHECK(h.EndingNgTarget(90, 3, 2, 600) == 3);      // a later, different ending counts
    CHECK(h.EndingNgTarget(91, 1, 7, 700) == 7);      // capped at 7
    // A resend of the same ending type soon after is a duplicate; another type or much later is not.
    GuestStory d;
    CHECK(d.EndingNgTarget(10, 3, 0, 1000) == 1);
    StoryIntent e;
    e.kind = StoryKind::Ending;
    e.id = 3;
    e.seq = 11;
    CHECK(d.DuplicateEnding(3, 1010) && !d.DuplicateEnding(2, 1010));
    CHECK(d.Offer(e, false, 1010) && d.QueueSize() == 0);   // dropped, not queued
    CHECK(!d.DuplicateEnding(3, 1000 + GuestStory::kEndingDedupeSeconds + 1));
    // The running ending is not queued a second time either.
    GuestStory r;
    e.seq = 20;
    CHECK(r.Offer(e, false, 10) && r.QueueSize() == 1);
    e.seq = 21;
    CHECK(r.Offer(e, false, 11) && r.QueueSize() == 1);
}

int main() {
    test_tables();
    test_guest_phase_change();
    test_json();
    test_host_queue();
    test_mirror_then_replay();
    test_mirror_needs_its_map();
    test_guest_replays_gaol_scene();
    test_cut_mirror_replayed();
    test_replay_waits_home_when_summoned_off();
    test_missed_scene_replayed_while_summoned();
    test_summoned_ending();
    test_ending_question();
    test_replay_unseen_with_sex_and_tod_guard();
    test_remo_never_starts();
    test_mirror_dropped_when_sent_home();
    test_endings();
    test_persistence();
    test_dedupe_skip();
    test_own_world_scene_not_replayed();
    test_host_story_up_guard();
    test_guest_intro_once_on_host();
    test_own_intro_held_for_host();
    test_mirror_at_block_boundary();
    test_host_scene_end_ends_mirror();
    test_ending_one_cycle();
    std::printf("party-story-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
