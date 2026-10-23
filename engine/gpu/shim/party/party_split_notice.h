// The notice when a party member's chr goes missing because it is in another map (split travel).
// Pure text + rate limit (tests/test_party_split_notice.cpp); party_b2.cpp SplitTick shows it as a
// join / leave notice (status::SetLastEvent, so the "Join / leave notices" switch governs it).
#pragma once
#include <cstdint>
#include <string>

namespace coop::splitnotice {

constexpr double kMinGapS = 10.0;  ///< per member, between two notices (a seam walker must not spam)

/// The area's usual name from a map id 0xAABBCCDD (AA = area: m27 is the Forbidden Woods); "" unknown.
inline const char* AreaNameOf(std::uint32_t map) {
    switch ((map >> 24) & 0xff) {
    case 21: return "the Hunter's Dream";
    case 22: return "Hemwick Charnel Lane";
    case 23: return "Old Yharnam";
    case 24: return "the Cathedral Ward";
    case 25: return "Cainhurst Castle";
    case 26: return "the Nightmare of Mensis";
    case 27: return "the Forbidden Woods";
    case 28: return "Yahar'gul";
    case 32: return "Byrgenwerth";
    case 33: return "the Nightmare Frontier";
    case 34: return "the Hunter's Nightmare";
    case 35: return "the Research Hall";
    case 36: return "the Fishing Hamlet";
    default: return "";
    }
}

inline std::string WentOn(const std::string& name, std::uint32_t map) {
    const std::string who = name.empty() ? "A party member" : name;
    const std::string area = AreaNameOf(map)[0] ? AreaNameOf(map) : "another area";
    return who + " went on to " + area + ": you'll see them again when you follow.";
}

inline std::string HereAgain(const std::string& name) {
    return (name.empty() ? std::string("A party member") : name) + " is here again.";
}

/// True (and `last_s` taken) when member notices may show again: never within kMinGapS of the last one.
inline bool MayNotify(double* last_s, double now_s) {
    if (*last_s > 0 && now_s - *last_s < kMinGapS) return false;
    *last_s = now_s;
    return true;
}

}  // namespace coop::splitnotice
