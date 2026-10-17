// SPDX-License-Identifier: GPL-3.0-or-later
// Late joiner catch-up, the game glue (the decisions are coop::Catchup in party_start.h, unit-
// tested by party-start-test). Main thread, from the director's tick, guests only.
#include "party_catchup.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_async_io.h"
#include "party_progress.h"
#include "party_start.h"
#include "party_travel.h"
#include "test_newgame.h"

#include "party_director.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>


namespace coop {
namespace {

constexpr std::uint64_t kSprjRemo = 0x5540058; // *(+8) remo player; +0x168 bit 0 playing
constexpr std::uint32_t kDreamLamp = 2102961;  // fallback gather point (the Hunter's Dream lamp)

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party catch-up: ") + line + "\n");
}

std::uint64_t Slot(std::uint64_t off) {
    std::uint64_t v = 0;
    return SafeGet(Guest(off), &v) ? v : 0;
}

bool RemoPlaying() {
    const std::uint64_t remo = Slot(kSprjRemo);
    std::uint64_t player = 0;
    std::uint8_t bits = 0;
    return remo && SafeGet(remo + 8, &player) && player && SafeGet(player + 0x168, &bits) && (bits & 1);
}

struct State {
    Catchup c;
    double flags_at = -1;
    StartFlags flags;
    bool logged_wait = false;
};
State& S() {
    static State s;
    return s;
}

void ApplyStart() {
    int ok = 0, n = 0;
    std::string list;
    for (const FlagWrite& w : PrologueSkipFlags()) {
        ++n;
        bool before = false;
        ReadEventFlag(w.flag, &before);
        const bool written = WriteEventFlag(w.flag, w.on);
        ok += written ? 1 : 0;
        list += (list.empty() ? "" : " ") + std::to_string(w.flag) + ":" + std::to_string(int(before)) + "->" +
                std::to_string(int(w.on)) + (written ? "" : "(NOT written)");
    }
    Log("campaign-start flags into the own save: %d of %d written (%s)", ok, n, list.c_str());
    for (const StartWeapon& w : PrologueSkipWeapons()) {
        const int before = ItemCount(kItemWeapon, w.weapon);
        if (before == 0) {
            const bool given = GiveItem(kItemWeapon, w.weapon, 1);
            if (given) {
                ShowItemGet(kItemWeapon, w.weapon, 1);
            }
            Log("starting %s-hand weapon %u: %s (now x%d)", w.hand, w.weapon, given ? "given" : "NOT given",
                ItemCount(kItemWeapon, w.weapon));
        } else {
            Log("starting %s-hand weapon %u: %s", w.hand, w.weapon,
                before > 0 ? "already owned" : "inventory unreadable; not given");
        }
    }
    RequestGameSave("late joiner catch-up");
}

} // namespace

/// The host's time of day (9800..9802, 3 bits: the Blood Moon and the night) into the guest's own
/// save when the guest's is behind. The C2 change stream carries booleans of the synced
/// categories, not this value, so a guest that was not summoned when the host's world changed
/// phase (a late joiner, or a quit before the save) kept day in its own save (campaign_soak.md
/// open 2 / 2b). Own pool only; the world shown stays whatever the game loaded. Once per second.
/// (Only increases: a new NG cycle starts at day again in both saves.)
void TimeOfDayCatchup(const GameSnapshot& s, bool guest_connected, double now) {
    static double last = 0;
    if (!guest_connected || !s.world_up || s.loading || now - last < 1.0 || PartyPrologueHold()) {
        return;  // #19: never into a prologue world
    }
    last = now;
    // The value: while summoned (load mode 1) the game's own view is the host's overlay; else the
    // host's C2 snapshot when it carries these flags (it does not today, live 2026-10-11).
    const bool summoned = s.session_role == RoleClient && progress::FlagLoadMode() == 1;
    int host = 0, own = 0;
    for (std::uint32_t i = 0; i < 3; ++i) {
        bool h = false, o = false;
        const bool hk = summoned ? progress::ReadFlag(9800 + i, &h) : progress::HostSnapshotFlag(9800 + i, &h);
        if (!hk || !progress::ReadFlagOwnSave(9800 + i, &o)) {
            return;
        }
        host = host << 1 | (h ? 1 : 0);
        own = own << 1 | (o ? 1 : 0);
    }
    if (host <= own) {
        return;
    }
    for (std::uint32_t i = 0; i < 3; ++i) {
        progress::WriteFlagOwnSave(9800 + i, ((host >> (2 - i)) & 1) != 0);
    }
    int after = 0;
    for (std::uint32_t i = 0; i < 3; ++i) {
        bool o = false;
        progress::ReadFlagOwnSave(9800 + i, &o);
        after = after << 1 | (o ? 1 : 0);
    }
    Log("time of day behind the host's: own save 9800..9802 %d -> %d (host %d)", own, after, host);
    RequestGameSave("catch-up time of day");
}

void CatchupTick(const GameSnapshot& s, bool guest_connected, double now, std::uint32_t host_map) {
    State& st = S();
    TimeOfDayCatchup(s, guest_connected, now);
    // Playtest 1 #19 (user rule: correct over seamless): a guest plays its own prologue; the late
    // joiner catch-up (skip the opening, write the campaign-start flags, warp to the host's lamp)
    // changed a prologue world, so it is opt-in now: BB_PARTY_CATCHUP=1.
    static const bool on = [] {
        const char* e = std::getenv("BB_PARTY_CATCHUP");
        return e && e[0] == '1';
    }();
    if (!on) {
        if (guest_connected && !st.logged_wait && s.world_up && !s.loading) {
            st.logged_wait = true;
            Log("off (BB_PARTY_CATCHUP=1 turns it on): this guest plays its own prologue; summons start once "
                "everybody is ready");
        }
        return;
    }
    if (st.c.phase() == Catchup::Phase::Done) {
        return;
    }
    const bool in_world = s.world_up && !s.loading;
    if (in_world && now - st.flags_at >= 0.25) {
        st.flags_at = now;
        st.flags = ReadStartFlags();
        st.flags.map_id = s.map_id;  // #19: the first Dream visit lasts until the headstone
        st.flags.left_first_dream = LocalStartReady();
    } else if (!s.world_up) {
        st.flags = StartFlags{};
    }
    CatchupInput in;
    in.guest = guest_connected;
    bool opening = false, first_dream = false;
    const bool k1 = progress::HostSnapshotFlag(kFlagOpeningDone, &opening);
    const bool k2 = progress::HostSnapshotFlag(kFlagFirstDream, &first_dream);
    in.host_known = k1 && k2;
    in.host_ready = opening && first_dream;
    in.in_world = in_world;
    in.own_idle = in_world && s.session_role == RoleIdle && progress::FlagLoadMode() == 0;
    in.remo_playing = in_world && RemoPlaying();
    in.flags = st.flags;
    in.t = now;
    if (guest_connected && in.host_known && !in.host_ready && !st.logged_wait) {
        st.logged_wait = true;
        Log("the host's own save is in its prologue (12410000 %d, 9401 %d): nothing to catch up to", opening ? 1 : 0,
            first_dream ? 1 : 0);
    }
    const Catchup::Phase before = st.c.phase();
    const CatchupAction a = st.c.Step(in);
    switch (a.kind) {
    case CatchupAction::None:
        break;
    case CatchupAction::SkipOpening: {
        // The game's own skip request (what its skip input does), not a kill of the scene.
        bool skippable = false;
        const bool ok = RequestRemoSkip(&skippable);
        static int quiet = 0;
        if (skippable || !ok || ++quiet % 10 == 1) { // not every second while the scene fades in
            Log("%s (%s): remo skip request %s (skippable bit %d%s)", a.why.c_str(),
                DescribeStart(st.flags, in_world).c_str(), ok ? "sent" : "NOT possible (code differs / no remo)",
                skippable ? 1 : 0, skippable ? "" : ": the game ignores it until the scene is skippable");
        }
        break;
    }
    case CatchupAction::ApplyStart:
        Log("%s; before: %s", a.why.c_str(), DescribeStart(st.flags, in_world).c_str());
        ApplyStart();
        break;
    case CatchupAction::Warp: {
        const std::uint32_t lamp = GatherLampFor(GuestDeathRedirectLamp(), host_map);
        const std::uint32_t to = lamp != kTravelNone && lamp != 0 ? lamp : kDreamLamp;
        const bool ok = LampWarp(to);
        Log("%s: lamp warp to %u%s: %s", a.why.c_str(), to, to == lamp ? " (the host's last lamp)" : " (host lamp unknown: the Dream)",
            ok ? "called" : "NOT called (code differs / no player)");
        break;
    }
    case CatchupAction::Finished:
        Log("%s (%s; %s)", a.why.c_str(), DescribeStart(ReadStartFlags(), in_world).c_str(), Describe(s).c_str());
        break;
    }
    if (st.c.phase() != before && a.kind == CatchupAction::None) {
        Log("phase %s -> %s", CatchupPhaseName(before), CatchupPhaseName(st.c.phase()));
    }
}

bool CatchupBusy() {
    return S().c.Busy();
}

} // namespace coop
