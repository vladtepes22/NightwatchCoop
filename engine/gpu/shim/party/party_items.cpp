// SPDX-License-Identifier: GPL-3.0-or-later
// Party items, phase C3 (see party_items.h; the RE is docs/party/items.md).
//
// Every site below was disassembled from smoketest/out/eboot.elf (1.09) and is compared byte for
// byte before anything is written. Offsets are ours (raw ELF VA). The game's code is System V:
// every call into it goes through a BB_COOP_SYSV pointer at Guest(off).
#include "party_items.h"
#include "party_gamedata.h"
#include "party_kill_award_rules.h"
#include "party_item_notice_rules.h"
#include "bbport_game_menu.h"
#include "party_b2.h"
#include "party_async_io.h"
#include "party_perf.h"

#include "../../party_env.h"

#include <algorithm>
#include <random>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

#ifndef BB_PARTY_ITEMS_NO_GAME
#include "coop_hooks.h"
#include "party_npctalk.h"
#include "game_state.h"
#include "party_director.h"
#include "party_flow.h"
#include "party_progress.h"
#include "party_split.h"
#include "party_status.h"
#include "../bbport_settings.h"
#include "seamless_rules.h"

#include <atomic>
#include <set>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <mutex>
#include "../net/bbnet_internal.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#endif

namespace coop {

// ---------------------------------------------------------------------------------------------
// Pure part (unit-tested)
// ---------------------------------------------------------------------------------------------

namespace {

// The tables come from the player's own game (party_gamedata.h: gpu/shim/gamedata builds them).
using gamedata::AuditRowData;
using gamedata::AwardSpot;
using gamedata::BossDropRow;
using gamedata::LedgerRow;
using gamedata::LotFlag;
using gamedata::SharedTreasure;
inline const gamedata::Tables& Tab() {
    return gamedata::Get();
}

struct SourceName {
    ItemSource source;
    const char* name;
};
constexpr SourceName kSourceNames[] = {
    {ItemSource::Unknown, "unknown"}, {ItemSource::Award, "award"}, {ItemSource::Flag, "flag"},
    {ItemSource::Full, "full"},       {ItemSource::Mark, "mark"},   {ItemSource::Test, "test"},
    {ItemSource::Own, "own"},         {ItemSource::Repeat, "repeat"}, {ItemSource::Chalice, "chalice"},
    {ItemSource::BossDrop, "boss"},
};

const std::unordered_map<std::int32_t, std::int32_t>& FlagToLot() {
    // The lowest lot per flag: the table is sorted by lot, so the first row wins (a chain head
    // before its continuation rows, the NG variant 34000 before 34030).
    static const std::unordered_map<std::int32_t, std::int32_t> m = [] {
        std::unordered_map<std::int32_t, std::int32_t> r;
        for (const LotFlag& e : Tab().lot_flags) {
            r.emplace(e.flag, e.lot);
        }
        return r;
    }();
    return m;
}

constexpr std::int32_t kTreasureBase = 50000000;
// Common event 5500+slot's completion flags: "the player owns Uncanny / Lost weapon X" (the shop's gates).
constexpr std::int32_t kShopOwnFirst = 5500;
constexpr std::int32_t kShopOwnLast = 5521;

bool InRange(std::int32_t v, std::int32_t lo, std::int32_t hi) {
    return v >= lo && v <= hi;
}

} // namespace

const char* ItemSourceName(ItemSource s) {
    for (const SourceName& n : kSourceNames) {
        if (n.source == s) {
            return n.name;
        }
    }
    return "unknown";
}

ItemSource ItemSourceFromName(const std::string& name) {
    for (const SourceName& n : kSourceNames) {
        if (name == n.name) {
            return n.source;
        }
    }
    return ItemSource::Unknown;
}

std::int64_t ItemGrant::DoneFlag() const {
    if (flag >= 0) {
        return flag;
    }
    if (ledger >= 0 && ledger < kItemLedgerSize) {
        return std::int64_t(kItemLedgerBase) + ledger;
    }
    return -1;
}

std::string DescribeItem(const ItemGrant& g) {
    char text[160];
    if (g.flag >= 0) {
        std::snprintf(text, sizeof text, "#%llu lot %d (flag %d, %s)", static_cast<unsigned long long>(g.seq), g.lot,
                      g.flag, ItemSourceName(g.source));
    } else {
        std::snprintf(text, sizeof text, "#%llu lot %d (ledger %d = flag %lld, %s)",
                      static_cast<unsigned long long>(g.seq), g.lot, g.ledger, static_cast<long long>(g.DoneFlag()),
                      ItemSourceName(g.source));
    }
    return text;
}

std::int32_t ItemLotFlag(std::int32_t lot) {
    const auto b = Tab().lot_flags.begin();
    const auto e = Tab().lot_flags.end();
    const auto it = std::lower_bound(b, e, lot, [](const LotFlag& x, std::int32_t v) { return x.lot < v; });
    return it != e && it->lot == lot ? it->flag : kItemNone;
}

std::int32_t ItemLotForFlag(std::int32_t flag) {
    if (flag <= 0) {
        return kItemNone;
    }
    if (flag >= kTreasureBase && ItemLotFlag(flag - kTreasureBase) == flag) {
        return flag - kTreasureBase;
    }
    const auto& m = FlagToLot();
    const auto it = m.find(flag);
    return it != m.end() ? it->second : kItemNone;
}

std::int32_t ItemLedgerIndex(std::int32_t lot) {
    for (const LedgerRow& r : Tab().ledger) {
        if (r.lot == lot) {
            return r.idx;
        }
    }
    return kItemNone;
}

std::int32_t ItemLedgerDoneFlag(std::int32_t idx) {
    for (const LedgerRow& r : Tab().ledger) {
        if (r.idx == idx) {
            return r.done_flag;
        }
    }
    return kItemNone;
}

namespace {
// Per-player shop gates of weapons (tools/party/shop_gates.py): the gamedata `shopGates` section, derived from
// the game (gpu/shim/gamedata/derive_shop.cpp): the pickup flags of the Chalice lots of the base-game Uncanny /
// Lost weapons (5100-5282 in 1.09) and the "owns weapon X" completion flags of common event 5500+slot (the Old
// Hunters ones, 5500-5521). Progress-class gates (5090/5091, 59xx) are not in the table.
} // namespace

std::int32_t ShopGateWeapon(std::int32_t flag) {
    for (const gamedata::ShopGateData& r : Tab().shop_gates) {
        if (r.flag == flag) {
            return r.weapon;
        }
    }
    return 0;
}

ShopGateRepair ShopGatesToRepair(const std::function<bool(std::uint32_t)>& is_set,
                                 const std::function<int(std::int32_t)>& owns) {
    ShopGateRepair r;
    if (is_set(kShopGatesRepaired)) {
        r.done = true;
        return r;
    }
    if (Tab().shop_gates.empty()) {
        r.complete = false;  // no game-data tables (yet): do not mark the save as checked
        return r;
    }
    for (const gamedata::ShopGateData& row : Tab().shop_gates) {
        if (!is_set(static_cast<std::uint32_t>(row.flag))) {
            continue;
        }
        const int o = owns(row.weapon);
        if (o < 0) {
            r.complete = false; // cannot tell: leave the flag, try again
        } else if (o == 0) {
            r.clear.push_back(row.flag);
        }
    }
    return r;
}

std::vector<std::uint32_t> LotOwnGateFlags(std::int32_t lot) {
    if (lot == kEyeLot) {
        return {kFlagNight, kFlagDlcOwned};
    }
    return {};
}

std::vector<std::uint32_t> GoodsOwnGateFlags(std::int32_t goods) {
    if (goods == kEyeGoods) {
        return {kFlagNight, kFlagDlcOwned};
    }
    return {};
}

int OwnGateState(const std::vector<std::uint32_t>& flags, const std::function<bool(std::uint32_t, bool*)>& read) {
    int state = 1;
    for (const std::uint32_t f : flags) {
        bool v = false;
        if (!read(f, &v)) {
            state = -1;
        } else if (!v) {
            return 0;
        }
    }
    return state;
}

ShopGateRepair BadgeGatesToRepair(const std::function<bool(std::uint32_t)>& is_set,
                                  const std::function<int(std::int32_t)>& held) {
    ShopGateRepair r;
    if (is_set(kBadgeGatesRepaired)) {
        r.done = true;
        return r;
    }
    for (std::int32_t n = 0; n < kBadgeGateCount; ++n) {
        const std::int32_t flag = kBadgeGateFirst + n;
        if (!is_set(static_cast<std::uint32_t>(flag))) {
            continue;
        }
        const int h = held(kBadgeGoodsFirst + n);
        if (h < 0) {
            r.complete = false;  // a list is unreadable: keep the flag, ask again
        } else if (h == 0) {
            r.clear.push_back(flag);
        }
    }
    return r;
}

std::vector<std::int32_t> BadgeGuardStep(BadgeGuardState& st, const std::function<bool(std::int32_t)>& open,
                                         const std::function<int(std::int32_t)>& held) {
    std::vector<std::int32_t> close;
    for (std::int32_t n = 0; n < kBadgeGateCount; ++n) {
        if (!open(kBadgeGateFirst + n) || held(kBadgeGoodsFirst + n) != 0) {
            st.missing[n] = 0;  // closed, held, or a list not readable: nothing to decide
            continue;
        }
        if (++st.missing[n] >= kBadgeGuardConfirm) {
            st.missing[n] = 0;
            close.push_back(kBadgeGateFirst + n);
        }
    }
    return close;
}

EyeRepair EyeToRepair(const std::function<bool(std::uint32_t)>& is_set, int ng, int eye_held) {
    EyeRepair r;
    if (is_set(kEyeGateRepaired)) {
        r.done = true;
        r.why = "checked before";
        return r;
    }
    if (ng < 0 || eye_held < 0) {
        r.complete = false;
        r.why = ng < 0 ? "the NG cycle is not readable yet" : "the key items are not readable yet";
        return r;
    }
    // A later cycle starts the night over while key items can come along from the cycle before: not decided
    // there, left as it is.
    if (ng > 0) {
        r.why = "NG+ (key items can come from the cycle before): left as it is";
        return r;
    }
    if (is_set(kFlagDlcEntered)) {
        r.why = "the DLC was entered already (13401000)";
        return r;
    }
    if (is_set(kFlagNight)) {
        r.why = "night reached (9801): the vanilla gate holds";
        return r;
    }
    r.why = "no night (9801) and the DLC never entered: the vanilla Messenger gift and sale cannot have run";
    r.remove_eye = eye_held > 0;
    for (const std::uint32_t f : {static_cast<std::uint32_t>(kEyeLotFlag), kFlagEyeSaleEvent, kFlagEyeSale}) {
        if (is_set(f)) {
            r.clear.push_back(f);
        }
    }
    return r;
}

bool PerPlayerItemGateEvent(std::uint32_t event_id) {
    return event_id == static_cast<std::uint32_t>(kBadgeGateFirst) || event_id == 12405263u;
}

std::vector<std::uint32_t> LedgerBitsToSeed(const std::function<bool(std::uint32_t)>& is_set) {
    std::vector<std::uint32_t> out;
    if (is_set(kItemLedgerSeeded)) {
        return out;
    }
    for (const LedgerRow& r : Tab().ledger) {
        const std::uint32_t bit = kItemLedgerBase + static_cast<std::uint32_t>(r.idx);
        if (r.done_flag > 0 && is_set(static_cast<std::uint32_t>(r.done_flag)) && !is_set(bit) &&
            std::find(out.begin(), out.end(), bit) == out.end()) {
            out.push_back(bit);
        }
    }
    return out;
}


std::vector<std::int32_t> KeyCatchupLots(const std::function<bool(std::uint32_t)>& own_flag,
                                         const std::function<bool(std::uint32_t)>& boss_flag) {
    std::vector<std::int32_t> out;
    for (const LedgerRow& r : Tab().ledger) {
        const std::uint32_t bit = kItemLedgerBase + static_cast<std::uint32_t>(r.idx);
        if (r.done_flag <= 0 || ItemLotDenied(r.lot, kItemNone)) {
            continue;
        }
        const std::uint32_t done = static_cast<std::uint32_t>(r.done_flag);
        if (own_flag(bit) && own_flag(done) && boss_flag(done) &&
            std::find(out.begin(), out.end(), r.lot) == out.end()) {
            out.push_back(r.lot);
        }
    }
    // The shared progression keys / workshop tools found as treasure: picked up (or replayed)
    // while summoned, their flag reached the own save but the key did not (the guest's save had
    // 52411000, the Blood Gem Workshop Tool's, and no tool); the corpse never offers it again.
    // Only the progression keys / workshop tools (catchup 1): quest items may have been handed in.
    for (const SharedTreasure& t : Tab().shared_treasure) {
        const std::int32_t flag = ItemLotFlag(t.lot);
        if (t.catchup && flag > 0 && own_flag(static_cast<std::uint32_t>(flag)) &&
            std::find(out.begin(), out.end(), t.lot) == out.end()) {
            out.push_back(t.lot);
        }
    }
    return out;
}

std::int32_t ItemLedgerLot(std::int32_t idx) {
    for (const LedgerRow& r : Tab().ledger) {
        if (r.idx == idx) {
            return r.lot;
        }
    }
    return kItemNone;
}

std::vector<std::int32_t> ItemAwardSpotFlags(std::int32_t lot) {
    std::vector<std::int32_t> out;
    for (const AwardSpot& r : Tab().award_spots) {
        if (r.lot == lot && std::find(out.begin(), out.end(), r.spot_flag) == out.end()) {
            out.push_back(r.spot_flag);
        }
    }
    return out;
}

std::size_t ItemLotFlagCount() {
    return std::size(Tab().lot_flags);
}

std::size_t ItemLedgerCount() {
    return std::size(Tab().ledger);
}

bool MessengerGiftLot(std::int32_t lot) {
    return lot == 10000 || lot == 10010 || lot == 10050;
}

bool OwnTalkGiftLotId(std::int32_t lot) {
    return lot == 43120 || lot == 43130 || lot == 17000;  // 0.7.9: 17000 = Annalise's Caryll rune
}

bool RepeatGiftLotId(std::int32_t lot) {
    return lot == 17010 || lot == 23000 || lot == 24060 || lot == 28000 || lot == 32020;
}

bool SharedRepeatGiftLot(std::int32_t lot) {
    return lot == 23000;
}

std::optional<ItemGrant> MakeRepeatGrant(std::int32_t lot, std::uint64_t n) {
    if (!RepeatGiftLotId(lot)) {
        return std::nullopt;
    }
    ItemGrant g;
    g.lot = lot;
    g.flag = kItemNone;
    g.ledger = kItemNone;
    g.source = ItemSource::Repeat;
    g.repeat_key = kRepeatKeyBase + static_cast<std::int64_t>(n);
    return g;
}

bool ChaliceAwardLot(std::int32_t lot) {
    return lot >= 100000000 || (lot > 0 && lot / 100000 == 29);
}

std::optional<ItemGrant> MakeChaliceGrant(std::int32_t lot, const std::vector<GrantItem>& items, std::uint64_t n) {
    if (!ChaliceAwardLot(lot) || items.empty() || items.size() > kMaxGrantItems) {
        return std::nullopt;
    }
    ItemGrant g;
    g.lot = lot;
    g.flag = kItemNone;
    g.ledger = kItemNone;
    g.source = ItemSource::Chalice;
    g.items = items;
    g.repeat_key = kChaliceKeyBase + static_cast<std::int64_t>(n);
    return g;
}

namespace {
}  // namespace

int BossDropArea(std::int32_t lot) {
    for (const BossDropRow& r : Tab().boss_drops) {
        if (r.lot == lot) {
            return r.area;
        }
    }
    return -1;
}

std::optional<ItemGrant> MakeBossDropGrant(std::int32_t lot, const std::vector<GrantItem>& items, std::uint64_t n) {
    if (BossDropArea(lot) < 0 || items.empty() || items.size() > kMaxGrantItems) {
        return std::nullopt;
    }
    ItemGrant g;
    g.lot = lot;
    g.flag = kItemNone;
    g.ledger = kItemNone;
    g.source = ItemSource::BossDrop;
    g.items = items;
    g.repeat_key = kBossDropKeyBase + static_cast<std::int64_t>(n);
    return g;
}

bool BossDropAccepted(bool summoned, std::uint32_t map_id, std::int32_t lot) {
    const int area = BossDropArea(lot);
    return summoned && area >= 0 && map_id != 0xffffffffu && static_cast<int>(map_id >> 24) == area;
}

bool ChaliceGrantAccepted(bool summoned, std::uint32_t map_id) {
    return summoned && map_id != 0xffffffffu && (map_id >> 24) == 29;
}

bool ItemLotUnshared(std::int32_t lot) {
    return std::binary_search(Tab().unshared.begin(), Tab().unshared.end(), lot);
}

bool ItemLotDenied(std::int32_t lot, std::int32_t flag) {
    if (lot <= 0 || lot >= 100000000) {
        return true; // unresolved; Chalice Dungeon lots (1100xxxxx, 2001xxxxx)
    }
    if (lot / 100000 == 29) {
        return true; // area 29 treasure (Chalice Dungeons)
    }
    if (flag > 0 && (flag / 10000000 == 1 || flag / 10000000 == 5) && (flag / 100000) % 100 == 29) {
        return true; // area 29 flags
    }
    // DLC boss rewards whose flags live in the personal block 6000-8999 (Ludwig 3401800 -> 6674,
    // Laurence 3401850 -> 6673): a phantom never gets them (2003[4] is skipped on clients) and
    // the flag is the guest's own, so a replay is exact. Every other lot with a personal flag is
    // a per-player gift (Messenger 10000/10010, DLC NPCs 43120/43130, 43800).
    if (lot == 3401800 || lot == 3401850) {
        return false;
    }
    // The Old Hunter Bell (lot 10050, personal flag 6670): its Messenger event 12101026 awards it
    // only for chr type 0 (the 2003[4] is skipped on a client), so a guest living in the host's
    // Dream never got it, and the talk flag 12101027 it waits for went to the host's world (the
    // host got the bell from the guest's talk). Replayed from the host like the Eye (10040); the
    // flag is each player's own, so the replay gives it once per player (0.7.3).
    if (lot == 10050) {
        return false;
    }
    if (InRange(flag, 6000, 8999)) {
        return true;
    }
    // Party policy (0.7.5): only key items and unique items are shared. A lot of consumables and
    // repeatable stacks stays with the player who found it (never broadcast, replayed or listed
    // in the catch-up).
    if (ItemLotUnshared(lot)) {
        return true;
    }
    switch (lot) {
    // Messenger gifts (per player). Not 10040, the Eye of a Blood-drunk Hunter (DLC entry): its
    // event 12101028 needs chr type 0, so it never runs on a summoned guest; the host's copy is
    // replayed to everyone (flag 50000100) or a guest who never stands in its own Dream lacks it.
    // Not 10050 either (the Old Hunter Bell, above). The Beckoning Bell comes from the director.
    case 10000: case 10010: case 10011:
    case 10500: case 11500: case 12500:                         // covenant gems (common 9440)
    case 17010: case 24060: case 28000: case 32020:             // common 9100 (repeatable)
    case 43000:                                                 // 13501940 (repeatable)
    case 2100900: case 2100910: case 2100920:                   // 12105064 DLC messenger hats
    case 43802:                                                 // cycle variant of 43800
    case 5020: case 16581:                                      // PvP / NPC kill texts
        return true;
    default:
        break;
    }
    if (InRange(lot, 100000, 101999)) {
        return true; // rune / gem use (common 9500)
    }
    if (InRange(lot, 5500, 5959)) {
        return true; // covenant / PvP lots 5500-5950 and their rows
    }
    return false;
}

std::optional<ItemGrant> ClassifyAward(std::int32_t lot, bool host_only) {
    if (!host_only) {
        return std::nullopt; // clients get it in vanilla (2003[36], GetRateItem_IgnoreMultiPlay)
    }
    ItemGrant g;
    g.lot = lot;
    g.flag = ItemLotFlag(lot);
    g.source = ItemSource::Award;
    if (ItemLotDenied(lot, g.flag)) {
        return std::nullopt;
    }
    if (g.flag < 0) {
        g.ledger = ItemLedgerIndex(lot);
        if (g.ledger < 0) {
            return std::nullopt; // flagless and not in the ledger: no way to give it exactly once
        }
    }
    return g;
}

std::optional<ItemGrant> ClassifyFlag(std::int32_t flag, const std::map<std::int32_t, std::int32_t>& captured) {
    ItemGrant g;
    const auto it = captured.find(flag);
    g.lot = it != captured.end() ? it->second : ItemLotForFlag(flag);
    if (g.lot < 0) {
        return std::nullopt;
    }
    g.flag = flag;
    g.source = ItemSource::Flag;
    if (ItemLotDenied(g.lot, flag)) {
        return std::nullopt;
    }
    return g;
}

bool SharedKeyTreasureLot(std::int32_t lot) {
    static const bool off = [] {
        const char* v = std::getenv("BB_PARTY_SHARED_TREASURE");
        return v && v[0] == '0' && !v[1];
    }();  // test switch: the old per-player rule
    if (off) {
        return false;
    }
    for (const SharedTreasure& t : Tab().shared_treasure) {
        if (t.lot == lot) {
            return true;
        }
    }
    return false;
}

namespace {
TreasureHider g_treasure_hider = nullptr;
}
void SetTreasureHider(TreasureHider h) {
    g_treasure_hider = h;
}
std::uint32_t TreasureFlagToHide(std::int32_t lot) {
    if (!(lot > 0 && lot < 10000000 && SharedKeyTreasureLot(lot))) {
        return 0u;
    }
    // The lot's own pickup flag (0.7.7): 50,000,000 + lot only holds for the plain rows (2420231's corpse flag is
    // 52420230, the Cannon 2400550's 52420550).
    const std::int32_t f = ItemLotFlag(lot);
    return f > 0 ? std::uint32_t(f) : 50000000u + std::uint32_t(lot);
}

SharePath ItemLotSharePath(std::int32_t lot) {
    const std::int32_t flag = ItemLotFlag(lot);
    if (ItemLotDenied(lot, flag)) {
        return SharePath::None;
    }
    if (flag > 0) {
        if (InstancedTreasureFlag(flag, true, false)) {
            return SharePath::None;  // ordinary world treasure: every player takes their own
        }
        return SharedKeyTreasureLot(lot) ? SharePath::Treasure : SharePath::Flag;
    }
    if (ItemLedgerIndex(lot) >= 0) {
        return SharePath::Ledger;
    }
    return BossDropArea(lot) >= 0 ? SharePath::BossDrop : SharePath::None;
}

namespace {
}  // namespace

namespace {
}  // namespace

std::vector<KillAwardRow> KillAwardRows() {
    std::vector<KillAwardRow> out;
    for (const gamedata::KillAward& r : Tab().kill_awards) {
        out.push_back({r.lot, r.area, r.entity, r.event, r.spot_flag});
    }
    return out;
}

std::uint32_t KillAwardEntity(std::int32_t lot) {
    for (const gamedata::KillAward& r : Tab().kill_awards) {
        if (r.lot == lot) {
            return r.entity;
        }
    }
    return 0;
}

std::vector<ItemAuditRow> ItemAuditRows() {
    std::vector<ItemAuditRow> out;
    for (const AuditRowData& r : Tab().audit_rows) {
        out.push_back({r.lot, r.required, r.note.c_str()});
    }
    return out;
}

bool InstancedTreasureFlag(std::int32_t flag, bool instanced, bool captured) {
    if (!(instanced && !captured && flag >= 50000000 && flag < 60000000 && flag / 100000 % 100 != 0)) {
        return false;
    }
    return !SharedKeyTreasureLot(ItemLotForFlag(flag));
}

json::Value ItemsToJson(const std::vector<ItemGrant>& items) {
    json::Value v = json::Value::make_object();
    json::Value a = json::Value::make_array();
    for (const ItemGrant& g : items) {
        json::Value row = json::Value::make_array();
        row.push(static_cast<unsigned long long>(g.seq)); // < 2^53: exact as a double
        row.push(g.lot);
        row.push(g.flag);
        row.push(g.ledger);
        row.push(ItemSourceName(g.source));
        if (!g.items.empty()) {
            json::Value list = json::Value::make_array();
            for (const GrantItem& it : g.items) {
                json::Value e = json::Value::make_array();
                e.push(static_cast<unsigned long long>(it.category));
                e.push(it.id);
                e.push(it.count);
                if (it.has_gem) {
                    json::Value gem = json::Value::make_array();
                    for (const std::int32_t x : it.gem) {
                        gem.push(x);
                    }
                    e.push(gem);
                }
                list.push(e);
            }
            row.push(list);
        }
        a.push(row);
    }
    v.set("items", a);
    return v;
}

bool ItemsFromJson(const json::Value& v, std::vector<ItemGrant>* out, std::string* error) {
    try {
        if (v.type != json::Value::Type::Object) {
            throw std::runtime_error("not an object");
        }
        std::vector<ItemGrant> r;
        if (json::arr(v, "items").size() > kMaxItemsPerEvent) {
            throw std::runtime_error("items: too many rows");
        }
        for (const json::Value& row : json::arr(v, "items")) {
            if (row.type != json::Value::Type::Array || (row.array.size() != 5 && row.array.size() != 6)) {
                throw std::runtime_error("items: a row needs [seq, lot, flag, ledger, source(, items)]");
            }
            auto integer = [](const json::Value& x, const char* what) -> double {
                // Range first: converting 1e300 to an integer is undefined.
                if (x.type != json::Value::Type::Number || !(x.number > -9.0e15 && x.number < 9.0e15) ||
                    x.number != static_cast<double>(static_cast<long long>(x.number))) {
                    throw std::runtime_error(std::string("items: ") + what + " is not an integer");
                }
                return x.number;
            };
            ItemGrant g;
            const double seq = integer(row.array[0], "seq");
            if (seq < 0) {
                throw std::runtime_error("items: seq is negative");
            }
            g.seq = static_cast<std::uint64_t>(seq);
            const double lot = integer(row.array[1], "lot"), flag = integer(row.array[2], "flag"),
                         ledger = integer(row.array[3], "ledger");
            if (lot <= 0 || lot > 2147483647.0 || flag < -1 || flag > 2147483647.0 || ledger < -1 ||
                ledger >= kItemLedgerSize) {
                throw std::runtime_error("items: value out of range");
            }
            g.lot = static_cast<std::int32_t>(lot);
            g.flag = static_cast<std::int32_t>(flag);
            g.ledger = static_cast<std::int32_t>(ledger);
            if (row.array[4].type != json::Value::Type::String) {
                throw std::runtime_error("items: source is not a string");
            }
            g.source = ItemSourceFromName(row.array[4].string);
            if (g.source == ItemSource::Chalice) {
                // G1: a Chalice award has no flag; the host's items are required (checked below).
                if (g.flag != kItemNone || g.ledger != kItemNone || !ChaliceAwardLot(g.lot) || row.array.size() != 6) {
                    throw std::runtime_error("items: a chalice row needs a dungeon lot, no flag and its items");
                }
            } else if (g.source == ItemSource::BossDrop) {
                if (g.flag != kItemNone || g.ledger != kItemNone || BossDropArea(g.lot) < 0 || row.array.size() != 6) {
                    throw std::runtime_error("items: a boss drop row needs a boss lot, no flag and its items");
                }
            } else if (g.DoneFlag() < 0) {
                throw std::runtime_error("items: a row needs a flag or a ledger index");
            }
            if (row.array.size() == 6) {
                const json::Value& list = row.array[5];
                if (list.type != json::Value::Type::Array || list.array.size() > kMaxGrantItems) {
                    throw std::runtime_error("items: the item list is not an array of at most 16");
                }
                for (const json::Value& e : list.array) {
                    if (e.type != json::Value::Type::Array || (e.array.size() != 3 && e.array.size() != 4)) {
                        throw std::runtime_error("items: an item needs [category, id, count(, gem)]");
                    }
                    const double cat = integer(e.array[0], "category"), id = integer(e.array[1], "item id"),
                                 count = integer(e.array[2], "count");
                    if (cat < 0 || cat > 15 || id <= 0 || id > 2147483647.0 || count < 1 || count > 255) {
                        throw std::runtime_error("items: item value out of range");
                    }
                    GrantItem it;
                    it.category = static_cast<std::uint32_t>(cat);
                    it.id = static_cast<std::int32_t>(id);
                    it.count = static_cast<std::int32_t>(count);
                    if (e.array.size() == 4) {
                        const json::Value& gem = e.array[3];
                        if (gem.type != json::Value::Type::Array || gem.array.size() != kGemFields) {
                            throw std::runtime_error("items: gem data needs 8 integers");
                        }
                        for (std::size_t k = 0; k < kGemFields; ++k) {
                            const double x = integer(gem.array[k], "gem field");
                            if (x < -1 || x > 2147483647.0) {
                                throw std::runtime_error("items: gem field out of range");
                            }
                            it.gem[k] = static_cast<std::int32_t>(x);
                        }
                        it.has_gem = true;
                    }
                    if (!GrantItemSupported(it)) {
                        throw std::runtime_error("items: item category / gem data not accepted");
                    }
                    g.items.push_back(it);
                }
            }
            r.push_back(g);
        }
        *out = std::move(r);
        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

bool ItemGrantFromPeerAllowed(const ItemGrant& g, std::string* why) {
    auto no = [&](const char* w) {
        if (why) {
            *why = w;
        }
        return false;
    };
    if (g.source == ItemSource::Chalice) {
        // G1: a dungeon lot with exactly the host's items, no flag; given only in the dungeon.
        if (!ChaliceAwardLot(g.lot) || g.flag != kItemNone || g.ledger != kItemNone || g.items.empty() ||
            g.items.size() > kMaxGrantItems) {
            return no("a chalice award needs a dungeon lot, no flag and the host's items");
        }
        return true;
    }
    if (g.source == ItemSource::BossDrop) {
        // 0.7.6: a world boss's lot with exactly the host's items, no flag; given only where the boss was.
        if (BossDropArea(g.lot) < 0 || g.flag != kItemNone || g.ledger != kItemNone || g.items.empty() ||
            g.items.size() > kMaxGrantItems) {
            return no("a boss drop needs a boss lot, no flag and the host's items");
        }
        return true;
    }
    if (g.source != ItemSource::Award && g.source != ItemSource::Flag && g.source != ItemSource::Full) {
        return no("not a host source (mark / test rows are guest-local)");
    }
    if (ItemLotDenied(g.lot, g.flag)) {
        return no("lot denied");
    }
    if (g.flag < 0) {
        if (g.ledger < 0 || ItemLedgerLot(g.ledger) != g.lot) {
            return no("ledger row does not match the lot");
        }
        return true;
    }
    const std::int32_t table_flag = ItemLotFlag(g.lot);
    if (table_flag == g.flag || ItemLotForFlag(g.flag) == g.lot) {
        return true;
    }
    if (table_flag != kItemNone) {
        return no("the lot has another flag");
    }
    if (ItemLotForFlag(g.flag) != kItemNone) {
        return no("the flag belongs to another lot");
    }
    // A lot / flag pair outside the tables. An honest host of the same game version never sends
    // one: ClassifyAward takes the flag from ITEM_LOT_FLAG and ClassifyFlag only maps flags the
    // table (or a capture of a table lot) gives, so this would be an arbitrary event flag write
    // plus an arbitrary lot award in the guest's own save. Refused (docs/party/items.md 6.5).
    return no("lot / flag pair outside the tables");
}

std::vector<ItemGrant> FilterPeerItems(const std::vector<ItemGrant>& in, std::size_t* rejected) {
    std::vector<ItemGrant> out;
    std::size_t bad = 0;
    for (const ItemGrant& g : in) {
        if (ItemGrantFromPeerAllowed(g)) {
            out.push_back(g);
        } else {
            ++bad;
        }
    }
    if (rejected) {
        *rejected = bad;
    }
    return out;
}

bool GuestGiftShareable(std::int32_t lot, std::int32_t flag) {
    if (SharedRepeatGiftLot(lot)) {
        // A flagless ledger lot: reported with its ledger bit, which the host's own award sets.
        const std::int32_t idx = ItemLedgerIndex(lot);
        return idx >= 0 && flag == static_cast<std::int32_t>(kItemLedgerBase) + idx && !ItemLotDenied(lot, kItemNone);
    }
    return flag > 0 && ItemLotFlag(lot) == flag && !ItemLotDenied(lot, flag) &&
           !InstancedTreasureFlag(flag, true, false);
}

std::string GiftUpToJsonText(std::int32_t lot, std::int32_t flag) {
    json::Value v = json::Value::make_object();
    v.set("lot", lot);
    v.set("flag", flag);
    return json::dump(v, 0);
}

bool GiftUpFromJsonText(const std::string& text, std::int32_t* lot, std::int32_t* flag, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err) || v.type != json::Value::Type::Object) {
        return fail("gift_up: not a JSON object");
    }
    const json::Value* l = v.find("lot");
    const json::Value* f = v.find("flag");
    auto integer = [](const json::Value* x, std::int32_t* out) {
        // Range first: converting 1e300 to an integer is undefined.
        if (!x || x->type != json::Value::Type::Number || !(x->number >= -2147483648.0 && x->number <= 2147483647.0) ||
            x->number != static_cast<double>(static_cast<long long>(x->number))) {
            return false;
        }
        *out = static_cast<std::int32_t>(x->number);
        return true;
    };
    std::int32_t a = 0, b = 0;
    if (!integer(l, &a) || !integer(f, &b)) {
        return fail("gift_up: lot / flag must be 32-bit integers");
    }
    if (!GuestGiftShareable(a, b)) {
        return fail("gift_up: not a shareable gift (unknown lot / flag pair, denied, or map treasure)");
    }
    *lot = a;
    *flag = b;
    return true;
}

bool GiftRateLimit::Take(int slot, double now) {
    if (slot < 0 || slot >= 8) {
        return false;
    }
    if (now - start_[slot] >= kWindow) {
        start_[slot] = now;
        used_[slot] = 0;
    }
    if (used_[slot] >= kPerWindow) {
        return false;
    }
    ++used_[slot];
    return true;
}

std::string ItemsToJsonText(const std::vector<ItemGrant>& items) {
    return json::dump(ItemsToJson(items), 0);
}

bool ItemsFromJsonText(const std::string& text, std::vector<ItemGrant>* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) {
        if (error) {
            *error = err;
        }
        return false;
    }
    return ItemsFromJson(v, out, error);
}

bool HostItems::Offer(ItemGrant g) {
    const std::int64_t key = g.Key();
    if (key < 0 || !sent_.insert(key).second) {
        return false;
    }
    g.seq = NextSeq();
    if (q_.size() >= kMaxQueue) {
        q_.pop_front();
    }
    q_.push_back(g);
    return true;
}

bool HostItems::Pop(ItemGrant* out) {
    if (q_.empty()) {
        return false;
    }
    *out = q_.front();
    q_.pop_front();
    return true;
}

bool ItemApplyAllowedNow(const ItemApplyState& s) {
    return s.world_up && !s.loading && s.load_mode == 0 && s.own_world == 1 && s.game_data &&
           (s.session_role == 0 /* idle */ || s.session_role == 3 /* hosting in the own world */);
}

bool ItemSummonedApplyAllowedNow(const ItemApplyState& s) {
    return s.world_up && !s.loading && s.load_mode == 1 && s.own_world == 0 && s.game_data &&
           s.session_role == 6 /* client in the host's world */;
}

const char* ItemPopupHold(const ItemApplyState& s) {
    if (!s.alive) {
        return "dead";
    }
    if (s.cutscene) {
        return "cutscene";
    }
    if (!s.boss_quiet) {
        return "boss defeat banner";
    }
    if (s.game_dialog) {
        return "game message";
    }
    if (!s.calm) {
        return "in a fight";
    }
    return nullptr;
}

bool ItemGetWatch::Step(double now, int entries, bool armed, bool alive, bool loading, const char** why) {
    if (entries <= 0) {
        up_ = crossed_load_ = false;
        dead_since_ = held_since_ = -1;
        return false;
    }
    if (!up_) {
        up_ = true;
        up_since_ = now;
        crossed_load_ = false;
        dead_since_ = held_since_ = -1;
    }
    if (loading) {
        crossed_load_ = true; // cleared once the load is over (the game runs the list again)
        dead_since_ = held_since_ = -1;
        return false;
    }
    const char* r = nullptr;
    if (crossed_load_) {
        r = "left over from before a load";
    }
    if (!alive) {
        dead_since_ = dead_since_ < 0 ? now : dead_since_;
        if (!r && now - dead_since_ >= kDeadFor) {
            r = "the player is dead";
        }
    } else {
        dead_since_ = -1;
    }
    if (!armed) {
        held_since_ = held_since_ < 0 ? now : held_since_;
        if (!r && now - held_since_ >= kHeldFor) {
            r = "its confirm check stays held";
        }
    } else {
        held_since_ = -1;
    }
    if (!r) {
        return false;
    }
    if (why) {
        *why = r;
    }
    up_ = crossed_load_ = false;
    dead_since_ = held_since_ = -1;
    return true;
}

ItemApplyMode ItemApplyModeNow(const ItemApplyState& s) {
    if (ItemApplyAllowedNow(s)) {
        return ItemApplyMode::Home;
    }
    return ItemSummonedApplyAllowedNow(s) ? ItemApplyMode::Summoned : ItemApplyMode::None;
}

bool GrantCategorySupported(std::uint32_t category) {
    return category == 0 || category == 1 || category == 2 || category == 4;
}

bool GemDataPlausible(const std::int32_t (&gem)[kGemFields]) {
    if (gem[0] < 0 || gem[0] > 3 || gem[1] < 0 || gem[1] > 1000) {
        return false;
    }
    for (std::size_t k = 2; k < kGemFields; ++k) {
        if (gem[k] < -1 || gem[k] > 99999999) {
            return false;
        }
    }
    return true;
}

bool GrantItemSupported(const GrantItem& it) {
    if (it.category == 8) {
        return it.has_gem && it.count == 1 && it.id > 0 && GemDataPlausible(it.gem);
    }
    return !it.has_gem && GrantCategorySupported(it.category);
}

const char* LotPlanResultName(LotPlanResult r) {
    switch (r) {
    case LotPlanResult::Give: return "give";
    case LotPlanResult::Random: return "random lot without the host's item list";
    case LotPlanResult::Unsupported: return "blood gem without the host's gem data / unsupported category";
    case LotPlanResult::Missing: return "no ItemLotParam row";
    }
    return "?";
}

namespace {

bool SlotWeighted(const LotRowData& r, int i) {
    return r.base[i] > 0 || r.cumulate[i] > 0;
}

bool SlotHasItem(const LotRowData& r, int i) {
    return r.id[i] > 0 && r.category[i] != 0xffffffffu;
}

int WeightedSlots(const LotRowData& r, int* only) {
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        if (SlotWeighted(r, i)) {
            if (only) {
                *only = i;
            }
            ++n;
        }
    }
    return n;
}

int SlotCount(const LotRowData& r, int i) {
    return r.num[i] > 0 ? r.num[i] : 1;
}

} // namespace

bool LotChainHasGem(const std::vector<LotRowData>& chain) {
    for (const LotRowData& r : chain) {
        for (int i = 0; i < 8; ++i) {
            if (SlotWeighted(r, i) && SlotHasItem(r, i) && r.category[i] == 8) {
                return true;
            }
        }
    }
    return false;
}

bool LotChainRandom(const std::vector<LotRowData>& chain) {
    for (const LotRowData& r : chain) {
        if (WeightedSlots(r, nullptr) > 1) {
            return true;
        }
    }
    return false;
}

std::vector<GrantItem> RollLotChain(const std::vector<LotRowData>& chain, std::uint64_t seed) {
    std::vector<GrantItem> out;
    std::uint64_t x = seed ? seed : 0x9e3779b97f4a7c15ull;
    for (const LotRowData& r : chain) {
        std::uint32_t total = 0;
        for (int i = 0; i < 8; ++i) {
            total += r.base[i];
        }
        if (total == 0) {
            int only = -1;
            if (WeightedSlots(r, &only) == 1 && SlotHasItem(r, only) && GrantCategorySupported(r.category[only])) {
                out.push_back({r.category[only], r.id[only], SlotCount(r, only)}); // a cumulate-only slot
            }
            continue;
        }
        x ^= x << 13; // xorshift64
        x ^= x >> 7;
        x ^= x << 17;
        std::uint32_t pick = static_cast<std::uint32_t>(x % total);
        for (int i = 0; i < 8; ++i) {
            if (pick < r.base[i]) {
                if (SlotHasItem(r, i) && GrantCategorySupported(r.category[i])) {
                    out.push_back({r.category[i], r.id[i], SlotCount(r, i)});
                }
                break;
            }
            pick -= r.base[i];
        }
    }
    return out;
}

LotPlan PlanSummonedLot(const std::vector<LotRowData>& chain, const std::vector<GrantItem>& host_items,
                        const std::function<bool(std::int32_t)>& row_done) {
    LotPlan p;
    if (chain.empty()) {
        p.result = LotPlanResult::Missing;
        p.why = "no ItemLotParam row";
        return p;
    }
    auto done = [&](const LotRowData& r) { return r.flag > 0 && row_done && row_done(r.flag); };
    auto add_flag = [&](const LotRowData& r) {
        if (r.flag > 0 && std::find(p.row_flags.begin(), p.row_flags.end(), r.flag) == p.row_flags.end()) {
            p.row_flags.push_back(r.flag);
        }
    };
    if (!host_items.empty()) {
        // The host's list: every item must be a weighted slot of a different row (what one roll
        // of the chain can give), else it is ignored and the rows decide.
        std::vector<bool> used(chain.size(), false);
        std::vector<int> row_of(host_items.size(), -1);
        bool ok = host_items.size() <= chain.size();
        for (std::size_t k = 0; k < host_items.size() && ok; ++k) {
            const GrantItem& it = host_items[k];
            ok = false;
            for (std::size_t r = 0; r < chain.size() && !ok; ++r) {
                if (used[r]) {
                    continue;
                }
                for (int i = 0; i < 8; ++i) {
                    // A blood gem: the generated id is the slot's plus the area's rank offset.
                    const bool id_ok = it.category == 8
                                           ? it.has_gem && it.count == 1 && it.id >= chain[r].id[i] &&
                                                 it.id - chain[r].id[i] <= kMaxGemIdOffset
                                           : chain[r].id[i] == it.id;
                    if (SlotWeighted(chain[r], i) && SlotHasItem(chain[r], i) && chain[r].category[i] == it.category &&
                        id_ok && it.count >= 1 && it.count <= SlotCount(chain[r], i)) {
                        used[r] = true;
                        row_of[k] = static_cast<int>(r);
                        ok = true;
                        break;
                    }
                }
            }
        }
        if (ok) {
            for (std::size_t k = 0; k < host_items.size(); ++k) {
                const LotRowData& r = chain[static_cast<std::size_t>(row_of[k])];
                if (done(r)) {
                    continue;
                }
                if (!GrantItemSupported(host_items[k])) {
                    p.result = LotPlanResult::Unsupported;
                    p.why = "category " + std::to_string(host_items[k].category);
                    p.items.clear();
                    p.row_flags.clear();
                    return p;
                }
                p.items.push_back(host_items[k]);
                add_flag(r);
            }
            // Rows the host's roll left empty are done as well (vanilla sets every record's flag).
            for (std::size_t r = 0; r < chain.size(); ++r) {
                if (!used[r] && !done(chain[r])) {
                    add_flag(chain[r]);
                }
            }
            p.result = LotPlanResult::Give;
            p.why = "host's item list";
            return p;
        }
        p.why = "host's item list does not match the rows; ";
    }
    for (const LotRowData& r : chain) {
        if (done(r)) {
            continue;
        }
        int only = -1;
        const int n = WeightedSlots(r, &only);
        if (n > 1) {
            p.result = LotPlanResult::Random;
            p.why += "row " + std::to_string(r.lot) + " has " + std::to_string(n) + " weighted slots";
            p.items.clear();
            p.row_flags.clear();
            return p;
        }
        add_flag(r);
        if (n == 0 || !SlotHasItem(r, only)) {
            continue; // an empty row gives nothing
        }
        if (!GrantCategorySupported(r.category[only])) {
            p.result = LotPlanResult::Unsupported;
            p.why += "row " + std::to_string(r.lot) + " gives category " + std::to_string(r.category[only]);
            p.items.clear();
            p.row_flags.clear();
            return p;
        }
        p.items.push_back({r.category[only], r.id[only], SlotCount(r, only)});
    }
    p.result = LotPlanResult::Give;
    p.why += "deterministic rows";
    return p;
}

void GoodsLimitsFromRow(const std::uint8_t* row, LotSlotState* s) {
    std::int16_t mx = 0, rep = 0;
    std::memcpy(&mx, row + 0x36, sizeof mx);
    std::memcpy(&rep, row + 0x48, sizeof rep);
    s->max_num = mx;
    s->max_storage = rep;
    s->goods_type = row[0x3A];
    // refId s32 +0x00 (the use's SpEffect / behaviour), goodsUseAnim u8 +0x3E: a goods item the
    // player can use (and so use up: the Gold Pendant becomes a blood gem).
    std::int32_t ref = -1;
    std::memcpy(&ref, row + 0x00, sizeof ref);
    s->usable = (ref >= 0 || row[0x3E] != 0) ? 1 : 0;
}

const char* LotFitName(LotFit f) {
    switch (f) {
    case LotFit::Give: return "fits";
    case LotFit::Owned: return "unique item held already";
    case LotFit::Full: return "would exceed the item's max";
    }
    return "?";
}

LotFit ClassifyLotFit(const std::vector<LotSlotState>& slots, std::string* why) {
    LotFit fit = LotFit::Give;
    for (const LotSlotState& s : slots) {
        // Goods only: weapons / armor / runes have no count limit the award can hit.
        if (s.category == 4 && s.id > 0 && s.goods_type == 1 && s.owned >= 1) {
            // A key item never goes to the storage: a second copy is refused with the blocking box
            // whatever maxRepositoryNum says (0.6.1: lot 31000 with the Oedon Tomb Key held).
            fit = LotFit::Owned;
            if (why) {
                *why = "goods " + std::to_string(s.id) + ": key item held " + std::to_string(s.owned);
            }
            continue;
        }
        if (s.category != 4 || s.id <= 0 || s.owned < 0 || s.max_num <= 0) {
            continue;
        }
        const int num = s.num > 0 ? s.num : 1;
        if (s.owned + num <= s.max_num || s.max_storage > 0) {
            continue; // fits, or the overflow goes to the storage
        }
        const LotFit f = s.max_num == 1 && s.owned >= 1 ? LotFit::Owned : LotFit::Full;
        if (fit == LotFit::Give || f == LotFit::Owned) {
            fit = f;
            if (why) {
                char b[160];
                std::snprintf(b, sizeof b, "goods %d: held %d + %d > max %d, no storage", s.id, s.owned, num,
                              s.max_num);
                *why = b;
            }
        }
    }
    return fit;
}

int SummonedRegiveCount(int expect, int now, int count) {
    if (expect < 0 || now < 0 || count <= 0 || now >= expect) {
        return 0;
    }
    return std::min(count, expect - now);
}

bool LedgerSkipSuspicious(const std::vector<LotSlotState>& slots) {
    if (slots.empty()) {
        return false;
    }
    for (const LotSlotState& s : slots) {
        if (s.category != 4 || s.max_num != 1 || s.max_storage > 0 || s.owned != 0) {
            return false;
        }
    }
    return true;
}

bool LotHasKeyItem(const std::vector<LotSlotState>& slots) {
    for (const LotSlotState& s : slots) {
        if (s.category == 4 && s.goods_type == 1) {
            return true;
        }
    }
    return false;
}

bool KeyGoods(const LotSlotState& s) {
    return s.category == 4 && s.id > 0 && (s.goods_type == 1 || (s.max_num == 1 && s.max_storage <= 0));
}

bool LotHasKeyGoods(const std::vector<LotSlotState>& slots) {
    for (const LotSlotState& s : slots) {
        if (KeyGoods(s)) {
            return true;
        }
    }
    return false;
}

bool SummonedGiveAllowed(const std::vector<LotSlotState>& slots) {
    return !LotHasKeyGoods(slots);
}

bool RepeatSummonedGiveAllowed(const std::vector<LotSlotState>& slots) {
    for (const LotSlotState& s : slots) {
        if (KeyGoods(s) && s.goods_type != 0) {
            return false;
        }
    }
    return true;
}

bool KeyGiveAllowedHeld(const LotSlotState& s, int held) {
    return !KeyGoods(s) || held == 0;
}

bool KeyRegiveAllowed(const LotSlotState& s) {
    return KeyGoods(s) && s.usable == 0;
}

bool KeyCatchupNeeded(const std::vector<LotSlotState>& slots) {
    if (!LedgerSkipSuspicious(slots) || !LotHasKeyGoods(slots)) {
        return false;
    }
    for (const LotSlotState& s : slots) {
        if (!KeyRegiveAllowed(s)) {
            return false; // a usable unique item (Gold Pendant, Tear Stone): reading 0, it was used
        }
    }
    return true;
}

bool SummonedKeyGiveNow(const std::vector<LotSlotState>& slots) {
    for (const LotSlotState& s : slots) {
        if (KeyGoods(s) && !KeyRegiveAllowed(s)) {
            return false;
        }
    }
    return true;
}

bool KeyViewOn(const char* env) {
    return !(env && env[0] == '0' && !env[1]);
}

bool SummonedKeyGiveNowRule(bool key_view, const char* give_env, bool usable) {
    if (key_view) {
        return !(give_env && give_env[0] == '0' && !give_env[1]);
    }
    return !usable && give_env && give_env[0] == '1' && !give_env[1];
}

bool OwedGiveAllowed(bool given, bool done_flag_set, const LotSlotState& s, int held) {
    if (given) {
        return KeyRegiveAllowed(s) && held == 0;
    }
    return !done_flag_set && KeyGiveAllowedHeld(s, held);
}

bool SummonedGiveForSlot(int entry_slot, int current_slot) {
    return entry_slot < 0 || current_slot < 0 || entry_slot == current_slot;
}

SgiveOwner SummonedGiveOwner(int entry_slot, std::uint32_t entry_id, int cur_slot, std::uint32_t cur_id, bool enforce) {
    if (!enforce) {
        return SummonedGiveForSlot(entry_slot, cur_slot) ? SgiveOwner::Replay : SgiveOwner::KeepForOther;
    }
    if (entry_slot < 0 || entry_id == 0) {
        return SgiveOwner::Drop;  // an older file: nobody can say whose it is
    }
    if (cur_slot < 0) {
        return SgiveOwner::Unsure;
    }
    if (entry_slot != cur_slot) {
        return SgiveOwner::KeepForOther;
    }
    if (cur_id == 0) {
        return SgiveOwner::Unsure;
    }
    return entry_id == cur_id ? SgiveOwner::Replay : SgiveOwner::Drop;  // same slot, another character
}

std::vector<std::uint32_t> GoodsEarnedFlags(std::int32_t goods) {
    // Lot pickup flags (50000000 + lot, or the lot's own), ledger bits (60009000 + index) with the awarding
    // event of the flagless lots (kItemLedgerBase + idx; tests check them against the ledger table).
    switch (goods) {
    case 4000: return {60009025, 12411800};  // Oedon Tomb Key, lot 31000
    case 4003: return {52410990};            // Cainhurst Summons
    case 4012: return {53200720};            // Lecture Theatre Key
    case 4013: return {53200810};            // Lunarium Key
    case 4103: return {52411000};            // Blood Gem Workshop Tool
    case 4104: return {52200360};            // Rune Workshop Tool
    case 4110: return {52410290};            // Saw Hunter Badge
    case 4111: return {50001900};            // Crow Hunter Badge
    case 4112: return {50001700};            // Powder Keg Hunter Badge
    case 4113: return {52100500, 52110030, 60009001, 12101800};  // Old Hunter Badge
    case 4114: return {60009036, 12411700};  // Sword Hunter Badge, lot 50000010
    case 4115: return {52400480};            // Radiant Sword Hunter Badge
    case 4116: return {50001810};            // Wheel Hunter Badge
    case 4117: return {50000205};            // Cainhurst Badge
    case 4118: return {60009038, 12301700};  // Spark Hunter Badge, lot 50800000
    case 4119: return {52420270, 52420280};  // Cosmic Eye Watcher Badge
    case 4308: return {52500910};            // Unopened Summons
    case 4310: return {60009028, 9076};      // Tonsil Stone, lot 39000
    case 4312: return {6670};                // Old Hunter Bell
    case 4907: return {52300070};            // Bloody Messenger Head Bandage
    default: return {};
    }
}

SgiveRepair SgivePoisonToRepair(const std::function<bool(std::uint32_t)>& is_set,
                                const std::function<int(std::int32_t)>& held, int ng) {
    SgiveRepair r;
    if (is_set(kSgiveRepaired)) {
        r.done = true;
        return r;
    }
    if (ng < 0) {
        r.complete = false;
        return r;
    }
    std::vector<std::int32_t> unearned;
    for (const std::int32_t goods : {4000, 4003, 4012, 4013, 4103, 4104, 4110, 4111, 4112, 4113, 4114, 4115, 4116,
                                     4117, 4118, 4119, 4308, 4310, 4312, 4907}) {
        const std::vector<std::uint32_t> src = GoodsEarnedFlags(goods);
        if (src.empty()) {
            continue;
        }
        bool earned = false;
        for (const std::uint32_t f : src) {
            earned = earned || is_set(f);
        }
        if (earned) {
            continue;
        }
        const int h = held(goods);
        if (h < 0) {
            r.complete = false;
            return r;
        }
        if (h > 0) {
            unearned.push_back(goods);
        }
    }
    // A later cycle keeps key items from the cycle before while its flags start over: not decided there.
    if (ng > 0) {
        r.kept = unearned;
        return r;
    }
    // A hunter badge without its own source is taken at once (the owner's rule); other key items only when
    // at least kSgiveMinUnearned items lack a source (one stray item has other explanations).
    const bool many = static_cast<int>(unearned.size()) >= kSgiveMinUnearned;
    for (const std::int32_t goods : unearned) {
        const bool badge = goods >= kBadgeGoodsFirst && goods < kBadgeGoodsFirst + kBadgeGateCount;
        (badge || many ? r.remove : r.kept).push_back(goods);
    }
    // Rows: every set row 12101000 + n whose badge is removed here or held nowhere.
    for (std::int32_t n = 0; n < kBadgeGateCount; ++n) {
        const std::uint32_t flag = static_cast<std::uint32_t>(kBadgeGateFirst + n);
        if (!is_set(flag)) {
            continue;
        }
        const std::int32_t goods = kBadgeGoodsFirst + n;
        const bool removed = std::find(r.remove.begin(), r.remove.end(), goods) != r.remove.end();
        const int h = removed ? 0 : held(goods);
        if (h < 0) {
            r.complete = false;
            return r;
        }
        if (h == 0) {
            r.clear.push_back(flag);
        }
    }
    return r;
}

std::size_t GuestItems::Offer(const std::vector<ItemGrant>& items) {
    std::size_t taken = 0;
    for (const ItemGrant& g : items) {
        const std::int64_t key = g.Key();
        if (key < 0 || done_.count(key)) {
            continue;
        }
        // An older host may still send consumables (party policy 0.7.5: key / unique items only).
        if ((g.source == ItemSource::Award || g.source == ItemSource::Flag || g.source == ItemSource::Full) &&
            ItemLotUnshared(g.lot)) {
            continue;
        }
        if (g.source == ItemSource::Mark) {
            if (q_.size() >= kMaxQueue) {
                continue;
            }
            q_.push_front(g); // before any replay of the same flag
            ++taken;
            continue;
        }
        if (q_.size() >= kMaxQueue || queued_.count(key)) {
            continue;
        }
        // A lot outside the tables comes once, whatever flags it is paired with.
        if (g.flag >= 0 && ItemLotFlag(g.lot) != g.flag && ItemLotForFlag(g.flag) != g.lot &&
            !adhoc_lots_.insert(g.lot).second) {
            continue;
        }
        queued_.insert(key);
        if (q_.size() >= kMaxQueue) {
            continue; // the host's next full list brings it again
        }
        q_.push_back(g);
        ++taken;
    }
    return taken;
}

std::optional<ItemGrant> GuestItems::Step(const ItemApplyState& s, double now) {
    last_hold_ = nullptr;
    const ItemApplyMode mode = ItemApplyModeNow(s);
    if (mode == ItemApplyMode::None) {
        stable_ = 0;
    } else {
        stable_ = mode == stable_mode_ ? stable_ + 1 : 1;
    }
    stable_mode_ = mode;
    if (q_.empty() || stable_ < kStableTicks) {
        return std::nullopt;
    }
    // Home: the queue in order. Summoned: the first grant not deferred to the own world (random
    // lots without the host's list, blood gems).
    auto it = q_.begin();
    if (mode == ItemApplyMode::Summoned) {
        while (it != q_.end() && it->source != ItemSource::Mark && home_only_.count(it->Key())) {
            ++it;
        }
        if (it == q_.end()) {
            return std::nullopt;
        }
    }
    // Marks only set a flag (no popup / notice): no spacing for them. A host catch-up grant given
    // while summoned is a quiet give too (no item-get dialog, one summary notice for the batch):
    // the spacing and the popup holds only exist for the dialogs.
    last_quiet_ = it->source == ItemSource::Full && mode == ItemApplyMode::Summoned;
    if (it->source != ItemSource::Mark && !last_quiet_) {
        if (now - last_apply_ < kSpacing) {
            return std::nullopt; // one popup / notice at a time
        }
        // None in a fight, while dead, in a cutscene or under the boss-defeat banner: the native
        // item-get box pushed then could not be dismissed (playtest 1 #15 / #12).
        if (const char* hold = ItemPopupHold(s)) {
            last_hold_ = hold;
            return std::nullopt;
        }
    }
    ItemGrant g = *it;
    q_.erase(it);
    if (g.source != ItemSource::Mark) {
        queued_.erase(g.Key());
    }
    last_mode_ = mode;
    return g;
}

void GuestItems::Defer(const ItemGrant& g) {
    const std::int64_t key = g.Key();
    if (key < 0 || done_.count(key) || queued_.count(key)) {
        return;
    }
    home_only_.insert(key);
    queued_.insert(key);
    q_.push_front(g); // keeps its place for the own world
}

// ---------------------------------------------------------------------------------------------
// Game part
// ---------------------------------------------------------------------------------------------
#ifndef BB_PARTY_ITEMS_NO_GAME

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using ull = unsigned long long;
using Clock = std::chrono::steady_clock;

constexpr u64 kAwardLot = 0x17ddc50;   // (MapItemMan*, int lot, char hostOnly)
constexpr u64 kGiveList = 0x17d89f0;   // (MapItemMan*, ItemList*): inventory add + popup
constexpr u64 kMapItemMan = 0x553d6e0; // slot: *(u64*)slot is the first argument of every lot call
constexpr u64 kFlagMan = 0x553b100;    // SprjEventFlagMan slot (+0x80 load mode)
constexpr u64 kIsFlag = 0x13cfc00;     // u8 (man, u32 id, u8* exists)
constexpr u64 kSetFlag = 0x13cfcc0;    // void (man, u32 id, u32 on)
constexpr u64 kWts = 0x5556678;        // WorldTransitionState / GameStateMan slot
constexpr u64 kWtsOwnWorld = 0x1592;   // 1 in the own world, 0 summoned into another

// Blood gems (docs/party/items.md 10.1).
constexpr u64 kSprjGaitem = 0x553e990;    // slot: +8 + (handle & 0xffff) * 8 = instance (+8 handle, +0xC item id)
constexpr u64 kGaitemCreate = 0x1a87590;  // u32* (u32* out, SprjGaitem*, u32 gem id): a new gem instance
constexpr u64 kGaitemRelease = 0x1a8bc00; // void (u32* handle): reference -1 (frees at 0)
constexpr u64 kItemGetList = 0x553b1c0;   // the item-get dialog's entries (+8 live list, +0x10 free nodes)
constexpr u64 kMenuState = 0x5562878;     // +0x130: the item-get dialog is up (0x16D86E0)

using AwardFn = void(BB_COOP_SYSV*)(u64, u32, u32);
using GiveListFn = void(BB_COOP_SYSV*)(u64, void*);
using GaitemCreateFn = u32*(BB_COOP_SYSV*)(u32*, u64, u32);
using GaitemReleaseFn = void(BB_COOP_SYSV*)(u32*);
using IsFlagFn = u8(BB_COOP_SYSV*)(u64, u32, u8*);
using SetFlagFn = void(BB_COOP_SYSV*)(u64, u32, u32);

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    perf::Scope z(perf::Zone::Log);
    coop::asyncio::Print(std::string("Party items: ") + line + "\n"); // never blocks the caller (party_async_io.h)
}

bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0' && !v[1];
}

double Now() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

std::atomic<bool> g_on{false};
std::atomic<bool> g_in_apply{false}; // our own 0x17DDC50 call: the hook stands aside
std::atomic<u32> g_added{0};         // items 0x17D89F0 added during our own award
std::atomic<bool> g_award_no_share{false}; // AwardLotGuestOnly (test aid): no gift_up to the host

std::mutex g_host_mu;
HostItems g_host;            // under g_host_mu
std::deque<int> g_full_due;  // under g_host_mu
std::mutex g_guest_mu;
GuestItems g_guest;          // under g_guest_mu
std::vector<ItemGrant> g_test; // BB_PARTY_ITEMS_TEST lots, offered once the tick runs
bool g_test_offered = false;
// BB_PARTY_ITEMS_HOST_TEST (live check of the summoned give): the host awards these lots through
// the game's own 0x17DDC50 (hostOnly 1, as EMEVD 2003[4]) once a cooperator has been in its world
// for 20 s, 10 s apart; BB_PARTY_ITEMS_HOST_TEST_FLAGS: flags it sets (SetEventFlag) with the first.
std::vector<std::int32_t> g_host_test;
std::vector<std::uint32_t> g_host_test_flags;
std::size_t g_host_test_next = 0;
double g_host_test_coop = 0, g_host_test_poll = 0, g_host_test_last = 0;

// D5 gifts (party_items.h GuestGiftShareable): guest reports to post, host awards to take.
struct GiftDue {
    int slot = -1;
    std::int32_t lot = 0, flag = 0;
};
std::mutex g_gift_mu;
std::deque<std::string> g_gift_up;  // guest: EVENT gift_up bodies (under g_gift_mu)
std::deque<GiftDue> g_gift_due;     // host: under g_gift_mu
GiftRateLimit g_gift_rate;          // host: under g_gift_mu
double g_gift_last = 0;             // host, main thread: the last gift award
// BB_PARTY_GIFT_TEST=<lot> (live check): a summoned guest awards the lot through the game's own
// 0x17DDC50 (hostOnly 0, as a talk gift) once its world has been steady for 20 s.
std::int32_t g_gift_test = 0;
double g_gift_test_since = 0;
bool g_gift_test_done = false;

u64 Ptr(u64 slot_off) {
    u64 p = 0;
    return SafeGet(Guest(slot_off), &p) ? p : 0;
}

int OwnWorld() {
    const u64 wts = Ptr(kWts);
    u8 v = 0xff;
    return wts && SafeGet(wts + kWtsOwnWorld, &v) ? v : -1;
}

int LoadMode(u64 man) {
    std::int32_t m = -1;
    return man && SafeGet(man + 0x80, &m) ? m : -1;
}

PartyRole Role() {
    return PartyDirector::Get().Role();
}

bool IsFlag(u64 man, std::int64_t id) {
    if (!man || id < 0) {
        return false;
    }
    u8 exists = 0;
    return reinterpret_cast<IsFlagFn>(Guest(kIsFlag))(man, static_cast<u32>(id), &exists) != 0;
}

void SetFlag(u64 man, std::int64_t id, bool on = true) {
    if (man && id >= 0) {
        const progress::FlagCause cause("party items");
        reinterpret_cast<SetFlagFn>(Guest(kSetFlag))(man, static_cast<u32>(id), on ? 1 : 0);
    }
}

void SeedHostSeq() {
    // Wall-clock seeded like travel: a restarted host keeps counting upward.
    const u64 seed = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    std::lock_guard<std::mutex> lk(g_host_mu);
    g_host.SeedSeq(seed);
}

// ---- Blood gems ----

/// A gem's gaitem instance (SprjGaitem +8 + index * 8; the handle at +8 must match), else 0.
u64 GemInstance(u32 handle) {
    const u64 gi = Ptr(kSprjGaitem);
    if (!gi || !handle || !(handle & 0x800000u) || (handle & 0xffffu) == 0xffffu) {
        return 0;
    }
    u64 inst = 0;
    u32 h = 0, type = 0;
    if (!SafeGet(gi + 8 + 8ull * (handle & 0xffffu), &inst) || !inst || !SafeGet(inst + 8, &h) || h != handle ||
        !SafeGet(inst + 0xc, &type) || (type >> 28) != 8) {
        return 0;
    }
    return inst;
}

/// Host: the gem behind a give-list handle (id + instance data), as 0x1A87E20 wrote it.
bool ReadGem(u32 handle, GrantItem* it) {
    const u64 inst = GemInstance(handle);
    u32 type = 0;
    if (!inst || !SafeGet(inst + 0xc, &type) || !SafeRead(inst + 0x18, it->gem, sizeof it->gem)) {
        return false;
    }
    it->category = 8;
    it->id = static_cast<std::int32_t>(type & 0x0fffffffu);
    it->count = 1;
    it->has_gem = true;
    return GemDataPlausible(it->gem);
}

std::string GemText(const GrantItem& it) {
    char b[160];
    std::snprintf(b, sizeof b, "gem %d (flags %d, rank %d, effects %d %d %d %d %d %d)", it.id, it.gem[0], it.gem[1],
                  it.gem[2], it.gem[3], it.gem[4], it.gem[5], it.gem[6], it.gem[7]);
    return b;
}

/// Guest: the host's gem recreated - a new gaitem instance with the host's id and instance data,
/// handed to the lot award's own give list 0x17D89F0 (inventory, else storage / the ground, and
/// the native item-get dialog with its sound, as the host's award showed it). The gem's
/// GemGenParam row and every effect's GemeffectParam row must exist first.
bool GiveGem(const GrantItem& it, std::string* why) {
    if (!it.has_gem || it.category != 8 || it.count != 1) {
        *why = "no gem data";
        return false;
    }
    if (!GameParamRow("GemGenParam", "GEM_GEN_PARAM_ST", static_cast<u32>(it.id))) {
        *why = "no GemGenParam row " + std::to_string(it.id);
        return false;
    }
    for (std::size_t k = 2; k < kGemFields; ++k) {
        if (it.gem[k] != -1 && !GameParamRow("GemeffectParam", "GEMEFFECT_PARAM_ST", static_cast<u32>(it.gem[k]))) {
            *why = "no GemeffectParam row " + std::to_string(it.gem[k]);
            return false;
        }
    }
    static const u8 kCreate[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                 0x53, 0x48, 0x83, 0xec, 0x28, 0x41, 0x89, 0xd6, 0x48, 0x89, 0xf3};
    static const u8 kRelease[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb, 0x8b, 0x33};
    static const u8 kGive[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                               0x53, 0x48, 0x81, 0xec, 0xb8, 0x01, 0x00, 0x00, 0x49, 0x89, 0xf7};
    if (!MatchesOriginal(kGaitemCreate, kCreate, sizeof kCreate) ||
        !MatchesOriginal(kGaitemRelease, kRelease, sizeof kRelease) || !MatchesOriginal(kGiveList, kGive, sizeof kGive)) {
        *why = "gaitem / give-list code differs from 1.09";
        return false;
    }
    const u64 gi = Ptr(kSprjGaitem), mim = Ptr(kMapItemMan);
    if (!gi || !mim) {
        *why = "no SprjGaitem / MapItemMan";
        return false;
    }
    u32 handle = 0;
    reinterpret_cast<GaitemCreateFn>(Guest(kGaitemCreate))(&handle, gi, static_cast<u32>(it.id));
    const u64 inst = GemInstance(handle);
    u32 type = 0;
    if (!inst || !SafeGet(inst + 0xc, &type) || type != (0x80000000u | static_cast<u32>(it.id))) {
        if (handle) {
            reinterpret_cast<GaitemReleaseFn>(Guest(kGaitemRelease))(&handle);
        }
        *why = "no gaitem instance (table full?)";
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(inst + 0x18), it.gem, sizeof it.gem);
    // ItemList {u32 n; n x {u32 gaitem, u32 id | category << 28, u32 count, u32 -1}} like 0x17DDC50's.
    u32 list[16] = {};
    list[0] = 1;
    list[1] = handle;
    list[2] = type;
    list[3] = 1;
    list[4] = 0xffffffffu;
    g_in_apply = true;
    reinterpret_cast<GiveListFn>(Guest(kGiveList))(mim, list);
    g_in_apply = false;
    reinterpret_cast<GaitemReleaseFn>(Guest(kGaitemRelease))(&handle); // the inventory keeps its own reference
    return true;
}

/// Room left in the own storage for goods `id` (0x14D9A80, the give list's own check; -1 unknown).
int StorageRoom(u32 id) {
    const u64 player = [] {
        u64 wcm = Ptr(0x553e878), p = 0;
        return wcm && SafeGet(wcm + 0x60, &p) ? p : 0;
    }();
    u64 vt = 0, fn = 0, data = 0, st = 0;
    if (!player || !SafeGet(player, &vt) || !SafeGet(vt + 0x1c8, &fn) || !fn) {
        return -1;
    }
    data = reinterpret_cast<u64(BB_COOP_SYSV*)(u64)>(fn)(player);
    if (!data || !SafeGet(data + 0x5b0, &st) || !st) {
        return -1;
    }
    u32 item = 0x40000000u | (id & 0xfffffffu);
    return reinterpret_cast<int(BB_COOP_SYSV*)(u64, u32*, int)>(Guest(0x14d9a80))(st, &item, 9999);
}

/// Guest, summoned: goods that would pass their inventory maximum go through the lot award's own
/// give list 0x17D89F0, which fills the inventory, puts the rest into the storage (else on the
/// ground, with the game's "inventory or storage max" box) and queues the item-get dialog.
/// GiveItemDirect (0x131CB70) only adds to the inventory: past maxNum the rest was lost.
bool GiveGoodsViaList(u32 id, int count) {
    static const u8 kGive[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                               0x53, 0x48, 0x81, 0xec, 0xb8, 0x01, 0x00, 0x00, 0x49, 0x89, 0xf7};
    const u64 mim = Ptr(kMapItemMan);
    if (!mim || count <= 0 || !MatchesOriginal(kGiveList, kGive, sizeof kGive)) {
        return false;
    }
    // ItemList as in GiveGem; a goods "gaitem" is its id with the goods tag (0x1A89070: bit 31 and
    // type 3 in bits 28-30 decode to category 4).
    u32 list[16] = {};
    list[0] = 1;
    list[1] = 0xB0000000u | (id & 0xfffffffu);
    list[2] = 0x40000000u | (id & 0xfffffffu);
    list[3] = static_cast<u32>(count);
    list[4] = 0xffffffffu;
    g_in_apply = true;
    reinterpret_cast<GiveListFn>(Guest(kGiveList))(mim, list);
    g_in_apply = false;
    return true;
}

/// Host: the last give lists with a blood gem that came from a pickup (0x17D8570 -> 0x17D89F0,
/// not through 0x17DDC50). The treasure's flag reaches OnHostFlagSet a little later (C2's scan),
/// and the gem the host really got rides along. Main thread only.
struct RecentGive {
    double at = 0;
    std::vector<GrantItem> items;
};
std::deque<RecentGive> g_recent_gem_gives;

// ---- The award hook (main thread: EMEVD, Lua) ----

AwardFn g_award_original = nullptr; // the trampoline (ReplacePrologue)
// G1: the host's Chalice Dungeon awards go to the guests in the dungeon (BB_PARTY_CHALICE_AWARDS=0: off).
const bool g_chalice_awards = !EnvOff("BB_PARTY_CHALICE_AWARDS");
bool g_capturing = false;           // main thread: the host's own award runs, GiveListEntry records
std::vector<GrantItem> g_captured;  // its items (category, id, count)
bool g_captured_bad = false;        // an item that could not be read (the guest resolves the lot itself)
bool g_captured_nothing = false;    // the roll made the game's empty record (item -1): no item exists (runenow077)

/// Host side of an award, before the game runs it: the grant to offer once the call returned
/// (its items captured), else nothing. Guest side: the mark for a lot received while summoned.
std::optional<ItemGrant> AwardEntry(u64 lot_arg, u64 host_only_arg) {
    perf::Scope z(perf::Zone::HookAward);
    const std::int32_t lot = static_cast<std::int32_t>(static_cast<u32>(lot_arg));
    const bool host_only = static_cast<u8>(host_only_arg) != 0;
    const PartyRole role = Role();
    const int own = OwnWorld();
    const std::int32_t flag = ItemLotFlag(lot);
    npctalk::NoteAward(lot);  // an NPC's gift during a conversation: shown to the party (D8)
    Log("award lot %d (hostOnly %d, flag %d) in %s world, role %s", lot, host_only ? 1 : 0, flag,
        own == 1 ? "the own" : own == 0 ? "another's" : "an unknown",
        role == PartyRole::Host ? "host" : role == PartyRole::Guest ? "guest" : "none");
    if (own == 1 && host_only && flag < 0 && !ItemLotDenied(lot, flag)) {
        // A ledger lot this player earns in its own world (any role, solo too): its ledger bit, so
        // a host's later replay of the same lot gives nothing twice.
        const std::int32_t idx = ItemLedgerIndex(lot);
        if (idx >= 0) {
            SetFlag(Ptr(kFlagMan), std::int64_t(kItemLedgerBase) + idx);
            Log("own award of ledger lot %d: ledger flag %d set", lot, int(kItemLedgerBase) + idx);
        }
    }
    if (role == PartyRole::Host && own == 1 && host_only && ChaliceAwardLot(lot) && g_chalice_awards) {
        // G1: a Chalice Dungeon award (a layer boss's ritual materials, ...): what the host's roll
        // gives goes to the guests in the dungeon now (AwardReplace fills the items).
        ItemGrant c;
        c.lot = lot;
        c.source = ItemSource::Chalice;
        return c;
    }
    if (role == PartyRole::Host && own == 1 && host_only && BossDropArea(lot) >= 0 &&
        BbSettings::Get().party_boss_drops.load() && !ClassifyAward(lot, host_only)) {
        // 0.7.6 (bossdrops): a world boss's item drop the game keeps for the host and the key / unique
        // rules do not share (Rom's Kin Coldblood): what the host's roll gives goes to the guests in the
        // boss's map area now (AwardReplace fills the items). Key / unique lots take the paths above.
        if (flag > 0) {
            std::lock_guard<std::mutex> lk(g_host_mu);
            g_host.Capture(flag, lot);
        }
        ItemGrant b;
        b.lot = lot;
        b.source = ItemSource::BossDrop;
        return b;
    }
    if (role == PartyRole::Host && own == 1) {
        const std::optional<ItemGrant> g = ClassifyAward(lot, host_only);
        {
            std::lock_guard<std::mutex> lk(g_host_mu);
            if (flag > 0) {
                g_host.Capture(flag, lot);
            }
        }
        if (!g && host_only && flag < 0 && !ItemLotDenied(lot, flag)) {
            Log("host award lot %d: flagless and not in the ledger table; not replayed", lot);
        }
        return g;
    } else if (role == PartyRole::Guest && own == 0 && host_only && MessengerGiftLot(lot) && flag > 0) {
        // A per-player Doll / Messenger gift (Old Hunter Bell ...) the guest's talk asked for in
        // the host's Dream: the game refuses hostOnly lots on a client and the lot is never
        // replayed (per player), so the guest gives it to itself (own inventory and own flag).
        ItemGrant m;
        m.lot = lot;
        m.flag = flag;
        m.source = ItemSource::Own;
        std::size_t taken = 0;
        {
            std::lock_guard<std::mutex> lk(g_guest_mu);
            taken = g_guest.Offer({m});
        }
        Log("guest's own gift lot %d (flag %d) refused by the game in the host's world: %s", lot, flag,
            taken ? "given to the guest itself (own inventory, own flag)" : "already queued or given");
    } else if (role == PartyRole::Guest && own == 0 && !host_only && flag > 0 && !ItemLotDenied(lot, flag)) {
        // We got it in the host's world; its flag went to the overlay only. Set it at home so the
        // host's replay of the same lot gives nothing twice.
        ItemGrant m;
        m.lot = lot;
        m.flag = flag;
        m.source = ItemSource::Mark;
        {
            std::lock_guard<std::mutex> lk(g_guest_mu);
            g_guest.Offer({m});
        }
        Log("guest received lot %d while summoned: flag %d goes into the own save (pool)", lot, flag);
        if (g_award_no_share.load()) {
            Log("gift: lot %d is a guest-only test award: not reported to the host", lot);
        } else if (GuestGiftShareable(lot, flag)) {
            // D5: an NPC's gift in the host's world (shared quest choices): the host takes it too.
            std::lock_guard<std::mutex> lk(g_gift_mu);
            if (g_gift_up.size() < 64) {
                g_gift_up.push_back(GiftUpToJsonText(lot, flag));
                Log("gift: lot %d (flag %d) reported to the host (the NPC moved on in its world too)", lot, flag);
            }
        }
    }
    return std::nullopt;
}

BB_COOP_SYSV void AwardReplace(u64 mim, u64 lot_arg, u64 host_only_arg) {
    if (!g_on.load() || g_in_apply.load()) {
        g_award_original(mim, static_cast<u32>(lot_arg), static_cast<u32>(host_only_arg));
        return;
    }
    std::optional<ItemGrant> g = AwardEntry(lot_arg, host_only_arg);
    g_captured.clear();
    g_captured_bad = false;
    g_captured_nothing = false;
    g_capturing = g.has_value();
    g_award_original(mim, static_cast<u32>(lot_arg), static_cast<u32>(host_only_arg));
    g_capturing = false;
    if (!g) {
        return;
    }
    if (g->source == ItemSource::Chalice) {
        static std::uint64_t n = 0;
        const std::optional<ItemGrant> c =
            g_captured_bad ? std::nullopt : MakeChaliceGrant(g->lot, g_captured, ++n);
        if (!c) {
            Log("host chalice award lot %d: %s; not sent", g->lot,
                g_captured_bad ? "an item could not be read" : g_captured.empty() ? "it gave nothing" : "too many items");
            return;
        }
        g = c;
    } else if (g->source == ItemSource::BossDrop) {
        static std::uint64_t n = 0;
        const std::optional<ItemGrant> c = g_captured_bad ? std::nullopt : MakeBossDropGrant(g->lot, g_captured, ++n);
        if (!c) {
            Log("host boss drop lot %d: %s; not sent", g->lot,
                g_captured_bad ? "an item could not be read" : g_captured.empty() ? "it gave nothing" : "too many items");
            return;
        }
        g = c;
    }
    // What the host's roll really gave: guests replay exactly that while summoned.
    if (!g_captured_bad && g_captured.size() <= kMaxGrantItems) {
        g->items = g_captured;
    }
    bool queued;
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        queued = g_host.Offer(*g);
    }
    std::string list;
    for (const GrantItem& it : g->items) {
        list += (list.empty() ? "" : ", ") + std::to_string(it.category) + ":" + std::to_string(it.id) + "x" +
                std::to_string(it.count);
    }
    for (const GrantItem& it : g->items) {
        if (it.has_gem) {
            list += " [" + GemText(it) + "]";
        }
    }
    Log("host award %s %s; items [%s]%s", DescribeItem(*g).c_str(), queued ? "queued for the party" : "already sent",
        list.c_str(), g_captured_bad ? " (an item could not be read: guests resolve the lot themselves)" : "");
    if (g_captured_nothing && g_captured.empty() && !g_captured_bad) {
        Log("host award lot %d: the game's roll made no item (an empty record, e.g. a gem id with no GemGenParam row): "
            "nothing exists to share, no notice",
            g->lot);
        return;
    }
    if (queued) {
        ShareNotice(*g, "host award");
    }
}

// Our own award's inventory adds (evidence for the apply log): ItemList {u32 n, then n x
// {u32 gaitem, u32 itemId | category << 28, u32 count, u32 -1}} from +4 (0x17D8A42..0x17D8A8A).
BB_COOP_SYSV void GiveListEntry(u64, u64 list, u64, u64, u64, u64) {
    perf::Scope z(perf::Zone::HookGiveList);
    // Host side: the award's list (g_capturing), or a pickup's list (it may hold a blood gem).
    const bool host_pickup = !g_in_apply.load() && !g_capturing && g_on.load() && Role() == PartyRole::Host &&
                             OwnWorld() == 1;
    if (!list || (!g_in_apply.load() && !g_capturing && !host_pickup)) {
        return;
    }
    u32 n = 0;
    if (!SafeGet(list, &n)) {
        return;
    }
    if (g_capturing || host_pickup) {
        std::vector<GrantItem> items;
        bool bad = false, gem = false;
        for (u32 i = 0; i < n && i < 8; ++i) {
            u32 e[4] = {};
            if (!SafeRead(list + 4 + 16ull * i, e, sizeof e)) {
                bad = true;
                continue;
            }
            GrantItem it;
            it.category = e[1] >> 28;
            it.id = static_cast<std::int32_t>(e[1] & 0x0fffffffu);
            it.count = static_cast<std::int32_t>(e[2]);
            if (e[0] == 0 && e[1] == 0xffffffffu) {
                // The game's empty record: the lot rolled nothing (a Caryll rune lot whose gem id has no GemGenParam
                // row, e.g. 227000 of lot 2700620: no member can ever get an item from it).
                if (g_capturing) {
                    g_captured_nothing = true;
                }
                continue;
            }
            if (it.category == 8) {
                if (it.count != 1 || !ReadGem(e[0], &it)) {
                    Log("give list entry %u/%u: gem %u (gaitem 0x%x, count %u) could not be read: instance data %s", i + 1, n,
                        e[1] & 0x0fffffffu, e[0], e[2],
                        it.id > 0 ? GemText(it).c_str() : "unreadable");
                    bad = true;
                    continue;
                }
                gem = true;
            }
            if (!GrantItemSupported(it) || it.id <= 0 || it.count < 1 || it.count > 255) {
                Log("give list entry %u/%u: category %u id %d count %d (gaitem 0x%x) not shared (%s)", i + 1, n, it.category,
                    it.id, it.count, e[0], it.has_gem ? GemText(it).c_str() : "no gem data");
                bad = true;
                continue;
            }
            items.push_back(it);
        }
        if (g_capturing) {
            g_captured.insert(g_captured.end(), items.begin(), items.end());
            g_captured_bad = g_captured_bad || bad;
        } else if (gem && !bad) {
            g_recent_gem_gives.push_back({Now(), items});
            while (g_recent_gem_gives.size() > 8) {
                g_recent_gem_gives.pop_front();
            }
            for (const GrantItem& it : items) {
                if (it.has_gem) {
                    Log("host picked up %s; kept for its treasure flag's report", GemText(it).c_str());
                }
            }
        }
        return;
    }
    g_added.fetch_add(n);
    for (u32 i = 0; i < n && i < 8; ++i) {
        u32 e[4] = {};
        if (SafeRead(list + 4 + 16ull * i, e, sizeof e)) {
            Log("  inventory add %u/%u: item %u category %u count %u (gaitem 0x%x)", i + 1, n, e[1] & 0x0fffffffu,
                e[1] >> 28, e[2], e[0]);
        }
    }
}

// ---- Parity patches ----

bool WriteImage(u64 off, const u8* bytes, std::size_t n) {
    unsigned char* at = Image() + off;
#ifdef _WIN32
    DWORD old = 0;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(at, bytes, n);
    DWORD unused = 0;
    VirtualProtect(at, n, old, &unused);
    FlushInstructionCache(GetCurrentProcess(), at, n);
#else
    std::memcpy(at, bytes, n);
#endif
    return true;
}

void Patch(const char* name, u64 off, std::initializer_list<u8> original, std::initializer_list<u8> patched) {
    if (Matches(off, patched.begin(), patched.size())) {
        Log("%s (+0x%llx): already applied", name, ull(off));
    } else if (!Matches(off, original.begin(), original.size())) {
        Log("%s (+0x%llx): MISMATCH, not applied", name, ull(off));
    } else if (!WriteImage(off, patched.begin(), patched.size())) {
        Log("%s (+0x%llx): could not be written", name, ull(off));
    } else {
        Log("%s (+0x%llx): applied", name, ull(off));
    }
}

void ParityPatches() {
    // docs/party/items.md 5, "optional parity patches"; each verified against the 1.09 bytes.
    // 0x18FC93D test cl, cl (isClient); 0x18FC93F jne +6 -> the 0.5 kill-echo multiplier.
    Patch("full kill echoes for clients", 0x18fc93f, {0x75, 0x06}, {0x90, 0x90});
    // 0x133557F cmp eax, 8 (chr type | 8 == 8: host or own-world grey); je skips the halving.
    Patch("full boss clear bonus for phantoms", 0x1335582, {0x74, 0x12}, {0xeb, 0x12});
    // 0x17DD18E setne cl; 0x17DD191 or cl, al -> xor cl, cl: the isGuestDrop filter's only input
    // ([rbp-0x208], read once at 0x17DD75D) is 0, so a client keeps the whole enemy drop.
    Patch("full enemy drops for clients", 0x17dd191, {0x08, 0xc1}, {0x30, 0xc9});
}

std::vector<long> EnvList(const char* name) {
    std::vector<long> out;
    const char* v = std::getenv(name);
    if (!v) {
        return out;
    }
    std::string s(v);
    std::size_t at = 0;
    while (at < s.size()) {
        std::size_t end = s.find(',', at);
        if (end == std::string::npos) {
            end = s.size();
        }
        const long x = std::strtol(s.substr(at, end - at).c_str(), nullptr, 10);
        if (x > 0) {
            out.push_back(x);
        }
        at = end + 1;
    }
    return out;
}

void ParseHostTest() {
    for (const long lot : EnvList("BB_PARTY_GIFT_TEST")) {
        g_gift_test = static_cast<std::int32_t>(lot);
        Log("BB_PARTY_GIFT_TEST: a summoned guest awards lot %d itself (shareable %d)", g_gift_test,
            GuestGiftShareable(g_gift_test, ItemLotFlag(g_gift_test)) ? 1 : 0);
    }
    for (const long lot : EnvList("BB_PARTY_ITEMS_HOST_TEST")) {
        g_host_test.push_back(static_cast<std::int32_t>(lot));
    }
    for (const long f : EnvList("BB_PARTY_ITEMS_HOST_TEST_FLAGS")) {
        g_host_test_flags.push_back(static_cast<std::uint32_t>(f));
    }
    if (!g_host_test.empty()) {
        Log("BB_PARTY_ITEMS_HOST_TEST: %zu lot(s), %zu flag(s): the host awards them once a cooperator is in",
            g_host_test.size(), g_host_test_flags.size());
    }
}

/// The host fixture (main thread): the game's award with hostOnly 1, through our hook.
void ForgetOwedLot(std::int32_t lot);
std::vector<LotSlotState> LotSlots(std::int32_t lot);

void HostTestTick(double now) {
    if (g_host_test_next >= g_host_test.size() || Role() != PartyRole::Host || OwnWorld() != 1) {
        return;
    }
    const GameSnapshot quick = ReadGameState(false);
    if (!quick.world_up || quick.loading) {
        return;
    }
    if (!g_host_test_coop) {
        if (now - g_host_test_poll < 1.0) {
            return;
        }
        g_host_test_poll = now;
        if (ReadGameState(true).cooperators > 0) {
            g_host_test_coop = now;
            Log("host test: a cooperator is in the world; awards in 20 s");
        }
        return;
    }
    if (now - g_host_test_coop < 20.0 || now - g_host_test_last < 10.0) {
        return;
    }
    {   // 0.7.6: a world boss's lot is awarded where the boss is (the guests present at the kill: the same map area)
        const int area = BossDropArea(g_host_test[g_host_test_next]);
        if (area >= 0 && (quick.map_id == 0xffffffffu || static_cast<int>(quick.map_id >> 24) != area)) {
            return;
        }
    }
    g_host_test_last = now;
    const std::int32_t lot = g_host_test[g_host_test_next++];
    const u64 mim = Ptr(kMapItemMan);
    if (!mim) {
        return;
    }
    {
        // 0.6.1: a test lot whose key item the host holds opened the game's blocking box.
        std::string why;
        const LotFit fit = ClassifyLotFit(LotSlots(lot), &why);
        if (fit != LotFit::Give) {
            Log("host test: lot %d not awarded: %s (%s)", lot, LotFitName(fit), why.c_str());
            return;
        }
    }
    Log("host test: awarding lot %d through 0x17DDC50 (hostOnly 1)", lot);
    reinterpret_cast<AwardFn>(Guest(kAwardLot))(mim, static_cast<u32>(lot), 1);
    if (g_host_test_next == 1) {
        const u64 man = Ptr(kFlagMan);
        for (const std::uint32_t f : g_host_test_flags) {
            const bool was = IsFlag(man, f);
            SetFlag(man, f);
            Log("host test: flag %u %d -> %d", f, was ? 1 : 0, IsFlag(man, f) ? 1 : 0);
        }
    }
}

// ---- 0.7.7 (CL-806406): "<item>: shared with the party" ----
itemnotice::Queue g_notices;  // main thread
std::mutex g_notice_mu;       // producers: the host award hook (game thread), the apply paths (main thread)

std::string Utf8Text(const char16_t* t) {
    std::string out;
    for (; t && *t && out.size() < 120; ++t) {
        std::uint32_t c = *t;
        if (c >= 0xD800 && c <= 0xDBFF && t[1] >= 0xDC00 && t[1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (t[1] - 0xDC00);
            ++t;
        }
        if (c == '\n' || c == '\r') {
            c = ' ';
        }
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

/// The game's name of an item (its own language); empty when the lookup is not up yet or there is none (runes).
std::string GameItemName(std::uint32_t category, std::int32_t id) {
    std::uint32_t fmg = 0;
    switch (category) {
    case 0: fmg = 11; break;
    case 1: fmg = 12; break;
    case 2: fmg = 13; break;
    case 4: fmg = 10; break;
    default: return {};
    }
    return id >= 0 ? Utf8Text(BbGameMenu::GameText(fmg, std::uint32_t(id))) : std::string();
}

std::vector<LotRowData> LotChain(std::int32_t lot);

/// The item the notice names: the first shareable one (a weapon, a rune, a key / unique goods item) of the grant's
/// list, else of the lot's rows; the first item when none qualifies.
std::string ShareNoticeItem(const ItemGrant& g) {
    std::vector<GrantItem> items = g.items;
    if (items.empty()) {
        for (const LotRowData& r : LotChain(g.lot)) {
            for (int i = 0; i < 8; ++i) {
                if (SlotWeighted(r, i) && SlotHasItem(r, i)) {
                    items.push_back({r.category[i], r.id[i], 1});
                }
            }
        }
    }
    const std::vector<LotSlotState> slots = LotSlots(g.lot);
    auto goods_shareable = [&](std::int32_t id) {
        for (const LotSlotState& sl : slots) {
            if (sl.category == 4 && sl.id == id) {
                return sl.goods_type == 1 || sl.max_num == 1;
            }
        }
        return false;
    };
    const GrantItem* pick = nullptr;
    for (const GrantItem& it : items) {
        if (it.category == 0 || it.category == 8 || (it.category == 4 && goods_shareable(it.id))) {
            pick = &it;
            break;
        }
    }
    if (!pick && !items.empty()) {
        pick = &items.front();
    }
    if (!pick) {
        return {};
    }
    std::string n = GameItemName(pick->category, pick->id);
    if (n.empty() && pick->category == 8) {
        n = "Caryll rune";
    }
    return n;
}

}  // namespace

/// A shared item was given on this game (or queued for the party by the host): queue the notice. `how`: log word.
/// Nothing for catch-up lists, marks, tests, per-player lots (no share path) or when the player switched it off.
void ShareNotice(const ItemGrant& g, const char* how) {
    if (g.source == ItemSource::Full || g.source == ItemSource::Mark || g.source == ItemSource::Test ||
        g.source == ItemSource::Chalice || ItemLotSharePath(g.lot) == SharePath::None || !party::status::Enabled()) {
        return;
    }
    if (!BbSettings::Get().party_item_notices.load()) {
        return;
    }
    const std::string text = itemnotice::Text(ShareNoticeItem(g));
    std::lock_guard<std::mutex> lk(g_notice_mu);
    const bool queued = g_notices.Push(text, Now());
    Log("notice \"%s\" (lot %d, %s): %s", text.c_str(), g.lot, how, queued ? "queued" : "dropped (same item just shown, or the queue is full)");
}

std::string PartyItemName(std::uint32_t category, std::int32_t id) {
    return GameItemName(category, id);
}

void PartyDropNotice(const std::string& text) {
    if (!party::status::Enabled() || !BbSettings::Get().party_item_notices.load()) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_notice_mu);
    const bool queued = g_notices.Push(text, Now());
    Log("notice \"%s\" (far shared drop): %s", text.c_str(), queued ? "queued" : "dropped (same text just shown, or the queue is full)");
}

namespace {

void NoticeTick() {
    std::string text;
    {
        std::lock_guard<std::mutex> lk(g_notice_mu);
        if (!g_notices.Pop(Now(), &text)) {
            return;
        }
    }
    party::status::SetLastEvent(text);  // the party notice corner (party_toasts); also the Party members page's "Last event"
    Log("notice shown: %s", text.c_str());
}

/// 0.7.7 (NW-4ae26be): the host's fallback for a kill award that needs entity 10000's blow (party_kill_award_rules.h).
/// Main thread, host, own world, a cooperator in the map. The entity's HP is read like the enemy census does
/// (chr +0x3B0 -> +0x20 -> +0xF8); the lot's flag is the "already awarded" bit, so the game's own event and this
/// fallback can never both give it. The award goes through the game's function with hostOnly 1 and so through our
/// award hook (ClassifyAward -> the party), exactly like the event's own award.
void KillAwardTick(double now) {
    static double next = 0;
    static std::map<std::int32_t, killaward::State> st;
    static std::uint32_t last_map = 0;
    static bool off = EnvOff("BB_PARTY_KILL_AWARD");
    if (off || Role() != PartyRole::Host || OwnWorld() != 1 || now < next) {
        return;
    }
    next = now + killaward::kPollS;
    const GameSnapshot quick = ReadGameState(false);
    if (!quick.world_up || quick.loading || quick.map_id == 0xffffffffu) {
        st.clear();
        return;
    }
    if (quick.map_id != last_map) {
        last_map = quick.map_id;
        st.clear();  // a new map load: a new world, every entity is a new chr
    }
    const u64 man = Ptr(kFlagMan);
    const u64 mim = Ptr(kMapItemMan);
    if (!man || !mim) {
        return;
    }
    for (const KillAwardRow& r : KillAwardRows()) {
        if (static_cast<int>(quick.map_id >> 24) != r.area) {
            continue;
        }
        killaward::State& s = st[r.lot];
        if (s.done) {
            continue;
        }
        const std::int32_t flag = ItemLotFlag(r.lot);
        const bool set = flag > 0 && IsFlag(man, flag);
        bool seen = false, alive = true;
        if (!set) {
            // the game's own chr-by-entity lookup (0x13C97A0, as the test kills and the boss clear bonus use it)
            const u64 chr = reinterpret_cast<u64(BB_COOP_SYSV*)(std::int32_t, u64, u64)>(Guest(0x13c97a0))(
                static_cast<std::int32_t>(r.entity), 0, 0);
            u64 a = 0, dm = 0;
            std::int32_t hp = -1;
            if (chr && SafeGet(chr + 0x3b0, &a) && a && SafeGet(a + 0x20, &dm) && dm) {
                SafeGet(dm + 0xf8, &hp);
            }
            seen = hp >= 0;
            alive = hp != 0;
        }
        if (!killaward::Due(&s, now, seen, alive, set)) {
            continue;
        }
        s.done = true;
        if (ReadGameState(true).cooperators <= 0) {
            Log("kill award: entity %u died alone, the game's event %u decides (lot %d flag %d unset)", r.entity, r.event,
                r.lot, flag);
            continue;
        }
        Log("kill award: entity %u is dead and lot %d (flag %d) was not awarded by event %u (the last blow was a guest's): "
            "the host awards it through 0x17DDC50 (hostOnly 1)", r.entity, r.lot, flag, r.event);
        reinterpret_cast<AwardFn>(Guest(kAwardLot))(mim, static_cast<u32>(r.lot), 1);
        if (r.spot_flag && !IsFlag(man, r.spot_flag)) {
            SetFlag(man, r.spot_flag);
        }
        Log("kill award: lot %d flag %d now %d", r.lot, flag, IsFlag(man, flag) ? 1 : 0);
    }
}

/// Held counts of a lot's deterministic items (evidence in the logs).
/// Defined below (the ItemLotParam readers).
std::vector<LotRowData> LotChain(std::int32_t lot);

std::string HeldSummary(std::int32_t lot) {
    std::string out;
    for (const LotRowData& r : LotChain(lot)) {
        for (int i = 0; i < 8; ++i) {
            if ((r.base[i] > 0 || r.cumulate[i] > 0) && r.id[i] > 0 && GrantCategorySupported(r.category[i])) {
                char b[96];
                std::snprintf(b, sizeof b, "%s%u:%d held %d", out.empty() ? "" : ", ", r.category[i], r.id[i],
                              ItemCount(r.category[i] << 28, static_cast<u32>(r.id[i])));
                out += b;
            }
        }
    }
    return out.empty() ? "-" : out;
}

void ParseTestLots() {
    const char* v = std::getenv("BB_PARTY_ITEMS_TEST");
    if (!v || !*v) {
        return;
    }
    std::string s(v);
    std::size_t at = 0;
    while (at < s.size()) {
        std::size_t end = s.find(',', at);
        if (end == std::string::npos) {
            end = s.size();
        }
        const long lot = std::strtol(s.substr(at, end - at).c_str(), nullptr, 10);
        at = end + 1;
        if (lot <= 0) {
            continue;
        }
        ItemGrant g;
        g.lot = static_cast<std::int32_t>(lot);
        g.flag = ItemLotFlag(g.lot);
        g.ledger = g.flag < 0 ? ItemLedgerIndex(g.lot) : kItemNone;
        g.source = ItemSource::Test;
        g.seq = g_test.size() + 1;
        if (g.DoneFlag() < 0) {
            Log("BB_PARTY_ITEMS_TEST: lot %d has neither a flag nor a ledger index; skipped", g.lot);
            continue;
        }
        g_test.push_back(g);
        Log("BB_PARTY_ITEMS_TEST: will apply %s in the own world", DescribeItem(g).c_str());
    }
}

void FillGoodsLimits(LotSlotState* s) {
    if (s->category != 4) {
        return;
    }
    s->owned = ItemCount(0x40000000u, static_cast<u32>(s->id));
    // maxRepositoryNum is at +0x48 (0x15813A0); it was read at +0x49 until 2026-10-21, which gave
    // 0 storage for most goods, so a give that the storage would have taken counted as "Full".
    u8 row[kGoodsRowBytes];
    if (const u64 goods = GameParamRow("EquipParamGoods", "EQUIP_PARAM_GOODS_ST", static_cast<u32>(s->id));
        goods && SafeRead(goods, row, sizeof row)) {
        GoodsLimitsFromRow(row, s);
    }
}

// ItemLotParam (ITEMLOT_PARAM_ST, 1.09 paramdef): lotItemId01..08 s32 +0x00, lotItemCategory01..08
// u32 +0x20, lotItemBasePoint01..08 u16 +0x40, lotItemNum01..08 u8 +0x8A. EquipParamGoods
// (EQUIP_PARAM_GOODS_ST): maxNum s16 +0x36, goodsType u8 +0x3A, maxRepositoryNum s16 +0x48.
/// A flagless lot whose awarding event is a boss defeat (its ledger row's done flag is a
/// boss_defeated flag): boss rewards (keys, badges) are never handed to an NPC or used up.
bool BossLedgerLot(const ItemGrant& g) {
    if (g.flag >= 0 || g.ledger < 0) {
        return false;
    }
    const std::int32_t f = ItemLedgerDoneFlag(g.ledger);
    return f > 0 && progress::CategoryOf(static_cast<u32>(f)) == progress::Category::boss_defeated;
}

std::vector<LotSlotState> LotSlots(std::int32_t lot) {
    std::vector<LotSlotState> out;
    const u64 row = GameParamRow("ItemLotParam", "ITEMLOT_PARAM_ST", static_cast<u32>(lot));
    if (!row) {
        return out;
    }
    for (int i = 0; i < 8; ++i) {
        LotSlotState s;
        std::uint16_t base = 0;
        u8 num = 0;
        if (!SafeGet(row + 4u * i, &s.id) || !SafeGet(row + 0x20 + 4u * i, &s.category) ||
            !SafeGet(row + 0x40 + 2u * i, &base) || !SafeGet(row + 0x8a + i, &num)) {
            continue;
        }
        if (s.id <= 0 || s.category == 0xffffffffu || base == 0) {
            continue;
        }
        s.num = num;
        FillGoodsLimits(&s);
        out.push_back(s);
    }
    return out;
}

/// ItemLotParam rows lot, lot + 1, ... while they exist (the roll's chain, 0x17CEAF0).
std::vector<LotRowData> LotChain(std::int32_t lot) {
    std::vector<LotRowData> out;
    for (std::size_t k = 0; k < kMaxLotChain; ++k) {
        const u64 row = GameParamRow("ItemLotParam", "ITEMLOT_PARAM_ST", static_cast<u32>(lot) + static_cast<u32>(k));
        if (!row) {
            break;
        }
        LotRowData r;
        r.lot = lot + static_cast<std::int32_t>(k);
        bool ok = SafeGet(row + 0x80, &r.flag);
        for (int i = 0; i < 8 && ok; ++i) {
            ok = SafeGet(row + 4u * i, &r.id[i]) && SafeGet(row + 0x20 + 4u * i, &r.category[i]) &&
                 SafeGet(row + 0x40 + 2u * i, &r.base[i]) && SafeGet(row + 0x50 + 2u * i, &r.cumulate[i]) &&
                 SafeGet(row + 0x8a + i, &r.num[i]);
        }
        if (!ok) {
            break;
        }
        out.push_back(r);
    }
    return out;
}

/// Host: a random lot gets one roll here, so every guest gets the same items (deterministic lots
/// are resolved by each guest from its own ItemLotParam).
void AttachHostRoll(ItemGrant* g) {
    if (!g || !g->items.empty()) {
        return;
    }
    const std::vector<LotRowData> chain = LotChain(g->lot);
    if (!LotChainRandom(chain) || LotChainHasGem(chain)) {
        return; // a gem cannot be rolled here (no instance data): the guest waits for its own world
    }
    static std::uint64_t salt = static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
    salt = salt * 6364136223846793005ull + 1442695040888963407ull;
    g->items = RollLotChain(chain, salt ^ static_cast<std::uint64_t>(g->lot));
    Log("host rolled random lot %d once for the party: %zu item(s)", g->lot, g->items.size());
}

/// ItemAwardSpotFlags unless BB_PARTY_ITEM_SPOTS=0 (the spot stays as before: only the lot's flag).
std::vector<std::int32_t> SpotFlags(std::int32_t lot) {
    static const bool off = EnvOff("BB_PARTY_ITEM_SPOTS");
    return off ? std::vector<std::int32_t>() : ItemAwardSpotFlags(lot);
}

/// At home (load mode 0: the live store is the own save): the lot's item spot flags
/// (ItemAwardSpotFlags) once the lot counts as given here. Returns the ids it set ("" if none).
std::string SetAwardSpotsHome(u64 man, const ItemGrant& g) {
    std::string set;
    for (const std::int32_t f : SpotFlags(g.lot)) {
        if (!IsFlag(man, f)) {
            SetFlag(man, f);
            set += (set.empty() ? "" : ",") + std::to_string(f);
        }
    }
    return set;
}

/// True when the award ran (a popup may have opened).
bool Apply(const ItemGrant& g) {
    const u64 man = Ptr(kFlagMan);
    const u64 mim = Ptr(kMapItemMan);
    const std::int64_t done = g.DoneFlag();
    if (g.source == ItemSource::Mark) {
        const bool was = IsFlag(man, done);
        SetFlag(man, done);
        Log("mark lot %d: flag %lld %s", g.lot, static_cast<long long>(done), was ? "was already set" : "set");
        return false;
    }
    if (g.source == ItemSource::Test) {
        // BB_PARTY_ITEMS_TEST: what the inventory check sees, before any other rule.
        std::string why;
        const std::vector<LotSlotState> slots = LotSlots(g.lot);
        const LotFit fit = ClassifyLotFit(slots, &why);
        for (const LotSlotState& s : slots) {
            Log("test lot %d slot: category %u id %d num %d held %d max %d storage max %d", g.lot, s.category, s.id,
                s.num, s.owned, s.max_num, s.max_storage);
        }
        Log("test lot %d: %zu slot(s), %s%s%s", g.lot, slots.size(), LotFitName(fit), why.empty() ? "" : ": ",
            why.c_str());
    }
    if (IsFlag(man, done)) {
        if ((BossLedgerLot(g) || g.source == ItemSource::Own) && KeyCatchupNeeded(LotSlots(g.lot))) {
            // #16: the ledger says given, the player holds none of the lot's unique items, and
            // they are never used up (a used Gold Pendant reads 0 too: skipped below).
            Log("%s: its done flag %lld is set but none of its unique items is held; giving it after all",
                DescribeItem(g).c_str(), static_cast<long long>(done));
            if (g.flag >= 0) {
                SetFlag(man, done, false); // the award's roll skips rows whose flag is set; it sets it again
            }
        } else {
            const std::string spots = SetAwardSpotsHome(man, g);
            Log("skip %s: already given (flag %lld set)%s%s", DescribeItem(g).c_str(), static_cast<long long>(done),
                spots.empty() ? "" : "; item spot flag(s) set: ", spots.c_str());
            return false;
        }
    }
    if (!mim) {
        Log("skip %s: no MapItemMan", DescribeItem(g).c_str());
        return false;
    }
    // A flagless (ledger) lot the guest earned itself is not checked through the awarding event's
    // completion flag here: C2 writes the host's boss / event flags into this save (join snapshot
    // at the first steady world, live while summoned) before any grant is applied, so the flag
    // would also skip every boss reward the guest never got (a boss killed together: 12401800
    // synced, lot 50000001 skipped). SeedLedgerFromOwnSave records the save's own history as
    // ledger bits before C2 touches it, and own-world awards set their bit in the award hook.
    // An item the game would refuse (a unique key / badge already held, a count at its max with
    // no storage): it would open a blocking "inventory or storage max" dialog. Count it as given.
    std::string why;
    const LotFit fit = ClassifyLotFit(LotSlots(g.lot), &why);
    if (fit != LotFit::Give) {
        SetFlag(man, done);
        const std::string spots = SetAwardSpotsHome(man, g);
        Log("skip %s: %s (%s); flag %lld set instead%s%s", DescribeItem(g).c_str(), LotFitName(fit), why.c_str(),
            static_cast<long long>(done), spots.empty() ? "" : ", item spot flag(s) ", spots.c_str());
        ForgetOwedLot(g.lot);  // held already: not owed
        return false;
    }
    // The ledger bit goes in first: the give can start a save (run A of the C3 check lost a
    // ledger bit written after the award), so the save that holds the item holds the bit too.
    // A lot that gives nothing clears it again.
    if (g.flag < 0) {
        SetFlag(man, done);
    }
    g_added = 0;
    g_in_apply = true;
    reinterpret_cast<AwardFn>(Guest(kAwardLot))(mim, static_cast<u32>(g.lot), 0);
    g_in_apply = false;
    const u32 added = g_added.load();
    if (g.flag < 0 && added == 0) {
        SetFlag(man, done, false);
    }
    const bool now_set = IsFlag(man, done);
    const std::string spots = now_set ? SetAwardSpotsHome(man, g) : std::string();
    Log("gave %s: 0x17DDC50(lot %d, 0) added %u item entr%s; flag %lld is %s%s%s", DescribeItem(g).c_str(), g.lot,
        added, added == 1 ? "y" : "ies", static_cast<long long>(done), now_set ? "set" : "CLEAR (nothing given)",
        spots.empty() ? "" : "; item spot flag(s) set: ", spots.c_str());
    if (added > 0) {
        ForgetOwedLot(g.lot);  // given at home in this session: not owed any more
        ShareNotice(g, "given at home");
    }
    return true;
}

const char* CategoryWord(std::uint32_t c) {
    switch (c) {
    case 0: return "weapon";
    case 1: return "armor";
    case 2: return "rune";
    case 4: return "goods";
    }
    return "item";
}

bool OwnSaveFlag(std::int64_t id) {
    bool v = false;
    return id >= 0 && progress::ReadFlagOwnSave(static_cast<u32>(id), &v) && v;
}

// 0.7.8: the own-world gate of a lot / goods whose vanilla giver checks the player's own progress (the Eye of a
// Blood-drunk Hunter: night 9801 + DLC 6899, m21 12101028). 1 holds, 0 not, -1 unreadable (the own pool).
int OwnGateNow(const std::vector<std::uint32_t>& flags) {
    return OwnGateState(flags, [](std::uint32_t f, bool* v) { return progress::ReadFlagOwnSave(f, v); });
}

std::string GateFlagsText(const std::vector<std::uint32_t>& flags) {
    std::string t;
    for (const std::uint32_t f : flags) {
        bool v = false;
        const bool ok = progress::ReadFlagOwnSave(f, &v);
        t += (t.empty() ? "" : ", ") + std::to_string(f) + (ok ? (v ? "=1" : "=0") : "=?");
    }
    return t;
}

// ---- goods given while summoned: given again after a load that dropped them (H3, par_c6) ----
struct SummonedGive {
    u32 bits = 0;
    u32 id = 0;
    int count = 0;
    int expect = -1;  // held right after the give (or the last re-give)
    int slot = -1;    // save slot of the character it was given to (-1 unknown: an older file)
    u32 cid = 0;      // that character's id (kCharIdValid bits in its save; 0: an older file)
};
std::mutex g_sgive_mu;
std::vector<SummonedGive> g_sgives;
bool SgiveOwnerOn() {
    static const bool on = !EnvOff("BB_PARTY_SGIVE_OWNER");
    return on;
}

/// The character's id in its own save (0: not written yet or not readable). `create`: write a random one when
/// the save has none (main thread; load mode 0 writes the live store, a summoned load the own pool).
u32 OwnCharId(bool create) {
    const u64 man = Ptr(kFlagMan);
    const int mode = LoadMode(man);
    if (!man || (mode != 0 && mode != 1)) {
        return 0;
    }
    auto read = [&](u32 id, bool* v) {
        if (mode == 0) {
            *v = IsFlag(man, id);
            return true;
        }
        return progress::ReadFlagOwnSave(id, v);
    };
    auto write = [&](u32 id, bool on) {
        if (mode == 0) {
            SetFlag(man, id, on);
            return true;
        }
        return progress::WriteFlagOwnSave(id, on);
    };
    bool valid = false;
    if (!read(kCharIdValid, &valid)) {
        return 0;
    }
    if (valid) {
        u32 id = 0;
        for (u32 b = 0; b < kCharIdBits; ++b) {
            bool v = false;
            if (!read(kCharIdFirst + b, &v)) {
                return 0;
            }
            id |= (v ? 1u : 0u) << b;
        }
        return id;
    }
    if (!create) {
        return 0;
    }
    static std::mt19937 rng{std::random_device{}()};
    u32 id = 0;
    while (id == 0) {
        id = static_cast<u32>(rng()) & ((1u << kCharIdBits) - 1);
    }
    for (u32 b = 0; b < kCharIdBits; ++b) {
        if (!write(kCharIdFirst + b, ((id >> b) & 1u) != 0)) {
            return 0;
        }
    }
    if (!write(kCharIdValid, true)) {
        return 0;
    }
    Log("character id %05x written to the own save (flags %u..%u): goods given while summoned are kept for this "
        "character only", id, kCharIdFirst, kCharIdValid);
    return id;
}
bool g_sgives_loaded = false;

std::filesystem::path SummonedGivesPath() {
    std::string dir = bbnet::settings().user_dir;
    if (dir.empty()) {
        if (const char* u = std::getenv("BB_GPU_USER_DIR")) dir = u;
        else if (const char* u2 = std::getenv("BB_USER_DIR")) dir = u2;
        else dir = std::string(std::getenv("BB_DATA_DIR") ? std::getenv("BB_DATA_DIR") : ".") + "/user";
    }
    return std::filesystem::path(dir) / "party_summoned_gives.txt";
}

/// Caller holds g_sgive_mu. Kept on disk so a crash while summoned does not forget them.
void SaveSummonedGives() {
    std::error_code ec;
    const std::filesystem::path path = SummonedGivesPath();
    if (g_sgives.empty()) {
        std::filesystem::remove(path, ec);
        return;
    }
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        for (const SummonedGive& g : g_sgives) {
            f << g.bits << ' ' << g.id << ' ' << g.count << ' ' << g.expect << ' ' << g.slot << ' ' << g.cid << "\n";
        }
    }
    std::filesystem::rename(tmp, path, ec);
}

void LoadSummonedGives() {
    std::lock_guard<std::mutex> lk(g_sgive_mu);
    if (g_sgives_loaded) {
        return;
    }
    g_sgives_loaded = true;
    std::ifstream f(SummonedGivesPath());
    std::string line;
    std::size_t no_identity = 0;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        SummonedGive g;
        if (!(in >> g.bits >> g.id >> g.count >> g.expect)) {
            continue;
        }
        if (!(in >> g.slot)) {
            g.slot = -1;  // a file from before the slot field
        }
        if (!(in >> g.cid)) {
            g.cid = 0;    // a file from before the character id
        }
        if (SgiveOwnerOn() && (g.slot < 0 || g.cid == 0)) {
            ++no_identity;  // whose it is cannot be told: never replayed (CL-55cea7)
            continue;
        }
        g_sgives.push_back(g);
    }
    if (no_identity) {
        Log("goods given while summoned: %zu entr%s from an older file without a character id dropped (they cannot be "
            "tied to this character; BB_PARTY_SGIVE_OWNER=0 restores the old replay)", no_identity,
            no_identity == 1 ? "y" : "ies");
        SaveSummonedGives();
    }
    if (!g_sgives.empty()) {
        Log("goods given while summoned before the last exit: %zu kept for the check after the next load", g_sgives.size());
    }
}

void NoteSummonedGive(u32 bits, u32 id, int count, int after) {
    if (after < 0 || count <= 0) {
        return;
    }
    const int slot = party::flow::CurrentSaveSlot();
    const u32 cid = SgiveOwnerOn() ? OwnCharId(true) : 0;
    if (SgiveOwnerOn() && (slot < 0 || cid == 0)) {
        Log("goods %u given while summoned: this character cannot be identified (slot %d, id %u); not kept for the "
            "own world", id, slot, cid);
        return;
    }
    std::lock_guard<std::mutex> lk(g_sgive_mu);
    for (SummonedGive& g : g_sgives) {
        if (g.bits == bits && g.id == id && g.slot == slot && g.cid == cid) {
            g.count += count;
            g.expect = after;
            SaveSummonedGives();
            return;
        }
    }
    g_sgives.push_back({bits, id, count, after, slot, cid});
    SaveSummonedGives();
}

/// Main thread: 3 s into the first steady OWN world (load mode 0) after a load, the goods given
/// while summoned that the own data lacks are given again (no item-get dialog: the player saw it
/// at the first give) and a save is requested, which keeps them for good; the list is cleared.
/// par_c6 read the Oedon Tomb Key at 0 after summon loads and title joins as a save that lost it;
/// verify_c6 (live) showed the save keeps it: a summoned guest's inventory never shows key items
/// (goodsType 1), and the own world (from the save, after crashes) holds them. This check stays as
/// a safety net; it found nothing missing live. Nothing is given while summoned (a key reads 0
/// there by design; a re-give there could double up when the session ends).
void SummonedGivesTick(const GameSnapshot& gs, int load_mode, double now) {
    static bool armed = true;  // a load (or the start) since the last check
    static double steady_at = -1;
    if (load_mode != 0) {
        armed = true;  // checked at the next own world
        steady_at = -1;
        return;
    }
    if (!gs.world_up || gs.loading || gs.map_id == 0xffffffffu || !gs.player_rec) {
        armed = true;
        steady_at = -1;
        return;
    }
    if (!armed) {
        return;
    }
    if (steady_at < 0) {
        steady_at = now;
    }
    if (now - steady_at < 3.0) {
        return;
    }
    armed = false;
    const bool enforce = SgiveOwnerOn();
    const u32 cid_now = enforce && OwnWorld() == 1 ? OwnCharId(true) : 0;  // a fresh character gets its id here
    LoadSummonedGives();
    std::lock_guard<std::mutex> lk(g_sgive_mu);
    if (g_sgives.empty()) {
        return;
    }
    std::string text;
    int regave = 0;
    const int slot_now = party::flow::CurrentSaveSlot();
    const auto owner_of = [&](const SummonedGive& g) {
        return SummonedGiveOwner(g.slot, g.cid, slot_now, cid_now, enforce);
    };
    {   // Entries of another character in this slot (a new character, or the old one's after a re-roll) are never
        // replayed: drop them.
        const std::size_t before = g_sgives.size();
        g_sgives.erase(std::remove_if(g_sgives.begin(), g_sgives.end(),
                                      [&](const SummonedGive& g) { return owner_of(g) == SgiveOwner::Drop; }),
                       g_sgives.end());
        if (g_sgives.size() != before) {
            Log("goods given while summoned: %zu entr%s given to another character in save slot %d (or without a "
                "character id) dropped: not this character's (id %05x)", before - g_sgives.size(),
                before - g_sgives.size() == 1 ? "y" : "ies", slot_now, cid_now);
            SaveSummonedGives();
            if (g_sgives.empty()) {
                return;
            }
        }
    }
    std::size_t other = 0;
    for (SummonedGive& g : g_sgives) {
        if (owner_of(g) != SgiveOwner::Replay) {
            ++other;  // another character's (save slot g.slot), or this one is not identifiable now: kept
            continue;
        }
        const int held = ItemCount(g.bits, g.id);
        const int n = SummonedRegiveCount(g.expect, held, g.count);
        if (n <= 0) {
            continue;
        }
        // Only a key item that is never used up is given again: the game puts the saved key section
        // back at home, without the summoned copy. Anything else given while summoned stays in the
        // inventory, so fewer held now means used, dropped or sold (0.6.9: Ritual Blood / a pendant
        // used while summoned came back at home).
        if ((g.bits >> 28) == 4) {
            const std::vector<std::uint32_t> gate = GoodsOwnGateFlags(static_cast<std::int32_t>(g.id));
            if (!gate.empty() && OwnGateNow(gate) != 1) {
                Log("goods %u given while summoned: not given again: this world has not reached its vanilla gate (%s)",
                    g.id, GateFlagsText(gate).c_str());
                continue;
            }
        }
        LotSlotState ks;
        ks.category = g.bits >> 28;
        ks.id = static_cast<std::int32_t>(g.id);
        FillGoodsLimits(&ks);
        if (!KeyRegiveAllowed(ks) || !KeyGiveAllowedHeld(ks, held)) {
            Log("%s %u given while summoned: held %d now (%d right after the give); not given again (%s)",
                CategoryWord(g.bits >> 28), g.id, held, g.expect,
                KeyRegiveAllowed(ks) ? "held" : "not a key item the game drops at home: used, dropped or sold");
            continue;
        }
        const bool ok = GiveItem(g.bits, g.id, n);
        const int after = ItemCount(g.bits, g.id);
        char b[128];
        std::snprintf(b, sizeof b, "%s %u x%d (held %d -> %d%s)", CategoryWord(g.bits >> 28), g.id, n, held, after,
                      ok ? "" : ", GiveItemDirect unavailable");
        text += (text.empty() ? "" : "; ") + std::string(b);
        regave += ok ? 1 : 0;
        if (ok && after >= 0) {
            g.expect = after;
        }
    }
    if (!text.empty()) {
        Log("goods given while summoned are missing in the own world; given again: %s", text.c_str());
    }
    if (other) {
        Log("goods given while summoned: %zu entr%s for another character (not save slot %d) kept for it", other,
            other == 1 ? "y" : "ies", slot_now);
    }
    if (OwnWorld() == 1) {
        RequestGameSave("C3 goods given while summoned, kept at home");
        Log("own world: %zu good(s) given while summoned are in the own save now; list cleared",
            g_sgives.size() - other);
        g_sgives.erase(std::remove_if(g_sgives.begin(), g_sgives.end(),
                                      [&](const SummonedGive& g) { return owner_of(g) == SgiveOwner::Replay; }),
                       g_sgives.end());
    }
    SaveSummonedGives();
    (void)regave;
    (void)load_mode;
}

// Key-item lots a summoned guest was handed but could not take (SummonedResult::Defer): kept on
// disk per save slot so they come at the next own world even when the guest quits from the host's
// world or keeps joining from its title (title join never loads the own world; live pard2 c4: the
// Pthumeru Chalice of the Blood-starved Beast waited, the session ended before the guest went home).
struct OwedLot {
    std::int32_t lot = 0;
    int slot = -1;
    bool given = true;  // given while summoned (only a dropped copy is given again); false: deferred
    u32 cid = 0;        // the character's id (SummonedGive::cid); 0: an older file
};
std::mutex g_owed_mu;
std::vector<OwedLot> g_owed;
bool g_owed_loaded = false;

std::filesystem::path OwedLotsPath() {
    return SummonedGivesPath().parent_path() / "party_owed_lots.txt";
}

void SaveOwedLotsLocked();

/// Caller holds g_owed_mu.
void LoadOwedLotsLocked() {
    if (g_owed_loaded) {
        return;
    }
    g_owed_loaded = true;
    std::ifstream f(OwedLotsPath());
    std::string line;
    std::size_t dropped = 0;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        OwedLot o;
        int given = 1;  // a file from before the third field: treated as given (never a second copy)
        if (!(in >> o.lot >> o.slot)) {
            continue;
        }
        if (in >> given) {
            o.given = given != 0;
        }
        if (!(in >> o.cid)) {
            o.cid = 0;
        }
        if (SgiveOwnerOn() && (o.slot < 0 || o.cid == 0)) {
            ++dropped;  // whose it is cannot be told: never given (CL-55cea7)
            continue;
        }
        g_owed.push_back(o);
    }
    if (dropped) {
        Log("owed key items: %zu entr%s from an older file without a character id dropped", dropped,
            dropped == 1 ? "y" : "ies");
        SaveOwedLotsLocked();
    }
}

/// Caller holds g_owed_mu.
void SaveOwedLotsLocked() {
    std::error_code ec;
    const std::filesystem::path path = OwedLotsPath();
    if (g_owed.empty()) {
        std::filesystem::remove(path, ec);
        return;
    }
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        for (const OwedLot& o : g_owed) {
            f << o.lot << ' ' << o.slot << ' ' << (o.given ? 1 : 0) << ' ' << o.cid << '\n';
        }
    }
    std::filesystem::rename(tmp, path, ec);
}

void NoteOwedLot(std::int32_t lot, bool given) {
    const int slot = party::flow::CurrentSaveSlot();
    const u32 cid = SgiveOwnerOn() ? OwnCharId(true) : 0;
    if (SgiveOwnerOn() && (slot < 0 || cid == 0)) {
        Log("owed key item: lot %d: this character cannot be identified (slot %d, id %u); not kept", lot, slot, cid);
        return;
    }
    std::lock_guard<std::mutex> lk(g_owed_mu);
    LoadOwedLotsLocked();
    for (OwedLot& o : g_owed) {
        if (o.lot == lot && o.slot == slot && o.cid == cid) {
            if (given && !o.given) {
                o.given = true;
                SaveOwedLotsLocked();
            }
            return;
        }
    }
    if (g_owed.size() < 64) {
        g_owed.push_back({lot, slot, given, cid});
        SaveOwedLotsLocked();
        Log("owed key item: lot %d (%s) kept for this character's next own world (save slot %d)", lot,
            given ? "given while summoned" : "not given yet", slot);
    }
}

void ForgetOwedLot(std::int32_t lot) {
    const int slot = party::flow::CurrentSaveSlot();
    const u32 cid = SgiveOwnerOn() ? OwnCharId(false) : 0;
    std::lock_guard<std::mutex> lk(g_owed_mu);
    LoadOwedLotsLocked();
    const auto it = std::remove_if(g_owed.begin(), g_owed.end(), [&](const OwedLot& o) {
        return o.lot == lot && SummonedGiveOwner(o.slot, o.cid, slot, cid, SgiveOwnerOn()) == SgiveOwner::Replay;
    });
    if (it != g_owed.end()) {
        g_owed.erase(it, g_owed.end());
        SaveOwedLotsLocked();
    }
}

/// Own world (KeyCatchupTick): the lots owed to this save slot whose items are still missing.
std::vector<OwedLot> TakeOwedLots() {
    const int slot = party::flow::CurrentSaveSlot();
    const u32 cid = SgiveOwnerOn() ? OwnCharId(true) : 0;
    std::lock_guard<std::mutex> lk(g_owed_mu);
    LoadOwedLotsLocked();
    std::vector<OwedLot> lots;
    auto it = std::remove_if(g_owed.begin(), g_owed.end(), [&](const OwedLot& o) {
        const SgiveOwner w = SummonedGiveOwner(o.slot, o.cid, slot, cid, SgiveOwnerOn());
        if (w == SgiveOwner::Drop) {
            Log("owed key item: lot %d dropped: given to another character in save slot %d", o.lot, o.slot);
            return true;  // never given to this one
        }
        if (w != SgiveOwner::Replay) {
            return false;
        }
        lots.push_back(o);
        return true;
    });
    if (it != g_owed.end()) {
        g_owed.erase(it, g_owed.end());
        SaveOwedLotsLocked();
    }
    return lots;
}

/// Main thread: 4 s into each steady OWN world (load mode 0) after a load, every boss-reward key
/// item this save's ledger says was given but the inventory lacks is given once more and a save is
/// requested (KeyCatchupLots / KeyCatchupNeeded). From the save alone: no host, no party list.
/// Playtest 1 save audit: the guest's save had the ledger bits of lot 31000 (Oedon Tomb Key) and
/// 50000010 (Sword Hunter Badge) and both boss flags, but no key item at all (0.6.0 gave them while
/// summoned; that inventory is not what the save keeps). Runs after SummonedGivesTick (3 s), so a
/// good it gives back reads as held here.
void KeyCatchupTick(const GameSnapshot& gs, int load_mode, double now) {
    static bool armed = true;
    static double steady_at = -1;
    if (load_mode != 0 || !gs.world_up || gs.loading || gs.map_id == 0xffffffffu || !gs.player_rec) {
        armed = true;
        steady_at = -1;
        return;
    }
    if (!armed) {
        return;
    }
    if (steady_at < 0) {
        steady_at = now;
    }
    if (now - steady_at < 4.0) {
        return;
    }
    armed = false;
    const u64 man = Ptr(kFlagMan);
    if (!man || LoadMode(man) != 0 || OwnWorld() != 1) {
        return;
    }
    const std::vector<std::int32_t> lots = KeyCatchupLots(
        [man](std::uint32_t id) { return IsFlag(man, id); },
        [](std::uint32_t id) { return progress::CategoryOf(id) == progress::Category::boss_defeated; });
    const std::vector<OwedLot> owed = TakeOwedLots();
    int gave = 0;
    // Owed lots: deferred while summoned (every goods slot, a unique one only when missing, never
    // once the lot's done flag is in this save) or given while summoned (only a dropped copy of an
    // item that is never used up). The done flag goes into the save with the give, so the lot is
    // never given a second time later (a rejoin's full list, the catch-up).
    for (const OwedLot& o : owed) {
        const std::int32_t lot = o.lot;
        const std::int32_t flag = ItemLotFlag(lot);
        const std::int32_t idx = flag > 0 ? kItemNone : ItemLedgerIndex(lot);
        const std::int64_t done = flag > 0 ? flag : idx >= 0 ? std::int64_t(kItemLedgerBase) + idx : -1;
        const bool done_set = done >= 0 && IsFlag(man, done);
        if (const std::vector<std::uint32_t> gate = LotOwnGateFlags(lot); !gate.empty() && OwnGateNow(gate) != 1) {
            Log("owed lot %d: not given: this world has not reached its vanilla gate (%s); the host's list brings it "
                "again once it has", lot, GateFlagsText(gate).c_str());
            continue;
        }
        bool any = false;
        for (const LotSlotState& sl : LotSlots(lot)) {
            if (sl.id <= 0) {
                continue;
            }
            if (sl.category != 4) {
                Log("owed lot %d: %s %d is not goods; not given here", lot, CategoryWord(std::uint32_t(sl.category)),
                    sl.id);
                continue;
            }
            const int n = std::max(1, sl.num);
            const int held_now = ItemCount(0x40000000u, static_cast<u32>(sl.id));
            if (!OwedGiveAllowed(o.given, done_set, sl, held_now)) {
                Log("owed lot %d (%s): goods %d held %d%s: not given again", lot,
                    o.given ? "given while summoned" : "not given yet", sl.id, held_now,
                    !o.given && done_set ? ", the lot's done flag is in this save"
                    : o.given && !KeyRegiveAllowed(sl) ? ", it can be used up" : "");
                continue;
            }
            const bool ok = GiveItem(0x40000000u, static_cast<u32>(sl.id), n);
            Log("owed lot %d (%s): %s goods %d x%d (held %d -> %d)", lot,
                o.given ? "given while summoned, missing here" : "not given yet", ok ? "gave" : "could not give", sl.id,
                n, held_now, ItemCount(0x40000000u, static_cast<u32>(sl.id)));
            gave += ok ? 1 : 0;
            any = any || ok;
        }
        if (any && done >= 0 && !done_set) {
            SetFlag(man, done);
            Log("owed lot %d: done flag %lld set", lot, static_cast<long long>(done));
        }
    }
    for (const std::int32_t lot : lots) {
        const std::vector<LotSlotState> slots = LotSlots(lot);
        if (!KeyCatchupNeeded(slots)) {
            continue;
        }
        const std::int32_t idx = ItemLedgerIndex(lot);
        char why[128];
        if (idx >= 0) {
            std::snprintf(why, sizeof why, "ledger %d = flag %d set with boss flag %d in this save", idx,
                          int(kItemLedgerBase) + idx, ItemLedgerDoneFlag(idx));
        } else {
            std::snprintf(why, sizeof why, "shared key treasure, flag %d set in this save", ItemLotFlag(lot));
        }
        for (const LotSlotState& sl : slots) {
            const int n = std::max(1, sl.num);
            // Re-read: an earlier lot of this pass may have given the same goods (Iron Door Key x2).
            const int held_now = ItemCount(0x40000000u, static_cast<u32>(sl.id));
            if (!KeyGiveAllowedHeld(sl, held_now)) {
                Log("key item catch-up: goods %d (lot %d) held %d now: not given again", sl.id, lot, held_now);
                continue;
            }
            const bool ok = GiveItem(0x40000000u, static_cast<u32>(sl.id), n);
            const int after = ItemCount(0x40000000u, static_cast<u32>(sl.id));
            Log("key item catch-up: %s goods %d x%d (lot %d, %s; held %d -> %d)",
                ok ? "gave" : "could not give (GiveItemDirect unavailable)", sl.id, n, lot, why, sl.owned, after);
            gave += ok ? 1 : 0;
        }
    }
    if (gave) {
        RequestGameSave("C3 key item catch-up");
    }
}

enum class SummonedResult { Given, Skipped, Defer };

/// While summoned (load mode 1: the live flag store is the host's overlay, where the host's flag
/// is already set, so 0x17DDC50 would roll nothing): the lot's items go straight into the own
/// inventory (GiveItemDirect 0x131CB70, no popup) and its flags / ledger bit into the own save's
/// pool (C2's mode-1 pool write). Nothing is written to the overlay.
SummonedResult ApplySummoned(const ItemGrant& g, bool quiet) {
    const std::int64_t done = g.DoneFlag();
    if (g.source == ItemSource::Mark) {
        const bool was = OwnSaveFlag(done);
        const bool ok = progress::WriteFlagOwnSave(static_cast<u32>(done), true);
        Log("mark lot %d while summoned: own-save flag %lld %s", g.lot, static_cast<long long>(done),
            was ? "was already set" : ok ? "set (pool)" : "NOT written");
        if (!was && ok) {
            RequestGameSave("C3 mark");
        }
        return SummonedResult::Skipped;
    }
    if (OwnSaveFlag(done)) {
        const std::vector<LotSlotState> slots = LotSlots(g.lot);
        if ((BossLedgerLot(g) || g.source == ItemSource::Own) && KeyCatchupNeeded(slots)) {
            // verify_c6 (live): the summoned view never holds key items; the 0.6.1 regression run
            // saw the same for unique goods (Amelia's 4001: held 0 here, 1 at home). Every slot of
            // this lot is unique goods (LedgerSkipSuspicious), so a give here would put the game's
            // "inventory or storage max" box up for an item the guest has. The own world checks it
            // (Apply / the key-item catch-up read the real held count first).
            Log("%s: its done flag %lld is set; its unique items read 0 here, as a summoned guest's key "
                "items do (%s): checked in the own world",
                DescribeItem(g).c_str(), static_cast<long long>(done), HeldSummary(g.lot).c_str());
            return SummonedResult::Defer;
        }
        // The lot's item spot (an altar's event flag, ITEM_AWARD_SPOT) goes with it: a save that
        // got the lot before this rule still offers the spot after every load (playtest 2026-10-20).
        std::string spots;
        for (const std::int32_t f : SpotFlags(g.lot)) {
            if (!OwnSaveFlag(f) && progress::WriteFlagOwnSave(static_cast<u32>(f), true)) {
                spots += (spots.empty() ? "" : ",") + std::to_string(f);
            }
        }
        Log("skip %s: already given (own-save flag %lld set; %s)%s%s", DescribeItem(g).c_str(),
            static_cast<long long>(done), HeldSummary(g.lot).c_str(),
            spots.empty() ? "" : "; item spot flag(s) set in the own pool: ", spots.c_str());
        if (!spots.empty()) {
            RequestGameSave("C3 item spot flags");
        }
        return SummonedResult::Skipped;
    }
    const std::vector<LotRowData> chain = LotChain(g.lot);
    // A re-give ignores the row flags (the done flag is one of them and is set).
    LotPlan plan = PlanSummonedLot(chain, g.items, [](std::int32_t f) { return OwnSaveFlag(f); });
    if (g.source == ItemSource::Chalice || g.source == ItemSource::BossDrop) {
        // G1: exactly the host's items, never a flag (dungeon flags are the slot's own), and no
        // key goods (a chalice reaches every member through the chalice share; a boss's key items
        // through the key / unique rules).
        plan.result = LotPlanResult::Give;
        plan.why = g.source == ItemSource::Chalice ? "the host's chalice award" : "the host's boss drop";
        plan.row_flags.clear();
        plan.items.clear();
        for (const GrantItem& it : g.items) {
            LotSlotState s;
            s.category = it.category;
            s.id = it.id;
            s.num = it.count;
            FillGoodsLimits(&s);
            if (KeyGoods(s)) {
                Log("%s: goods %d is a key item (shared by its own rule); left out", DescribeItem(g).c_str(),
                    it.id);
                continue;
            }
            plan.items.push_back(it);
        }
    }
    if (plan.result != LotPlanResult::Give) {
        Log("%s: not while summoned (%s: %s); waits for the own world", DescribeItem(g).c_str(),
            LotPlanResultName(plan.result), plan.why.c_str());
        return SummonedResult::Defer;
    }
    // The same refusal rule as at home: a unique item already held / no room counts as given.
    std::vector<LotSlotState> slots;
    for (const GrantItem& it : plan.items) {
        LotSlotState s;
        s.category = it.category;
        s.id = it.id;
        s.num = it.count;
        FillGoodsLimits(&s);
        slots.push_back(s);
    }
    if (!SummonedGiveAllowed(slots) && !(g.source == ItemSource::Repeat && RepeatSummonedGiveAllowed(slots))) {
        // A key item / unique goods. With the own key section shown (H4, SummonedKeyViewActive) the
        // held count is the real one and a give lands in the section the save keeps: given now
        // (BB_PARTY_KEY_ITEMS_SUMMONED=0: waits for the own world). In the game's summoned view
        // (BB_PARTY_KEY_VIEW=0) it reads 0 whether the save holds it or not (0.6.2) and the view is
        // rebuilt without it at the next host-world load; a key the save already holds unseen would
        // get a second stack in a save written while summoned (items.md 10.6): only
        // BB_PARTY_KEY_ITEMS_SUMMONED=1 gives it then. Given now, it is owed too (on disk): the own
        // world reads the real held count and gives it there only if the copy did not stay
        // (KeyCatchupTick, SummonedGivesTick).
        std::string keys;
        for (const LotSlotState& s : slots) {
            if (KeyGoods(s)) {
                keys += (keys.empty() ? "" : ", ") + std::to_string(s.id);
            }
        }
        // In the game's view a usable unique item (Gold Pendant, Tear Stone) is never given here:
        // used while summoned, it would read 0 at home and the owed check could not tell it from a
        // dropped copy.
        const bool usable = !SummonedKeyGiveNow(slots);
        const bool view = SummonedKeyViewActive();
        // With the view: System > My party: Rewards "Key items at once" (bbport.ini
        // party_key_items_summoned; BB_PARTY_KEY_ITEMS_SUMMONED at start), live. Without: only an
        // explicit BB_PARTY_KEY_ITEMS_SUMMONED=1 (the setting's default on means nothing there).
        const bool now = SummonedKeyGiveNowRule(
            view, view ? (BbSettings::Get().party_key_items_summoned.load() ? "1" : "0")
                       : std::getenv("BB_PARTY_KEY_ITEMS_SUMMONED"),
            usable);
        NoteOwedLot(g.lot, now);
        if (!now) {
            Log("%s: key item / unique goods %s: not given while summoned%s; waits for the own world",
                DescribeItem(g).c_str(), keys.c_str(),
                view ? " (Key items at once is off)" : usable ? " (it can be used up)" : "");
            return SummonedResult::Defer;
        }
        Log("%s: key item / unique goods %s: given now while summoned%s; also owed to the own world, which "
            "checks the held count",
            DescribeItem(g).c_str(), keys.c_str(), view ? " (own key section shown)" : "");
    }
    std::string why;
    const LotFit fit = ClassifyLotFit(slots, &why);
    // Flags first (the save that holds the items holds the flags; a save between the two loses an
    // item rather than giving it twice).
    std::vector<std::int64_t> flags;
    if (done >= 0) {  // a repeatable gift has none
        flags.push_back(done);
    }
    for (const std::int32_t f : plan.row_flags) {
        if (f != done && f > 0) {
            flags.push_back(f);
        }
    }
    // The item spot's own flag (ITEM_AWARD_SPOT): the guest's overlay item bits follow its own pool
    // (D4), so the spot is gone for it once the lot was shared - after loads too (an altar offered
    // again after the host's death respawn, playtest 2026-10-20).
    for (const std::int32_t f : SpotFlags(g.lot)) {
        if (std::find(flags.begin(), flags.end(), f) == flags.end()) {
            flags.push_back(f);
        }
    }
    std::string flag_list;
    for (const std::int64_t f : flags) {
        const bool ok = progress::WriteFlagOwnSave(static_cast<u32>(f), true);
        flag_list += (flag_list.empty() ? "" : ",") + std::to_string(f) + (ok ? "" : "(NOT written)");
    }
    // The treasure this lot lies as in the host's world (shared key items): taken from this game's copy
    // too, else it stays on the ground for the guest and a second pickup gives the key twice.
    if (const std::uint32_t tf = TreasureFlagToHide(g.lot); tf && g_treasure_hider && g_treasure_hider(tf)) {
        Log("%s: its treasure (flag %u) taken out of this game's copy of the host's world", DescribeItem(g).c_str(), tf);
    }
    if (fit != LotFit::Give) {
        Log("skip %s while summoned: %s (%s); own-save flag(s) %s set instead", DescribeItem(g).c_str(),
            LotFitName(fit), why.c_str(), flag_list.c_str());
        RequestGameSave("C3 item flags");
        return SummonedResult::Skipped;
    }
    std::string given, notice;
    int gave = 0, native = 0;
    for (const GrantItem& it : plan.items) {
        char b[256];
        if (it.category == 8) {
            // A blood gem: the host's own gem recreated, given through the award's give list
            // (which also queues the native item-get dialog).
            std::string why;
            const u32 added_before = g_added.load();
            const bool ok = GiveGem(it, &why);
            gave += ok ? 1 : 0;
            native += ok ? 1 : 0;
            std::snprintf(b, sizeof b, "blood %s (%s)", GemText(it).c_str(),
                          !ok ? why.c_str()
                              : g_added.load() != added_before ? "inventory add, item-get dialog"
                                                                 : "given, item-get dialog");
            given += (given.empty() ? "" : "; ") + std::string(b);
            std::snprintf(b, sizeof b, "blood gem %d", it.id);
            notice += (notice.empty() ? "" : ", ") + std::string(b);
            continue;
        }
        const u32 bits = it.category << 28;
        const int before = ItemCount(bits, static_cast<u32>(it.id));
        LotSlotState lim;
        lim.category = it.category;
        lim.id = it.id;
        FillGoodsLimits(&lim);
        // Past the inventory maximum: the game's give list (the rest into the storage, its own
        // item-get dialog); else the direct give and our dialog entry.
        const bool over = it.category == 4 && lim.max_num > 0 && before >= 0 && before + it.count > lim.max_num;
        const int room_before = over ? StorageRoom(static_cast<u32>(it.id)) : -1;
        const bool via_list = over && GiveGoodsViaList(static_cast<u32>(it.id), it.count);
        const int room_after = via_list ? StorageRoom(static_cast<u32>(it.id)) : -1;
        const bool ok = via_list || GiveItem(bits, static_cast<u32>(it.id), it.count);
        const int after = ItemCount(bits, static_cast<u32>(it.id));
        // The game's own "item acquired" dialog entry, as the host's lot award shows it.
        const bool shown = via_list || (ok && !quiet && ShowItemGet(bits, static_cast<u32>(it.id), it.count));
        LotSlotState noted;
        noted.category = it.category;
        noted.id = it.id;
        FillGoodsLimits(&noted);
        if (ok && KeyRegiveAllowed(noted)) {  // only these can be dropped by the game at home
            NoteSummonedGive(bits, static_cast<u32>(it.id), it.count, after);
        }
        gave += ok ? 1 : 0;
        native += shown ? 1 : 0;
        char room[96] = "";
        if (via_list) {
            std::snprintf(room, sizeof room, ", past the maximum: the game's give list, storage room %d -> %d", room_before,
                          room_after);
        }
        std::snprintf(b, sizeof b, "%s %d x%d (held %d -> %d%s%s%s)", CategoryWord(it.category), it.id, it.count,
                      before, after, ok ? "" : ", GiveItemDirect unavailable", room,
                      !ok ? "" : shown ? ", item-get dialog" : quiet ? ", no dialog (catch-up batch)" : ", NO item-get dialog");
        given += (given.empty() ? "" : "; ") + std::string(b);
        std::snprintf(b, sizeof b, "%s %d x%d", CategoryWord(it.category), it.id, it.count);
        notice += (notice.empty() ? "" : ", ") + std::string(b);
    }
    if (!plan.items.empty() && gave == 0) {
        for (const std::int64_t f : flags) {
            progress::WriteFlagOwnSave(static_cast<u32>(f), false);
        }
        Log("%s: the direct give is not available; flags cleared again, waits for the own world",
            DescribeItem(g).c_str());
        return SummonedResult::Defer;
    }
    Log("gave %s while summoned (%s): %s; own-save flag(s) %s set (pool, load mode %d), overlay untouched",
        DescribeItem(g).c_str(), plan.why.c_str(), given.empty() ? "nothing left to give" : given.c_str(),
        flag_list.c_str(), progress::FlagLoadMode());
    RequestGameSave("C3 items given while summoned"); // items and flags land in the same save
    if (gave == 0) {
        return SummonedResult::Skipped;
    }
    if (!quiet) {
        ShareNotice(g, "given while summoned");
    }
    if (native < gave && !quiet) {
        // Fallback only: the native dialog could not be queued for some item.
        party::status::SetLastEvent("Party item received: " + notice);
    }
    return SummonedResult::Given;
}

// ---- H4 (0.7.1): a summoned guest sees its own key items (items.md 10.7) ----
// The inventory (PlayerGameData +0x328, EquipInventoryData; offsets from it) holds an item
// container at +0x10 (0x1AC5340 builds it): the normal section {+0x18 entries, +0x20 count}, the
// key section the save keeps {+0x28 entries, +0x30 count}, a second, summoned key section {+0x38,
// +0x40} (64 entries each, +0x24), and the views every lookup reads: +0x48 normal, +0x58 key (+0x60
// a pointer to its count). The
// in-game step's start 0x1937570 calls 0x1AC5600(container, summoned) for a summoned load (game
// mode 3): it empties the summoned key section and points the key view at it; an own-world load
// points the key view at the saved section instead (inline, same as summoned 0). The save writer
// 0x1AC5D10 writes all three, so a key given into the summoned view was in a save next to the saved
// one (items.md 10.6). Here a party guest's summoned load runs the game's call as it is (the
// summoned section emptied), then the same call with 0: the key view is the saved section again,
// its index rebuilt by the game (0x1AC5720), and a give lands there, one stack in the save.
constexpr u64 kKeyViewCall = 0x1937a97;  // call 0x1AC5600 (rdi = PlayerGameData + 0x338, esi = 1)
constexpr u64 kKeyViewSet = 0x1ac5600;   // (item container*, int summoned)
std::atomic<bool> g_key_view{false};

void BB_COOP_SYSV KeyViewHook(u64 container, u32 summoned) {
    using Fn = void(BB_COOP_SYSV*)(u64, u32);
    const Fn set = reinterpret_cast<Fn>(Guest(kKeyViewSet));
    set(container, summoned);
    if (summoned != 0 && g_key_view.load()) {
        set(container, 0);
        std::uint32_t n = 0;
        SafeGet(container + 0x20, &n);  // the saved key section's count (inventory +0x30)
        Log("summoned load: the own key section is shown (%u key item(s); the summoned one emptied)", n);
    }
}

void InstallKeyView() {
    // System > My party: Rewards "Own key items as guest" (bbport.ini party_key_view; BB_PARTY_KEY_VIEW
    // at start): read here once, next start.
    if (!KeyViewOn(BbSettings::Get().party_key_view.load() ? "1" : "0")) {
        Log("own key items as guest off (party_key_view / BB_PARTY_KEY_VIEW=0): the game's summoned key "
            "view (empty after each host-world load)");
        return;
    }
    // lea rdi, [rbx + 0x338]; mov esi, 1 before the call, and 0x1AC5600's own first bytes.
    if (!Matches(kKeyViewCall - 12, {0x48, 0x8d, 0xbb, 0x38, 0x03, 0x00, 0x00, 0xbe, 0x01, 0x00, 0x00, 0x00}) ||
        !MatchesOriginal(kKeyViewSet, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53})) {
        Log("summoned key view: code differs; the game's view stays");
        return;
    }
    if (HookCallSite(kKeyViewCall, kKeyViewSet, reinterpret_cast<const void*>(&KeyViewHook),
                     "summoned key view (0x1937A97)")) {
        g_key_view = true;
        Log("summoned key view: a summoned load shows the own key section (BB_PARTY_KEY_VIEW=0: off)");
    }
}

} // namespace

bool SummonedKeyViewActive() {
    return g_key_view.load();
}

void InstallItemsPatches() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) {
        return;
    }
    if (!Image()) {
        Log("no image; items off");
        return;
    }
    if (bb_party_enabled() && !EnvOff("BB_PARTY_FULL_REWARDS")) {
        ParityPatches();
    } else {
        Log("parity patches off (%s)", bb_party_enabled() ? "BB_PARTY_FULL_REWARDS=0" : "not in party mode");
    }
    if (bb_party_enabled()) {
        InstallKeyView();
    }
    if (EnvOff("BB_PARTY_ITEMS")) {
        Log("BB_PARTY_ITEMS=0: no item replay");
        return;
    }
    // push rbp; mov rbp, rsp; push r15, r14, r13, r12, rbx (13 bytes; then sub rsp, 0x1a8).
    // Replaced (not an entry hook): the host's call runs inside ours, so the items its roll gave
    // (0x17D89F0, GiveListEntry) travel with the grant.
    void* original = nullptr;
    if (ReplacePrologue(kAwardLot, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                        reinterpret_cast<const void*>(&AwardReplace), &original, "item award (0x17DDC50)")) {
        g_award_original = reinterpret_cast<AwardFn>(original);
    } else {
        Log("no award hook: host-only lots are not reported (flag reports still work)");
    }
    HookPrologue(kGiveList, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                 &GiveListEntry, "item give log (0x17D89F0)");
    SeedHostSeq();
    ParseTestLots();
    ParseHostTest();
    // C2 hands every host item_lot_picked flag 0 -> 1 to OnHostFlagSet (main thread).
    progress::SetItemFlagObserver(&OnHostFlagSet);
    progress::SetBeforeApplyHook(&SeedLedgerFromOwnSave);
    Log("C2 item-flag observer registered");
    Log("%zu flagged lots, %zu ledger lots (flags %u..%u)", ItemLotFlagCount(), ItemLedgerCount(), kItemLedgerBase,
        kItemLedgerBase + kItemLedgerSize - 1);
    g_on = true;
}

void SeedLedgerFromOwnSave() {
    if (!g_on.load()) {
        return;
    }
    const u64 man = Ptr(kFlagMan);
    const int mode = LoadMode(man);
    if (!man || (mode != 0 && mode != 1)) {
        return;
    }
    // Mode 0: the live store is the own save. Mode 1 (summoned from the first load on, seamless
    // play): the own pool, never the host's overlay.
    auto is_set = [man, mode](std::uint32_t id) { return mode == 0 ? IsFlag(man, id) : OwnSaveFlag(id); };
    if (is_set(kItemLedgerSeeded)) {
        return;
    }
    // Once per save, before C2 ever wrote a host flag into it: every ledger lot whose awarding
    // event already ran here was earned by this player itself.
    const std::vector<std::uint32_t> bits = LedgerBitsToSeed(is_set);
    for (const std::uint32_t bit : bits) {
        if (mode == 0) {
            SetFlag(man, bit);
        } else {
            progress::WriteFlagOwnSave(bit, true);
        }
    }
    if (mode == 0) {
        SetFlag(man, kItemLedgerSeeded);
    } else {
        progress::WriteFlagOwnSave(kItemLedgerSeeded, true);
    }
    Log("ledger seeded from the own save (load mode %d): %zu lot%s this player already earned (marker flag %u)", mode,
        bits.size(), bits.size() == 1 ? "" : "s", kItemLedgerSeeded);
}

void OnHostFlagSet(std::uint32_t flag) {
    if (!g_on.load() || Role() != PartyRole::Host || flag > 0x7fffffffu) {
        return;
    }
    std::optional<ItemGrant> g;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        const auto& cap = g_host.Captured();
        if (InstancedTreasureFlag(static_cast<std::int32_t>(flag), progress::InstancedItems(),
                                  cap.count(static_cast<std::int32_t>(flag)) != 0)) {
            Log("host flag %u: map treasure, per player (guests pick up their own)", flag);
            return;
        }
        g = ClassifyFlag(static_cast<std::int32_t>(flag), cap);
    }
    if (g && g->items.empty()) {
        // A treasure with a blood gem: the gem the host picked up a moment ago (its give list).
        const std::vector<LotRowData> chain = LotChain(g->lot);
        if (LotChainHasGem(chain)) {
            const double now = Now();
            for (auto it = g_recent_gem_gives.rbegin(); it != g_recent_gem_gives.rend(); ++it) {
                if (now - it->at > 120.0) {
                    break;
                }
                const LotPlan plan = PlanSummonedLot(chain, it->items, nullptr);
                if (plan.result == LotPlanResult::Give && plan.why == "host's item list") {
                    g->items = it->items;
                    g_recent_gem_gives.erase(std::next(it).base());
                    Log("host flag %u: lot %d carries the blood gem the host picked up", flag, g->lot);
                    break;
                }
            }
        }
    }
    if (g) {
        AttachHostRoll(&*g); // main thread (C2's observer)
        std::lock_guard<std::mutex> lk(g_host_mu);
        queued = g_host.Offer(*g);
    }
    if (queued) {
        Log("host flag %u -> %s queued for the party", flag, DescribeItem(*g).c_str());
        ShareNotice(*g, "host pickup");
    }
}

bool PopHostItems(ItemGrant* out) {
    std::lock_guard<std::mutex> lk(g_host_mu);
    return g_host.Pop(out);
}

void RequestHostFullItems(int slot) {
    if (!g_on.load()) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_host_mu);
    if (std::find(g_full_due.begin(), g_full_due.end(), slot) == g_full_due.end()) {
        g_full_due.push_back(slot);
    }
}

bool TakeHostFullItems(int* slot, std::vector<ItemGrant>* out) {
    if (!g_on.load()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        if (g_full_due.empty()) {
            return false;
        }
    }
    // The host's own save must be live: world up, flag store in mode 0, own world.
    const GameSnapshot gs = ReadGameState(false);
    const u64 man = Ptr(kFlagMan);
    if (!gs.world_up || gs.loading || LoadMode(man) != 0 || OwnWorld() != 1) {
        return false;
    }
    std::vector<ItemGrant> list;
    std::map<std::int32_t, std::int32_t> captured;
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        *slot = g_full_due.front();
        g_full_due.pop_front();
        captured = g_host.Captured();
    }
    std::set<std::int32_t> seen;
    for (const LotFlag& e : Tab().lot_flags) {
        if (!seen.insert(e.flag).second || !IsFlag(man, e.flag) ||
            InstancedTreasureFlag(e.flag, progress::InstancedItems(), captured.count(e.flag) != 0)) {
            continue;
        }
        if (std::optional<ItemGrant> g = ClassifyFlag(e.flag, captured)) {
            g->source = ItemSource::Full;
            list.push_back(*g);
        }
    }
    for (const LedgerRow& r : Tab().ledger) {
        // The awarding event's done flag, or the host's own ledger bit (its own award of the lot,
        // AwardEntry): Iosefka's Blood Vial (ledger 5) comes from common 9100; the common 9040
        // slot with its done flag 9046 is set by no talk script.
        const bool had = IsFlag(man, r.done_flag) || IsFlag(man, std::int64_t(kItemLedgerBase) + r.idx);
        if (had && !ItemLotDenied(r.lot, kItemNone)) {
            ItemGrant g;
            g.lot = r.lot;
            g.ledger = r.idx;
            g.source = ItemSource::Full;
            list.push_back(g);
        }
    }
    for (ItemGrant& g : list) {
        AttachHostRoll(&g);
    }
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        for (ItemGrant& g : list) {
            g.seq = g_host.NextSeq();
        }
    }
    Log("full item list for slot %d: %zu lots", *slot, list.size());
    *out = std::move(list);
    return true;
}

void RequestGuestItems(const std::vector<ItemGrant>& in) {
    if (!g_on.load()) {
        return;
    }
    std::vector<ItemGrant> items;
    items.reserve(in.size());
    for (const ItemGrant& g : in) {
        if (g.source == ItemSource::BossDrop) {
            // 0.7.6: only a guest summoned into the boss's map area takes the host's boss drop (the guests at
            // the kill); its own key (once; no flag is ever written for it).
            static std::uint64_t nb = 0;
            const GameSnapshot q = ReadGameState(false);
            const bool summoned = q.session_role == RoleClient && progress::FlagLoadMode() == 1;
            if (!BossDropAccepted(summoned, q.map_id, g.lot) ||
                !party::status::HostRuleOn(party::status::HostRule::BossDrops)) {
                Log("boss drop lot %d from the host: not taken (not in the boss's area at the kill: map %08x, role %d, "
                    "rule %s)",
                    g.lot, q.map_id, q.session_role,
                    party::status::HostRuleOn(party::status::HostRule::BossDrops) ? "on" : "off");
                continue;
            }
            ItemGrant c = g;
            c.repeat_key = kBossDropKeyBase + static_cast<std::int64_t>(++nb);
            Log("boss drop lot %d from the host (%zu item(s)): queued for this guest (present at the kill)", g.lot,
                g.items.size());
            items.push_back(c);
            continue;
        }
        if (g.source != ItemSource::Chalice) {
            items.push_back(g);
            continue;
        }
        // G1: only a guest in the dungeon now (summoned) takes the host's Chalice award; its own
        // key (once; no flag is ever written for it).
        static std::uint64_t n = 0;
        const GameSnapshot q = ReadGameState(false);
        const bool summoned = q.session_role == RoleClient && progress::FlagLoadMode() == 1;
        if (!ChaliceGrantAccepted(summoned, q.map_id)) {
            Log("chalice award lot %d from the host: not taken (this player is not in the host's dungeon: map %08x, "
                "role %d)",
                g.lot, q.map_id, q.session_role);
            continue;
        }
        ItemGrant c = g;
        c.repeat_key = kChaliceKeyBase + static_cast<std::int64_t>(++n);
        Log("chalice award lot %d from the host (%zu item(s)): queued for this guest (in the dungeon at the kill)",
            g.lot, g.items.size());
        items.push_back(c);
    }
    std::size_t taken, pending;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        taken = g_guest.Offer(items);
        pending = g_guest.Pending();
    }
    Log("guest got %zu grants from the host, %zu new; %zu pending", items.size(), taken, pending);
}

namespace {

/// No HP lost for kCalmAfter s (ChrIns +0x3B0 -> +0x20: +0xF8 HP): not in a fight, so an award
/// popup does not land on a player under attack. A new player (load) starts calm.
/// The local player's HP (ChrIns +0x3B0 -> +0x20: +0xF8), -1 unknown.
std::int32_t PlayerHp(u64 player) {
    u64 a = 0, b = 0;
    std::int32_t hp = -1;
    if (!player || !SafeGet(player + 0x3b0, &a) || !a || !SafeGet(a + 0x20, &b) || !b || !SafeGet(b + 0xf8, &hp)) {
        return -1;
    }
    return hp;
}

/// No boss health bar now and none for kBossBannerQuiet s (the defeat banner runs after it goes).
bool BossQuiet(double now) {
    static bool last = false;
    static double off_at = -1e9;
    const bool shown = BossBarShown();
    if (last && !shown) {
        off_at = now;
    }
    last = shown;
    // A boss killed without its bar on this game (a guest away from the fight, a scripted kill)
    // still shows its defeat banner: an item notice opened under it had no OK row and could not be
    // closed (live pard3 O1, Rom's Kin Coldblood on the guest).
    return !shown && now - off_at >= kBossBannerQuiet && BannerAgeSeconds() >= kBossBannerQuiet;
}

/// The item-get dialog's watchdog (ItemGetWatch): every role, party mode.
/// BB_PARTY_ITEMGET_PROBE: the gates of the action-button registration 0x1CD91F0 the item notice's
/// confirm (0x16D86E0: button 0x1130, category 0xB) goes through, while an entry waits.
void ItemConfirmGateProbe(double now) {
    static double next = 0;
    if (now < next) {
        return;
    }
    next = now + 1.0;
    auto rd = [](u64 a, u64 off) -> u64 {
        u64 v = 0;
        return a && SafeGet(a + off, &v) ? v : 0;
    };
    auto rb = [](u64 a, u64 off) -> int {
        u8 v = 0;
        return a && SafeGet(a + off, &v) ? v : -1;
    };
    auto ri = [](u64 a, u64 off) -> int {
        std::int32_t v = 0;
        return a && SafeGet(a + off, &v) ? v : -1;
    };
    const u64 wcm = rd(Guest(0x553e878), 0), chr = rd(wcm, 0x60), man = rd(Guest(0x5540048), 0),
              menu = rd(Guest(0x5562878), 0);
    u32 f1e1 = 0;
    if (chr) {
        SafeGet(chr + 0x1e1, &f1e1);
    }
    const u64 cnt_ptr = rd(rd(chr, 0x58), 0x248);
    const u64 l15 = rd(rd(rd(rd(rd(chr, 0x3b0), 0x58), 8), 0x58), 0x38);
    const u64 l11 = rd(rd(rd(l15, 8), 8), 0x3b0);
    const u64 l3 = rd(l11, 0x30);
    Log("item confirm gates: man +0x50 %d +0x51 %d +0x52 %d +0x55 %d; menu +0xC0 %d +0x118 %d +0x130 %d; chr +0x1E1 "
        "%#x (0x400 %d); +0x58->+0x248 count %d; L3+900 %d; L11+8->+0x10 %d; L15+0x5D %d",
        rb(man, 0x50), rb(man, 0x51), rb(man, 0x52), rb(man, 0x55), ri(menu, 0xc0), ri(menu, 0x118), ri(menu, 0x130),
        f1e1, (f1e1 & 0x400) ? 1 : 0, cnt_ptr ? ri(cnt_ptr, 0) : -9, rb(l3, 900), rb(rd(l11, 8), 0x10), rb(l15, 0x5d));
}

void ItemGetWatchTick(const GameSnapshot& gs, double now) {
    static ItemGetWatch watch;
    static bool off = [] {
        const char* v = std::getenv("BB_PARTY_ITEMGET_WATCH");
        return v && v[0] == '0';
    }();
    if (off) {
        return;
    }
    const int n = ItemGetEntries();
    // A summoned guest's game holds every action button off (SprjActionButtonMan +0x52 = 1: a
    // phantom never picks anything up in vanilla), and the item notice's confirm is an action
    // button (0x16D86E0 -> 0x1CD91F0 button 0x1130): the box had no OK row and Cross did nothing
    // (live pard3 R3: +0x52 1 in the field, 0 in the Dream where it worked). While an item notice
    // is up the gate is lifted, and put back once it is closed.
    {
        static bool lifted = false;
        static const bool keep = std::getenv("BB_PARTY_ITEMGET_CONFIRM") && std::getenv("BB_PARTY_ITEMGET_CONFIRM")[0] == '0';
        u64 man = 0;
        if (!keep && SafeGet(Guest(0x5540048), &man) && man) {
            u8 gate = 0;
            SafeGet(man + 0x52, &gate);
            if (n > 0 && gs.session_role == 6 && gate == 1) {
                const u8 zero = 0;
                SafeWrite(man + 0x52, &zero, 1);
                if (!lifted) {
                    lifted = true;
                    Log("item notice up on a summoned guest: the action-button hold (+0x52) lifted so its OK confirms it");
                }
            } else if (n <= 0 && lifted) {
                lifted = false;
                // After a boss clear the hold is the boss-clear one, lifted for good (issue #4).
                const bool k10 = b2::K10ActionHoldLifted();
                if (gs.session_role == 6 && gate == 0 && !k10) {
                    const u8 one = 1;
                    SafeWrite(man + 0x52, &one, 1);
                }
                Log("item notice closed: %s", k10 ? "the action-button hold stays lifted (after a boss clear the party stays)"
                                                   : "the action-button hold put back");
            }
        }
    }
    static const bool probe = std::getenv("BB_PARTY_ITEMGET_PROBE") != nullptr;
    if (probe && n > 0) {
        ItemConfirmGateProbe(now);
    }
    const std::int32_t hp = PlayerHp(gs.player);
    const char* why = nullptr;
    if (!watch.Step(now, n, ItemGetConfirmArmed(), hp != 0, gs.loading || !gs.world_up, &why)) {
        return;
    }
    const double up = now - watch.UpSince();
    const int cleared = ClearItemGet();
    Log("item-get dialog cleared (%d entr%s up %.1f s: %s); the items stay in the inventory", cleared,
        cleared == 1 ? "y" : "ies", up, why);
    if (cleared > 0) {
        party::status::SetLastEvent("Item notice closed - the items are in your inventory");
    }
}

bool Calm(u64 player, double now) {
    constexpr double kCalmAfter = 15.0;
    static u64 last_player = 0;
    static std::int32_t last_hp = -1;
    static double last_hit = -1e9;
    u64 a = 0, b = 0;
    std::int32_t hp = -1;
    if (!player || !SafeGet(player + 0x3b0, &a) || !a || !SafeGet(a + 0x20, &b) || !b || !SafeGet(b + 0xf8, &hp)) {
        return true;
    }
    if (player == last_player && last_hp >= 0 && hp < last_hp) {
        last_hit = now;
    }
    last_player = player;
    last_hp = hp;
    return now - last_hit >= kCalmAfter;
}

std::atomic<u64> g_hud_obj{0}; // BB_PARTY_ITEMGET_PROBE: the HUD object 0x1BFB2A0 last ran with

BB_COOP_SYSV void HudTickEntry(u64 hud, u64, u64, u64, u64, u64) {
    g_hud_obj = hud;
}

// 0x1C02290 (functor, Scaleform list item, const u32* index): one GetItemList row from the HUD model.
std::atomic<u32> g_row_calls{0}, g_row_max_index{0};
std::atomic<u64> g_hud_update_calls{0};
BB_COOP_SYSV void ItemRowEntry(u64, u64, u64 index_ptr, u64, u64, u64) {
    u32 idx = 0;
    SafeGet(index_ptr, &idx);
    g_row_calls.fetch_add(1);
    if (idx > g_row_max_index.load()) {
        g_row_max_index = idx;
    }
}
BB_COOP_SYSV void HudUpdateEntry(u64, u64, u64, u64, u64, u64) {
    g_hud_update_calls.fetch_add(1);
}

// 0x1F5E200 (out, menu res, flags, const wchar16* name): loads menu:/<name>.gfx. Probe: who asks
// for an empty name.
using MovieLoadFn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64);
MovieLoadFn g_movie_load_orig = nullptr;
BB_COOP_SYSV u64 MovieLoadReplace(u64 out, u64 res, u64 flags, u64 name) {
    const u64 ret = reinterpret_cast<u64>(__builtin_return_address(0));
    std::uint16_t first = 0;
    if (!name || !SafeGet(name, &first) || first == 0) {
        Log("item-get probe: menu movie with an EMPTY name (flags 0x%llx, name ptr 0x%llx) from guest +0x%llx",
            static_cast<unsigned long long>(flags), static_cast<unsigned long long>(name),
            static_cast<unsigned long long>(ret - Guest(0)));
    }
    return g_movie_load_orig(out, res, flags, name);
}

/// BB_PARTY_ITEMGET_PROBE=1: logs every change of the item-get dialog's state - entries in the
/// list at *0x553B1C0 (+8), the "dialog up" word *0x5562878 +0x130 that 0x16D86E0 sets while the
/// list is not empty, and the gate of its confirm check (player flag WorldChrMan+0x60 -> +0x58 ->
/// +0x38 -> +0x41 bit 7, *0x5540050).
void ItemGetProbe() {
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_ITEMGET_PROBE");
        return v && *v && *v != '0';
    }();
    if (!on) {
        return;
    }
    static int last_n = -1, last_up = -1, last_gate = -1;
    static double since = 0;
    const u64 list = Ptr(kItemGetList);
    int n = 0;
    u64 node = 0;
    if (list && SafeGet(list + 8, &node)) {
        while (node && n < 16) {
            ++n;
            if (!SafeGet(node, &node)) {
                break;
            }
        }
    }
    u32 up = 0;
    const u64 menu = Ptr(kMenuState);
    if (menu) {
        SafeGet(menu + 0x130, &up);
    }
    u64 a = Ptr(0x553e878), b = 0, c = 0;
    u8 flag = 0;
    const bool chr = a && SafeGet(a + 0x60, &b) && b && SafeGet(b + 0x58, &c) && c && SafeGet(c + 0x38, &c) && c &&
                     SafeGet(c + 0x41, &flag);
    const int gate = (chr && (flag & 0x80)) ? 0 : (Ptr(0x5540050) ? 1 : 0);
    // The HUD model's item-get rows (0x1BFB2A0 copies the list into HUD +0xA88 -> +0x1238, count
    // +0x13A8, up to 5 a frame).
    static int last_rows = -2;
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        HookPrologue(0x1bfb2a0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                     &HudTickEntry, "HUD tick (item-get probe)");
        HookPrologue(0x1c02290, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                     &ItemRowEntry, "GetItemList row (item-get probe)");
        HookPrologue(0x1bf7c20, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                     &HudUpdateEntry, "ingameTop update (item-get probe)");
        void* orig = nullptr;
        if (ReplacePrologue(0x1f5e200, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                            reinterpret_cast<const void*>(&MovieLoadReplace), &orig, "menu movie load (item-get probe)")) {
            g_movie_load_orig = reinterpret_cast<MovieLoadFn>(orig);
        }
    }
    // Once: the menu movie tables 0x1F5D0E0 loads from (name pointers; empty names fail to load).
    static bool tables_dumped = false;
    if (!tables_dumped && Ptr(kMenuState) && g_hud_obj.load()) {
        tables_dumped = true;
        struct T {
            u64 base;
            int n;
            int stride;
            int name_off;
        };
        const T tabs[] = {{0x5595e30, 0x4f, 16, 8}, {0x5595cc0, 0x16, 16, 8}, {0x5356ad0, 9, 8, 0}, {0x5356aa0, 6, 8, 0}};
        for (const T& t : tabs) {
            std::string line;
            for (int i = 0; i < t.n; ++i) {
                u64 first = 0, name = 0;
                SafeGet(Guest(t.base + u64(i) * t.stride), &first);
                SafeGet(Guest(t.base + u64(i) * t.stride + t.name_off), &name);
                char s[48] = {};
                if (name) {
                    SafeRead(name, s, sizeof s - 1);
                }
                char b[96];
                std::snprintf(b, sizeof b, " [%d 0x%llx '%s']", i, static_cast<unsigned long long>(first), s);
                line += b;
            }
            Log("item-get probe: movie table 0x%llx:%s", static_cast<unsigned long long>(t.base), line.c_str());
        }
        // The GetItemList row functor's vtable (0x1BF7C20: local functor &0x5344970 over HUD rows).
        std::string vt;
        for (int i = 0; i < 8; ++i) {
            u64 f = 0;
            SafeGet(Guest(0x5344970 + 8ull * i), &f);
            char b[32];
            std::snprintf(b, sizeof b, " +0x%llx", static_cast<unsigned long long>(f ? f - Guest(0) : 0));
            vt += b;
        }
        Log("item-get probe: GetItemList functor vtable 0x5344970:%s", vt.c_str());
    }
    int rows = -1;
    u64 model = 0, cnt = 0;
    if (const u64 hud = g_hud_obj.load(); hud && SafeGet(hud + 0xa88, &model) && model && SafeGet(model + 0x13a8, &cnt)) {
        rows = static_cast<int>(cnt);
    }
    // 0x1BFB2A0's open edge (rows 0 -> n): *0x5578A38 < 1, menu +0x84 == 0, GSM +0x1544 == 0
    // -> menu +0x25C = 1 (and a save request).
    std::int32_t blk = -1, m84 = -1, m25c = -1;
    u8 g1544 = 0xff;
    SafeGet(Guest(0x5578a38), &blk);
    if (menu) {
        SafeGet(menu + 0x84, &m84);
        SafeGet(menu + 0x25c, &m25c);
    }
    if (const u64 gsm = Ptr(0x5556678)) {
        SafeGet(gsm + 0x1544, &g1544);
    }
    static std::int32_t last_blk = -2, last_m84 = -2, last_m25c = -2;
    static int last_g1544 = -2;
    static double last_rowlog = 0;
    if (Now() - last_rowlog >= 5.0) {
        last_rowlog = Now();
        Log("item-get probe: ingameTop updates %llu, GetItemList row calls %u (max index %u)",
            static_cast<unsigned long long>(g_hud_update_calls.load()), g_row_calls.load(), g_row_max_index.load());
    }
    if (rows != last_rows || blk != last_blk || m84 != last_m84 || m25c != last_m25c || g1544 != last_g1544) {
        Log("item-get probe: HUD model rows %d (HUD 0x%llx); open gate *0x5578A38 %d, menu+0x84 %d, GSM+0x1544 %u, "
            "menu+0x25C %d",
            rows, static_cast<unsigned long long>(g_hud_obj.load()), blk, m84, g1544, m25c);
        last_rows = rows;
        last_blk = blk;
        last_m84 = m84;
        last_m25c = m25c;
        last_g1544 = g1544;
    }
    if (n != last_n || static_cast<int>(up) != last_up || gate != last_gate) {
        const double now = Now();
        Log("item-get probe: %d entr%s in the dialog list, dialog %s, confirm check %s (%.2f s since the last change)", n,
            n == 1 ? "y" : "ies", up ? "UP" : "down", gate ? "armed" : "held (player busy / no action-button man)",
            now - since);
        since = now;
        last_n = n;
        last_up = static_cast<int>(up);
        last_gate = gate;
    }
}

} // namespace

namespace {

/// Main thread: the guest's gift test and the host's gift awards.
void GiftTick(double now) {
    const PartyRole role = Role();
    const int own = OwnWorld();
    if (g_gift_test > 0 && !g_gift_test_done && role == PartyRole::Guest) {
        const GameSnapshot q = ReadGameState(false);
        if (own != 0 || !q.world_up || q.loading || LoadMode(Ptr(kFlagMan)) != 1) {
            g_gift_test_since = 0;
        } else if (!g_gift_test_since) {
            g_gift_test_since = now;
        } else if (now - g_gift_test_since >= 20.0 && Ptr(kMapItemMan)) {
            g_gift_test_done = true;
            // BB_PARTY_GIFT_TEST_HOSTONLY=1: hostOnly 1, as the Doll / Messenger talk awards its
            // per-player gifts (the Old Hunter Bell, lot 10050).
            const char* ho = std::getenv("BB_PARTY_GIFT_TEST_HOSTONLY");
            const u32 host_only = ho && ho[0] == '1' ? 1 : 0;
            if (LotHasKeyGoods(LotSlots(g_gift_test))) {
                Log("gift test: lot %d holds a key item / unique goods: not awarded while summoned", g_gift_test);
                return;
            }
            Log("gift test: awarding lot %d through 0x17DDC50 (hostOnly %u) in the host's world", g_gift_test, host_only);
            reinterpret_cast<AwardFn>(Guest(kAwardLot))(Ptr(kMapItemMan), static_cast<u32>(g_gift_test), host_only);
        }
    }
    {   // BB_PARTY_GUEST_PICKUP_TEST=<lot>,...: the guest "picks up" shared treasure in the host's world (0.7.7 smoke
        // aid): what the D4 pickup path does (the flag into the own save, ReportGuestKeyTreasure), 20 s after its world is steady.
        static const std::vector<long> lots = EnvList("BB_PARTY_GUEST_PICKUP_TEST");
        static double since = 0;
        static bool done = false;
        if (!lots.empty() && !done && role == PartyRole::Guest) {
            const GameSnapshot q = ReadGameState(false);
            if (own != 0 || !q.world_up || q.loading || LoadMode(Ptr(kFlagMan)) != 1) {
                since = 0;
            } else if (!since) {
                since = now;
            } else if (now - since >= 20.0) {
                done = true;
                for (const long lot : lots) {
                    const std::int32_t flag = ItemLotFlag(static_cast<std::int32_t>(lot));
                    const bool own_ok = flag > 0 && progress::WriteFlagOwnSave(static_cast<std::uint32_t>(flag), true);
                    const bool sent = flag > 0 && ReportGuestKeyTreasure(flag);
                    Log("guest pickup test: lot %ld flag %d: own save %s, report to the host %s", lot, flag,
                        own_ok ? "set" : "NOT set", sent ? "sent" : "refused (not shared / not allowed)");
                }
            }
        }
    }
    if (role != PartyRole::Host || own != 1 || now - g_gift_last < 2.0) {
        return;
    }
    GiftDue d;
    {
        std::lock_guard<std::mutex> lk(g_gift_mu);
        if (g_gift_due.empty()) {
            return;
        }
        d = g_gift_due.front();
    }
    const GameSnapshot q = ReadGameState(false);
    const u64 mim = Ptr(kMapItemMan);
    if (!q.world_up || q.loading || !mim || LoadMode(Ptr(kFlagMan)) != 0) {
        return;  // keep it until the host's own world is steady
    }
    if (split::DialogUp()) {
        return;  // the award's item box would open under a game message (parity67 D4): wait
    }
    {
        std::lock_guard<std::mutex> lk(g_gift_mu);
        g_gift_due.pop_front();
    }
    g_gift_last = now;
    static const bool hide_on = !EnvOff("BB_PARTY_GIFT_HIDE");
    // One award per lot per session (BB_PARTY_GIFT_HIDE=0 turns this and the hide off).
    static std::set<std::int32_t> awarded;
    if (hide_on && awarded.count(d.lot)) {
        Log("gift from slot %d: lot %d was already awarded this session; nothing to give", d.slot, d.lot);
        return;
    }
    if (IsFlag(Ptr(kFlagMan), d.flag)) {
        Log("gift from slot %d: lot %d already in the host's world (flag %d set); nothing to give", d.slot, d.lot, d.flag);
        if (hide_on) {
            if (const std::uint32_t tf = TreasureFlagToHide(d.lot); tf && g_treasure_hider && g_treasure_hider(tf)) {
                Log("gift from slot %d: lot %d's leftover treasure (flag %u) taken out", d.slot, d.lot, tf);
            }
        }
        return;
    }
    {
        std::string why;
        const LotFit fit = ClassifyLotFit(LotSlots(d.lot), &why);
        if (fit != LotFit::Give) {
            Log("gift from slot %d: lot %d not awarded in the host's world: %s (%s)", d.slot, d.lot, LotFitName(fit),
                why.c_str());
            return;
        }
    }
    Log("gift from slot %d: lot %d (flag %d) awarded in the host's world; C3 hands it to the party", d.slot, d.lot, d.flag);
    // The game's own award (hostOnly 1, as EMEVD 2003[4]): the award hook captures it and C3
    // broadcasts it; the guest that talked holds the flag already (its mark) and skips it.
    reinterpret_cast<AwardFn>(Guest(kAwardLot))(mim, static_cast<u32>(d.lot), 1);
    Log("gift from slot %d: flag %d is now %d", d.slot, d.flag, IsFlag(Ptr(kFlagMan), d.flag) ? 1 : 0);
    if (hide_on) {
        awarded.insert(d.lot);
        // The treasure still lying in the host's own world: taken out, else the host picks it up a second
        // time (a duplicate key item and the "second item" warning).
        if (const std::uint32_t tf = TreasureFlagToHide(d.lot); tf && g_treasure_hider && g_treasure_hider(tf)) {
            Log("gift from slot %d: lot %d's treasure (flag %u) taken out of the host's world", d.slot, d.lot, tf);
        }
    }
}

} // namespace

bool OfferRepeatGift(std::int32_t lot) {
    if (!g_on.load() || EnvOff("BB_PARTY_ITEMS")) {
        return false;
    }
    static std::uint64_t n = 0;
    const std::optional<ItemGrant> g = MakeRepeatGrant(lot, ++n);
    if (!g) {
        return false;
    }
    std::size_t taken = 0;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        taken = g_guest.Offer({*g});
    }
    Log("guest's own repeatable gift lot %d (#%llu this session): %s", lot, static_cast<unsigned long long>(n),
        taken ? "queued (given while summoned, or at home)" : "queue full");
    if (SharedRepeatGiftLot(lot)) {
        // 0.7.3: Iosefka's Blood Vial goes to the whole party (all progress shared): the talk moved
        // her on in the host's world too (72410301, npc_quest), so nobody else could get one from
        // her. The host's world awards it (once: the host's ledger bit) and C3 hands it on; this
        // guest holds its own copy by then (unique goods: "owned", skipped).
        const std::int32_t bit = static_cast<std::int32_t>(kItemLedgerBase) + ItemLedgerIndex(lot);
        std::lock_guard<std::mutex> lk(g_gift_mu);
        if (GuestGiftShareable(lot, bit) && g_gift_up.size() < 64) {
            g_gift_up.push_back(GiftUpToJsonText(lot, bit));
            Log("gift: lot %d (ledger flag %d) reported to the host (shared with the party)", lot, bit);
        }
    }
    return taken != 0;
}

bool OfferOwnGift(std::int32_t lot) {
    if (!g_on.load() || EnvOff("BB_PARTY_ITEMS") || !OwnTalkGiftLotId(lot)) {
        return false;
    }
    ItemGrant g;
    g.lot = lot;
    g.flag = ItemLotFlag(lot);
    g.source = ItemSource::Own;
    if (g.flag <= 0) {
        return false;
    }
    std::size_t taken = 0;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        taken = g_guest.Offer({g});
    }
    Log("guest's own talk gift lot %d (flag %d): %s", lot, g.flag,
        taken ? "queued (given while summoned, or at home)" : "already queued or given");
    return taken != 0;
}

bool AwardLotGuestOnly(std::uint32_t lot) {
    g_award_no_share = true;
    const bool ok = AwardItemLot(lot);
    g_award_no_share = false;
    return ok;
}

bool ReportGuestKeyTreasure(std::int32_t flag) {
    const std::int32_t lot = ItemLotForFlag(flag);
    if (!g_on.load() || lot <= 0 || !SharedKeyTreasureLot(lot) || !GuestGiftShareable(lot, flag)) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_gift_mu);
    if (g_gift_up.size() >= 64) {
        return false;
    }
    g_gift_up.push_back(GiftUpToJsonText(lot, flag));
    Log("gift: shared key treasure lot %d (flag %d) picked up by this guest: reported to the host (its world "
        "awards it, C3 hands it to every member)", lot, flag);
    ItemGrant picked;
    picked.lot = lot;
    picked.flag = flag;
    picked.source = ItemSource::Flag;
    ShareNotice(picked, "picked up here");
    return true;
}

bool PopGuestGift(std::string* body) {
    std::lock_guard<std::mutex> lk(g_gift_mu);
    if (g_gift_up.empty()) {
        return false;
    }
    *body = std::move(g_gift_up.front());
    g_gift_up.pop_front();
    return true;
}

bool OnHostGiftUp(int slot, const std::string& body, std::string* why) {
    std::int32_t lot = 0, flag = 0;
    if (!GiftUpFromJsonText(body, &lot, &flag, why)) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_gift_mu);
    if (!g_gift_rate.Take(slot, Now())) {
        if (why) {
            *why = "gift_up: rate limit";
        }
        return false;
    }
    if (g_gift_due.size() >= 32) {
        if (why) {
            *why = "gift_up: queue full";
        }
        return false;
    }
    g_gift_due.push_back({slot, lot, flag});
    Log("gift from slot %d: lot %d (flag %d) queued for the host's world", slot, lot, flag);
    return true;
}

/// H3 diagnostic (par_c6): the unique key goods of the ledger lots (Oedon Tomb Key, badges, ...):
/// their held counts are logged once in each world and every change after that, with the state
/// (map, load mode, session), so a key that vanishes from a save shows where (live: a guest's
/// Oedon Tomb Key given while summoned was gone after its next restart while its done flag stayed).
void KeyGoodsWatchTick(const GameSnapshot& gs, int load_mode, double now) {
    static double next = 0;
    static std::map<std::int32_t, int> last;
    static bool baseline = false;
    if (!gs.world_up || gs.loading || gs.map_id == 0xffffffffu || !gs.player_rec) {
        baseline = false;  // a load: the next world logs its baseline again
        next = now + 3.0;
        return;
    }
    if (now < next) {
        return;
    }
    next = now + 5.0;
    std::map<std::int32_t, int> held;
    std::map<std::int32_t, bool> regive;  // KeyRegiveAllowed per goods id
    auto add = [&](std::int32_t lot) {
        for (const LotSlotState& sl : LotSlots(lot)) {
            if (sl.category == 4 && sl.max_num == 1 && sl.max_storage <= 0 && sl.owned >= 0) {
                held[static_cast<std::int32_t>(sl.id)] = sl.owned;
                regive[static_cast<std::int32_t>(sl.id)] = KeyRegiveAllowed(sl);
            }
        }
    };
    for (const LedgerRow& r : Tab().ledger) {
        add(r.lot);
    }
    for (const SharedTreasure& t : Tab().shared_treasure) {
        add(t.lot);
    }
    if (held.empty()) {
        return;
    }
    std::string text;
    for (const auto& [id, n] : held) {
        const auto it = last.find(id);
        if (!baseline) {
            text += (text.empty() ? "" : ", ") + std::to_string(id) + " x" + std::to_string(n);
        } else if (it != last.end() && it->second != n) {
            text += (text.empty() ? "" : ", ") + std::to_string(id) + " " + std::to_string(it->second) + " -> " +
                    std::to_string(n);
            if (load_mode == 1 && n > it->second && regive[id] && !SummonedKeyViewActive()) {
                // The game's summoned key view (BB_PARTY_KEY_VIEW=0): a key item that reached this
                // guest in the host's world by any way (an NPC's gift such as the Tonsil Stone, a
                // pickup; playtest logs: 4310 0 -> 1, gone after the next load) is dropped with that
                // view at the next load, so it is kept for the own-world check like a party give
                // (count bounded by the held count right after it: a party give noted twice is still
                // one). With the own key section shown it lands there and stays.
                NoteSummonedGive(0x40000000u, static_cast<u32>(id), n - it->second, n);
                Log("key item %d reached this guest while summoned: kept for the own-world check", id);
            }
        }
    }
    if (!baseline || !text.empty()) {
        // A unique item in two stacks (the save lists it twice): never expected (smoke_party.py checks it).
        for (const auto& [id, n] : held) {
            const int stacks = n > 0 ? ItemStacks(0x40000000u, static_cast<u32>(id)) : 0;
            if (stacks > 1) {
                Log("key goods %d: DUPLICATE STACK (%d entries in the inventory)", id, stacks);
            }
        }
    }
    if (!text.empty()) {
        Log("key goods %s: %s (%s, load mode %d%s)", baseline ? "changed" : "held", text.c_str(), Describe(gs).c_str(),
            load_mode,
            load_mode != 1                ? ""
            : SummonedKeyViewActive()     ? "; summoned: the own key section is shown"
                                          : "; summoned: key items (goodsType 1) are not shown here, the own save keeps them");
    }
    last = std::move(held);
    baseline = true;
}

// Main thread, own world (load mode 0), world settled: once per save (marker kShopGatesRepaired), clears
// every weapon shop-gate flag (5100-5282, 5500-5521) that is set although the player owns no such weapon (inventory and
// storage box, any upgrade level). 0.7.4-era sessions wrote them into the own save of a summoned guest.
// BB_PARTY_SHOP_REPAIR=0 switches it off.
void RepairShopGatesTick(const GameSnapshot& gs) {
    static double next = 0.0, up_since = 0.0;
    static bool done = false, logged_wait = false;
    static bool off = std::getenv("BB_PARTY_SHOP_REPAIR") && std::string(std::getenv("BB_PARTY_SHOP_REPAIR")) == "0";
    if (!g_on.load() || off || done) {
        return;
    }
    const double now = Now();
    const u64 man = Ptr(kFlagMan);
    // The own world only (load mode 0: the live store is the own save), the world up and settled so the
    // inventory and the storage are loaded.
    if (!man || LoadMode(man) != 0 || !gs.world_up || gs.loading || gs.map_id == 0xffffffffu || gs.player_rec == 0) {
        up_since = 0.0;
        return;
    }
    if (up_since == 0.0) {
        up_since = now;
    }
    if (now - up_since < 8.0 || now < next) {
        return;
    }
    next = now + 2.0;
    auto is_set = [man](std::uint32_t id) { return IsFlag(man, id); };
    auto owns = [](std::int32_t base) {
        bool unknown = false;
        for (int lvl = 0; lvl <= kShopWeaponMaxLevel; ++lvl) {
            const u32 id = static_cast<u32>(base + 100 * lvl);
            const int inv = ItemCount(0, id), box = StorageItemCount(0, id);
            if (inv > 0 || box > 0) {
                return 1;
            }
            unknown = unknown || inv < 0 || box < 0;
        }
        return unknown ? -1 : 0;
    };
    const ShopGateRepair r = ShopGatesToRepair(is_set, owns);
    if (r.done) {
        done = true;
        return;
    }
    std::string cleared;
    for (const std::int32_t f : r.clear) {
        SetFlag(man, f, false);
        cleared += (cleared.empty() ? "" : ",") + std::to_string(f) + "(" + std::to_string(ShopGateWeapon(f)) + ")";
    }
    if (!r.complete) {
        if (!logged_wait) {
            logged_wait = true;
            Log("shop gate repair: the inventory, the storage or the game-data tables are not readable yet, trying again");
        }
        return;
    }
    SetFlag(man, kShopGatesRepaired);
    done = true;
    std::string kept;
    for (const gamedata::ShopGateData& row : Tab().shop_gates) {
        if (IsFlag(man, static_cast<u32>(row.flag))) {
            kept += (kept.empty() ? "" : ",") + std::to_string(row.flag);
        }
    }
    Log("shop gate repair: %zu 'owns the weapon' flag%s set without the weapon cleared%s%s; still set (weapon owned): %s; "
        "save marked (flag %u)",
        r.clear.size(), r.clear.size() == 1 ? "" : "s", cleared.empty() ? "" : ": ", cleared.c_str(),
        kept.empty() ? "none" : kept.c_str(), kShopGatesRepaired);
}

// Goods held anywhere in this player's data: the inventory (the key section in view), the key section out of
// view (ItemStacks counts both) and the storage box. 1 / 0 / -1 unknown (a list not readable).
int GoodsHeldAnywhere(std::int32_t goods) {
    const u32 id = static_cast<u32>(goods);
    const int inv = ItemCount(0x40000000u, id), stacks = ItemStacks(0x40000000u, id);
    const int box = StorageItemCount(0x40000000u, id);
    if (inv > 0 || stacks > 0 || box > 0) {
        return 1;
    }
    return inv < 0 || stacks < 0 || box < 0 ? -1 : 0;
}

const char* BadgeName(std::int32_t goods) {
    static const char* const kNames[kBadgeGateCount] = {
        "Saw Hunter Badge",   "Crow Hunter Badge", "Powder Keg Hunter Badge", "Old Hunter Badge",   "Sword Hunter Badge",
        "Radiant Sword Hunter Badge", "Wheel Hunter Badge", "Cainhurst Badge", "Spark Hunter Badge", "Cosmic Eye Watch Badge"};
    const std::int32_t n = goods - kBadgeGoodsFirst;
    return n >= 0 && n < kBadgeGateCount ? kNames[n] : "badge";
}

// Main thread, own world (load mode 0), world settled: once per save (marker kBadgeGatesRepaired), clears
// every badge lineup flag 12101000 + n that is set while badge 4110 + n is held nowhere. Before 0.7.8 another
// player's copy of m21 event 12101000 ended this game's copy (packet 0x31) and the flags were shared as NPC
// quest progress: the Messenger sold Chikage, the Beak Mask etc. to players without the badge (playtest
// 2026-11-01). BB_PARTY_SHOP_REPAIR=0 switches it off with the weapon repair.
void RepairBadgeGatesTick(const GameSnapshot& gs) {
    static double next = 0.0, up_since = 0.0;
    static bool done = false, logged_wait = false;
    static bool off = std::getenv("BB_PARTY_SHOP_REPAIR") && std::string(std::getenv("BB_PARTY_SHOP_REPAIR")) == "0";
    if (!g_on.load() || off || done) {
        return;
    }
    const double now = Now();
    const u64 man = Ptr(kFlagMan);
    if (!man || LoadMode(man) != 0 || !gs.world_up || gs.loading || gs.map_id == 0xffffffffu || gs.player_rec == 0) {
        up_since = 0.0;
        return;
    }
    if (up_since == 0.0) {
        up_since = now;
    }
    if (now - up_since < 8.0 || now < next) {
        return;
    }
    next = now + 2.0;
    auto is_set = [man](std::uint32_t id) { return IsFlag(man, id); };
    const ShopGateRepair r = BadgeGatesToRepair(is_set, GoodsHeldAnywhere);
    if (r.done) {
        done = true;
        return;
    }
    if (!r.complete) {
        // Nothing is cleared while one list is unreadable: the whole check runs again.
        if (!logged_wait) {
            logged_wait = true;
            Log("shop gate repair: the key items or the storage are not readable yet, trying again (badge rows)");
        }
        return;
    }
    for (const std::int32_t f : r.clear) {
        SetFlag(man, f, false);
        const std::int32_t goods = kBadgeGoodsFirst + (f - kBadgeGateFirst);
        Log("shop gate repair: flag %d cleared (%s %d not held)", f, BadgeName(goods), goods);
    }
    SetFlag(man, kBadgeGatesRepaired);
    done = true;
    std::string kept;
    for (std::int32_t n = 0; n < kBadgeGateCount; ++n) {
        if (IsFlag(man, kBadgeGateFirst + n)) {
            kept += (kept.empty() ? "" : ",") + std::to_string(kBadgeGateFirst + n);
        }
    }
    Log("shop gate repair: badge rows: %zu flag%s cleared; still set (badge held): %s; save marked (flag %u)",
        r.clear.size(), r.clear.size() == 1 ? "" : "s", kept.empty() ? "none" : kept.c_str(), kBadgeGatesRepaired);
    if (!r.clear.empty()) {
        RequestGameSave("shop gate repair: badge rows");
    }
}

// Main thread, 0.7.8 (BB_PARTY_BADGE_GUARD=0 off): the live guard of the badge rows. The playtest of 2026-11-01
// sold Tonitrus (row 12101008) to players who never held the Spark Hunter Badge and no log shows who set the
// flag. Every 3 s, in the own world (load mode 0) and while summoned (load mode 1: the overlay the guest's shop
// reads AND its own save's pool), a row 12101000 + n that is open while badge 4110 + n is in neither key section
// nor the storage box (twice in a row) is closed, with one log line that names the latest writers the hooks saw
// (progress::BadgeRowWriters). The Eye sale flags have their own repair and are left alone.
void BadgeGuardTick(const GameSnapshot& gs, int load_mode, double now) {
    static double next = 0.0, up_since = 0.0;
    static BadgeGuardState st;
    static int lines = 0;
    static const bool off = std::getenv("BB_PARTY_BADGE_GUARD") && std::string(std::getenv("BB_PARTY_BADGE_GUARD")) == "0";
    if (!g_on.load() || off) {
        return;
    }
    const u64 man = Ptr(kFlagMan);
    const bool summoned = load_mode == 1;
    if (!man || (load_mode != 0 && !summoned) || !gs.world_up || gs.loading || gs.map_id == 0xffffffffu ||
        gs.player_rec == 0) {
        up_since = 0.0;
        st = BadgeGuardState();
        return;
    }
    if (up_since == 0.0) {
        up_since = now;
    }
    if (now - up_since < 8.0 || now < next) {
        return;
    }
    next = now + 3.0;
    // What the player's shop reads: the visible tree (own world: the live store; summoned: the host-world overlay);
    // summoned also the own pool, which ShopGateTick copies back into the overlay.
    auto own_open = [&](std::int32_t f) {
        bool v = false;
        return summoned && progress::ReadFlagOwnSave(static_cast<u32>(f), &v) && v;
    };
    auto open = [&](std::int32_t f) { return IsFlag(man, f) || own_open(f); };
    const std::vector<std::int32_t> close = BadgeGuardStep(st, open, GoodsHeldAnywhere);
    for (const std::int32_t f : close) {
        const std::int32_t goods = kBadgeGoodsFirst + (f - kBadgeGateFirst);
        const std::string who = progress::BadgeRowWriters(static_cast<u32>(f));
        const bool pool = own_open(f);
        if (IsFlag(man, f)) {
            progress::FlagCause cause("badge guard");
            if (summoned) {
                progress::WriteFlagVisible(static_cast<u32>(f), false);
            } else {
                SetFlag(man, f, false);
            }
        }
        if (pool) {
            progress::FlagCause cause("badge guard");
            progress::WriteFlagOwnSave(static_cast<u32>(f), false);
        }
        if (lines < 60) {
            ++lines;
            Log("badge row %d was open without badge %d: closed (was set by: %s) [%s, %s]", f, goods,
                who.c_str(), BadgeName(goods), summoned ? "summoned, overlay and own save" : "own world");
        }
    }
    if (!close.empty() && !summoned) {
        RequestGameSave("badge guard");
    }
}

// Main thread, own world, world settled: once per save (marker kEyeGateRepaired), an NG0 save that holds the
// Eye of a Blood-drunk Hunter (or its pickup / sale flags) without night 9801 and never entered the DLC loses
// them: the vanilla Messenger gives and sells the Eye only at night (m21 12101028 / 12101100), so the party
// put it there (before 0.7.8 lot 10040 was replayed to every member at once). BB_PARTY_EYE_REPAIR=0 off.
void RepairEyeGateTick(const GameSnapshot& gs) {
    static double next = 0.0, up_since = 0.0;
    static bool done = false;
    static const char* logged_wait = nullptr;
    static bool off = std::getenv("BB_PARTY_EYE_REPAIR") && std::string(std::getenv("BB_PARTY_EYE_REPAIR")) == "0";
    if (!g_on.load() || off || done) {
        return;
    }
    const double now = Now();
    const u64 man = Ptr(kFlagMan);
    if (!man || LoadMode(man) != 0 || !gs.world_up || gs.loading || gs.map_id == 0xffffffffu || gs.player_rec == 0) {
        up_since = 0.0;
        return;
    }
    if (up_since == 0.0) {
        up_since = now;
    }
    if (now - up_since < 8.0 || now < next) {
        return;
    }
    next = now + 2.0;
    auto is_set = [man](std::uint32_t id) { return IsFlag(man, id); };
    const EyeRepair r = EyeToRepair(is_set, NgCycleLive(), GoodsHeldAnywhere(kEyeGoods));
    if (r.done) {
        done = true;
        return;
    }
    if (!r.complete) {
        if (logged_wait != r.why) {
            logged_wait = r.why;
            Log("eye repair: %s, trying again", r.why);
        }
        return;
    }
    if (r.remove_eye) {
        // At home the key section in view is the own one; GiveItemDirect with a negative count takes it.
        const u32 eye = static_cast<u32>(kEyeGoods);
        const int before = ItemStacks(0x40000000u, eye);
        const int inv = ItemCount(0x40000000u, eye);
        const bool ok = inv <= 0 || GiveItem(0x40000000u, eye, -inv);
        const int after = ItemStacks(0x40000000u, eye);
        const int box = StorageItemCount(0x40000000u, eye);
        Log("eye repair: Eye of a Blood-drunk Hunter %d removed (%s): key stacks %d -> %d%s", kEyeGoods, r.why, before,
            after, !ok ? " (GiveItemDirect unavailable)" : box > 0 ? "; one more in the storage box, left there" : "");
        if (!ok || after != 0) {
            return;  // not taken: the flags stay as they are, the check runs again
        }
    }
    for (const std::uint32_t f : r.clear) {
        SetFlag(man, f, false);
        Log("eye repair: flag %u cleared (%s)", f,
            f == static_cast<std::uint32_t>(kEyeLotFlag) ? "the Messenger's Eye gift, lot 10040"
            : f == kFlagEyeSale                            ? "the Eye's shop sale"
                                                           : "the Eye's sale event");
    }
    SetFlag(man, kEyeGateRepaired);
    done = true;
    Log("eye repair: %s: %s; save marked (flag %u)", r.remove_eye || !r.clear.empty() ? "repaired" : "nothing to do",
        r.why, kEyeGateRepaired);
    if (r.remove_eye || !r.clear.empty()) {
        RequestGameSave("eye repair");
    }
}

// Main thread, own world, world settled: once per save (marker kSgiveRepaired), a character the old replay of
// party_summoned_gives.txt poisoned (CL-55cea7: another character's list was given to a new one) loses the key
// items its own save never earned: a hunter badge without its source flag (lot pickup flag / ledger bit / quest
// flag, GoodsEarnedFlags) and, with at least two such items, the other key items too, plus the shop rows
// 12101000 + n of removed badges. An item whose source is unknown is kept. BB_PARTY_SGIVE_OWNER=0 switches it off.
void RepairSgivePoisonTick(const GameSnapshot& gs) {
    static double next = 0.0, up_since = 0.0;
    static bool done = false, logged_wait = false;
    if (!g_on.load() || !SgiveOwnerOn() || done) {
        return;
    }
    const double now = Now();
    const u64 man = Ptr(kFlagMan);
    if (!man || LoadMode(man) != 0 || !gs.world_up || gs.loading || gs.map_id == 0xffffffffu || gs.player_rec == 0 ||
        OwnWorld() != 1) {
        up_since = 0.0;
        return;
    }
    if (up_since == 0.0) {
        up_since = now;
    }
    if (now - up_since < 9.0 || now < next) {
        return;
    }
    next = now + 2.0;
    auto is_set = [man](std::uint32_t id) { return IsFlag(man, id); };
    const SgiveRepair r = SgivePoisonToRepair(is_set, GoodsHeldAnywhere, NgCycleLive());
    if (r.done) {
        done = true;
        return;
    }
    if (!r.complete) {
        if (!logged_wait) {
            logged_wait = true;
            Log("key item repair: the key items, the storage or the NG cycle are not readable yet, trying again");
        }
        return;
    }
    bool failed = false;
    for (const std::int32_t goods : r.remove) {
        const u32 id = static_cast<u32>(goods);
        const int before = ItemStacks(0x40000000u, id);
        const int inv = ItemCount(0x40000000u, id);
        const bool ok = inv <= 0 || GiveItem(0x40000000u, id, -inv);
        const int after = ItemStacks(0x40000000u, id);
        const int box = StorageItemCount(0x40000000u, id);
        Log("key item repair: goods %d removed: none of its vanilla source flags is set in this save (key stacks %d -> %d)%s%s",
            goods, before, after, !ok ? " (GiveItemDirect unavailable)" : "",
            box > 0 ? "; one more in the storage box, left there" : "");
        failed = failed || !ok || after != 0;
    }
    if (failed) {
        return;  // the flags stay as they are, the check runs again
    }
    for (const std::uint32_t f : r.clear) {
        SetFlag(man, f, false);
        Log("key item repair: badge row flag %u cleared (%s %d not held or not earned)", f,
            BadgeName(kBadgeGoodsFirst + static_cast<std::int32_t>(f - kBadgeGateFirst)),
            kBadgeGoodsFirst + static_cast<std::int32_t>(f - kBadgeGateFirst));
    }
    for (const std::int32_t goods : r.kept) {
        Log("key item repair: goods %d held without a source flag but kept (%s)", goods,
            NgCycleLive() > 0 ? "NG+" : "too few such items to call it a stale replay");
    }
    SetFlag(man, kSgiveRepaired);
    done = true;
    Log("key item repair: %zu key item%s removed, %zu badge row flag%s cleared, %zu kept; save marked (flag %u)",
        r.remove.size(), r.remove.size() == 1 ? "" : "s", r.clear.size(), r.clear.size() == 1 ? "" : "s", r.kept.size(),
        kSgiveRepaired);
    if (!r.remove.empty() || !r.clear.empty()) {
        RequestGameSave("key item repair");
    }
}

// ---- the guest's shop gates while summoned (0.7.5) ----
// The messenger shop lists the Uncanny / Lost weapons of ShopLineupParam rows gated by the event
// flags 5500-5521 (tools/party/shop_gates.py). Their setter is common event 5500+slot, "the player
// owns weapon X": it runs for the chr type of the player and ends at once for anyone else. For a
// summoned guest (a phantom in the host's world) it ends at once, and the completion write sets
// all 22 flags in the host-world overlay, so the guest's shop listed every Uncanny / Lost weapon
// (playtest 2026-10-23, guest logs: "5500=1 ... 5521=1" among the local flag differences). The
// flags mean "this player obtained it": the overlay shows the own save's value, like the other
// item_lot_picked bits of the overlay (D4).
void ShopGateTick(const GameSnapshot& gs, int load_mode, double now) {
    static double next = 0.0;
    static int fixed_total = 0;
    static bool off = std::getenv("BB_PARTY_SHOP_GATES") && std::string(std::getenv("BB_PARTY_SHOP_GATES")) == "0";
    if (off || load_mode != 1 || gs.session_role != RoleClient || !gs.world_up || now < next) {
        return;
    }
    next = now + 0.5;
    const u64 man = Ptr(kFlagMan);
    if (!man) {
        return;
    }
    std::string fixed;
    // 0.7.8: also the badge rows 12101000-12101009 (own badges, PerPlayerItemGateEvent) and the Eye's sale
    // 12101100 / 12101101 (the own night and DLC): the host's values listed its rows in this guest's shop.
    std::vector<std::int32_t> gates;
    for (std::int32_t f = kShopOwnFirst; f <= kShopOwnLast; ++f) {
        gates.push_back(f);
    }
    for (std::int32_t n = 0; n < kBadgeGateCount; ++n) {
        gates.push_back(kBadgeGateFirst + n);
    }
    gates.push_back(static_cast<std::int32_t>(kFlagEyeSaleEvent));
    gates.push_back(static_cast<std::int32_t>(kFlagEyeSale));
    for (const std::int32_t f : gates) {
        bool own = false;
        if (!progress::ReadFlagOwnSave(static_cast<u32>(f), &own)) {
            return;  // the own pool is not readable now: leave everything as it is
        }
        if (IsFlag(man, f) != own) {
            SetFlag(man, f, own);
            fixed += (fixed.empty() ? "" : ",") + std::to_string(f) + (own ? "=1" : "=0");
        }
    }
    if (!fixed.empty() && fixed_total < 40) {
        fixed_total++;
        Log("shop gates while summoned: the host-world overlay shows the own save's 'owns the weapon' / badge / Eye sale "
            "flags: %s",
            fixed.c_str());
    }
}

// Diagnostics (BB_PARTY_SHOP_GATE_LOG=1, off by default): the Chalice "case" flags 92905300+n of the dungeon
// the player stands in, as this game's flag manager reads them (Sinister Root Chalice shop gates are
// 92905385-92905389). Logged when the set changes, at most 20 lines.
void ChaliceCaseFlagLogTick(const GameSnapshot& gs, int load_mode, double now) {
    static const bool on = std::getenv("BB_PARTY_SHOP_GATE_LOG") && std::string(std::getenv("BB_PARTY_SHOP_GATE_LOG")) == "1";
    static double next = 0.0;
    static std::string last;
    static int lines = 0;
    if (!on || !gs.world_up || gs.loading || gs.map_id == 0xffffffffu || (gs.map_id >> 24) != 29 || now < next || lines >= 20) {
        return;
    }
    next = now + 1.0;
    const u64 man = Ptr(kFlagMan);
    if (!man) {
        return;
    }
    std::string set;
    for (std::int32_t f = 92905300; f < 92905400; ++f) {
        if (IsFlag(man, f)) {
            set += (set.empty() ? "" : ",") + std::to_string(f);
        }
    }
    // the same flags in the chalice slot groups (area 40 + k, type 9, zone 5)
    for (int k = 0; k < 7; ++k) {
        std::string in;
        for (std::int32_t n = 300; n < 400; ++n) {
            if (IsFlag(man, 94005000 + k * 100000 + n)) {
                in += (in.empty() ? "" : ",") + std::to_string(n);
            }
        }
        set += "; slot " + std::to_string(k) + " group: " + (in.empty() ? "none" : in);
    }
    {
        std::string reg;
        for (std::int32_t f = 12907200; f < 12907260; ++f) {
            if (IsFlag(man, f)) {
                reg += (reg.empty() ? "" : ",") + std::to_string(f);
            }
        }
        set += "; region flags 129072xx: " + (reg.empty() ? std::string("none") : reg);
    }
    if (set != last) {
        last = set;
        lines++;
        Log("chalice case flags (role %d, load mode %d, map 0x%08x): %s", int(gs.session_role), load_mode, gs.map_id,
            set.empty() ? "none" : set.c_str());
    }
}

int g_batch_n = 0;         // quiet catch-up gives since the last summary notice
double g_batch_last = 0.0;  // when the last one landed

void ItemsTick() {
    if (!g_on.load()) {
        return;
    }
    if (g_batch_n > 0 && Now() - g_batch_last >= 1.5) {
        // One notice for the whole host catch-up instead of one item-get dialog per item.
        char b[96];
        std::snprintf(b, sizeof b, g_batch_n == 1 ? "Received %d item from the party" : "Received %d items from the party",
                      g_batch_n);
        Log("catch-up batch: %d item grant(s) given quietly: %s", g_batch_n, b);
        party::status::SetLastEvent(b);
        g_batch_n = 0;
    }
    ItemGetProbe();
    if (!g_test_offered && !g_test.empty()) {
        g_test_offered = true;
        std::lock_guard<std::mutex> lk(g_guest_mu);
        g_guest.Offer(g_test);
    }
    HostTestTick(Now());
    KillAwardTick(Now());
    NoticeTick();
    GiftTick(Now());
    const GameSnapshot gs = ReadGameState(false);
    ItemApplyState s;
    s.world_up = gs.world_up && gs.map_id != 0xffffffffu; // the player stands in a map
    s.loading = gs.loading;
    s.session_role = gs.session_role;
    s.load_mode = LoadMode(Ptr(kFlagMan));
    s.own_world = OwnWorld();
    s.game_data = gs.player_rec != 0 && Ptr(kMapItemMan) != 0;
    s.calm = Calm(gs.player, Now());
    s.alive = PlayerHp(gs.player) != 0;
    s.cutscene = CutscenePlaying();
    s.boss_quiet = BossQuiet(Now());
    s.game_dialog = gs.world_up && split::DialogUp();
    ItemGetWatchTick(gs, Now());
    ShopGateTick(gs, s.load_mode, Now());
    ChaliceCaseFlagLogTick(gs, s.load_mode, Now());
    RepairShopGatesTick(gs);
    RepairBadgeGatesTick(gs);
    BadgeGuardTick(gs, s.load_mode, Now());
    RepairEyeGateTick(gs);
    RepairSgivePoisonTick(gs);
    KeyGoodsWatchTick(gs, s.load_mode, Now());
    SummonedGivesTick(gs, s.load_mode, Now());
    KeyCatchupTick(gs, s.load_mode, Now());
    static ItemApplyMode gate_logged = ItemApplyMode::None;
    const ItemApplyMode mode_now = ItemApplyModeNow(s);
    if (mode_now != gate_logged) {
        gate_logged = mode_now;
        Log("apply gate %s (%s, load mode %d, own world %d)",
            mode_now == ItemApplyMode::Home       ? "open: own world"
            : mode_now == ItemApplyMode::Summoned ? "open: summoned (direct give, own pool)"
                                                  : "closed",
            Describe(gs).c_str(), s.load_mode, s.own_world);
    }
    {
        // Playtest 1 #19: no item grants into a prologue world (this guest's, or while the host is
        // in its own prologue); they wait in the queue.
        static bool held = false;
        const char* why = nullptr;
        if (coop::PartyPrologueHold(&why)) {
            if (!held) {
                held = true;
                Log("item grants held: %s (#19: no sync into a prologue world)", why);
            }
            return;
        }
        if (held) {
            held = false;
            Log("item grants: prologue over; the queue goes on");
        }
    }
    std::optional<ItemGrant> g;
    ItemApplyMode mode;
    const char* hold = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        g = g_guest.Step(s, Now());
        mode = g_guest.LastMode();
        hold = g_guest.LastHold();
    }
    static const char* hold_logged = nullptr;
    if (hold != hold_logged) {
        hold_logged = hold;
        if (hold) {
            Log("item popup held (%s): the next grant waits", hold);
        } else {
            Log("item popup hold over");
        }
    }
    if (!g) {
        return;
    }
    if (const std::vector<std::uint32_t> gate = LotOwnGateFlags(g->lot); !gate.empty() && OwnGateNow(gate) != 1) {
        // 0.7.8: the Eye of a Blood-drunk Hunter (lot 10040, the DLC entry) only once this player's own world has
        // reached the vanilla gift's gate (night 9801, DLC 6899): before, the host's Eye went to every member at
        // once (playtest 2026-11-01: a guest's party-given Eye before its own night). Not marked done: the host's
        // next list brings it again.
        static std::set<std::int32_t> logged;
        if (logged.insert(g->lot).second) {
            Log("%s: held back: this player's own world has not reached the vanilla gate (%s); it comes with the "
                "host's list once it has", DescribeItem(*g).c_str(), GateFlagsText(gate).c_str());
        }
        return;
    }
    if (mode == ItemApplyMode::Summoned) {
        const bool quiet = g->source == ItemSource::Full;
        const SummonedResult r = ApplySummoned(*g, quiet);
        if (quiet && r == SummonedResult::Given) {
            g_batch_n++;
            g_batch_last = Now();
        }
        std::lock_guard<std::mutex> lk(g_guest_mu);
        if (r == SummonedResult::Defer && g->source == ItemSource::BossDrop) {
            // A boss drop is for the guests at the kill: never carried home.
            Log("%s: could not be given here; dropped (not carried home)", DescribeItem(*g).c_str());
        } else if (r == SummonedResult::Defer && g->source == ItemSource::Chalice) {
            // A dungeon award is for the dungeon: never carried home.
            Log("%s: could not be given in the dungeon; dropped (not carried home)", DescribeItem(*g).c_str());
        } else if (r == SummonedResult::Defer) {
            g_guest.Defer(*g);
            return;
        }
        g_guest.Done(g->Key());
        if (r == SummonedResult::Given && !quiet) {
            g_guest.Awarded(Now());
        }
        return;
    }
    const bool awarded = Apply(*g);
    std::lock_guard<std::mutex> lk(g_guest_mu);
    g_guest.Done(g->Key());
    if (awarded) {
        g_guest.Awarded(Now());
    }
}

// ---- 0.7.8 give items to a party member (party_give.cpp) ----

bool ReadGiveFacts(std::uint32_t id, give::GoodsFacts* out) {
    LotSlotState s;
    s.category = 4;
    s.id = static_cast<std::int32_t>(id);
    FillGoodsLimits(&s);
    out->id = id;
    out->held = s.owned;
    out->max_num = s.max_num;
    out->max_storage = s.max_storage;
    out->goods_type = s.goods_type;
    out->quick = false;
    for (int slot = 0; slot < 6; ++slot) {
        if (QuickItemId(slot) == id) {
            out->quick = true;
        }
    }
    return s.owned >= 0 && s.max_num >= 0 && s.goods_type >= 0;
}

int GiveStorageRoom(std::uint32_t id) {
    return StorageRoom(id);
}

int GiveGoodsToOwn(std::uint32_t id, int n, bool dialog) {
    const u32 bits = 0x40000000u;
    const int inv0 = ItemCount(bits, id);
    const int box0 = StorageItemCount(bits, id);
    if (inv0 < 0 || n <= 0) {
        return -1;
    }
    give::GoodsFacts f;
    ReadGiveFacts(id, &f);
    const bool over = f.max_num > 0 && inv0 + n > f.max_num;
    bool ok = false;
    if (over) {
        ok = GiveGoodsViaList(id, n);  // fills the inventory, the rest into the storage box; queues its own dialog
    } else {
        ok = GiveItem(bits, id, n);
        if (ok && dialog) {
            ShowItemGet(bits, id, n);
        }
    }
    if (!ok) {
        return -1;
    }
    const int inv1 = ItemCount(bits, id);
    const int box1 = StorageItemCount(bits, id);
    return (inv1 - inv0) + (std::max(box1, 0) - std::max(box0, 0));
}

int TakeGoodsFromOwn(std::uint32_t id, int n) {
    const u32 bits = 0x40000000u;
    const int before = ItemCount(bits, id);
    if (before < 0 || n <= 0 || !GiveItem(bits, id, -n)) {
        return -1;
    }
    const int after = ItemCount(bits, id);
    return after < 0 ? -1 : before - after;
}


// ---- 0.7.9 give weapons and armor (party_give.cpp; the rules are party_give_rules.h, docs/party/give_items.md 5) ----
//
// An inventory entry is {gaitem handle, type bits | id, count}. A weapon / armor entry's handle names a dynamic GaItem
// instance (SprjGaitem table, bit 0x800000, as the gems': +8 handle, +0xC item type). A weapon instance carries five gem
// slots: a shape mask at +0x38 + 0x10 * i and the attached gem's handle at +0x3C + 0x10 * i (written by the game through
// the refcounted assign 0x1A8BED0, which is also what 0x1A876D0 does when it builds a weapon with its gems). A gem
// attached to a weapon is not in the inventory list: the weapon's slot is its only reference.

namespace {

constexpr u64 kGaitemAssign = 0x1a8bed0;  // u32* (u32* dst handle, u32* src handle): refcounted handle assign
constexpr std::uint32_t kGearAux = 0x28, kGearSlotMask = 0x38, kGearSlotHandle = 0x3c, kGearSlotStride = 0x10;

/// Any dynamic GaItem instance of a handle (SprjGaitem +8 + index * 8; the handle at +8 must match), else 0.
u64 DynInstance(u32 handle) {
    const u64 gi = Ptr(kSprjGaitem);
    if (!gi || !handle || !(handle & 0x800000u) || (handle & 0xffffu) == 0xffffu) {
        return 0;
    }
    u64 inst = 0;
    u32 h = 0;
    if (!SafeGet(gi + 8 + 8ull * (handle & 0xffffu), &inst) || !inst || !SafeGet(inst + 8, &h) || h != handle) {
        return 0;
    }
    return inst;
}

bool GearTypeBits(give::Kind k, u32* bits) {
    if (k == give::Kind::Weapon) {
        *bits = kItemWeapon;
    } else if (k == give::Kind::Armor) {
        *bits = kItemProtector;
    } else {
        return false;
    }
    return true;
}

/// The inventory entry as a gear item (weapon: with its blood gems). False for anything that is not a weapon / armor
/// or whose instance cannot be read.
bool GearFromEntry(const InventoryEntry& e, GearItem* out) {
    const u32 type = e.item & 0xf0000000u;
    GearItem g;
    if (type == kItemWeapon) {
        g.kind = give::Kind::Weapon;
    } else if (type == kItemProtector) {
        g.kind = give::Kind::Armor;
    } else {
        return false;
    }
    g.id = e.item & 0x0fffffffu;
    g.handle = e.handle;
    g.index = e.index;
    g.equipped = HandleEquipped(e.handle);
    if (g.kind == give::Kind::Weapon) {
        const u64 inst = DynInstance(e.handle);
        if (!inst) {
            return false;
        }
        if (!SafeGet(inst + kGearAux, &g.aux)) {
            return false;
        }
        for (int i = 0; i < give::kGearSlots; ++i) {
            u32 gh = 0;
            if (!SafeGet(inst + kGearSlotMask + kGearSlotStride * i, &g.masks[i])) {
                return false;
            }
            if (!SafeGet(inst + kGearSlotHandle + kGearSlotStride * i, &gh) || gh == 0 || gh == 0xffffffffu) {
                continue;
            }
            const u64 gi = GemInstance(gh);
            u32 type_id = 0;
            give::GearGem& gg = g.gems[g.gem_count];
            if (!gi || !SafeGet(gi + 0xc, &type_id) || !SafeRead(gi + 0x18, gg.data, sizeof gg.data)) {
                return false;  // a slot we cannot read: never move a weapon half-understood
            }
            gg.slot = i;
            gg.id = static_cast<std::int32_t>(type_id & 0x0fffffffu);
            ++g.gem_count;
        }
    }
    *out = g;
    return true;
}

}  // namespace

bool ReadGearEntry(give::Kind kind, std::uint32_t id, std::uint32_t handle, GearItem* out) {
    u32 bits = 0;
    if (!GearTypeBits(kind, &bits) || !handle) {
        return false;
    }
    for (const InventoryEntry& e : InventoryEntries()) {
        if (e.handle == handle && e.item == (bits | id)) {
            return GearFromEntry(e, out);
        }
    }
    return false;
}

std::vector<GearItem> OwnGear() {
    std::vector<GearItem> out;
    for (const InventoryEntry& e : InventoryEntries()) {
        GearItem g;
        if (GearFromEntry(e, &g)) {
            out.push_back(g);
        }
    }
    return out;
}

int GearFreeSlots() {
    return InventoryFreeSlots();
}

bool GearEntryExists(std::uint32_t handle) {
    for (const InventoryEntry& e : InventoryEntries()) {
        if (e.handle == handle) {
            return true;
        }
    }
    return false;
}

bool TakeGearFromOwn(std::uint32_t handle) {
    int index = -1;
    for (const InventoryEntry& e : InventoryEntries()) {
        if (e.handle == handle) {
            index = e.index;
        }
    }
    if (index < 0 || !RemoveInventoryIndex(index)) {
        return false;
    }
    return !GearEntryExists(handle);  // the entry must be gone
}

namespace {

/// A new gem instance with the data of `gg` (one reference held by `*handle`); false when the game cannot make it.
bool MakeGem(const give::GearGem& gg, u32* handle) {
    static const u8 kCreate[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                 0x53, 0x48, 0x83, 0xec, 0x28, 0x41, 0x89, 0xd6, 0x48, 0x89, 0xf3};
    if (!MatchesOriginal(kGaitemCreate, kCreate, sizeof kCreate) ||
        !GameParamRow("GemGenParam", "GEM_GEN_PARAM_ST", static_cast<u32>(gg.id))) {
        return false;
    }
    for (int k = 2; k < 8; ++k) {
        if (gg.data[k] != -1 && !GameParamRow("GemeffectParam", "GEMEFFECT_PARAM_ST", static_cast<u32>(gg.data[k]))) {
            return false;
        }
    }
    const u64 gi = Ptr(kSprjGaitem);
    if (!gi) {
        return false;
    }
    u32 h = 0;
    reinterpret_cast<GaitemCreateFn>(Guest(kGaitemCreate))(&h, gi, static_cast<u32>(gg.id));
    const u64 inst = GemInstance(h);
    u32 type = 0;
    if (!inst || !SafeGet(inst + 0xc, &type) || type != (0x80000000u | static_cast<u32>(gg.id))) {
        if (h) {
            reinterpret_cast<GaitemReleaseFn>(Guest(kGaitemRelease))(&h);
        }
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(inst + 0x18), gg.data, sizeof gg.data);
    *handle = h;
    return true;
}

/// A gem the new weapon cannot hold (no matching slot): into the inventory as a loose gem, never lost.
bool GemToInventory(const give::GearGem& gg) {
    GrantItem it;
    it.category = 8;
    it.id = gg.id;
    it.count = 1;
    it.has_gem = true;
    for (int k = 0; k < 8; ++k) {
        it.gem[k] = gg.data[k];
    }
    std::string why;
    return GiveGem(it, &why);
}

}  // namespace

void EnsureGearTable() {
    static bool done = false;
    if (done || give::GearTable::Get().Ready()) {
        return;
    }
    const auto wrows = GameParamRows("EquipParamWeapon", "EQUIP_PARAM_WEAPON_ST");
    const auto arows = GameParamRows("EquipParamProtector", "EQUIP_PARAM_PROTECTOR_ST");
    const auto lots = GameParamRows("ItemLotParam", "ITEMLOT_PARAM_ST");
    if (wrows.empty() || arows.empty() || lots.empty()) {
        return;  // the params are not loaded yet
    }
    done = true;
    // Gear a lot with a party share path gives reaches every member anyway (items.md 11 / 12): not offered. A weapon's family is its
    // 10000-id block (id = base + 100 * level, variants in the next blocks), an over-exclusion is the safe side.
    std::set<u32> shared_blocks, shared_armor;
    for (const auto& [lot, row] : lots) {
        (void)row;
        if (ItemLotSharePath(static_cast<std::int32_t>(lot)) == SharePath::None) {
            continue;
        }
        for (const LotRowData& r : LotChain(static_cast<std::int32_t>(lot))) {
            for (int i = 0; i < 8; ++i) {
                if (r.id[i] <= 0) continue;
                if (r.category[i] == 0) shared_blocks.insert(static_cast<u32>(r.id[i]) / 10000);
                if (r.category[i] == 1) shared_armor.insert(static_cast<u32>(r.id[i]));
            }
        }
    }
    // isDeposit: EquipParamWeapon byte 259 bit 7, EquipParamProtector byte 216 bit 0 (1.09 paramdef; a normal inventory row, the
    // NPC / internal rows are 0).
    std::vector<u32> weapons, armor;
    for (const auto& [id, row] : wrows) {
        u8 b = 0;
        if (SafeGet(row + 259, &b) && (b & 0x80) && !shared_blocks.count(id / 10000)) weapons.push_back(id);
    }
    for (const auto& [id, row] : arows) {
        u8 b = 0;
        if (SafeGet(row + 216, &b) && (b & 0x01) && !shared_armor.count(id)) armor.push_back(id);
    }
    give::GearTable::Get().Set(std::move(weapons), std::move(armor));
    Log("gear table: %zu giveable weapon rows, %zu armor rows (%zu shared weapon blocks, %zu shared armor ids excluded)",
        give::GearTable::Get().Weapons(), give::GearTable::Get().Armor(), shared_blocks.size(), shared_armor.size());
}

bool GearBuildable(give::Kind kind, std::uint32_t id, int gems, const give::GearGem* g) {
    if (kind == give::Kind::Weapon) {
        if (!GameParamRow("EquipParamWeapon", "EQUIP_PARAM_WEAPON_ST", id)) return false;
    } else if (kind == give::Kind::Armor) {
        if (!GameParamRow("EquipParamProtector", "EQUIP_PARAM_PROTECTOR_ST", id)) return false;
    } else {
        return false;
    }
    for (int i = 0; i < gems; ++i) {
        if (!GameParamRow("GemGenParam", "GEM_GEN_PARAM_ST", static_cast<u32>(g[i].id))) return false;
        for (int k = 2; k < 8; ++k) {
            if (g[i].data[k] != -1 && !GameParamRow("GemeffectParam", "GEMEFFECT_PARAM_ST", static_cast<u32>(g[i].data[k]))) return false;
        }
    }
    return true;
}

namespace {

/// Builds `gg` as a new gem instance and assigns it to slot `gg.slot` of the weapon instance `inst` when the slot is free and
/// its shape mask fits the gem's (the game's own test, 0x1A876D0). The slot's reference is the gem's only one afterwards.
bool PlaceGem(u64 inst, const give::GearGem& gg) {
    static const u8 kAssign[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x49, 0x89, 0xf6, 0x48, 0x89, 0xfb, 0x8b, 0x33};
    u32 mask = 0, gem_h = 0, now_h = 0;
    const bool code_ok = MatchesOriginal(kGaitemAssign, kAssign, sizeof kAssign);
    const bool read_ok = inst && gg.slot >= 0 && gg.slot < give::kGearSlots &&
                         SafeGet(inst + kGearSlotMask + kGearSlotStride * gg.slot, &mask) &&
                         SafeGet(inst + kGearSlotHandle + kGearSlotStride * gg.slot, &now_h);
    if (!code_ok || !read_ok || !MakeGem(gg, &gem_h)) {
        Log("gear gem: gem %d into slot %d not built: code %d read %d (slot mask %#x, handle %#x)", gg.id, gg.slot, code_ok, read_ok, mask, now_h);
        return false;
    }
    bool placed = false;
    u32 gem_mask = 0;
    const u64 gem_inst = GemInstance(gem_h);
    if (gem_inst && SafeGet(gem_inst + 0x1c, &gem_mask) && (mask & gem_mask) != 0 && mask != 0x80000000u &&
        (now_h == 0 || now_h == 0xffffffffu)) {
        reinterpret_cast<u32*(BB_COOP_SYSV*)(u32*, u32*)>(Guest(kGaitemAssign))(
            reinterpret_cast<u32*>(inst + kGearSlotHandle + kGearSlotStride * gg.slot), &gem_h);
        placed = true;
    }
    Log("gear gem: gem %d into slot %d: slot mask %#x, slot handle %#x, gem mask %#x: %s", gg.id, gg.slot, mask, now_h, gem_mask,
        placed ? "placed" : "not placed");
    reinterpret_cast<GaitemReleaseFn>(Guest(kGaitemRelease))(&gem_h);  // our reference: the slot keeps its own
    return placed;
}

}  // namespace

int GearFixtureGems(std::uint32_t handle, int want, give::GearGem* out, std::uint32_t* masks, std::uint32_t* aux) {
    // Real gems of the test save (GemGenParam / GemeffectParam rows exist), tried in each free slot until one fits its mask.
    static const give::GearGem kCatalogue[] = {
        {0, 124001, {1, 1, 126301, 43101, -1, -1, -1, -1}},
        {0, 150002, {1, 1, 3133407, -1, -1, -1, -1, -1}},
        {0, 123000, {1, 8, 122401, -1, -1, -1, -1, -1}},
    };
    const u64 inst = DynInstance(handle);
    int n = 0;
    // A fresh weapon has no slots: give the first `want` slots the shape of the first catalogue gem (mask 1) or the second kind (8).
    const u32 shape[give::kGearSlots] = {1, 8, 1, 8, 1};
    const u32 none = give::kNoSlot, one_aux = 1;
    for (int slot = 0; inst && slot < give::kGearSlots; ++slot) {
        masks[slot] = slot < want ? shape[slot] : none;
        SafeWrite(inst + kGearSlotMask + kGearSlotStride * slot, &masks[slot], 4);
    }
    if (inst) {
        *aux = one_aux;
        SafeWrite(inst + kGearAux, &one_aux, 4);
    }
    for (int slot = 0; inst && slot < give::kGearSlots && n < want; ++slot) {
        for (const give::GearGem& c : kCatalogue) {
            give::GearGem g = c;
            g.slot = slot;
            if (PlaceGem(inst, g)) {
                out[n++] = g;
                break;
            }
        }
    }
    return n;
}

int GiveGearToOwn(const GearItem& want, bool dialog, GearGiveResult* res) {
    GearGiveResult r;
    u32 bits = 0;
    if (!GearTypeBits(want.kind, &bits) || want.id == 0 || want.gem_count < 0 || want.gem_count > give::kGearSlots) {
        if (res) *res = r;
        return -1;
    }
    std::set<u32> before;
    for (const InventoryEntry& e : InventoryEntries()) {
        before.insert(e.handle);
    }
    if (!GiveItem(bits, want.id, 1)) {
        if (res) *res = r;
        return -1;
    }
    u32 fresh = 0;
    int fresh_n = 0;
    for (const InventoryEntry& e : InventoryEntries()) {
        if (!before.count(e.handle) && e.item == (bits | want.id)) {
            fresh = e.handle;
            ++fresh_n;
        }
    }
    if (fresh_n != 1) {
        // Nothing, or more than one new entry: we cannot tell which is ours (a failed add leaves nothing).
        r.arrived = fresh_n;
        if (res) *res = r;
        return fresh_n == 0 ? 0 : -1;
    }
    r.handle = fresh;
    r.arrived = 1;
    if (want.kind == give::Kind::Weapon) {
        // The slot layout first (a fresh weapon has none), then the gems into their slots.
        const u64 inst = DynInstance(fresh);
        if (inst) {
            SafeWrite(inst + kGearAux, &want.aux, 4);
            for (int i = 0; i < give::kGearSlots; ++i) {
                SafeWrite(inst + kGearSlotMask + kGearSlotStride * i, &want.masks[i], 4);
            }
        }
        for (int i = 0; i < want.gem_count; ++i) {
            if (PlaceGem(inst, want.gems[i])) {
                ++r.gems_placed;
            } else if (GemToInventory(want.gems[i])) {
                ++r.gems_loose;  // the slot did not take it: the gem is in the inventory, not lost
            } else {
                ++r.gems_lost;
            }
        }
    }
    if (dialog) {
        ShowItemGet(bits, want.id, 1);
    }
    if (res) *res = r;
    return 1;
}

void GearProbe() {
    int n = 0;
    for (const InventoryEntry& e : InventoryEntries()) {
        const u32 type = e.item & 0xf0000000u;
        if (type != kItemWeapon && type != kItemProtector) {
            continue;
        }
        GearItem g;
        const bool ok = GearFromEntry(e, &g);
        Log("gear probe: index %d handle %#x item %#x count %d ok %d equipped %d gems %d free-slots %d", e.index, e.handle, e.item,
            e.count, ok ? 1 : 0, g.equipped ? 1 : 0, g.gem_count, InventoryFreeSlots());
        for (int i = 0; ok && i < g.gem_count; ++i) {
            Log("gear probe:   slot %d gem %d data %d %d %d %d %d %d %d %d", g.gems[i].slot, g.gems[i].id, g.gems[i].data[0],
                g.gems[i].data[1], g.gems[i].data[2], g.gems[i].data[3], g.gems[i].data[4], g.gems[i].data[5],
                g.gems[i].data[6], g.gems[i].data[7]);
        }
        if (type == kItemWeapon && n < 12) {
            if (const u64 inst = DynInstance(e.handle)) {
                u32 w[40] = {};
                SafeRead(inst, w, sizeof w);
                char line[600];
                int o = 0;
                for (int k = 0; k < 40; ++k) {
                    o += std::snprintf(line + o, sizeof line - o, "%s%08x", k ? " " : "", w[k]);
                }
                Log("gear probe:   instance %s", line);
            }
        }
        ++n;
    }
    // the code bytes of the functions we call (to pin them in CodeOk)
    static const u64 fns[] = {0x14d0310, 0x1a8bed0, 0x14dab20};
    for (u64 f : fns) {
        u8 b[24] = {};
        SafeRead(Guest(f), b, sizeof b);
        char line[120];
        int o = 0;
        for (int k = 0; k < 24; ++k) o += std::snprintf(line + o, sizeof line - o, "%02x ", b[k]);
        Log("gear probe: code %#llx: %s", static_cast<unsigned long long>(f), line);
    }
    Log("gear probe: %d weapon / armor entries, free slots %d", n, InventoryFreeSlots());
}

#endif // BB_PARTY_ITEMS_NO_GAME

} // namespace coop
