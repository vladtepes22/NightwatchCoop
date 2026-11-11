// SPDX-License-Identifier: GPL-3.0-or-later
// Converted game files (zstd081): BB_ZSTD_DIR names a folder made by bb-zstdpack (tools/zstdpack). When the game opens
// /app0/dvdroot_ps4/<path> and the folder holds a converted <path> (listed in its bbzstd.manifest), the open goes to
// the converted file, a DCX container whose zlib stream was recompressed with zstd. The bytes the engine reads
// through its inflate hook (bbport_zstd_inflate.h) are the same as the original's. The original is used when the
// converted file is missing, or when the original's size or modification time differ from the manifest's (a mod that
// replaces the file wins). Host-side readers (party tables, game-data checks, the launcher's tools) are unaffected:
// they read the game folder itself.
#pragma once

#include <string>

namespace BbZstdFiles {
/// Loads <dir>/bbzstd.manifest and routes the runtime's /app0 file lookups. False (and nothing changes) when the
/// manifest is missing or unreadable. Call once, before the game opens files.
bool Enable(const std::string& dir);
}  // namespace BbZstdFiles
