// SPDX-License-Identifier: GPL-3.0-or-later
// 0.8.1 seamless join and party travel (seam081; design research/reseam081.md 3).
//
// Two hooks, both pass-through unless a toggle uses them (BB_PARTY_SEAM_HOOKS=0: neither is installed):
//  - InGameStart's NowLoading clear (call 0x176D3C0 at 0x193A755, inside InGameStart 0x193A3E0): the loading
//    screen's drop can be held (A: until the guest stands beside the host) or deferred (B: until frames settle).
//  - the GX frame end (bbport_stall_probe.cpp's call-site hook at 0x1d97853): the quiet point after the frame
//    end returned (the render thread idle, the next frame not handed yet). The held drop and queued party
//    work run there.
//  - native hold (0.8.2): MoveMapStep's screen gate (step 12, 0x193A3E0) is kept shut by writing its hold timer +0x278
//    from a prologue hook (remp081 3.3), so InGameStart itself waits (nothing of the world starts under the held screen).
//    Opt-in (BB_PARTY_NATIVE_HOLD=1; +0.7 s measured); the InGameStart clear hook above is the default and the fallback.
//  - in-load placement (0.8.2, C): the spawn position the load resolves (MoveMapStep +0xF0 / +0x100; 0x19423A0 resolves it,
//    0x1939C80 applies it) is replaced by the beside-host spot before it is applied (BB_PARTY_LOAD_PLACE=0: off).
// Toggles (My party: Loading, launcher My Party tab, bbport.ini; off by default):
//  A party_parallel_follow (BB_PARTY_PARALLEL_FOLLOW): the guest loads the host's party lamp at once (party_b2.cpp
//    GuestPhase::ParallelLoading), the screen held until it is placed beside the host.
//  B party_settled_drop (BB_PARTY_SETTLED_DROP): in a party, every InGameStart drop waits until frames run under 50 ms
//    for 1 s without a break (at most 3 s) and runs at the quiet point; BB_PARTY_QUIET_POINT=0 runs the queued work in the tick.
#pragma once

#include <functional>

namespace coop::seam {

/// bbgpu_patch_image after coop::HooksInit (every mode; nothing runs unless a toggle is on).
void Install();

bool ParallelFollowOn();  ///< A's setting, party process only
bool SettledDropOn();     ///< B's setting, party process only
bool HoldHookIn();        ///< the InGameStart clear hook is in (the screen can be held)

/// A (main thread): the next / current loading screen stays up past InGameStart until ReleaseHold.
void Hold(const char* why);
/// Ends the hold. `drop`: the screen goes now (B on: once frames settled, at the quiet point). Not `drop`:
/// the hold is forgotten and the screen stays up for the caller's next load (whose InGameStart clears it).
/// Returns whether InGameStart's clear had been held (the load ended under the hold).
bool ReleaseHold(const char* why, bool drop);
bool Holding();
/// A drop is held or deferred right now (the party overlay stays hidden as under a loading screen).
bool ScreenHeld();

/// The native hold is in (step 12's prologue hook installed and not switched off).
bool NativeHoldIn();
/// The hold in force is the native one: InGameStart has not run under it (it runs at the release).
bool HoldIsNative();
/// The native hold keeps step 12 shut right now (its gate has been seen held): game_state.cpp reports no load then.
bool NativeHeld();

// ---- C: in-load placement ----
/// The in-load placement hooks are in.
bool PlacementHookIn();
/// Main thread, before the load starts: the next MoveMapStep of map block `stage` (high 16 bits) spawns its player at
/// `pos` / `rot` (the game's own position vector / rotation vector formats) instead of the game's own spot, if that is
/// within seam_rules.h kPlaceMaxShiftM of it. If the placement is not written, the loading screen is held (as C did
/// before) so the caller's arrival warp stays hidden. Nothing happens when the hooks are not in.
void ArmPlacement(unsigned stage, const float pos[4], const float rot[4]);
/// Forgets an armed placement (the load ended, or the follow was given up).
void DisarmPlacement(const char* why);
struct PlacementState {
    bool armed = false;    ///< armed and not used yet
    bool applied = false;  ///< written into the load (this load)
    float shift_m = -1;    ///< from the game's own spot to the written one
};
PlacementState Placement();

/// Runs `fn` on the main thread at the next frame end's quiet point (B on and the frame-end hook in), else at
/// once. In order; at most ~4 ms of them per frame (the rest the next frame).
void AtQuietPoint(const char* name, std::function<void()> fn);

}  // namespace coop::seam
