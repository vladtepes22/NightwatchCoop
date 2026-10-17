// SPDX-License-Identifier: GPL-3.0-or-later
// Host session watchdog (gpu/shim/party/party_watchdog): when a host in role host(3) with 0
// cooperators ends its session (guest gone, stuck summon, hard cap) and when it must not.
// Build and run: ninja -C out/gpu party-watchdog-test && out/gpu/party-watchdog-test.exe
#include "party/party_watchdog.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace coop;
using party::MemberState;

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

static std::vector<party::RosterEntry> Roster(MemberState guest, bool connected = true, std::uint32_t map = 0x18010000) {
    std::vector<party::RosterEntry> r(2);
    r[0].slot = party::kHostSlot;
    r[0].name = "Hunter0";
    r[0].state = MemberState::Home;
    r[0].connected = true;
    r[1].slot = 1;
    r[1].name = "Hunter1";
    r[1].state = guest;
    r[1].connected = connected;
    r[1].map_id = map;
    return r;
}

static HostWatchdogInput In(double t, int role, int coop, std::vector<party::RosterEntry> roster, bool world = true) {
    HostWatchdogInput in;
    in.now_s = t;
    in.in_world = world;
    in.session_role = role;
    in.cooperators = coop;
    in.roster = std::move(roster);
    return in;
}

/// Runs the watchdog from t0 to t1 in 0.5 s steps with a fixed input; the time of the first stop or -1.
static double RunUntilStop(HostWatchdog& w, double t0, double t1, int role, int coop,
                           const std::vector<party::RosterEntry>& roster) {
    for (double t = t0; t <= t1; t += 0.5) {
        if (w.Update(In(t, role, coop, roster))) {
            return t;
        }
    }
    return -1;
}

static void test_not_host() {
    HostWatchdog w;
    // Idle, trying to host, client, leaving: never.
    for (int role : {0, 1, 2, 4, 5, 6, 7}) {
        CHECK(RunUntilStop(w, 0, 600, role, 0, Roster(MemberState::Home)) < 0);
    }
    // A cooperator in the world: never.
    HostWatchdog w2;
    CHECK(RunUntilStop(w2, 0, 600, 3, 1, Roster(MemberState::InHostWorld)) < 0);
    // Loading screen / no world: never.
    HostWatchdog w3;
    bool stopped = false;
    for (double t = 0; t < 600; t += 0.5) {
        stopped = stopped || w3.Update(In(t, 3, 0, Roster(MemberState::Home), false));
    }
    CHECK(!stopped);
    // Unknown cooperator count: nothing.
    HostWatchdog w4;
    CHECK(RunUntilStop(w4, 0, 600, 3, -1, Roster(MemberState::Home)) < 0);
}

static void test_guest_gone() {
    // The guest crashed: its slot is kept (lost), or it is back at the title / dead / prologue.
    for (const auto& roster : {Roster(MemberState::InHostWorld, false), Roster(MemberState::Title),
                               Roster(MemberState::Home, false), Roster(MemberState::Loading, false),
                               Roster(MemberState::Dead), Roster(MemberState::Prologue)}) {
        HostWatchdog w;
        const double t = RunUntilStop(w, 100, 400, 3, 0, roster);
        CHECK(t == 125);
        CHECK(!w.why().empty());
    }
    // No other member at all.
    std::vector<party::RosterEntry> alone(1);
    alone[0].slot = party::kHostSlot;
    alone[0].connected = true;
    HostWatchdog w;
    CHECK(RunUntilStop(w, 0, 100, 3, 0, alone) == 25);
    // A member waits at home and is never invited (2026-10-10: a normal invite took 26 s): wait_s.
    HostWatchdog h;
    CHECK(RunUntilStop(h, 100, 400, 3, 0, Roster(MemberState::Home)) == 190);
    // Home for 40 s, then the invite arrives (Joining): no stop at 25 s, the stall clock restarts.
    HostWatchdog h2;
    CHECK(RunUntilStop(h2, 0, 40, 3, 0, Roster(MemberState::Home)) < 0);
    CHECK(RunUntilStop(h2, 40.5, 110, 3, 0, Roster(MemberState::Joining)) == 100.5);
}

static void test_stuck_summon() {
    // The restarted guest sits in the host's world (role client) and never gets the world data.
    HostWatchdog w;
    CHECK(RunUntilStop(w, 0, 400, 3, 0, Roster(MemberState::InHostWorld)) == 60);
    // Joining (role trying-to-join) forever: the same.
    HostWatchdog w2;
    CHECK(RunUntilStop(w2, 0, 400, 3, 0, Roster(MemberState::Joining)) == 60);
}

static void test_progress_keeps_it() {
    // A normal slow summon: Home -> Joining -> InHostWorld -> Loading (map 0, -1) -> InHostWorld
    // -> cooperator; each step under 60 s, 150 s in all: no stop.
    HostWatchdog w;
    struct Step {
        double until;
        MemberState s;
        std::uint32_t map;
    } steps[] = {{5, MemberState::Home, 1},         {40, MemberState::Joining, 1},
                 {90, MemberState::InHostWorld, 1}, {120, MemberState::Loading, 0},
                 {145, MemberState::Loading, 0xffffffffu}, {150, MemberState::InHostWorld, 1}};
    double t = 0;
    bool stopped = false;
    for (const Step& st : steps) {
        for (; t < st.until; t += 0.5) {
            stopped = stopped || w.Update(In(t, 3, 0, Roster(st.s, true, st.map)));
        }
    }
    CHECK(!stopped);
    CHECK(w.armed());
    // The cooperator arrives: disarmed.
    CHECK(!w.Update(In(t, 3, 1, Roster(MemberState::InHostWorld))));
    CHECK(!w.armed());
    // A frame with an unknown count keeps it disarmed and changes nothing.
    CHECK(!w.Update(In(t + 0.5, 3, -1, Roster(MemberState::InHostWorld))));
    CHECK(!w.armed());
}

static void test_hard_cap() {
    // The guest keeps flapping its state (progress every 30 s) but never arrives: the hard cap.
    HostWatchdog w;
    double stop = -1;
    for (double t = 0; t <= 400 && stop < 0; t += 0.5) {
        const MemberState s = (int(t / 30) % 2) ? MemberState::Loading : MemberState::InHostWorld;
        if (w.Update(In(t, 3, 0, Roster(s)))) {
            stop = t;
        }
    }
    CHECK(stop == 240);
}

static void test_retry_and_reset() {
    HostWatchdog w;
    CHECK(RunUntilStop(w, 0, 100, 3, 0, Roster(MemberState::Title)) == 25);
    // Still role 3 right after the stop (the leave takes frames): not again for retry_s.
    CHECK(RunUntilStop(w, 25.5, 39.5, 3, 0, Roster(MemberState::Title)) < 0);
    CHECK(RunUntilStop(w, 40, 41, 3, 0, Roster(MemberState::Title)) == 40);
    // Leaving, idle, then a new room: the clocks start again.
    CHECK(!w.Update(In(50, 7, 0, Roster(MemberState::Title))));
    CHECK(!w.Update(In(51, 0, 0, Roster(MemberState::Title))));
    CHECK(!w.armed());
    CHECK(RunUntilStop(w, 60, 200, 3, 0, Roster(MemberState::Title)) == 85);
}

static void test_test_stop() {
    HostWatchdogConfig c;
    c.test_stop_s = 3;
    HostWatchdog w(c);
    CHECK(RunUntilStop(w, 0, 10, 3, 0, Roster(MemberState::Home)) < 0);
    CHECK(RunUntilStop(w, 10.5, 20, 3, 0, Roster(MemberState::InHostWorld)) == 13.5);
    CHECK(!w.Update(In(20.5, 0, 0, Roster(MemberState::Home)))); // idle
    // Once only: the next room's join runs to the end.
    CHECK(RunUntilStop(w, 30, 50, 3, 0, Roster(MemberState::InHostWorld)) < 0);
}

static void SetEnv(const char* k, const char* v) {
#ifdef _WIN32
    std::string s = std::string(k) + "=" + v;
    _putenv(s.c_str());
#else
    setenv(k, v, 1);
#endif
}

static void test_config() {
    HostWatchdogConfig c;
    c.on = false;
    HostWatchdog w(c);
    CHECK(RunUntilStop(w, 0, 1000, 3, 0, Roster(MemberState::Home)) < 0);
    SetEnv("BB_PARTY_HOST_WATCHDOG", "0");
    CHECK(!HostWatchdogConfigFromEnv().on);
    SetEnv("BB_PARTY_HOST_WATCHDOG", "1");
    SetEnv("BB_PARTY_HOST_STALL_S", "30");
    SetEnv("BB_PARTY_HOST_GONE_S", "bogus");
    const HostWatchdogConfig e = HostWatchdogConfigFromEnv();
    CHECK(e.on && e.stall_s == 30 && e.gone_s == 25 && e.wait_s == 90 && e.hard_s == 240);
}

// Split travel: a member alone in another area is present (no chr in the host's world, so 0
// cooperators): never a stop while it stays apart; once it is no longer apart the clocks start.
static void test_members_apart() {
    HostWatchdog w;
    bool stopped = false;
    for (double t = 0; t < 900; t += 0.5) {
        HostWatchdogInput in = In(t, 3, 0, Roster(MemberState::InHostWorld, true, 0x15000000));
        in.members_apart = 1;
        stopped = stopped || w.Update(in);
    }
    CHECK(!stopped);
    CHECK(RunUntilStop(w, 900, 2000, 3, 0, Roster(MemberState::InHostWorld)) > 900);
}

/// Playtest 1 H1:3694 (batch 6): a joining member counted as stuck through the host's travel. The
/// load (world down, cooperators -1) and the B2 re-join after it (host_travel) restart the clocks.
static void test_host_travel() {
    HostWatchdog w;
    const auto joining = Roster(MemberState::InHostWorld);
    // 50 s stuck before the travel.
    CHECK(RunUntilStop(w, 0, 50, 3, 0, joining) < 0);
    // The host's load: world down and the cooperator count unknown (-1) for 20 s.
    bool stopped = false;
    for (double t = 50.5; t <= 70; t += 0.5) {
        stopped = stopped || w.Update(In(t, 3, -1, joining, false));
    }
    CHECK(!stopped);
    // Back in the world, the B2 re-join runs for 40 s: no stop.
    for (double t = 70.5; t <= 110; t += 0.5) {
        HostWatchdogInput in = In(t, 3, 0, joining);
        in.host_travel = true;
        stopped = stopped || w.Update(in);
    }
    CHECK(!stopped);
    // The re-join is over: the stall clock starts again from here (60 s).
    const double stop = RunUntilStop(w, 110.5, 300, 3, 0, joining);
    CHECK(stop >= 170 && stop <= 172);
    // Before the fix: the -1 frames kept the clocks of the pre-travel stall running.
    HostWatchdog w2;
    CHECK(RunUntilStop(w2, 0, 50, 3, 0, joining) < 0);
    for (double t = 50.5; t <= 60; t += 0.5) {
        CHECK(!w2.Update(In(t, 3, -1, joining, true)));  // in the world, count unknown: clocks kept
    }
    CHECK(RunUntilStop(w2, 60.5, 80, 3, 0, joining) > 0);  // ...so the 60 s stall still ends it
}

int main() {
    test_host_travel();
    test_not_host();
    test_members_apart();
    test_guest_gone();
    test_stuck_summon();
    test_progress_keeps_it();
    test_hard_cap();
    test_retry_and_reset();
    test_test_stop();
    test_config();
    std::printf("party-watchdog-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
