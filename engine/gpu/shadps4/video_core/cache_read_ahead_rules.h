// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: shader cache read-ahead (cache_storage.cpp): how many threads read the cache files at start.
// BB_CACHE_READ_AHEAD: unset = automatic, 0 = off (files are read one at a time, as before),
// a number = the most threads to use. Test: ninja -C out/gpu cache-read-ahead-test.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>

namespace Storage::ReadAheadRules {

/// The most threads when the switch gives no number (from Lantern 0.9.40: 16).
constexpr unsigned DefaultMaxThreads = 16;
/// One thread per this many files, so a small cache is not read by a crowd.
constexpr size_t FilesPerThread = 64;
/// Upper bound for a number given by the switch.
constexpr unsigned HardMaxThreads = 64;

/// Threads to read `files` files with; 0 means read-ahead is off. `env` is the value of
/// BB_CACHE_READ_AHEAD or null.
inline unsigned ThreadCount(const char* env, size_t files) {
    unsigned cap = DefaultMaxThreads;
    if (env && env[0] != '\0') {
        if (env[0] == '0' && env[1] == '\0') {
            return 0;
        }
        char* end = nullptr;
        const long value = std::strtol(env, &end, 10);
        if (end != env && value >= 1) {
            cap = static_cast<unsigned>(std::min<long>(value, HardMaxThreads));
        } else if (env[0] == '0') {
            return 0;
        }
    }
    const size_t wanted = std::clamp<size_t>(files / FilesPerThread, 1, cap);
    return static_cast<unsigned>(wanted);
}

} // namespace Storage::ReadAheadRules
