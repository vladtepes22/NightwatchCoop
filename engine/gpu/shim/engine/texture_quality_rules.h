// Texture quality (texture_quality in bbport.ini): big block-compressed materials are made
// without their largest mip levels to save video memory. Ported from visy c55cd33d (after
// Mrsuss60/bloodborne_pc_windows_port 8841db5b and deadinside28/bloodborne_pc). Pure rules
// for the native test; the texture cache applies them (texture_cache.cpp, FindImage).
#pragma once

#include <algorithm>
#include <cstdint>

namespace TextureQuality {

enum Quality : int { Auto = 0, Full = 1, Half = 2, Quarter = 3, Count = 4 };

inline const char* Name(int quality) {
    switch (quality) {
    case Full: return "full";
    case Half: return "half";
    case Quarter: return "quarter";
    default: return "auto";
    }
}

/// bbport.ini / BB_TEXTURE_QUALITY text: a name in any case ("Half"), or -1 for anything else
/// (the setting then keeps its value: auto by default).
inline int Parse(const char* text) {
    for (int q = 0; q < Count; ++q) {
        const char* name = Name(q);
        int i = 0;
        while (name[i] != 0 && text[i] != 0 &&
               (text[i] == name[i] || (text[i] >= 'A' && text[i] <= 'Z' && text[i] + 32 == name[i]))) {
            ++i;
        }
        if (name[i] == 0 && text[i] == 0) {
            return q;
        }
    }
    return -1;
}

/// Levels of a reduced image (made without its top `skip` levels, `kept` left) that a range of
/// the whole texture's levels (`first`, `count`) covers: shifted down by `skip`, inside the
/// image. Views, and every layout change of a reduced image, use this range: the whole
/// texture's range ran past the image's levels (Image::GetBarriers' subresource assertion, a
/// GPU stop with exit code 23 at the first bind of a reduced texture; 0.2.7).
struct LevelRange {
    uint32_t first;
    uint32_t count;
};
inline LevelRange KeptLevels(uint32_t first, uint32_t count, uint32_t skip, uint32_t kept) {
    if (skip == 0 || kept == 0) {
        return {first, count};
    }
    const uint32_t last = first + std::max(count, 1u) - 1;
    const uint32_t new_first = std::min(first > skip ? first - skip : 0u, kept - 1);
    const uint32_t new_last = std::min(last > skip ? last - skip : 0u, kept - 1);
    return {new_first, std::max(new_last, new_first) - new_first + 1};
}

/// A view's minimum LOD (1/256 level units) on a reduced image.
inline uint32_t KeptMinLod(uint32_t min_lod, uint32_t skip) {
    return min_lod > skip * 256 ? min_lod - skip * 256 : 0;
}

/// Levels the setting asks for: 0 full, 1 half, 2 quarter. Auto is half on a card whose own
/// device-local memory is 3 GB or less, else full.
inline uint32_t WantedSkip(int quality, uint64_t device_memory) {
    if (quality == Auto) {
        quality = device_memory != 0 && device_memory <= (3ull << 30) ? Half : Full;
    }
    return quality == Quarter ? 2 : quality == Half ? 1 : 0;
}

/// Levels one texture may lose. `eligible` = a sampled, plain 2D block-compressed colour
/// texture with one layer and one sample that the GPU has not written. A level goes only while
/// the level being dropped has a smaller side of at least 512 and 3 levels are left before it
/// (visy's rule: a 512 texture becomes 256 at half).
inline uint32_t SkipLevels(bool eligible, uint32_t width, uint32_t height, uint32_t levels,
                           uint32_t wanted) {
    if (!eligible || wanted == 0 || levels < 3) {
        return 0;
    }
    uint32_t skip = 0;
    while (skip < wanted && levels - skip >= 3 && (std::min(width, height) >> skip) >= 512) {
        ++skip;
    }
    return skip;
}

} // namespace TextureQuality
