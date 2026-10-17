// SPDX-License-Identifier: GPL-3.0-or-later
// The Hunter's Dream mirror's appearance editor in character creation's frame (H5, 0.6.2).
//
// Ported from droogie/bbhost (GPL-3.0-or-later; src/engine/change_appearance.cpp and
// menu_steps.cpp, offsets there are Ghidra's with a 0x400000 base, ours without it). The mirror's
// talk command opens character creation's Appearance list alone ("ChrMakeCommandList", menu step
// type 7). In character creation that list sits in the ChrMake_BG window (chrmake_bg.gfx: the
// book, the panels, the hunter's preview), which character creation's top controller opens; at the
// mirror nothing does, so the list floated over the world with the game's HUD drawn on top of it
// and an empty preview panel (live 2026-10-15). Here, the menu step constructor 0x1C1CCE0 is
// watched (an observer hook); when it makes that list while the hunter is in the world, the tick
// opens ChrMake_BG with a window descriptor filled as character creation fills its own
// (0x1AD8150), runs its controller's update every frame, holds a reference on the list's step and
// closes the window (0x1AD8E60) once ours is the last reference (the list has closed).
// BB_MIRROR_FRAME=0: off (the list alone, as before).
#pragma once

namespace coop::mirror {

/// Hooks the menu step constructor (byte-verified; solo and party). After HooksInit.
void Install();
/// Main thread, every frame (the lamp travel / director tick).
void Tick();

}  // namespace coop::mirror
