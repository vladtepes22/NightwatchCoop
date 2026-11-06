// SPDX-License-Identifier: GPL-3.0-or-later
// Item lot tables of the party (docs/party/items.md): which lots carry a pickup flag, which awards are
// host-only (the ledger), award spots, treasure and boss drops that are shared, the lot audit rows. The
// rules are the ones of the former Python item_tool.
#include "derive.h"

#include <algorithm>
#include <cctype>
#include <tuple>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace coop::gamedata {
namespace {

using bbdata::MarkerKind;

constexpr std::int64_t kLedgerSize = 1000;

struct Lot {
    std::int64_t flag = 0;
    std::int64_t cat[8] = {};
    std::int64_t id[8] = {};
};
using Lots = std::map<std::int64_t, Lot>;
struct Good {
    std::int64_t type = 0, max_num = 0;
};
using Goods = std::unordered_map<std::int64_t, Good>;
using Items = std::vector<std::pair<std::int64_t, std::int64_t>>;  // (category, id)

std::int64_t FDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

// EMEVD sources whose lots are never replayed
bool DenyEvent(const std::string& stem, std::int64_t ev) {
    return (stem == "common" && (ev == 9100 || ev == 9440 || ev == 9500)) || (stem == "m35_00_00_00" && ev == 13501940) ||
           (stem == "m21_00_00_00" && ev == 12105064);
}
bool DenyLot(std::int64_t lot) { return lot == 0 || lot == 43802; }

// questline items that are not goodsType 1 but the guests must have
const std::set<std::int64_t> kSharedQuestLots = {2410900, 2410910, 2410920};
// progression keys / workshop tools re-given by the key catch-up
const std::set<std::int64_t> kSharedCatchupLots = {2400450, 2800290, 2600550, 2600610, 3200720, 3200810, 3500800, 2200360, 2411000};
// kill awards that are rewards of a defeat the whole party earned
const std::set<std::int64_t> kSharedRewardLots = {2605010};
struct KillAwardDef {
    std::int64_t lot;
    int area;
    std::int64_t entity, event, spot_flag;
};
constexpr KillAwardDef kKillAwards[] = {{2605010, 26, 2600129, 12600029, 5913}};

// Ledger indices of earlier releases (the guest's "given" bit is flag 60009000 + index in its own save).
const std::map<std::int64_t, int> kLedgerLegacy = {{14000, 0},    {15000, 1},    {21000, 3},        {23000, 5},
                                                   {29000, 23},   {30000, 24},   {31000, 25},       {39000, 28},
                                                   {25700000, 33}, {50000001, 35}, {50000010, 36}, {50800000, 38}};
constexpr int kLedgerReserved = 43;

// Lots the party never replays (mirror of ItemLotDenied in party_items.cpp) with the owner's reason.
const std::map<std::int64_t, const char*> kPerPlayerLots = {
    {10000, "Messenger gift (personal flag)"}, {10010, "Messenger gift (personal flag)"},
    {10011, "Messenger gift (personal flag)"}, {10500, "covenant gem (per player)"},
    {11500, "covenant gem (per player)"},      {12500, "covenant gem (per player)"},
    {17010, "repeatable NPC gift"},            {24060, "repeatable NPC gift"},
    {28000, "repeatable NPC gift"},            {32020, "repeatable NPC gift"},
    {43000, "repeatable NPC gift (Clock Tower)"}, {2100900, "DLC messenger hat (per player)"},
    {2100910, "DLC messenger hat (per player)"}, {2100920, "DLC messenger hat (per player)"},
    {43802, "cycle variant of 43800"},         {5020, "PvP text lot"},
    {16581, "NPC kill text lot"},
};
const std::set<std::int64_t> kReplayedPersonal = {3401800, 3401850, 10050};

bool IsChalice(std::int64_t lot, std::int64_t flag) {
    return lot >= 100000000 || (flag > 0 && FDiv(flag, 100000) % 100 == 29 && (FDiv(flag, 10000000) == 1 || FDiv(flag, 10000000) == 5));
}

bool TreasureFlag(std::int64_t f) {
    if (!(50000000 <= f && f < 60000000)) return false;
    const std::int64_t a = FDiv(f, 100000) % 100;
    return a != 0 && a != 99;
}

Items SlotItems(const Lot& r) {
    Items out;
    for (int k = 0; k < 8; ++k) {
        if (r.id[k] > 0 && r.cat[k] >= 0) out.emplace_back(r.cat[k], r.id[k]);
    }
    return out;
}

bool GoodsKey(const Goods& goods, std::int64_t id) {
    const auto it = goods.find(id);
    return it != goods.end() && (it->second.type == 1 || it->second.max_num == 1);
}

bool ShareItem(std::int64_t cat, std::int64_t id, const Goods& goods, const std::set<std::int64_t>& runes) {
    if (cat == 0 || cat == 1 || cat == 2) return true;
    if (cat == 8 && 200000 <= id && id < 300000) return true;
    if (cat == 8 && runes.count(id)) return true;
    if (cat == 4) return GoodsKey(goods, id);
    return false;
}

bool TreasureItemShared(std::int64_t cat, std::int64_t id, const Goods& goods, const std::set<std::int64_t>& runes) {
    if (cat == 0) return true;
    if (cat == 8) return (200000 <= id && id < 300000) || runes.count(id) > 0;
    if (cat == 4) return GoodsKey(goods, id);
    return false;
}

bool LotShareable(const Lots& rows, const Goods& goods, std::int64_t lot, const std::set<std::int64_t>& runes) {
    for (std::int64_t n = lot; rows.count(n) && n - lot < 8; ++n) {
        for (const auto& [c, i] : SlotItems(rows.at(n))) {
            if (ShareItem(c, i, goods, runes)) return true;
        }
    }
    return false;
}

std::int64_t FirstValue(const bbdata::Marker& m, bool* ok) {
    *ok = !m.v.empty();
    return *ok ? m.v[0] : 0;
}

struct Candidate {
    std::int64_t done;
    std::string note;
};

std::string EvNote(const std::string& stem, const bbdata::EventRec& r) {
    return stem + " ev " + std::to_string(r.event) + " slot " + std::to_string(r.slot);
}

std::map<std::int64_t, Candidate> LedgerCandidates(const Lots& rows, const std::vector<FileEvents>& events) {
    std::map<std::int64_t, Candidate> out;
    for (const FileEvents& fe : events) {
        for (const bbdata::EventRec& r : fe.recs) {
            for (const bbdata::Marker& m : r.markers) {
                if (m.kind != MarkerKind::ItemLot) continue;
                bool ok;
                const std::int64_t lot = FirstValue(m, &ok);
                if (!ok || DenyLot(lot) || fe.stem.rfind("m29", 0) == 0) continue;
                const auto it = rows.find(lot);
                if (it == rows.end() || it->second.flag > 0 || IsChalice(lot, -1)) continue;
                if (DenyEvent(fe.stem, r.event)) continue;
                Candidate c{r.event + r.slot, EvNote(fe.stem, r)};
                const auto cur = out.find(lot);
                if (cur == out.end() || c.done < cur->second.done || (c.done == cur->second.done && c.note < cur->second.note)) {
                    out[lot] = std::move(c);
                }
            }
        }
    }
    return out;
}

struct BossDrop {
    std::int64_t lot;
    int area;
    std::string note;
};

std::vector<BossDrop> BossDrops(const Lots& rows, const std::vector<FileEvents>& events) {
    std::map<std::int64_t, BossDrop> out;
    for (const FileEvents& fe : events) {
        const std::string& s = fe.stem;
        const bool map_stem = s.size() >= 4 && s[0] == 'm' && std::isdigit(static_cast<unsigned char>(s[1])) &&
                              std::isdigit(static_cast<unsigned char>(s[2])) && s[3] == '_';
        if (s.rfind("m29", 0) == 0 || !map_stem) continue;
        for (const bbdata::EventRec& r : fe.recs) {
            if (!r.Has(MarkerKind::BossDefeat) && !r.Has(MarkerKind::MidbossDefeat)) continue;
            for (const bbdata::Marker& m : r.markers) {
                if (m.kind != MarkerKind::ItemLot) continue;
                bool ok;
                const std::int64_t lot = FirstValue(m, &ok);
                if (ok && lot > 0 && rows.count(lot) && !out.count(lot)) {
                    out[lot] = {lot, (s[1] - '0') * 10 + (s[2] - '0'), s + " ev " + std::to_string(r.event)};
                }
            }
        }
    }
    std::vector<BossDrop> v;
    for (auto& [k, b] : out) v.push_back(std::move(b));
    return v;
}

struct SharedT {
    std::int64_t lot;
    int catchup;
    std::string note;
};

std::vector<SharedT> SharedTreasure(const Lots& rows, const Goods& goods, const std::set<std::int64_t>& runes) {
    std::map<std::int64_t, std::vector<std::int64_t>> by_flag;
    for (const auto& [lot, r] : rows) {
        if (IsChalice(lot, r.flag) || !TreasureFlag(r.flag)) continue;
        by_flag[r.flag].push_back(lot);
    }
    std::vector<SharedT> out;
    for (const auto& [f, lots] : by_flag) {
        bool hit = false;
        for (const std::int64_t lot : lots) {
            if (kSharedQuestLots.count(lot) || kSharedRewardLots.count(lot)) hit = true;
            for (const auto& [c, i] : SlotItems(rows.at(lot))) {
                if (TreasureItemShared(c, i, goods, runes)) hit = true;
            }
        }
        if (!hit) continue;
        for (const std::int64_t lot : lots) {
            std::string note = "items ";
            bool first = true;
            for (const auto& [c, i] : SlotItems(rows.at(lot))) {
                if (!first) note += "+";
                note += std::to_string(i);
                first = false;
            }
            out.push_back({lot, kSharedCatchupLots.count(lot) ? 1 : 0, note});
        }
    }
    return out;
}

struct AwardSpotT {
    std::int64_t lot, done;
    std::string note;
    bool operator<(const AwardSpotT& o) const {
        return std::tie(lot, done, note) < std::tie(o.lot, o.done, o.note);
    }
};

std::vector<AwardSpotT> AwardSpots(const Lots& rows, const std::vector<FileEvents>& events, const FlagCategories& cats) {
    std::set<AwardSpotT> out;
    for (const FileEvents& fe : events) {
        if (fe.stem.empty() || fe.stem[0] != 'm' || fe.stem.rfind("m29", 0) == 0) continue;
        for (const bbdata::EventRec& r : fe.recs) {
            if (r.rest != 0) continue;  // restarts on reload: its flag is not a "taken" state
            for (const bbdata::Marker& m : r.markers) {
                if (m.kind != MarkerKind::ItemLot) continue;
                bool ok;
                const std::int64_t lot = FirstValue(m, &ok);
                if (!ok) continue;
                const auto it = rows.find(lot);
                if (it == rows.end() || it->second.flag <= 0 || IsChalice(lot, it->second.flag)) continue;
                if (6000 <= it->second.flag && it->second.flag <= 8999) continue;  // per-player lots
                const std::int64_t done = r.event + r.slot;
                if (FDiv(done, 10000000) != 1 || cats.Lookup(done) != Cat::item_lot_picked) continue;
                out.insert({lot, done, EvNote(fe.stem, r)});
            }
        }
    }
    return {out.begin(), out.end()};
}

struct LedgerRowT {
    int idx;
    std::int64_t lot, done;
    std::string note;
};

std::vector<LedgerRowT> AssignLedger(const std::map<std::int64_t, Candidate>& cand) {
    std::set<int> used;
    for (int i = 0; i < kLedgerReserved; ++i) used.insert(i);
    int nxt = kLedgerReserved;
    std::vector<std::int64_t> order;
    for (const auto& [lot, c] : cand) order.push_back(lot);
    std::stable_sort(order.begin(), order.end(), [](std::int64_t a, std::int64_t b) {
        const auto ia = kLedgerLegacy.find(a), ib = kLedgerLegacy.find(b);
        const std::int64_t ka = ia == kLedgerLegacy.end() ? (std::int64_t{1} << 30) : ia->second;
        const std::int64_t kb = ib == kLedgerLegacy.end() ? (std::int64_t{1} << 30) : ib->second;
        return ka != kb ? ka < kb : a < b;
    });
    std::vector<LedgerRowT> out;
    for (const std::int64_t lot : order) {
        int idx;
        const auto it = kLedgerLegacy.find(lot);
        if (it != kLedgerLegacy.end()) {
            idx = it->second;
        } else {
            while (used.count(nxt)) ++nxt;
            idx = nxt;
            used.insert(idx);
        }
        if (idx >= kLedgerSize) throw bbdata::FormatError("ledger full");
        out.push_back({idx, lot, cand.at(lot).done, cand.at(lot).note});
    }
    std::sort(out.begin(), out.end(), [](const LedgerRowT& a, const LedgerRowT& b) { return a.idx < b.idx; });
    return out;
}

std::set<std::string> RequiredKinds(const Lot& row, const Goods& goods, const std::set<std::int64_t>& runes) {
    std::set<std::string> k;
    for (const auto& [c, i] : SlotItems(row)) {
        if (c == 0) k.insert("weapon");
        else if (c == 1) k.insert("armor");
        else if (c == 2) k.insert("ring");
        else if (c == 8 && 200000 <= i && i < 300000) k.insert("caryll");
        else if (c == 8 && runes.count(i)) k.insert("boss-rune");
        else if (c == 4) {
            const auto it = goods.find(i);
            if (it != goods.end() && it->second.type == 1) k.insert("key");
            else if (it != goods.end() && it->second.max_num == 1) k.insert("unique");
        }
    }
    return k;
}

// "reason (detail): more" -> "reason" (the cache keeps the short reason)
std::string ReasonHead(std::string s) {
    const auto a = s.find(" (");
    if (a != std::string::npos) s.resize(a);
    const auto b = s.find(':');
    if (b != std::string::npos) s.resize(b);
    return s;
}

}  // namespace

Sections DeriveItems(const ParamSet& P, const std::vector<FileEvents>& events, const FlagCategories& cats) {
    using bbdata::ParamTable;
    Lots rows;
    {
        const ParamTable& t = P.item_lot;
        const int cf = t.Col("getItemFlagId");
        int cc[8], ci[8];
        for (int k = 0; k < 8; ++k) {
            cc[k] = t.Col("lotItemCategory0" + std::to_string(k + 1));
            ci[k] = t.Col("lotItemId0" + std::to_string(k + 1));
        }
        for (std::size_t r = 0; r < t.Rows(); ++r) {
            Lot l;
            l.flag = t.At(r, cf);
            for (int k = 0; k < 8; ++k) {
                l.cat[k] = t.At(r, cc[k]);
                l.id[k] = t.At(r, ci[k]);
            }
            rows[t.ids[r]] = l;  // a repeated id: the last row, as a dict
        }
    }
    Goods goods;
    {
        const ParamTable& t = P.goods;
        const int ct = t.Col("goodsType"), cm = t.Col("maxNum");
        for (std::size_t r = 0; r < t.Rows(); ++r) goods[t.ids[r]] = {t.At(r, ct), t.At(r, cm)};
    }

    std::vector<std::pair<std::int64_t, std::int64_t>> flagged;
    for (const auto& [lot, r] : rows) {
        if (r.flag > 0 && !IsChalice(lot, r.flag)) flagged.emplace_back(lot, r.flag);
    }
    const auto cand_all = LedgerCandidates(rows, events);
    const std::vector<BossDrop> bossdrops = BossDrops(rows, events);
    std::set<std::int64_t> runes;
    for (const BossDrop& b : bossdrops) {
        for (const auto& [c, i] : SlotItems(rows.at(b.lot))) {
            if (c == 8) runes.insert(i);
        }
    }
    const std::vector<SharedT> shared = SharedTreasure(rows, goods, runes);
    std::set<std::int64_t> shared_lots;
    for (const SharedT& s : shared) shared_lots.insert(s.lot);
    std::set<std::int64_t> unshared;
    auto consider = [&](std::int64_t lot) {
        if (!shared_lots.count(lot) && !LotShareable(rows, goods, lot, runes)) unshared.insert(lot);
    };
    for (const auto& [lot, c] : cand_all) consider(lot);
    for (const auto& f : flagged) consider(f.first);
    std::map<std::int64_t, Candidate> cand;
    for (const auto& [lot, c] : cand_all) {
        if (!unshared.count(lot)) cand[lot] = c;
    }
    const std::vector<LedgerRowT> ledger = AssignLedger(cand);
    const std::vector<AwardSpotT> spots = AwardSpots(rows, events, cats);

    // the lot audit: which rows reach every member, which stay per player
    std::set<std::int64_t> ledger_lots, boss_lots, sources;
    for (const LedgerRowT& l : ledger) ledger_lots.insert(l.lot);
    for (const BossDrop& b : bossdrops) boss_lots.insert(b.lot);
    for (const FileEvents& fe : events) {
        for (const bbdata::EventRec& r : fe.recs) {
            for (const bbdata::Marker& m : r.markers) {
                if ((m.kind == MarkerKind::ItemLot || m.kind == MarkerKind::ItemLotClient) && !m.v.empty()) sources.insert(m.v[0]);
            }
        }
    }
    std::vector<Row> require, per_player;
    for (const auto& [lot, row] : rows) {
        std::set<std::string> kinds = RequiredKinds(row, goods, runes);
        if (kSharedRewardLots.count(lot)) kinds.insert("reward");
        if (kSharedQuestLots.count(lot)) kinds.insert("quest");
        if (kinds.empty()) continue;
        const std::int64_t f = row.flag;
        std::string kind_text;
        for (const std::string& k : kinds) kind_text += (kind_text.empty() ? "" : "+") + k;
        auto shared_row = [&] { require.push_back({std::to_string(lot), kind_text}); };
        auto per_row = [&](const std::string& why) { per_player.push_back({std::to_string(lot), ReasonHead(why)}); };
        const bool only_armor_ring = std::all_of(kinds.begin(), kinds.end(), [](const std::string& k) { return k == "armor" || k == "ring"; });
        if (lot == 0) {
        } else if (IsChalice(lot, f)) {
        } else if (f > 0 && FDiv(f, 100000) % 100 == 99 && 50000000 <= f && f < 60000000) {
        } else if (f > 0 && TreasureFlag(f)) {
            if (shared_lots.count(lot)) shared_row();
            else if (only_armor_ring) per_row("treasure: armor / ring only (owner rule: weapons, runes and key / unique items are shared)");
        } else if (6000 <= f && f <= 8999 && !kReplayedPersonal.count(lot)) {
            per_row("personal flag block 6000-8999 (Messenger gifts, DLC costumes / NPC gifts)");
        } else if (kPerPlayerLots.count(lot)) {
            per_row(kPerPlayerLots.at(lot));
        } else if (100000 <= lot && lot <= 101999) {
            per_row("rune / gem use (common event 9500)");
        } else if (5500 <= lot && lot <= 5959) {
            per_row("covenant / PvP lots");
        } else if (unshared.count(lot)) {
            // GAP: unshared by the policy although it gives a shareable item
        } else if (f > 0) {
            shared_row();
        } else if (ledger_lots.count(lot)) {
            shared_row();
        } else if (boss_lots.count(lot)) {
            shared_row();
        }
    }

    Sections s;
    auto S = [](std::int64_t v) { return std::to_string(v); };
    for (const auto& [lot, flag] : flagged) s["itemLotFlag"].push_back({S(lot), S(flag)});
    for (const LedgerRowT& l : ledger) s["itemLedger"].push_back({S(l.idx), S(l.lot), S(l.done), l.note});
    for (const AwardSpotT& a : spots) s["itemAwardSpot"].push_back({S(a.lot), S(a.done), a.note});
    for (const std::int64_t lot : unshared) s["itemUnshared"].push_back({S(lot)});
    for (const SharedT& t : shared) s["itemSharedTreasure"].push_back({S(t.lot), S(t.catchup), t.note});
    for (const BossDrop& b : bossdrops) s["itemBossDrop"].push_back({S(b.lot), S(b.area), b.note});
    s["itemRequireShared"] = std::move(require);
    s["itemPerPlayer"] = std::move(per_player);
    for (const KillAwardDef& k : kKillAwards) {
        s["itemKillAward"].push_back({S(k.lot), S(k.area), S(k.entity), S(k.event), S(k.spot_flag)});
    }
    for (const char* name : {"itemLotFlag", "itemLedger", "itemAwardSpot", "itemUnshared", "itemSharedTreasure", "itemBossDrop",
                             "itemRequireShared", "itemPerPlayer", "itemKillAward"}) {
        s[name];  // present even when empty
    }
    return s;
}

}  // namespace coop::gamedata
