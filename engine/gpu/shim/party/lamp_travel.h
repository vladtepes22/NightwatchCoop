// SPDX-License-Identifier: GPL-3.0-or-later
// Direct lamp travel (BB_LAMP_TRAVEL, launcher "Direct lamp travel", default on): a lamp in the
// world offers the headstones' travel list - the Hunter's Dream and every lit lamp, grouped as
// the headstones list them - in the game's own warp menu, and a choice travels straight there
// with one vanilla loading screen (the lamp warp 0x13CDF30, the headstones' own; the load resets
// the world like a Dream trip). Solo and in a party: in a party the warp is an ordinary player
// lamp warp, so the party travel rules apply unchanged (host: "Travel with the party?" on the
// host only, then the whole party moves; guest: the question on the guest, then its request).
//
// How (lamp_travel_rules.h for the ids): the world lamp's talk script runs the talk command
// OpenGenericDialog(2, 10010713 "Return to the Hunter's Dream?") on "Return to the Hunter's
// Dream". A replacement of the talk command switch 0x203FFD0 runs, instead of that one command,
// the headstones' own commands with the lamp's talk: ClearWarpList (0x14), AddWarpEntry (0x13)
// per entry (ints: value type 2), OpenWarpMenu (0x29, "Awaken Above Ground"; it also records the
// lamp as the last lamp, as resting there). The dialog result is reset to -1 so the script never
// reads a stale YES. The main-thread tick reads the chosen entry (MenuMan +0xEC0, the headstones'
// f23; -1 while open / after Return): the Hunter's Dream sets the lamp's own Dream flag (its
// common event 7200: the vanilla rest trip with its animation), any other lamp runs 0x13CDF30(id)
// as the player (a party's split question / host-led travel apply). Nothing is offered in the
// Dream, during a cutscene or a party's boss hold, or when no other lamp is lit (vanilla question).
//
// Test aid BB_LAMP_TRAVEL_TEST (scripted, in-process pad only): "continue" (solo: Cross on the
// title until the world is up), "members" (party host: wait for a cooperator), "lamp=ID" (stand
// at that lamp; default the nearest lit lamp of the map), "to=ID|other" (the entry to choose;
// other = the first lamp of another map), "start=ID" (first a plain lamp warp there when the save is in
// another map; "startany": also in the same map), "titleany" (title presses also in a party process
// whose host never answers: Right = CANCEL on the server error, Cross), "dream" (Cross / Left / Cross at
// the lamp - the vanilla Dream box - done once a load starts).
#pragma once

#include <cstdint>

namespace coop::lamptravel {

/// bbgpu_patch_image, before PartyInit: installs the talk command replacement (BB_LAMP_TRAVEL on)
/// and, without a party (no CoopTick), its own main-thread tick.
void Install(unsigned char* image, std::uint64_t size);
/// Main thread, once a frame (CoopTick in a party; its own tick otherwise).
void Tick();
/// Lamp travel hooked its own main-thread tick (solo with BB_LAMP_TRAVEL on): Tick() runs every
/// frame without a party (the world warper's tick rides on it).
bool OwnsTick();
bool ObservesTalkCommands();

} // namespace coop::lamptravel
