// SPDX-License-Identifier: GPL-3.0-or-later
// Boss fights after a party load (0.7.2 known issue 2, docs/party/world_actions.md): which boss state
// a guest's game takes from the host after a load, and when a guest's boss fog counts as walked
// through. Pure logic, no game access (unit test: tests/test_party_bossfog_rules.cpp); the game side
// is party_b2.cpp (the overlay guard: SaveOverlay / RestoreZeroedOverlay) and party_worldact.cpp
// (the guest's fog report, GuestTick). Also: how each boss's first fight really starts (FirstFights:
// a guest's fog starts only a walk-in one; an action one keeps its room open, party_phantom
// BossRoomRules) and every co-op wall of the game (ConfinementWalls), both from an EMEVD scan
// (tools/party/bossroom_tool.py).
//
// The boss fog events (every map EMEVD, flag_tool emevd): a boss's state lives in its map's T1 zone 4
// group, 1AAB4xxx - the host-entered flag 1AAB4800 that the boss's start event (`ボスが動き出す`)
// waits on, the guest-entered flag 1AAB4801 its guest-entry event (`ゲストがボス部屋入場_*`) sets,
// the fight's start event 1AAB4802 (boss health bar) - next to the zone 5 / 6 groups (enemy AI,
// boss parts). None of them is saved across a load: the host's own load (a death, a wipe's respawn
// load, a lamp travel) puts them back to zero, the boss stands at its start and its fog is closed,
// as after a vanilla death. A guest's game takes them from the host's join snapshot only (the world
// mirror leaves them out: progress WorldMirrorKey), so after a load they must be the snapshot's.
//
// What went wrong (0.7.1 smoke wipe_late, wiperace run 1): the overlay guard (run 18: a re-sent
// snapshot left the whole overlay zero, the guest replayed the opening) restored EVERY group that
// came back all zero from the copy saved before the load - also the boss's zone 4 group, which the
// host's wipe load had rightly reset to zero. The guest's game then read the fight as running
// (1AAB4800 = 1: its boss start event showed the health bar at the lamp) and its own old
// guest-entered flag (1AAB4801 = 1) as a fresh walk-through, which it reported 3 s after the load:
// the host opened the fight and the boss attacked the party at the lamp.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "party_gamedata.h"

namespace coop::bossfog {

/// A map's fight-state group: flag block (id / 1000) 1AABZ of type 1, a map (not the global area 0
/// block 0), zone 4 (boss fight, fog, NPC summon), 5 (enemy AI) or 6 (boss parts, traps). Reset by
/// every load of the map; never in the world mirror.
inline bool FightStateBlock(std::uint32_t block) {
    if (block < 10000u || block >= 100000u) {
        return false;
    }
    const std::uint32_t type = block / 10000u, area = block / 100u % 100u, mblock = block / 10u % 10u,
                        zone = block % 10u;
    return type == 1u && !(area == 0u && mblock == 0u) && zone >= 4u && zone <= 6u;
}
inline bool FightStateFlag(std::uint32_t id) {
    return id >= 10000000u && FightStateBlock(id / 1000u);
}

/// The bosses' HP-driven phase events (EMEVD names `..._ヒートアップ..`, every map; Ludwig 13404820-25,
/// whose latter half plays the Holy Blade scene and warps into the arena). The host's own copy decides
/// from the host's boss chr; a guest's copy runs on the guest's chr, whose HP can disagree after the
/// host's wipe load (the guest still holds the old fight until its own load) and then tells the host
/// "done" in a network event state (packet 0x31): the host played the phase-2 scene at the lamp and
/// stood alone in the fight (player report 2026-10-25, smoke lw with BB_LW_INJECT). The host takes no
/// peer's state for these events.
inline bool HostOnlyBossEvent(std::uint32_t id) {
    // Derived from the player's game when the party tables are built (gamedata section hostOnlyEvents: every event
    // named "..._ヒートアップ.." in the EMELD files, Ludwig's included); none without the tables.
    const std::vector<std::uint32_t>& v = gamedata::Get().host_only_events;
    return std::binary_search(v.begin(), v.end(), id);
}

/// Mods that add or copy a boss's phase events give the copies ids of their own, which the list
/// above cannot know: a guest's state for such a copy would be applied on the host. A host's mods
/// name them in out/party_host_only.txt (scripts/mods.py: the enabled mods' own party_host_only.txt
/// files, merged), read once by the host (party_guestsvc.cpp ModHostOnlyEvents). Format: one decimal
/// event id (1 .. 4294967295) per line; blank lines and `#` comments (whole line or after the id)
/// are skipped. A file with any other line, or more than kMaxModHostOnlyEvents ids, counts as no
/// file at all (false, no ids): a broken file changes nothing. `ids`: sorted, no duplicates.
constexpr std::size_t kMaxModHostOnlyEvents = 4096;
inline bool ParseModHostOnlyEvents(std::string_view text, std::vector<std::uint32_t>* ids) {
    ids->clear();
    if (text.substr(0, 3) == "\xEF\xBB\xBF") {
        text.remove_prefix(3);  // a UTF-8 byte order mark (Windows editors)
    }
    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text.remove_prefix(eol == std::string_view::npos ? text.size() : eol + 1);
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        std::uint64_t v = 0;
        for (char c : line) {
            if (c < '0' || c > '9' || v > 0xffffffffu) {
                ids->clear();
                return false;
            }
            v = v * 10u + std::uint64_t(c - '0');
        }
        if (v == 0 || v > 0xffffffffu || ids->size() >= kMaxModHostOnlyEvents) {
            ids->clear();
            return false;
        }
        ids->push_back(std::uint32_t(v));
    }
    std::sort(ids->begin(), ids->end());
    ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
    return true;
}
/// The fixed list above or one of the ids a host's mods named (`mod_ids`: ParseModHostOnlyEvents).
inline bool HostOnlyBossEvent(std::uint32_t id, const std::vector<std::uint32_t>& mod_ids) {
    return HostOnlyBossEvent(id) || std::binary_search(mod_ids.begin(), mod_ids.end(), id);
}

/// Guest, after the host's snapshot was applied for a load into the host's world: the overlay group
/// of `block` came back all zero (`now_zero`); the copy saved before the load had bits in it
/// (`saved_nonzero`). Restore it from that copy? Never a fight-state group: zero is the host's reset
/// state after its load (a restore brought a wiped fight back to the lamp). Every other group: yes
/// (run 18's zeroed overlay: the opening and the Dream flags).
inline bool GuardRestores(std::uint32_t block, bool now_zero, bool saved_nonzero) {
    return now_zero && saved_nonzero && !FightStateBlock(block);
}

/// Guest, its stay in the host's world since the last load: is the guest-entered flag of a boss
/// fog a stale one, not a walk-through? A guest opens a fight only by going through the fog itself
/// (the fog's action, which sets the flag in the guest's game): a 0 -> 1 this game saw during the
/// stay. A flag that already read 1 from the stay's start (`seen_out` false) came with the load,
/// never from this guest's walk: it is not reported to the host (and not taken for "in the boss
/// room" either). `handled`: the flag was reported / dealt with this stay already.
inline bool StaleFogFlag(bool seen_out, bool in, bool handled) {
    return in && !handled && !seen_out;
}

/// The guest's report of a walk-through: a flag seen 0 this stay that reads 1 now, not yet reported.
inline bool FogWalkedThrough(bool seen_out, bool in, bool handled) {
    return in && !handled && seen_out;
}

// ---- How a boss's first fight really starts (Laurence, player report 2026-10-22) -----------------
// The first-fight event behind each fog gate's intro condition (BossGate::intro_flag), every map
// EMEVD (tools/party/bossroom_tool.py scan). `entry`: it waits only on the host's position (a 3[2]
// region / 3[3] distance next to flag and chr-type checks) - the host walking in starts it, so a
// guest's walk through the fog may stand in for it (the host's chr visits the spot, P5 / D10).
// `action`: it waits on something the host does inside the room - Laurence (13404861 / 13401851:
// goods 4014 Laurence's Skull held, 3[4], in the altar region 3402855), Ebrietas (12421802: the
// host's own hit, 3[23] attacker 10000), Rom (m32 13201802: anyone's first hit on her, 3[23] attacker
// -1; her fog 3201800 and its guest-entry event 13204831 were missing from BossGates, so after the
// host's first hit - the fog wall up - no guest ever got the prompt: player report 2026-10-25),
// Lady Maria (13501801 waits on 13500947, set when the host
// examines her, 13500948). Until then the room is open: the host's re-fight event disables the fog
// until the first fight has run (1003[101] intro -> label 0, else 2005[3](fog, 0), a wait on the
// intro flag, 2005[3](fog, 1)), but its `1003[5] skip 2 if client` keeps the fog up in a guest's
// game. What went wrong: Laurence counted as a guest-startable first fight, so a guest's fog had
// its prompt; going through it was taken for the fight's start (the host's chr visited the altar
// without the skull: nothing started) and left the guest shut in behind the fog until a Hunter's
// Mark. The Nightmare Grand Cathedral's lamp 3402953 also stands inside that room: a guest that
// arrived with the host there stood behind a closed fog too.
// Now an action first fight is never started from the fog, and a guest's game skips that client
// skip (`client_skip_index`): the room is open to it as to the host until the fight really starts
// (the intro flag on), then the fog comes up in its game as in the host's.
enum class FirstStart : std::uint8_t {
    none,    ///< no first-fight event behind the gate: the fog starts it
    entry,   ///< the host walking in (position only): a guest's walk through the fog may start it
    action,  ///< something the host does inside the room: never from the fog
};
struct FirstFight {
    std::uint32_t guest_event;            ///< BossGate::guest_event
    FirstStart start;
    std::int32_t refight_event = 0;       ///< action: the host's re-fight entry event (the fog's on / off)
    std::int32_t client_skip_index = -1;  ///< action: its `1003[5] skip 2 if client` instruction
    const char* notice = nullptr;         ///< action: the guest's notice at the fog before the fight
};
inline const FirstFight* FirstFights(std::size_t* n) {
    static const FirstFight rows[] = {
        {12204891, FirstStart::entry},  // Witch of Hemwick 12201802, region 2202805
        {12304811, FirstStart::entry},  // Blood-starved Beast 12301802, region 2302805
        {12304731, FirstStart::entry},  // Darkbeast Paarl 12301702, 16 m of 2300810
        {12404841, FirstStart::entry},  // Vicar Amelia 12401802, region 2402805
        {12414731, FirstStart::entry},  // Cleric Beast 12411702, region 2412805
        {12414811, FirstStart::entry},  // Father Gascoigne 12411802, region 2412815
        {12424811, FirstStart::action, 12424810, 2, "Ebrietas: the fight starts with the host's first hit on her"},
        {12424711, FirstStart::entry},  // the Celestial Emissary 12421702, region 2422815 (enables the small emissaries)
        {12504811, FirstStart::entry},  // Martyr Logarius 12501802, region 2502805
        {12604846, FirstStart::entry},  // Mergo's Wet Nurse 12601802, region 2602805
        {12604861, FirstStart::entry},  // Micolash 12601852, 32 m of 2600850
        {12704841, FirstStart::entry},  // Shadows of Yharnam 12701802, region 2702805
        {12804881, FirstStart::entry},  // the One Reborn 12801802, region 2802805
        {13204831, FirstStart::action, 13204830, 2, "Rom: the fight starts with the first hit on her"},
        {13304871, FirstStart::entry},  // Amygdala 13301802, region 3302805
        {13404801, FirstStart::entry},  // Ludwig 13404811 + 13401801, region 3402805
        {13404851, FirstStart::action, 13404850, 2,
         "Laurence: the fight starts when the host brings Laurence's Skull to the altar"},
        {13504801, FirstStart::action, 13504800, 2, "Lady Maria: the fight starts when the host goes up to her"},
        {13504851, FirstStart::entry},  // the Living Failures 13501851, region 3502815
        {13604801, FirstStart::entry},  // the Orphan of Kos 13604811 + 13601801, region 3602805
    };
    *n = sizeof rows / sizeof rows[0];
    return rows;
}
inline const FirstFight* FirstFightFor(std::uint32_t guest_event) {
    std::size_t n = 0;
    const FirstFight* rows = FirstFights(&n);
    for (std::size_t i = 0; i < n; ++i) {
        if (rows[i].guest_event == guest_event) {
            return &rows[i];
        }
    }
    return nullptr;
}
/// A guest's walk through this gate's fog may start the boss's FIRST fight (P5): an `entry` one only.
inline bool GuestMayStartFirstFight(std::uint32_t guest_event) {
    const FirstFight* f = FirstFightFor(guest_event);
    return f && f->start == FirstStart::entry;
}
/// Guest (a client): skip this instruction, so the room of an action first fight stays open in its
/// game until the fight starts, as in the host's.
inline bool GuestRoomOpenSkip(std::int32_t event, std::int32_t bank, std::int32_t id, std::int32_t index) {
    std::size_t n = 0;
    const FirstFight* rows = FirstFights(&n);
    for (std::size_t i = 0; i < n; ++i) {
        if (rows[i].start == FirstStart::action && rows[i].refight_event == event && bank == 1003 && id == 5 &&
            rows[i].client_skip_index == index) {
            return true;
        }
    }
    return false;
}

// ---- Multiplayer confinement walls (Ludwig's back door, player report 2026-10-22) ----------------
// Every event that puts up a wall object (2005[3](obj, 1), its SFX 2006[2]) right after a wait on
// the multiplayer state (3[6]), every EMEVD file (tools/party/bossroom_tool.py scan; the summon
// sign events that only make an SFX are no walls): common 7600 (the co-op area walls), its Chalice
// copy m29 12907600, and m34 13400010 `聖堂街D_マルチ閉じ込め壁` - wall 3401801 (MSB `霧壁_ボス0_1`)
// at the door at the back of Ludwig's arena, the way on into the DLC. 13400010 puts it up while in
// multiplayer and Ludwig lives (9471 off) and takes it down only when the session is over AND 9471
// is on (its group 1: 0[0] "not in multiplayer" [11] + 3[0] 9471 on [12], then a restart whose
// first wait needs 9471 off). Vanilla ends the co-op at a boss kill; a party stays, so the wall
// stood after Ludwig's death on the host and the guest until the guest left or the game was
// restarted (the player's log: 13400010 ended only when the guest left). Open world on (bbport.ini
// party_open_world, the default): no wall goes up (the wall and SFX instructions are skipped, as
// for 7600). Always in party mode: the "not in multiplayer" condition `alone_index` is skipped, so
// the wall goes at its progress flag with the open world off too.
struct ConfinementWall {
    std::int32_t event;
    std::int32_t on_index;     ///< 2005[3](wall, 1)
    std::int32_t sfx_index;    ///< 2006[2](wall SFX)
    std::int32_t alone_index;  ///< 0[0] "not in multiplayer" next to `until_flag` (-1: none)
    std::uint32_t until_flag;  ///< the wall is down for good once this is on (0: none)
};
inline const ConfinementWall* ConfinementWalls(std::size_t* n) {
    static const ConfinementWall rows[] = {
        {7600, 6, 7, -1, 0},          // common マルチ閉じ込め壁_XX (args: wall, SFX)
        {12907600, 6, 7, -1, 0},      // m29: the Chalice Dungeons' copy
        {13400010, 7, 8, 11, 9471},   // m34: wall 3401801 / SFX 3403801 behind Ludwig, until he is dead
    };
    *n = sizeof rows / sizeof rows[0];
    return rows;
}
/// Skip this instruction? `open_world`: the party's open world (no co-op walls) is on.
inline bool ConfinementWallSkip(std::int32_t event, std::int32_t bank, std::int32_t id, std::int32_t index,
                                bool open_world) {
    std::size_t n = 0;
    const ConfinementWall* rows = ConfinementWalls(&n);
    for (std::size_t i = 0; i < n; ++i) {
        const ConfinementWall& w = rows[i];
        if (w.event != event) {
            continue;
        }
        if (open_world && ((bank == 2005 && id == 3 && index == w.on_index) ||
                           (bank == 2006 && id == 2 && index == w.sfx_index))) {
            return true;
        }
        if (w.alone_index >= 0 && bank == 0 && id == 0 && index == w.alone_index) {
            return true;
        }
    }
    return false;
}

} // namespace coop::bossfog
