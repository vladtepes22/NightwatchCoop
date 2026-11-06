// SPDX-License-Identifier: GPL-3.0-or-later
// The per-player weapon gates of the Messenger shop (party_items.cpp ShopGateWeapon / ShopGatesToRepair), derived
// from the player's own game: a shop row (ShopLineupParam) sells a weapon behind an event flag (qwcId) that only
// means "this player got the weapon":
//
//  * the pickup flag of an item lot that gives exactly that weapon (the Chalice dungeons' Uncanny / Lost weapons);
//  * the completion flag of a common event 5500 + slot, which ends once the player holds the weapon the event
//    names (3[4] IfPlayerHasItem, item type 0 = weapon) and that the same flag gates in the shop.
//
// Progress gates (boss / NPC events) and the other items are not in the table: those are shared.
#include "derive.h"

#include <algorithm>
#include <map>
#include <set>

namespace coop::gamedata {

Sections DeriveShopGates(const ParamSet& P, const std::vector<FileEvents>& events) {
    // flag -> the weapons the shop sells behind it
    std::map<std::int64_t, std::set<std::int64_t>> sold;
    {
        const bbdata::ParamTable& t = P.shop;
        const int qwc = t.Col("qwcId"), type = t.Col("equipType"), id = t.Col("equipId");
        for (std::size_t r = 0; r < t.Rows(); ++r) {
            if (t.At(r, qwc) > 0 && t.At(r, type) == 0) sold[t.At(r, qwc)].insert(t.At(r, id));
        }
    }
    std::map<std::int64_t, std::int64_t> gate;  // flag -> weapon
    // item lots: the lot's pickup flag is a gate flag of the shop and the lot gives one of the gated weapons
    {
        const bbdata::ParamTable& t = P.item_lot;
        const int cf = t.Col("getItemFlagId");
        int cc[8], ci[8];
        for (int k = 0; k < 8; ++k) {
            cc[k] = t.Col("lotItemCategory0" + std::to_string(k + 1));
            ci[k] = t.Col("lotItemId0" + std::to_string(k + 1));
        }
        for (std::size_t r = 0; r < t.Rows(); ++r) {
            const std::int64_t flag = t.At(r, cf);
            const auto it = sold.find(flag);
            if (flag <= 0 || it == sold.end()) continue;
            std::int64_t weapon = 0;
            int given = 0;
            for (int k = 0; k < 8; ++k) {
                if (t.At(r, ci[k]) <= 0) continue;
                ++given;
                if (t.At(r, cc[k]) == 0 && it->second.count(t.At(r, ci[k]))) weapon = t.At(r, ci[k]);
            }
            if (given == 1 && weapon && it->second.size() == 1) gate[flag] = weapon;
        }
    }
    // common event 5500 + slot: the weapon its 3[4] names, when the shop gates exactly that weapon on 5500 + slot
    for (const FileEvents& fe : events) {
        if (fe.stem != "common") continue;
        for (const bbdata::EventRec& r : fe.recs) {
            if (r.event != 5500) continue;
            const std::int64_t flag = r.event + r.slot;
            const auto it = sold.find(flag);
            if (it == sold.end() || it->second.size() != 1) continue;
            for (const bbdata::Cond& c : r.conds) {
                if (c.bank == 3 && c.id == 4 && c.v.size() >= 4 && c.v[1] == 0 && c.v[3] == 1 && it->second.count(c.v[2])) {
                    gate[flag] = c.v[2];
                    break;
                }
            }
        }
    }
    Sections s;
    auto& rows = s["shopGates"];
    for (const auto& [flag, weapon] : gate) rows.push_back({std::to_string(flag), std::to_string(weapon)});
    return s;
}

Sections DeriveStartWeapons(const ParamSet& P) {
    // The Dream messengers' starting weapons: the shop rows whose stock flag (eventFlag) the prologue's choice
    // events wait for (72101000 right hand, 72101011 left hand). A randomized game puts other weapons there.
    constexpr std::int64_t kStock[] = {72101000, 72101011};
    Sections s;
    auto& rows = s["startWeapons"];
    const bbdata::ParamTable& t = P.shop;
    const int ev = t.Col("eventFlag"), type = t.Col("equipType"), id = t.Col("equipId");
    for (const std::int64_t stock : kStock) {
        for (std::size_t r = 0; r < t.Rows(); ++r) {  // rows are in id order: the lowest row wins
            if (t.At(r, ev) == stock && t.At(r, type) == 0 && t.At(r, id) > 0) {
                rows.push_back({std::to_string(stock), std::to_string(t.At(r, id))});
                break;
            }
        }
    }
    return s;
}

}  // namespace coop::gamedata
