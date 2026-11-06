// SPDX-License-Identifier: GPL-3.0-or-later
// The boss fog gates and the area gate rules of the world actions (party_worldact.h BossGates / GateRules),
// derived from the event scripts of the player's own game:
//
//  * bossGates: every event named `ゲストがボス部屋入場` (the guest-entry event of a boss fog). Its 3[0] flag
//    conditions in order are the boss's defeat flag, the host's first-fight entry (the intro) and, last, the
//    host-entered flag the boss's start event waits on (an entry event with only two conditions has no intro: the
//    host's first-fight event `ホストがボス部屋入場_初戦` of that boss, which sets its own id, stands in); its 3[24]
//    action button is the fog object; the flag it sets to 1 is the guest-entered flag. A gate whose entry event sets no flag takes it from the landing event
//    (`ゲストが着地`, same file) that waits on the same host-entered flag.
//  * gateRules: every boss-defeat event (2003[12] / [15] / [53]) and the area gate flags (2000-3999) it sets to
//    1; the first event to set a gate flag owns it (later events of the same arena, the Omega / Patient B
//    re-fights, set the same flags). An event that sets a gate flag once two boss-defeat flags are both on
//    (the multiplayer permission control of an area with two bosses) gives a rule with `also`.
#include "derive.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace coop::gamedata {
namespace {

bool Has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

constexpr std::int64_t kGateLo = 2000, kGateHi = 3999;

std::vector<std::int64_t> FlagConds(const bbdata::EventRec& r) {
    std::vector<std::int64_t> out;
    for (const bbdata::Cond& c : r.conds) {
        if (c.bank == 3 && c.id == 0 && c.v.size() >= 4) out.push_back(c.v[3]);
    }
    return out;
}

bool SetsFlag(const bbdata::EventRec& r, std::int64_t flag) {
    for (const bbdata::Op& o : r.ops) {
        if (o.op == bbdata::FlagOp::Set && o.StateIs(1) && o.first == flag && o.last == flag) return true;
    }
    return false;
}

std::int64_t SetFlag(const bbdata::EventRec& r) {
    for (const bbdata::Op& o : r.ops) {
        if (o.op == bbdata::FlagOp::Set && o.StateIs(1) && o.first == o.last) return o.first;
    }
    return 0;
}

}  // namespace

Sections DeriveWorldAct(const std::vector<FileEvents>& events) {
    Sections s;
    // ---- gateRules ----
    {
        // (event id, gate flag), the first event to set a gate flag owns it
        std::set<std::int64_t> boss_events;
        std::map<std::int64_t, std::set<std::int64_t>> sets;  // boss-defeat event -> gate flags it sets to 1
        for (const FileEvents& fe : events) {
            for (const bbdata::EventRec& r : fe.recs) {
                if (!r.Has(bbdata::MarkerKind::BossDefeat) && !r.Has(bbdata::MarkerKind::MidbossDefeat)) continue;
                boss_events.insert(r.event);
                for (const bbdata::Op& o : r.ops) {
                    if (o.op == bbdata::FlagOp::Set && o.StateIs(1) && o.first == o.last && o.first >= kGateLo && o.first <= kGateHi) {
                        sets[r.event].insert(o.first);
                    }
                }
            }
        }
        std::set<std::int64_t> owned;
        std::vector<std::tuple<std::int64_t, std::int64_t, std::int64_t>> rules;  // boss, gate, also
        for (const auto& [ev, gates] : sets) {
            for (const std::int64_t g : gates) {
                if (owned.insert(g).second) rules.emplace_back(ev, g, 0);
            }
        }
        for (const FileEvents& fe : events) {
            for (const bbdata::EventRec& r : fe.recs) {
                if (boss_events.count(r.event)) continue;
                const std::int64_t g = SetFlag(r);
                const std::vector<std::int64_t> conds = FlagConds(r);
                if (g < kGateLo || g > kGateHi || conds.size() != 2 || !boss_events.count(conds[0]) || !boss_events.count(conds[1])) continue;
                if (owned.insert(g).second) rules.emplace_back(conds[0], g, conds[1]);
            }
        }
        std::sort(rules.begin(), rules.end());
        auto& rows = s["gateRules"];
        for (const auto& [b, g, a] : rules) rows.push_back({std::to_string(b), std::to_string(g), std::to_string(a)});
    }
    // ---- bossGates ----
    {
        struct Gate {
            std::int64_t entry, host, guest, boss, intro, fog, landing;
        };
        std::vector<Gate> gates;
        for (const FileEvents& fe : events) {
            std::set<std::int64_t> seen;
            for (const bbdata::EventRec& r : fe.recs) {
                if (!Has(r.name, "ゲストがボス部屋入場") || !seen.insert(r.event).second) continue;
                const std::vector<std::int64_t> conds = FlagConds(r);
                if (conds.size() < 2) continue;
                Gate g{r.event, conds.back(), SetFlag(r), conds[0], conds.size() >= 3 ? conds[1] : 0, 0, 0};
                for (const bbdata::Cond& c : r.conds) {
                    if (c.bank == 3 && c.id == 24 && c.v.size() >= 3) {
                        g.fog = c.v[2];
                        break;
                    }
                }
                if (!g.intro) {
                    // an entry event with no intro condition of its own: the host's first-fight event of the same
                    // boss, which marks itself done by setting the flag of its own id
                    for (const bbdata::EventRec& f : fe.recs) {
                        const std::vector<std::int64_t> fc = FlagConds(f);
                        if (Has(f.name, "ホストがボス部屋入場_初戦") && !fc.empty() && fc[0] == g.boss && SetsFlag(f, f.event)) {
                            g.intro = f.event;
                            break;
                        }
                    }
                }
                if (!g.guest) {
                    // the landing event (same file) that waits on the host-entered flag sets the guest's flag
                    for (const bbdata::EventRec& l : fe.recs) {
                        const std::vector<std::int64_t> lc = FlagConds(l);
                        if (Has(l.name, "ゲストが着地") && !lc.empty() && lc.back() == g.host && SetFlag(l)) {
                            g.guest = SetFlag(l);
                            g.landing = l.event;
                            break;
                        }
                    }
                }
                if (g.guest && g.fog) gates.push_back(g);
            }
        }
        auto& rows = s["bossGates"];
        for (const Gate& g : gates) {
            rows.push_back({std::to_string(g.entry), std::to_string(g.host), std::to_string(g.guest), std::to_string(g.boss),
                            std::to_string(g.intro), std::to_string(g.fog), std::to_string(g.landing)});
        }
    }
    return s;
}

Sections DeriveBossEntities(const std::vector<FileEvents>& events, const std::vector<bbdata::EventRec>& chalice) {
    // the boss-doping SpEffects (party_balance.cpp): an event that gives 7500 / 7501 to an entity
    constexpr std::int64_t kDopeA = 7500, kDopeB = 7501;
    auto dope = [&](const bbdata::EventRec& r, std::set<std::int64_t>* out) {
        for (const bbdata::Cond& c : r.conds) {
            if (c.bank == 2004 && c.id == 8 && c.v.size() >= 2 && (c.v[1] == kDopeA || c.v[1] == kDopeB) && c.v[0] > 0) out->insert(c.v[0]);
        }
    };
    std::set<std::int64_t> world, dungeon;
    for (const FileEvents& fe : events) {
        const bool area29 = fe.stem.rfind("m29", 0) == 0;  // the chalice dungeons' own scripts
        for (const bbdata::EventRec& r : fe.recs) dope(r, area29 ? &dungeon : &world);
    }
    for (const bbdata::EventRec& r : chalice) dope(r, &dungeon);
    Sections s;
    auto& rows = s["bossEntities"];
    for (const std::int64_t e : world) rows.push_back({std::to_string(e), "0"});
    for (const std::int64_t e : dungeon) {
        if (!world.count(e)) rows.push_back({std::to_string(e), "1"});
    }
    return s;
}

}  // namespace coop::gamedata
