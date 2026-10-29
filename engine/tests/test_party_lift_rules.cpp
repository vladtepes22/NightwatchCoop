// SPDX-License-Identifier: GPL-3.0-or-later
// Lifts in a party (gpu/shim/party/party_lift_rules.h): the table of every lift and the decisions.
// cmake --build out/gpu --target party-lift-rules-test && out/party-lift-rules-test
#include "party/party_lift_rules.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

using namespace coop::liftrules;

static int g_fail = 0;
#define CHECK(x)                                                        \
    do {                                                                \
        if (!(x)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);    \
            ++g_fail;                                                   \
        }                                                               \
    } while (0)

int main() {
    // The table: every lift of the main game and the DLC (tools/party/lift_tool.py check: the up /
    // down events of every map, named or not), one entry per car and per position flag.
    struct Expect {
        std::int32_t car;
        std::uint32_t pos;
    };
    const Expect all[] = {
        {2201210, 12200125}, {2401101, 12400150}, {2401102, 12400827}, {2411320, 12410350}, {2421250, 12420154},
        {2501010, 12500074}, {2601210, 12601250}, {2601220, 12601251}, {2601230, 12601252}, {2601240, 12601256},
        {2601280, 12601254}, {2601290, 12601255}, {2601260, 12601253}, {2701032, 12700134}, {2701042, 12700144},
        {2801400, 12800610}, {3301000, 13300115}, {3501100, 13501106}, {3501105, 13501116}, {3501110, 13501126},
        {3601100, 13601106},
    };
    CHECK(Lifts().size() == sizeof all / sizeof all[0]);
    std::set<std::int32_t> cars;
    std::set<std::uint32_t> flags;
    for (const Expect& e : all) {
        const Lift* l = LiftByCar(e.car);
        CHECK(l && l->pos == e.pos && LiftByPosFlag(e.pos) == l);
    }
    for (const Lift& l : Lifts()) {
        CHECK(cars.insert(l.car).second && flags.insert(l.pos).second);
        CHECK(l.busy == 0 || (!LiftByPosFlag(l.busy) && flags.count(l.busy) == 0));
        CHECK(l.pos / 100000u % 100u == std::uint32_t(l.car) / 100000u % 100u);  // flag and car of one area
        // Replaying lifts: the library lift (no busy flag) and Cainhurst's (a copy that did not follow).
        CHECK(l.Replays() == (l.car == 2601260 || l.car == 2501010));
    }
    CHECK(!LiftByPosFlag(0) && !LiftByPosFlag(12410351) && !LiftByCar(0) && !LiftByCar(2601261));

    // The library lift (12601352): from the top (pos 1) car 6, 3; from the bottom 8, 5; its ends:
    // pos 1 car 7 (init 2), pos 0 car 9 (init 4).
    const Lift* lib = LiftByPosFlag(12601253);
    CHECK(lib && lib->Replays() && lib->car == 2601260 && lib->gate == 2601261 && lib->gate_rides);
    for (const Lift& l : Lifts()) {
        CHECK(l.gate_rides == (&l == lib));  // only the library lift has a second cage
    }
    for (const int a : {6, 3, 8, 5}) {
        CHECK(PhaseOf(*lib, a) == CarPhase::riding);
    }
    CHECK(PhaseOf(*lib, 7) == CarPhase::at_on && PhaseOf(*lib, 2) == CarPhase::at_on);
    CHECK(PhaseOf(*lib, 9) == CarPhase::at_off && PhaseOf(*lib, 4) == CarPhase::at_off);
    CHECK(PhaseOf(*lib, -1) == CarPhase::unknown && PhaseOf(*lib, 1) == CarPhase::unknown);
    CHECK(CarAgreesWithFlag(*lib, true, 7) && CarAgreesWithFlag(*lib, true, 2) && CarAgreesWithFlag(*lib, false, 9) &&
          CarAgreesWithFlag(*lib, false, 4));
    CHECK(!CarAgreesWithFlag(*lib, true, 9) && !CarAgreesWithFlag(*lib, false, 7) && !CarAgreesWithFlag(*lib, true, 3));
    const Lift* cy = LiftByPosFlag(12410350);  // Central Yharnam: a busy flag, network-synced, no replay
    CHECK(cy && !cy->Replays() && PhaseOf(*cy, 6) == CarPhase::unknown && !HoldMirrorFlip(*cy, 6) &&
          !MirrorHold(*cy, 6, true));

    // Cainhurst's lift (live run 5: the host's copy did not follow, its intro cutscene ran): it replays; pos 1 = the car at
    // the bottom (end 1), pos 0 = at the top (end 11); rides 0 (down) / 10 (up).
    const Lift* ch = LiftByCar(2501010);
    CHECK(ch && ch->Replays() && ch->busy == 12505074 && ch->gate == 0);
    CHECK(PhaseOf(*ch, 0) == CarPhase::riding && PhaseOf(*ch, 10) == CarPhase::riding);
    CHECK(CarAgreesWithFlag(*ch, true, 1) && CarAgreesWithFlag(*ch, false, 11) && !CarAgreesWithFlag(*ch, true, 11));
    CHECK(LiftNeedsReplay(*ch, true, 11) && !LiftNeedsReplay(*ch, false, 11) && MirrorHold(*ch, 10, false));
    CHECK(HostLiftSync(false, false, true, true, ch->Replays(), false) == LiftSync::apply);

    // The flag hold (the 0.7.1 full smoke's el ride 5: both rode down; the host's copy ended first,
    // and the host's flip reached the guest in the very tick the guest's own end animation 9 stopped:
    // 0.7.1 held it - the guest stood in the car - and put the guest's own end back; its flag stayed 1
    // with the car at the bottom until the next ride).
    CHECK(HoldMirrorFlip(*lib, 3) && HoldMirrorFlip(*lib, 5));     // the ride, or the car arrived and
    CHECK(HoldMirrorFlip(*lib, 6) && HoldMirrorFlip(*lib, 8));     // its player still in it: not read yet
    CHECK(!HoldMirrorFlip(*lib, 9) && !HoldMirrorFlip(*lib, 7));   // the copy's end: it has read the flag
    CHECK(!HoldMirrorFlip(*lib, 2) && !HoldMirrorFlip(*lib, 4) && !HoldMirrorFlip(*lib, -1));
    CHECK(!LiftNeedsReplay(*lib, false, 9));                       // ... and its end needs no replay
    // The mirror's hold: from the ride's start until the copy's end animation stops (it writes then).
    CHECK(MirrorHold(*lib, 6, true) && MirrorHold(*lib, 3, true) && MirrorHold(*lib, 3, false));
    CHECK(MirrorHold(*lib, 9, true) && MirrorHold(*lib, 7, true));
    CHECK(!MirrorHold(*lib, 9, false) && !MirrorHold(*lib, 7, false) && !MirrorHold(*lib, 2, false) &&
          !MirrorHold(*lib, -1, false));

    // The replay: another game's ride (the car stands at the other end, or no pose yet).
    CHECK(!LiftNeedsReplay(*lib, true, 7) && !LiftNeedsReplay(*lib, true, 2) && !LiftNeedsReplay(*lib, false, 4));
    CHECK(LiftNeedsReplay(*lib, true, 9) && LiftNeedsReplay(*lib, false, 7) && LiftNeedsReplay(*lib, true, -1) &&
          LiftNeedsReplay(*lib, false, 2));
    CHECK(!LiftNeedsReplay(*cy, true, 0) && !LiftNeedsReplay(*cy, false, 30));  // network-synced: never

    // The host's answer to a guest's finished ride.
    CHECK(HostLiftSync(true, false, false, true) == LiftSync::same);
    CHECK(HostLiftSync(false, false, true, false) == LiftSync::same);
    CHECK(HostLiftSync(false, true, false, true) == LiftSync::busy);
    CHECK(HostLiftSync(false, true, true, true) == LiftSync::busy);
    CHECK(HostLiftSync(false, false, true, true) == LiftSync::loaded);
    CHECK(HostLiftSync(false, false, false, true) == LiftSync::apply);
    CHECK(HostLiftSync(true, false, false, false) == LiftSync::apply);
    CHECK(HostLiftSync(false, false, true, true, true, false) == LiftSync::apply);
    CHECK(HostLiftSync(false, false, true, true, true, true) == LiftSync::occupied);
    CHECK(HostLiftSync(true, false, true, true, true, true) == LiftSync::same);
    CHECK(GuestLiftRideDone(true, true, false) && GuestLiftRideDone(false, false, false));
    CHECK(!GuestLiftRideDone(true, true, true) && !GuestLiftRideDone(false, true, false));

    // The guest's report: every ride to an end the host's world lacks goes up once; again after the
    // host's world had the guest's end in between (0.7.1: once per (flag, value) for the whole stay).
    CHECK(GuestLiftReport(true, false, true, -1) == GuestReport::wait);   // the ride runs
    CHECK(GuestLiftReport(true, false, false, -1) == GuestReport::send);  // over: up to the host
    CHECK(GuestLiftReport(true, false, false, 1) == GuestReport::sent);   // not again
    CHECK(GuestLiftReport(true, true, false, 1) == GuestReport::same);    // the host's world has it
    CHECK(GuestLiftReport(false, true, false, 1) == GuestReport::send);   // the ride back down
    CHECK(GuestLiftReport(true, false, false, 0) == GuestReport::send);   // and up again
    CHECK(GuestLiftReport(false, false, false, -1) == GuestReport::same);

    // The ride ends that wait for the local player alone (Cainhurst, the library lift) become the
    // all-players check of every other lift: 3[2] (group, state, entity, region) -> 3[7] (group,
    // state, region at +4). The library lift's end (12601352 [25]): group 5, outside, 10000, 2602241.
    {
        std::uint8_t a[12] = {0x05, 0x00, 0, 0, 0x10, 0x27, 0, 0, 0x01, 0xb5, 0x27, 0x00};
        CHECK(LocalEndToAllPlayers(12601352, 3, 2, a, sizeof a));
        std::int32_t region = 0, rest = -1;
        std::memcpy(&region, a + 4, 4);
        std::memcpy(&rest, a + 8, 4);
        CHECK(a[0] == 5 && a[1] == 0 && region == 2602241 && rest == 0);
        // its start checks (inside, OR groups -1 / -2) stay the local player's
        std::uint8_t start[12] = {0xff, 0x01, 0, 0, 0x10, 0x27, 0, 0, 0x01, 0xb5, 0x27, 0x00};
        CHECK(!LocalEndToAllPlayers(12601352, 3, 2, start, sizeof start) && start[4] == 0x10);
        // Cainhurst's end (12500076 [12]: group 0, outside, 10000, 2502001)
        std::uint8_t c[12] = {0x00, 0x00, 0, 0, 0x10, 0x27, 0, 0, 0x71, 0x2d, 0x26, 0x00};
        CHECK(LocalEndToAllPlayers(12500076, 3, 2, c, sizeof c));
        std::memcpy(&region, c + 4, 4);
        CHECK(region == 2502001);
        // not a listed event, not 3[2], another entity, already rewritten, short args: untouched
        std::uint8_t o[12] = {0x05, 0x00, 0, 0, 0x10, 0x27, 0, 0, 0x01, 0xb5, 0x27, 0x00};
        CHECK(!LocalEndToAllPlayers(12410322, 3, 2, o, sizeof o) && !LocalEndToAllPlayers(12601352, 3, 3, o, sizeof o));
        CHECK(!LocalEndToAllPlayers(12601352, 3, 2, o, 8));
        std::uint8_t e[12] = {0x05, 0x00, 0, 0, 0x11, 0x27, 0, 0, 0x01, 0xb5, 0x27, 0x00};  // entity 10001
        CHECK(!LocalEndToAllPlayers(12601352, 3, 2, e, sizeof e));
        CHECK(!LocalEndToAllPlayers(12601352, 3, 2, a, sizeof a));  // the rewritten bytes again: entity 2602241
        CHECK(!LocalEndToAllPlayers(12601352, 3, 7, a, sizeof a));  // ... also once the id is 7
        // a later run of the event: id 7 already, its argument bytes afresh from the script (live run 3)
        std::uint8_t again[12] = {0x05, 0x00, 0, 0, 0x10, 0x27, 0, 0, 0x02, 0xb5, 0x27, 0x00};
        CHECK(LocalEndToAllPlayers(12601352, 3, 7, again, sizeof again));
        std::memcpy(&region, again + 4, 4);
        CHECK(region == 2602242);
        CHECK(LocalEndRideEvent(12500077) && !LocalEndRideEvent(12410323) && !LocalEndRideEvent(0));
    }

    // The rider guard: the report's ride (Research Hall lift 3501105, ~2.5 m/s down from 1583.2) and a
    // drop through the car.
    {
        RiderTrack r;
        float put[3] = {};
        CHECK(RiderTick(r, false, true, 0.0, -344.6f, 1583.2f, -793.0f, put) == RiderStep::idle);
        double t = 0;
        float y = 1583.2f;
        for (int k = 0; k < 30; ++k, t += 1.0 / 30, y -= 2.5f / 30) {
            CHECK(RiderTick(r, true, true, t, -344.6f, y, -793.0f, put) == RiderStep::riding);
        }
        t -= 1.0 / 30, y += 2.5f / 30;  // the last sample
        CHECK(std::fabs(r.vy + 2.5f) < 0.1f);
        // the bit flickers for a frame in the car (a roll): no put-back
        CHECK(RiderTick(r, true, false, t + 0.05, -344.7f, y - 0.15f, -793.0f, put) == RiderStep::riding);
        // through the floor: the bit off, straight down at 9 m/s for 0.3 s
        CHECK(RiderTick(r, true, false, t + 0.3, -344.6f, y - 2.8f, -792.9f, put) == RiderStep::through);
        CHECK(std::fabs(put[0] + 344.6f) < 0.01f && std::fabs(put[2] + 793.0f) < 0.01f);
        CHECK(put[1] > y - 1.0f && put[1] < y);  // the car's height then, 0.75 m lower, + 0.3
        // back on the car: riding again
        CHECK(RiderTick(r, true, true, t + 0.4, -344.6f, put[1] - 0.2f, -793.0f, put) == RiderStep::riding);
        // walked off an open platform (sideways), or long gone: no put-back
        RiderTrack w = r;
        CHECK(RiderTick(w, true, false, t + 0.6, -342.0f, put[1] - 3.0f, -793.0f, put) == RiderStep::off && !w.riding);
        RiderTrack g = r;
        CHECK(RiderTick(g, true, false, t + 3.0, -344.6f, put[1] - 9.0f, -793.0f, put) == RiderStep::off);
        // a jump in the car: up, no put-back; the ride's end: idle and reset
        RiderTrack j = r;
        CHECK(RiderTick(j, true, false, t + 0.5, -344.6f, put[1] + 0.8f, -793.0f, put) == RiderStep::riding);
        CHECK(RiderTick(j, false, false, t + 0.6, -344.6f, put[1], -793.0f, put) == RiderStep::idle && !j.riding);
        // never on the car (stood beside it): never a put-back
        RiderTrack n;
        CHECK(RiderTick(n, true, false, 0.0, -350.0f, 1583.0f, -793.0f, put) == RiderStep::idle);
        CHECK(RiderTick(n, true, false, 0.5, -350.0f, 1570.0f, -793.0f, put) == RiderStep::idle);
    }

    // lifts2: the Forbidden Woods report. The ride up (anim 6, +5 m/s), its arrival (anim 7) and the
    // ride down (anim 2) can follow without a frame between them in which the car stops playing; the
    // rider's bit went off at the top. 0.7.4 kept +5 m/s and every put-back climbed over the last.
    {
        auto ride_up = [](RiderTrack& r, double& t, float& y, std::int32_t anim) {
            float put[3];
            for (int k = 0; k < 30; ++k, t += 1.0 / 30, y += 5.0f / 30) {
                CHECK(RiderTick(r, true, true, t, -216.6f, y, 279.4f, put, anim) == RiderStep::riding);
            }
            t -= 1.0 / 30, y -= 5.0f / 30;
        };
        float put[3] = {};
        // (a) the report: up at +5 m/s, the bit off, the car turns (anim 2) and goes down, the chr after it
        {
            RiderTrack r;
            double t = 0;
            float y = -82.9f;
            ride_up(r, t, y, 6);
            CHECK(r.vy > 4.5f);
            int puts = 0;
            for (int k = 1; k < 120; ++k) {
                const RiderStep st = RiderTick(r, true, false, t + k / 60.0, -216.6f, y - k * 0.08f, 279.4f, put, 2);
                puts += st == RiderStep::through;
                if (st == RiderStep::off) break;
            }
            CHECK(puts == 0);  // no speed known for the turned car: no put-back, never higher
        }
        // (b) a drop through the floor within one animation still gets its put-back, at this ride's speed
        {
            RiderTrack r;
            double t = 0;
            float y = -82.9f;
            ride_up(r, t, y, 6);
            CHECK(RiderTick(r, true, false, t + 0.3, -216.6f, y - 1.5f, 279.4f, put, 6) == RiderStep::through);
            CHECK(std::fabs(put[1] - (y + 1.5f + 0.3f)) < 0.1f && r.after_put && r.puts == 1);
            // landed on the car again: that first sample gives no speed (the warp is not the car's motion)
            const float vy = r.vy;
            CHECK(RiderTick(r, true, true, t + 0.32, -216.6f, put[1] - 0.1f, 279.4f, put, 6) == RiderStep::riding);
            CHECK(r.vy == vy && !r.after_put);
        }
        // (c) put-backs are bounded per ride (0.7.4: each one re-armed the track, no end while the car played)
        {
            RiderTrack r;
            double t = 0;
            float y = -82.9f;
            ride_up(r, t, y, 6);
            int puts = 0;
            for (int k = 1; k < 60; ++k) {
                puts += RiderTick(r, true, false, t + k * 0.1, -216.6f, -120.0f, 279.4f, put, 6) == RiderStep::through;
            }
            CHECK(puts == kRiderMaxPuts);
        }
        // (d) the same car animation: speed measured as before; a turn starts the speed afresh
        {
            RiderTrack r;
            double t = 0;
            float y = 0;
            ride_up(r, t, y, -1);
            CHECK(r.vy > 4.5f);
            CHECK(RiderTick(r, true, true, t + 1.0 / 30, -216.6f, y, 279.4f, put, 2) == RiderStep::riding && r.vy == 0.0f);
        }
    }

    // A duplicate forward of one ride (two guests send the same end): the host's world has it after the
    // first, so the second is `same` (nothing written, no kick).
    {
        CHECK(HostLiftSync(false, false, true, false, false, false) == LiftSync::same);
        CHECK(HostLiftSync(true, true, true, false, false, false) == LiftSync::busy);
    }

    // The Research Hall busy clear moved to the event's end (runs 11-15).
    {
        const BusyDefer* d = nullptr;
        CHECK(BusyDeferStep(13501113, 2003, 2, 26, 13504110u, &d) == BusyStep::keep && d && d->busy == 13504110u);
        CHECK(BusyDeferStep(13501112, 1000, 4, 30, 0, &d) == BusyStep::clear);
        CHECK(BusyDeferStep(13501123, 2003, 2, 26, 13504120u, &d) == BusyStep::keep);
        CHECK(BusyDeferStep(13501113, 2003, 2, 19, 13501116u, &d) == BusyStep::none);   // the position flag
        CHECK(BusyDeferStep(13501113, 2003, 2, 26, 13501116u, &d) == BusyStep::none);   // not the busy flag
        CHECK(BusyDeferStep(13501103, 2003, 2, 47, 13504100u, &d) == BusyStep::none);   // the altar lift waits 1 s
        CHECK(BusyDeferStep(12410322, 1000, 4, 26, 0, &d) == BusyStep::none);
        for (const BusyDefer& b : BusyDefers()) {
            bool lift = false;
            for (const Lift& l : Lifts()) lift = lift || l.busy == b.busy;
            CHECK(lift);  // a lift of the table
        }
    }

    // chalicelamp: the Chalice Dungeons' lift cars are learned from their rides' 2003[18].
    {
        CHECK(ChaliceLiftCar(12900354, 2003, 18, 2901010) == 2901010);  // 21 m floor lift down (the report's)
        CHECK(ChaliceLiftCar(12900351, 2003, 18, 2901004) == 2901004);
        CHECK(ChaliceLiftCar(12900365, 2003, 18, 2901300) == 2901300);  // a balance lift's car
        CHECK(ChaliceLiftCar(12900354, 2003, 2, 2901010) == 0);          // not the animation
        CHECK(ChaliceLiftCar(12900347, 2003, 18, 2901010) == 0);         // the init (no ride)
        CHECK(ChaliceLiftCar(12410350, 2003, 18, 2411320) == 0);         // a main game lift: the table's
        CHECK(ChaliceLiftCar(12900354, 2003, 18, 10000) == 0);           // never the player
        for (const Lift& l : Lifts()) CHECK(!ChaliceLiftEvent(std::int32_t(l.pos)));
    }
    // The dungeon rider guard (the report: the guest 5 m under the car at the shaft's bottom).
    {
        float put[3];
        CarRider r;
        CHECK(CarRiderTick(r, false, 10.5f, -128, 10.5f, 13, put) == RiderStep::idle);
        CHECK(CarRiderTick(r, true, 10.0f, -128, 10.5f, 13, put) == RiderStep::riding);  // on the car: offset 0.5
        CHECK(CarRiderTick(r, true, 5.0f, -128, 5.5f, 13, put) == RiderStep::riding);
        CHECK(CarRiderTick(r, true, 4.8f, -128.2f, 5.4f, 13, put) == RiderStep::riding);   // a frame's lag
        CHECK(CarRiderTick(r, true, 4.0f, -128.2f, 2.0f, 13, put) == RiderStep::through);  // through the floor
        CHECK(std::fabs(put[1] - 4.8f) < 0.01f && std::fabs(put[0] + 128.2f) < 0.01f);
        CHECK(CarRiderTick(r, true, 3.8f, -128.2f, 4.3f, 13, put) == RiderStep::riding);   // back on it
        CHECK(CarRiderTick(r, true, 3.6f, -126.0f, 4.1f, 13, put) == RiderStep::off);      // stepped off
        CHECK(CarRiderTick(r, true, 3.0f, -126.0f, -5.0f, 13, put) == RiderStep::idle);    // under it: not tracked
        // A gate or door that plays at the top and never moves: nobody is put back up to it.
        CarRider g;
        CHECK(CarRiderTick(g, true, 10.5f, -128, 10.5f, 13, put) == RiderStep::riding);
        CHECK(CarRiderTick(g, true, 10.5f, -128, 6.0f, 13, put) == RiderStep::riding);
        CHECK(CarRiderTick(g, true, 10.5f, -128, 1.0f, 13, put) == RiderStep::riding);
        // On a landing beside a car coming up from below: never tracked (not on it at the start).
        CarRider l;
        CHECK(CarRiderTick(l, true, -10.5f, -126, 10.5f, 13, put) == RiderStep::idle);
        CHECK(CarRiderTick(l, true, 9.0f, -126, 10.5f, 13, put) == RiderStep::riding);   // the car arrives
        CHECK(CarRiderTick(l, true, 9.5f, -126, 10.5f, 13, put) == RiderStep::riding);   // standing still: never
        // A car going up with the rider: always riding.
        CarRider u;
        CHECK(CarRiderTick(u, true, -10.5f, -128, -10.0f, 13, put) == RiderStep::riding);
        for (float y = -10.0f; y < 10.0f; y += 0.4f) CHECK(CarRiderTick(u, true, y - 0.5f, -128, y, 13, put) == RiderStep::riding);
    }

    // liftcar: the Frontier report at 60 FPS (car 3301000 down from 1492.71 at ~4 m/s, the rider carried
    // at ~9 m/s with its ride bit on, then the bit off and a fall). With the car's measured height every
    // put-back lands on the car's floor (car + 0.27), not lower each time.
    {
        RiderTrack r;
        float put[3] = {};
        CHECK(RiderTick(r, true, true, 0.0, 10.5f, 1492.98f, -36.0f, put, 330100002, 1492.71f) == RiderStep::riding);
        double t = 0;
        float car = 1492.71f, y = 1492.98f;
        for (int i = 0; i < 10; ++i) {  // 1 s: the car 4 m down, the rider 9 m, the bit on
            t += 0.1, car -= 0.4f, y -= 0.9f;
            const RiderStep s = RiderTick(r, true, true, t, 10.5f, y, -36.0f, put, 330100002, car);
            CHECK(s == RiderStep::riding || s == RiderStep::through);
            if (s == RiderStep::through) {
                CHECK(std::fabs(put[1] - (car + 0.27f + 0.3f)) < 0.01f);
                y = put[1];
            }
        }
        CHECK(y > car);  // still on the car
        // the bit off and a fall: back on the car's floor, not the old speed guess
        t += 0.1, car -= 0.4f;
        CHECK(RiderTick(r, true, false, t, 10.5f, car - 3.0f, -36.0f, put, 330100002, car) == RiderStep::through);
        CHECK(std::fabs(put[1] - (car + 0.57f)) < 0.01f && put[0] == 10.5f && put[2] == -36.0f);
        // stepping off the car sideways at the bottom: off, no put-back
        CHECK(RiderTick(r, true, false, t + 0.1, 12.5f, car + 0.27f, -36.0f, put, 330100002, car) == RiderStep::off);
        // a chr not on the car when it starts (standing at the bottom landing, the car 20 m up): idle
        RiderTrack w;
        CHECK(RiderTick(w, true, false, 0.0, 10.5f, 1471.0f, -36.0f, put, 330100002, 1492.0f) == RiderStep::idle);
        CHECK(RiderTick(w, true, true, 0.1, 10.5f, 1471.0f, -36.0f, put, 330100002, 1492.0f) == RiderStep::idle);
        // the put-backs a ride are capped: then the fall recovery has it
        RiderTrack c;
        CHECK(RiderTick(c, true, true, 0.0, 0, 10.27f, 0, put, 1, 10.0f) == RiderStep::riding);
        int puts = 0;
        for (int i = 1; i < 40; ++i) puts += RiderTick(c, true, false, 0.1 * i, 0, 0.0f, 0, put, 1, 10.0f) == RiderStep::through;
        CHECK(puts == kRiderMaxPutsMeasured);
        // lifts077: the car's height jumps to its end (the arrival animation) while the rider is still being
        // carried up 5 m behind: no put-back while it rises; stopped under the car it is put back
        RiderTrack u;
        CHECK(RiderTick(u, true, true, 0.0, 0, -85.0f, 0, put, 6, -85.5f) == RiderStep::riding);
        double ut = 0;
        float uy = -85.0f;
        for (int i = 0; i < 5; ++i) {
            ut += 0.1, uy += 1.0f;
            CHECK(RiderTick(u, true, true, ut, 0, uy, 0, put, 6, -78.0f) == RiderStep::riding);  // car at its end
        }
        int through = 0;
        for (int i = 0; i < 6; ++i) {
            ut += 0.1;  // stuck 3 m under the floor
            through += RiderTick(u, true, false, ut, 0, uy, 0, put, 6, -78.0f) == RiderStep::through;
        }
        CHECK(through >= 1);
        // lifts077: a car whose measured height never moves (Cainhurst / Research Hall): the rider carried
        // down 2 m with its bit on is no drop through the car; the speed guess takes over
        RiderTrack k;
        CHECK(RiderTick(k, true, true, 0.0, 0, 65.1f, 0, put, 1, 65.05f) == RiderStep::riding);
        double kt = 0;
        float ky = 65.1f;
        for (int i = 0; i < 20; ++i) {
            kt += 0.1, ky -= 0.2f;
            CHECK(RiderTick(k, true, true, kt, 0, ky, 0, put, 1, 65.05f) == RiderStep::riding);
        }
        CHECK(k.static_car);
        // car height not readable: the old speed guess is unchanged (NaN default)
        RiderTrack n;
        CHECK(RiderTick(n, true, true, 0.0, 0, 5.0f, 0, put, 1) == RiderStep::riding);
        CHECK(!n.has_off);
    }

    {
        // liftre077: a lift car asked to play before it is built holds its wait; far from everyone it does not
        CHECK(HoldCarAnim(true, true, false, true));
        CHECK(!HoldCarAnim(true, true, true, true));    // built: the animation plays for real
        CHECK(!HoldCarAnim(true, true, false, false));  // nobody near: the game's fast forward stands
        CHECK(!HoldCarAnim(true, false, false, true));  // not a lift car
        CHECK(!HoldCarAnim(false, true, false, true));  // BB_PARTY_LIFT_HOLD=0
        CHECK(!CarHoldExpired(kCarHoldMax - 0.1) && CarHoldExpired(kCarHoldMax + 0.1));
    }

    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("party-lift-rules-test: all checks passed\n");
    return 0;
}
