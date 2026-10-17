// SPDX-License-Identifier: GPL-3.0-or-later
// The world warper (BB_WORLD_WARPER; launcher Advanced > "Build shader cache (world warper)"):
// a solo test mode that fills the shader cache by itself. It continues the save on the title,
// then warps through every ReturnPointParam row with a spot (the Hunter's Dream, every lamp, the
// boss-arena and event respawn points; lamp warp 0x13CDF30, one vanilla loading screen each),
// and in each area turns the camera all the way round, walks a loop, uses the weapon (both
// forms, charged, firearm, dodges), fights and kills the nearest enemies, and once per run
// switches through the other weapons and quick items. The player's HP is kept full meanwhile.
//
// It counts the pipelines new to the cache per area and per action (BbCompileProgress
// created_total) and the hitches (frames over 50 ms), logs a line per area ("World warper:
// [12/75] ..."), shows its progress in a small panel at the top of the screen and appends each
// area to a progress file (BB_WORLD_WARPER_STATE; default <user>\world_warper_state.txt) so a
// restarted game resumes where the last one stopped (a row the game crashed in twice is skipped).
// At the end it writes "finished", flushes the caches and ends the game; the launcher then
// exports the shader cache as one .zip. Rules (route, options, progress file):
// world_warper_rules.h. Solo only (not in party mode); runs on a copy of the save folder (the
// launcher's job: the run changes the character's position, items and last lamp).
#pragma once

#include <cstdint>
#include <string>

namespace coop::warper {

/// bbgpu_patch_image, after lamptravel::Install: parses BB_WORLD_WARPER and, when lamp travel did
/// not hook the main-thread tick, hooks it itself.
void Install(unsigned char* image, std::uint64_t size);
/// Main thread, once a frame (from lamp travel's tick or our own).
void Tick();
/// The overlay's progress panel: false while the warper is off.
bool Active();
/// Up to three lines for the panel (title, area, counts).
std::string StatusText();

} // namespace coop::warper
