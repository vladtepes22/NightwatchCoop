// SPDX-License-Identifier: GPL-3.0-or-later
// The event flag category table (the Python tools' flag_tool.category_rows): the flagRows section and the
// category lookup the item award spots use. Rows are, in match order (first match wins): covenant rows,
// override rows, the specific rows the scripts and params give (merged by category inside a 1000-flag group),
// the global rows and the per-map structural defaults.
#include "derive.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <tuple>

namespace coop::gamedata {
namespace {

struct CatRow {
    std::int64_t lo, hi;
    Cat cat;
    const char* note;
};

using enum Cat;
#include "flag_category_rows.inc"

std::int64_t FDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
std::int64_t FMod(std::int64_t a, std::int64_t b) { return a - FDiv(a, b) * b; }

bool Has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }
template <std::size_t N>
bool HasAny(const std::string& hay, const char* const (&words)[N]) {
    for (const char* w : words) {
        if (Has(hay, w)) return true;
    }
    return false;
}

constexpr const char* kDoorKw[] = {"扉", "門", "エレベ", "ハシゴ", "梯子", "はしご", "ショートカット", "レバー", "跳ね橋", "隠し", "仕掛け", "開放", "鍵を開け", "リフト"};
constexpr const char* kDoorSkip[] = {"msg", "Msg", "メッセージ", "動かない", "PlayLog", "プレイログ", "ナビメッシュ", "初期化"};
constexpr const char* kNpcKw[] = {"NPC", "人形", "老人", "メッセンジャー", "乞食", "娼婦", "少女", "神父", "女医", "復讐者", "血族狩り", "クモ男",
                                  "老婆", "偏屈", "ヘンリック", "門番", "連盟", "学長", "司祭", "アルフレート", "アイリーン", "ヴァルトール", "シモン"};
constexpr const char* kKeyKw[] = {"時間帯変化", "エンド", "人さらい", "異様な月"};

// the flag store's map table: every map with pooled groups
constexpr int kStoreMaps[][2] = {{21, 0}, {21, 1}, {22, 0}, {23, 0}, {24, 0}, {24, 1}, {24, 2}, {25, 0}, {26, 0},
                                 {27, 0}, {28, 0}, {29, 0}, {32, 0}, {33, 0}, {34, 0}, {35, 0}, {36, 0}};

void AddStatic(std::vector<FlagCatRow>* out, const CatRow* b, const CatRow* e) {
    for (; b != e; ++b) out->push_back({b->lo, b->hi, b->cat, b->note});
}

const std::vector<FlagCatRow>& Global() {
    static const std::vector<FlagCatRow> rows = [] {
        std::vector<FlagCatRow> r;
        AddStatic(&r, std::begin(kGlobalRows), std::end(kGlobalRows));
        return r;
    }();
    return rows;
}

// the global rows, then the per-map / per-type / per-zone defaults
const std::vector<FlagCatRow>& Structural() {
    static const std::vector<FlagCatRow> rows = [] {
        std::vector<FlagCatRow> r = Global();
        const int types[] = {1, 5, 6, 7, 9};
        for (int typ : types) {
            for (const auto& m : kStoreMaps) {
                for (int zone = 0; zone < 10; ++zone) {
                    const std::int64_t lo = typ * 10000000LL + m[0] * 100000LL + m[1] * 10000LL + zone * 1000LL;
                    Cat c = never_sync;
                    std::string note = "pooled group with no data writers";
                    if (typ == 9 || m[0] == 29) {
                        note = "chalice dungeon / type 9 groups (out of scope)";
                    } else {
                        const ZoneDef* any = nullptr;
                        const ZoneDef* exact = nullptr;
                        for (const ZoneDef& z : kZoneDefaults) {
                            if (z.typ == typ && z.zone == -1) any = &z;
                            if (z.typ == typ && z.zone == zone) exact = &z;
                        }
                        const ZoneDef* use = any ? any : exact;
                        if (use) {
                            c = use->cat;
                            note = use->note;
                        }
                    }
                    char head[64];
                    std::snprintf(head, sizeof head, "m%d_%02d T%d zone %d: ", m[0], m[1], typ, zone);
                    r.push_back({lo, lo + 999, c, head + note});
                }
            }
        }
        for (int typ : types) {  // m29 blocks 1..9: chalice layouts
            const std::int64_t lo = typ * 10000000LL + 29 * 100000LL + 10000;
            r.push_back({lo, lo + 89999, never_sync, "m29 blocks 1-9 T" + std::to_string(typ) + ": chalice layouts"});
        }
        for (int area = 41; area < 47; ++area) {  // chalice instance areas
            for (int typ : types) {
                const std::int64_t lo = typ * 10000000LL + area * 100000LL;
                r.push_back({lo, lo + 9999, never_sync, "chalice instance area " + std::to_string(area) + " T" + std::to_string(typ)});
            }
        }
        for (int typ : {1, 5, 6, 7}) {  // area 99
            const std::int64_t lo = typ * 10000000LL + 99 * 100000LL;
            r.push_back({lo, lo + 9999, never_sync, "area 99 T" + std::to_string(typ) + ": chalice runtime groups, not in the saved pool"});
        }
        return r;
    }();
    return rows;
}

const std::vector<FlagCatRow>& Covenant() {
    static const std::vector<FlagCatRow> rows = [] {
        std::vector<FlagCatRow> r;
        AddStatic(&r, std::begin(kCovenantRows), std::end(kCovenantRows));
        return r;
    }();
    return rows;
}
const std::vector<FlagCatRow>& Overrides() {
    static const std::vector<FlagCatRow> rows = [] {
        std::vector<FlagCatRow> r;
        AddStatic(&r, std::begin(kOverrideRows), std::end(kOverrideRows));
        return r;
    }();
    return rows;
}

Cat Scan(const std::vector<FlagCatRow>& rows, std::int64_t f) {
    for (const FlagCatRow& r : rows) {
        if (r.lo <= f && f <= r.hi) return r.cat;
    }
    return Cat::none;
}

enum class PSrc { ItemLot, ItemLotCumulate, ShopEvent, ShopQwc };
struct PFlag {
    std::int64_t flag;
    PSrc src;
    std::string src_name;
    std::int32_t row_id;
};

std::vector<PFlag> ParamFlags(const ParamSet& P) {
    std::vector<PFlag> res;
    const bbdata::ParamTable& lot = P.item_lot;
    int cols[9];
    std::string names[9];
    names[0] = "getItemFlagId";
    for (int i = 1; i <= 8; ++i) names[i] = "getItemFlagId0" + std::to_string(i);
    for (int i = 0; i < 9; ++i) cols[i] = lot.Col(names[i]);
    const int cum = lot.Col("cumulateNumFlagId");
    for (std::size_t r = 0; r < lot.Rows(); ++r) {
        for (int i = 0; i < 9; ++i) {
            const std::int64_t f = lot.At(r, cols[i]);
            if (f > 0) res.push_back({f, PSrc::ItemLot, "ItemLotParam." + names[i], lot.ids[r]});
        }
        const std::int64_t f = lot.At(r, cum);
        if (f > 0) res.push_back({f, PSrc::ItemLotCumulate, "ItemLotParam.cumulateNumFlagId", lot.ids[r]});
    }
    const bbdata::ParamTable& shop = P.shop;
    const int ev = shop.Col("eventFlag"), qwc = shop.Col("qwcId");
    for (std::size_t r = 0; r < shop.Rows(); ++r) {
        if (shop.At(r, ev) > 0) res.push_back({shop.At(r, ev), PSrc::ShopEvent, "ShopLineupParam.eventFlag", shop.ids[r]});
        if (shop.At(r, qwc) > 0) res.push_back({shop.At(r, qwc), PSrc::ShopQwc, "ShopLineupParam.qwcId", shop.ids[r]});
    }
    return res;
}

}  // namespace

const char* CatName(Cat c) {
    switch (c) {
        case Cat::boss_defeated: return "boss_defeated";
        case Cat::boss_area: return "boss_area";
        case Cat::lamp_unlocked: return "lamp_unlocked";
        case Cat::shortcut_door: return "shortcut_door";
        case Cat::npc_quest: return "npc_quest";
        case Cat::item_lot_picked: return "item_lot_picked";
        case Cat::key_event: return "key_event";
        case Cat::cutscene_seen: return "cutscene_seen";
        case Cat::world_state: return "world_state";
        case Cat::session_runtime: return "session_runtime";
        case Cat::never_sync: return "never_sync";
        case Cat::time_of_day: return "time_of_day";
        case Cat::covenant_outcome: return "covenant_outcome";
        default: return "none";
    }
}

FlagCategories::FlagCategories(const std::vector<FileEvents>& events, const ParamSet& params) {
    using bbdata::FlagOp;
    using bbdata::MarkerKind;
    auto put = [&](std::int64_t f, Cat cat, const std::string& note, int prio) {
        if (f < 0) return;
        const auto it = spec_.find(f);
        if (it == spec_.end() || prio > it->second.prio) spec_[f] = {cat, note, prio};
    };
    for (const FileEvents& fe : events) {
        for (const bbdata::EventRec& r : fe.recs) {
            const std::string& name = r.name;
            const std::int64_t done = r.event >= 10000000 ? r.event + r.slot : 0;
            const std::string tag = fe.stem + " ev " + std::to_string(r.event) + (r.slot ? "+" + std::to_string(r.slot) : std::string());
            if (r.Has(MarkerKind::BossDefeat) || r.Has(MarkerKind::MidbossDefeat)) {
                if (done) put(done, boss_defeated, tag + " boss defeat event done", 9);
                for (const bbdata::Op& o : r.ops) {
                    if (o.StateIs(1) && o.first == o.last) {
                        const std::int64_t f = o.first;
                        if (f < 2100 || (5000 <= f && f <= 8999) || (9000 <= f && f <= 9451)) continue;
                        put(f, boss_defeated, "set by " + tag + " (boss defeat)", 8);
                    }
                }
            }
            if (name.rfind("ワープOBJ_", 0) == 0) {
                std::vector<std::int64_t> ids;
                for (const auto& rd : r.reads) ids.insert(ids.end(), rd.begin(), rd.end());
                for (const bbdata::Op& o : r.ops) {
                    if (o.first == o.last) ids.push_back(o.first);
                }
                for (const std::int64_t f : ids) {
                    if (FDiv(f, 10000000) != 7 || FMod(FDiv(f, 1000), 10) != 0) continue;
                    if (name.rfind("ワープOBJ_起動", 0) == 0 && FDiv(FMod(f, 1000), 100) == 2) {
                        put(f, lamp_unlocked, "lamp lit (7AAB02xx), awaited by " + tag + "; cleared for absent DLC", 9);
                    } else {
                        put(f, session_runtime, "lamp warp request, " + tag, 6);
                    }
                }
            }
            for (const bbdata::Op& o : r.ops) {
                if (o.op == FlagOp::Lamp && o.first > 0) {
                    put(o.first, lamp_unlocked, "lamp object flag (2009[5] registration) by " + tag, 9);
                } else if (o.op == FlagOp::Ladder && o.first > 0) {
                    put(o.first, shortcut_door, "ladder 2009[0] registered by " + tag, 7);
                }
            }
            if (!done) continue;
            const std::int64_t zone = FMod(FDiv(done, 1000), 10), typ = FDiv(done, 10000000);
            if (typ != 1) continue;
            if (r.rest == 1 && (zone == 0 || zone == 1)) {
                put(done, session_runtime, tag + " restarts on reload", 3);
                continue;
            }
            if (zone != 0 && zone != 1) continue;
            auto sets_in_zone01 = [&](Cat c) {
                for (const bbdata::Op& o : r.ops) {
                    const std::int64_t f = o.first;
                    if (o.StateIs(1) && f == o.last && f >= 10000000 && FDiv(f, 10000000) == 1 &&
                        (FMod(FDiv(f, 1000), 10) == 0 || FMod(FDiv(f, 1000), 10) == 1)) {
                        put(f, c, "set by " + tag, 5);
                    }
                }
            };
            if (HasAny(name, kKeyKw)) {
                put(done, key_event, tag + " world progression", 6);
                sets_in_zone01(key_event);
            } else if (HasAny(name, kDoorKw) && !HasAny(name, kDoorSkip)) {
                put(done, shortcut_door, tag + " door/lever/elevator", 6);
                sets_in_zone01(shortcut_door);
            } else if (Has(name, "宝箱") || r.Has(MarkerKind::ItemLot) || r.Has(MarkerKind::ItemLotClient)) {
                put(done, item_lot_picked, tag + " chest / item award", 5);
            } else if (r.Has(MarkerKind::Cutscene)) {
                put(done, cutscene_seen, tag + " cutscene played", 4);
            } else if (HasAny(name, kNpcKw)) {
                put(done, npc_quest, tag + " NPC", 4);
            }
        }
    }
    const std::vector<PFlag> pflags = ParamFlags(params);
    for (const PFlag& p : pflags) {
        if (p.src == PSrc::ItemLot) {
            put(p.flag, item_lot_picked, p.src_name + " row " + std::to_string(p.row_id), 2);
        } else if (p.src == PSrc::ShopEvent) {
            put(p.flag, never_sync, "shop stock row " + std::to_string(p.row_id) + " (per-player purchase)", 2);
        }
    }
    // Messenger shop gates that are story / boss progress without an item lot: shared set-only.
    std::map<std::int64_t, bool> lot_flags;
    for (const PFlag& p : pflags) {
        if (p.src == PSrc::ItemLot || p.src == PSrc::ItemLotCumulate) lot_flags[p.flag] = true;
    }
    for (const PFlag& p : pflags) {
        const std::int64_t f = p.flag;
        if (p.src != PSrc::ShopQwc || lot_flags.count(f) || (5500 <= f && f <= 5521) || (6000 <= f && f <= 8999) || f >= 90000000) {
            continue;
        }
        Cat have;
        const auto old = spec_.find(f);
        if (old != spec_.end()) {
            have = old->second.cat;
        } else {
            have = Scan(Structural(), f);
            if (have == Cat::none) have = never_sync;
        }
        if (have == item_lot_picked || have == never_sync || have == session_runtime) {
            put(f, world_state, "shop gate (ShopLineupParam " + std::to_string(p.row_id) + "): progress without an item lot, shared set-only", 10);
        }
    }
}

Cat FlagCategories::Lookup(std::int64_t f) const {
    Cat c = Scan(Covenant(), f);
    if (c != Cat::none) return c;
    c = Scan(Overrides(), f);
    if (c != Cat::none) return c;
    const auto it = spec_.find(f);
    if (it != spec_.end()) return it->second.cat;
    return Scan(Structural(), f);
}

std::vector<std::int64_t> FlagCategories::FlagsOf(Cat cat) const {
    std::vector<std::int64_t> out;
    for (const auto& [f, s] : spec_) {
        if (s.cat == cat && Lookup(f) == cat) out.push_back(f);
    }
    return out;
}

std::vector<FlagCatRow> FlagCategories::Rows() const {
    std::vector<FlagCatRow> out = Covenant();
    out.insert(out.end(), Overrides().begin(), Overrides().end());
    // the specific rows: contiguous ids of one category inside one 1000-flag group become one row
    struct Cur {
        std::int64_t lo, hi, blk;
        Cat cat;
        std::vector<std::string> notes;
    };
    std::vector<Cur> merged;
    for (const auto& [f, s] : spec_) {
        const std::int64_t blk = FDiv(f, 1000);
        if (!merged.empty() && merged.back().cat == s.cat && f == merged.back().hi + 1 && merged.back().blk == blk) {
            Cur& c = merged.back();
            c.hi = f;
            if (std::find(c.notes.begin(), c.notes.end(), s.note) == c.notes.end()) c.notes.push_back(s.note);
            continue;
        }
        merged.push_back({f, f, blk, s.cat, {s.note}});
    }
    for (const Cur& c : merged) {
        std::string note = c.notes[0];
        if (c.notes.size() > 1) note += " (+" + std::to_string(c.notes.size() - 1) + " more)";
        out.push_back({c.lo, c.hi, c.cat, note});
    }
    out.insert(out.end(), Structural().begin(), Structural().end());
    return out;
}

namespace {
// The Hunter's Dream headstones' menus (talk scripts t210000..t210003, t210721): the groups in the order the
// headstones list them, and for each the maps (AAB = area * 10 + block) in listing order. Our own layout of the
// warp menu; which lamps exist comes from the game. Map 320 is split between two headstones by lamp index.
struct Headstone {
    int group;
    std::vector<int> maps;
};
const std::vector<Headstone>& Headstones() {
    static const std::vector<Headstone> h = {
        {1, {241, 240, 242, 230}},  // Yharnam: Central Yharnam, Cathedral Ward, Upper Cathedral Ward, Old Yharnam
        {2, {220, 270, 320}},       // Frontier: Hemwick, Forbidden Woods, Byrgenwerth (lamps 0 and 2 of 320)
        {3, {280, 250, 211}},       // Unseen: Yahar'gul, Cainhurst, Abandoned Old Workshop
        {4, {320, 330, 260}},       // Nightmare: Lecture Building (lamps 1 and 3 of 320), Nightmare Frontier, Mensis
        {5, {340, 350, 360}},       // The Old Hunters
    };
    return h;
}
}  // namespace

Sections DeriveLamps(const FlagCategories& cats) {
    struct L {
        int aab, n, group, order;
    };
    std::vector<L> lamps;
    for (const std::int64_t f : cats.FlagsOf(Cat::lamp_unlocked)) {
        const std::int64_t area = FMod(FDiv(f, 100000), 100);
        if (!(70000000 <= f && f < 80000000) || FMod(FDiv(f, 100), 100) != 2 || area == 29 || area >= 40) continue;
        L l{static_cast<int>(FMod(FDiv(f, 10000), 1000)), static_cast<int>(FMod(f, 100)), 1, 1000};
        int order = 0;
        for (const Headstone& h : Headstones()) {
            for (const int m : h.maps) {
                ++order;
                if (m != l.aab) continue;
                if (m == 320 && ((h.group == 2) != (l.n % 2 == 0))) continue;  // 0, 2: Frontier; 1, 3: Nightmare
                l.group = h.group;
                l.order = order;
            }
        }
        lamps.push_back(l);  // a lamp of a map not listed: the first headstone, after the listed ones
    }
    std::sort(lamps.begin(), lamps.end(), [](const L& a, const L& b) {
        return std::tie(a.order, a.aab, a.n) < std::tie(b.order, b.aab, b.n);
    });
    Sections s;
    auto& rows = s["lamps"];
    for (const L& l : lamps) rows.push_back({std::to_string(l.aab), std::to_string(l.n), std::to_string(l.group)});
    return s;
}

Sections DeriveHostOnlyEvents(const std::vector<FileEvents>& events) {
    std::set<std::int64_t> ids;
    for (const FileEvents& fe : events) {
        for (const bbdata::EventRec& r : fe.recs) {
            if (r.name.find("ヒートアップ") != std::string::npos && r.event > 0 && r.event < 4294967296LL) ids.insert(r.event);
        }
    }
    Sections s;
    auto& rows = s["hostOnlyEvents"];
    for (const std::int64_t id : ids) rows.push_back({std::to_string(id)});
    return s;
}

Sections DeriveFlagRows(const FlagCategories& cats) {
    Sections s;
    auto& rows = s["flagRows"];
    for (const FlagCatRow& r : cats.Rows()) {
        // the Python esc(): a backslash is doubled, a double quote becomes an apostrophe
        std::string note;
        for (const char c : r.note) {
            if (c == '\\') note += "\\\\";
            else if (c == '"') note += '\'';
            else note += c;
        }
        rows.push_back({std::to_string(r.lo), std::to_string(r.hi), CatName(r.cat), note});
    }
    return s;
}

}  // namespace coop::gamedata
