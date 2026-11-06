// SPDX-License-Identifier: GPL-3.0-or-later
// party-tables-cli GAME_DIR OUT_FILE [--force] [--threads N]: the engine's table derivation from the command
// line (tests compare its output with the Python oracle scripts/partydata/oracle.py; timings per step).
#include "gamedata/build.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: party-tables-cli GAME_DIR OUT_FILE [--force] [--threads N]\n");
        return 2;
    }
    bool force = false;
    unsigned threads = 0;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--force")) force = true;
        else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc) threads = static_cast<unsigned>(std::atoi(argv[++i]));
    }
    std::string err, log;
    const bool ok = coop::gamedata::BuildToFile(argv[1], argv[2], force, threads, &err, &log);
    std::fputs(log.c_str(), stdout);
    if (!ok) {
        std::fprintf(stderr, "failed: %s\n", err.c_str());
        return 1;
    }
    return 0;
}
