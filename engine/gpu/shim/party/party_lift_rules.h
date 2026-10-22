// SPDX-License-Identifier: GPL-3.0-or-later
// Lifts in a party (D3, docs/party/world_actions.md 10.1): the table of every lift of the main game
// and the DLC, and the decisions the party makes about them. Pure logic, no game access (unit test:
// tests/test_party_lift_rules.cpp); the game side is party_worldact.cpp (LiftReplayTick, the guest's
// ride report, the host's lift_set / lift_clr) and party_b2.cpp (the fall recovery's ride test).
//
// How a lift moves in a party (live, EMEVD traces of every game, lifts071 / liftfix): every game runs
// its own copy of the lift's events, and the game's own network sync starts the copies of the other
// games that have the lift's block loaded when any player starts a ride (a step into the car, a call
// lever). Each copy ends its ride by itself: the newer lifts once their busy flag clears, the
// Nightmare of Mensis' library lift (12601352) once its OWN player is out of the car's regions; that
// copy then reads the position flag to pick the end it plays and writes the flag (1: the car at the
// top). So the copies of a ride ridden together end at different moments, and the world mirror can
// bring the host's flip to a guest while the guest's copy has not read the flag yet.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace coop::liftrules {

/// A lift. `pos` is where the car is (the up event sets it 1, the down event 0, or the other way
/// round), `busy` is on while a ride runs (and after it until every player left the car), `host_state`
/// the newer lifts' "host's state" copy of `pos` (event `...ホスト状態管理`, 0 = none), `car` the car's
/// map object. A game with the lift's block streamed out never runs its copy: its world keeps the car
/// at the old end, and the block's init event puts the car there when the block streams in again.
/// Lifts whose ride ends with the flag alone (busy 0, no other "ride over" state) replay: `gate` and
/// the end animations are what their own copy plays right before it writes `pos` (the car and its gate
/// standing at that end); `ride_from_on` / `ride_from_off` the car's ride animations from that end (the
/// copy has not read the flag for its end yet while the car shows one of them), `init_on` / `init_off`
/// the poses the block's init event (2005[7]) puts the car in.
struct Lift {
    std::uint32_t pos, busy, host_state;
    std::int32_t car;
    std::int32_t gate = 0;
    std::int16_t car_end_on = -1, car_end_off = -1, gate_end_on = -1, gate_end_off = -1;
    std::int16_t ride_from_on[2] = {-1, -1}, ride_from_off[2] = {-1, -1};
    std::int16_t init_on = -1, init_off = -1;
    /// liftfix: `gate` is a second car riding the other way in the same event (the library lift's
    /// second cage): the in-car and on-a-moving-car tests look at both.
    bool gate_rides = false;
    bool Replays() const { return car_end_on >= 0 && car_end_off >= 0; }
};

/// Every lift of the main game and the DLC. Checked against the game data (tools/party/lift_tool.py
/// check GAME: every event named エレベ.. with a ride into a region and a waited car animation, and a
/// scan of every event of every map for an unnamed one - none): 21 lifts. The Chalice Dungeons'
/// lifts are generated (thousands of layout slots of m29's 12900240.. / 12900315.. / 12900347..
/// events, cars 2901001..); their copies are network-synced like the main game's, and the fall
/// recovery's rate rule (deathrules::FallRecoveryDue) covers them without a table.
inline const std::vector<Lift>& Lifts() {
    // flag_tool emevd + an instruction dump of every elevator event (lifts071): {pos, busy, host
    // state, car}. Old lifts (down / up events only): busy is set at the ride's start and cleared
    // with pos at its end. Newer ones (init / host state / up / down): pos flips at the start.
    static const std::vector<Lift> lifts = {
        {12200125u, 12200126u, 0u, 2201210},         // Hemwick
        {12400150u, 12400151u, 0u, 2401101},         // Cathedral Ward (church lift)
        {12400827u, 12400828u, 0u, 2401102},         // Cathedral Ward <-> Old Yharnam
        {12410350u, 12415350u, 12410351u, 2411320},  // Central Yharnam
        {12420154u, 12420155u, 0u, 2421250},         // Upper Cathedral Ward (to Ebrietas)
        // Cainhurst Castle (liftfall): it replays (live liftfix run 5: the host sat in its first-arrival
        // cutscene, its copy did not follow the guest's ride, and the host's world kept the car at the top
        // as "loaded, its copy did not follow"): a guest's ride is applied with the car loaded and the
        // car put at that end. Down 12500077: car 0, end 1, pos 1 (the car at the bottom); up 12500076:
        // car 10, end 11, pos 0. Init: pos 1 car 1, pos 0 car 11.
        {12500074u, 12505074u, 0u, 2501010, 0, 1, 11, -1, -1, {10, -1}, {0, -1}, 1, 11},
        // Nightmare of Mensis (liftfall: its common up / down events 12601320 / 12601330, slots 0-5)
        {12601250u, 12605250u, 0u, 2601210},
        {12601251u, 12605251u, 0u, 2601220},         // roof lift (start state reversed, 12601340)
        {12601252u, 12605252u, 0u, 2601230},
        {12601254u, 12605253u, 0u, 2601280},         // roof lift
        {12601255u, 12605254u, 0u, 2601290},         // roof lift
        {12601256u, 12605256u, 0u, 2601240},         // first shortcut lift (28 m, lamp 2602953)
        // the library lift (12601351 / 12601352, after Micolash: the 60 m ride down of the player's
        // report): no busy flag; TWO cages, 1.7 m apart, riding opposite ways in one event (liftfix ride
        // frames): 2601260 (regions 2602241 / 2602242, bottom / top) and 2601261, so far called its gate
        // (regions 2602243 / 2602244). Pos 1: cage 1 at the top, cage 2 at the bottom. From pos 1 (a step
        // into cage 1 at the top or cage 2 at the bottom): cage 1 6, 3, cage 2 8, 5; from pos 0: cage 1
        // 8, 5, cage 2 6, 3. Its end once every rider is out: pos 1 -> cage 2 7 / cage 1 9, pos 0; pos 0
        // -> cage 2 9 / cage 1 7, pos 1. Init: pos 1 cage 1 2, pos 0 cage 1 4.
        {12601253u, 0u, 0u, 2601260, 2601261, 7, 9, 9, 7, {6, 3}, {8, 5}, 2, 4, true},
        {12700134u, 12700135u, 0u, 2701032},         // Forbidden Woods
        {12700144u, 12700145u, 0u, 2701042},         // Forbidden Woods
        {12800610u, 12804700u, 12800611u, 2801400},  // Yahar'gul
        {13300115u, 13300116u, 0u, 3301000},         // Nightmare Frontier (liftfall)
        {13501106u, 13504100u, 13501107u, 3501100},  // m35 altar lift (DLC)
        {13501116u, 13504110u, 13501117u, 3501105},  // m35 1F <-> 3F (DLC)
        {13501126u, 13504120u, 13501127u, 3501110},  // m35 3F <-> 5F (DLC)
        {13601106u, 13604100u, 13601107u, 3601100},  // Fishing Hamlet (DLC)
    };
    return lifts;
}

/// liftfix: the lift events whose ride ends once the LOCAL player has left the car (3[2] on entity
/// 10000, "outside"): Cainhurst's (12500076 down / 12500077 up) and the Nightmare of Mensis' library
/// lift (12601352). Every other lift waits until ALL players are out (3[7]). In a party every game
/// runs its copy and the game's own sync passes a condition for every copy once one copy passes it
/// (live, EMEVD traces of both games: the guest's copy went past its "outside" wait 0.1 s after the
/// host's while the guest stood in the car, then its start wait at once): the copy of a player who is
/// not in the car ends every game's ride the moment the car arrives, with the rider still in it, and
/// the restarted copy sees the rider in its start region and sends the car straight back - the rider
/// rides back and forth until it steps out at the right moment (the 0.7.1 full smoke: 5-6 rides for 3;
/// liftfix run 2: 15 for 3). Their end checks become the all-players check of the other lifts.
constexpr std::int32_t kLocalEndRideEvents[] = {12500076, 12500077, 12601352};
inline bool LocalEndRideEvent(std::int32_t event_id) {
    for (const std::int32_t e : kLocalEndRideEvents) {
        if (e == event_id) {
            return true;
        }
    }
    return false;
}

/// liftfix: instruction (bank, id) with `args` (`size` bytes) of event `event_id` is such an end check
/// (3[2]: s8 group, u8 state 0 = outside, s32 entity 10000 = the local player, s32 region): it becomes
/// 3[7] (s8 group, u8 state, s32 region): the region moves to +4. True when `args` was changed (the
/// caller then writes id 7). Start checks (state 1, inside) stay the local player's. Also for id 7 with
/// the old bytes: the id the caller wrote stays in the loaded script, but every run of the event gets
/// its argument bytes afresh from it (live run 3: the second ride's 3[07] read 10000 as its region and
/// never ended), so each dispatch moves the region again.
inline bool LocalEndToAllPlayers(std::int32_t event_id, std::int32_t bank, std::int32_t id, std::uint8_t* args,
                                 std::uint32_t size) {
    if (bank != 3 || (id != 2 && id != 7) || size < 12 || !LocalEndRideEvent(event_id) || args[1] != 0) {
        return false;
    }
    std::int32_t entity = 0, region = 0;
    std::memcpy(&entity, args + 4, 4);
    std::memcpy(&region, args + 8, 4);
    if (entity != 10000 || region <= 0) {
        return false;
    }
    std::memcpy(args + 4, &region, 4);
    std::memset(args + 8, 0, 4);
    return true;
}

/// liftfix (a player's report: "I was riding an elevator and went through the ground and died", the
/// Research Hall lift 3501105 at 60 FPS: the host on the car at the top, HP 0 37 m lower, 11 m under
/// the shaft's bottom floor, with the game's fall state set and its ride bit cleared): a rider that
/// drops through its moving car. The game keeps a rider on its car by the chr's ride bit (PlayerIns
/// +0x1E5 bit 0x02, deathrules::RidingObject); in the report the bit cleared while the car still played
/// its ride and the chr stood where it had stood on the car, then fell faster than the car (the car's
/// step past the chr's ground probe on a long frame: the log shows render stalls there). The rider is
/// put back on the car: where it last rode, at the car's height now (that sample's height carried on
/// at the car's speed) plus a little, so it lands on the car's floor again.
struct RiderTrack {
    bool riding = false;
    double t = 0;
    float x = 0, y = 0, z = 0, vy = 0;  // the last sample on the car, the car's vertical speed (m/s)
};
constexpr float kRiderSlip = 1.0f;      // m below the car's height before it counts as a drop through it
constexpr float kRiderSideways = 1.0f;  // m sideways from the last ride sample: stepped off (no put-back)
constexpr double kRiderWindow = 2.0;    // s after the last ride sample: no longer this ride
enum class RiderStep { idle, riding, through, off };
/// One sample of the local chr while `car_playing` (a lift car within reach plays a ride animation).
/// `put` gets the spot to put it back at for `through`.
inline RiderStep RiderTick(RiderTrack& r, bool car_playing, bool ride_bit, double t, float x, float y, float z,
                           float put[3]) {
    if (!car_playing) {
        r = RiderTrack{};
        return RiderStep::idle;
    }
    if (ride_bit) {
        const double dt = t - r.t;
        if (r.riding && dt > 0.0 && dt < 0.5) {
            const float v = float((y - r.y) / dt);
            if (std::fabs(v) < 16.0f) {  // a lift's speed (deathrules::kLiftMaxRate)
                r.vy = v;
            }
        } else if (!r.riding) {
            r.vy = 0;
        }
        r.riding = true;
        r.t = t, r.x = x, r.y = y, r.z = z;
        return RiderStep::riding;
    }
    if (!r.riding) {
        return RiderStep::idle;
    }
    const double dt = t - r.t;
    const float side = std::hypot(x - r.x, z - r.z);
    if (dt > kRiderWindow || side > kRiderSideways) {
        r = RiderTrack{};
        return RiderStep::off;
    }
    const float car = r.y + r.vy * float(dt);
    if (y >= car - kRiderSlip) {
        return RiderStep::riding;  // the bit flickers (a roll, a jump in the car): still on it
    }
    put[0] = r.x, put[1] = car + 0.3f, put[2] = r.z;
    r.t = t, r.y = car;  // the next sample measures from here
    return RiderStep::through;
}

/// liftfix (the Research Hall report, live runs 11-15): the Research Hall's two lifts (1F <-> 3F car
/// 3501105, 3F <-> 5F car 3501110) clear their busy flag in the frame their ride's "every player out of
/// the arrival box" check passes (13501112 / 13 / 22 / 23: 3[7] then 2003[2] busy = 0), while the other
/// direction's start check "the player in that same box" (3[2]) still reads the player inside for that
/// frame. So the car leaves again the moment its rider steps out of the box: live, alone and in a party,
/// at 30 and 60 FPS, the car went back up from the bottom with the host still on its wide platform or
/// just off it, and the host fell into the 12 m pit under the car (HP 0 at y 1546, the report's spot).
/// Every other lift of the game clears busy only after its end animation (or after a 1 s wait: the
/// Research Hall altar lift 13501102 / 03). Here the clear is moved to the event's end, after the end
/// animation: the instruction is rewritten to keep busy on, and the end (1000[4]) clears it.
struct BusyDefer {
    std::int32_t event;
    std::int32_t clear_index, end_index;  // the 2003[2] busy = 0 right after the 3[7]; the 1000[4] end
    std::uint32_t busy;
};
inline const std::vector<BusyDefer>& BusyDefers() {
    static const std::vector<BusyDefer> d = {
        {13501112, 26, 30, 13504110u},  // Research Hall 1F <-> 3F, up
        {13501113, 26, 30, 13504110u},  // ... down
        {13501122, 26, 30, 13504120u},  // Research Hall 3F <-> 5F, up
        {13501123, 26, 30, 13504120u},  // ... down
    };
    return d;
}
enum class BusyStep { none, keep, clear };
/// The instruction (bank, id) at `index` of event `event_id`: `keep` (rewrite the busy clear to keep it
/// on: its flag must be `flag`), `clear` (the event's end: clear busy now), or none.
inline BusyStep BusyDeferStep(std::int32_t event_id, std::int32_t bank, std::int32_t id, std::int32_t index,
                              std::uint32_t flag, const BusyDefer** which) {
    for (const BusyDefer& d : BusyDefers()) {
        if (d.event != event_id) {
            continue;
        }
        if (which) *which = &d;
        if (bank == 2003 && id == 2 && index == d.clear_index && flag == d.busy) {
            return BusyStep::keep;
        }
        if (bank == 1000 && id == 4 && index == d.end_index) {
            return BusyStep::clear;
        }
    }
    return BusyStep::none;
}

/// The lift whose position flag is `pos`, or nullptr.
inline const Lift* LiftByPosFlag(std::uint32_t pos) {
    for (const Lift& l : Lifts()) {
        if (l.pos == pos) {
            return &l;
        }
    }
    return nullptr;
}

/// The lift whose car is map object `car`, or nullptr.
inline const Lift* LiftByCar(std::int32_t car) {
    for (const Lift& l : Lifts()) {
        if (l.car == car) {
            return &l;
        }
    }
    return nullptr;
}

/// What a replaying lift's car animation says: `riding` (one of its ride animations: this game's copy
/// rides, or its car arrived and the copy waits for its player to leave the car; it has not read the
/// flag for its end yet), `at_on` / `at_off` (an end or init pose: the car stands at the end of pos 1 /
/// pos 0), `unknown` (no animation yet, or a lift that does not replay).
enum class CarPhase { unknown, riding, at_on, at_off };
inline CarPhase PhaseOf(const Lift& l, std::int32_t anim) {
    if (!l.Replays() || anim < 0) {
        return CarPhase::unknown;
    }
    for (const std::int16_t a : {l.ride_from_on[0], l.ride_from_on[1], l.ride_from_off[0], l.ride_from_off[1]}) {
        if (a >= 0 && anim == a) {
            return CarPhase::riding;
        }
    }
    if (anim == l.car_end_on || anim == l.init_on) {
        return CarPhase::at_on;
    }
    if (anim == l.car_end_off || anim == l.init_off) {
        return CarPhase::at_off;
    }
    return CarPhase::unknown;
}

/// The car of a replaying lift stands at the end its position flag names (both games after a ride:
/// the same flag and the car at that end; the smoke checks it after every ride).
inline bool CarAgreesWithFlag(const Lift& l, bool pos, std::int32_t anim) {
    return PhaseOf(l, anim) == (pos ? CarPhase::at_on : CarPhase::at_off);
}

/// liftfix: the position flag of a replaying lift changed on this game without its own write (the
/// mirror brought the host's flip, the host took a guest's ride): put back until this game's copy has
/// played its own end? Only while its car shows a ride animation: the copy then still has to read the
/// flag to pick its end (12601352: "goto if 12601253 on" once its player left the car), and a flip
/// would make it play the other end's animation and write the old value back. Once its car shows an end
/// pose the copy has read the flag (or never rode): a flip is the other game's ride (replayed) or the
/// same value its copy writes. (0.7.1 held it while the player stood within a few m of the car: the
/// copy's own end, in the same tick as the host's flip, was put back too, and the guest kept the old
/// end until its next ride: full smoke 0.7.1, "kept at 1 until its end" after the car's anim 9 ended.)
inline bool HoldMirrorFlip(const Lift& l, std::int32_t car_anim) {
    return PhaseOf(l, car_anim) == CarPhase::riding;
}

/// liftfix: a guest's world mirror holds a replaying lift's position flag back (progress::SetMirrorHold:
/// the host's flip waits) while this game's copy of a ride has not written its own end yet: its car
/// shows a ride animation (the copy reads the flag once its player left the car) or an end animation
/// still plays (the copy writes the flag when it stops). Released, the copy's own end stands (the
/// mirror's image may still hold the host's end of the ride before - live run 2 wrote that stale end
/// back over the guest's own); a later change of the host's goes in as usual, and a guest's end that
/// differs from the host's world goes up as its ride report (GuestLiftReport).
inline bool MirrorHold(const Lift& l, std::int32_t car_anim, bool playing) {
    const CarPhase p = PhaseOf(l, car_anim);
    return p == CarPhase::riding || ((p == CarPhase::at_on || p == CarPhase::at_off) && playing);
}

/// What LiftReplayTick does when `pos` changed on this game (and the flip is not held): nothing when
/// the car stands at the new end already (its own copy rode there) or the lift does not replay; else
/// the end animations of that end are played (the car and its gate to the other game's end).
inline bool LiftNeedsReplay(const Lift& l, bool pos_now, std::int32_t car_anim) {
    return l.Replays() && !CarAgreesWithFlag(l, pos_now, car_anim);
}

/// The host's answer to a guest's finished ride (lift_set / lift_clr): `same` the host's world has
/// that end already (its own copy ran: the usual case), `busy` a ride runs in the host's world (its
/// own copy decides), `loaded` the car is loaded in the host's game but its copy did not follow
/// (left alone: setting the flag would not move the car), `apply` the lift's block is not loaded
/// here: the flags are written and the block's init event puts the car at that end when it
/// streams in.
/// liftfall: a lift that `replays` (Lift::Replays) is applied with its car loaded too (the host's
/// LiftReplayTick then puts the car at that end), unless the host stands at it (`occupied`: within
/// a few m of the car, or the car rides: its own copy decides).
enum class LiftSync { apply, same, busy, loaded, occupied };
inline LiftSync HostLiftSync(bool host_pos, bool host_busy, bool car_loaded, bool want, bool replays = false,
                             bool occupied = false) {
    if (host_pos == want) {
        return LiftSync::same;
    }
    if (host_busy) {
        return LiftSync::busy;
    }
    if (car_loaded && replays) {
        return occupied ? LiftSync::occupied : LiftSync::apply;
    }
    return car_loaded ? LiftSync::loaded : LiftSync::apply;
}

/// Guest: a lift position change of its own copy goes to the host once the ride is over (`busy`
/// off) and the flag still reads the value it changed to; false while it runs.
inline bool GuestLiftRideDone(bool pos_now, bool pos_changed_to, bool busy) {
    return !busy && pos_now == pos_changed_to;
}

/// liftfix: the guest's report of a finished ride, per lift: `send` it to the host (the host's world
/// has the other end and this end was not sent yet), `same` (the host's world has it: nothing to
/// send; forget what was sent), `sent` (sent already: the host answered, or will; never again until
/// the host's world agrees or this end changes). `last_sent`: the end sent last (-1 none). 0.7.1 kept a
/// "seen" list of (flag, value) for the whole stay in a map, so a second ride to an end the guest had
/// reported once never went up again (a host with the lift's block streamed out kept the old end).
enum class GuestReport { wait, same, send, sent };
inline GuestReport GuestLiftReport(bool pos_now, bool host_pos, bool busy, int last_sent) {
    if (busy) {
        return GuestReport::wait;
    }
    if (pos_now == host_pos) {
        return GuestReport::same;
    }
    return last_sent == (pos_now ? 1 : 0) ? GuestReport::sent : GuestReport::send;
}

// ---- Chalice Dungeon lifts (chalicelamp) ----
// A guest's report ("I died on the elevator"): on a Pthumeru Chalice floor lift (21 m, 10.5 -> -10.5) the
// guest's chr ended 5 m under the car with the game's fall-death state, its drop taken as 18.9 m: the
// table above has no Chalice Dungeon lift (their layouts are generated), so neither the on-a-moving-car
// test nor the rider guard knew the car. The dungeons' lift events are m29's (one copy per layout slot,
// started from each layout's script); every ride plays the car's animation with 2003[18] on the car
// entity (2901001.. of the layout). The game side learns the cars from those instructions as they run.
/// m29's lift events that animate a car (2003[18]): the altar lift (祭壇エレベーター 12900245 down /
/// 12900250 up), the cage lift (籠牢 12900323 / 12900331), the floor lifts (フロア移動 21 m 12900351 /
/// 12900354, 42 m 12900353 / 12900356), the two balance lifts (天秤 12900361..12900375, two cars each).
constexpr std::int32_t kChaliceLiftEvents[] = {12900245, 12900250, 12900323, 12900331, 12900351, 12900353,
                                               12900354, 12900356, 12900361, 12900363, 12900365, 12900367,
                                               12900369, 12900371, 12900373, 12900375};
inline bool ChaliceLiftEvent(std::int32_t event_id) {
    for (const std::int32_t e : kChaliceLiftEvents) {
        if (e == event_id) {
            return true;
        }
    }
    return false;
}
/// The car of a Chalice Dungeon lift instruction: 2003[18] (s32 entity, s32 anim, ...) of one of the
/// events above, its entity a map object of the dungeon (29xxxxx). 0: not one.
inline std::int32_t ChaliceLiftCar(std::int32_t event_id, std::int32_t bank, std::int32_t id, std::int32_t entity) {
    return bank == 2003 && id == 18 && ChaliceLiftEvent(event_id) && entity / 100000 == 29 ? entity : 0;
}
/// The local chr counts as on a learned Chalice car (the on-a-moving-car test) only within this height
/// of the car object (a lift's other animated objects, gates and doors, stay at their end).
constexpr float kChaliceCarBand = 3.0f;
/// The rider guard for a car the chr's ride bit says nothing about (the dungeon cars; the local chr's bit
/// is not known to be set there): the chr's height against the car object's own height (ObjectPoint).
/// At the ride's first sample the chr stands within kChaliceCarBand of the car: its offset to the car is
/// kept. A drop through the car: the chr falls (lower than its last sample) to more than kRiderSlip under
/// car + offset while within kRiderSideways of its last sample on the car, and the car has moved since
/// the ride began (an object that never moves, a gate, never puts anyone anywhere).
struct CarRider {
    bool on = false;
    float off = 0, car_y0 = 0, x = 0, y = 0, z = 0;
};
inline RiderStep CarRiderTick(CarRider& r, bool car_playing, float car_y, float x, float y, float z, float put[3]) {
    if (!car_playing) {
        r = CarRider{};
        return RiderStep::idle;
    }
    if (!r.on) {
        if (std::fabs(y - car_y) > kChaliceCarBand) {
            return RiderStep::idle;  // not on this car when its ride began (on a landing, under it)
        }
        r.on = true;
        r.off = y - car_y, r.car_y0 = car_y;
        r.x = x, r.y = y, r.z = z;
        return RiderStep::riding;
    }
    if (std::hypot(x - r.x, z - r.z) > kRiderSideways) {
        r = CarRider{};
        return RiderStep::off;  // stepped (or rolled) off
    }
    const float floor = car_y + r.off;
    const bool moved = std::fabs(car_y - r.car_y0) > 0.3f;
    if (y >= floor - kRiderSlip || y >= r.y || !moved) {
        if (y >= floor - kRiderSlip) {
            r.x = x, r.y = y, r.z = z;
        }
        return RiderStep::riding;
    }
    put[0] = r.x, put[1] = floor + 0.3f, put[2] = r.z;
    r.y = floor + 0.3f;
    return RiderStep::through;
}

}  // namespace coop::liftrules
