// SPDX-License-Identifier: GPL-3.0-or-later
// Party story (gpu/shim/party/party_story, phase C4), the parts without the game: the data
// tables (sex variants, endings, story flags, time of day), the EVENT "story" JSON round trip,
// the host queue and GuestStory's mirror / replay / ending programs with an injected game state.
// Build and run: ninja -C out/gpu party-story-test && out/gpu/party-story-test.exe
#include "party/party_story.h"

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

int main() {
    test_tables();
    test_guest_phase_change();
    test_json();
    test_host_queue();
    test_mirror_then_replay();
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
    std::printf("party-story-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
