// SPDX-License-Identifier: GPL-3.0-or-later
// Builds the party game-data tables from the player's own game files on a worker pool, with a cache in the
// user folder. docs/party/gamedata.md.
#pragma once

#include "derive.h"
#include "parallel.h"

#include <string>

namespace coop::gamedata {

/// `path` or `path/dvdroot_ps4`, whichever holds the game's files.
std::string GameRoot(const std::string& path);

/// Everything the cache holds, derived from the game files under `game_root` (a dvdroot_ps4 folder) with
/// `threads` threads. Progress / timings are optional. Throws (FormatError, runtime_error) when a file is
/// unreadable or unexpected.
Sections DeriveAll(const std::string& game_root, unsigned threads, Progress* progress, Timings* timings);

/// Cache text: `BBPARTYTABLES 1`, `key <key>`, then `[section] <rows>` + tab separated rows, sections sorted.
std::string RenderCache(const std::string& key, const Sections& sections);

/// The cache key: derivation version + names, sizes and times of the inputs (params, event scripts, MSBs,
/// the map / remo listings). Empty when the game has no params.
std::string ComputeKey(const std::string& game_root);

/// The key of cache text, "" when it is not a cache of this format.
std::string CacheTextKey(const std::string& text);

/// Starts the build on a background thread (returns at once). `app0`: the game folder (else BB_GAME_DIR),
/// `user_dir`: where party_tables.txt is cached. Does nothing when BB_PARTY_TABLES names a cache file or
/// BB_PARTY_TABLES_BUILD=0. Progress goes to the console and to gamedata::Preparing() / PreparingPercent().
void StartBackground(const std::string& app0, const std::string& user_dir);

/// Foreground build to a cache file (the CLI and the tests): cache hit -> returns after the key check.
/// Returns false with `err` set on failure. `log`: one line per step.
bool BuildToFile(const std::string& game, const std::string& cache_path, bool force, unsigned threads,
                 std::string* err, std::string* log);

}  // namespace coop::gamedata
