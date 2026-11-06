// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests that need the game's data (map / lamp / lot tables) read the party tables the launcher builds
// from the player's own game files; nothing extracted is committed. Build the cache once, then point the
// test at it:
//   out/gpu/party-tables-cli.exe <game dir> <some dir>/party_tables.txt
//   BB_PARTY_TABLES=<some dir>/party_tables.txt out/gpu/<test>.exe
// Without the cache the test prints "skipped" and succeeds.
#pragma once

#include "party/party_gamedata.h"

#include <cstdio>

namespace coop::test {

/// Installs the tables named by BB_PARTY_TABLES (else the default user\party_tables.txt). False: absent.
inline bool UseGameTables(const char* who) {
    if (gamedata::InstallFromEnv()) {
        return true;
    }
    std::printf("%s: skipped (no party tables: see tests/party_tables_test.h)\n", who);
    return false;
}

}  // namespace coop::test
