// SPDX-License-Identifier: GPL-3.0-or-later
// Four-player parties (host + 3 cooperators): the byte-verified rules of docs/party/four_players.md.
//
// FourpInit (PartyInit, bbgpu_patch_image, after HooksInit; before the game runs) installs, when
// BB_PARTY is set and BB_PARTY_FOURP is not "0":
//   H1  0x186fe40 SessionSlotSummary: ReplacePrologue; after the game's own code, a used 2nd
//       cooperator slot (out+4 == 2) is reported free while fewer than 3 cooperators are counted
//       (0x18796c0 over kinds 7, 1, 0x15, 0x16, 0x17) - only while the party's max players >= 4.
//       The selector's own "max cooperators" (sel+0x1fc = 3, vanilla) then stops at 3.
//   H2  0x17bf187 (EMEVD 1003[109] boss count): HookCallSite, min(0x15bdc20(slots), 2), so 3
//       cooperators take the 2-cooperator branch (SpEffect 7501) instead of "no scaling".
//   H3  0x138bcc2 (native MultiDoping): jne -> jl, count >= 2 -> the 7501 branch.
//   H4  0x18c6db0 (chr ApplySpEffect, vfunc +0x3f0): asm stub, SpEffect 7501 -> 7502 (2.5x HP)
//       when the game counts >= 3 cooperators.
//   E6  EMEVD 3[29] "cooperators < 2" in the NPC summon sign events (xx04400..xx04406, 12906962)
//       -> "< 3" while the party's max players >= 4 (via the EMEVD dispatch filter,
//       seamless_rules.h SetEmevdRewriter), reverted when it drops below 4.
//   P5  0x1e97770 RecruitNum `mov ecx,2` -> 3 (optional, cosmetic: only told to a server) when
//       the local BB_PARTY_MAX >= 4.
// H2-H4 change nothing while at most 2 cooperators exist (vanilla cap), so they are installed for
// every party; only H1 / E6 follow the max players, which can change at run time.
//
// Max players: BB_PARTY_MAX (clamped 2..kMaxPlayers = 4, PartyLink). It and FourpRulesTag() form the party
// rules ("max 4 players, 4p:v1:..."), part of the PartyLink version check (LinkConfig::rules,
// HELLO): a guest with another max players or rule set is rejected with a reason naming both.
// The host's value also arrives in WELCOME (party::PartyLink::max_players) and FourpTick applies
// it on a connected guest, so the host's setting is what runs. WELCOME arrives at the title (the
// network starts there), before any world is loaded.
//
// Env: BB_PARTY_FOURP=0 nothing installed (tag "4p:off");
//      BB_PARTY_FOURP_SCALING=0 no H2-H4 (boss HP stays at the game's 0/1/2 table);
//      BB_PARTY_FOURP_NPC_SIGNS=0 no E6; BB_PARTY_FOURP_RECRUIT=0 no P5.
#pragma once

#include <cstdint>
#include <string>

namespace coop::fourp {

// ---- Pure rules (no game; tests/test_party_fourp.cpp) ----

/// Players in a party, the host included (= party::kMaxPartyPlayers): host + 4 (A7, 5 for tests).
constexpr int kMaxPlayers = 5;

struct Config {
    bool on = false;        // BB_PARTY set and BB_PARTY_FOURP != 0
    bool scaling = true;    // H2-H4
    bool npc_signs = true;  // E6
    bool recruit = true;    // P5
    int local_max = 3;      // BB_PARTY_MAX clamped 2..kMaxPlayers (default 3)
};
/// From the environment.
Config ConfigFromEnv();
/// The rule set every peer must share: "4p:off" or "4p:v1:H1,H2,H3,H4,E6" (P5 is server-only, not
/// in it). Goes into the party rules of the version check (with the max players).
std::string RulesTag(const Config& c);
/// The tag for this process (ConfigFromEnv).
std::string FourpRulesTag();
/// Max players in force: the host's from WELCOME on a connected guest, else the local setting.
int EffectiveMaxPlayers(int local_max, bool guest, bool connected, int host_max);
/// H5: the selector's "max cooperators" (sel+0x1fc) for a party of `max_players`: 3 (vanilla) below
/// 4, else the party size (the SOS filter counts the pending request too).
int SelectorMaxCoop(int max_players);
/// H1: report a used 2nd cooperator slot (status 2) free?
bool LiftCoopSlot(int rec0_status, int coop_count, int max_players);
/// H2: the cooperator count the boss events see.
int BossCount(int count);
/// H4: the SpEffect the game applies (7501 -> 7502 with >= 3 cooperators).
int DopingSpEffect(int sp_effect, int coop_count);
/// The boss doping rows (7500 / 7501 / 7502: 1 / 2 / 3 cooperators).
bool IsDopingSpEffect(int sp_effect);
/// H4 at ApplySpEffect: what to do with a doping apply on a chr that already holds `doping_held`
/// doping rows (any of 7500-7502). A boss is doped once per fight (its fight-start event); a
/// second copy (the same row from another game's event, sent over the link, or the event run
/// again) multiplied its HP again (live pard4 B2: Logarius rate 14.19 = 5.676 x 2.5, a guest's
/// max HP 22704 instead of 9081). Keep / upgrade 7501 -> 7502 / skip (dedupe off: never skip).
enum class DopingAction { Keep, Upgrade, Skip };
DopingAction DopingApply(int sp_effect, int coop_count, int doping_held, bool dedupe = true);
/// B2 / summon placement beside the host: spot 1..3 (0 = the host; 4 defined for a larger party)
/// as offsets along the host's right and forward axes: a line, right / left / 2x right, neighbours
/// `d` (BB_PARTY_SUMMON_OFFSET, 0.5 m) apart.
void SpotOffset(int spot, float d, float* right, float* forward);
/// The first free spot for party slot `slot` beside a host at (hx, hz) facing `yaw`: its own spot,
/// then right / left at 1, 2, 3 spacings `d`; free = at least `min_gap` (horizontal) from every
/// position in `others` (n of them, x/z pairs: the other members' reported positions). The own
/// spot when none is free. Live (2026-10-12, 4 players): the last joiner's own spot (2x right) was
/// 0.36 m from the first guest, who had moved: it spawned on top of it.
int PickFreeSpot(int slot, float d, float hx, float hz, float yaw, const float (*others)[2], int n, float min_gap);
/// The right-axis offset of PickFreeSpot's result (-1: the own slot's spot).
float FreeSpotRight(int pick, int slot, float d);
/// The spot party slot `slot` tries on attempt 0, 1, 2 (its own, the mirrored one, then 3 or 1).
int SpotForTry(int slot, int attempt);
/// E6: an NPC summon sign event (xx04400..xx04406 or 12906962).
bool IsNpcSignEvent(int event_id);
/// E6: the new count byte for the 3[29] argument bytes `a` (group, client type, comparison, count)
/// of an NPC sign event, or -1 to leave them. "coop < 2" -> "< 3" when max >= 4; back otherwise.
int NpcSignCount(const std::uint8_t a[4], int max_players);

// ---- Game side ----

/// Call from PartyInit after HooksInit (no-op unless Config::on).
void FourpInit();
/// Call once a frame from the main-thread coop tick.
void FourpTick();
/// Max players in force (2..kMaxPlayers).
int FourpEffectiveMax();

} // namespace coop::fourp
