// SPDX-License-Identifier: GPL-2.0-or-later
// The game's boot image (was scripts/prepare.py, game_check.py, link_modules.py and
// content_profile.py): the eboot's loaded image with its relocations and imports, the bundled
// system modules linked after it, and the AppContent profile. Writes out/boot-linked.bin,
// out/eboot.elf and out/content.bin; reused while what they are made from is unchanged.
#pragma once
#include "common.h"

namespace start {

/// Game files bbport does not run (exit code 2, as prepare.py).
struct GameCheckError : Error {
    using Error::Error;
};

/// `engine` is the engine folder (patches/redzone.json, data/image_109_hashes.txt).
void PrepareImage(const fs::path& game, const fs::path& out, const fs::path& engine);

} // namespace start
