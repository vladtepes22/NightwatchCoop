// SPDX-License-Identifier: GPL-3.0-or-later
// D4b: one shared enemy drop roll for the whole party (playtest 1 #4, user decision 2026-10-14:
// "roll should be the same for all always"). Every player still gets its own copy of the drop
// (each game spawns it for its own player), but every game draws the SAME random numbers for a
// given enemy kill, so the items are the same for everyone.
//
// How: each game rolls an enemy's drop in 0x17DD0B0 (MapItemMan, chr handle, NpcParam row) with
// the game RNG, an MT19937 at GameStateMan +0xA78 (+4 draws left, +8 next word, +0x10 the 624
// state words; 0x181E5C0 refills it). The hook saves that RNG, seeds it from
//   DropRollSeed(host salt, host world generation, map, enemy entity ids, item lot)
// runs the game's own roll, and puts the saved RNG back, so nothing else in the game sees a change.
// The host's salt (random per host run) and generation (+1 on each of the host's loads: a rest,
// a death, a travel - enemies respawn exactly then) reach the guests in EVENT "drop_seed".
//
// Item discovery is per player (user decision 0.6.2): the dice are shared, but each game applies
// its OWN player's item discovery (request +4, 0x1581830: 1.0 below Arcane 8, 1.7 at 30, 2.1 at
// 50, plus the SpEffects of type 0x42 such as the Eye rune). How the game uses it (0x17CEAF0, one
// draw per ItemLotParam row of the lot's chain): every slot with a weight gets
//   weight = trunc(basePoint * 100 (cumulate-blended) * (enableLuck ? discovery : 1.0)), min 1
// and ONE draw r = total * (MT output >> 9) / 2^23 walks the slots in order. So discovery only
// grows the luck slots (usually the rare item) against the others (usually "nothing" or a
// common item), renormalised per row. Consequences for the same draw (LotRowPick, unit-tested;
// tools/party/drop_discovery.py over every enemy lot row of 1.09):
//  - a row with ONE luck slot: that slot's interval only widens with more discovery (its start
//    moves down, its end up), so a higher-discovery player gets that item whenever a lower one
//    does, plus sometimes when the lower one does not. Holds strictly.
//  - an item never turns into "nothing" for the higher-discovery player in the 1.09 enemy lots.
//  - a row with TWO or more luck slots (58 of the 628 enemy lot rows): the luck items' total
//    chance still grows, but the boundaries between them move, so the same draw can give the
//    higher-discovery player ANOTHER luck item of the row than the lower one got.
//  - a non-luck item (a Blood Vial in a "Vial or Quicksilver Bullet" row) can become the luck item
//    for the higher-discovery player: discovery trades common items for rare ones, as in vanilla.
// BB_PARTY_DROP_DISCOVERY=host: the 0.6.1 behaviour (every game uses the host's discovery, so
// everyone gets exactly the same items).
//
// Env: BB_PARTY_SHARED_DROP_ROLL=0 turns it off (the old behaviour: every player's own roll).
//
// The pure part is unit-tested (tests/test_party_droproll.cpp, BB_PARTY_DROPROLL_NO_GAME).
#pragma once

#include <cstdint>
#include <string>

namespace coop {
struct GameSnapshot;
}
namespace party {
class PartyLink;
}

namespace coop::droproll {

constexpr const char* kEventName = "drop_seed";

/// What a guest needs to roll like the host: the host run's salt, its world generation and its
/// item-discovery value (used only with BB_PARTY_DROP_DISCOVERY=host; 0 = unknown: own).
struct DropSeed {
    std::uint64_t salt = 0;
    std::uint32_t gen = 0;
    float discovery = 0.0f;
    bool operator==(const DropSeed& o) const { return salt == o.salt && gen == o.gen && discovery == o.discovery; }
};
std::string DropSeedToJson(const DropSeed& s);
/// Parses and validates (salt != 0, discovery 0 or 1..1000).
bool DropSeedFromJson(const std::string& text, DropSeed* out, std::string* error = nullptr);

/// The enemy's identity as both games see it: the map, its map entity id (chr +0x1F0) and, only
/// when it has none, its chr table slot (handle & 0x3FFFFF: set and index, MSB order).
struct DropKey {
    std::uint32_t map = 0;
    std::int32_t entity = 0;  ///< chr +0x1F0, the map entity id
    std::int32_t entity2 = 0; ///< chr +0x3B8, the census key: logged only, differs between games
    std::uint32_t handle = 0;
    std::int32_t lot = -1;    ///< NpcParam itemLotId
};
/// The MT19937 seed of one enemy's drop: equal inputs give an equal seed on every game; another
/// generation (the enemies respawned), another salt (another host run) or another enemy differ.
std::uint32_t DropRollSeed(std::uint64_t salt, std::uint32_t gen, const DropKey& k);

/// MT19937 init_genrand: the 624 state words for `seed` (Matsumoto / Nishimura).
void SeedMt19937(std::uint32_t (&state)[624], std::uint32_t seed);
/// The first tempered output of a generator seeded with `seed` (the reference value for tests:
/// seed 5489 gives 3499211612).
std::uint32_t Mt19937First(std::uint32_t seed);
/// Guest: a drop roll while its world is not up yet (loading in, map unknown) is a kill of the
/// host's from before this player arrived: no roll, no drop on this game (dropdisc / pard3).
bool DropRollBeforeArrival(bool world_up, bool loading, std::uint32_t map);

/// Is the shared roll on (BB_PARTY_SHARED_DROP_ROLL, default on)? `v` = the variable's value.
bool SharedRollFromEnv(const char* v);

/// BB_PARTY_DROP_DISCOVERY: "host" = every game rolls with the host's item discovery (0.6.1);
/// anything else (default) = each game with its own player's.
bool HostDiscoveryFromEnv(const char* v);

/// One slot of an ItemLotParam row as the roll (0x17CEAF0) sees it.
struct LotSlot {
    std::uint16_t base = 0;      ///< lotItemBasePoint (+0x40)
    std::uint16_t cumulate = 0;  ///< cumulateLotPoint (+0x50)
    bool luck = false;           ///< enableLuck (+0x92 bit i): weight x item discovery
    bool taken = false;          ///< its pickup flag is already set: not in the roll
};
/// The game's per-row pick: which slot (0..7) the draw `mt_output` (one tempered MT19937 word)
/// picks with item discovery `discovery`; `cumulate_frac` = the row's drop-counter fraction
/// (0 for rows without one). -1 = no slot has a weight (no draw is taken then).
int LotRowPick(const LotSlot (&slots)[8], float cumulate_frac, float discovery, std::uint32_t mt_output);
/// The slot weights the roll uses (0 = not in the roll); returns their total.
std::uint32_t LotRowWeights(const LotSlot (&slots)[8], float cumulate_frac, float discovery,
                            std::uint32_t (&weights)[8]);

#ifndef BB_PARTY_DROPROLL_NO_GAME
/// Once, from worldact::Init (party mode): the roll hook (replaces the old log-only hook).
void Init(bool log_rolls);
/// Main thread, once a frame (worldact::Tick): host: counts its loads and sends the seed;
/// guest: nothing (the seed arrives through OnEvent).
void Tick(const GameSnapshot& s, party::PartyLink* link, bool host);
/// Guest: EVENT drop_seed from the host (link thread).
bool OnEvent(const std::string& body, std::string* why);
/// The shared roll is installed and on.
bool Enabled();
/// worldact's test fixture hooks the drop insert (0x17D5320) itself and hands each record here.
void FixtureInsert(std::uint64_t rec);
#endif

} // namespace coop::droproll
