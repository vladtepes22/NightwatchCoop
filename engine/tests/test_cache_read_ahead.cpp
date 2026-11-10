// SPDX-License-Identifier: GPL-2.0-or-later
// Shader cache read-ahead thread count (gpu/shadps4/video_core/cache_read_ahead_rules.h).
// ninja -C out/gpu cache-read-ahead-test
#include "video_core/cache_read_ahead_rules.h"

#include <cassert>
#include <cstdio>

using namespace Storage::ReadAheadRules;

int main() {
    assert(ThreadCount(nullptr, 0) == 1);
    assert(ThreadCount(nullptr, 63) == 1);
    assert(ThreadCount(nullptr, 64) == 1);
    assert(ThreadCount(nullptr, 128) == 2);
    assert(ThreadCount(nullptr, 100000) == 16);  // default cap
    assert(ThreadCount("", 100000) == 16);
    assert(ThreadCount("0", 100000) == 0);       // off
    assert(ThreadCount("4", 100000) == 4);       // number = cap
    assert(ThreadCount("4", 128) == 2);          // small cache: fewer
    assert(ThreadCount("1", 100000) == 1);
    assert(ThreadCount("1000", 1000000) == HardMaxThreads);
    assert(ThreadCount("abc", 100000) == 16);    // junk: default
    assert(ThreadCount("-3", 100000) == 16);
    std::puts("cache read-ahead rules ok");
    return 0;
}
