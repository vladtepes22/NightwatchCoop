// SPDX-License-Identifier: GPL-3.0-or-later
// Six players (0.8.0, CL-10e56b, six080): the engine's fixed player tables grown from 5 seats (host + 4 remote) to 6.
// docs/party/six_players.md has the census, the design and the stages. Everything here is OFF unless BB_PARTY_LARGE=2
// and BB_PARTY_MAX=6 at start (every peer must run the same patch set: the rules tag "6p:vN" and the hook list are part of
// the party version check); with it off nothing is patched. A patch set is applied whole or not at all: every site is
// compared with the 1.09 bytes first, one mismatch refuses the set (and the game stays at 5 seats).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace coop::sixp {

/// BB_PARTY_LARGE=2 (the launcher's "Six players" switch) and BB_PARTY_MAX=6: this game boots with the 6-seat patch set.
inline bool EnabledFromEnv() {
    const char* l = std::getenv("BB_PARTY_LARGE");
    const char* m = std::getenv("BB_PARTY_MAX");
    return l && l[0] == '2' && !l[1] && m && std::atoi(m) == 6;
}

/// One byte-checked patch: `from` is what the 1.09 image holds at `off`, `to` replaces it (same length).
struct Site {
    std::uint64_t off;
    std::vector<std::uint8_t> from, to;
    const char* what;
};

/// A site list is sane: same lengths, from != to, no empty site, no two sites overlapping.
inline bool SitesSane(const std::vector<Site>& v, std::string* why = nullptr) {
    for (std::size_t i = 0; i < v.size(); ++i) {
        const Site& s = v[i];
        if (s.from.empty() || s.from.size() != s.to.size() || s.from == s.to) {
            if (why) *why = std::string("bad site ") + s.what;
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            const Site& t = v[j];
            if (s.off < t.off + t.from.size() && t.off < s.off + s.from.size()) {
                if (why) *why = std::string("overlap ") + s.what + " / " + t.what;
                return false;
            }
        }
    }
    return true;
}

/// Stage 1 (docs/party/six_players.md): the player ChrSet, 5 -> 6 entries. Constructor 0x18da8d0 allocates 0x118 = 5 x 0x38,
/// writes the count 5 and clears the block with a memset of 0x118; every entry's initial state is all zero, so a bigger,
/// fully cleared block is a valid 6-entry set. (The free-slot scan 0x18db500 and the handle lookup read the count.)
inline std::vector<Site> Stage1Sites() {
    return {
        {0x18da8ec, {0xbe, 0x18, 0x01, 0x00, 0x00}, {0xbe, 0x50, 0x01, 0x00, 0x00}, "ChrSet alloc size 0x118 -> 0x150"},
        {0x18da906, {0x41, 0xc7, 0x06, 0x05, 0x00, 0x00, 0x00}, {0x41, 0xc7, 0x06, 0x06, 0x00, 0x00, 0x00}, "ChrSet count 5 -> 6"},
        {0x18da911, {0xba, 0x18, 0x01, 0x00, 0x00}, {0xba, 0x50, 0x01, 0x00, 0x00}, "ChrSet memset size 0x118 -> 0x150"},
    };
}

/// Stage 2: the GameDataMan remote rows, 4 -> 5 (host + 5 remote players = 6 seats). The init 0x14e4e80 allocates 0x10 + 4 x 0x6a0 and
/// constructs four PlayerGameData rows; the allocation size and count are patched here and the 5th row and the 5-byte occupancy
/// array are made by a post-init hook (SixpInit). Every loop / index bound over the rows (cmp reg, 4 / 3) is a site.
inline std::vector<Site> Stage2Sites() {
    return {
#include "party_sixp_sites2.inc"
    };
}

/// Stage 3: the presentation object (`*(flow 0x5556678 + 0x16f8)`, allocated 0xb0 at 0x156783e, ctor 0x15ba630). 5 slots of 0x14 bytes
/// at +0x1c, then four tracked chr handles at +0x80 with their count at +0x90. A 6th slot at +0x80 swallows exactly that area
/// (0x80..0x93), so the tracked handles and count move to +0xb0..+0xc3 (object 0xc8) and nothing else in the tail moves. Sites: every
/// slot loop bound (5 -> 6 slots), every access to the tracked area (+0x30), the allocation size. Three hooks (SixpInit) initialise
/// the new fields and keep the slots packed.
inline std::vector<Site> Stage3Sites() {
    return {
#include "party_sixp_sites3.inc"
    };
}

/// The stages proven so far (each one by fp5x / fpi5x with 5 players, then fp6 / fpi6 for the last).
constexpr int kDefaultStage = 3;

/// The highest stage in force: BB_PARTY_SIX_STAGE (1, 2, 3; a test aid for proving the stages one by one), default kDefaultStage.
inline int StageFromEnv() {
    const char* e = std::getenv("BB_PARTY_SIX_STAGE");
    const int n = e ? std::atoi(e) : 0;
    return n >= 1 && n <= 9 ? n : kDefaultStage;
}

/// The patch sets in force, in order (stage N includes the stages before it).
inline std::vector<Site> AllSites(int stage = 9) {
    std::vector<Site> v = Stage1Sites();
    if (stage >= 2) {
        for (Site& s : Stage2Sites()) v.push_back(std::move(s));
    }
    if (stage >= 3) {
        for (Site& s : Stage3Sites()) v.push_back(std::move(s));
    }
    return v;
}

constexpr const char* kRulesTag = "6p:v1";

/// Installs the set (whole or not at all) when EnabledFromEnv(); logs "Party 6p: ...". Called by PartyInit after FourpInit.
void SixpInit();
/// BB_PARTY_SIX_PROBE=1: a read-only look at the engine tables once the objects exist (offsets check).
void SixpTick();
/// True once the whole set is in.
bool SixpActive();

} // namespace coop::sixp
