// SPDX-License-Identifier: GPL-3.0-or-later
// Talk NPCs stay solid for guests (playtest 1 bugs 11 / 17, part of 5).
//
// On a client, chr creation 0x18E4F94..0x18E4FDC turns an NPC whose NpcParam row has
// isChangeWanderGhost (byte 0x145 bit 7) into a wandering ghost: chr type 4, translucent, no
// collision and no talk. Vanilla does that so a phantom cannot talk to the host's NPCs; the party
// lets guests talk to them (D8 world talk: the quest flags go to the host as world actions), so
// every talk NPC's row loses the bit. seamless_rules.cpp writes the rows (BB_PARTY_GUEST_SERVICES=0
// leaves them vanilla); only clients read the bit, so the host and solo play see no change.
//
// Rows (Bloodborne 1.09 gameparam NpcParam, tools/party/flag_tool.py load_params): every row with
// the bit that a talk NPC uses. Enemies with the bit (109100, 111010, 111011, 252100, the other
// 402xxx / 409xxx rows, the chalice rows ...) keep it.
// P3 (quest_audit.md, 2026-10-14): the enemy-family rows a talk NPC uses were checked against every
// MSB part (all maps, chalice layouts included): 504020 (the suspicious beggar, Cathedral Ward
// m24_00, talk 240217), 504030 (the same beggar transformed, Forbidden Woods m27, talk 270217),
// 513000 (Willem, m32, talk 320310; the two m32 cutscene dummies with entity -1), 402080 / 402090
// (the Research Hall's talking patients, m35), 402280 / 402281 (the brain-fluid heads), 402290 /
// 402300 (Adeline, head and body), 405100 (the Fishing Hamlet priest, m36), 409000 (the corpse in
// Ludwig's arena, talk 340540) and 451100 (Ludwig's head): no part uses them except these talk NPCs,
// so clearing the bit changes nothing else. A hostile one (the beggar once he turns) is then a
// solid enemy on the guest's screen, as every other enemy is, instead of an untouchable ghost.
// 0.7.6: 111012 is Patches in the Nightmare Frontier (m33 entity 3300710, model 6200, the only part
// with that row in any map; his push event 13300200 is named for him): a guest saw a ghost it could not hit.
#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::npcghost {

struct GhostRow {
    std::uint32_t row;
    std::uint8_t byte;  ///< 1.09 value of byte 0x145 (bit 7 set; 113010 also has isUnduration)
    const char* who;
};

constexpr std::uint32_t kField = 0x145;
constexpr std::uint8_t kBit = 0x80;

inline constexpr GhostRow kTalkNpcRows[] = {
    {6040, 0x80, "talk NPC 6040"},
    {6060, 0x80, "Iosefka (clinic door)"},
    {6070, 0x80, "the fake Iosefka"},
    {6071, 0x80, "the fake Iosefka (2)"},
    {6080, 0x80, "the old man (Cathedral Ward)"},
    {6090, 0x80, "the lonely old woman"},
    {6100, 0x80, "talk NPC 6100"},
    {6110, 0x80, "Arianna"},
    {6111, 0x80, "Arianna (2)"},
    {6112, 0x80, "Arianna (3)"},
    {6113, 0x80, "Arianna (after the birth)"},
    {6120, 0x80, "talk NPC 6120"},
    {6130, 0x80, "talk NPC 6130"},
    {6140, 0x80, "talk NPC 6140"},
    {6141, 0x80, "talk NPC 6141"},
    {6142, 0x80, "talk NPC 6142"},
    {6143, 0x80, "talk NPC 6143"},
    {6144, 0x80, "talk NPC 6144"},
    {6150, 0x80, "Alfred"},
    {6151, 0x80, "Alfred (Oedon Chapel steps)"},
    {6154, 0x80, "Alfred (3)"},
    {6155, 0x80, "Alfred (4)"},
    {6160, 0x80, "Eileen"},
    {6161, 0x80, "Eileen (2)"},
    {6180, 0x80, "talk NPC 6180"},
    {6210, 0x80, "talk NPC 6210"},
    {6500, 0x80, "talk NPC 6500"},
    {6510, 0x80, "talk NPC 6510"},
    {6511, 0x80, "talk NPC 6511"},
    {6521, 0x80, "talk NPC 6521"},
    {6590, 0x80, "talk NPC 6590"},
    {6970, 0x80, "talk NPC 6970"},
    {6980, 0x80, "talk NPC 6980"},
    {6990, 0x80, "talk NPC 6990"},
    {8120, 0x80, "talk NPC 8120"},
    {8121, 0x80, "talk NPC 8121"},
    {8122, 0x80, "talk NPC 8122"},
    {8123, 0x80, "talk NPC 8123"},
    {8125, 0x80, "talk NPC 8125"},
    {111012, 0xc4, "Patches (Nightmare Frontier)"},
    {113010, 0xc0, "the Oedon Chapel Dweller"},
    {402080, 0x80, "a Research Hall patient (talks)"},
    {402090, 0x80, "a Research Hall patient (talks, 2)"},
    {402280, 0xc0, "a brain-fluid head (siblings)"},
    {402281, 0xc0, "a brain-fluid head (beam walkway)"},
    {402290, 0xc0, "Adeline (head)"},
    {402300, 0xc0, "Adeline"},
    {405100, 0x80, "the Fishing Hamlet priest"},
    {409000, 0xc0, "the corpse in Ludwig's arena"},
    {451100, 0xc0, "Ludwig's head"},
    {504020, 0xc8, "the suspicious beggar"},
    {504030, 0xc8, "the suspicious beggar (Forbidden Woods)"},
    {513000, 0x80, "Willem"},
    {803000, 0x80, "the Doll"},
    {804000, 0x80, "Gehrman"},
};
inline constexpr std::size_t kTalkNpcRowCount = sizeof(kTalkNpcRows) / sizeof(kTalkNpcRows[0]);

inline const GhostRow* FindTalkNpcRow(std::uint32_t row) {
    for (const GhostRow& r : kTalkNpcRows) {
        if (r.row == row) {
            return &r;
        }
    }
    return nullptr;
}

enum class Verdict { write, holds, refuse };

/// What to do with a row whose byte 0x145 reads `now`: the 1.09 value -> write it without bit 7
/// (`*out`), already without it -> holds, anything else -> refuse (another data version: leave it).
inline Verdict Decide(const GhostRow& r, std::uint8_t now, std::uint8_t* out) {
    const std::uint8_t want = std::uint8_t(r.byte & ~kBit);
    if (out) {
        *out = want;
    }
    if (now == want) {
        return Verdict::holds;
    }
    return now == r.byte ? Verdict::write : Verdict::refuse;
}

} // namespace coop::npcghost
