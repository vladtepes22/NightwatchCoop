// SPDX-License-Identifier: GPL-3.0-or-later
// Bullet debug log for modding (BB_DEBUG_BULLETS): every bullet's param id, position, stage changes,
// end and character hits, so a broken bullet chain shows up in a headless run's log.
#pragma once

namespace coop::bulletdebug {

/// Installs the byte-verified hooks when BB_DEBUG_BULLETS is set ("1" / "all", or id ranges such as
/// "485-600,2000"); nothing otherwise.
void Init();
/// Main-thread frame tick: walks the live bullet list and logs new bullets and stage changes.
void Tick();

} // namespace coop::bulletdebug
