// SPDX-License-Identifier: GPL-3.0-or-later
// Id allowlists and spots of the party (the former Python ids_tool / npcfog_tool / warper_tool).
#include "derive_ids.h"

#include "container.h"
#include "parallel.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstring>
#include <set>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace coop::gamedata {

std::string FixedText(double v, int digits) {
    char buf[96];
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, digits);
    return std::string(buf, r.ptr);
}

namespace {

using bbdata::MarkerKind;

std::int64_t Packed(std::int64_t a, std::int64_t b, std::int64_t c, std::int64_t d) { return a << 24 | b << 16 | c << 8 | d; }

bool Has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// People the party meets (MSB character parts): names with 会話 "talk" or NPC, never an object-like chr
// that carries a talk script nor an item placeholder; plus two named ones.
bool IsNpcName(const std::string& name) {
    const bool talk = Has(name, "会話") || Has(name, "NPC");
    const bool not_npc = Has(name, "アイテム") || Has(name, "ワープ") || Has(name, "メッセンジャー") || Has(name, "墓石") || Has(name, "ダミーCHR");
    return (talk && !not_npc) || name == "獣の守護者_デュラ" || name == "ヤマムラ";
}

constexpr double kDegToRad = 3.141592653589793 / 180.0;

Row SpotRow(std::int64_t id, const bbdata::MsbPoint& p) {
    return {std::to_string(id), FixedText(static_cast<double>(p.x), 4), FixedText(static_cast<double>(p.y), 4),
            FixedText(static_cast<double>(p.z), 4), FixedText(static_cast<double>(p.yaw_deg) * kDegToRad, 6)};
}

std::vector<Row> One(const std::set<std::int64_t>& vals) {
    std::vector<Row> rows;
    rows.reserve(vals.size());
    for (const std::int64_t v : vals) rows.push_back({std::to_string(v)});
    return rows;
}

}  // namespace

MsbInfo ReadMsb(const MsbFile& f) {
    MsbInfo info;
    if (f.b >= 10) {
        return info;  // Chalice layouts: no AAB id space
    }
    const bbdata::Bytes data = bbdata::ReadMaybeDcx(f.path);
    const bbdata::Span s(data);
    try {
        bbdata::ScanMsbEntities(s, f.a * 100000LL + f.b * 10000LL, &info.entities);
        if (f.c || f.d) {
            return info;
        }
        const std::vector<bbdata::MsbList> lists = bbdata::ParseMsbLists(s);
        if (const auto* ev = bbdata::FindMsbList(lists, "EVENT_PARAM_ST")) {
            for (const std::int64_t o : *ev) {
                const std::int64_t base = s.Le<std::int64_t>(o + 0x18), typ = s.Le<std::int64_t>(o + 0x20);
                if (!base || !typ || o + base + 12 > static_cast<std::int64_t>(s.n)) continue;
                const std::int32_t eid = s.Le<std::int32_t>(o + base + 8);
                std::int32_t region = -1;
                if (s.Has(o + typ, 4)) region = s.Le<std::int32_t>(o + typ);
                info.events.emplace_back(eid, region);
            }
        }
        if (const auto* pt = bbdata::FindMsbList(lists, "POINT_PARAM_ST")) {
            info.points = bbdata::ReadMsbPoints(s, *pt, false);
        }
        if (const auto* parts = bbdata::FindMsbList(lists, "PARTS_PARAM_ST")) {
            const std::int64_t lo = f.a * 100000LL + f.b * 10000LL;
            for (const std::int64_t o : *parts) {
                const std::int64_t name_off = s.Le<std::int64_t>(o);
                const std::int32_t typ = s.Le<std::int32_t>(o + 0x14);
                const std::int64_t ent_off = s.Le<std::int64_t>(o + 0xB0);
                if (typ != 2 || !(0 < ent_off) || o + ent_off + 4 > static_cast<std::int64_t>(s.n)) continue;
                const std::int32_t eid = s.Le<std::int32_t>(o + ent_off);
                const std::string name = name_off > 0 ? bbdata::Utf16z(s, o + name_off) : std::string();
                if (!(lo <= eid && eid < lo + 10000)) continue;
                if (IsNpcName(name)) info.npcs.push_back(eid);
            }
        }
    } catch (const bbdata::FormatError& e) {
        throw bbdata::FormatError(f.path + ": " + e.what());
    }
    return info;
}

Sections DeriveIds(const IdsInput& in) {
    const ParamSet& P = *in.params;
    const std::vector<MsbFile>& msbs = *in.msbs;
    const std::vector<MsbInfo>& infos = *in.infos;
    const std::set<std::int64_t>& maps = in.maps;

    // return points whose map exists
    const bbdata::ParamTable& rp_t = P.return_point;
    const int c_area = rp_t.Col("areaNo"), c_block = rp_t.Col("blockNo"), c_ent = rp_t.Col("returnPointEntityId");
    std::set<std::int64_t> rp;
    for (std::size_t r = 0; r < rp_t.Rows(); ++r) {
        const std::int64_t id = rp_t.ids[r];
        if (maps.count(Packed(rp_t.At(r, c_area), rp_t.At(r, c_block), 0, 0)) && id > 0) rp.insert(id);
    }
    std::map<std::int64_t, std::int64_t> rp_entity;  // id -> returnPointEntityId (last row of an id)
    for (std::size_t r = 0; r < rp_t.Rows(); ++r) {
        if (rp.count(rp_t.ids[r])) rp_entity[rp_t.ids[r]] = rp_t.At(r, c_ent);
    }

    // entity ids: the MSB words, plus the warp points the event scripts name, plus the return points
    std::set<std::int64_t> ents;
    for (const MsbInfo& i : infos) ents.insert(i.entities.begin(), i.entities.end());
    std::set<std::int64_t> e_remos, e_warps;
    for (const FileEvents& fe : *in.events) {
        for (const bbdata::EventRec& r : fe.recs) {
            for (const bbdata::Marker& m : r.markers) {
                const auto& v = m.v;
                if (m.kind == MarkerKind::Cutscene && m.sub != 8 && !v.empty() && v[0] > 0) e_remos.insert(v[0]);
                if (m.kind == MarkerKind::Cutscene && (m.sub == 2 || m.sub == 4 || m.sub == 6) && v.size() >= 3 && v[2] > 0) e_warps.insert(v[2]);
                if (m.kind == MarkerKind::MapMove && v.size() >= 3 && v[2] > 0) e_warps.insert(v[2]);
            }
        }
    }
    auto entity_map = [](std::int64_t e) { return Packed(e / 100000, e / 10000 % 10, 0, 0); };
    std::set<std::int64_t> warp_or_rp = e_warps;
    warp_or_rp.insert(rp.begin(), rp.end());
    for (const std::int64_t w : warp_or_rp) {
        if (1000000 <= w && w < 100000000 && maps.count(entity_map(w))) ents.insert(w);
    }
    std::set<std::int64_t> remos = in.remo_files;
    for (const std::int64_t r : e_remos) {
        if (0 < r && r < 100000000) remos.insert(r);
    }

    // where a lamp / headstone warp puts the player (the first MSB that holds the return point's event)
    std::set<std::int64_t> want_rp;
    for (const auto& [id, ent] : rp_entity) want_rp.insert(ent);
    std::map<std::int64_t, bbdata::MsbPoint> ret_spots, warp_spots;
    std::set<std::int64_t> want_warp;
    for (const std::int64_t w : e_warps) {
        if (1000000 <= w && w < 100000000) want_warp.insert(w);
    }
    std::set<std::int64_t> npcs;
    for (std::size_t k = 0; k < msbs.size(); ++k) {
        if (msbs[k].c || msbs[k].d || msbs[k].b >= 10) continue;
        const MsbInfo& i = infos[k];
        for (const auto& [eid, region] : i.events) {
            if (!want_rp.count(eid) || ret_spots.count(eid)) continue;
            if (!(0 <= region && static_cast<std::size_t>(region) < i.points.size())) continue;
            ret_spots[eid] = i.points[static_cast<std::size_t>(region)];
        }
        for (const bbdata::MsbPoint& p : i.points) {
            if (p.has_entity && want_warp.count(p.entity) && !warp_spots.count(p.entity)) warp_spots[p.entity] = p;
        }
        npcs.insert(i.npcs.begin(), i.npcs.end());
    }

    Sections s;
    s["maps"] = One(maps);
    s["returnPoints"] = One(rp);
    s["entities"] = One(ents);
    s["remos"] = One(remos);
    s["npcMirror"] = One(npcs);
    for (const auto& [id, ent] : rp_entity) {
        const auto it = ret_spots.find(ent);
        if (it != ret_spots.end()) s["returnSpots"].push_back(SpotRow(id, it->second));
    }
    for (const auto& [id, p] : warp_spots) s["warpSpots"].push_back(SpotRow(id, p));
    for (const char* n : {"returnSpots", "warpSpots"}) s[n];
    return s;
}

std::vector<FogCalls> CollectFogCalls(const std::string& stem, const bbdata::Emevd& evd) {
    FogCalls fc;
    fc.stem = stem;
    for (const bbdata::EvEvent& e : evd.events) {
        if (e.id != 0) continue;
        for (const bbdata::EvInstr& in : e.ins) {
            if (in.bank != 2000 || in.id != 0 || in.len < 8) continue;
            const bbdata::Span raw = evd.Args(in);
            std::vector<std::int32_t> ints(raw.n / 4);
            std::memcpy(ints.data(), raw.p, ints.size() * 4);
            if (ints[1] >= 12000000 && (ints[1] % 10000 == 4450 || ints[1] % 10000 == 4460)) fc.calls.push_back(std::move(ints));
        }
    }
    std::vector<FogCalls> out;
    if (!fc.calls.empty()) out.push_back(std::move(fc));
    return out;
}

Sections DeriveNpcFog(const std::vector<FogCalls>& calls, const std::string& msb_dir, unsigned threads) {
    namespace fs = std::filesystem;
    struct Region {
        float x, y, z, yaw;
    };
    std::vector<std::unordered_map<std::int32_t, Region>> regions(calls.size());
    std::vector<char> present(calls.size(), 0);
    ParallelFor(calls.size(), threads, [&](std::size_t k) {
        const fs::path p = fs::path(msb_dir) / (calls[k].stem + ".msb.dcx");
        std::error_code ec;
        if (!fs::exists(p, ec)) return;
        present[k] = 1;
        const bbdata::Bytes data = bbdata::ReadMaybeDcx(p.string());
        const bbdata::Span s(data);
        const auto lists = bbdata::ParseMsbLists(s);
        const auto* pts = bbdata::FindMsbList(lists, "POINT_PARAM_ST");
        if (!pts) throw bbdata::FormatError(p.string() + ": no POINT_PARAM_ST");
        for (const bbdata::MsbPoint& pt : bbdata::ReadMsbPoints(s, *pts, true)) {
            if (pt.entity > 0) regions[k][pt.entity] = {pt.x, pt.y, pt.z, pt.yaw_deg};
        }
    });
    std::vector<std::size_t> order(calls.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return calls[a].stem < calls[b].stem; });
    Sections s;
    s["npcFog"];
    for (const std::size_t k : order) {
        // npc -> call (a later call of the same npc replaces an earlier one)
        std::map<std::int32_t, const std::vector<std::int32_t>*> walk, enter;
        for (const auto& c : calls[k].calls) {
            if (c.size() < 7) throw bbdata::FormatError(calls[k].stem + ": short InitializeEvent");
            if (c[1] % 10000 == 4450) walk[c[2]] = &c;
            else enter[c[2]] = &c;
        }
        for (const auto& [npc, w] : walk) {
            const auto e = enter.find(npc);
            if (e == enter.end()) continue;
            const std::int32_t arena = (*e->second)[4];
            const auto r = regions[k].find(arena);
            if (!present[k] || r == regions[k].end()) continue;
            const Region& g = r->second;
            s["npcFog"].push_back({std::to_string(npc), std::to_string((*w)[4]), std::to_string((*w)[5]), std::to_string((*w)[6]),
                                   std::to_string(arena), FixedText(static_cast<double>(g.x), 2), FixedText(static_cast<double>(g.y), 2),
                                   FixedText(static_cast<double>(g.z), 2), FixedText(static_cast<double>(g.yaw), 1)});
        }
    }
    return s;
}

Sections DeriveWarper(const ParamSet& P) {
    struct Item {
        std::int64_t id, model;
    };
    auto one_per_model = [](std::vector<Item> v) {
        std::stable_sort(v.begin(), v.end(), [](const Item& a, const Item& b) { return a.id < b.id; });
        std::set<std::int64_t> seen;
        std::vector<Row> out;
        for (const Item& i : v) {
            if (!seen.insert(i.model).second) continue;
            out.push_back({std::to_string(i.id)});
        }
        return out;
    };
    const bbdata::ParamTable& w = P.weapon;
    const int c_sort = w.Col("sortId"), c_model = w.Col("equipModelId"), c_right = w.Col("rightHandEquipable"), c_left = w.Col("leftHandEquipable");
    std::vector<Item> right, left;
    for (std::size_t r = 0; r < w.Rows(); ++r) {
        const std::int64_t id = w.ids[r];
        if (!(id >= 1000000 && id % 10000 == 0 && w.At(r, c_sort) >= 0)) continue;
        if (w.At(r, c_right)) right.push_back({id, w.At(r, c_model)});
        else if (w.At(r, c_left)) left.push_back({id, w.At(r, c_model)});
    }
    const bbdata::ParamTable& p = P.protector;
    const int p_sort = p.Col("sortId"), p_model = p.Col("equipModelId"), p_head = p.Col("headEquip");
    std::vector<Item> heads;
    for (std::size_t r = 0; r < p.Rows(); ++r) {
        if (p.ids[r] >= 10000 && p.At(r, p_head) && p.At(r, p_sort) >= 0) heads.push_back({p.ids[r], p.At(r, p_model)});
    }
    Sections s;
    s["warperRight"] = one_per_model(right);
    s["warperLeft"] = one_per_model(left);
    s["warperArmor"] = one_per_model(heads);
    return s;
}

}  // namespace coop::gamedata
