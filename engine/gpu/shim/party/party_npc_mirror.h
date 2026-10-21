// SPDX-License-Identifier: GPL-3.0-or-later
// NPC presence mirror (PR #6 finding 2, the prologue's clinic NPC): a summoned guest shows the
// NPCs the host's world has turned off as off too.
//
// Many NPC events of the maps run only in the own world (EMEVD 4[3] "chr type of 10000 is 0",
// else 1000[2] end): on a summoned guest they end at once and never run their 2004[5] disable,
// so the NPC keeps its MSB default (on). Vanilla never summons into most of these places (the
// clinic is a no-multiplayer area, -241000); the party does. Live (misc071 prorun1, fresh
// characters, BB_PARTY_START=immediate): m24_01 events 12410729 / 12410733 / 12410737 turn the
// clinic's NPC 2410700 off on the host; the guest summoned into the clinic saw it standing at
// the door (its game only; the host's shows nobody).
//
// Host (party_balance.cpp NpcMirrorTick): the map entity ids of the non-player, non-enemy chrs
// that are off in its loaded world, as EVENT "npc_off" {"map":M,"off":[...]} on change and every
// 10 s. Guest in the host's world, same map: each of those it shows is turned off
// (ChrIns_SetDisable 0x18c6390, as the event does); one it turned off that the host turns on
// again is turned on again. Nothing else is touched (an NPC the host shows and the guest does
// not stays as it is: enabling chrs on a client is the host's events' job).
// Only NPCs (0.7.2, lamprule): the mirror turns off nothing but the entity ids of the npcMirror table
// (the npcMirror table, scripts/partydata/ids_tool.py: the MSB character parts that are people).
// Before, it took every non-player, non-enemy chr the host had off, which included the lamps'
// "warp OBJ talk CHR"s (AAB0950+n): the host's game keeps an unlit lamp's talk chr off, so a guest
// at that lamp lost its talk (t241001 "Light lamp") and could not light it (live lamprule run 3:
// NPC 2410951 turned off 0.1 s after the guest reached Central Yharnam's lamp, no prompt since).
// The host's rule (System > Party rules: World, party_npc_mirror; BB_PARTY_NPC_MIRROR=0/1 at start). Pure parts here (unit test tests/test_party_npc_mirror.cpp).
#pragma once

#include "party_gamedata.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

namespace coop::npcmirror {

/// An NPC the mirror may turn off (the npcMirror table of the player's own game, party_gamedata.h). Never a
/// lamp / headstone talk chr, a Dream messenger or dummy, a bullet owner or another object-like chr. With no
/// tables loaded nothing is mirrored.
inline bool Mirrored(std::uint32_t entity) {
    const auto& v = gamedata::Get().npc_mirror;
    return std::binary_search(v.begin(), v.end(), entity);
}

constexpr const char* kEventName = "npc_off";
constexpr std::size_t kMaxIds = 128;

/// A chr of the guest's loaded world that the host's list may name.
struct LocalNpc {
    std::uint32_t entity = 0;
    bool on = false;    // enabled (drawn) here
    bool ours = false;  // turned off by this mirror
};

struct Plan {
    std::vector<std::uint32_t> disable;  // the host has it off, this game shows it
    std::vector<std::uint32_t> enable;   // turned off by us, the host no longer has it off
};

/// What the guest does with the host's list `host_off` (sorted or not) for its local chrs.
inline Plan PlanMirror(const std::vector<std::uint32_t>& host_off, const std::vector<LocalNpc>& local) {
    Plan p;
    for (const LocalNpc& n : local) {
        const bool off = std::find(host_off.begin(), host_off.end(), n.entity) != host_off.end();
        if (off && n.on && Mirrored(n.entity)) p.disable.push_back(n.entity);
        else if (!off && n.ours && !n.on) p.enable.push_back(n.entity);
    }
    return p;
}

inline std::string ToJson(std::uint32_t map, const std::vector<std::uint32_t>& off) {
    std::string j = "{\"map\":" + std::to_string(map) + ",\"off\":[";
    for (std::size_t i = 0; i < off.size() && i < kMaxIds; ++i) j += (i ? "," : "") + std::to_string(off[i]);
    return j + "]}";
}

/// Strict parse of ToJson's shape (a peer's input): map and at most kMaxIds ids, each a map
/// entity id (1000000..99999999). False on anything else.
inline bool FromJson(const std::string& j, std::uint32_t* map, std::vector<std::uint32_t>* off) {
    const std::string head = "{\"map\":";
    if (j.compare(0, head.size(), head) != 0) return false;
    char* end = nullptr;
    const unsigned long m = std::strtoul(j.c_str() + head.size(), &end, 10);
    const std::string mid = ",\"off\":[";
    if (!end || std::string(end).compare(0, mid.size(), mid) != 0) return false;
    const char* p = end + mid.size();
    std::vector<std::uint32_t> ids;
    while (*p && *p != ']') {
        if (*p < '0' || *p > '9') return false;
        const unsigned long v = std::strtoul(p, &end, 10);
        if (v < 1000000ul || v > 99999999ul || ids.size() >= kMaxIds) return false;
        ids.push_back(std::uint32_t(v));
        p = end;
        if (*p == ',') ++p;
        else if (*p != ']') return false;
    }
    if (std::string(p) != "]}") return false;
    *map = std::uint32_t(m);
    *off = std::move(ids);
    return true;
}

}  // namespace coop::npcmirror
