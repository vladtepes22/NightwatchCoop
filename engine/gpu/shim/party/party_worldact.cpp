// SPDX-License-Identifier: GPL-3.0-or-later
// D3 / D8: a guest's actions change the host's world (see party_worldact.h,
// docs/party/world_actions.md).
#include "party_worldact.h"

#include "party_progress.h"
#include "party_bossfog_rules.h"
#include "net/json.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace coop::worldact {

using progress::Category;

bool NpcGiftTrigger(std::uint32_t id) {
    if (id < 9042u || id > 9077u) {
        return false;
    }
    // The common 9040 slots (common.emevd event 0: InitializeEvent(slot, 9040, flag, lot)).
    return !(id == 9043u || id == 9044u || id == 9053u || id == 9065u || id == 9068u || id == 9070u);
}

std::int32_t RepeatGiftLot(std::uint32_t trigger) {
    // common.emevd event 0: InitializeEvent(slot 0..4, 9100, trigger, lot).
    switch (trigger) {
    case 72410359u: return 24060;  // Blue Elixir x3
    case 72410310u: return 23000;  // Iosefka's Blood Vial
    case 72400398u: return 32020;  // Blood of Adella
    case 72400489u: return 28000;  // Blood of Arianna
    case 9043u: return 17010;      // Queenly Flesh
    default: return -1;
    }
}

std::int32_t OwnTalkGiftLot(std::uint32_t trigger) {
    // m27 event 12700906 (`DLC_NPCアイテム入手XX`, slots 0 / 1): trigger flag + the lot's personal flag.
    switch (trigger) {
    case 72700321u: return 43120;  // Valtr: the Impurity rune (GemGen 200030, flag 6676)
    case 72700322u: return 43130;  // Valtr: the League Staff (goods 131, flag 6678)
    default: return -1;
    }
}

bool WorldActionAllowed(std::uint32_t id, bool clear, std::uint32_t host_map) {
    if (RepeatGiftLot(id) > 0) {
        return false;  // P2: the guest's own gift (its own grant), never the host's 9100
    }
    if (OwnTalkGiftLot(id) > 0) {
        return false;  // 0.7.3: the guest's own gift; the host's 12700906 would give it to the host
    }
    const int key = progress::PoolKeyOfId(id);
    if (key < 0 || !progress::WorldMirrorKey(key) || progress::NeverWritable(id) ||
        !progress::GetBit(progress::WorldMirrorMask(key).data(), id % progress::kBitsPerBlock)) {
        return false;
    }
    if (NpcGiftTrigger(id)) {
        return !clear;  // set only: the host's own 9040 awards the gift once (P1)
    }
    // 0.7.3: covenant outcomes are per player while the host's rule is on (the host's world keeps
    // its own; the guest's change stays in its overlay). Off: the base category, as before.
    const Category c = progress::EffectiveCategory(id, progress::CovenantPerPlayer());
    // Elevator / two-state object flags (C2's state ids, e.g. 12410350/351 Central Yharnam's lift):
    // the lift's own network-synced events and the replicated ObjAct kick move it on the host; a
    // guest's value, arriving later, would fight the host's ride. A lift ride the host's game did
    // not run (its block streamed out there) goes up as Request::lift_set / lift_clr (lifts071).
    if (c != Category::npc_quest &&
        progress::ModeFor(progress::Policy::Defaults(), id, c) == progress::SyncMode::State) {
        return false;
    }
    bool global_ok = false;
    switch (c) {
    case Category::shortcut_door:
    case Category::world_state:
        break;
    case Category::npc_quest:
        // NPC quests are state machines: a step clears the old state flag and sets the new one
        // (Iosefka live: 1141 off, 1143 on); both halves go to the host.
        global_ok = true;
        break;
    case Category::key_event:
    case Category::cutscene_seen:  // D10: a scene a guest's event played is seen in the host's world too
        if (clear) {
            return false;
        }
        global_ok = true;
        break;
    default:
        return false;
    }
    const std::uint32_t area = id >= 10000000u ? id / 100000u % 100u : 0u;
    if (area == 0) {
        return global_ok;  // 1000-1999 NPC state, 5000-5299 key items ...; never a door of no map
    }
    if (host_map == 0xffffffffu) {
        return false;
    }
    return area == (host_map >> 24);  // the guest acts in the host's world: the host's area
}

bool BossKillActionAllowed(std::uint32_t id, bool clear, std::uint32_t member_map, bool apart) {
    if (!apart || clear || member_map == 0xffffffffu || id < 10000000u ||
        progress::CategoryOf(id) != Category::boss_defeated || progress::NeverWritable(id)) {
        return false;
    }
    return id / 100000u % 100u == (member_map >> 24);
}

std::string RequestToJson(const Request& r) {
    json::Value v = json::Value::make_object();
    v.set("v", 1u);
    v.set("map", static_cast<unsigned>(r.map));
    json::Value s = json::Value::make_array(), c = json::Value::make_array();
    for (std::uint32_t id : r.set) {
        s.push(static_cast<unsigned>(id));
    }
    for (std::uint32_t id : r.clr) {
        c.push(static_cast<unsigned>(id));
    }
    v.set("set", std::move(s));
    v.set("clr", std::move(c));
    if (!r.kick.empty()) {
        json::Value k = json::Value::make_array();
        for (std::int32_t id : r.kick) {
            k.push(static_cast<unsigned>(id));
        }
        v.set("kick", std::move(k));
    }
    if (!r.fog.empty()) {
        json::Value f = json::Value::make_array();
        for (std::uint32_t id : r.fog) {
            f.push(static_cast<unsigned>(id));
        }
        v.set("fog", std::move(f));
    }
    if (!r.story.empty()) {
        json::Value f = json::Value::make_array();
        for (std::uint32_t id : r.story) {
            f.push(static_cast<unsigned>(id));
        }
        v.set("story", std::move(f));
    }
    for (const auto& [name, ids] : {std::make_pair("lift_set", &r.lift_set), std::make_pair("lift_clr", &r.lift_clr)}) {
        if (!ids->empty()) {
            json::Value f = json::Value::make_array();
            for (std::uint32_t id : *ids) {
                f.push(static_cast<unsigned>(id));
            }
            v.set(name, std::move(f));
        }
    }
    return json::dump(v, 0);
}

bool RequestFromJson(const std::string& text, Request* out, std::string* error) {
    json::Value v;
    std::string err;
    if (text.size() > 4096 || !json::parse(text, v, err)) {
        if (error) {
            *error = err.empty() ? "too long" : err;
        }
        return false;
    }
    try {
        if (v.type != json::Value::Type::Object || json::u64(v, "v") != 1) {
            throw std::runtime_error("bad world_action body");
        }
        Request r;
        const std::uint64_t map = json::u64(v, "map");
        if (map > 0xffffffffull) {
            throw std::runtime_error("bad map");
        }
        r.map = std::uint32_t(map);
        const auto& set = json::arr(v, "set");
        const auto& clr = json::arr(v, "clr");
        const json::Value* kick = v.find("kick");
        if (kick) {
            if (kick->type != json::Value::Type::Array || kick->array.size() > kMaxKicks) {
                throw std::runtime_error("bad kick list");
            }
            for (const json::Value& x : kick->array) {
                const std::uint64_t id = json::as_u64(x, "kick");
                if (id == 0 || id > 99999999ull) {
                    throw std::runtime_error("bad kick event id");
                }
                if (std::find(r.kick.begin(), r.kick.end(), std::int32_t(id)) != r.kick.end()) {
                    throw std::runtime_error("duplicate kick");
                }
                r.kick.push_back(std::int32_t(id));
            }
        }
        if (const json::Value* fog = v.find("fog")) {
            if (fog->type != json::Value::Type::Array || fog->array.size() > kMaxFog) {
                throw std::runtime_error("bad fog list");
            }
            for (const json::Value& x : fog->array) {
                const std::uint64_t id = json::as_u64(x, "fog");
                if (id > 0xffffffffull || !BossGateByGuestFlag(std::uint32_t(id))) {
                    throw std::runtime_error("not a boss fog flag");
                }
                if (std::find(r.fog.begin(), r.fog.end(), std::uint32_t(id)) != r.fog.end()) {
                    throw std::runtime_error("duplicate fog");
                }
                r.fog.push_back(std::uint32_t(id));
            }
        }
        if (const json::Value* story = v.find("story")) {
            if (story->type != json::Value::Type::Array || story->array.size() > kMaxStory) {
                throw std::runtime_error("bad story list");
            }
            for (const json::Value& x : story->array) {
                const std::uint64_t id = json::as_u64(x, "story");
                if (id > 0xffffffffull || !StoryTriggerByEvent(std::uint32_t(id))) {
                    throw std::runtime_error("not a story trigger");
                }
                if (std::find(r.story.begin(), r.story.end(), std::uint32_t(id)) != r.story.end()) {
                    throw std::runtime_error("duplicate story");
                }
                r.story.push_back(std::uint32_t(id));
            }
        }
        for (const auto& [name, to] : {std::make_pair("lift_set", &r.lift_set), std::make_pair("lift_clr", &r.lift_clr)}) {
            const json::Value* lf = v.find(name);
            if (!lf) {
                continue;
            }
            if (lf->type != json::Value::Type::Array || lf->array.size() > kMaxLift) {
                throw std::runtime_error("bad lift list");
            }
            for (const json::Value& x : lf->array) {
                const std::uint64_t id = json::as_u64(x, "lift");
                if (id > 0xffffffffull || !LiftByPosFlag(std::uint32_t(id))) {
                    throw std::runtime_error("not a lift position flag");
                }
                if (std::find(r.lift_set.begin(), r.lift_set.end(), std::uint32_t(id)) != r.lift_set.end() ||
                    std::find(r.lift_clr.begin(), r.lift_clr.end(), std::uint32_t(id)) != r.lift_clr.end()) {
                    throw std::runtime_error("duplicate lift");
                }
                to->push_back(std::uint32_t(id));
            }
        }
        if (set.size() > kMaxSet || clr.size() > kMaxClear ||
            (set.empty() && clr.empty() && r.kick.empty() && r.fog.empty() && r.story.empty() && r.lift_set.empty() &&
             r.lift_clr.empty())) {
            throw std::runtime_error("bad world_action size");
        }
        auto take = [](const std::vector<json::Value>& a, bool clear, std::vector<std::uint32_t>* to) {
            for (const json::Value& x : a) {
                const std::uint64_t id = json::as_u64(x, "id");
                if (id > 0xffffffffull) {
                    throw std::runtime_error("bad id");
                }
                const std::uint32_t id32 = std::uint32_t(id);
                const std::uint32_t area = id32 >= 10000000u ? id32 / 100000u % 100u : 21u;
                if (!WorldActionAllowed(id32, clear, area << 24) && !BossKillActionAllowed(id32, clear, area << 24, true)) {
                    throw std::runtime_error("flag not allowed: " + std::to_string(id32));
                }
                if (std::find(to->begin(), to->end(), id32) != to->end()) {
                    throw std::runtime_error("duplicate id");
                }
                to->push_back(id32);
            }
        };
        take(set, false, &r.set);
        take(clr, true, &r.clr);
        for (std::uint32_t id : r.set) {
            if (std::find(r.clr.begin(), r.clr.end(), id) != r.clr.end()) {
                throw std::runtime_error("id both set and cleared");
            }
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

bool RateLimit::Take(int slot, int n, double now) {
    if (slot < 0 || slot >= 8 || n < 0 || n > kRateFlags || !std::isfinite(now)) {
        return false;
    }
    Slot& s = slots_[slot];
    if (now - s.start >= kRateWindow || now < s.start) {
        s.start = now;
        s.used = 0;
    }
    if (s.used + n > kRateFlags) {
        return false;
    }
    s.used += n;
    return true;
}

bool WorldTalkAllowed(std::uint32_t talk) {
    if (talk < 100000 || talk > 999999) {
        return false;
    }
    const std::uint32_t map = talk / 1000;  // area * 10 + block
    if (map / 10 == 21) {
        return false;  // the Hunter's Dream: guest services' own list (Gehrman's ending is never a guest's)
    }
    if (map >= 210 && map <= 369 && map % 10 <= 2 && talk % 1000 <= 3) {
        return false;  // world lamps (LampTalk): lamp requests
    }
    return map >= 220 && map <= 369;  // the world maps' NPCs (Gilbert, Eileen, Alfred, Iosefka, ...)
}

const std::vector<GateRule>& GateRules() {
    // From every EMEVD boss-defeat event (2003[12]/[15]/[53]): its own flag and the 2000-3999
    // flags it sets (tools: flag_tool expand_all; 12411899 sets 2410 once both Central Yharnam
    // bosses are dead).
    static const std::vector<GateRule> rules = {
        {12201800, 2200}, {12301700, 2301}, {12301800, 2300}, {12401800, 2400}, {12401800, 2401},
        {12401800, 2402}, {12411700, 2411}, {12411800, 2412}, {12411800, 2410, 12411700},
        {12421700, 2420}, {12421800, 2421}, {12501800, 2500}, {12601800, 2601}, {12601850, 2600},
        {12701800, 2700}, {12701800, 2701}, {12801800, 2800}, {13201800, 3200}, {13301800, 3300},
        {13401800, 3400}, {13401800, 3401}, {13401800, 3402}, {13401800, 3403}, {13501800, 3510},
        {13501800, 3511}, {13501800, 3512}, {13501800, 3513}, {13501850, 3517}, {13601800, 3600},
    };
    return rules;
}

const std::vector<BossGate>& BossGates() {
    // flag_tool.py emevd --all-ops, every `ゲストがボス部屋入場` event: {event, its last 3[0] flag
    // (= the flag `ボスが動き出す` waits on), the flag it sets, its boss flag}.
    // intro_flag: the 3[0] ON condition before the host-entered one (the host's first-fight entry
    // event, an intro cutscene for most); fog_entity: the 3[24] action-button object (the fog).
    static const std::vector<BossGate> gates = {
        {12204891, 12204800, 12204801, 12201800, 12201802, 2201800},
        {12304811, 12304800, 12304801, 12301800, 12301802, 2301800},
        {12304731, 12304700, 12304701, 12301700, 12301702, 2301810},
        {12404841, 12404800, 12404801, 12401800, 12401802, 2401800},
        {12414731, 12414700, 12414701, 12411700, 12411702, 2411800},
        {12414811, 12414800, 12414801, 12411800, 12411802, 2411810},
        {12424811, 12424800, 12424801, 12421800, 12421802, 2421800},
        {12424711, 12424700, 12424701, 12421700, 0, 2421700},
        {12504811, 12504800, 12504801, 12501800, 12501802, 2501800},
        {12604846, 12604800, 12604801, 12601800, 12601802, 2601800},
        {12604861, 12604850, 12604851, 12601850, 12601852, 2601850},
        {12704841, 12704800, 12704801, 12701800, 12701802, 2701800},
        {12804881, 12804800, 12804801, 12801800, 12801802, 2801800},
        // Rom (m32_00): the fog 3201800 asks for the host's entry like the others (13204831 waits on
        // 13201802 + 13204800); the guest's flag comes from the landing event 13204821 in the arena.
        {13204831, 13204800, 13204801, 13201800, 13201802, 3201800, 13204821},
        {13304871, 13304800, 13304801, 13301800, 13301802, 3301800},
        {13404801, 13404808, 13404809, 13401800, 13401801, 3401800},
        {13404851, 13404858, 13404859, 13401850, 13401851, 3401850},
        {13504801, 13504808, 13504809, 13501800, 13501801, 3501800},
        {13504851, 13504858, 13504859, 13501850, 13501851, 3501810},
        {13604801, 13604808, 13604809, 13601800, 13601801, 3601800},
    };
    return gates;
}

const BossGate* BossGateByEvent(std::int32_t guest_event) {
    for (const BossGate& g : BossGates()) {
        if (static_cast<std::int32_t>(g.guest_event) == guest_event) {
            return &g;
        }
    }
    return nullptr;
}

const BossGate* BossGateByLandingEvent(std::int32_t event_id) {
    for (const BossGate& g : BossGates()) {
        if (g.landing_event && static_cast<std::int32_t>(g.landing_event) == event_id) {
            return &g;
        }
    }
    return nullptr;
}

const BossGate* BossGateByGuestFlag(std::uint32_t guest_flag) {
    for (const BossGate& g : BossGates()) {
        if (g.guest_flag == guest_flag) {
            return &g;
        }
    }
    return nullptr;
}

bool BossGateSkipCondition(std::int32_t event_id, int bank, int id, std::uint32_t flag) {
    if (bank != 3 || id != 0) {
        return false;
    }
    const BossGate* g = BossGateByEvent(event_id);
    if (!g) {
        g = BossGateByLandingEvent(event_id);
    }
    return g && g->host_flag == flag;
}

bool BossGateWaitsForHost(const BossGate& g, bool intro_done, bool host_in, bool boss_dead) {
    return g.intro_flag != 0 && !intro_done && !host_in && !boss_dead;
}

bool GuestFogOpenWhileHostAt(const BossGate& g, std::uint32_t host_map) {
    if (host_map == 0 || host_map == 0xffffffffu) {
        return true;
    }
    return (host_map >> 24) == g.host_flag / 100000u % 100u;
}

bool BossFogNoticeDue(const BossGate& g, bool read, bool intro_done, bool host_in, bool boss_dead, float dist_m) {
    return read && BossGateWaitsForHost(g, intro_done, host_in, boss_dead) && dist_m >= 0.0f &&
           dist_m <= kBossFogNoticeRange;
}

const float* PickPlayerPos(bool physics_ok, const float* physics, bool drawn_ok, const float* drawn) {
    return physics_ok && physics ? physics : drawn_ok && drawn ? drawn : nullptr;
}

bool BossGateInBlock(const BossGate& g, std::uint32_t map) {
    const std::uint32_t area = g.host_flag / 100000 % 100, block = g.host_flag / 10000 % 10;
    return map != 0xffffffffu && (map >> 24) == area && ((map >> 16) & 0xff) == block;
}

bool BossGateHostOpens(const BossGate& g, bool guest_in, bool host_in, bool boss_dead, std::uint32_t host_map) {
    const std::uint32_t area = g.host_flag / 100000 % 100, block = g.host_flag / 10000 % 10;
    return guest_in && !host_in && !boss_dead && host_map != 0xffffffffu && (host_map >> 24) == area &&
           ((host_map >> 16) & 0xff) == block;
}

bool BossGateGuestStarts(const BossGate& g) {
    // The guest-entry events whose host first-fight event (EMEVD, flag_tool emevd + an instruction
    // dump) has one position condition (bossfog::FirstFights, `entry`): Witch of Hemwick 12201802
    // (3[2] 2202805), Blood-starved Beast 12301802 (3[2] 2302805), Darkbeast Paarl 12301702 (3[3]
    // 16 m), Vicar Amelia 12401802 (3[2] 2402805, scene 24000060), Cleric Beast 12411702 (3[2]
    // 2412805), Father Gascoigne 12411802 (3[2] 2412815, scene 24010010), Logarius 12501802 (3[2]
    // 2502805, scene 25000020), Mergo's Wet Nurse 12601802 (3[2] 2602805, scene 26000010), Micolash
    // 12601852 (3[3] 32 m, scene 26000060), Shadows of Yharnam 12701802 (3[2] 2702805), the One
    // Reborn 12801802 (3[2] 2802805, scene 28000000), Amygdala 13301802 (3[2] 3302805). DLC
    // (pard2): Ludwig 13404801 (13404811 + 13401801 both 3[2] 3402805, scene 34000020), Living
    // Failures 13504851 (13501851 3[2] 3502815, no scene), the Orphan of Kos 13604801 (13604811 +
    // 13601801 3[2] 3602805, scene 36000000). Not Laurence: his first fight needs goods 4014
    // Laurence's Skull held by the host at the altar (13404861 / 13401851 3[4] + 3[2] 3402855), an
    // action in the room, not the walk in (`action`, like Ebrietas and Lady Maria).
    return g.intro_flag != 0 && bossfog::GuestMayStartFirstFight(g.guest_event);
}

bool BossGateSkipFirstFight(std::int32_t event_id, int bank, int id, std::uint32_t flag) {
    if (bank != 3 || id != 0) {
        return false;
    }
    const BossGate* g = BossGateByEvent(event_id);
    return g && BossGateGuestStarts(*g) && g->intro_flag == flag;
}

const BossGate* BossGateByIntroEvent(std::int32_t event_id) {
    for (const BossGate& g : BossGates()) {
        if (g.intro_flag != 0 && static_cast<std::int32_t>(g.intro_flag) == event_id) {
            return &g;
        }
    }
    return nullptr;
}

const FirstFightSpot* FirstFightSpotFor(std::uint32_t intro_event) {
    // flag_tool / MSB: the region of each first-fight event's 3[2] (origin y + 0.5 m), or 2 m off
    // the 3[3] target's MSB spot (Darkbeast Paarl 2300810 within 16 m, Micolash 2600850 within 32 m).
    static const FirstFightSpot spots[] = {
        {12201802, {-336.14f, 2.10f, 726.64f}},    // Witch of Hemwick, region 2202805
        {12301802, {-136.33f, -126.33f, 53.89f}},  // Blood-starved Beast, region 2302805
        {12301702, {113.50f, -120.31f, -63.68f}},  // Darkbeast Paarl, 2300810
        {12401802, {57.70f, 35.21f, 322.60f}, false, true},     // Vicar Amelia, region 2402805
        {12411702, {-136.52f, -26.92f, 52.77f}},   // Cleric Beast, region 2412805
        {12411802, {-48.76f, -40.42f, 72.44f}, false, true},    // Father Gascoigne, region 2412815
        {12501802, {62.81f, 109.37f, -371.63f}, false, true},   // Martyr Logarius, region 2502805
        {12601802, {146.46f, 1124.75f, -37.90f}, false, true},  // Mergo's Wet Nurse, region 2602805
        {12601852, {176.82f, 1038.48f, -35.82f}, false, true},  // Micolash, 2600850
        {12701802, {-335.60f, -187.48f, 485.44f}}, // Shadows of Yharnam, region 2702805
        {12801802, {402.90f, -125.00f, -237.50f}, false, true}, // the One Reborn, region 2802805
        {13301802, {-98.85f, 1461.50f, -46.33f}},  // Amygdala, region 3302805
        {13401801, {-407.93f, 1503.50f, -721.36f}, true, true},  // Ludwig, region 3402805 (two parts)
        {13401851, {-433.10f, 1536.11f, -261.59f}, true, true},  // Laurence, region 3402855 (two parts)
        {13501851, {-380.91f, 1593.30f, -824.55f}},        // Living Failures, region 3502815
        {13601801, {-700.20f, 1575.90f, -956.20f}, true, true},  // the Orphan of Kos, region 3602805 (two parts)
    };
    for (const FirstFightSpot& f : spots) {
        if (f.intro_event == intro_event) {
            return &f;
        }
    }
    return nullptr;
}

const std::vector<StoryTrigger>& StoryTriggers() {
    // m32_00 13201803 after Rom (13201800 -> label 1): 4[3] chr type 0 + 3[3] 12 m to 3200801
    // (MSB part `ダンジョン人花嫁の幻影`, (-510.86, -271.25, 370.35)); the Moonside Lake lamp 3202952
    // stands 2 m from her (its ReturnPointParam spot, raised 0.5 m).
    static const std::vector<StoryTrigger> t = {
        {13201803u, 13201800u, 70002802u, {-510.86f, -271.25f, 370.35f}, 12.0f, {-508.98f, -270.74f, 371.03f}},
    };
    return t;
}

const StoryTrigger* StoryTriggerByEvent(std::uint32_t event) {
    for (const StoryTrigger& t : StoryTriggers()) {
        if (t.event == event) {
            return &t;
        }
    }
    return nullptr;
}

bool StoryTriggerInBlock(const StoryTrigger& t, std::uint32_t map) {
    const std::uint32_t area = t.event / 100000u % 100u, block = t.event / 10000u % 10u;
    return map != 0xffffffffu && (map >> 24) == area && ((map >> 16) & 0xffu) == block;
}

bool StoryTriggerDue(const StoryTrigger& t, std::uint32_t map, bool needs_on, bool done_on, bool event_done,
                     const float* me) {
    if (!me || !StoryTriggerInBlock(t, map) || !needs_on || done_on || event_done) {
        return false;
    }
    const float dx = me[0] - t.centre[0], dy = me[1] - t.centre[1], dz = me[2] - t.centre[2];
    return dx * dx + dy * dy + dz * dz <= t.radius * t.radius;
}

const KeyItemGate* KeyItemGateFor(std::int32_t event_id) {
    // flag_tool emevd + an instruction dump of m24_00 12400760: 3[4] group 1 goods 4011 state 0,
    // group 2 state 1, both with 3[24] action button 2400030 at 2401220; group 3 = 5[2] ObjAct
    // 12400170 (the far-side lever: the gate opens, no player animation); sets 12400160.
    static const KeyItemGate gates[] = {
        {12400760, 4011, 52400450u, 2, 12400160u, 12400170},
    };
    for (const KeyItemGate& g : gates) {
        if (g.event == event_id) {
            return &g;
        }
    }
    return nullptr;
}

const KeyItemGate* KeyItemGateByOpenFlag(std::uint32_t open_flag) {
    const KeyItemGate* g = KeyItemGateFor(12400760);
    return g && g->open_flag == open_flag ? g : nullptr;
}

std::uint8_t KeyItemGateHeldState(const KeyItemGate& g, int group, bool invert) {
    return static_cast<std::uint8_t>((group == g.held_group) != invert ? 1 : 0);
}

bool KeyItemGateInvert(bool summoned_guest, bool own_flag, bool host_world_flag, int held) {
    return summoned_guest && (own_flag || host_world_flag) && held <= 0;
}

bool FirstFightVisitDone(bool host_flag_on, bool flag9180_on, bool cutscene, double visit_s, bool chained,
                         bool scene) {
    return host_flag_on || (flag9180_on && !chained && !scene) || cutscene || visit_s >= kFirstFightVisitSeconds;
}

bool FirstFightPutBackAfterScene(const float here[3], const float back[3], const float spot[3]) {
    const auto dist = [](const float* a, const float* b) {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    return dist(here, back) > kFirstFightBackSlack && dist(here, spot) < kFirstFightSceneRadius;
}

bool FirstFightHostReady(int host_hp, bool boss_bar_up, bool cutscene) {
    return host_hp > 0 && !boss_bar_up && !cutscene;
}

FogAction HostFogAction(const BossGate& g, bool guest_starts_on, bool intro_done, bool host_in, bool boss_dead,
                        std::uint32_t host_map) {
    if (boss_dead || host_in) {
        return FogAction::none;
    }
    if (g.intro_flag == 0 || intro_done) {
        return BossGateHostOpens(g, true, host_in, boss_dead, host_map) ? FogAction::open : FogAction::none;
    }
    // First fight: writing host_flag alone would start a boss whose first-fight event never ran
    // (Amelia stays disabled until it does) and replay its scene when the host walks in later.
    if (!guest_starts_on || !BossGateGuestStarts(g)) {
        return FogAction::none;
    }
    return BossGateInBlock(g, host_map) ? FogAction::start_first : FogAction::wait_host;
}

} // namespace coop::worldact

#ifndef BB_PARTY_WORLDACT_NO_GAME

#include "coop_hooks.h"
#include "party_b2.h"
#include "party_balance.h"
#include "party_death_rules.h"
#include "game_state.h"
#include "party_director.h"
#include "party_link.h"
#include "party_guestsvc.h"
#include "party_status.h"
#include "party_droproll.h"
#include "party_items.h"
#include "seamless_rules.h"
#include "party_bossfog_rules.h"
#include "bbport_present_hold.h"
#include "engine/frame_rate.h"
#include "party_lift_watch.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <map>
#include <set>
#include <array>

extern "C" void runtime_pad_director_press(std::uint32_t buttons); // src/runtime_pad.c

namespace coop::worldact {
namespace {
// liftre077: when this game last saw a lift car it rides play a ride animation (LiftRiderTick), Now() s
std::atomic<double> g_lift_ride_seen{-1e9};

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[640];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("[coop] Party: world: %s\n", line);  // "Party: ": the harness reaper counts it as activity
    std::fflush(stdout);
}

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// D3 (pard4): the host's map as this guest last knew it (party link vitals, b2::MemberMap(0)),
// for the EMEVD dispatch's fog decision (GuestFogSkipAllowed); 0 = unknown.
std::atomic<std::uint32_t> g_fog_host_map{0};
// A guest that went through a fog while the host was in another area: put back outside then.
struct FogPutBack {
    std::int32_t fog_entity = 0;
    std::uint32_t guest_event = 0;
    std::uint32_t guest_flag = 0;
    double at = 0;
};
std::mutex g_putback_mu;
std::vector<FogPutBack> g_putback;

/// A tag that differs from step to step without digits: the harness reaper blanks numbers when it
/// counts distinct "Party:" lines, so a fixture's repeated steps need words to stay "active".
std::string StepTag(int n) {
    std::string t;
    n = n < 0 ? 0 : n;
    do {
        t.insert(t.begin(), char('a' + n % 26));
        n /= 26;
    } while (n > 0);
    return "step-" + t;
}

std::vector<std::uint32_t> ParseIds(const char* v) {
    std::vector<std::uint32_t> out;
    while (v && *v) {
        char* end = nullptr;
        const unsigned long x = std::strtoul(v, &end, 10);
        if (end == v) {
            break;
        }
        out.push_back(std::uint32_t(x));
        v = *end ? end + 1 : end;
    }
    return out;
}

struct State {
    std::once_flag once;
    bool enabled = false;
    std::vector<std::uint32_t> watch;  // BB_PARTY_WORLD_WATCH
    std::mutex mu;
    std::deque<std::pair<int, Request>> in;  // host: from guests (link thread -> tick)
    RateLimit rate;
    // guest (main thread)
    std::uint32_t map = 0xffffffffu;
    double steady_since = 0, last_scan = 0;
    bool steady = false, baselined = false;
    std::set<std::uint64_t> known;  // id * 2 + value: local changes already sent or held back
    std::set<std::uint32_t> fog_in;  // guest: boss fogs it went through (B2), reported once
    std::set<std::uint32_t> fog_apart;  // guest: put back out of that boss room this stay (D3)
    std::set<std::uint32_t> fog_out;    // guest: guest-entered flags seen 0 this stay (bossfog rules)
    std::set<std::uint32_t> fog_stale;  // guest: ... already 1 at the stay's start: not a walk-through
    std::set<std::uint32_t> story_sent;  // guest: story triggers it reported (D10), once per map
    std::map<std::uint32_t, bool> lift_pending;  // guest: lift pos flag -> the end its own ride goes to (D3)
    std::set<std::uint64_t> lift_baseline;       // guest: lift flag * 2 + value held back by the baseline
    std::map<std::uint32_t, int> lift_sent;      // guest: lift pos flag -> the end last sent to the host
    bool pickups = false;           // D4 patches in: the guest sees and takes the host world's treasure
    Stats stats;
} g_st;

// Guest: kick event ids of the local player's ObjAct kicks (LocalKickEntry, main thread) for the
// next world_action. Host: remote kicks its game applied (NetKickEntry): kick event id -> time.
// MSB treasure spawns seen on this game (0x17DC300(MapItemMan, entity handle, MsbEvent, pos)):
// the fixture's pickup targets. Main thread.
std::atomic<std::uint64_t> g_pickups{0};  // map item pickups on this game (PickupEntry)
std::atomic<double> g_pickup_at{-1e9};     // Now() of the last one
// P1: NPC gift triggers (NpcGiftTrigger) this game's own talk set (SetEventFlag hook, game thread);
// the guest tick sends them to the host while it stands in the host's world, else drops them.
std::mutex g_gift_mu;
std::vector<std::uint32_t> g_gift_sets;
std::vector<std::uint32_t> g_repeat_sets;  // P2: repeatable gift triggers (RepeatGiftLot) this game set
std::vector<std::uint32_t> g_own_sets;     // 0.7.3: own talk gift triggers (OwnTalkGiftLot) this game set
std::mutex g_kick_mu;
std::vector<std::int32_t> g_kicks_out;
std::atomic<std::uint32_t> g_keygate_own{0};  // guest: a key-item gate it opened (own save, GuestTick)
void OnLocalGiftSet(std::uint32_t id) {
    if (const KeyItemGate* kg = KeyItemGateByOpenFlag(id); kg && LocalSessionRole() == 6) {
        // D6 (pard3, live P1b): the guest's copy of the gate's event opened it with its own key
        // (KeyItemGate). The flag is a T1 zone-0 one the world mirror does not carry, so it never
        // went up: the host gets the gate's far-side ObjAct kick (its copy opens with the
        // animation and sets the flag; the mirror and C2 bring it back), the guest's own save
        // the flag now.
        std::lock_guard<std::mutex> lk(g_kick_mu);
        if (g_kicks_out.size() < 16) {
            g_kicks_out.push_back(kg->far_kick);
        }
        g_keygate_own.store(id);
        return;
    }
    if (RepeatGiftLot(id) > 0) {
        std::lock_guard<std::mutex> lk(g_gift_mu);
        if (g_repeat_sets.size() < 16) {
            g_repeat_sets.push_back(id);
        }
        return;
    }
    if (OwnTalkGiftLot(id) > 0) {
        std::lock_guard<std::mutex> lk(g_gift_mu);
        if (g_own_sets.size() < 16 && std::find(g_own_sets.begin(), g_own_sets.end(), id) == g_own_sets.end()) {
            g_own_sets.push_back(id);
        }
        return;
    }
    if (!NpcGiftTrigger(id)) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_gift_mu);
    if (g_gift_sets.size() < 16 && std::find(g_gift_sets.begin(), g_gift_sets.end(), id) == g_gift_sets.end()) {
        g_gift_sets.push_back(id);
    }
}
std::vector<std::uint32_t> TakeGiftSets() {
    std::lock_guard<std::mutex> lk(g_gift_mu);
    std::vector<std::uint32_t> out;
    out.swap(g_gift_sets);
    return out;
}
std::vector<std::uint32_t> TakeRepeatSets() {
    std::lock_guard<std::mutex> lk(g_gift_mu);
    std::vector<std::uint32_t> out;
    out.swap(g_repeat_sets);
    return out;
}
std::vector<std::uint32_t> TakeOwnSets() {
    std::lock_guard<std::mutex> lk(g_gift_mu);
    std::vector<std::uint32_t> out;
    out.swap(g_own_sets);
    return out;
}
std::mutex g_treasure_mu;
struct Treasure {
    std::array<float, 3> pos{};
    std::uint32_t flag = 0;  // 50000000 + item lot (the MSB event's lot; 0 = not found)
};
std::vector<Treasure> g_treasure;
std::atomic<int> g_tgate[8];  // TreasureGate results (test fixture)
std::atomic<int> g_rec[4];     // record inserts by kind, [0] removes (test fixture)
std::atomic<std::uint64_t> g_rec_man{0};  // the MapItemMan the last insert went to
std::vector<std::pair<std::int32_t, double>> g_kicks_seen;

/// A map treasure's pickup flag (5AABZnnn, a map's item lot): what a guest's pickup in the host's
/// world sets. Global item flags (5000-5999, 9042-9075 ...) are the guest game's own bookkeeping.
bool MapItemFlag(std::uint32_t id) {
    return id >= 50000000u && id < 60000000u && id / 100000u % 100u != 0 &&
           progress::CategoryOf(id) == Category::item_lot_picked;
}

State& S() {
    std::call_once(g_st.once, [] {
        const char* party = std::getenv("BB_PARTY");
        const char* on = std::getenv("BB_PARTY_WORLD_ACTIONS");
        g_st.enabled = party && party[0] && !(on && on[0] == '0');
        g_st.watch = ParseIds(std::getenv("BB_PARTY_WORLD_WATCH"));
        if (g_st.enabled) {
            progress::AddLocalFlagSetObserver(&OnLocalGiftSet);
        }
        Log("guest world actions %s (levers, doors, shortcuts, elevators, NPC talk -> the host's world)",
            g_st.enabled ? "on" : "off");
        // On by default since 0.5.9 (live 2026-10-12: a guest and the host each took their own copy
        // of the same clinic treasure, docs/party/world_actions.md 4); BB_PARTY_GUEST_PICKUP=0 off.
        const char* pk = std::getenv("BB_PARTY_GUEST_PICKUP");
        if (g_st.enabled && !(pk && pk[0] == '0')) {
            // D4, instanced world items (items.md 2.4): MSB treasure is spawned on a client too
            // (0x17DC300: role 6 / session slot gates), and its pickup sets the pickup flag and runs
            // its Lua event there (0x17D6780: the "client in another world" skip) - into the
            // host-world overlay; GuestTick copies it into the guest's own save, the host's world
            // keeps its item. Treasure visibility follows the guest's own save (progress mirror).
            const bool a = PatchBytes(0x17DC3D6, {0x0f, 0x84, 0xa5, 0x0c, 0x00, 0x00},
                                      {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00}, "treasure spawn on a client (role 6)");
            const bool b = PatchBytes(0x17DC3E3, {0x0f, 0x85, 0x98, 0x0c, 0x00, 0x00},
                                      {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00}, "treasure spawn on a client (slot)");
            const bool c = PatchBytes(0x17D6838, {0x0f, 0x84, 0x2b, 0x02, 0x00, 0x00},
                                      {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00}, "pickup flag on a client in another world");
            // The host's map item sync (net handler 0x17D2F30: "clear kind 3" for -1 / 3) dropped every
            // treasure record a guest had just spawned (live 2026-10-12: 53 records -> 0 at the join
            // sync, no 0x17D6780 removal): the guest keeps its own treasure, the host never sends any.
            const bool d = PatchBytes(0x17D34A0, {0xe8, 0x9b, 0x26, 0x00, 0x00}, {0x0f, 0x1f, 0x44, 0x00, 0x00},
                                      "keep own treasure at the host's item sync (all kinds)");
            const bool e = PatchBytes(0x17D40AB, {0xe8, 0x90, 0x1a, 0x00, 0x00}, {0x0f, 0x1f, 0x44, 0x00, 0x00},
                                      "keep own treasure at the host's item sync (treasure)");
            // A client's treasure record gets no handle of its own (0x17D4FB0 numbers records only on
            // the host: role 6 / slot gates), so all of them kept handle 0x3 and a pickup's lookup
            // (0x17D81A0, first match) found another record: number them like the host does.
            const bool g = PatchBytes(0x17D52D9, {0x74, 0x26}, {0x66, 0x90}, "own treasure handles on a client (role 6)");
            const bool h = PatchBytes(0x17D52E6, {0x75, 0x1c}, {0x66, 0x90}, "own treasure handles on a client (slot)");
            // ... and the pickup flag itself (+0x28 of the record, the lot's getItemFlag) is set past a
            // second "client in another world" gate in 0x17D6780 (0x17D6AF6).
            const bool k = PatchBytes(0x17D6AF6, {0x0f, 0x84, 0x5c, 0x05, 0x00, 0x00},
                                      {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00}, "pickup flag on a client (lot flag)");
            g_st.pickups = a && b && c && d && e && g && h && k;
            progress::SetInstancedItems(g_st.pickups);
            Log("guest treasure pickups in the host's world: %s (instanced per player)",
                g_st.pickups ? "on" : "NOT installed");
        }
        // Enemy drops per player (user decision 2026-10-11): every player's game rolls each enemy's
        // drop itself (0x17DD0B0 on enemy death; live: the same enemy rolled on host and guest);
        // a client keeps only isGuestDrop items (vials, bullets) unless 0x17DD764 je -> jmp.
        // On by default (live 2026-10-12, fresh characters: the guest kept "Iosefka's Blood Vial" from
        // its own roll of a clinic enemy the host killed); BB_PARTY_GUEST_DROPS=0 turns it off.
        const char* dr = std::getenv("BB_PARTY_GUEST_DROPS");
        if (g_st.enabled && !(dr && dr[0] == '0')) {
            const bool d = PatchBytes(0x17DD764, {0x0f, 0x84, 0x3a, 0x02, 0x00, 0x00},
                                      {0xe9, 0x3b, 0x02, 0x00, 0x00, 0x90}, "full enemy drops on a client");
            Log("guest enemy drops: %s", d ? "full, own roll per player" : "NOT installed");
        }
    });
    return g_st;
}

void WatchTick(const GameSnapshot& s) {
    State& st = S();
    static double last = 0;
    static std::vector<int> prev, frame;
    if (st.watch.empty() || !s.world_up || s.loading) {
        return;
    }
    // Every frame: a visible value that flips between the 0.5 s status lines (a flag that pulses).
    frame.resize(st.watch.size(), -1);
    static int pulses = 0;
    for (std::size_t i = 0; i < st.watch.size() && pulses < 800; ++i) {
        bool vis = false;
        const int v = progress::ReadFlag(st.watch[i], &vis) ? (vis ? 1 : 0) : 2;
        if (frame[i] >= 0 && v != frame[i]) {
            ++pulses;
            Log("watch %u: visible %d -> %d (frame)", st.watch[i], frame[i], v);
        }
        frame[i] = v;
    }
    if (Now() - last < 0.5) {
        return;
    }
    last = Now();
    prev.resize(st.watch.size(), -2);
    for (std::size_t i = 0; i < st.watch.size(); ++i) {
        bool vis = false, own = false, host = false;
        const bool hv = progress::ReadFlag(st.watch[i], &vis);
        const bool ho = progress::ReadFlagOwnSave(st.watch[i], &own);
        const bool hh = progress::HostWorldFlag(st.watch[i], &host);
        const int code = (hv ? (vis ? 1 : 0) : 2) | (ho ? (own ? 1 : 0) : 2) << 2 | (hh ? (host ? 1 : 0) : 2) << 4;
        if (code != prev[i]) {
            prev[i] = code;
            Log("watch %u: visible %s, own save %s, host image %s (load mode %d, role %d, %s)", st.watch[i],
                hv ? (vis ? "1" : "0") : "-", ho ? (own ? "1" : "0") : "-", hh ? (host ? "1" : "0") : "-",
                progress::FlagLoadMode(), s.session_role, MapName(s.map_id).c_str());
        }
    }
}

std::string IdList(const std::vector<std::uint32_t>& ids) {
    std::string out;
    for (std::uint32_t id : ids) {
        out += (out.empty() ? "" : " ") + std::to_string(id) + "(" +
               progress::CategoryName(progress::CategoryOf(id)) + ")";
    }
    return out;
}

/// BB_PARTY_GATE_CATCHUP=1 (opt-in for 0.5.2: not run live; 2410-style flags also gate bells).
bool GateCatchupOn() {
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_GATE_CATCHUP");
        return v && v[0] == '1';
    }();
    return on;
}

/// The area gate flags of the bosses the guest's own save has killed, into the own save (pool in
/// load mode 1). Once per steady world; logged when something was written.
void GateCatchup() {
    std::string list;
    for (const GateRule& r : GateRules()) {
        bool boss = false, also = true, gate = false;
        if (!progress::ReadFlagOwnSave(r.boss, &boss) || !boss || (r.also && (!progress::ReadFlagOwnSave(r.also, &also) || !also)) ||
            !progress::ReadFlagOwnSave(r.gate, &gate) || gate) {
            continue;
        }
        if (progress::WriteFlagOwnSave(r.gate, true)) {
            list += " " + std::to_string(r.gate) + "(boss " + std::to_string(r.boss) + ")";
        }
    }
    if (!list.empty()) {
        Log("guest catch-up: area gate flags of its own bosses into the own save:%s", list.c_str());
        RequestGameSave("guest gate catch-up");
    }
}

bool MyPos(float out[3]);

void GuestTick(const GameSnapshot& s, party::PartyLink* link) {
    State& st = S();
    const double now = Now();
    const bool ok = s.session_role == 6 && s.world_up && !s.loading && progress::FlagLoadMode() == 1;
    if (!ok || s.map_id != st.map) {
        st.steady = false;
        st.baselined = false;
        st.map = s.map_id;
        st.fog_in.clear();
        st.fog_apart.clear();
        st.fog_out.clear();
        st.fog_stale.clear();
        st.story_sent.clear();
        // A gift trigger set in the own world (its own 9040 awards it) or during a load: not the host's.
        if (const std::vector<std::uint32_t> g = TakeGiftSets(); !g.empty()) {
            Log("guest: NPC gift trigger(s) %s set outside the host's steady world: kept local", IdList(g).c_str());
        }
        if (s.session_role != 6) {
            TakeRepeatSets();  // its own world: its own 9100 awards them
            TakeOwnSets();     // its own world: its own 12700906 awards them
        }
        return;
    }
    if (!st.steady) {
        st.steady = true;
        st.steady_since = now;
        st.known.clear();
        st.lift_baseline.clear();
        st.lift_pending.clear();
        st.lift_sent.clear();
    }
    for (const BossGate& g : BossGates()) {
        // 0.7.2 known issue 2: from the stay's first steady frame on, which guest-entered flags of
        // this block read 0 - only a 0 -> 1 after that is this guest's walk through the fog.
        bool in = false;
        if (BossGateInBlock(g, s.map_id) && progress::ReadFlag(g.guest_flag, &in) && !in) {
            st.fog_out.insert(g.guest_flag);
            st.fog_stale.erase(g.guest_flag);
        }
    }
    if (now - st.last_scan < 0.25 || now - st.steady_since < 3.0) {
        return;
    }
    st.last_scan = now;
    std::vector<std::pair<std::uint32_t, bool>> changes;
    if (!progress::GuestLocalWorldChanges(&changes, 4096)) {
        st.steady = false;  // overlay rebuilt / no image: a new baseline once it settles
        st.baselined = false;
        return;
    }
    if (!st.baselined) {
        // What the guest's own game wrote while loading the map (client branches of events end
        // early and set their completion flags) stays local.
        for (const auto& [id, v] : changes) {
            // Item pickups never: the overlay's item bits are the own pool's (instanced items), so
            // a difference is always a pickup this guest made (also one during a map change).
            if (!(st.pickups && MapItemFlag(id))) {
                st.known.insert(std::uint64_t(id) * 2 + (v ? 1 : 0));
                if (LiftByPosFlag(id)) {
                    if (now - g_lift_ride_seen.load() < 25.0 && v) {
                        // liftre077: the ride's own end, written as the guest crossed into the next block at the top
                        // (Cathedral Ward: m24_01 -> m24_00): the baseline of that load swallowed it and the host's
                        // world kept the car at the other end. A ride seen lately is a ride to report.
                        st.lift_pending[id] = v;
                        Log("guest: lift flag %u = %d changed during the load after a ride seen %.0f s ago: reported as a ride end", id,
                            v ? 1 : 0, now - g_lift_ride_seen.load());
                    } else {
                        st.lift_baseline.insert(std::uint64_t(id) * 2 + (v ? 1 : 0));
                    }
                }
            }
        }
        st.baselined = true;
        // Which ones (the first 48, id=value): a world difference a guest sees alone (an NPC only
        // its game shows) starts from this list.
        std::string ids;
        for (std::size_t k = 0; k < changes.size() && k < 48; ++k) {
            ids += (k ? " " : "") + std::to_string(changes[k].first) + "=" + (changes[k].second ? "1" : "0");
        }
        Log("guest: %zu local flag difference(s) in the host's world held back as the baseline (%s): %s%s",
            changes.size(), MapName(s.map_id).c_str(), ids.c_str(), changes.size() > 48 ? " ..." : "");
        if (GateCatchupOn()) {
            GateCatchup();
        }
        return;
    }
    Request r;
    r.map = s.map_id;
    std::vector<std::uint32_t> local, picked;
    // P1: the guest's talk handed over an NPC gift. Its own 9040 slot ended at load (client), and the
    // overlay bit already read 1 then (no change to see): the trigger goes to the host, whose 9040
    // awards the lot; C3 gives it to every member (ledger / lot flag), the guest included.
    // P2: a repeatable gift (Blood of Arianna ...) the guest's talk asked for in the host's world:
    // the guest's own, given here (the host's 9100 never sees the trigger), the trigger cleared again.
    for (std::uint32_t id : TakeRepeatSets()) {
        const std::int32_t lot = RepeatGiftLot(id);
        bool host_set = false;
        if (progress::HostWorldFlag(id, &host_set) && host_set) {
            // The host's own talk set it (its 9100 awards the host): not this guest's gift.
            Log("guest: repeatable NPC gift trigger %u is set in the host's world too: the host's own gift; "
                "nothing given here", id);
            continue;
        }
        const bool queued = OfferRepeatGift(lot);
        const bool cleared = WriteEventFlag(id, false);
        Log("guest: repeatable NPC gift trigger %u set by its talk in the host's world: lot %d %s; trigger %s "
            "(common 9100 does that on the host only)", id, lot,
            queued ? "given to this guest (own grant)" : "NOT queued (items off)", cleared ? "cleared" : "NOT cleared");
    }
    for (std::uint32_t id : TakeOwnSets()) {
        // 0.7.3: Valtr's rune / staff. Joining the League is this player's own (covenant outcomes are
        // per player), and so is the item: its own grant with the lot's personal flag (6676 / 6678 stay
        // in the own pool while summoned), never the host's. The trigger stays in the overlay.
        const std::int32_t lot = OwnTalkGiftLot(id);
        const std::int32_t flag = ItemLotFlag(lot);
        bool have = false;
        if (flag > 0 && progress::ReadFlagOwnSave(static_cast<std::uint32_t>(flag), &have) && have) {
            Log("guest: own talk gift trigger %u set by its talk in the host's world: lot %d already given (own flag "
                "%d set); nothing given", id, lot, flag);
            continue;
        }
        const bool queued = OfferOwnGift(lot);
        Log("guest: own talk gift trigger %u set by its talk in the host's world: lot %d (flag %d) %s (m27 event "
            "12700906 awards it in the own world only)", id, lot, flag,
            queued ? "given to this guest (own grant)" : "NOT queued (items off or already queued)");
    }
    if (const std::uint32_t kgf = g_keygate_own.exchange(0)) {
        const bool ok = progress::WriteFlagOwnSave(kgf, true);
        Log("guest: opened key-item gate (flag %u) with its own key: the host's far-side ObjAct kick goes up; own "
            "save %s", kgf, ok ? "set" : "NOT written");
    }
    for (std::uint32_t id : TakeGiftSets()) {
        bool given = false;
        if (progress::HostWorldFlag(id, &given) && given) {
            Log("guest: NPC gift trigger %u set by its talk: already given in the host's world; nothing sent", id);
            continue;
        }
        if (WorldActionAllowed(id, false, s.map_id) && r.set.size() < kMaxSet) {
            r.set.push_back(id);
            Log("guest: NPC gift trigger %u set by its talk: to the host (its common event 9040 awards the gift, "
                "C3 hands it to the party)", id);
        }
    }
    for (const auto& [id, v] : changes) {
        if (v && st.pickups && MapItemFlag(id) && now - g_pickup_at.load() < 15.0) {
            // D4: the guest picked up the host world's treasure - instanced: its own save only.
            if (progress::WriteFlagOwnSave(id, true)) {
                ++st.stats.own_save;
                picked.push_back(id);
            }
            // D4 (pard4): a progression key / workshop tool goes to the whole party (gift_up).
            ReportGuestKeyTreasure(static_cast<std::int32_t>(id));
            continue;
        }
        const std::uint64_t k = std::uint64_t(id) * 2 + (v ? 1 : 0);
        if (LiftByPosFlag(id) && id / 100000u % 100u == (s.map_id >> 24)) {
            // D3 (lifts071): this guest's own copy of the lift's events moved the car, in any block of
            // the area (live l8: Cathedral Ward's lift while the guest's map still read Central
            // Yharnam). A host with the lift's block loaded ran its copy too (the game's own sync);
            // one that has it streamed out did not. Sent once the ride is over (GuestLiftReport).
            // liftfix: every ride, not once per (flag, value) for the whole stay (`known`): a second
            // ride to an end reported before never went up; only the baseline's differences (an init
            // event's own write while loading) stay local.
            if (!st.lift_baseline.count(k)) {
                st.lift_pending[id] = v;
            }
            continue;
        }
        if (st.known.count(k)) {
            continue;
        }
        st.known.insert(k);
        // A map flag only of the block the guest stands in: a streamed-in neighbour block runs its
        // constructors (NPC states, live run 9: m24_00's 12400501.. while in m24_01) - not the
        // guest's doing; once the guest's map is that block, the new baseline holds them back.
        if (id >= 10000000u && id / 100000u % 100u != 0 &&
            (id / 100000u % 100u != (s.map_id >> 24) || id / 10000u % 10u != ((s.map_id >> 16) & 0xffu))) {
            local.push_back(id);
            continue;
        }
        if (!WorldActionAllowed(id, !v, s.map_id) && !BossKillActionAllowed(id, !v, s.map_id, b2::MemberApart(0))) {
            local.push_back(id);
            continue;
        }
        std::vector<std::uint32_t>& to = v ? r.set : r.clr;
        if (to.size() < (v ? kMaxSet : kMaxClear)) {
            to.push_back(id);
        }
    }
    if (!picked.empty()) {
        RequestGameSave("guest pickup");
        std::string hv;
        for (std::uint32_t id : picked) {
            bool h = false;
            hv += (hv.empty() ? "" : " ") + std::to_string(id) + "=" +
                  (progress::HostWorldFlag(id, &h) ? (h ? "1" : "0") : "-");
        }
        Log("guest: picked up in the host's world (instanced, own save only): %s; the host's world: %s",
            IdList(picked).c_str(), hv.c_str());
    }
    if (!local.empty()) {
        Log("guest: local flag change(s) kept local (not a world action): %s", IdList(local).substr(0, 500).c_str());
    }
    // liftfix: an end sent before is forgotten once the host's world has the guest's end again (its own
    // ride, or ours applied), so the next ride to it goes up too.
    for (auto it = st.lift_sent.begin(); it != st.lift_sent.end();) {
        bool pos = false, host = false;
        const bool same = progress::ReadFlag(it->first, &pos) && progress::HostWorldFlag(it->first, &host) && pos == host;
        it = same ? st.lift_sent.erase(it) : std::next(it);
    }
    for (auto it = st.lift_pending.begin(); it != st.lift_pending.end();) {
        const Lift* l = LiftByPosFlag(it->first);
        bool pos = false, busy = false, host = false;
        if (!l || !progress::ReadFlag(l->pos, &pos) || (l->busy && !progress::ReadFlag(l->busy, &busy))) {
            it = st.lift_pending.erase(it);
            continue;
        }
        if (!l->busy) {
            busy = LiftBusyHere(*l, kLiftInCarRadius);  // liftfall: no busy flag: the car or its rider here
        }
        const bool known_host = progress::HostWorldFlag(l->pos, &host);
        const auto sent = st.lift_sent.find(l->pos);
        const liftrules::GuestReport what =
            liftrules::GuestLiftReport(pos, known_host ? host : !pos, busy, sent == st.lift_sent.end() ? -1 : sent->second);
        if (what == liftrules::GuestReport::wait) {
            ++it;  // the ride runs: its end decides
            continue;
        }
        if (what == liftrules::GuestReport::same) {
            Log("guest: lift %d ride over (flag %u = %d): the host's world has that end already", l->car, l->pos,
                pos ? 1 : 0);
        } else if (what == liftrules::GuestReport::send && (pos ? r.lift_set : r.lift_clr).size() < kMaxLift) {
            (pos ? r.lift_set : r.lift_clr).push_back(l->pos);
            st.lift_sent[l->pos] = pos ? 1 : 0;
            Log("guest: lift %d ride over (flag %u = %d, the host's world %s): to the host", l->car, l->pos, pos ? 1 : 0,
                known_host ? (host ? "1" : "0") : "unknown");
        }
        it = st.lift_pending.erase(it);
    }
    {
        std::lock_guard<std::mutex> lk(g_kick_mu);
        for (std::int32_t k : g_kicks_out) {
            if (r.kick.size() < kMaxKicks && std::find(r.kick.begin(), r.kick.end(), k) == r.kick.end()) {
                r.kick.push_back(k);
            }
        }
        g_kicks_out.clear();
    }
    for (const BossGate& g : BossGates()) {
        // B2: a boss fog this guest went through (its guest-entry event set the flag in the
        // overlay; the host's game does not get it): the host opens the fight.
        if (g.guest_flag / 100000u % 100u != (s.map_id >> 24) || g.guest_flag / 10000u % 10u != ((s.map_id >> 16) & 0xffu)) {
            continue;
        }
        bool in = false;
        progress::ReadFlag(g.guest_flag, &in);
        const bool was = st.fog_in.count(g.guest_flag) != 0;
        if (bossfog::StaleFogFlag(st.fog_out.count(g.guest_flag) != 0, in, was || st.fog_stale.count(g.guest_flag))) {
            // Set since before this stay (it came with the load): not this guest's walk through the
            // fog. Not sent (the host opened the fight from it at the lamp), not "in the boss room".
            st.fog_stale.insert(g.guest_flag);
            bool host_in = false;
            progress::ReadFlag(g.host_flag, &host_in);
            Log("guest: boss fog (event %u): guest-entered flag %u already 1 when this stay began (host-entered "
                "%u = %d): not a walk through the fog, not sent", g.guest_event, g.guest_flag, g.host_flag,
                host_in ? 1 : 0);
        }
        if (st.fog_stale.count(g.guest_flag)) {
            continue;  // until it reads 0 again (then a real walk-through counts)
        }
        const std::uint32_t host_map = b2::MemberMap(0);
        const bool apart = !GuestFogOpenWhileHostAt(g, host_map);
        if (!apart || !in) {
            st.fog_apart.erase(g.guest_flag);
        }
        if (in && apart && !st.fog_apart.count(g.guest_flag)) {
            // D3 (pard4): in the boss room while the host is in another area - through the fog
            // after the host left (its prompt had opened while the host was here), or the host
            // travelled alone during the fight: nothing can start or end this fight here (the
            // boss's events are the host's). Put back outside the fog, once per stay.
            st.fog_apart.insert(g.guest_flag);
            st.fog_in.insert(g.guest_flag);
            bool dead = false;
            progress::ReadFlag(g.boss_flag, &dead);
            if (!dead && g.fog_entity) {
                std::lock_guard<std::mutex> lk(g_putback_mu);
                g_putback.push_back({g.fog_entity, g.guest_event, g.guest_flag, Now() + kFogApartPutBackAfter});
            }
            Log("guest: in boss room (event %u, flag %u) while the host is in %s (%s): no fight without the host's "
                "area%s", g.guest_event, g.guest_flag, MapName(host_map).c_str(),
                was ? "the host left during the fight" : "went through the fog", dead ? "; the boss is dead here" :
                "; putting back outside");
        } else if (bossfog::FogWalkedThrough(st.fog_out.count(g.guest_flag) != 0, in, was) && r.fog.size() < kMaxFog) {
            st.fog_in.insert(g.guest_flag);
            r.fog.push_back(g.guest_flag);
            Log("guest: went through boss fog (event %u, flag %u): the host opens the fight", g.guest_event, g.guest_flag);
        } else if (!in && was) {
            st.fog_in.erase(g.guest_flag);
        }
    }
    for (const StoryTrigger& t : StoryTriggers()) {
        // D10: the guest stands where a host-only story event would start (Blood Moon): the host's
        // chr runs it. Once per map stay; again after a load if it still did not run.
        if (!StoryTriggerInBlock(t, s.map_id) || st.story_sent.count(t.event) || r.story.size() >= kMaxStory) {
            continue;
        }
        bool needs = false, done = false, ran = false;
        float me[3] = {};
        const bool have_me = MyPos(me);
        progress::ReadFlag(t.needs_flag, &needs);
        progress::ReadFlag(t.done_flag, &done);
        progress::ReadFlag(t.event, &ran);
        if (StoryTriggerDue(t, s.map_id, needs, done, ran, have_me ? me : nullptr)) {
            st.story_sent.insert(t.event);
            r.story.push_back(t.event);
            Log("guest: at story event %u's spot (%.1f, %.1f, %.1f), %.0f m radius: the host's chr runs it (its scene "
                "plays for the party)", t.event, me[0], me[1], me[2], t.radius);
        }
    }
    if (r.set.empty() && r.clr.empty() && r.kick.empty() && r.fog.empty() && r.story.empty() && r.lift_set.empty() &&
        r.lift_clr.empty()) {
        return;
    }
    // The guest's own save keeps what it did (its own world after the session).
    int own = 0;
    for (std::uint32_t id : r.set) {
        own += progress::WriteFlagOwnSave(id, true) ? 1 : 0;
    }
    for (std::uint32_t id : r.clr) {  // a quest step's old state, an elevator's other position
        own += progress::WriteFlagOwnSave(id, false) ? 1 : 0;
    }
    if (own) {
        st.stats.own_save += std::uint64_t(own);
        RequestGameSave("guest world action");
    }
    if (link) {
        link->send_event(0, kEventName, RequestToJson(r));
    }
    ++st.stats.sent;
    st.stats.sent_flags += r.set.size() + r.clr.size();
    std::string kicks;
    for (std::int32_t k : r.kick) {
        kicks += (kicks.empty() ? "" : " ") + std::to_string(k);
    }
    Log("guest: world action sent to the host (%s): set %s; clear %s; ObjAct kick %s; lift %s / %s; %d written to "
        "the own save", MapName(s.map_id).c_str(), r.set.empty() ? "-" : IdList(r.set).c_str(),
        r.clr.empty() ? "-" : IdList(r.clr).c_str(), kicks.empty() ? "-" : kicks.c_str(),
        r.lift_set.empty() ? "-" : IdList(r.lift_set).c_str(), r.lift_clr.empty() ? "-" : IdList(r.lift_clr).c_str(), own);
}

/// Host: the guest kicked ObjAct `kick_event` in its copy of the host's world. Unless the host's
/// game applied that kick itself (a remote kick in the last 15 s), replay it as a remote kick on
/// the host's own entry (0x13AF830(blockman, idx, -1, its current state), what the ObjAct
/// manager does for a network kick): the host's EMEVD sees "ObjAct executed" and the object
/// animates in the host's world.
void ReplayKick(int slot, std::int32_t kick_event, double now) {
    {
        std::lock_guard<std::mutex> lk(g_kick_mu);
        for (const auto& [id, at] : g_kicks_seen) {
            if (id == kick_event && now - at < 15.0) {
                Log("host: slot %d's ObjAct kick %d already reached the host's game (vanilla)", slot, kick_event);
                return;
            }
        }
    }
    std::uint64_t mgr = 0, blocks = 0;
    std::int32_t nblocks = 0;
    if (!SafeGet(Guest(0x553b0f0), &mgr) || !mgr || !SafeGet(mgr + 0x20, &nblocks) || !SafeGet(mgr + 0x28, &blocks) ||
        !blocks || nblocks <= 0 || nblocks > 64) {
        Log("host: slot %d's ObjAct kick %d: no ObjAct manager", slot, kick_event);
        return;
    }
    for (std::int32_t b = 0; b < nblocks; ++b) {
        const std::uint64_t bm = blocks + std::uint64_t(b) * 0xd8;
        std::uint64_t arr = 0;
        std::int32_t n = 0;
        if (!SafeGet(bm + 0x90, &arr) || !arr || !SafeGet(bm + 0x98, &n) || n <= 0 || n > 4096) {
            continue;
        }
        for (std::int32_t i = 0; i < n; ++i) {
            const std::uint64_t e = arr + std::uint64_t(i) * 0x140;
            std::int32_t ev = -1;
            std::uint64_t obj = 0, vt = 0, fn = 0;
            if (!SafeGet(e + 0xc, &ev) || ev != kick_event) {
                continue;
            }
            std::int32_t state = 0;
            if (SafeGet(e + 0x90, &obj) && obj && SafeGet(obj, &vt) && vt && SafeGet(vt + 0x38, &fn) && fn) {
                state = reinterpret_cast<std::int32_t(BB_COOP_SYSV*)(std::uint64_t)>(fn)(obj);
            }
            using RemoteKick = std::uint8_t(BB_COOP_SYSV*)(std::uint64_t, std::int32_t, std::int32_t, std::int32_t);
            const std::uint8_t ok = reinterpret_cast<RemoteKick>(Guest(0x13af830))(bm, i, -1, state);
            Log("host: slot %d's ObjAct kick %d replayed on the host's object (block %d entry %d, state %d): %s", slot,
                kick_event, b, i, state, ok ? "kicked" : "refused by the game");
            return;
        }
    }
    Log("host: slot %d's ObjAct kick %d: no such ObjAct in the host's loaded world", slot, kick_event);
}

namespace {
// P5 / D10: a guest's start of a first fight on the host: the host's chr visits its first-fight
// event's region for a moment (FirstFightSpotFor) and is put back once the event went on.
struct FirstFightWait {
    std::uint32_t guest_event = 0;
    int slot = -1;
    double since = 0;
    int phase = 0;  // 0 waits for the host in the block, 1 visiting, 3 in the intro scene, 2 back (waits for the event's end)
    int tries = 0;
    double visit_at = 0;
    double held_logged = 0;
    double scene_over_at = 0;
    bool back_after_scene = false;
    alignas(16) float back_pos[4] = {};
    alignas(16) float back_rot[4] = {};
};
std::mutex g_ff_mu;
std::vector<FirstFightWait> g_ff_wait;

bool LocalChrPose(float pos[4], float rot[4]) {
    std::uint64_t man = 0, player = 0, a = 0, b = 0, c = 0, d = 0;
    return SafeGet(Guest(0x553e878), &man) && man && SafeGet(man + 0x60, &player) && player &&
           SafeGet(player + 0x58, &a) && a && SafeGet(a + 8, &b) && b && SafeGet(b + 0x3b0, &c) && c &&
           SafeGet(c + 0x68, &d) && d && SafeRead(d + 0x1e0, pos, 16) && SafeRead(d + 0x1d0, rot, 16);
}

// The host's screen keeps its last picture while its chr is away (live pard2: without it 2-4
// frames of the arena spot's view showed before the put-back). BB_PARTY_FF_HOLD=0: off.
void FirstFightHold(double seconds) {
    static const bool on = !(std::getenv("BB_PARTY_FF_HOLD") && std::getenv("BB_PARTY_FF_HOLD")[0] == '0');
    if (on) {
        BbPresentHold::For(seconds);
    }
}

bool WarpLocalChr(const float pos[4], const float rot[4]) {
    std::uint64_t man = 0, player = 0, vt = 0, fn = 0;
    if (!SafeGet(Guest(0x553e878), &man) || !man || !SafeGet(man + 0x60, &player) || !player ||
        !SafeGet(player, &vt) || !SafeGet(vt + 0x5f8, &fn) || !fn) {
        return false;
    }
    alignas(16) float p[4] = {pos[0], pos[1], pos[2], 1.0f};
    alignas(16) float r[4] = {rot[0], rot[1], rot[2], rot[3]};
    using WarpFn = void(BB_COOP_SYSV*)(std::uint64_t, const float*, const float*, std::uint64_t);
    reinterpret_cast<WarpFn>(fn)(player, p, r, 1);
    return true;
}

void FirstFightRequest(const BossGate& g, int slot, std::uint32_t host_map) {
    std::lock_guard<std::mutex> lk(g_ff_mu);
    for (FirstFightWait& w : g_ff_wait) {
        if (w.guest_event == g.guest_event) {
            w.slot = slot;
            return;
        }
    }
    FirstFightWait w;
    w.guest_event = g.guest_event;
    w.slot = slot;
    w.since = Now();
    g_ff_wait.push_back(w);
    if (!BossGateInBlock(g, host_map)) {
        Log("host: boss fog: slot %d went in before the host's first fight (guest event %u), the host stands in "
            "%s: the fight starts once the host is in the boss's block (up to %.0f s)",
            slot, g.guest_event, MapName(host_map).c_str(), kFirstFightWaitSeconds);
        party::status::SetLastEvent("A party member waits in a boss arena: go there to start the fight");
    } else {
        Log("host: boss fog: slot %d went in before the host's first fight here (guest event %u): the host's "
            "first-fight event %u starts now (its scene plays for the whole party)", slot, g.guest_event, g.intro_flag);
    }
}

// Host tick: runs a waiting start once the host stands steady in the boss's block, puts the host
// back after the visit, and drops starts that ran (intro flag on), whose boss died, or that waited
// too long.
void FirstFightTick(const GameSnapshot& s) {
    std::lock_guard<std::mutex> lk(g_ff_mu);
    if (g_ff_wait.empty()) {
        return;
    }
    const double now = Now();
    for (auto it = g_ff_wait.begin(); it != g_ff_wait.end();) {
        FirstFightWait& w = *it;
        const BossGate* g = BossGateByEvent(static_cast<std::int32_t>(w.guest_event));
        const FirstFightSpot* spot = g ? FirstFightSpotFor(g->intro_flag) : nullptr;
        bool intro = false, dead = false, host_in = false, f9180 = false;
        if (g) {
            progress::ReadFlag(g->intro_flag, &intro);
            progress::ReadFlag(g->boss_flag, &dead);
            progress::ReadFlag(g->host_flag, &host_in);
        }
        progress::ReadFlag(9180, &f9180);
        const bool cutscene = CutscenePlaying();
        const bool chained = spot && spot->chained;
        // The host's death is the party's respawn load at its lamp (the guest leaves the boss
        // room with it, the room resets): a start that has not run yet is void.
        const int hp_now = b2::LocalHp();
        const bool host_died = w.phase == 0 && hp_now == 0 && s.world_up;
        const bool scene = spot && spot->scene;
        if (w.phase == 1 && scene && cutscene) {
            // The intro scene runs with the host at the arena's entrance, as when it walks in
            // (live pard2: a host put back before the scene was killed the moment the scene
            // ended, HP 719 -> 0). It stays there for the scene; the scene is on screen.
            FirstFightHold(0);
            w.phase = 3;
            Log("host: first fight (guest event %u): its scene started %.2f s into the visit: the host stays at the "
                "arena's entrance until it is over", w.guest_event, now - w.visit_at);
        } else if (w.phase == 3 && cutscene) {
            w.scene_over_at = 0;
        } else if (w.phase == 3 && !cutscene && w.scene_over_at == 0) {
            w.scene_over_at = now;  // the put-back waits a moment after the scene (live pard2 p5e)
        } else if (w.phase == 3 && now - w.scene_over_at >= kFirstFightAfterScene) {
            FirstFightHold(kFirstFightHoldAfter);
            const bool ok = WarpLocalChr(w.back_pos, w.back_rot);
            Log("host: first fight (guest event %u): scene over: the host is put back at (%.2f, %.2f, %.2f)%s",
                w.guest_event, w.back_pos[0], w.back_pos[1], w.back_pos[2], ok ? "" : " (warp NOT possible)");
            w.phase = 2;
            w.visit_at = now;
            w.back_after_scene = true;  // the chr warp lands next frame: no second put-back below
        }
        if (w.phase == 1 && FirstFightVisitDone(host_in, f9180, cutscene, now - w.visit_at, chained, scene)) {
            const bool ok = WarpLocalChr(w.back_pos, w.back_rot);
            FirstFightHold(kFirstFightHoldAfter);  // until the camera is back behind the host
            Log("host: first fight (guest event %u): visit over after %.2f s (boss started %d, 9180 %d, cutscene %d): "
                "the host is put back at (%.2f, %.2f, %.2f)%s", w.guest_event, now - w.visit_at, host_in ? 1 : 0,
                f9180 ? 1 : 0, cutscene ? 1 : 0, w.back_pos[0], w.back_pos[1], w.back_pos[2],
                ok ? "" : " (warp NOT possible)");
            w.phase = (host_in || (f9180 && !chained && !scene) || cutscene) ? 2 : 0;  // a visit that started nothing: try again
            w.visit_at = now;
        }
        if (w.phase == 3 && !dead && now - w.since <= kFirstFightWaitSeconds) {
            // In the scene: its event may end (intro flag on) before the scene does (live pard2
            // p5e: the host put back 6 s before the end of Amelia's scene was killed as it
            // ended, HP 719 -> 0). The host stays at the spot until the scene is over.
            ++it;
            continue;
        }
        if (!g || !spot || intro || dead || host_died || now - w.since > kFirstFightWaitSeconds ||
            w.tries > kFirstFightVisitTries) {
            if (g && intro) {
                Log("host: first fight of guest event %u started (first-fight event %u done, %.1f s after the "
                    "guest's start)", w.guest_event, g->intro_flag, now - w.since);
                // The intro scene leaves the player at the arena's entrance (live pard2: the host,
                // 217 m away at its lamp, stood in Amelia's arena after her scene and was killed
                // there). The host goes back to where it stood; it walks in through its own fog.
                alignas(16) float here[4] = {}, rot[4] = {};
                if (w.tries > 0 && spot && !w.back_after_scene && LocalChrPose(here, rot) &&
                    FirstFightPutBackAfterScene(here, w.back_pos, spot->pos)) {
                    FirstFightHold(kFirstFightHoldAfter);
                    const bool ok = WarpLocalChr(w.back_pos, w.back_rot);
                    Log("host: first fight (guest event %u): the scene left the host at (%.2f, %.2f, %.2f) by the "
                        "arena: put back at (%.2f, %.2f, %.2f)%s", w.guest_event, here[0], here[1], here[2],
                        w.back_pos[0], w.back_pos[1], w.back_pos[2], ok ? "" : " (warp NOT possible)");
                }
            } else if (g) {
                Log("host: first-fight start for guest event %u dropped (%s)", w.guest_event,
                    dead ? "boss dead"
                    : host_died ? "the host died: the party respawns at its lamp, the boss room resets"
                    : !spot ? "no spot"
                    : w.tries > kFirstFightVisitTries ? "the event did not start"
                                                      : "the host did not come within the wait");
            }
            if (w.phase == 1 || w.phase == 3) {
                WarpLocalChr(w.back_pos, w.back_rot);
                FirstFightHold(kFirstFightHoldAfter);
            }
            it = g_ff_wait.erase(it);
            continue;
        }
        // A dead host (its respawn load comes), another boss fight of this block or a scene: later.
        const int hp = hp_now;
        const bool bar = BossBarShown();
        const bool ready = s.world_up && !s.loading && BossGateInBlock(*g, s.map_id) &&
                           FirstFightHostReady(hp, bar, cutscene) && progress::FlagLoadMode() == 0 &&
                           now - w.visit_at > 1.0;
        if (w.phase == 0 && !ready && s.world_up && !s.loading && BossGateInBlock(*g, s.map_id) &&
            now - w.held_logged > 10.0 && !FirstFightHostReady(hp, bar, cutscene)) {
            w.held_logged = now;
            Log("host: first fight (guest event %u): the visit waits (host HP %d, boss bar %d, cutscene %d)",
                w.guest_event, hp, bar ? 1 : 0, cutscene ? 1 : 0);
        }
        if (w.phase == 0 && ready && w.tries < kFirstFightVisitTries && LocalChrPose(w.back_pos, w.back_rot)) {
            alignas(16) const float to[4] = {spot->pos[0], spot->pos[1], spot->pos[2], 1.0f};
            FirstFightHold(kFirstFightVisitSeconds + kFirstFightHoldAfter);  // ended sooner at the put-back
            if (WarpLocalChr(to, w.back_rot)) {
                ++w.tries;
                w.phase = 1;
                w.visit_at = now;
                Log("host: first fight (guest event %u, try %d): the host's chr visits its first-fight event %u's spot "
                    "(%.2f, %.2f, %.2f) from (%.2f, %.2f, %.2f)", w.guest_event, w.tries, g->intro_flag, to[0], to[1],
                    to[2], w.back_pos[0], w.back_pos[1], w.back_pos[2]);
                party::status::SetLastEvent("Boss fight started by a party member");
            }
        } else if (w.phase == 0 && w.tries >= kFirstFightVisitTries && now - w.visit_at > 5.0) {
            w.tries = kFirstFightVisitTries + 1;  // dropped on the next tick
        }
        ++it;
    }
}

// D10 (pard3): a guest at a host-only story event's spot (StoryTrigger): the host's chr visits the
// spot (the same hold / put-back as a guest-started first fight), the event plays its scene for
// the whole party (live mirror); 1 s after the scene the host goes back, unless the event's own
// story warp is loading it (then the travel question asks the host, P8).
struct StoryVisit {
    std::uint32_t event = 0;
    int slot = 0;
    int phase = 0;  // 0 waits, 1 visiting, 3 in the scene
    int tries = 0;
    double since = 0, visit_at = -1e9, scene_over_at = 0, held_logged = -1e9;
    alignas(16) float back_pos[4] = {}, back_rot[4] = {};
};
std::mutex g_story_mu;
std::vector<StoryVisit> g_story_wait;

void StoryRequest(std::uint32_t event, int slot) {
    std::lock_guard<std::mutex> lk(g_story_mu);
    for (StoryVisit& w : g_story_wait) {
        if (w.event == event) {
            w.slot = slot;
            return;
        }
    }
    StoryVisit w;
    w.event = event;
    w.slot = slot;
    w.since = Now();
    g_story_wait.push_back(w);
    Log("host: slot %d stands at story event %u's spot: the host's chr runs it (its scene plays for the party)", slot,
        event);
}

void StoryTick(const GameSnapshot& s) {
    std::lock_guard<std::mutex> lk(g_story_mu);
    const double now = Now();
    for (auto it = g_story_wait.begin(); it != g_story_wait.end();) {
        StoryVisit& w = *it;
        const StoryTrigger* t = StoryTriggerByEvent(w.event);
        bool needs = false, done = false, ran = false;
        if (t) {
            progress::ReadFlag(t->needs_flag, &needs);
            progress::ReadFlag(t->done_flag, &done);
            progress::ReadFlag(t->event, &ran);
        }
        const bool cutscene = CutscenePlaying();
        if (w.phase == 1 && cutscene) {
            FirstFightHold(0);
            w.phase = 3;
            Log("host: story event %u: its scene started %.2f s into the visit: the host stays for it", w.event,
                now - w.visit_at);
        } else if (w.phase == 3 && cutscene) {
            w.scene_over_at = 0;
        } else if (w.phase == 3 && !cutscene && w.scene_over_at == 0) {
            w.scene_over_at = now;
        } else if (w.phase == 3 && now - w.scene_over_at >= kFirstFightAfterScene) {
            // The event's story warp may be loading the host already (a load moves it anyway).
            const bool moving = !s.world_up || s.loading;
            const bool ok = !moving && WarpLocalChr(w.back_pos, w.back_rot);
            if (!moving) {
                FirstFightHold(kFirstFightHoldAfter);
            }
            Log("host: story event %u: scene over (ran %d, done flag %u = %d): %s", w.event, ran ? 1 : 0,
                t ? t->done_flag : 0u, done ? 1 : 0,
                moving ? "a load runs (its story warp): not put back" : ok ? "the host is put back" : "put back NOT possible");
            it = g_story_wait.erase(it);
            continue;
        }
        if (w.phase == 1 && now - w.visit_at >= kFirstFightVisitSeconds + 1.0) {
            const bool ok = WarpLocalChr(w.back_pos, w.back_rot);
            FirstFightHold(kFirstFightHoldAfter);
            Log("host: story event %u: no scene %.1f s into visit %d: put back%s", w.event, now - w.visit_at, w.tries,
                ok ? "" : " (warp NOT possible)");
            w.phase = 0;
            w.visit_at = now;
        }
        if (w.phase == 3) {
            ++it;
            continue;
        }
        const int hp = b2::LocalHp();
        if (!t || !needs || done || ran || w.tries >= kFirstFightVisitTries || now - w.since > kFirstFightWaitSeconds) {
            if (w.phase == 1) {
                WarpLocalChr(w.back_pos, w.back_rot);
                FirstFightHold(kFirstFightHoldAfter);
            }
            Log("host: story event %u (slot %d) %s", w.event, w.slot,
                !t ? "unknown" : (done || ran) ? "has run" : !needs ? "is not due in the host's world (its gate flag is off)"
                : w.tries >= kFirstFightVisitTries ? "did not start after the visits: dropped" : "waited too long: dropped");
            it = g_story_wait.erase(it);
            continue;
        }
        const bool bar = BossBarShown();
        const bool ready = s.world_up && !s.loading && StoryTriggerInBlock(*t, s.map_id) &&
                           FirstFightHostReady(hp, bar, cutscene) && progress::FlagLoadMode() == 0 &&
                           now - w.visit_at > 1.0;
        if (w.phase == 0 && !ready && now - w.held_logged > 10.0) {
            w.held_logged = now;
            Log("host: story event %u: the visit waits (host in %s, HP %d, boss bar %d, cutscene %d)", w.event,
                MapName(s.map_id).c_str(), hp, bar ? 1 : 0, cutscene ? 1 : 0);
        }
        if (w.phase == 0 && ready && LocalChrPose(w.back_pos, w.back_rot)) {
            alignas(16) const float to[4] = {t->spot[0], t->spot[1], t->spot[2], 1.0f};
            FirstFightHold(kFirstFightVisitSeconds + 1.0 + kFirstFightHoldAfter);
            if (WarpLocalChr(to, w.back_rot)) {
                ++w.tries;
                w.phase = 1;
                w.visit_at = now;
                Log("host: story event %u (try %d): the host's chr visits its spot (%.2f, %.2f, %.2f) from (%.2f, "
                    "%.2f, %.2f)", w.event, w.tries, to[0], to[1], to[2], w.back_pos[0], w.back_pos[1], w.back_pos[2]);
                party::status::SetLastEvent("A party member started a story scene");
            }
        }
        ++it;
    }
}
} // namespace

std::mutex g_lift_wait_mu;
std::map<std::uint32_t, bool> g_lift_wait;  // host: lift pos flag -> a guest's end, applied once the host is out

// liftfix: the host's answer to a guest's ride (HostLiftSync) and its write. Occupied: the host stands in
// the car (within kLiftInCarRadius of its axis: 0.7.1 took 4 m, which on a landing beside Cainhurst's car
// refused every ride) or its own copy rides.
LiftSync HostLiftDecide(const Lift& l, bool want, bool* ok) {
    bool pos = false, busy = false;
    progress::ReadFlag(l.pos, &pos);
    if (l.busy) progress::ReadFlag(l.busy, &busy);
    ObjAnimState car;
    const bool loaded = ReadObjAnim(l.car, &car);
    const bool occupied = loaded && l.Replays() &&
                          (LiftBusyHere(l, kLiftInCarRadius) || liftrules::MirrorHold(l, car.anim, car.playing));
    const LiftSync what = HostLiftSync(pos, busy, loaded, want, l.Replays(), occupied);
    *ok = true;
    if (what == LiftSync::apply) {
        *ok = progress::WriteFlagOwnSave(l.pos, want);
        if (l.host_state) {
            *ok = progress::WriteFlagOwnSave(l.host_state, want) && *ok;
        }
    }
    return what;
}

// liftfix: guests' rides refused while the host stood in the car, applied once it is out.
void HostLiftWaitTick() {
    std::map<std::uint32_t, bool> wait;
    {
        std::lock_guard<std::mutex> lk(g_lift_wait_mu);
        wait.swap(g_lift_wait);
    }
    for (const auto& [id, want] : wait) {
        const Lift* l = LiftByPosFlag(id);
        bool ok = true;
        const LiftSync what = l ? HostLiftDecide(*l, want, &ok) : LiftSync::same;
        if (what == LiftSync::occupied || what == LiftSync::busy) {
            std::lock_guard<std::mutex> lk(g_lift_wait_mu);
            g_lift_wait.emplace(id, want);  // still in the car / riding
        } else if (l) {
            Log("host: lift %d %u=%d (a guest's ride): %s", l->car, id, want ? 1 : 0,
                what == LiftSync::apply ? (ok ? "applied now that the host is out of the car" : "NOT written")
                                        : "the host's world has it by now");
        }
    }
}

void HostTick(const GameSnapshot& s) {
    State& st = S();
    if (s.world_up && !s.loading && progress::FlagLoadMode() == 0) {
        HostLiftWaitTick();
    }
    std::deque<std::pair<int, Request>> in;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        if (!s.world_up || s.loading || progress::FlagLoadMode() != 0) {
            return;  // kept until the host's world is steady
        }
        in.swap(st.in);
    }
    for (const auto& [slot, r] : in) {
        if (!st.rate.Take(slot, int(r.set.size() + r.clr.size() + r.lift_set.size() + r.lift_clr.size()), Now())) {
            ++st.stats.refused;
            Log("host: world action from slot %d refused: rate limit", slot);
            continue;
        }
        std::string list;
        bool applied_any = false;
        // Split travel: a guest alone in a map the host is not in changes that map's flags; they
        // are checked against the map it stands in (docs/party/split_travel.md 6).
        const std::uint32_t check_map = b2::MemberApart(slot) ? b2::MemberMap(slot) : s.map_id;
        if (check_map != s.map_id) {
            Log("host: world action from slot %d, alone in %s: checked against its map", slot, MapName(check_map).c_str());
        }
        auto apply = [&](std::uint32_t id, bool v) {
            if (!WorldActionAllowed(id, !v, check_map) &&
                !BossKillActionAllowed(id, !v, check_map, check_map != s.map_id)) {
                ++st.stats.refused;
                list += " " + std::to_string(id) + "(refused)";
                return;
            }
            bool before = false;
            progress::ReadFlag(id, &before);
            const bool ok = before == v || progress::WriteFlagOwnSave(id, v);
            st.stats.applied += ok && before != v ? 1 : 0;
            applied_any = applied_any || (ok && before != v);
            list += " " + std::to_string(id) + (v ? "=1" : "=0") + (before == v ? "(was)" : ok ? "" : "(NOT written)") +
                    "[" + progress::CategoryName(progress::CategoryOf(id)) + "]";
        };
        for (std::uint32_t id : r.set) {
            bool was = false;
            const KeyItemGate* kg = KeyItemGateByOpenFlag(id);
            const bool kg_read = kg && progress::ReadFlag(id, &was);
            apply(id, true);
            bool now_on = false;
            if (kg_read && !was && progress::ReadFlag(id, &now_on) && now_on) {
                // D6: a guest opened a key-item gate with its own key: the host's copy of the
                // event still waits at the gate; the far-side ObjAct opens it there (animation).
                Log("host: slot %d opened key-item gate %d (flag %u): its far-side ObjAct %d is kicked here", slot,
                    kg->event, id, kg->far_kick);
                ReplayKick(slot, kg->far_kick, Now());
            }
        }
        for (std::uint32_t id : r.clr) {
            apply(id, false);
        }
        for (std::int32_t k : r.kick) {
            // D6: a key-item gate's far-side kick only while the gate is still shut in the host's
            // world (the host's own copy usually opened it already from the guest's press).
            const KeyItemGate* kg = KeyItemGateFor(12400760);
            bool open_now = false;
            if (kg && k == kg->far_kick && progress::ReadFlag(kg->open_flag, &open_now) && open_now) {
                Log("host: slot %d's gate kick %d: the gate is open here already; nothing to replay", slot, k);
                continue;
            }
            ReplayKick(slot, k, Now());
        }
        for (std::uint32_t e : r.story) {
            StoryRequest(e, slot);
        }
        for (const auto& [ids, want] : {std::make_pair(&r.lift_set, true), std::make_pair(&r.lift_clr, false)}) {
            for (std::uint32_t id : *ids) {
                // D3 (lifts071): a guest's finished lift ride. The host's own copy ran it when the
                // lift's block is loaded here (the game's own sync, `same`); with the block streamed
                // out it never ran, so the host's world kept the car at the old end and every game
                // that loads the block later put it there too (live l7: Cathedral Ward's lift, the
                // host far away in Central Yharnam). The flags are written; the block's init event
                // puts the car at that end when it streams in, the mirror carries them to the party.
                const Lift* l = LiftByPosFlag(id);
                if (!l || l->pos / 100000u % 100u != (check_map >> 24)) {
                    ++st.stats.refused;
                    list += " lift " + std::to_string(id) + "(refused: another area)";
                    continue;
                }
                bool ok = true;
                const LiftSync what = HostLiftDecide(*l, want, &ok);
                if (what == LiftSync::apply) {
                    st.stats.applied += ok ? 1 : 0;
                    applied_any = applied_any || ok;
                } else if (what == LiftSync::occupied) {
                    // liftfix: applied once the host is out of the car (HostLiftWaitTick): a guest
                    // reports a ride once, and a copy that did not follow the guest's ride (Cainhurst's
                    // while the host sat in a cutscene, live run 5) would keep the host's car at the old end.
                    std::lock_guard<std::mutex> lk(g_lift_wait_mu);
                    g_lift_wait[l->pos] = want;
                }
                list += " lift " + std::to_string(l->car) + " " + std::to_string(id) + (want ? "=1" : "=0") +
                        (what == LiftSync::same    ? "(same here)"
                         : what == LiftSync::busy  ? "(a ride runs here: left alone)"
                         : what == LiftSync::loaded ? "(car loaded here, its copy did not follow: left alone)"
                         : what == LiftSync::occupied ? "(the host stands in the car: applied once it is out)"
                         : ok                      ? "(applied: the lift's block is not loaded here)"
                                                   : "(NOT written)");
            }
        }
        for (std::uint32_t f : r.fog) {
            // B2: the guest went through a boss fog before the host: the host's game sets its
            // host-entered flag, which starts the boss (its `ボスが動き出す` waits on it). The
            // host still walks in through its own fog.
            const BossGate* g = BossGateByGuestFlag(f);
            bool host_in = false, dead = false, intro = false;
            if (!g) {
                continue;
            }
            progress::ReadFlag(g->host_flag, &host_in);
            progress::ReadFlag(g->boss_flag, &dead);
            if (g->intro_flag) {
                progress::ReadFlag(g->intro_flag, &intro);
            }
            const FogAction act = HostFogAction(*g, GuestFirstFightEnabled(), intro, host_in, dead, s.map_id);
            if (act == FogAction::start_first || act == FogAction::wait_host) {
                // P5 / D10: a guest went through the fog of a first fight: the host's first-fight
                // event runs as if the host had walked in (its scene plays for everyone).
                FirstFightRequest(*g, slot, s.map_id);
                continue;
            }
            if (act != FogAction::open) {
                Log("host: boss fog: slot %d went in (flag %u); nothing to open (host in %d, boss dead %d, host in %s)",
                    slot, f, host_in ? 1 : 0, dead ? 1 : 0, MapName(s.map_id).c_str());
                continue;
            }
            const bool ok = progress::WriteFlagOwnSave(g->host_flag, true);
            bool after = false;
            progress::ReadFlag(g->host_flag, &after);
            Log("host: boss fog: slot %d went in first (event %u): host-entered flag %u%s -> %d; the boss starts, "
                "the host's own fog stays", slot, g->guest_event, g->host_flag, ok ? "" : " NOT written", after ? 1 : 0);
        }
        if (applied_any) {
            // The host's own save holds the host's world: without a request the change waited for
            // the game's next autosave, and a quit soon after lost it (NPC quest flags read 0 on the
            // host's Continue while the guest's own save had them).
            RequestGameSave("guest world action (host's world)");
        }
        if (!list.empty()) {
            Log("host: slot %d changed the host's world (guest in %s, host in %s):%s", slot, MapName(r.map).c_str(),
                MapName(s.map_id).c_str(), list.c_str());
        }
    }
}

bool PutLocal(const float target[3], float px, float pz, float dist, float angle_deg, std::string* what) {
    const std::uint64_t wcm = Guest(0x553e878);
    std::uint64_t man = 0, player = 0, vt = 0, fn = 0;
    if (!wcm || !SafeGet(wcm, &man) || !man || !SafeGet(man + 0x60, &player) || !player ||
        !SafeGet(player, &vt) || !SafeGet(vt + 0x5f8, &fn) || !fn) {
        return false;
    }
    float h = std::sqrt(px * px + pz * pz);
    if (h < 1e-3f) {
        px = 0, pz = 1, h = 1;
    }
    px /= h, pz /= h;
    const float a = angle_deg * 3.14159265f / 180.0f;
    const float dx = px * std::cos(a) + pz * std::sin(a), dz = -px * std::sin(a) + pz * std::cos(a);
    alignas(16) float pos[4] = {target[0] + dx * dist, target[1], target[2] + dz * dist, 1.0f};
    // forward(yaw) = (-sin yaw, -cos yaw) (party_guestsvc StandAt): face the target.
    const float yaw = std::atan2(-(target[0] - pos[0]), -(target[2] - pos[2]));
    alignas(16) float rot[4] = {0, yaw, 0, 0};
    using WarpFn = void(BB_COOP_SYSV*)(std::uint64_t, const float*, const float*, std::uint64_t);
    reinterpret_cast<WarpFn>(fn)(player, pos, rot, 0);
    char b[160];
    std::snprintf(b, sizeof b, "target (%.2f, %.2f, %.2f), standing at (%.2f, %.2f, %.2f) yaw %.2f", target[0],
                  target[1], target[2], pos[0], pos[1], pos[2], yaw);
    if (what) {
        *what = b;
    }
    return true;
}


// BB_PARTY_WORLDACT_TEST (live tests, the guest): "obj=E[/E...];chr=E[/E...];after=S": S s
// (default 8) after the guest's world became steady in the host's world, for each map object
// (lever, door) and then each character (NPC): stand around it (8 directions x 1.0 / 1.6 m) until
// an action prompt shows, press Cross (then three more for a talk: the dialogue lines; "talk=N": N
// presses in all), wait, next.
struct Fixture {
    bool parsed = false;
    std::vector<std::int32_t> objs, chrs;
    int pickups = 0;  // "pickup=N": the N treasures nearest the guest, one try each
    int pickups0 = 0;  // its parsed N (FixtureRearm)
    std::string map;  // "map=NAME": only while the current map's name contains NAME
    int kills = 0;    // "kill=N": N times the nearest live map enemy: ForceDead, then pick up its drop
    std::int32_t loot = 0;  // "loot=ENTITY": wait until that enemy is dead (killed anywhere), then pick up its drop
    std::uint32_t grab = 0;  // "grab=FLAG": the treasure with that pickup flag, picked up through the game's own
                             // pickup function 0x17D8570 (what Cross on it calls) - no standing spot needed
    bool lootall = false;    // "lootall": every enemy whose drop this game rolls: stand on it, Cross x5
    std::size_t lootall_idx = 0;
    int lootall_stage = 0;
    int loot_stage = 0;
    double loot_since = 0;
    bool attack = false;  // "attack": kill with the weapon (R1 taps) instead of ForceDead
    bool has_near = false;  // "near=X:Y:Z": the treasure nearest that point, not the player
    float near[3] = {};
    int kill_stage = 0;
    float kill_pos[3] = {};
    std::int32_t kill_id = 0;
    double after = 8;
    int talk = 4;  // "talk=N": Cross presses at a character (the first opens the talk), default 4
    std::uint32_t prompt = 0;  // "prompt=ID": press only while that action prompt is the only one up
    double gap = 0;            // "gap=S": S s before each further object / character
    std::size_t idx = 0;
    int tries = 0, presses = 0;
    double next = 0, release = 0;
    bool done = false;
} g_fx;

void FixtureList(const std::string& v, const char* key, std::vector<std::int32_t>* out) {
    const std::size_t at = v.find(key);
    if (at == std::string::npos) {
        return;
    }
    const std::size_t from = at + std::strlen(key), end = v.find(';', at);
    std::string part = v.substr(from, end == std::string::npos ? std::string::npos : end - from);
    for (char& c : part) {
        c = c == '/' ? ',' : c;
    }
    for (std::uint32_t id : ParseIds(part.c_str())) {
        out->push_back(std::int32_t(id));
    }
}

// The local player's position.
bool MyPos(float out[3]) {
    std::uint64_t man = 0, player = 0;
    if (!SafeGet(Guest(0x553e878), &man) || !man || !SafeGet(man + 0x60, &player) || !player) {
        return false;
    }
    // The physics position first (PickPlayerPos: +0xC0 goes stale on a guest in the host's world).
    std::uint64_t a = 0, b = 0, c = 0, d = 0;
    float phys[3] = {}, drawn[4] = {};
    const bool phys_ok = SafeGet(player + 0x58, &a) && a && SafeGet(a + 8, &b) && b && SafeGet(b + 0x3b0, &c) && c &&
                         SafeGet(c + 0x68, &d) && d && SafeRead(d + 0x1e0, phys, sizeof phys);
    const bool drawn_ok = SafeRead(player + 0xc0, drawn, sizeof drawn);
    const float* p = PickPlayerPos(phys_ok, phys, drawn_ok, drawn);
    if (!p) {
        return false;
    }
    std::memcpy(out, p, 12);
    return true;
}

// The nearest live map character (WorldChrMan block lists: +0x28 blocks of 0x160, count +0x20;
// block +0xC0 list of 0x38-byte entries, count +0xB8; entry +0 the ChrIns) within 45 m that is not
// a player (PlayerIns +0x78 chr type is only meaningful for players: skip the local player and
// chrs whose entity +0x3B8 is negative).
std::uint64_t NearestEnemy(const float me[3], float pos[3], const std::vector<std::uint64_t>& skip) {
    std::uint64_t wcm = 0, blocks = 0, local = 0;
    std::int32_t nb = 0;
    if (!SafeGet(Guest(0x553e878), &wcm) || !wcm || !SafeGet(wcm + 0x20, &nb) || !SafeGet(wcm + 0x28, &blocks) ||
        !blocks || nb <= 0 || nb > 64) {
        return 0;
    }
    SafeGet(wcm + 0x60, &local);
    std::uint64_t best = 0;
    float bd = 45.0f * 45.0f;
    int seen = 0, dead = 0, logged = 0;
    for (std::int32_t b = 0; b < nb; ++b) {
        std::uint64_t list = 0;
        std::int32_t n = 0;
        if (!SafeGet(blocks + std::uint64_t(b) * 0x160 + 0xb8, &n) || n <= 0 || n > 4096 ||
            !SafeGet(blocks + std::uint64_t(b) * 0x160 + 0xc0, &list) || !list) {
            continue;
        }
        for (std::int32_t k = 0; k < n; ++k) {
            std::uint64_t chr = 0, cm = 0, dm = 0;
            std::int32_t hp = 0;
            float p[4] = {};
            if (!SafeGet(list + std::uint64_t(k) * 0x38, &chr) || !chr || chr == local ||
                std::find(skip.begin(), skip.end(), chr) != skip.end() || !SafeRead(chr + 0xc0, p, sizeof p)) {
                continue;
            }
            ++seen;
            if (!SafeGet(chr + 0x3b0, &cm) || !cm || !SafeGet(cm + 0x20, &dm) || !dm || !SafeGet(dm + 0xf8, &hp) ||
                hp <= 0) {
                ++dead;
                continue;
            }
            const float dx = p[0] - me[0], dy = p[1] - me[1], dz = p[2] - me[2];
            const float d = dx * dx + dy * dy + dz * dz;
            std::int32_t ent = 0, type = -1;
            SafeGet(chr + 0x3b8, &ent);
            SafeGet(chr + 0x78, &type);
            if ((ent >= 10000000 && ent <= 10000010) || type == 0 || type == 1 || type == 2 || type == 8 || type == 12) {
                continue;  // a player (the host or another guest: chr type 0/1/2/8/12), not an enemy
            }
            if (type == 4) {
                continue;  // live (Central Yharnam lamp): a type-4 chr at the lamp took no damage - not hostile
            }
            if (hp >= 999) {
                continue;  // live: an untouchable 999-HP chr stands on the lamp (the lamp's own chr)
            }
            if (d < 30.0f * 30.0f && logged < 6) {
                ++logged;
                Log("test: fixture: candidate chr %p +0x3B8 %d +0x78 %d HP %d at %.1f m", reinterpret_cast<void*>(chr),
                    ent, type, hp, std::sqrt(d));
            }
            if (d < bd) {
                bd = d;
                best = chr;
                std::memcpy(pos, p, 12);
            }
        }
    }
    if (!best) {
        Log("test: fixture: map characters seen: %d (%d dead / unreadable), none alive within 45 m", seen, dead);
    }
    return best;
}

void KillTick(Fixture& f, double now) {
    static std::vector<std::uint64_t> done_ids;
    static std::uint64_t kill_chr = 0;
    float me[3] = {};
    if (!MyPos(me)) {
        return;
    }
    if (f.kill_stage == 0) {
        kill_chr = NearestEnemy(me, f.kill_pos, done_ids);
        f.kill_id = 0;
        if (kill_chr) {
            SafeGet(kill_chr + 0x3b8, &f.kill_id);
        }
        if (!kill_chr) {
            Log("test: fixture: no live enemy within 45 m");
            f.kills = 0;
            return;
        }
        done_ids.push_back(kill_chr);
        if (f.attack) {
            f.kill_stage = 10;  // weapon: R1 taps facing it (stages 10..)
            Log("test: fixture: attacking enemy (chr +0x3B8 %d) at (%.1f, %.1f, %.1f) with the weapon", f.kill_id,
                f.kill_pos[0], f.kill_pos[1], f.kill_pos[2]);
            return;
        }
        // What Lua ForceDead (0x13320C0) does with the chr: clear +0x1E3 bit 5, then 0x18BF9A0(chr)
        // (HP 0 and the dead flag): the enemy's normal death on this game.
        std::uint8_t b = 0;
        if (SafeGet(kill_chr + 0x1e3, &b)) {
            b = std::uint8_t(b & 0xdf);
            SafeWrite(kill_chr + 0x1e3, &b, 1);
        }
        reinterpret_cast<void(BB_COOP_SYSV*)(std::uint64_t)>(Guest(0x18bf9a0))(kill_chr);
        Log("test: fixture: enemy (chr +0x3B8 %d) at (%.1f, %.1f, %.1f) killed by this player (ForceDead)", f.kill_id,
            f.kill_pos[0], f.kill_pos[1], f.kill_pos[2]);
        f.kill_stage = 1;
        f.next = now + 6.0;  // death animation; the drop appears
        return;
    }
    if (f.kill_stage >= 10) {
        // Stand 1.3 m in front of it, facing it, R1; until its HP is 0 (60 taps at most).
        std::uint64_t cm = 0, dm = 0;
        std::int32_t hp = 1;
        float p[4] = {};
        if (SafeRead(kill_chr + 0xc0, p, sizeof p)) {
            std::memcpy(f.kill_pos, p, 12);
        }
        const bool read = SafeGet(kill_chr + 0x3b0, &cm) && cm && SafeGet(cm + 0x20, &dm) && dm && SafeGet(dm + 0xf8, &hp);
        if ((read && hp <= 0) || f.kill_stage >= 70) {
            Log("test: fixture: enemy (chr +0x3B8 %d) %s after %d weapon tap(s)", f.kill_id,
                read && hp <= 0 ? "killed by this player's weapon" : "still alive", f.kill_stage - 10);
            f.kill_stage = 1;
            f.next = now + 6.0;
            return;
        }
        if ((f.kill_stage - 10) % 6 == 0) {
            std::string what;
            PutLocal(f.kill_pos, 0.0f, 1.0f, 1.3f, 0.0f, &what);
        }
        runtime_pad_director_press(0x800);  // R1
        f.release = now + 0.12;
        ++f.kill_stage;
        f.next = now + 0.7;
        Log("test: fixture: R1 %s, enemy HP %d", StepTag(f.kill_stage - 10).c_str(), read ? hp : -1);
        return;
    }
    if (f.kill_stage >= 1 && f.kill_stage <= 3 && std::getenv("BB_PARTY_WORLDACT_NOWALK")) {
        f.kill_stage = 4;  // no walk to the corpse: the drop is taken through the pickup function (lootall)
    }
    if (f.kill_stage >= 1 && f.kill_stage <= 3) {
        std::string what;
        if (f.kill_stage == 1) {
            PutLocal(f.kill_pos, 0.0f, 1.0f, 0.3f, 0.0f, &what);
            Log("test: fixture: at enemy %d's spot: %s", f.kill_id, what.c_str());
        }
        std::uint32_t pp = 0;
        const bool up = GuestSvcPromptUp(&pp);
        runtime_pad_director_press(0x4000);
        f.release = now + 0.15;
        Log("test: fixture: drop of enemy %d: prompt %s %u; Cross", f.kill_id, up ? "up" : "not seen", pp);
        ++f.kill_stage;
        f.next = now + 2.0;
        return;
    }
    f.kill_stage = 0;
    --f.kills;
    f.next = now + 2.0;
}

void FixtureTick(double now) {
    Fixture& f = g_fx;
    if (static const bool watch = std::getenv("BB_PARTY_ANIM_WATCH") != nullptr; watch) {
        // Test aid: every new animation the local player's TimeAct ring ([[chr+0x3B0]+0x18]: 10 x
        // {anim id, ...} at +0x20, next write index at +0xC0) records, so an animation that the other
        // players' actions start on this player shows in the log.
        static std::int32_t last_idx = -1;
        std::uint64_t wcm = 0, player = 0, cm = 0, ta = 0;
        std::int32_t idx = -1;
        if (SafeGet(Guest(0x553e878), &wcm) && wcm && SafeGet(wcm + 0x60, &player) && player &&
            SafeGet(player + 0x3b0, &cm) && cm && SafeGet(cm + 0x18, &ta) && ta && SafeGet(ta + 0xc0, &idx) &&
            idx >= 0 && idx < 10 && idx != last_idx) {
            static std::int32_t last_anim = 0;
            std::int32_t anim = 0;
            SafeGet(ta + 0x20 + std::uint64_t((idx + 9) % 10) * 0x10, &anim);
            if (last_idx >= 0 && anim != last_anim) {
                Log("test: anim watch: local player animation %d (t %.2f)", anim, now);
            }
            last_anim = anim;
            last_idx = idx;
        }
        // The pickup's net event carries the sender's play id (PlayerGameData +0x10); a receiver
        // plays the animation on the player chr whose +0x3B8 equals it (0x13C97A0, entity 10001 + id).
        static std::string last_ids;
        std::uint64_t gdm = 0, pgd = 0;
        std::int32_t pid = -1, n = 0;
        if (SafeGet(Guest(0x553b130), &gdm) && gdm && SafeGet(gdm + 8, &pgd) && pgd) {
            SafeGet(pgd + 0x10, &pid);
        }
        std::string ids = "own play id " + std::to_string(pid) + "; player chrs";
        std::uint64_t arr = 0;
        if (wcm && SafeGet(wcm + 0x30, &n) && SafeGet(wcm + 0x38, &arr) && arr && n > 0 && n <= 8) {
            for (std::int32_t i = 0; i < n; ++i) {
                std::uint64_t c = 0;
                std::int32_t no = -1;
                if (SafeGet(arr + std::uint64_t(i) * 0x38, &c) && c) {
                    SafeGet(c + 0x3b8, &no);
                    ids += " [" + std::to_string(i) + (c == player ? " local" : "") + " +0x3B8 " + std::to_string(no) + "]";
                }
            }
        }
        if (ids != last_ids) {
            Log("test: anim watch: %s", ids.c_str());
            last_ids = ids;
        }
        // "BB_PARTY_ANIM_WATCH=210691,210698": the same ring of these characters (talk id or entity:
        // GuestSvcFindChr), e.g. the Bath Messengers while another player uses their shop.
        static const std::vector<std::uint32_t> npcs = ParseIds(std::getenv("BB_PARTY_ANIM_WATCH"));
        static std::int32_t npc_idx[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
        static std::int32_t npc_anim[8] = {};
        for (std::size_t i = 0; i < npcs.size() && i < 8; ++i) {
            const std::uint64_t c = GuestSvcFindChr(npcs[i]);
            std::uint64_t ncm = 0, nta = 0;
            std::int32_t ni = -1, anim = 0;
            if (!c || !SafeGet(c + 0x3b0, &ncm) || !ncm || !SafeGet(ncm + 0x18, &nta) || !nta ||
                !SafeGet(nta + 0xc0, &ni) || ni < 0 || ni >= 10 || ni == npc_idx[i]) {
                continue;
            }
            SafeGet(nta + 0x20 + std::uint64_t((ni + 9) % 10) * 0x10, &anim);
            if (npc_idx[i] >= 0 && anim != npc_anim[i]) {
                Log("test: anim watch: npc %u animation %d (t %.2f)", npcs[i], anim, now);
            }
            npc_anim[i] = anim;
            npc_idx[i] = ni;
        }
    }
    if (const std::uint64_t m = g_rec_man.load()) {
        // Test diagnostics: the treasure record count on this game, logged when it changes.
        static int last = -1;
        std::uint64_t node = 0;
        int c = 0;
        SafeGet(m + 0x28, &node);
        for (; node && c < 4096; ++c) {
            if (!SafeGet(node, &node)) {
                break;
            }
        }
        if (c != last) {
            Log("test: treasure records on this game: %d -> %d (inserts %d, removes %d, role %d)", last, c,
                g_rec[3].load(), g_rec[0].load(), LocalSessionRole());
            last = c;
        }
    }
    if (!f.parsed) {
        f.parsed = true;
        const char* t = std::getenv("BB_PARTY_WORLDACT_TEST");
        if (!t || !t[0]) {
            f.done = true;
            return;
        }
        const std::string v(t);
        FixtureList(v, "obj=", &f.objs);
        FixtureList(v, "chr=", &f.chrs);
        if (const std::size_t a = v.find("after="); a != std::string::npos) {
            f.after = std::atof(v.c_str() + a + 6);
        }
        if (const std::size_t a = v.find("pickup="); a != std::string::npos) {
            f.pickups = std::atoi(v.c_str() + a + 7);
            f.pickups0 = f.pickups;
        }
        if (const std::size_t a = v.find("map="); a != std::string::npos) {
            f.map = v.substr(a + 4, v.find(';', a) == std::string::npos ? std::string::npos : v.find(';', a) - a - 4);
        }
        f.attack = v.find("attack") != std::string::npos;
        if (const std::size_t a = v.find("near="); a != std::string::npos) {
            f.has_near = std::sscanf(v.c_str() + a + 5, "%f:%f:%f", &f.near[0], &f.near[1], &f.near[2]) == 3;
        }
        if (const std::size_t a = v.find("grab="); a != std::string::npos) {
            f.grab = std::uint32_t(std::strtoul(v.c_str() + a + 5, nullptr, 10));
        }
        f.lootall = v.find("lootall") != std::string::npos;
        if (const std::size_t a = v.find("loot="); a != std::string::npos) {
            f.loot = std::atoi(v.c_str() + a + 5);
        }
        if (const std::size_t a = v.find("talk="); a != std::string::npos) {
            f.talk = std::clamp(std::atoi(v.c_str() + a + 5), 1, 12);
        }
        if (const std::size_t a = v.find("kill="); a != std::string::npos) {
            f.kills = std::atoi(v.c_str() + a + 5);
        }
        if (const std::size_t a = v.find("prompt="); a != std::string::npos) {
            f.prompt = std::uint32_t(std::strtoul(v.c_str() + a + 7, nullptr, 10));
        }
        if (const std::size_t a = v.find("gap="); a != std::string::npos) {
            f.gap = std::atof(v.c_str() + a + 4);
        }
        Log("test: fixture: %zu object(s), %zu character(s), %.0f s after a steady world", f.objs.size(),
            f.chrs.size(), f.after);
    }
    if (f.release > 0 && now >= f.release) {
        runtime_pad_director_press(0);
        f.release = 0;
    }
    State& st = S();
    if (f.done || !st.steady || now - st.steady_since < f.after || now < f.next ||
        (!f.map.empty() && MapName(st.map).find(f.map) == std::string::npos)) {
        return;
    }
    const std::size_t total = f.objs.size() + f.chrs.size();
    if (f.idx >= total && (f.grab || f.lootall)) {
        // MapItemMan (*0x553D6E0) record lists (0x17D81A0): kind 3 (treasure) at +0x28, kind 2 (enemy
        // drops) at +0x38; node +0 next, +0x20 handle, +0x28 pickup flag, +0x54 item count, items from
        // +0x58 (16 bytes: +0 id with category bits, +4 count). The pickup 0x17D8570(man, &handle) is
        // what Cross on the item calls.
        std::uint64_t man = 0;
        if (!SafeGet(Guest(0x553d6e0), &man) || !man) {
            return;
        }
        auto items = [](std::uint64_t node) {
            std::int32_t n = 0;
            SafeGet(node + 0x54, &n);
            std::string out;
            for (std::int32_t i = 0; i < n && i < 8; ++i) {
                std::uint32_t id = 0, cnt = 0;
                SafeGet(node + 0x58 + std::uint64_t(i) * 16, &id);
                SafeGet(node + 0x58 + std::uint64_t(i) * 16 + 4, &cnt);
                char b[48];
                std::snprintf(b, sizeof b, " %#x x%u", id, cnt);
                out += b;
            }
            return out.empty() ? std::string(" (none)") : out;
        };
        using Pick = void(BB_COOP_SYSV*)(std::uint64_t, std::int32_t*);
        const Pick pick = reinterpret_cast<Pick>(Guest(0x17d8570));
        if (f.grab) {
            std::uint64_t node = 0;
            SafeGet(man + 0x28, &node);
            std::uint64_t hit = 0;
            int count = 0;
            std::string seen;
            for (int guard = 0; node && guard < 4096; ++guard, ++count) {
                std::uint32_t flag = 0;
                SafeGet(node + 0x28, &flag);
                if (flag == f.grab) {
                    hit = node;
                }
                if (count < 6) {
                    seen += " " + std::to_string(flag);
                }
                if (!SafeGet(node, &node)) {
                    break;
                }
            }
            if (!hit) {
                int n18 = 0, n38 = 0;
                for (std::uint64_t list : {man + 0x18, man + 0x38}) {
                    std::uint64_t q = 0;
                    SafeGet(list, &q);
                    int c = 0;
                    for (; q && c < 4096; ++c) {
                        if (!SafeGet(q, &q)) {
                            break;
                        }
                    }
                    (list == man + 0x18 ? n18 : n38) = c;
                }
                Log("test: fixture: grab: no treasure record with flag %u (%d kind-3 records, first flags%s; kind-1 %d, "
                    "kind-2 %d; MapItemMan %p, inserts went to %p); inserts k1 %d k2 %d k3 %d, removes %d; spawn gates: pass %d, no gd %d, role %d, slot %d, chr type %d, handle %d, entity %d",
                    f.grab, count, seen.c_str(), n18, n38, reinterpret_cast<void*>(man), reinterpret_cast<void*>(g_rec_man.load()), g_rec[1].load(), g_rec[2].load(), g_rec[3].load(), g_rec[0].load(),
                    g_tgate[0].load(), g_tgate[1].load(), g_tgate[2].load(),
                    g_tgate[3].load(), g_tgate[4].load(), g_tgate[5].load(), g_tgate[6].load());
            } else {
                bool vis = false, own = false, host = false;
                progress::ReadFlag(f.grab, &vis);
                progress::ReadFlagOwnSave(f.grab, &own);
                progress::HostWorldFlag(f.grab, &host);
                std::int32_t handle = 0;
                SafeGet(hit + 0x20, &handle);
                const std::uint64_t picks0 = g_pickups.load();
                const std::string what = items(hit);
                std::uint16_t f48 = 0;
                SafeGet(hit + 0x48, &f48);
                std::int32_t f24 = 0;
                SafeGet(hit + 0x24, &f24);
                Log("test: fixture: grab: record flags +0x48 %#x (0x40 live, 0x80 host-owned), +0x24 %d", unsigned(f48), f24);
                using Find = std::uint64_t(BB_COOP_SYSV*)(std::uint64_t, std::int32_t);
                const std::uint64_t found = reinterpret_cast<Find>(Guest(0x17d81a0))(man, handle);
                std::string hs;
                {
                    std::uint64_t q = 0;
                    SafeGet(man + 0x28, &q);
                    for (int g = 0; q && g < 12; ++g) {
                        std::int32_t hh = 0, ff = 0;
                        std::uint16_t fl = 0;
                        SafeGet(q + 0x20, &hh);
                        SafeGet(q + 0x28, &ff);
                        SafeGet(q + 0x48, &fl);
                        char b[64];
                        std::snprintf(b, sizeof b, " [%#x %d %#x]", unsigned(hh), ff, unsigned(fl));
                        hs += b;
                        if (!SafeGet(q, &q)) {
                            break;
                        }
                    }
                }
                Log("test: fixture: grab: lookup 0x17D81A0(%#x) -> %p (record %p); list [handle flag bits]:%s, next handle %#x",
                    unsigned(handle), reinterpret_cast<void*>(found), reinterpret_cast<void*>(hit), hs.c_str(),
                    [&] { std::uint32_t c = 0; SafeGet(man + 0xc, &c); return c; }());
                const int rm0 = g_rec[0].load();
                pick(man, &handle);
                {
                    bool still = false;
                    std::uint64_t q = 0;
                    SafeGet(man + 0x28, &q);
                    for (int g = 0; q && g < 4096; ++g) {
                        still = still || q == hit;
                        if (!SafeGet(q, &q)) {
                            break;
                        }
                    }
                    std::uint16_t f2 = 0;
                    SafeGet(hit + 0x48, &f2);
                    Log("test: fixture: grab: after the pickup: record %s, removes %d -> %d, flags %#x", still ? "still listed" : "gone",
                        rm0, g_rec[0].load(), unsigned(f2));
                }
                Log("test: fixture: grab: treasure flag %u (handle %#x, items%s) picked up through 0x17D8570: %s; "
                    "before: visible %d, own save %d, host %d", f.grab, unsigned(handle), what.c_str(),
                    g_pickups.load() > picks0 ? "pickup ran" : "pickup did NOT run", int(vis), int(own), int(host));
            }
            f.grab = 0;
            f.next = now + 3.0;
            return;
        }
        // lootall: every enemy drop record this game holds, once.
        static std::vector<std::int32_t> done;
        std::uint64_t node = 0;
        SafeGet(man + 0x38, &node);
        for (int guard = 0; node && guard < 4096; ++guard) {
            std::int32_t handle = 0;
            SafeGet(node + 0x20, &handle);
            if (std::find(done.begin(), done.end(), handle) == done.end()) {
                done.push_back(handle);
                const std::string what = items(node);
                const std::uint64_t picks0 = g_pickups.load();
                std::int32_t h = handle;
                pick(man, &h);
                Log("test: fixture: lootall: enemy drop record %#x (items%s) picked up through 0x17D8570: %s", unsigned(handle),
                    what.c_str(), g_pickups.load() > picks0 ? "pickup ran" : "pickup did NOT run");
                f.next = now + 2.0;
                return;
            }
            if (!SafeGet(node, &node)) {
                break;
            }
        }
        // Same text while waiting (the reaper sees an idle run); a hard 25 s timeout.
        static double since = 0;
        if (!since) {
            since = now;
        }
        if (now - since > 25.0) {
            Log("test: fixture: lootall: FAILED / done: no new drop record for 25 s; stopping");
            f.lootall = false;
            f.done = true;
            return;
        }
        Log("test: fixture: lootall: no new drop record yet");
        f.next = now + 5.0;
        return;
    }
    if (f.idx >= total && f.pickups <= 0 && f.loot > 0) {
        // The enemy's chr (0x13C97A0(entity)); its HP from the data module +0xF8 (max +0xFC).
        using Find = std::uint64_t(BB_COOP_SYSV*)(std::int32_t, std::int32_t, std::uint64_t);
        const std::uint64_t chr = reinterpret_cast<Find>(Guest(0x13c97a0))(f.loot, f.loot, 0);
        std::uint64_t cm = 0, dm = 0;
        std::int32_t hp = -1;
        float p[4] = {};
        const bool ok = chr && SafeRead(chr + 0xc0, p, sizeof p) && SafeGet(chr + 0x3b0, &cm) && cm &&
                        SafeGet(cm + 0x20, &dm) && dm && SafeGet(dm + 0xf8, &hp);
        if (f.loot_stage == 0) {
            if (!f.loot_since) {
                f.loot_since = now;
                Log("test: fixture: loot: waiting for enemy %d to die (%s, HP %d)", f.loot, ok ? "here" : "not found", hp);
            }
            if (ok && hp <= 0) {
                std::memcpy(f.kill_pos, p, 12);
                Log("test: fixture: loot: enemy %d is dead at (%.1f, %.1f, %.1f); its drop next", f.loot, p[0], p[1], p[2]);
                f.loot_stage = 1;
                f.next = now + 5.0;  // death animation, the drop appears
            } else if (now - f.loot_since > 30) {
                Log("test: fixture: loot: enemy %d did not die", f.loot);
                f.loot = 0;
            } else {
                Log("test: fixture: loot: waiting for the enemy to die");  // same text: the reaper sees idleness
                f.next = now + 5.0;
            }
            return;
        }
        if (f.loot_stage <= 10) {
            static std::uint64_t picks0 = 0;
            if (f.loot_stage == 1) {
                picks0 = g_pickups.load();
            } else if (g_pickups.load() > picks0) {
                Log("test: fixture: loot: enemy %d's drop picked up after %d spot(s)", f.loot, f.loot_stage - 1);
                f.loot = 0;
                f.next = now + 3.0;
                return;
            }
            std::string what;
            PutLocal(f.kill_pos, 0.0f, 1.0f, f.loot_stage <= 5 ? 0.5f : 1.0f, float(f.loot_stage % 5) * 72.0f, &what);
            runtime_pad_director_press(0x4000);
            f.release = now + 0.15;
            Log("test: fixture: loot %s: Cross at the drop", StepTag(f.loot_stage).c_str());
            ++f.loot_stage;
            f.next = now + 1.2;
            return;
        }
        Log("test: fixture: loot: no pickup at enemy %d's spot", f.loot);
        f.loot = 0;
        return;
    }
    if (f.idx >= total && f.pickups <= 0 && f.kills > 0) {
        KillTick(f, now);
        return;
    }
    if (f.idx >= total && f.pickups > 0) {
        // The nearest treasure not tried yet: stand on it, Cross (pick up), next one 5 s later.
        static std::array<float, 3> cur{};
        static std::uint64_t picks_before = 0;
        if (f.presses >= 1) {
            // Around the treasure (8 directions x 0.6 / 1.1 m, facing it), Cross at each spot until
            // the game's pickup function ran (PickupEntry).
            if (g_pickups.load() > picks_before || f.presses > 16) {
                Log("test: fixture: treasure: %s after %d spot(s)",
                    g_pickups.load() > picks_before ? "picked up" : "no pickup", f.presses);
                f.presses = 0;
                f.next = now + 4.0;
                --f.pickups;
                return;
            }
            if (f.release > 0) {
                return;
            }
            std::string what;
            // 0.0 / 0.3 m first (the item glow is often off the MSB point), then 0.6 / 1.1 m around it.
            const float dd = f.presses <= 2 ? 0.3f * float(f.presses - 1) : f.presses <= 9 ? 0.6f : 1.1f;
            PutLocal(cur.data(), 0.0f, 1.0f, dd, float(f.presses % 8) * 45.0f, &what);
            Log("test: fixture: treasure %s: Cross", StepTag(f.presses).c_str());
            runtime_pad_director_press(0x4000);
            f.release = now + 0.15;
            ++f.presses;
            f.next = now + 1.0;
            return;
        }
        static std::vector<std::array<float, 3>> tried;
        const char* only = std::getenv("BB_PARTY_WORLDACT_TEST");
        const char* fl = only ? std::strstr(only, "flag=") : nullptr;
        const std::uint32_t want = fl ? std::uint32_t(std::strtoul(fl + 5, nullptr, 10)) : 0;
        float me[4] = {};
        std::uint64_t wcm = 0, man = 0, player = 0;
        if (!SafeGet(Guest(0x553e878), &man) || !man || !SafeGet(man + 0x60, &player) || !player ||
            !SafeRead(player + 0xc0, me, sizeof me)) {
            return;
        }
        (void)wcm;
        std::array<float, 3> best{};
        float bd = 1e9f;
        {
            std::lock_guard<std::mutex> lk(g_treasure_mu);
            for (const Treasure& tr : g_treasure) {
                const auto& t = tr.pos;
                if (std::find(tried.begin(), tried.end(), t) != tried.end() || (want && tr.flag != want)) {
                    continue;
                }
                bool taken = false;
                if (!want && tr.flag && progress::ReadFlag(tr.flag, &taken) && taken) {
                    continue;  // taken in this guest's view (its own pickups, instanced)
                }
                const float* from = f.has_near ? f.near : me;
                const float dx = t[0] - from[0], dy = t[1] - from[1], dz = t[2] - from[2];
                const float d = dx * dx + dy * dy + dz * dz;
                if (d < bd) {
                    bd = d;
                    best = t;
                }
            }
            if (f.tries == 0) {
                // The nearest treasures with their pickup flag and this player's view (taken or not).
                std::vector<std::pair<float, const Treasure*>> near;
                const float* from = f.has_near ? f.near : me;
                for (const Treasure& tr : g_treasure) {
                    const float dx = tr.pos[0] - from[0], dy = tr.pos[1] - from[1], dz = tr.pos[2] - from[2];
                    near.emplace_back(std::sqrt(dx * dx + dy * dy + dz * dz), &tr);
                }
                std::sort(near.begin(), near.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                for (std::size_t i = 0; i < near.size() && i < 8; ++i) {
                    bool taken = false;
                    const bool known = near[i].second->flag && progress::ReadFlag(near[i].second->flag, &taken);
                    Log("test: fixture: treasure %zu: (%.1f, %.1f, %.1f) %.1f m, flag %u %s", i, near[i].second->pos[0],
                        near[i].second->pos[1], near[i].second->pos[2], near[i].first, near[i].second->flag,
                        !known ? "?" : taken ? "taken" : "NOT taken");
                }
                std::size_t with = 0;
                for (const Treasure& tr : g_treasure) {
                    with += tr.flag ? 1 : 0;
                }
                Log("test: fixture: %zu treasure spawn(s) seen on this game (%zu with a pickup flag)", g_treasure.size(),
                    with);
            }
        }
        // flag=F names one treasure: go to it wherever it is in the loaded map (no 60 m limit).
        if (want ? bd > 1e8f : bd > 60.0f * 60.0f) {
            f.pickups = 0;
            Log("test: fixture: no untaken treasure within 60 m (nearest %.0f m)", std::sqrt(bd));
            return;
        }
        if (f.presses == 0) {
            tried.push_back(best);
            std::string what;
            cur = best;
            picks_before = g_pickups.load();
            PutLocal(best.data(), 0.0f, 1.0f, 0.6f, 0.0f, &what);
            Log("test: fixture: treasure at (%.1f, %.1f, %.1f), %.1f m away: %s", best[0], best[1], best[2],
                std::sqrt(bd), what.c_str());
            f.presses = 1;
            f.next = now + 1.2;
            ++f.tries;
            return;
        }
        std::uint32_t pp = 0;
        const bool up = GuestSvcPromptUp(&pp);
        Log("test: fixture: treasure: prompt %s %u; Cross", up ? "up" : "not seen", pp);
        runtime_pad_director_press(0x4000);
        f.release = now + 0.15;
        f.presses = 0;
        f.next = now + 5.0;
        --f.pickups;
        return;
    }
    if (f.idx >= total) {
        f.done = true;
        Log("test: fixture done");
        return;
    }
    const bool obj = f.idx < f.objs.size();
    const char* kind = obj ? "object" : "character";
    const std::int32_t ent = obj ? f.objs[f.idx] : f.chrs[f.idx - f.objs.size()];
    std::uint32_t prompt = 0;
    if (f.presses > 0) {
        if (!obj && f.presses < f.talk) {
            runtime_pad_director_press(0x4000);  // talk: the next dialogue lines
            f.release = now + 0.15;
            ++f.presses;
            f.next = now + 1.5;
            Log("test: fixture: character %d: Cross #%d", ent, f.presses);
            return;
        }
        Log("test: fixture: %s %d done", kind, ent);
        if (obj) {
            runtime_pad_director_press(0x2000);  // Circle: back out of a menu the object opened
            f.release = now + 0.15;
        }
        ++f.idx;
        f.tries = 0;
        f.presses = 0;
        f.next = now + 1.0 + f.gap;
        return;
    }
    std::uint32_t other = 0;
    if (f.tries > 0 && f.prompt && GuestSvcPromptUp(&prompt) && !GuestSvcOnlyPrompt(f.prompt, &other)) {
        Log("test: fixture: %s %d: prompt %u is not only %u (also %u); next spot", kind, ent, prompt, f.prompt, other);
    } else if (f.tries > 0 && GuestSvcPromptUp(&prompt)) {
        prompt = f.prompt ? f.prompt : prompt;
        Log("test: fixture: %s %d: prompt %u up after %d spot(s); Cross", kind, ent, prompt, f.tries);
        runtime_pad_director_press(0x4000);
        f.release = now + 0.15;
        f.presses = 1;
        f.next = now + (obj ? 8.0 : 1.5);
        return;
    }
    if (f.tries >= 16) {
        Log("test: fixture: %s %d: no prompt at 16 spots; next", kind, ent);
        ++f.idx;
        f.tries = 0;
        return;
    }
    const float angle = float(f.tries % 8) * 45.0f, dist = f.tries < 8 ? 1.0f : 1.6f;
    std::string what;
    const bool ok = obj ? StandAtObject(ent, dist, angle, &what) : StandAtChr(ent, dist, angle, &what);
    ++f.tries;
    f.next = now + 1.0;
    if (!ok) {
        Log("test: fixture: %s %d not loaded", kind, ent);
        f.tries = 16;
        return;
    }
    if (f.tries == 1 || f.tries == 9) {
        Log("test: fixture: %s %d: %s", kind, ent, what.c_str());
    }
}


// ObjAct kicks (levers, doors, elevators): the per-block ObjAct manager (0xD8 bytes; entries at
// +0x90, 0x140 each, count +0x98; entry +0x8 list index, +0xC kick event id, +0x58 object handle,
// +0x5C trigger state). 0x13AE370(blockman, idx, chr) = the local player's kick (the action button
// update 0x13ADCC0); 0x13AF830(blockman, idx, chr, state) = a kick another player made, applied
// by the ObjAct manager update 0x13B2790 from its network queue (0x13B2F90). Logged both ways.
std::atomic<std::uint64_t> g_kicks_local{0}, g_kicks_net{0};
void LogKick(const char* who, std::uint64_t man, std::uint64_t idx, std::uint64_t a3, std::uint64_t a4) {
    std::int32_t n = 0, list = -1, ev = -1, handle = -1, trig = -1;
    std::uint64_t arr = 0;
    if (SafeGet(man + 0x98, &n) && SafeGet(man + 0x90, &arr) && arr && std::int32_t(idx) >= 0 &&
        std::int32_t(idx) < n) {
        const std::uint64_t e = arr + (idx & 0xffffffffu) * 0x140;
        SafeGet(e + 0x8, &list);
        SafeGet(e + 0xc, &ev);
        SafeGet(e + 0x58, &handle);
        SafeGet(e + 0x5c, &trig);
    }
    Log("ObjAct kick (%s): block man %p entry %d: list %d, kick event %d, object handle %#x, trigger state %d; "
        "arg %d / %d", who, reinterpret_cast<void*>(man), std::int32_t(idx), list, ev, unsigned(handle), trig,
        std::int32_t(a3), std::int32_t(a4));
}
BB_COOP_SYSV void LocalKickEntry(std::uint64_t man, std::uint64_t idx, std::uint64_t chr, std::uint64_t, std::uint64_t,
                                 std::uint64_t) {
    g_kicks_local.fetch_add(1);
    LogKick("local player", man, idx, chr, 0);
    std::int32_t n = 0, ev = -1;
    std::uint64_t arr = 0;
    if (LocalSessionRole() == 6 && SafeGet(man + 0x98, &n) && SafeGet(man + 0x90, &arr) && arr &&
        std::int32_t(idx) >= 0 && std::int32_t(idx) < n && SafeGet(arr + (idx & 0xffffffffu) * 0x140 + 0xc, &ev) &&
        ev > 0) {
        std::lock_guard<std::mutex> lk(g_kick_mu);
        if (g_kicks_out.size() < 16) {
            g_kicks_out.push_back(ev);
        }
    }
}
// The gates of 0x17DC300 before its lot roll, evaluated like the game does (test fixture diagnostics):
// 0 passes, 1 no player GameData, 2 role 6, 3 session slot, 4 chr-type byte [2], 5 handle -1,
// 6 handle's map block / entity index out of range.
int TreasureGate(std::uint64_t handle, std::int32_t* role_out, std::int32_t* type_out) {
    std::uint64_t holder = 0, gd = 0, rolep = 0, gp = 0, slotp = 0, tab = 0;
    if (!SafeGet(Guest(0x553b130), &holder) || !holder || !SafeGet(holder + 8, &gd) || !gd) {
        return 1;
    }
    std::int32_t role = -1, slot = 0;
    std::uint32_t type = 0;
    if (SafeGet(Guest(0x5540290), &rolep) && rolep) {
        SafeGet(rolep + 0x124, &role);
    }
    *role_out = role;
    SafeGet(gd + 0xa4, &type);
    *type_out = std::int32_t(type);
    if (role == 6 && !S().pickups) {
        return 2;
    }
    if (SafeGet(Guest(0x5556678), &gp) && gp && SafeGet(gp + 0x16f8, &slotp) && slotp) {
        SafeGet(slotp + 0x98, &slot);
        if (slot != -1 && !S().pickups) {
            return 3;
        }
    }
    std::uint8_t may = 0;
    SafeGet(Guest(type <= 13 ? 0x47348a0 + std::uint64_t(type) * 12 + 2 : 0x47348a0 + 0xa8 + 2), &may);
    if (!may) {
        return 4;
    }
    const std::uint32_t h = std::uint32_t(handle);
    if (h == 0xffffffffu) {
        return 5;
    }
    std::int32_t nb = 0, cnt = 0;
    std::uint64_t blocks = 0, data = 0;
    const std::uint32_t b = (h >> 20) & 0xff, i = h & 0xfffff;
    if (!SafeGet(Guest(0x553b168), &tab) || !tab || !SafeGet(tab + 0x20, &nb) || std::int32_t(b) >= nb ||
        !SafeGet(tab + 0x28, &blocks) || !SafeGet(blocks + b * 0x1f8ull + 0xc8, &cnt) || cnt <= std::int32_t(i) ||
        !SafeGet(blocks + b * 0x1f8ull + 0xd0, &data) || !data) {
        return 6;
    }
    return 0;
}
BB_COOP_SYSV void TreasureSpawnEntry(std::uint64_t, std::uint64_t handle, std::uint64_t ev, std::uint64_t pos,
                                     std::uint64_t, std::uint64_t) {
    std::int32_t type = -1;
    float p[3] = {};
    if (!ev || !SafeGet(ev + 0xc, &type) || type != 4 || !pos || !SafeRead(pos, p, sizeof p)) {
        return;
    }
    {
        std::int32_t role = -1, ctype = -1;
        const int g = TreasureGate(handle, &role, &ctype);
        if (g_tgate[g].fetch_add(1) < 2) {
            Log("test: treasure spawn: handle %#x role %d chr type %d -> gate %d (0 = all gates pass)", unsigned(handle),
                role, ctype, g);
        }
    }
    // The treasure's item lot: an int of the MSB event whose 50000000 + lot is an item pickup flag
    // (lot 2410180 -> flag 52410180, as the C3 log shows).
    Treasure t;
    t.pos = {p[0], p[1], p[2]};
    std::int32_t words[48] = {};
    if (SafeRead(ev, words, sizeof words)) {
        for (std::int32_t w : words) {
            if (w >= 1000000 && w < 50000000 &&
                progress::CategoryOf(50000000u + std::uint32_t(w)) == Category::item_lot_picked) {
                t.flag = 50000000u + std::uint32_t(w);
                break;
            }
        }
    }
    std::lock_guard<std::mutex> lk(g_treasure_mu);
    if (g_treasure.size() < 512) {
        g_treasure.push_back(t);
    }
}

// Test fixture diagnostics: MapItemMan record inserts by kind (0x17DC300's tail) and removals.
template <int K>
BB_COOP_SYSV void RecordInsertEntry(std::uint64_t man, std::uint64_t a1, std::uint64_t, std::uint64_t, std::uint64_t,
                                    std::uint64_t) {
    g_rec_man.store(man);
    if (K == 2) {
        droproll::FixtureInsert(a1);  // the drop insert: the shared roll's result log
    }
    if (g_rec[K].fetch_add(1) < 2) {
        Log("test: MapItemMan record insert kind %d (MapItemMan %p, role %d)", K, reinterpret_cast<void*>(man),
            LocalSessionRole());
    }
}
template <int K>
BB_COOP_SYSV void RecordClearEntry(std::uint64_t man, std::uint64_t a1, std::uint64_t a2, std::uint64_t, std::uint64_t,
                                   std::uint64_t) {
    Log("test: MapItemMan treasure clear %s (MapItemMan %p, args %#llx %#llx, role %d)",
        K == 0 ? "all 0x17D57C0" : "0x17D5B40 (net handler 0x17D2F30)", reinterpret_cast<void*>(man),
        static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2), LocalSessionRole());
}
BB_COOP_SYSV void RecordRemoveEntry(std::uint64_t, std::uint64_t handle_ptr, std::uint64_t set_flag, std::uint64_t,
                                    std::uint64_t, std::uint64_t) {
    std::int32_t h = -1;
    SafeGet(handle_ptr, &h);
    if (g_rec[0].fetch_add(1) < 4) {
        Log("test: MapItemMan record remove: handle %#x, set flag %d (role %d)", unsigned(h), int(set_flag & 0xff),
            LocalSessionRole());
    }
}

BB_COOP_SYSV void PickupEntry(std::uint64_t, std::uint64_t handle_ptr, std::uint64_t, std::uint64_t, std::uint64_t,
                              std::uint64_t) {
    std::int32_t h = -1;
    SafeGet(handle_ptr, &h);
    g_pickups.fetch_add(1);
    g_pickup_at.store(Now());
    Log("map item pickup on this game (role %d): item handle %#x", LocalSessionRole(), unsigned(h));
}

BB_COOP_SYSV void NetKickEntry(std::uint64_t man, std::uint64_t idx, std::uint64_t chr, std::uint64_t state,
                               std::uint64_t, std::uint64_t) {
    g_kicks_net.fetch_add(1);
    LogKick("another player", man, idx, chr, state);
    std::int32_t n = 0, ev = -1;
    std::uint64_t arr = 0;
    if ((chr & 0xffffffffu) != 0xffffffffu && SafeGet(man + 0x98, &n) && SafeGet(man + 0x90, &arr) && arr && std::int32_t(idx) >= 0 &&
        std::int32_t(idx) < n && SafeGet(arr + (idx & 0xffffffffu) * 0x140 + 0xc, &ev)) {
        std::lock_guard<std::mutex> lk(g_kick_mu);
        if (g_kicks_seen.size() >= 32) {
            g_kicks_seen.erase(g_kicks_seen.begin());
        }
        g_kicks_seen.emplace_back(ev, Now());
    }
}

} // namespace

void FixtureRearm() {
    // The objects and the treasure pickups once more (treasures tried before stay skipped), "after"
    // counted from now: the same checks again after an in-place revive (guest_fixture step).
    Fixture& f = g_fx;
    if (!f.parsed || (f.objs.empty() && f.chrs.empty() && f.pickups0 <= 0)) {
        Log("test: fixture: re-arm: no fixture");
        return;
    }
    f.done = false;
    f.idx = 0;
    f.tries = 0;
    f.presses = 0;
    f.pickups = f.pickups0;
    f.next = Now() + f.after;
    Log("test: fixture: re-armed: %zu object(s), %d pickup(s) in %.0f s", f.objs.size(), f.pickups, f.after);
}

bool StandAtObject(std::int32_t entity, float dist, float angle_deg, std::string* what, float dy) {
    std::uint32_t idx = 0xffffffffu;
    using Find = std::uint64_t(BB_COOP_SYSV*)(std::int32_t, std::uint32_t*);
    const std::uint64_t fn = Guest(0x13c9af0);
    const std::uint64_t rec = fn ? reinterpret_cast<Find>(fn)(entity, &idx) : 0;
    float m[16] = {};
    if (!rec || !SafeRead(rec + 0x230, m, sizeof m)) {
        return false;
    }
    // 3x4 rows at +0x240 (m[4..15]): translation in column 3, the local z axis in column 2.
    const float t[3] = {m[7], m[11] + dy, m[15]};
    return PutLocal(t, m[6], m[14], dist, angle_deg, what);
}

bool ObjectPoint(std::int32_t entity, float dist, float angle_deg, float out[3]) {
    std::uint32_t idx = 0xffffffffu;
    using Find = std::uint64_t(BB_COOP_SYSV*)(std::int32_t, std::uint32_t*);
    const std::uint64_t fn = Guest(0x13c9af0);
    const std::uint64_t rec = fn ? reinterpret_cast<Find>(fn)(entity, &idx) : 0;
    float m[16] = {};
    if (!rec || !SafeRead(rec + 0x230, m, sizeof m)) {
        return false;
    }
    // The same point StandAtObject puts the player on (PutLocal's geometry), without the warp.
    float px = m[6], pz = m[14];
    float h = std::sqrt(px * px + pz * pz);
    if (h < 1e-3f) {
        px = 0, pz = 1, h = 1;
    }
    px /= h, pz /= h;
    const float a = angle_deg * 3.14159265f / 180.0f;
    out[0] = m[7] + (px * std::cos(a) + pz * std::sin(a)) * dist;
    out[1] = m[11];
    out[2] = m[15] + (-px * std::sin(a) + pz * std::cos(a)) * dist;
    return true;
}

bool ReadObjAnim(std::int32_t entity, ObjAnimState* out) {
    std::uint32_t idx = 0xffffffffu;
    using Find = std::uint64_t(BB_COOP_SYSV*)(std::int32_t, std::uint32_t*);
    const std::uint64_t fn = Guest(0x13c9af0);
    if (!fn || !reinterpret_cast<Find>(fn)(entity, &idx) || idx == 0xffffffffu) {
        return false;
    }
    std::uint64_t tab = 0, blocks = 0, data = 0;
    std::int32_t nb = 0, cnt = 0;
    const std::uint32_t b = (idx >> 20) & 0xff, i = idx & 0xfffff;
    if (!SafeGet(Guest(0x553b168), &tab) || !tab || !SafeGet(tab + 0x20, &nb) || std::int32_t(b) >= nb ||
        !SafeGet(tab + 0x28, &blocks) || !SafeGet(blocks + b * 0x1f8ull + 0xc8, &cnt) || cnt <= std::int32_t(i) ||
        !SafeGet(blocks + b * 0x1f8ull + 0xd0, &data) || !data) {
        return false;
    }
    const std::uint64_t e = data + i * 0x2e0ull;
    std::uint8_t f8 = 0;
    out->entry = e;
    out->handle = idx;
    SafeGet(e + 0x1fc, &out->flags);
    SafeGet(e + 0x1fe, &f8);
    SafeGet(e + 0x208, &out->anim);
    SafeGet(e + 0x230, &out->state);
    out->playing = (f8 & 8) != 0;
    return true;
}

bool LiftBusyHere(const Lift& l, float radius) {
    ObjAnimState car;
    float at[3], me[4], rot[4];
    if (!ReadObjAnim(l.car, &car)) {
        return false;
    }
    if (car.playing) {
        return true;
    }
    if (!LocalChrPose(me, rot)) {
        return false;
    }
    // liftfix: the library lift's second cage (Lift::gate_rides) carries riders too
    for (const std::int32_t e : {l.car, l.gate_rides ? l.gate : 0}) {
        if (e && ObjectPoint(e, 0, 0, at) && std::hypot(me[0] - at[0], me[2] - at[2]) < radius) {
            return true;
        }
    }
    return false;
}

// chalicelamp: the Chalice Dungeon lift cars this game has seen ride (liftrules::ChaliceLiftCar, from
// the EMEVD dispatch thread); cleared at every load (another layout).
std::mutex g_chalice_cars_mu;
std::vector<std::int32_t> g_chalice_cars;
void NoteChaliceCar(std::int32_t car) {
    std::lock_guard<std::mutex> lk(g_chalice_cars_mu);
    if (std::find(g_chalice_cars.begin(), g_chalice_cars.end(), car) != g_chalice_cars.end() ||
        g_chalice_cars.size() >= 32) {
        return;
    }
    g_chalice_cars.push_back(car);
    Log("lift: Chalice Dungeon lift car %d seen riding (the on-a-moving-car test and the rider guard know it now)", car);
}
std::vector<std::int32_t> ChaliceCars() {
    std::lock_guard<std::mutex> lk(g_chalice_cars_mu);
    return g_chalice_cars;
}
// A learned dungeon car playing a ride within kLiftCarRadius (horizontally) of `p`: its height in *car_y.
bool ChaliceCarNear(std::int32_t car, const float p[3], float band, float* car_y) {
    ObjAnimState st;
    float at[3];
    if (!ReadObjAnim(car, &st) || !st.playing || !ObjectPoint(car, 0, 0, at) ||
        std::hypot(p[0] - at[0], p[2] - at[2]) >= kLiftCarRadius || (band > 0 && std::fabs(p[1] - at[1]) > band)) {
        return false;
    }
    *car_y = at[1];
    return true;
}

bool OnMovingLiftCar(const float p[3]) {
    for (const std::int32_t c : ChaliceCars()) {
        float y = 0;
        if (ChaliceCarNear(c, p, liftrules::kChaliceCarBand, &y)) {
            return true;
        }
    }
    for (const Lift& l : Lifts()) {
        ObjAnimState car;
        float at[3];
        if (!ReadObjAnim(l.car, &car) || !car.playing) {
            continue;
        }
        for (const std::int32_t e : {l.car, l.gate_rides ? l.gate : 0}) {
            if (e && ObjectPoint(e, 0, 0, at) && std::hypot(p[0] - at[0], p[2] - at[2]) < kLiftCarRadius) {
                return true;
            }
        }
    }
    return false;
}

// liftcar: a lift car's live height. The map entry's matrix keeps the car's placement, not the ride;
// the car's hit object (entry+0x288, its height at +0xac) moves with it (headless Frontier rides at 30
// and 60 FPS: 0.27 m under the rider all the way, the placement's height at rest). NaN: not readable
// or not within a lift's travel of the placement (`placed_y`).
float LiftCarHeight(std::uint64_t entry, float placed_y) {
    std::uint64_t hit = 0;
    float y = 0;
    if (!entry || !SafeGet(entry + 0x288, &hit) || hit < 0x100000000ull || hit >= 0x7ff000000000ull || (hit & 7) ||
        !SafeGet(hit + 0xac, &y) || !std::isfinite(y) || std::fabs(y - placed_y) > 80.0f) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return y;
}

// liftfix: the rider guard (liftrules::RiderTick): every frame while a lift car within reach plays a
// ride, the local chr's ride bit and position; a drop through the car puts it back on the car.
void LiftRiderTick(const GameSnapshot& s) {
    static liftrules::RiderTrack track;
    static std::map<std::int32_t, liftrules::CarRider> car_riders;  // chalicelamp: per learned dungeon car
    static int logged = 0;
    if (!s.world_up || s.loading || !s.player) {
        track = liftrules::RiderTrack{};
        car_riders.clear();
        if (s.loading) {
            std::lock_guard<std::mutex> lk(g_chalice_cars_mu);
            g_chalice_cars.clear();
        }
        return;
    }
    float me[4], rot[4];
    if (!LocalChrPose(me, rot)) {
        return;
    }
    for (const std::int32_t c : ChaliceCars()) {
        float car_y = 0, put[3];
        const bool near = ChaliceCarNear(c, me, 0.0f, &car_y);
        liftrules::CarRider& r = car_riders[c];
        if (liftrules::CarRiderTick(r, near, car_y, me[0], me[1], me[2], put) != liftrules::RiderStep::through) {
            continue;
        }
        alignas(16) float to[4] = {put[0], put[1], put[2], 1.0f};
        alignas(16) float r16[4] = {rot[0], rot[1], rot[2], rot[3]};
        const bool ok = b2::LocalChrWarp(s.player, to, r16, "a drop through a Chalice Dungeon lift's car: back on the car");
        if (logged++ < 32) {
            Log("lift ride: the local chr dropped through Chalice Dungeon car %d at (%.1f, %.1f, %.1f) (%.1f m under its "
                "floor); %s (%.1f, %.1f, %.1f)", c, double(me[0]), double(me[1]), double(me[2]),
                double(put[1] - 0.3f - me[1]), ok ? "put back on the car at" : "NOT put back to", double(put[0]),
                double(put[1]), double(put[2]));
        }
        return;
    }
    bool playing = false;
    std::int32_t anim = -1;  // lifts2: the playing car's animation (its arrival / turn is another segment)
    float car_y = std::numeric_limits<float>::quiet_NaN();  // liftcar: the playing car's live height
    for (const Lift& l : Lifts()) {
        ObjAnimState car;
        float at[3];
        if (!ReadObjAnim(l.car, &car) || !car.playing) {
            continue;
        }
        for (const std::int32_t e : {l.car, l.gate_rides ? l.gate : 0}) {
            if (e && ObjectPoint(e, 0, 0, at) && std::hypot(me[0] - at[0], me[2] - at[2]) < kLiftCarRadius) {
                if (!playing) anim = l.car * 100 + car.anim, car_y = LiftCarHeight(car.entry, at[1]);
                playing = true;
            }
        }
    }
    if (playing) g_lift_ride_seen.store(Now());
    std::uint64_t f = 0;
    if (!playing && !track.riding) {
        // NW-666485f (Mensis, frame generation): riders died with no `lift ride:` line. While the
        // local chr rides an object and no table car plays within reach, say once per 10 s which
        // car is nearest and whether it reads as playing (car not found / not playing / too far).
        static double said = -100;
        static int diag = 0;
        const double now = Now();
        if (diag < 16 && now - said >= 10.0 && SafeRead(s.player + 0x1e1, &f, 5) && deathrules::RidingObject(f)) {
            said = now;
            std::int32_t near_car = 0;
            float near_d = 1e9f, near_y = 0;
            for (const Lift& l : Lifts()) {
                float at[3];
                const float d = ObjectPoint(l.car, 0, 0, at) ? std::hypot(me[0] - at[0], me[2] - at[2]) : 1e9f;
                if (d < near_d) near_car = l.car, near_d = d, near_y = at[1];
            }
            if (near_d < 40.0f) {
                ObjAnimState car;
                const bool read = ReadObjAnim(near_car, &car);
                ++diag;
                Log("lift ride: riding an object at (%.1f, %.1f, %.1f) with no table car playing within %.0f m: nearest car "
                    "%d %.1f m away (y %.1f), %s", double(me[0]), double(me[1]), double(me[2]), double(kLiftCarRadius),
                    near_car, double(near_d), double(near_y),
                    !read ? "its animation unreadable" : car.playing ? "playing (outside the radius)" : "not playing");
            }
        }
        return;
    }
    SafeRead(s.player + 0x1e1, &f, 5);
    bool ride_bit = deathrules::RidingObject(f);
    {
        // Test aid (lifts2, BB_TEST_LIFT_RIDEBIT_OFF=START:LEN): the local chr's ride bit reads off from
        // START s for LEN s into every stretch of a car playing within reach (the report's rider whose
        // bit went off on the moving car; the guard's put-backs then show live).
        static const char* env = std::getenv("BB_TEST_LIFT_RIDEBIT_OFF");
        static double play_from = -1;
        if (!playing) {
            play_from = -1;
        } else if (env && env[0]) {
            const double now = Now();
            if (play_from < 0) play_from = now;
            float a = 0, b = 0;
            if (std::sscanf(env, "%f:%f", &a, &b) == 2 && now - play_from >= a && now - play_from < a + b) {
                static double logged_from = -2;
                if (logged_from != play_from) {
                    logged_from = play_from;
                    Log("lift ride: test: the ride bit reads off for %.1f s (BB_TEST_LIFT_RIDEBIT_OFF); chr y %.2f, track %s vy %.2f",
                        double(b), double(me[1]), track.riding ? "riding" : "idle", double(track.vy));
                }
                ride_bit = false;
            }
        }
    }
    float put[3];
    const liftrules::RiderStep step =
        liftrules::RiderTick(track, playing, ride_bit, Now(), me[0], me[1], me[2], put, anim, car_y);
    {
        // Test aid (lifts077, BB_TEST_LIFT_TRACE=1): a line per 0.1 s while a car plays within reach.
        static const bool trace = std::getenv("BB_TEST_LIFT_TRACE") != nullptr;
        static double last_t = 0;
        if (trace && playing && Now() - last_t >= 0.1) {
            last_t = Now();
            Log("lift trace: chr y %.2f car_y %.2f off %.2f bit %d anim %d step %d puts %d", double(me[1]), double(car_y),
                double(track.off), int(ride_bit), int(anim), int(step), track.puts);
        }
    }
    if (step != liftrules::RiderStep::through) {
        return;
    }
    alignas(16) float to[4] = {put[0], put[1], put[2], 1.0f};
    alignas(16) float r16[4] = {rot[0], rot[1], rot[2], rot[3]};
    const bool ok = b2::LocalChrWarp(s.player, to, r16, "a drop through a lift's car: back on the car");
    if (logged++ < 32) {
        Log("lift ride: the local chr dropped through the moving car at (%.1f, %.1f, %.1f) (ride bit off, %.1f m under "
            "the car); %s (%.1f, %.1f, %.1f)", double(me[0]), double(me[1]), double(me[2]), double(put[1] - 0.3f - me[1]),
            ok ? "put back on the car at" : "NOT put back to", double(put[0]), double(put[1]), double(put[2]));
    }
}

// liftre077: a lift car's animation requested while the car is not built yet (docs/party/world_actions.md 10.1.2).
// A map object is built (model, hit object entry+0x288, animation controller entry+0x290) some 2-4 s AFTER a
// player comes near it (flag 0x40 of entry+0x1FC set at once, the build by the object's state machine 0x15A30F0
// -> 0x15A2A00 as its resources arrive). PlayAnim (0x15A25C0) on an object without a controller plays nothing
// and does not mark it playing, so the lift event's wait for the animation's end (2003[18], check 0x13CC180:
// entry+0x208 == the animation and entry+0x1FE bit 3) passes at once: the up event runs its three animations
// (31, 32, 7) in 40 ms, the car shows the end pose when the build finishes (the build replays the last
// requested animation, 0x15A1710) and the rider, who never left, is snapped up by the rider guard. Far from every
// player the same fast-forward is right (nobody sees the car). So for a lift car that is wanted here (flag 0x40)
// but not built: mark it playing (entry+0x1FE bit 3) until the build replays the request, which then plays for real.
struct CarHold {
    std::uint64_t entry;
    std::int32_t anim;
    double since;
    std::int32_t car = 0;
};
std::mutex g_hold_mu;
std::vector<CarHold> g_holds;
void* g_playanim_orig = nullptr;
std::atomic<int> g_hold_made{0}, g_hold_done{0}, g_hold_timeout{0};

bool LiftCarEntity(std::uint64_t entry, std::int32_t* car) {
    for (const Lift& l : Lifts()) {
        for (const std::int32_t e : {l.car, l.gate_rides ? l.gate : 0}) {
            ObjAnimState a;
            if (e && ReadObjAnim(e, &a) && a.entry == entry) {
                *car = e;
                return true;
            }
        }
    }
    return false;
}

BB_COOP_SYSV void PlayAnimHook(std::uint64_t entry, std::int32_t anim) {
    reinterpret_cast<void(BB_COOP_SYSV*)(std::uint64_t, std::int32_t)>(g_playanim_orig)(entry, anim);
    if (!entry || anim < 0) return;
    std::uint64_t ctrl = 0;
    std::uint32_t fl = 0;
    if (!SafeGet(entry + 0x290, &ctrl)) return;
    if (ctrl) {
        std::lock_guard<std::mutex> lk(g_hold_mu);
        for (std::size_t i = 0; i < g_holds.size(); ++i) {
            if (g_holds[i].entry == entry) {
                Log("lift car hold: entry %#llx built after %.2f s; animation %d plays for real now (held %d)",
                    static_cast<unsigned long long>(entry), Now() - g_holds[i].since, int(anim), g_holds[i].anim);
                g_holds.erase(g_holds.begin() + static_cast<std::ptrdiff_t>(i));
                g_hold_done.fetch_add(1);
                break;
            }
        }
        return;
    }
    static const bool off = [] {
        const char* e = std::getenv("BB_PARTY_LIFT_HOLD");
        return e && e[0] == '0';
    }();
    SafeGet(entry + 0x1fc, &fl);
    std::int32_t car = 0;
    // not wanted here: nobody sees this car, the wait passes as in the game
    if (!liftrules::HoldCarAnim(!off, (fl & 0x40) != 0 && LiftCarEntity(entry, &car), false, (fl & 0x40) != 0)) return;
    std::uint8_t f8 = 0;
    if (!SafeGet(entry + 0x1fe, &f8)) return;
    f8 |= 8;
    SafeWrite(entry + 0x1fe, &f8, 1);
    std::lock_guard<std::mutex> lk(g_hold_mu);
    bool known = false;
    for (CarHold& h : g_holds) {
        if (h.entry == entry) h.anim = anim, known = true;
    }
    if (!known) {
        g_holds.push_back({entry, anim, Now(), car});
        g_hold_made.fetch_add(1);
        Log("lift car hold: car %d (entry %#llx) is not built yet (flags %#x): animation %d is held as playing until it is",
            car, static_cast<unsigned long long>(entry), fl, int(anim));
    }
}

// liftre077 test aid (BB_TEST_LIFT_PROBE): the lift event's wait check (0x13CC180) as it answers for a lift car.
void* g_waitchk_orig = nullptr;
BB_COOP_SYSV std::uint64_t WaitCheckHook(std::uint64_t entity, std::uint64_t anim) {
    std::uint64_t r = reinterpret_cast<std::uint64_t(BB_COOP_SYSV*)(std::uint64_t, std::uint64_t)>(g_waitchk_orig)(entity, anim);
    // liftre077: the game clears the unbuilt car's playing bit again within a step (live: the check read 1 once,
    // then 0, and the three animations ran in 40 ms), so the wait answers "playing" itself while the hold stands.
    if (!(r & 0xff)) {
        std::lock_guard<std::mutex> lk(g_hold_mu);
        for (const CarHold& h : g_holds) {
            if (h.car == static_cast<std::int32_t>(entity) && h.anim == static_cast<std::int32_t>(anim)) {
                r = 1;
                break;
            }
        }
    }
    static const bool on = std::getenv("BB_TEST_LIFT_PROBE") != nullptr;
    static std::atomic<std::uint64_t> last{~0ull};
    if (on && liftrules::LiftByCar(static_cast<std::int32_t>(entity))) {
        const std::uint64_t key = (entity << 20) ^ (anim << 4) ^ (r & 0xff);
        if (last.exchange(key) != key) {
            ObjAnimState a;
            ReadObjAnim(static_cast<std::int32_t>(entity), &a);
            Log("lift wait check: entity %d anim %d -> %d (obj anim %d playing %d flags %#x) step %llu", int(entity), int(anim),
                int(r & 0xff), a.anim, int(a.playing), a.flags, (unsigned long long)BbEngine::FrameRate::Steps());
        }
    }
    return r;
}

// Main thread, every tick: a held car that never got built lets go.
void LiftHoldTick() {
    std::lock_guard<std::mutex> lk(g_hold_mu);
    for (std::size_t i = 0; i < g_holds.size();) {
        std::uint64_t ctrl = 0;
        CarHold& h = g_holds[i];
        if (SafeGet(h.entry + 0x290, &ctrl) && !ctrl && liftrules::CarHoldExpired(Now() - h.since)) {
            std::uint8_t f8 = 0;
            if (SafeGet(h.entry + 0x1fe, &f8)) {
                f8 &= static_cast<std::uint8_t>(~8);
                SafeWrite(h.entry + 0x1fe, &f8, 1);
            }
            Log("lift car hold: entry %#llx was not built in %.0f s; animation %d released", static_cast<unsigned long long>(h.entry),
                liftrules::kCarHoldMax, int(h.anim));
            g_hold_timeout.fetch_add(1);
            g_holds.erase(g_holds.begin() + static_cast<std::ptrdiff_t>(i));
        } else if (ctrl) {
            g_holds.erase(g_holds.begin() + static_cast<std::ptrdiff_t>(i));  // built (the replay's hook logs)
        } else {
            ++i;
        }
    }
}

// liftre077: lift cars within kPrebuildRadius of the local chr stay fully built. The game keeps an object at a level
// by the grid cells the loaded area covers (0x15B2B20 -> SetLevel 0x15A4810: 2 builds it fully, 1 model only, 3 drops
// it); a car is built only when a player is already close, 2-4 s before the floor and the animation exist. A player
// placed by an in-world warp (gather, party travel) or running up to a lift reaches it first. Cars within 150 m of the
// local chr are asked for the full level, so they are built long before anyone stands on them.
constexpr float kPrebuildRadius = 150.0f;
std::atomic<std::uint64_t> g_pre_entry[48];
std::atomic<bool> g_pre_near[48];
std::atomic<int> g_pre_n{0};
void* g_setlevel_orig = nullptr;
std::atomic<int> g_pre_forced{0};
BB_COOP_SYSV std::uint64_t SetLevelHook(std::uint64_t entry, std::int32_t level) {
    if (level != 2) {
        const int n = g_pre_n.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) {
            if (g_pre_entry[i].load(std::memory_order_relaxed) == entry) {
                if (g_pre_near[i].load(std::memory_order_relaxed)) {
                    if (g_pre_forced.fetch_add(1) < 6) {
                        Log("lift car prebuild: car entry %#llx asked for level %d, kept fully built (local chr within %.0f m)",
                            static_cast<unsigned long long>(entry), int(level), double(kPrebuildRadius));
                    }
                    level = 2;
                }
                break;
            }
        }
    }
    return reinterpret_cast<std::uint64_t(BB_COOP_SYSV*)(std::uint64_t, std::int32_t)>(g_setlevel_orig)(entry, level);
}
void LiftPrebuildTick(const GameSnapshot& s) {
    static const bool off = [] {
        const char* e = std::getenv("BB_PARTY_LIFT_PREBUILD");
        return e && e[0] == '0';
    }();
    float me[4], rot[4];
    if (!s.world_up || s.loading) {
        g_pre_n.store(0, std::memory_order_relaxed);  // no object table yet / being rebuilt
        return;
    }
    const bool ok = !off && s.player && LocalChrPose(me, rot);
    int n = 0;
    for (const Lift& l : Lifts()) {
        for (const std::int32_t e : {l.car, l.gate_rides ? l.gate : 0}) {
            ObjAnimState a;
            float at[3];
            if (!e || n >= 48 || !ReadObjAnim(e, &a) || !a.entry) continue;
            g_pre_entry[n].store(a.entry, std::memory_order_relaxed);
            const bool near = ok && ObjectPoint(e, 0, 0, at) && std::hypot(me[0] - at[0], me[2] - at[2]) < kPrebuildRadius;
            g_pre_near[n].store(near, std::memory_order_relaxed);
            ++n;
            // a car nobody asked for yet (level 0): ask for the full build now, the way the cell pass does
            std::uint8_t rq = 0;
            static double last_ask[48];
            if (near && g_setlevel_orig && SafeGet(a.entry + 0x1f9, &rq) && rq != 4 && Now() - last_ask[n - 1] > 1.0) {
                last_ask[n - 1] = Now();
                reinterpret_cast<std::uint64_t(BB_COOP_SYSV*)(std::uint64_t, std::int32_t)>(g_setlevel_orig)(a.entry, 2);
                static int logged = 0;
                if (logged++ < 8) {
                    Log("lift car prebuild: car %d (entry %#llx) was at level %u: asked to build fully (local chr %.0f m away)", e,
                        static_cast<unsigned long long>(a.entry), unsigned(rq),
                        double(std::hypot(me[0] - at[0], me[2] - at[2])));
                }
            }
        }
    }
    g_pre_n.store(n, std::memory_order_relaxed);
}

// liftre077: a lift car that is wanted here but not built has no floor either (hit object entry+0x288 absent): a
// player who steps into its footprint falls down the shaft (live, lf at 60 FPS with the ride 1.2 s after the
// arrival: the guest dropped 25 m through the unbuilt car and died). While the local chr is near such a car and
// starts to fall, it is put back on the last spot it stood on and held there until the car is built (or 10 s).
void LiftBuildGuardTick(const GameSnapshot& s) {
    static struct {
        bool have = false, pinned = false;
        float good[3] = {}, rot[4] = {}, last_y = 0;
        double t = 0, pin_since = 0, grace = 0;
    } g;
    static const bool off = [] {
        const char* e = std::getenv("BB_PARTY_LIFT_HOLD");
        return e && e[0] == '0';
    }();
    if (off || !s.world_up || s.loading || !s.player) {
        g.have = g.pinned = false;
        return;
    }
    float me[4], rot[4];
    if (!LocalChrPose(me, rot)) return;
    bool unbuilt_near = false;
    for (const Lift& l : Lifts()) {
        for (const std::int32_t e : {l.car, l.gate_rides ? l.gate : 0}) {
            ObjAnimState a;
            float at[3];
            std::uint64_t ctrl = 1;
            if (!e || !ReadObjAnim(e, &a) || !a.entry || !(a.flags & 0x40) || !SafeGet(a.entry + 0x290, &ctrl) || ctrl ||
                !ObjectPoint(e, 0, 0, at)) {
                continue;
            }
            if (std::hypot(me[0] - at[0], me[2] - at[2]) < kLiftCarRadius + 2.0f) unbuilt_near = true;
        }
    }
    const double now = Now();
    if (unbuilt_near) g.grace = now + 2.5;  // the car's floor enters the world a moment after the build
    if (!unbuilt_near && now > g.grace) {
        g.have = g.pinned = false;
        return;
    }
    const double dt = now - g.t;
    const float vy = g.have && dt > 0.0 ? float((me[1] - g.last_y) / dt) : 0.0f;
    g.t = now, g.last_y = me[1];
    if (g.pinned && !unbuilt_near) g.pinned = false;  // built: no more holding, a fall is still put back
    if (g.pinned && now - g.pin_since > 10.0) {
        g.pinned = false;
        Log("lift car floor: the car near the local chr was not built in 10 s; released");
        return;
    }
    if (!g.pinned && (!g.have || vy > -3.0f)) {
        g.have = true;
        std::memcpy(g.good, me, sizeof g.good);
        std::memcpy(g.rot, rot, sizeof g.rot);
        return;
    }
    if (!g.pinned) {
        g.pinned = unbuilt_near;
        g.pin_since = now;
        Log("lift car floor: the local chr falls (%.1f m/s) at (%.1f, %.1f, %.1f) next to a lift car that is not built yet: held at "
            "(%.1f, %.1f, %.1f) until it is", double(vy), double(me[0]), double(me[1]), double(me[2]), double(g.good[0]),
            double(g.good[1]), double(g.good[2]));
    }
    alignas(16) float to[4] = {g.good[0], g.good[1], g.good[2], 1.0f};
    alignas(16) float r16[4] = {g.rot[0], g.rot[1], g.rot[2], g.rot[3]};
    b2::LocalChrWarp(s.player, to, r16, "next to a lift car that is not built yet: held on the spot it stood on");
}

// liftre077 test aid (BB_TEST_LIFT_PROBE=1 or 2): the lift cars' object state and the local chr's carry state per
// game step. 1: a line when a car's state changes (the ints below) and every 25th step while a car plays; 2: a line
// every step while a car within reach plays or the chr rides (the hit record's floats too).
void LiftProbeTick(const GameSnapshot& s) {
    static const int mode = [] {
        const char* e = std::getenv("BB_TEST_LIFT_PROBE");
        return e && *e ? std::atoi(e) : 0;
    }();
    if (!mode || !s.world_up || s.loading || !s.player) return;
    static std::map<std::int32_t, std::string> last;
    const std::uint64_t step = BbEngine::FrameRate::Steps();
    float me[4], rot[4];
    const bool have_me = LocalChrPose(me, rot);
    std::uint64_t chr_hit = 0, chr_flags = 0;
    SafeGet(s.player + 0x288, &chr_hit);
    SafeRead(s.player + 0x1e1, &chr_flags, 5);
    float chr_dt = 0;
    SafeGet(s.player + 0xe0, &chr_dt);
    {
        // BB_TEST_LIFT_WATCH=1: hardware write breakpoints (party_lift_watch.cpp) on the chr's position y, standing
        // hit, flag dword and the nearest car's hit height while a car is within 25 m.
        static const bool watch = std::getenv("BB_TEST_LIFT_WATCH") != nullptr;
        static const bool entry_mode = watch && std::atoi(std::getenv("BB_TEST_LIFT_WATCH")) == 2;
        std::uint64_t car_entry = 0;
        static int logged = 0;
        if (watch) {
            liftwatch::SetStep(step);
            std::uint64_t phys = 0, hitp = 0;
            std::uint64_t mod = 0;
            if (SafeGet(s.player + 0x3b0, &mod) && mod && SafeGet(mod + 0x68, &phys) && phys && have_me) {
                for (const Lift& l : Lifts()) {
                    ObjAnimState c2;
                    float at[3];
                    if (ReadObjAnim(l.car, &c2) && c2.entry && ObjectPoint(l.car, 0, 0, at) &&
                        std::hypot(me[0] - at[0], me[2] - at[2]) < 25.0f) {
                        SafeGet(c2.entry + 0x288, &hitp);
                        car_entry = c2.entry;
                        break;
                    }
                }
                if (entry_mode && car_entry) {
                    // BB_TEST_LIFT_WATCH=2: the car's own build state (state / requested level / flags, resource bits,
                    // hit object, animation controller)
                    const std::uint64_t addr[4] = {car_entry + 0x1f8, car_entry + 0x1c8, car_entry + 0x288, car_entry + 0x290};
                    const int len[4] = {8, 8, 8, 8};
                    liftwatch::Arm(reinterpret_cast<std::uint64_t>(coop::Image()), addr, len);
                } else if (!entry_mode && hitp > 0x100000000ull && hitp < 0x7ff000000000ull) {
                    const std::uint64_t addr[4] = {phys + 0x1e4, s.player + 0x288, s.player + 0x1e4, hitp + 0xac};
                    const int len[4] = {4, 8, 4, 4};
                    liftwatch::Arm(reinterpret_cast<std::uint64_t>(coop::Image()), addr, len);
                }
            }
            liftwatch::Hit h;
            while (liftwatch::Pop(&h)) {
                if (logged++ > 6000) break;
                static const char* names[4] = {"chr_y", "chr_hit", "chr_flags", "car_hit_y"};
                static const char* enames[4] = {"car_1f8", "car_1c8", "car_hit", "car_ctrl"};
                char v[32];
                if (entry_mode) {
                    std::snprintf(v, sizeof v, "%#llx", static_cast<unsigned long long>(h.value));
                } else if (h.which == 0 || h.which == 3) {
                    float f;
                    std::memcpy(&f, &h.value, 4);
                    std::snprintf(v, sizeof v, "%.4f", double(f));
                } else {
                    std::snprintf(v, sizeof v, "%#llx", static_cast<unsigned long long>(h.value));
                }
                Log("lift watch: step %llu tid %u %s <- %s at +%llx ret %llx %llx %llx %llx rsp0 %llx", (unsigned long long)h.step,
                    h.tid, entry_mode ? enames[h.which & 3] : names[h.which & 3], v, (unsigned long long)h.rip, (unsigned long long)h.ret[0],
                    (unsigned long long)h.ret[1], (unsigned long long)h.ret[2], (unsigned long long)h.ret[3],
                    (unsigned long long)h.rsp0);
            }
        }
    }
    for (const Lift& l : Lifts()) {
        ObjAnimState car;
        if (!ReadObjAnim(l.car, &car) || !car.entry) continue;
        const std::uint64_t e = car.entry;
        std::uint8_t st = 0, rq = 0, f8 = 0, c8 = 0;
        std::uint32_t c0 = 0;
        std::uint64_t m278 = 0, hit = 0, ctrl = 0, mdl = 0, p1d0 = 0;
        std::int32_t h = -1;
        SafeGet(e + 0x1f8, &st); SafeGet(e + 0x1f9, &rq); SafeGet(e + 0x1fe, &f8); SafeGet(e + 0x1c8, &c8);
        SafeGet(e + 0x1c0, &c0); SafeGet(e + 0x278, &m278); SafeGet(e + 0x288, &hit); SafeGet(e + 0x290, &ctrl);
        SafeGet(e + 0x238, &mdl); SafeGet(e + 0x1d0, &p1d0); SafeGet(e + 0x20c, &h);
        std::int32_t hs = -1; std::uint8_t hf = 0;
        float hy = std::numeric_limits<float>::quiet_NaN();
        if (hit > 0x100000000ull && hit < 0x7ff000000000ull) {
            SafeGet(hit + 0x6c, &hs); SafeGet(hit + 0xc0, &hf); SafeGet(hit + 0xac, &hy);
        }
        char key[320];
        std::snprintf(key, sizeof key,
                      "st %u rq %u f1fc %#x f1fe %#x c8 %#x c0 %u anim %d play %d h20c %d state230 %d ctrl %d mdl %d "
                      "hit %d hs %d hf %#x",
                      st, rq, car.flags, f8, c8, c0, car.anim, int(car.playing), h, car.state, ctrl != 0, mdl != 0,
                      hit != 0, hs, hf);
        std::string& prev = last[l.car];
        const bool changed = prev != key;
        prev = key;
        bool near = false;
        if (have_me) {
            float at[3];
            near = ObjectPoint(l.car, 0, 0, at) && std::hypot(me[0] - at[0], me[2] - at[2]) < 25.0f;
        }
        const bool active = near && (car.playing || (chr_flags & 0x200000000ull));
        // The standing contact the way the game's mover test (0x18CB0E0) reads it: the chr's physics module (+0xa8 the
        // contact object, +0xa0 its node) and that node's motion vectors (|+0x40|^2, |+0x50|^2 against 0.0005).
        char mv[160] = "";
        if (have_me && (active || changed)) {
            std::uint64_t mod = 0, phys = 0, contact = 0, model = 0, nmap = 0, mgr = 0, mgr2 = 0, arrA = 0, arrB = 0;
            std::int32_t node = -1, sk = -1, bi = -1;
            if (SafeGet(s.player + 0x3b0, &mod) && mod && SafeGet(mod + 0x68, &phys) && phys && SafeGet(phys + 0xa8, &contact)) {
                SafeGet(phys + 0xa0, &node);
                if (contact == e && SafeGet(e + 0x278, &model) && model && SafeGet(model + 0x28, &nmap) && nmap) {
                    std::uint64_t map2 = 0;
                    std::int32_t cnt = 0;
                    if (SafeGet(nmap + 0x20, &map2) && map2 && SafeGet(nmap + 0x28, &cnt) && node >= 0 && node < cnt &&
                        SafeGet(map2 + 4ull * node, &sk) && SafeGet(Guest(0x553d700), &mgr) && mgr && SafeGet(mgr + 0x28, &mgr2) &&
                        mgr2 && SafeGet(mgr2 + 8, &mgr2) && mgr2 && SafeGet(mgr2 + 0x20, &arrA) && SafeGet(mgr2 + 0xe0, &arrB) &&
                        arrA && arrB && SafeGet(arrA + 0x68 + 0x90ull * sk, &bi)) {
                        float v40[4] = {}, v50[4] = {};
                        SafeRead(arrB + 0x40 + 0x80ull * bi, v40, 16);
                        SafeRead(arrB + 0x50 + 0x80ull * bi, v50, 16);
                        std::snprintf(mv, sizeof mv, "node %d sk %d bi %d |v40|2 %.6f (%.4f %.4f %.4f) |v50|2 %.6f", node, sk, bi,
                                      double(v40[0] * v40[0] + v40[1] * v40[1] + v40[2] * v40[2]), double(v40[0]), double(v40[1]),
                                      double(v40[2]), double(v50[0] * v50[0] + v50[1] * v50[1] + v50[2] * v50[2]));
                    }
                }
            }
            if (!mv[0]) std::snprintf(mv, sizeof mv, "contact %s", contact == e ? "=car (no node vector)" : (contact ? "other" : "none"));
        }
        if (changed || (active && (mode >= 2 || (step % 25) == 0))) {
            float ey = std::numeric_limits<float>::quiet_NaN(), hit_chr_diff = 0;
            float at[3];
            if (ObjectPoint(l.car, 0, 0, at)) ey = at[1];
            hit_chr_diff = hy - (have_me ? me[1] : 0.0f);
            Log("lift probe: step %llu t %.3f dt %.5f chrdt %.5f car %d %s%s | map_y %.2f hit_y %.3f chr_y %.3f (hit-chr %.3f) "
                "chrhit %s f1e1 %010llx | %s",
                (unsigned long long)step, BbEngine::FrameRate::GameSeconds(), double(BbEngine::FrameRate::LastDt()), double(chr_dt), l.car, key,
                changed ? " CHANGED" : "", double(ey), double(hy), have_me ? double(me[1]) : 0.0, double(hit_chr_diff),
                chr_hit == hit ? "=car" : (chr_hit ? "other" : "none"), static_cast<unsigned long long>(chr_flags), mv);
            if (mode >= 2 && active && hit > 0x100000000ull && hit < 0x7ff000000000ull) {
                float f[32] = {};
                SafeRead(hit + 0x30, f, sizeof f);
                Log("lift probe: hit34..b4: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | "
                    "%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
                    double(f[0]), double(f[1]), double(f[2]), double(f[3]), double(f[4]), double(f[5]), double(f[6]),
                    double(f[7]), double(f[8]), double(f[9]), double(f[10]), double(f[11]), double(f[12]), double(f[13]),
                    double(f[14]), double(f[15]), double(f[16]), double(f[17]), double(f[18]), double(f[19]), double(f[20]),
                    double(f[21]), double(f[22]), double(f[23]), double(f[24]), double(f[25]), double(f[26]), double(f[27]),
                    double(f[28]), double(f[29]), double(f[30]), double(f[31]));
            }
        }
    }
}

void LiftReplayTick(const GameSnapshot& s) {
    static std::map<std::uint32_t, bool> last;  // a replaying lift's pos flag -> its value last tick
    static std::set<std::uint32_t> held;        // pos flags the world mirror holds back (liftfix)
    static std::uint32_t map = 0xffffffffu;
    auto hold = [](const Lift& l, bool on, std::int32_t anim) {
        if (on == (held.count(l.pos) != 0)) {
            return;
        }
        progress::SetMirrorHold(l.pos, on);
        if (on) {
            held.insert(l.pos);
            Log("lift %d: this game's own ride (car anim %d): flag %u from the host's world waits for its end", l.car,
                anim, l.pos);
        } else {
            held.erase(l.pos);
            Log("lift %d: this game's ride is over (car anim %d): flag %u follows the host's world again", l.car, anim,
                l.pos);
        }
    };
    if (!s.world_up || s.loading || s.map_id != map) {
        last.clear();  // a load: the block's init event puts each car at its flag's end
        for (const Lift& l : Lifts()) {
            hold(l, false, -1);
        }
        map = (s.world_up && !s.loading) ? s.map_id : 0xffffffffu;
        return;
    }
    const bool guest = progress::FlagLoadMode() == 1;
    for (const Lift& l : Lifts()) {
        bool v = false;
        if (!l.Replays() || l.pos / 100000u % 100u != (s.map_id >> 24) || !progress::ReadFlag(l.pos, &v)) {
            hold(l, false, -1);
            continue;
        }
        ObjAnimState car;
        const bool loaded = ReadObjAnim(l.car, &car);
        // liftfix: a guest's mirror keeps the host's flip of this flag back while this game's copy of a
        // ride has not written its own end (liftrules::MirrorHold); the release lets it in.
        hold(l, guest && loaded && liftrules::MirrorHold(l, car.anim, car.playing), loaded ? car.anim : -1);
        const auto it = last.find(l.pos);
        if (it == last.end() || it->second == v) {
            last[l.pos] = v;
            continue;
        }
        if (!loaded) {
            it->second = v;
            Log("lift %d: flag %u = %d here, the car is not loaded (its init puts it at that end)", l.car, l.pos, v ? 1 : 0);
            continue;
        }
        // This game's copy rides (its car shows a ride animation): it reads the flag once its player
        // has left the car, to pick its end (12601352: "goto if 12601253 on"). The mirror holds the
        // host's flip back meanwhile (above); a flip that came in anyway (a full mirror copy) is put
        // back on a guest, so the copy ends where it rode and writes its value itself; looked at again
        // once the car stands at an end (liftfix: only then - 0.7.1 also held while the player stood
        // near the car, and put back the copy's own end written in the same tick as the host's flip:
        // liftrules::HoldMirrorFlip).
        if (liftrules::HoldMirrorFlip(l, car.anim)) {
            if (progress::FlagLoadMode() == 1 && WriteEventFlag(l.pos, it->second)) {
                Log("lift %d: flag %u = %d from the host's world during this game's own ride (car anim %d): kept at "
                    "%d until its end", l.car, l.pos, v ? 1 : 0, car.anim, it->second ? 1 : 0);
            }
            continue;
        }
        it->second = v;
        if (!LiftNeedsReplay(l, v, car.anim)) {
            Log("lift %d: flag %u = %d by this game's own ride (car anim %d)", l.car, l.pos, v ? 1 : 0, car.anim);
            continue;
        }
        // Another game's ride (the mirror brought the host's to this guest, or the host took a
        // guest's): the car and its gate to the end that game's copy left them at, as that copy's
        // 2003[18] does (0x13CBFD0: entity, animation, loop, -).
        using PlayAnim = void(BB_COOP_SYSV*)(std::int32_t, std::uint32_t, std::uint64_t, std::uint32_t);
        const std::uint64_t fn = Guest(0x13cbfd0);
        if (!fn) {
            continue;
        }
        const std::int32_t gate_anim = v ? l.gate_end_on : l.gate_end_off;
        const std::int32_t car_anim = v ? l.car_end_on : l.car_end_off;
        if (l.gate && gate_anim >= 0) {
            reinterpret_cast<PlayAnim>(fn)(l.gate, std::uint32_t(gate_anim), 0, 0);
        }
        reinterpret_cast<PlayAnim>(fn)(l.car, std::uint32_t(car_anim), 0, 0);
        Log("lift %d: flag %u = %d from another game's ride (car anim %d here): car to that end (anim %d, gate %d anim %d)",
            l.car, l.pos, v ? 1 : 0, car.anim, car_anim, l.gate, gate_anim);
    }
}

bool StandAtChr(std::int32_t entity, float dist, float angle_deg, std::string* what) {
    using Find = std::uint64_t(BB_COOP_SYSV*)(std::int32_t, std::int32_t, std::uint64_t);
    const std::uint64_t fn = Guest(0x13c97a0);
    const std::uint64_t chr = fn ? reinterpret_cast<Find>(fn)(entity, entity, 0) : 0;
    float p[4] = {}, r[4] = {};
    const std::uint64_t c = chr ? chr : GuestSvcFindChr(std::uint32_t(entity));
    if (!c || !SafeRead(c + 0xc0, p, sizeof p) || !SafeRead(c + 0xd0, r, sizeof r)) {
        return false;
    }
    // Where it stands now (its physics position): +0xc0 can lag far behind a chr that walked
    // (D11 nf run 2: Henryk 14 m from his +0xc0 spot, the guest's R1s hit the air).
    float now[3] = {};
    if (balance::ChrPosition(c, now)) {
        p[0] = now[0], p[1] = now[1], p[2] = now[2];
    }
    // The chr's front: forward(yaw) = (-sin, -cos), yaw = +0xd0 [1].
    return PutLocal(p, -std::sin(r[1]), -std::cos(r[1]), dist, angle_deg, what);
}

namespace {
// D6 (pard3): KeyItemGate. The dispatch thread, before the game reads the instruction's arguments:
// each 3[4] of the gate's event gets the held state this game's copy should check (the argument
// bytes are the loaded script's, shared by every run of the event, so every dispatch writes the
// value for the current role: back to the data's own at home).
std::atomic<int> g_keygate_last{-1};
void KeyGateRewriter(int event_id, int bank, int id, std::uint64_t event, std::uint64_t instr) {
    if (bank != 3 || id != 4) {
        return;
    }
    const KeyItemGate* g = KeyItemGateFor(event_id);
    if (!g) {
        return;
    }
    std::uint32_t size = 0;
    const std::uint64_t args = EmevdInstructionArgs(event, instr, &size);
    std::int8_t group = 0;
    std::int32_t goods = 0;
    std::uint8_t state = 0;
    if (!args || size < 9 || !SafeGet(args, &group) || !SafeGet(args + 4, &goods) || !SafeGet(args + 8, &state) ||
        goods != g->goods) {
        return;
    }
    const bool summoned = LocalSessionRole() == 6 && progress::FlagLoadMode() == 1;
    bool own = false, host = false;
    if (summoned) {
        progress::ReadFlagOwnSave(g->own_flag, &own);
        progress::HostWorldFlag(g->own_flag, &host);
    }
    // The local held count: 0 in the game's summoned key view, the real one with the own key
    // section shown (items.md 10.7); -1 (unknown) counts as 0, the old rule.
    const int held = summoned ? GoodsCount(static_cast<std::uint32_t>(g->goods)) : 0;
    const bool invert = KeyItemGateInvert(summoned, own, host, held);
    const std::uint8_t want = KeyItemGateHeldState(*g, group, invert);
    if (want != state) {
        SafeWrite(args + 8, &want, 1);
    }
    const int key = (invert ? 1 : 0) | (own ? 2 : 0) | (host ? 4 : 0) | (held > 0 ? 8 : 0);
    if (g_keygate_last.exchange(key) != key) {
        Log("key item gate: event %d checks goods %d %s (summoned %d, own pickup flag %u %d, host world's %d, "
            "held %d)",
            event_id, goods, invert ? "as held: the guest's own key opens it in the host's world" : "as in the data",
            summoned ? 1 : 0, g->own_flag, own ? 1 : 0, host ? 1 : 0, held);
    }
}

// liftfix: the ride end of Cainhurst's lift and the Nightmare of Mensis' library lift waits for the
// local player alone; in a party the game's sync passes it on every game as soon as the copy of a
// player outside the car passes it, the rider still in the car (liftrules::LocalEndToAllPlayers). On
// its first dispatch the instruction becomes the all-players check every other lift uses (3[7]): the
// loaded script's bytes, kept for every later run of the event until the map unloads; every game of
// the party does the same, so their copies agree.
std::atomic<int> g_lift_end_rewrites{0};
void LiftEndRewriter(int event_id, int bank, int id, std::uint64_t event, std::uint64_t instr) {
    if (bank != 3 || (id != 2 && id != 7) || !liftrules::LocalEndRideEvent(event_id)) {
        return;
    }
    std::uint32_t size = 0;
    const std::uint64_t args = EmevdInstructionArgs(event, instr, &size);
    std::uint8_t a[12] = {};
    if (!args || size < 12 || !SafeRead(args, a, sizeof a) ||
        !liftrules::LocalEndToAllPlayers(event_id, bank, id, a, sizeof a)) {
        return;
    }
    // The id stays in the loaded script; the argument bytes come afresh with every run of the event
    // (live run 3), so they are moved on every dispatch, the id only the first time.
    const std::int32_t all_players = 7;
    if (!SafeWrite(args, a, sizeof a) || (id == 2 && !SafeWrite(instr + 4, &all_players, 4))) {
        Log("lift ride end: event %d: 3[2] -> 3[7] NOT written", event_id);
        return;
    }
    if (id == 7) {
        return;  // a later run of the event: logged once, at the first rewrite
    }
    std::int32_t region = 0;
    std::memcpy(&region, a + 4, 4);
    if (g_lift_end_rewrites.fetch_add(1) < 16) {
        Log("lift ride end: event %d waits until every player is out of region %d (3[2] local player -> 3[7], as "
            "the other lifts)", event_id, region);
    }
}


// liftfix: the Research Hall lifts' busy clear moved to their ride event's end (liftrules::BusyDefer).
std::atomic<int> g_busy_defer_logs{0};
void BusyDeferRewriter(int event_id, int bank, int id, std::uint64_t event, std::uint64_t instr) {
    if ((bank != 2003 || id != 2) && (bank != 1000 || id != 4)) {
        return;
    }
    std::int32_t index = -1;
    SafeGet(event + 0xa0, &index);
    std::uint32_t size = 0;
    const std::uint64_t args = EmevdInstructionArgs(event, instr, &size);
    std::uint32_t flag = 0;
    if (bank == 2003 && (!args || size < 5 || !SafeGet(args, &flag))) {
        return;
    }
    const liftrules::BusyDefer* d = nullptr;
    const liftrules::BusyStep step = liftrules::BusyDeferStep(event_id, bank, id, index, flag, &d);
    if (step == liftrules::BusyStep::keep) {
        const std::uint8_t on = 1;
        SafeWrite(args + 4, &on, 1);  // busy stays on until the end (the loaded script's bytes: every run)
    } else if (step == liftrules::BusyStep::clear && d) {
        WriteEventFlag(d->busy, false);
        if (g_busy_defer_logs.fetch_add(1) < 16) {
            Log("lift ride end: event %d clears busy %u after its end animation (not in the frame its rider left "
                "the car: the car left again at once with the rider on its edge)", event_id, d->busy);
        }
    }
}
// chalicelamp: a Chalice Dungeon lift's ride animates its car (2003[18]): the car is learned.
void ChaliceLiftWatcher(int event_id, int bank, int id, std::uint64_t event, std::uint64_t instr) {
    if (bank != 2003 || id != 18 || !liftrules::ChaliceLiftEvent(event_id)) {
        return;
    }
    std::uint32_t size = 0;
    const std::uint64_t args = EmevdInstructionArgs(event, instr, &size);
    std::int32_t entity = 0;
    if (!args || size < 4 || !SafeGet(args, &entity)) {
        return;
    }
    if (const std::int32_t car = liftrules::ChaliceLiftCar(event_id, bank, id, entity)) {
        NoteChaliceCar(car);
    }
}
void WorldactRewriter(int event_id, int bank, int id, std::uint64_t event, std::uint64_t instr) {
    ChaliceLiftWatcher(event_id, bank, id, event, instr);
    KeyGateRewriter(event_id, bank, id, event, instr);
    LiftEndRewriter(event_id, bank, id, event, instr);
    BusyDeferRewriter(event_id, bank, id, event, instr);
}
} // namespace

namespace {
/// A shared key treasure was given to this member (party_items ApplySummoned): the treasure lying in
/// its game's copy of the host's world is taken out of MapItemMan (kind 3 list at +0x28, node +0x20 handle,
/// +0x28 pickup flag) with the game's own removal 0x17D6780(man, &handle, set_flag = 0): no flag is set, the
/// record and its object go. The spawn of a later load checks the own save's flag (set by the give).
bool HideTreasureByFlag(std::uint32_t flag) {
    std::uint64_t man = 0, node = 0;
    if (!SafeGet(Guest(0x553d6e0), &man) || !man || !SafeGet(man + 0x28, &node)) {
        return false;
    }
    using Remove = void(BB_COOP_SYSV*)(std::uint64_t, std::int32_t*, std::uint8_t);
    for (int guard = 0; node && guard < 4096; ++guard) {
        std::uint32_t f = 0;
        SafeGet(node + 0x28, &f);
        if (f == flag) {
            std::int32_t handle = 0;
            if (!SafeGet(node + 0x20, &handle)) {
                return false;
            }
            reinterpret_cast<Remove>(Guest(0x17d6780))(man, &handle, 0);
            return true;
        }
        if (!SafeGet(node, &node)) {
            break;
        }
    }
    return false;
}
} // namespace

void Init() {
    State& st = S();
    if (!st.enabled) {
        return;
    }
    SetEmevdRewriter3(&WorldactRewriter);
    EnsureEmevdFilter();
    SetTreasureHider(&HideTreasureByFlag);
    // push rbp; mov rbp, rsp; push r15; push r14; push r13; push r12; push rbx (13 bytes, both).
    const bool a = HookPrologue(0x13AE370, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                                &LocalKickEntry, "ObjAct local kick (log)");
    const bool b = HookPrologue(0x13AF830, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                                &NetKickEntry, "ObjAct remote kick (log)");
    Log("ObjAct kick log %s", a && b ? "on" : "partly off");
    // liftre077: PlayAnim (0x15A25C0: push rbp; mov rbp, rsp; push r15; push r14), a lift car asked to play before it is built.
    if (!ReplacePrologue(0x15A25C0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56}, reinterpret_cast<const void*>(&PlayAnimHook),
                         &g_playanim_orig, "lift car animation held until the car is built (0x15A25C0)")) {
        g_playanim_orig = nullptr;
        Log("lift car hold: NOT installed (bytes differ)");
    }
    if (!ReplacePrologue(0x15A4810, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56}, reinterpret_cast<const void*>(&SetLevelHook),
                         &g_setlevel_orig, "lift cars near the local chr stay built (0x15A4810)")) {
        g_setlevel_orig = nullptr;
        Log("lift car prebuild: NOT installed (bytes differ)");
    }
    if (!ReplacePrologue(0x13CC180, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56}, reinterpret_cast<const void*>(&WaitCheckHook),
                         &g_waitchk_orig, "lift car animation wait held while the car is built (0x13CC180)")) {
        g_waitchk_orig = nullptr;
    }
    // Enemy drop rolls and map item pickups: logged (the D4 evidence) - only with D4 on or a test.
    const bool d4 = st.pickups || std::getenv("BB_PARTY_WORLDACT_TEST");
    // The enemy drop roll (0x17DD0B0): one shared roll per kill for the party (party_droproll.h),
    // logged with D4 on or a test.
    droproll::Init(d4);
    if (d4) {
    HookPrologue(0x17D8570, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                 &PickupEntry, "map item pickup (log)");
    }
    if (std::getenv("BB_PARTY_WORLDACT_TEST")) {
        HookPrologue(0x17DC300, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                     &TreasureSpawnEntry, "MSB treasure spawn (test fixture)");
        const std::initializer_list<std::uint8_t> pro = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                                         0x41, 0x55, 0x41, 0x54, 0x53};
        HookPrologue(0x17D4C40, pro, &RecordInsertEntry<1>, "MapItemMan insert kind 1 (test)");
        HookPrologue(0x17D5320, pro, &RecordInsertEntry<2>, "MapItemMan insert kind 2 (test)");
        HookPrologue(0x17D4FB0, pro, &RecordInsertEntry<3>, "MapItemMan insert kind 3 (test)");
        HookPrologue(0x17D6780, pro, &RecordRemoveEntry, "MapItemMan remove (test)");
        HookPrologue(0x17D57C0, pro, &RecordClearEntry<0>, "MapItemMan clear treasure (test)");
        HookPrologue(0x17D5B40, pro, &RecordClearEntry<1>, "MapItemMan clear treasure, net (test)");
    }
}

bool Enabled() {
    return S().enabled;
}

bool OnHostEvent(int slot, const std::string& body, std::string* why) {
    State& st = S();
    if (!st.enabled || PartyDirector::Get().Role() != PartyRole::Host) {
        if (why) {
            *why = "world actions are off on the host";
        }
        return false;
    }
    Request r;
    if (!RequestFromJson(body, &r, why)) {
        std::lock_guard<std::mutex> lk(st.mu);
        ++st.stats.refused;
        return false;
    }
    std::lock_guard<std::mutex> lk(st.mu);
    ++st.stats.received;
    if (st.in.size() >= 32) {
        if (why) {
            *why = "queue full";
        }
        return false;
    }
    st.in.emplace_back(slot, std::move(r));
    return true;
}

namespace {

void FogPutBackTick(const GameSnapshot& s) {
    std::vector<FogPutBack> due;
    {
        std::lock_guard<std::mutex> lk(g_putback_mu);
        const double now = Now();
        for (auto it = g_putback.begin(); it != g_putback.end();) {
            if (now >= it->at) {
                due.push_back(*it);
                it = g_putback.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (s.loading || !s.world_up) {
        return;
    }
    static double last_action = -1e9;
    for (const FogPutBack& p : due) {
        if (Now() - last_action < 30.0) {
            Log("guest: in boss room %d (event %u) again %.0f s after the last put-back / load: left as is (no loop)",
                p.fog_entity, p.guest_event, Now() - last_action);
            continue;
        }
        last_action = Now();
        if (BossBarShown()) {
            // The fight was on (the host left during it): its boss bar and the boss stay on this
            // game; correct over seamless - a loading screen to the host's lamp (where the party is).
            party::status::SetLastEvent(kBossFogApartNotice);
            const bool ok = b2::ReloadAtHostLamp("boss fight without the host's area (the host left)");
            Log("guest: in boss room %d (event %u) with the boss bar up while the host is in another area: %s; notice "
                "shown", p.fog_entity, p.guest_event, ok ? "a loading screen to the host's lamp" : "NO alone load");
            if (ok) {
                continue;
            }
        }
        std::string what;
        const bool ok = StandAtObject(p.fog_entity, 1.5f, 0.0f, &what);
        // Out of the room: its "guest in" flag off in this copy, so another walk in is seen again.
        if (ok && p.guest_flag) {
            WriteEventFlag(p.guest_flag, false);
        }
        party::status::SetLastEvent(kBossFogApartNotice);
        Log("guest: in boss room %d (event %u) while the host is in another area: no fight can start here "
            "(the boss's events are the host's); put back outside the fog %s%s%s; notice shown",
            p.fog_entity, p.guest_event, ok ? "" : "FAILED (fog not loaded)", what.empty() ? "" : ": ",
            what.c_str());
    }
}
} // namespace

bool GuestFogSkipAllowed(std::int32_t guest_event) {
    const BossGate* g = BossGateByEvent(guest_event);
    if (!g) {
        g = BossGateByLandingEvent(guest_event);
    }
    if (!g) {
        return true;
    }
    const std::uint32_t host_map = g_fog_host_map.load(std::memory_order_relaxed);
    const bool ok = GuestFogOpenWhileHostAt(*g, host_map);
    if (!ok) {
        static std::atomic<std::int32_t> logged{0};
        if (logged.exchange(guest_event) != guest_event) {
            Log("guest: boss fog (guest-entry event %d) keeps the vanilla phantom rule: the host is in %s, another "
                "area (a lone guest's boss fight would have no start or defeat; LB2)", guest_event,
                MapName(host_map).c_str());
        }
    }
    return ok;
}

namespace {
// Guest (playtest 1 bug 8): standing at a boss fog of the host's world that has no prompt yet
// because the host has not had its first fight there (the intro condition stays, BossGate): a
// notice says why, at most once a minute per fog, and a log line.
void FogWaitTick(const GameSnapshot& s) {
    static double last_scan = 0;
    static std::uint32_t last_gate = 0;
    static double last_notice = -1e9;
    const double now = Now();
    if (s.session_role != 6 || !s.world_up || s.loading || now - last_scan < 1.0) {
        return;
    }
    last_scan = now;
    float me[3] = {};
    const bool have_me = MyPos(me);
    static std::string last_state;
    std::string state;
    const BossGate* hit = nullptr;
    float hit_d = 0;
    bool hit_apart = false;
    const std::uint32_t host_map = g_fog_host_map.load(std::memory_order_relaxed);
    for (const BossGate& g : BossGates()) {
        if (!g.fog_entity || !BossGateInBlock(g, s.map_id)) {
            continue;
        }
        bool intro = false, host_in = false, dead = false;
        const bool read = progress::ReadFlag(g.intro_flag ? g.intro_flag : g.host_flag, &intro) &&
                          progress::ReadFlag(g.host_flag, &host_in) && progress::ReadFlag(g.boss_flag, &dead);
        float fog[3] = {};
        const bool found = ObjectPoint(g.fog_entity, 0.0f, 0.0f, fog);
        float d = -1;
        if (found && have_me) {
            const float dx = fog[0] - me[0], dy = fog[1] - me[1], dz = fog[2] - me[2];
            d = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        // Diagnostics: the gate's state whenever it changes (distance bucket: near / 20 m / far).
        char buf[160];
        std::snprintf(buf, sizeof buf, " [%u: read %d intro %d host %d dead %d fog %s%s]", g.guest_event, read ? 1 : 0,
                      intro ? 1 : 0, host_in ? 1 : 0, dead ? 1 : 0, !found ? "not loaded" : !have_me ? "?" : "",
                      !found || !have_me ? "" : d <= kBossFogNoticeRange ? "near" : d <= 20.0f ? "within 20 m" : "far");
        state += buf;
        // D3 (pard4): the host is in another area: the fog keeps the phantom rule (GuestFogSkipAllowed).
        if (!hit && read && !dead && !host_in && found && have_me && d <= kBossFogNoticeRange &&
            !GuestFogOpenWhileHostAt(g, host_map)) {
            hit = &g;
            hit_d = d;
            hit_apart = true;
            continue;
        }
        // P5: a guest-startable first fight has its fog prompt (FogFirstSkip): no "host first" notice.
        const bool guest_starts = GuestFirstFightEnabled() && BossGateGuestStarts(g);
        if (!guest_starts && BossFogNoticeDue(g, read, intro, host_in, dead, found && have_me ? d : -1.0f) && !hit) {
            hit = &g;
            hit_d = d;
        }
    }
    if (state != last_state) {
        last_state = state;
        Log("guest: boss fogs of this block:%s", state.empty() ? " none" : state.c_str());
    }
    if (hit) {
        const BossGate& g = *hit;
        const double d = hit_d;
        if (last_gate == g.guest_event && now - last_notice < 60.0) {
            return;
        }
        last_gate = g.guest_event;
        last_notice = now;
        if (hit_apart) {
            party::status::SetLastEvent(kBossFogApartNotice);
            Log("guest: at boss fog %d (event %u, %.1f m): the host is in %s, another area - no boss fight without "
                "the host's area (the fog keeps the phantom rule); notice shown", g.fog_entity, g.guest_event, d,
                MapName(host_map).c_str());
            return;
        }
        // An action first fight (Laurence's skull, Ebrietas's hit, Lady Maria): what starts it.
        const bossfog::FirstFight* ff = bossfog::FirstFightFor(g.guest_event);
        const char* notice = ff && ff->notice ? ff->notice : kBossFogWaitNotice;
        party::status::SetLastEvent(notice);
        Log("guest: at boss fog %d (event %u, %.1f m): first fight here - %s (host's first-fight flag %u off); "
            "notice shown", g.fog_entity, g.guest_event, d,
            ff && ff->start == bossfog::FirstStart::action ? "it starts by the host's action in the room"
                                                           : "the host goes in first",
            g.intro_flag);
    }
}
} // namespace

void Tick(const GameSnapshot& s, party::PartyLink* link) {
    State& st = S();
    WatchTick(s);
    FixtureTick(Now());
    if (!st.enabled) {
        return;
    }
    const PartyRole role = PartyDirector::Get().Role();
    LiftReplayTick(s);
    LiftRiderTick(s);
    LiftPrebuildTick(s);
    LiftHoldTick();
    LiftBuildGuardTick(s);
    LiftProbeTick(s);
    droproll::Tick(s, link, role == PartyRole::Host);
    if (role == PartyRole::Guest) {
        g_fog_host_map.store(b2::MemberMap(0), std::memory_order_relaxed);
        FogPutBackTick(s);
        FogWaitTick(s);
        GuestTick(s, link);
    } else if (role == PartyRole::Host) {
        // The fixture's clock on the host (kill=N: the host's own kill, each player rolls its drop).
        const bool steady = s.world_up && !s.loading && s.cooperators > 0;
        if (!steady || s.map_id != st.map) {
            st.steady = false;
            st.map = s.map_id;
        } else if (!st.steady) {
            st.steady = true;
            st.steady_since = Now();
        }
        FirstFightTick(s);
        StoryTick(s);
        HostTick(s);
    }
}

bool GuestFirstFightEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("BB_PARTY_GUEST_FIRST_FIGHT");
        return !(e && e[0] == '0');
    }();
    return on;
}

Stats GetStats() {
    std::lock_guard<std::mutex> lk(S().mu);
    return S().stats;
}

} // namespace coop::worldact

#endif // BB_PARTY_WORLDACT_NO_GAME
