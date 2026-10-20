// SPDX-License-Identifier: GPL-2.0-or-later
// Loose-file mods and a game update kept beside the game (was scripts/mods.py): a folder of
// links over the game - junctions for folders, hard links for files - with the mods' files in
// place of the game's. The game and the mods are never written.
#pragma once
#include "common.h"

namespace start {

/// The game folder to start: `game` itself, or an overlay built in `out` (removed with RemoveOverlay).
fs::path BuildModsOverlay(const fs::path& game, const fs::path& out, const fs::path& mods_dir, const fs::path& config,
                          bool enabled);
/// Deletes an overlay without following its links.
void RemoveOverlay(const fs::path& path);

} // namespace start
