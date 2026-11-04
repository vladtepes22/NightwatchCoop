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

    // bbport.ini text: any case; anything else is -1 (the setting keeps its value).
    assert(Parse("half") == Half && Parse("Half") == Half && Parse("QUARTER") == Quarter);
    assert(Parse("full") == Full && Parse("Auto") == Auto);
    assert(Parse("") == -1 && Parse("halfx") == -1 && Parse("hal") == -1 && Parse("2") == -1);

    // The 0.2.7 crash (Full/Half/Quarter by hand, NW-5203608 and the RX 550 report): a bind
    // changed a reduced image's layout over the whole texture's levels. Image::GetBarriers
    // keeps one state per level of the image, so a level past the kept ones failed its
    // `subres_idx < subresource_states.size()` assertion (exit 23). Every range of the whole
    // texture must land inside the kept levels.
    const auto inside = [](LevelRange r, uint32_t kept) {
        return r.count >= 1 && r.first + r.count <= kept;
    };
    for (uint32_t levels = 3; levels <= 14; ++levels) {
        for (uint32_t skip = 1; skip <= 2 && levels - skip >= 2; ++skip) {
            const uint32_t kept = levels - skip;
            // What a sampled bind passes: the T#'s levels, all of them in the usual case.
            assert(!inside({0, levels}, kept)); // the range used before the fix
            for (uint32_t first = 0; first < levels; ++first) {
                for (uint32_t count = 1; first + count <= levels; ++count) {
                    assert(inside(KeptLevels(first, count, skip, kept), kept));
                }
            }
            assert(KeptLevels(0, levels, skip, kept).first == 0);
            assert(KeptLevels(0, levels, skip, kept).count == kept);
        }
    }
    // A view from a level that was dropped starts at the first kept one; later ones shift down.
    assert(KeptLevels(1, 11, 1, 11).first == 0 && KeptLevels(1, 11, 1, 11).count == 11);
    assert(KeptLevels(3, 1, 2, 10).first == 1 && KeptLevels(3, 1, 2, 10).count == 1);
    assert(KeptLevels(0, 1, 2, 10).first == 0 && KeptLevels(0, 1, 2, 10).count == 1);
    // A whole image (skip 0) keeps its range.
    assert(KeptLevels(2, 5, 0, 12).first == 2 && KeptLevels(2, 5, 0, 12).count == 5);
    assert(KeptMinLod(600, 1) == 344 && KeptMinLod(200, 1) == 0 && KeptMinLod(600, 0) == 600);
    std::puts("texture quality rules: ok");
    return 0;
}
