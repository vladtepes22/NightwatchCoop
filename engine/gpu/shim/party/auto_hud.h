// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-hide HUD (0.7.8, CL-7d51fe), the game side: finds the game's "PlayerHUD" group (HP and stamina bars, quick
// item / weapon / bullet slots, Blood Echoes and Insight counters) and fades its alpha by the rules in
// auto_hud_rules.h. Elden Ring's "HUD: Auto" is the model (docs/auto_hud.md). Solo and party; local only.
// Setting: bbport.ini auto_hide_hud (off by default; env BB_AUTO_HUD=0/1 over it).
#pragma once

namespace coop::autohud {

/// Hooks the HUD object's constructor (byte-verified). After HooksInit.
void Install();
/// Main thread, every frame.
void Tick();

}  // namespace coop::autohud
