// Texture quality rules (gpu/shim/engine/texture_quality_rules.h; visy c55cd33d).
#include <cassert>
#include <cstdio>
#include <cstring>

#include "engine/texture_quality_rules.h"

using namespace TextureQuality;

int main() {
    constexpr unsigned long long GB = 1ull << 30;
    // Auto: half on a card with 3 GB or less, full above; an unknown size (0) stays full.
    assert(WantedSkip(Auto, 2 * GB) == 1);
    assert(WantedSkip(Auto, 3 * GB) == 1);
    assert(WantedSkip(Auto, 4 * GB) == 0);
    assert(WantedSkip(Auto, 0) == 0);
    assert(WantedSkip(Full, 1 * GB) == 0);
    assert(WantedSkip(Half, 16 * GB) == 1);
    assert(WantedSkip(Quarter, 16 * GB) == 2);

    // Names round-trip; anything else reads as auto.
    for (int q = 0; q < Count; ++q) {
        int back = -1;
        for (int r = 0; r < Count; ++r) {
            if (std::strcmp(Name(q), Name(r)) == 0) back = r;
        }
        assert(back == q);
    }
    assert(std::strcmp(Name(99), "auto") == 0);

    // Per texture: only eligible ones, never below 3 levels, the level dropped at least 512.
    assert(SkipLevels(false, 2048, 2048, 12, 2) == 0);
    assert(SkipLevels(true, 2048, 2048, 12, 0) == 0);
    assert(SkipLevels(true, 2048, 2048, 12, 1) == 1);
    assert(SkipLevels(true, 2048, 2048, 12, 2) == 2);
    assert(SkipLevels(true, 2048, 1024, 11, 2) == 2);  // 1024 and 512 sides dropped
    assert(SkipLevels(true, 1024, 512, 10, 2) == 1);   // the next top would be 256 wide
    assert(SkipLevels(true, 512, 512, 10, 2) == 1);
    assert(SkipLevels(true, 256, 256, 9, 2) == 0);
    assert(SkipLevels(true, 2048, 2048, 2, 2) == 0);   // too few levels
    assert(SkipLevels(true, 2048, 2048, 3, 2) == 1);   // keeps 2 after one: visy's rule stops there
    assert(SkipLevels(true, 2048, 2048, 4, 2) == 2);
    std::puts("texture quality rules: ok");
    return 0;
}
