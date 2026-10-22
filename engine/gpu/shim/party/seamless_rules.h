// SPDX-License-Identifier: GPL-3.0-or-later
// Seamless party rules (plan phase A6): what the party layer changes in the game besides the
// "Party: ..." byte patches of patches/Bloodborne.xml (which scripts/patches.py enables with
// BB_PARTY set and BB_PARTY_SEAMLESS != 0; the loader writes them before the game runs).
//
// SeamlessRulesInit (bbgpu_patch_image, after HooksInit, before the game runs):
//   - reports, per XML party patch site, whether the image holds the patched bytes ("applied"),
//     the 1.09 original ("off") or something else ("MISMATCH");
//   - installs the EMEVD instruction dispatch filter (0x17b93a0) when BB_PARTY_EMEVD_TRACE=1 or
//     a skip rule exists (BB_PARTY_EMEVD_SKIP or the built-in table, empty for now).
// SeamlessRulesTick (the main-thread coop tick, once a frame): param writes once the game's
// params are loaded, re-checked every 2 s (a param reload would restore the game's values):
//   - SpEffectParam 9006 (cooperator) and 9026 (invader phantom) maxHpRate 0.7 -> 1.0
//   - EquipParamGoods 200 (Beckoning Bell) consumeHeroPoint 1 -> 0: no Insight needed or spent
//   - opt-in (BB_PARTY_GUEST_MARK=1): EquipParamGoods 100 / 1400 (Hunter's Marks) byte 0x44 gets
//     enable_white | enable_multi, so guests can use them (party_phantom.h)
//
// Env: BB_PARTY_SEAMLESS=0 turns all of it off (and patches.py leaves the XML patches out);
//      BB_PARTY_FULL_HP=0, BB_PARTY_BELL_NO_INSIGHT=0 turn off a single param rule;
//      BB_PARTY_EMEVD_TRACE=1 logs each new (event, bank, instruction) the game executes;
//      BB_PARTY_EMEVD_SKIP="event:bank:id[@index][,...]" ("*" = any event; @index = only that
//      instruction index in the event) skips those instructions. Built in (party mode, B1 travel
//      on, BB_PARTY_OPEN_WORLD not 0): 7600:2005:3@6, 7600:2006:2@7 (confinement walls). The
//      filter also feeds party_phantom's boss-Insight observer (BB_PARTY_GUEST_INSIGHT not 0).
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace coop {

/// BB_PARTY set and BB_PARTY_SEAMLESS is not "0".
bool SeamlessRequested();
/// Call once from PartyInit after HooksInit (only when the party layer is on).
void SeamlessRulesInit();
/// Call once a frame from the main-thread coop tick.
void SeamlessRulesTick();

/// An EMEVD rewriter (party_fourp.cpp): runs on the dispatching thread for every instruction the
/// game is about to execute (not for skipped ones), before the game reads its arguments. `event`
/// is the SprjEmkEventIns, `instr` its instruction record; EmevdInstructionArgs finds the bytes.
using EmevdRewriter = void (*)(int event_id, int bank, int id, std::uint64_t event, std::uint64_t instr);
/// An extra skip predicate (party_b2: a client in the host's world never plays the opening
/// cutscene): true = the instruction is skipped like a BB_PARTY_EMEVD_SKIP rule. `event` / `instr`
/// as for EmevdRewriter (EmevdInstructionArgs finds the bytes). One; nullptr removes.
using EmevdSkipPredicate = bool (*)(int event_id, int bank, int id, std::uint64_t event, std::uint64_t instr);
void SetEmevdSkipPredicate(EmevdSkipPredicate fn);
/// A second predicate slot (party_npcfight: a guest copy's progress writes, D11), asked after the first.
void SetEmevdSkipPredicate2(EmevdSkipPredicate fn);
/// Sets the rewriter (one; nullptr removes it). Call before EnsureEmevdFilter.
void SetEmevdRewriter(EmevdRewriter fn);
/// A second, independent rewriter slot (party_guestsvc), run after the first.
void SetEmevdRewriter2(EmevdRewriter fn);
/// A third slot (party_worldact: key-item gates a guest opens with its own key, D6).
void SetEmevdRewriter3(EmevdRewriter fn);
/// Installs the EMEVD dispatch filter (0x17b93a0) now if SeamlessRulesInit did not (idempotent,
/// whether or not the seamless rules are on). True once installed.
bool EnsureEmevdFilter();
/// A11: a boss health bar is up (EMEVD 2003[11] seen with "on" and not yet "off"; a load resets
/// it): the host is in a boss fight.
bool BossBarShown();
/// The boss health bars stacked on screen (0 while none is up; 1 + the highest bar slot shown since
/// the bar came up): the party HUD keeps clear of them.
int BossBarRows();
/// Seconds since this game's last text banner (EMEVD 2007[2], e.g. PREY SLAUGHTERED) or boss
/// defeat instruction (2003[12/15/53]); large when none ran.
double BannerAgeSeconds();
void BossBarReset();
/// The argument bytes of instruction record `instr` of `event` (the copied arguments at
/// event+0xb8, else the EVD data); 0 when unknown. *size receives the argument size.
std::uint64_t EmevdInstructionArgs(std::uint64_t event, std::uint64_t instr, std::uint32_t* size);
/// The data of row `id` of the loaded game param `name` (file type `type`, e.g. "ItemLotParam",
/// "ITEM_LOT_PARAM_ST"), or 0 when the params are not loaded or the row does not exist. The
/// address stays valid until the params are reloaded (main thread).
std::uint64_t GameParamRow(const char* name, const char* type, std::uint32_t id);
/// Every row of a game param table as (id, row data address), in id order; empty without it.
std::vector<std::pair<std::uint32_t, std::uint64_t>> GameParamRows(const char* name, const char* type);

} // namespace coop
