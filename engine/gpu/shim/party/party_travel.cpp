// SPDX-License-Identifier: GPL-3.0-or-later
// Party travel, phase B1 (see party_travel.h; the RE is docs/party/travel.md).
//
// Every site below was disassembled from smoketest/out/eboot.elf (1.09) and is compared byte for
// byte before anything is written. Offsets are ours (raw ELF VA). The game's code is System V:
// every call into it goes through a BB_COOP_SYSV pointer at Guest(off).
#include "party_travel.h"
#include "party_async_io.h"
#include "party_perf.h"
#include "party_ids.h"
#include "party_death_rules.h"
#include "party_progress.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#ifndef BB_PARTY_TRAVEL_NO_GAME
#include "coop_hooks.h"
#include "game_state.h"
#include "party_b2.h"
#include "party_chalice.h"
#include "party_director.h"
#include "party_dsummon.h"
#include "party_status.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <mutex>
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

struct KindName {
    TravelKind kind;
    const char* name;
};
constexpr KindName kKindNames[] = {
    {TravelKind::Unknown, "unknown"},
    {TravelKind::Lamp, "lamp"},
    {TravelKind::ScriptedWarp, "scripted_warp"},
    {TravelKind::LuaStageWarp, "lua_stage_warp"},
    {TravelKind::LuaBonfireWarp, "lua_bonfire_warp"},
    {TravelKind::LuaStageKick, "lua_stage_kick"},
    {TravelKind::HostDeath, "host_death"},
    {TravelKind::HuntersMark, "hunters_mark"},
    {TravelKind::Transform, "transform"},
    {TravelKind::GuestSentHome, "guest_sent_home"},
    {TravelKind::GuestDied, "guest_died"},
    {TravelKind::BossCleared, "boss_cleared"},
    {TravelKind::MissionOrPk, "mission_or_pk"},
    {TravelKind::TitleOrDebug, "title_or_debug"},
};

struct CallSiteKind {
    std::uint64_t site;
    TravelKind kind;
};
// travel.md 1.6, re-derived by scanning eboot.elf for every e8/e9 rel32 to 0x13CDE30 (15 calls,
// 4 tail jumps: 0x13175EF, 0x1383A5C, 0x13859DF, 0x1389F20).
constexpr CallSiteKind kCallSites[] = {
    {0x13ce002, TravelKind::Lamp},           // 0x13CDF30 lamp warp by id
    {0x17c0bed, TravelKind::ScriptedWarp},   // EMEVD 2003[14]
    {0x132e03e, TravelKind::LuaStageWarp},   // WarpNextStage
    {0x132e0b4, TravelKind::LuaBonfireWarp}, // WarpNextStage_Bonfire
    {0x138c5f0, TravelKind::LuaBonfireWarp}, // Lua_Warp_1
    {0x1332b84, TravelKind::LuaStageKick},   // WarpNextStageKick
    {0x1381ede, TravelKind::HostDeath},      // SoloPlayDeath_2
    {0x1389f20, TravelKind::HuntersMark},    // OnReviveMagic_1 (tail jump)
    // The Hunter's Mark item (goods 100, SpEffect 2100, stateInfo 16): the special-effect update
    // 0x18FEF50 calls 0x1317550(ctx, 1) at 0x18FFA1D, which selects the last lamp (0x1332BC0) and
    // tail-jumps to the funnel at 0x13175EF - so the funnel's return address is this call's.
    {0x18ffa1d, TravelKind::HuntersMark},
    {0x1381a93, TravelKind::GuestSentHome},  // HostDead_1
    {0x138a51c, TravelKind::GuestSentHome},  // OnLeave_Limit
    {0x1382663, TravelKind::GuestDied},      // PartyGhostDeath_2
    {0x13856f4, TravelKind::BossCleared},    // BlockClear2_1
    {0x13859df, TravelKind::BossCleared},    // BlockClear2_3 (tail jump)
    {0x138b513, TravelKind::MissionOrPk},    // Failed_BossAreaMission_LeaveMap
    {0x1383a5c, TravelKind::MissionOrPk},    // PlayerKill_4030_1 (tail jump)
    {0x132e11e, TravelKind::TitleOrDebug},   // 0x132E0C0, map 0x01000000
    {0x132e166, TravelKind::TitleOrDebug},
    {0x13175ef, TravelKind::TitleOrDebug},   // 0x1317550 debug presets (tail jump)
    {0x1317632, TravelKind::TitleOrDebug},
};

std::uint32_t Low(std::uint64_t v) {
    return static_cast<std::uint32_t>(v);
}

} // namespace

const char* TravelKindName(TravelKind k) {
    for (const KindName& n : kKindNames) {
        if (n.kind == k) {
            return n.name;
        }
    }
    return "unknown";
}

const char* TravelKindLabel(TravelKind k) {
    switch (k) {
    case TravelKind::HostDeath: return "Respawning with the party";
    case TravelKind::HuntersMark: return "Hunter's Mark: back to the lamp with the party";
    case TravelKind::Lamp:
    case TravelKind::LuaBonfireWarp: return "Travelling to a lamp with the party";
    case TravelKind::GuestDied: return "Respawning with the party";
    default: return "Travelling with the party";
    }
}

TravelKind TravelKindFromName(const std::string& name) {
    for (const KindName& n : kKindNames) {
        if (name == n.name) {
            return n.kind;
        }
    }
    return TravelKind::Unknown;
}

TravelKind TravelKindFromCallSite(std::uint64_t call_site) {
    for (const CallSiteKind& c : kCallSites) {
        if (c.site == call_site) {
            return c.kind;
        }
    }
    return TravelKind::Unknown;
}

bool TravelKindBroadcast(TravelKind k) {
    switch (k) {
    case TravelKind::Lamp:
    case TravelKind::ScriptedWarp:
    case TravelKind::LuaStageWarp:
    case TravelKind::LuaBonfireWarp:
    case TravelKind::LuaStageKick:
    case TravelKind::HostDeath:
    case TravelKind::HuntersMark:
    case TravelKind::Transform:
        return true;
    default:
        return false;
    }
}

std::string DescribeTravel(const TravelIntent& t) {
    char text[384];
    std::snprintf(text, sizeof text,
                  "#%llu %s (site +0x%x): lamp %d, map m%02u_%02u_%02u_%02u (0x%08x), warp point %d, mode %u, "
                  "respawn 0x%016llx, last lamp 0x%016llx%s%s",
                  static_cast<unsigned long long>(t.seq), TravelKindName(t.kind), t.call_site,
                  static_cast<int>(t.lamp_id), t.Area(), t.Block(), t.Region(), t.Index(), t.packed_map,
                  static_cast<int>(t.warp_point), t.respawn_mode, static_cast<unsigned long long>(t.respawn_record),
                  static_cast<unsigned long long>(t.last_lamp), t.has_pos ? ", exact position" : "",
                  t.chalice_channel ? ", chalice dungeon" : "");
    std::string out = text;
    if (t.chalice_channel) {
        out += " " + std::to_string(t.chalice_channel);
    }
    return out;
}

json::Value TravelToJson(const TravelIntent& t) {
    json::Value v = json::Value::make_object();
    v.set("seq", json::hex(t.seq));
    v.set("kind", TravelKindName(t.kind));
    v.set("site", t.call_site);
    v.set("lamp", t.lamp_id);
    v.set("map", t.packed_map);
    v.set("warp_point", t.warp_point);
    v.set("mode", t.respawn_mode);
    v.set("respawn", json::hex(t.respawn_record));
    v.set("last_lamp", json::hex(t.last_lamp));
    if (t.chalice_channel) {
        v.set("chalice", json::hex(t.chalice_channel));
    }
    if (t.alone) {
        v.set("alone", 1u);
    }
    if (t.has_pos) {
        json::Value p = json::Value::make_object();
        p.set("map", t.pos_map);
        json::Value pos = json::Value::make_array(), rot = json::Value::make_array();
        for (int i = 0; i < 4; ++i) {
            pos.push(json::f32(t.pos[i])); // exact bits: the guest lands where the host stood
            rot.push(json::f32(t.rot[i]));
        }
        p.set("pos", pos);
        p.set("rot", rot);
        v.set("transform", p);
    }
    return v;
}

bool TravelFromJson(const json::Value& v, TravelIntent* out, std::string* error) {
    try {
        if (v.type != json::Value::Type::Object) {
            throw std::runtime_error("not an object");
        }
        TravelIntent t;
        t.seq = json::u64(v, "seq");
        t.kind = TravelKindFromName(json::str(v, "kind"));
        t.call_site = json::u32(v, "site");
        t.lamp_id = json::u32(v, "lamp");
        t.packed_map = json::u32(v, "map");
        t.warp_point = json::u32(v, "warp_point");
        t.respawn_mode = json::u32(v, "mode");
        t.respawn_record = json::u64(v, "respawn");
        t.last_lamp = json::u64(v, "last_lamp");
        if (v.find("chalice")) {
            t.chalice_channel = json::u64(v, "chalice");
        }
        if (v.find("alone")) {
            t.alone = json::u32(v, "alone") != 0;
        }
        if (const json::Value* p = v.find("transform")) {
            t.pos_map = json::u32(*p, "map");
            const std::vector<json::Value>& pos = json::arr(*p, "pos");
            const std::vector<json::Value>& rot = json::arr(*p, "rot");
            if (pos.size() != 4 || rot.size() != 4) {
                throw std::runtime_error("transform: pos / rot need 4 components");
            }
            for (int i = 0; i < 4; ++i) {
                t.pos[i] = json::as_f32(pos[i], "pos");
                t.rot[i] = json::as_f32(rot[i], "rot");
            }
            t.has_pos = true;
        }
        if (t.seq == 0) {
            throw std::runtime_error("seq 0");
        }
        *out = t;
        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

bool TravelMapKnown(std::uint32_t packed_map) {
    return ids::MapKnown(packed_map);
}

bool TravelLampKnown(std::uint32_t id) {
    return ids::ReturnPointKnown(id);
}

bool TravelBonfireKnown(std::uint32_t id) {
    // 0x132E050 derives the map from the id itself (area id / 100000, block id / 10000 % 10).
    return ids::EntityKnown(id) && ids::MapKnown(ids::EntityMap(id));
}

bool TravelIntoChalice(const TravelIntent& t) {
    auto area_of_map = [](std::uint32_t m) { return m != kTravelNone && ((m >> 24) & 0xff) == 29; };
    auto area_of_id = [](std::uint32_t id) { return id != kTravelNone && id / 100000 == 29; };
    const bool lamp_kind = t.kind == TravelKind::Lamp || t.kind == TravelKind::HostDeath ||
                           t.kind == TravelKind::HuntersMark;
    return area_of_map(t.packed_map) || (t.has_pos && area_of_map(t.pos_map)) ||
           (lamp_kind && area_of_id(t.lamp_id)) || (t.kind == TravelKind::LuaBonfireWarp && area_of_id(t.warp_point)) ||
           ((t.kind == TravelKind::HostDeath || t.kind == TravelKind::HuntersMark) &&
            area_of_id(static_cast<std::uint32_t>(t.last_lamp)));
}

bool ChaliceReplayAllowed(const TravelIntent& t, std::uint64_t local_channel) {
    return !TravelIntoChalice(t) || (t.chalice_channel != 0 && t.chalice_channel == local_channel);
}

bool SanitizePeerTravel(TravelIntent* t, std::string* why) {
    if (!TravelKindBroadcast(t->kind)) {
        if (why) {
            *why = std::string("kind ") + TravelKindName(t->kind) + " is never broadcast";
        }
        return false;
    }
    auto lamp = [](std::uint32_t id) { return id == kTravelNone || TravelLampKnown(id) ? id : kTravelNone; };
    auto record = [&](std::uint64_t r) {
        const std::uint32_t lo = static_cast<std::uint32_t>(r);
        return lo == kTravelNone || TravelLampKnown(lo) ? r : ~0ull;
    };
    t->lamp_id = lamp(t->lamp_id);
    t->respawn_record = record(t->respawn_record);
    t->last_lamp = record(t->last_lamp);
    if (t->packed_map != kTravelNone && !TravelMapKnown(t->packed_map)) {
        t->packed_map = kTravelNone;
    }
    if (t->kind == TravelKind::LuaBonfireWarp) {
        if (t->warp_point != kTravelNone && !TravelBonfireKnown(t->warp_point)) {
            t->warp_point = kTravelNone;
        }
    } else if (t->warp_point != kTravelNone &&
               (t->packed_map == kTravelNone || !ids::WarpPointKnown(t->packed_map, t->warp_point))) {
        // A stage warp to a point that is not in the destination map: the map's default entry.
        t->warp_point = kTravelNone;
    }
    if (t->has_pos) {
        bool ok = TravelMapKnown(t->pos_map);
        for (int i = 0; i < 4; ++i) {
            ok = ok && std::isfinite(t->pos[i]) && std::fabs(t->pos[i]) < 100000.0f && std::isfinite(t->rot[i]) &&
                 std::fabs(t->rot[i]) < 1000.0f;
        }
        if (!ok) {
            t->has_pos = false;
            t->pos_map = kTravelNone;
        }
    }
    return true;
}

std::string TravelToJsonText(const TravelIntent& t) {
    return json::dump(TravelToJson(t), 0);
}

bool TravelFromJsonText(const std::string& text, TravelIntent* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) {
        if (error) {
            *error = err;
        }
        return false;
    }
    return TravelFromJson(v, out, error);
}

const char* ReplayMethodName(ReplayMethod m) {
    switch (m) {
    case ReplayMethod::None: return "none";
    case ReplayMethod::LampWarp: return "lamp warp 0x13CDF30";
    case ReplayMethod::StageWarp: return "stage warp 0x132E010";
    case ReplayMethod::BonfireWarp: return "bonfire warp 0x132E050";
    case ReplayMethod::Transform: return "forced transform";
    }
    return "?";
}

ReplayPlan ChooseReplay(const TravelIntent& t) {
    ReplayPlan p;
    switch (t.kind) {
    case TravelKind::Lamp: {
        // The key 0x13CDF30 got; else the row's +4 it stored in +0x153C (equal in every observed
        // row, travel.md probe P5).
        const std::uint32_t id = t.lamp_id != kTravelNone ? t.lamp_id : Low(t.respawn_record);
        if (id != kTravelNone) {
            p = {ReplayMethod::LampWarp, id};
        }
        break;
    }
    case TravelKind::HostDeath:
    case TravelKind::HuntersMark:
        // Both respawn at the last lamp (+0x1528, the record 0x1332BC0 reads); same id space as
        // 0x13CDF30 (travel.md 1.1.0 / 3.2).
        if (Low(t.last_lamp) != kTravelNone) {
            p = {ReplayMethod::LampWarp, Low(t.last_lamp)};
        }
        break;
    case TravelKind::ScriptedWarp:
    case TravelKind::LuaStageWarp:
    case TravelKind::LuaStageKick:
        if (t.packed_map != kTravelNone) {
            p = {ReplayMethod::StageWarp, kTravelNone};
        }
        break;
    case TravelKind::LuaBonfireWarp:
        if (t.warp_point != kTravelNone) {
            p = {ReplayMethod::BonfireWarp, t.warp_point};
        }
        break;
    default:
        break;
    }
    if (p.method == ReplayMethod::None && t.has_pos && t.pos_map != kTravelNone) {
        p = {ReplayMethod::Transform, kTravelNone};
    }
    return p;
}

bool HostTravelQueue::Push(const TravelIntent& t) {
    if (t.seq <= last_seq_) {
        return false;
    }
    last_seq_ = t.seq;
    if (q_.size() >= kMax) {
        q_.pop_front();
    }
    q_.push_back(t);
    return true;
}

bool HostTravelQueue::Pop(TravelIntent* out) {
    if (q_.empty()) {
        return false;
    }
    *out = q_.front();
    q_.pop_front();
    return true;
}

const char* TravelFollowName(TravelFollow f) {
    switch (f) {
    case TravelFollow::Follow: return "follow";
    case TravelFollow::IgnorePrologue: return "in own world (prologue)";
    case TravelFollow::IgnoreOwnWorld: return "in own world (not summoned)";
    }
    return "?";
}

EmptyRoomStep PlanEmptyRoom(const EmptyRoomView& v) {
    if (v.since_travel < 0) {
        return EmptyRoomStep::None;
    }
    if (v.b2_holds) {
        return v.load_seen && v.in_world && !v.b2_rejoin ? EmptyRoomStep::Drop : EmptyRoomStep::None;
    }
    if (v.since_travel > kEmptyRoomTimeout) {
        return EmptyRoomStep::Timeout;
    }
    return v.load_seen && v.in_world ? EmptyRoomStep::Check : EmptyRoomStep::None;
}

TravelFollow GuestFollowsHostTravel(const FollowState& s) {
    constexpr int kJoining = 4, kClient = 6; // RoleTryingToJoin, RoleClient (game_state.h)
    if (s.session_role == kClient || s.session_role == kJoining) {
        return TravelFollow::Follow;
    }
    if (s.client_ago >= 0 && s.client_ago <= kFollowClientWindow) {
        return TravelFollow::Follow; // just left the host's world (sent home, B2 fallback)
    }
    return s.start_ready ? TravelFollow::IgnoreOwnWorld : TravelFollow::IgnorePrologue;
}

bool ReplayAllowedNow(const ReplayState& s) {
    return s.world_up && !s.loading && !s.transition_requested && s.session_role != 4 /* joining */ &&
           s.session_role != 7 /* leaving */;
}

bool GuestTravel::Offer(const TravelIntent& t, double now) {
    if (t.seq <= last_seq_) {
        return false;
    }
    // The newest wins, also over one under way (a second warp while loading: travel.md 3.3).
    last_seq_ = t.seq;
    cur_ = t;
    phase_ = Phase::Pending;
    since_ = now;
    load_seen_ = false;
    return true;
}

std::optional<TravelIntent> GuestTravel::Step(const ReplayState& s, double now) {
    stable_ = ReplayAllowedNow(s) ? stable_ + 1 : 0;
    switch (phase_) {
    case Phase::Idle:
        return std::nullopt;
    case Phase::Pending:
        if (now - since_ > kPendingTimeout) {
            phase_ = Phase::Idle;
            return std::nullopt;
        }
        if (stable_ >= kStableTicks) {
            phase_ = Phase::Underway;
            since_ = now;
            load_seen_ = false;
            return cur_;
        }
        return std::nullopt;
    case Phase::Underway:
        // Arrived: a load happened and the world has been steady since.
        load_seen_ = load_seen_ || s.loading || !s.world_up;
        if ((load_seen_ && stable_ >= kStableTicks) || now - since_ > kActiveTimeout) {
            phase_ = Phase::Idle;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<TravelIntent> GuestTravel::Destination() const {
    if (phase_ == Phase::Idle) {
        return std::nullopt;
    }
    return cur_;
}

void GuestTravel::MarkReplayed(double now) {
    if (phase_ == Phase::Idle) {
        return;
    }
    phase_ = Phase::Underway;
    since_ = now;
    stable_ = 0;
    load_seen_ = false;
}

const char* GuestRedirectName(GuestRedirect r) {
    switch (r) {
    case GuestRedirect::None: return "none (home)";
    case GuestRedirect::TravelDestination: return "the party's travel destination";
    case GuestRedirect::HostLamp: return "the host's last lamp";
    }
    return "?";
}

GuestRedirect ChooseGuestRedirect(TravelKind kind, bool have_travel_destination, std::uint32_t host_lamp_id) {
    if (kind != TravelKind::GuestDied && kind != TravelKind::HuntersMark) {
        return GuestRedirect::None;
    }
    // A host warp in flight wins: the host will not be at its old lamp.
    if (have_travel_destination) {
        return GuestRedirect::TravelDestination;
    }
    // 0 is no lamp row either (WarpParam / bonfire ids are 7-digit map-based ids).
    if (host_lamp_id != kTravelNone && host_lamp_id != 0) {
        return GuestRedirect::HostLamp;
    }
    return GuestRedirect::None;
}

std::uint32_t HostLampFromTravel(const TravelIntent& t) {
    const std::uint32_t id = Low(t.last_lamp);
    return id == 0 ? kTravelNone : id;
}

std::uint32_t GatherLampFor(std::uint32_t host_lamp, std::uint32_t host_map) {
    if (host_map == 0 || host_map == 0xffffffffu) {
        return host_lamp;
    }
    const std::uint32_t area = (host_map >> 24) & 0xff, block = (host_map >> 16) & 0xff;
    auto in_map = [&](std::uint32_t id) { return id / 100000 == area && (id / 10000) % 10 == block; };
    if (host_lamp != kTravelNone && host_lamp != 0 && in_map(host_lamp)) {
        return host_lamp;
    }
    for (const std::uint32_t id : ids::kPartyReturnPoints) {
        if (in_map(id)) {
            return id;
        }
    }
    return host_lamp;
}

// ---------------------------------------------------------------------------------------------
// Game part
// ---------------------------------------------------------------------------------------------
#ifndef BB_PARTY_TRAVEL_NO_GAME

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using ull = unsigned long long;
using Clock = std::chrono::steady_clock;

// WorldTransitionState / GameStateMan slot: 0x13CDE3E `mov rax, [rip -> 0x5556678]`.
constexpr u64 kWts = 0x5556678;
constexpr u64 kWtsRequested = 0x08, kWtsMap = 0x0c, kWtsWarpPoint = 0x10, kWtsForced = 0x1520,
              kWtsLastLamp = 0x1528, kWtsRespawnMode = 0x1538, kWtsRespawn = 0x153c, kWtsOwnWorld = 0x1592;
constexpr u64 kWorldChrMan = 0x553e878; // +0x60 PlayerIns, +0x78 chr type

constexpr u64 kFunnel = 0x13cde30;        // SessionWorldTransition, no arguments, returns 0/1
constexpr u64 kLampWarp = 0x13cdf30;      // void (u32 id)
constexpr u64 kOnReviveMagic = 0x1389d20; // Lua state: Hunter's Mark (session left at 0x1389DDD)
constexpr u64 kBlockClear3 = 0x1385930;   // Lua state BlockClear2_3: tail jump 0x13859DF -> funnel
constexpr u64 kBlockClear3Jump = 0x13859df;
constexpr u64 kGuestDiedCall = 0x1382663; // PartyGhostDeath_2: `call 0x13CDE30` (e8 c8 b7 04 00)
constexpr u64 kMarkTailJump = 0x1389f20;  // OnReviveMagic_1: `jmp 0x13CDE30` (e9 0b 3f 04 00)
constexpr u64 kMarkItemCall = 0x18ffa1d;  // `call 0x1317550` (Hunter's Mark item); its tail jump reaches the funnel
constexpr u64 kWarpNextStage = 0x132e010; // (ctx unused, area, block, region, u8 index, warp point)
constexpr u64 kWarpBonfire = 0x132e050;   // (ctx unused, id)
constexpr u64 kSetForcedMap = 0x156cf10;  // (const u32*) -> +0x14F0
constexpr u64 kSetForcedPos = 0x156cf20;  // (const vec4*, 16-aligned: vmovaps) -> +0x1500
constexpr u64 kSetForcedRot = 0x156cf40;  // (const vec4*, 16-aligned) -> +0x1510
constexpr u64 kSetForced = 0x156cf60;     // () -> +0x1520 = 1
constexpr u64 kSelectTarget = 0x1332bc0;  // (ctx unused, int) : +0x1520 ? +0x0C = +0x14F0 : last lamp

using FunnelFn = u64(BB_COOP_SYSV*)();
using LampWarpFn = void(BB_COOP_SYSV*)(u32);
using StageWarpFn = u64(BB_COOP_SYSV*)(u64, u32, u32, u32, u32, u32);
using BonfireWarpFn = void(BB_COOP_SYSV*)(u64, u32);
using PtrFn = void(BB_COOP_SYSV*)(const void*);
using VoidFn = void(BB_COOP_SYSV*)();
using SelectFn = u64(BB_COOP_SYSV*)(u64, u32);
using Lua6Fn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64);

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    perf::Scope z(perf::Zone::Log);
    coop::asyncio::Print(std::string("Party travel: ") + PartyLogStamp() + line + "\n"); // never blocks the caller (party_async_io.h)
}

bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0' && !v[1];
}

double Now() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

std::atomic<bool> g_travel_on{false};
std::atomic<bool> g_in_replay{false};       // our own warp is running: the hooks stand aside
std::atomic<u32> g_lamp_key{kTravelNone};   // 0x13CDF30's edi, read by the funnel it calls
std::atomic<int> g_block_clear3{0};         // inside BlockClear2_3 (its funnel call is a tail jump)
std::atomic<u32> g_guest_map{0};            // guest: its current map (TravelTick), 0 unknown
std::atomic<u32> g_death_lamp{kTravelNone}; // guest: the host's last lamp (SetGuestDeathRedirect)
std::atomic<std::uint64_t> g_host_chalice{0};  // guest: the host's chalice dungeon (its last intent's)
std::atomic<bool> g_start_ready{true};      // the director's party start READY (SetLocalStartReady)
std::atomic<double> g_last_client{-1.0};    // guest: Now() when its session role last was client (6)
std::atomic<double> g_offer_at{-1.0};       // guest: Now() when the pending travel was taken

/// Whether this guest follows a host travel now; `at` the moment that counts for "just left the
/// host's world" (the offer's time for a pending travel).
TravelFollow FollowNow(int session_role, double at) {
    FollowState f;
    f.start_ready = g_start_ready.load();
    f.session_role = session_role;
    const double last = g_last_client.load();
    f.client_ago = last < 0 ? -1.0 : std::max(0.0, at - last);
    return GuestFollowsHostTravel(f);
}

/// A host lamp a guest may be sent to: one in a Chalice Dungeon only when the guest's own game
/// has the host's dungeon in its current slot (party_chalice.h).
bool LampUsable(u32 lamp) {
    if (lamp == kTravelNone || lamp / 100000 != 29) {
        return true;
    }
    const std::uint64_t host = g_host_chalice.load();
    return host != 0 && host == chalice::LocalChannel();
}
std::atomic<GuestRedirectFn> g_redirect_cb{nullptr};
std::atomic<bool> g_death_redirect_on{true};
std::atomic<SendHomeVetoFn> g_send_home_veto{nullptr};
std::atomic<GuestDeathHoldFn> g_guest_death_hold{nullptr};
std::atomic<HostDeathInterceptFn> g_host_death_intercept{nullptr};
void* g_funnel_orig = nullptr;
void* g_block_clear3_orig = nullptr;
void* g_lamp_warp_orig = nullptr;
std::atomic<LampWarpInterceptor> g_lamp_intercept{nullptr};
std::atomic<bool> g_lamp_decided{false};  // RunLampWarpDecided: the interceptor stands aside
// Batch 6: the host's lamp warp held while a member it just served a room loads in.
std::atomic<u32> g_held_lamp{kTravelNone};
std::atomic<bool> g_held_alone{false};
std::atomic<double> g_held_at{0};
std::atomic<bool> g_hold_release{false};  // HostHeldLampTick runs the held warp: no observer, no hold
std::atomic<MarkInterceptor> g_mark_intercept{nullptr};
std::atomic<SameMapWarpFn> g_same_map{nullptr};
std::atomic<SameMapStageFn> g_same_map_stage{nullptr};
std::atomic<bool> g_next_alone{false};    // host: the next captured travel is alone (split travel)
std::atomic<bool> g_death_alone{false};   // host: every captured HostDeath is alone (hostdeath)
std::atomic<double> g_last_alone_at{0};   // host: when the last alone travel was captured (Now())
std::atomic<LampUseObserver> g_lamp_observer{nullptr};
// RunLampWarpNotRest: the warp running now is no lamp use of the player (going home / falling
// recovery): the observer (party_balance's rest) does not see it.
std::atomic<const char*> g_warp_not_rest{nullptr};

// Seeded from the wall clock: a restarted host keeps counting upward, so guests that saw the
// old host's intents do not drop the new ones as duplicates.
std::atomic<u64> g_seq{0};
u64 NextSeq() {
    u64 cur = g_seq.load();
    if (cur == 0) {
        const u64 seed = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
        g_seq.compare_exchange_strong(cur, seed);
    }
    return g_seq.fetch_add(1) + 1;
}

std::mutex g_host_mu;
HostTravelQueue g_host_queue; // under g_host_mu
// Host: when its last broadcast travel started (Now()); 0 none. Main thread / the funnel hook
// (also the main thread).
std::atomic<double> g_host_left_at{0};
std::atomic<double> g_last_capture{0}; // host: the last CaptureHost (Now())
bool g_host_load_seen = false;

// 0x13CDDA0: BeginLeaveSession(SprjSessionManager) 0x1ED07A0 (role -> 7, LeaveRoom), then
// 0x15BED10(network flow +0x16F8) - the game's own leave idiom (the funnel's, OnLeaveMenu_Yes's);
// the host watchdog (party_director.cpp) uses the same helper.
constexpr u64 kLeaveSessionAndDrop = 0x13cdda0;
using LeaveFn = void(BB_COOP_SYSV*)();

/// Host, main thread: after its own travel the game keeps the room through the load (the
/// "Keep session on map reload" patch retargets the stage destructor's stop, 0x19471B1), and the
/// guests have left it to follow. An empty room left open blocks the next Beckoning Bell (the
/// director rings only when idle) until the game's own timeout ends it ~60 s later, and the
/// summon into the stale room did not complete (live run 2026-10-10). So once the host is in
/// its new world with no cooperator left, it leaves the room as vanilla would have at the load.
void HostCloseEmptyRoom() {
    const double at = g_host_left_at.load();
    if (at == 0) {
        return;
    }
    const double now = Now();
    const GameSnapshot g = ReadGameState(false);
    g_host_load_seen = g_host_load_seen || g.loading || !g.world_up;
    // B2 keeps the room while a member's task exists; once the guest fell back to B1 (task
    // gone) the empty room is closed as in B1.
    EmptyRoomView v;
    v.since_travel = now - at;
    v.load_seen = g_host_load_seen;
    v.in_world = g.world_up && !g.loading;
    v.b2_holds = b2::Enabled() && b2::HostHasMemberTasks();
    v.b2_rejoin = b2::HostRejoinRunning();
    switch (PlanEmptyRoom(v)) {
    case EmptyRoomStep::None:
        return;
    case EmptyRoomStep::Drop:
        g_host_left_at = 0;
        g_host_load_seen = false;
        Log("host travel: arrived %.1f s after the warp; the B2 re-join kept the room", now - at);
        return;
    case EmptyRoomStep::Timeout:
        g_host_left_at = 0;
        g_host_load_seen = false;
        Log("host travel: no arrival within %.0f s; room left as it is", kEmptyRoomTimeout);
        return;
    case EmptyRoomStep::Check:
        break;
    }
    if (g.session_role != 3 /* host */ && g.session_role != 1 /* creating */) {
        g_host_left_at = 0;
        g_host_load_seen = false;
        Log("host travel: arrived %.1f s after the warp, session %s: nothing to close", now - at,
            SessionRoleName(g.session_role));
        return;
    }
    const GameSnapshot full = ReadGameState(true);
    if (full.cooperators != 0) {
        if (full.cooperators > 0) {
            g_host_left_at = 0; // someone is (still) in: keep the room
            g_host_load_seen = false;
            Log("host travel: arrived with %d cooperator(s) in the room; kept", full.cooperators);
        }
        return;
    }
    g_host_left_at = 0;
    g_host_load_seen = false;
    static const bool code_ok = Matches(kLeaveSessionAndDrop, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48,
                                                               0x8b, 0x05, 0xc7, 0x88, 0x18, 0x04}) &&
                                CallsTo(kLeaveSessionAndDrop + 0x55, 0x1ed07a0);
    if (code_ok) {
        reinterpret_cast<LeaveFn>(Guest(kLeaveSessionAndDrop))();
    }
    const bool ok = code_ok && ReadGameState(false).session_role != 3; // leaving (7), or idle at once
    Log("host travel: arrived %.1f s after the warp with an empty room: leave %s", now - at,
        ok ? "started (bell next)" : "refused");
}
std::mutex g_guest_mu;
GuestTravel g_guest;          // under g_guest_mu

u64 Wts() {
    u64 p = 0;
    return SafeGet(Guest(kWts), &p) ? p : 0;
}

template <class T>
T WtsField(u64 wts, u64 off, T fallback) {
    T v{};
    return wts && SafeGet(wts + off, &v) ? v : fallback;
}

int LocalChrType() {
    u64 wcm = 0, player = 0;
    int type = -1;
    if (SafeGet(Guest(kWorldChrMan), &wcm) && wcm && SafeGet(wcm + 0x60, &player) && player) {
        SafeGet(player + 0x78, &type);
    }
    return type;
}

PartyRole Role() {
    return PartyDirector::Get().Role();
}

// ---- Host capture ----

void CaptureHost(u64 site, TravelKind kind) {
    const u64 wts = Wts();
    if (!wts) {
        return;
    }
    // Only a warp out of our own world (+0x1592 = 1; 0 = a summoned guest's) is the party's.
    if (WtsField<u8>(wts, kWtsOwnWorld, 0) == 0) {
        return;
    }
    TravelIntent t;
    t.seq = NextSeq();
    t.kind = kind;
    t.call_site = static_cast<u32>(site);
    t.packed_map = WtsField<u32>(wts, kWtsMap, kTravelNone);
    t.warp_point = WtsField<u32>(wts, kWtsWarpPoint, kTravelNone);
    t.respawn_mode = WtsField<u32>(wts, kWtsRespawnMode, 0);
    t.respawn_record = WtsField<u64>(wts, kWtsRespawn, ~0ull);
    t.last_lamp = WtsField<u64>(wts, kWtsLastLamp, ~0ull);
    if (kind == TravelKind::Lamp) {
        t.lamp_id = g_lamp_key.exchange(kTravelNone);
    }
    t.alone = g_next_alone.exchange(false) || (kind == TravelKind::HostDeath && g_death_alone.load());
    if (t.alone) {
        g_last_alone_at = Now();
    }
    {
        // One warp, one TRAVEL: the game can reach the funnel twice for the same lamp warp within a
        // few frames (live: a guest's lamp request run by the host captured twice, 26 ms apart, two
        // TRAVEL and two "rested" events). Same kind, lamp, map and respawn within 1 s = the same warp.
        static TravelIntent last{};
        static double last_at = -1e9;
        const double now = Now();
        if (now - last_at < 1.0 && last.kind == t.kind && last.lamp_id == t.lamp_id && last.packed_map == t.packed_map &&
            last.respawn_record == t.respawn_record) {
            Log("host travel at +0x%llx: the same warp again within 1 s; not sent twice", ull(site));
            return;
        }
        last = t;
        last_at = now;
    }
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        g_host_queue.Push(t);
    }
    g_host_left_at = Now(); // the room is closed once the host has arrived (TravelTick)
    g_last_capture = Now();
    Log("host travel %s -> %s%s", DescribeTravel(t).c_str(), ReplayMethodName(ChooseReplay(t).method),
        t.alone ? " (alone: the guests stay)" : "");
}

// Replaces 0x13CDF30 (void(u32 id)): the key for the host's capture, and the interceptor.
thread_local u64 t_lamp_callers[6];

__attribute__((noinline)) BB_COOP_SYSV void LampWarpHook(u64 id) {
    perf::Scope z(perf::Zone::HookLampWarp);
    {
        // Entered by a jmp: our return address is the game's call site, the saved rbp the
        // caller's frame (the game's functions keep rbp frames).
        const u64 base = Guest(0);
        const auto rel = [base](u64 a) { return a >= base && a - base < (1ull << 32) ? a - base : 0; };
        t_lamp_callers[0] = rel(reinterpret_cast<u64>(__builtin_return_address(0)));
        u64 fp = 0;
        SafeGet(reinterpret_cast<u64>(__builtin_frame_address(0)), &fp);
        for (int i = 1; i < 6; ++i) {
            u64 ret = 0, next = 0;
            if (!fp || !SafeGet(fp + 8, &ret) || !SafeGet(fp, &next)) {
                t_lamp_callers[i] = 0;
                continue;
            }
            t_lamp_callers[i] = rel(ret);
            fp = next;
        }
    }
    if (!g_in_replay.load()) {
        if (const char* why = g_warp_not_rest.load()) {
            Log("lamp warp %u (%s): no lamp use of the player, not a rest", static_cast<u32>(id), why);
        } else if (LampUseObserver obs = g_lamp_observer.load(); obs && !g_hold_release.load()) {
            obs(static_cast<u32>(id));
        }
        if (LampWarpInterceptor fn = g_lamp_intercept.load(); fn && !g_lamp_decided.load() && fn(static_cast<u32>(id))) {
            return;
        }
        // Batch 6: a member the host just served a room is still loading in: the travel waits
        // (HostHeldLampTick runs it), or that guest arrives in an empty map and its join stalls.
        if (Role() == PartyRole::Host && !g_hold_release.load()) {
            std::string why;
            if (b2::HostTravelHeldForJoin(&why)) {
                const u32 was = g_held_lamp.exchange(static_cast<u32>(id));
                g_held_alone = g_next_alone.exchange(false);
                if (was == kTravelNone) {
                    g_held_at = Now();
                }
                Log("host lamp warp to %u held: %s; it runs once the member is in (at most %.0f s)%s",
                    static_cast<u32>(id), why.c_str(), dsummon::kHostTravelHoldMax,
                    was != kTravelNone ? " (replaces the held warp)" : "");
                party::status::SetLastEvent("Travel waits: a member is joining");
                return;
            }
        }
        // A lamp of the loaded map: no load at all (party_b2 InWorldLampWarp; every caller: the
        // lamp menu, a headstone, a test step, a request run by the host).
        if (SameMapWarpFn fn = g_same_map.load(); fn && fn(static_cast<u32>(id), g_next_alone.load())) {
            g_next_alone = false;
            return;
        }
        g_lamp_key = static_cast<u32>(id);
    }
    reinterpret_cast<LampWarpFn>(g_lamp_warp_orig)(static_cast<u32>(id));
}

/// Host, main thread (TravelTick): runs a lamp warp held for a joining member once the join is
/// through (in the world, failed) or the hold ran out; dropped when a load came first.
void HostHeldLampTick() {
    const u32 id = g_held_lamp.load();
    if (id == kTravelNone) {
        return;
    }
    const GameSnapshot g = ReadGameState(false);
    if (Role() != PartyRole::Host || g.loading || !g.world_up) {
        g_held_lamp = kTravelNone;
        Log("host lamp warp to %u held: dropped (%s)", id, Role() != PartyRole::Host ? "no longer the host" : "a load came first");
        return;
    }
    const double held = Now() - g_held_at.load();
    std::string why;
    if (held < dsummon::kHostTravelHoldMax + 5.0 && b2::HostTravelHeldForJoin(&why)) {
        return;
    }
    g_held_lamp = kTravelNone;
    Log("host lamp warp to %u released after %.1f s (%s)", id, held,
        held >= dsummon::kHostTravelHoldMax + 5.0 ? "hold ran out" : "the joining member is in or its join ended");
    g_hold_release = true;
    RunLampWarpDecided(id, g_held_alone.load());
    g_hold_release = false;
}

BB_COOP_SYSV void ReviveMagicEntry(u64, u64, u64, u64, u64, u64) {
    // Hunter's Mark: OnReviveMagic leaves the session at 0x1389DDD and reaches the funnel only
    // later through OnReviveMagic_1's tail jump, so the intent is taken here. Phantoms (1, 2,
    // 0xC) are not the host; only an own-world body (0, or 8 dead/grey) is.
    if (!g_travel_on.load() || g_in_replay.load() || Role() != PartyRole::Host) {
        return;
    }
    const int type = LocalChrType();
    if (type == 0 || type == 8) {
        CaptureHost(kOnReviveMagic, TravelKind::HuntersMark);
    }
}

// ---- Guest replay ----

void ClearForcedPlacement() {
    // The send-home code set +0x1520 (forced = pre-summon spot); our target must win.
    if (const u64 wts = Wts()) {
        const u8 zero = 0;
        SafeWrite(wts + kWtsForced, &zero, 1);
    }
}

/// Runs the native warp for `t`; false when there is nothing to run.
bool Replay(const TravelIntent& t, const ReplayPlan& p) {
    if (p.method == ReplayMethod::None) {
        return false;
    }
    g_in_replay = true;
    switch (p.method) {
    case ReplayMethod::LampWarp:
        ClearForcedPlacement();
        reinterpret_cast<LampWarpFn>(Guest(kLampWarp))(p.id);
        break;
    case ReplayMethod::StageWarp:
        ClearForcedPlacement();
        reinterpret_cast<StageWarpFn>(Guest(kWarpNextStage))(0, t.Area(), t.Block(), t.Region(), t.Index(),
                                                              t.warp_point);
        break;
    case ReplayMethod::BonfireWarp:
        ClearForcedPlacement();
        reinterpret_cast<BonfireWarpFn>(Guest(kWarpBonfire))(0, p.id);
        break;
    case ReplayMethod::Transform: {
        // HostDead_1's own sequence with the host's transform (travel.md 3.2).
        alignas(16) float pos[4], rot[4];
        std::memcpy(pos, t.pos, sizeof pos);
        std::memcpy(rot, t.rot, sizeof rot);
        const u32 map = t.pos_map;
        reinterpret_cast<PtrFn>(Guest(kSetForcedMap))(&map);
        reinterpret_cast<PtrFn>(Guest(kSetForcedPos))(pos);
        reinterpret_cast<PtrFn>(Guest(kSetForcedRot))(rot);
        reinterpret_cast<VoidFn>(Guest(kSetForced))();
        reinterpret_cast<SelectFn>(Guest(kSelectTarget))(0, kTravelNone);
        reinterpret_cast<FunnelFn>(Guest(kFunnel))();
        break;
    }
    case ReplayMethod::None:
        break;
    }
    g_in_replay = false;
    return true;
}

/// A stock "send home" warp at `site` on a guest: go to the party's destination instead.
std::atomic<bool> g_allow_send_home{false};
std::atomic<bool> g_own_leave_veto{false};

bool RetargetSendHome(u64 site) {
    if (g_allow_send_home.exchange(false)) {
        // B2 may still take the guest's own Leave home its own way (a title-joined guest).
        if (SendHomeVetoFn veto = g_send_home_veto.load(); veto) {
            g_own_leave_veto = true;
            const bool vetoed = veto(site);
            g_own_leave_veto = false;
            if (vetoed) {
                return true;
            }
        }
        Log("send-home at +0x%llx: the guest's Leave, home as usual", ull(site));
        return false;
    }
    // Playtest 1 (H4:1855, H4:2591): a member left the host's room with an error and the host's
    // own game ran OnLeave_Limit, the phantom's irregular-leave path. A host is in its own world:
    // its "home" load had no return data and fell back to the title (40.5 s and 59.9 s back into
    // the world). The host stays; the session handler closes the room for the member that left.
    if (g_travel_on.load() && Role() == PartyRole::Host && site == 0x138a51c) {
        Log("send-home at +0x%llx (OnLeave_Limit) on the host dropped: the host is in its own world (it went "
            "to the title in playtest 1); session role %d, cooperators %d", ull(site),
            ReadGameState(false).session_role, ReadGameState(false).cooperators);
        return true;
    }
    if (!g_travel_on.load() || g_in_replay.load() || Role() != PartyRole::Guest) {
        return false;
    }
    if (SendHomeVetoFn veto = g_send_home_veto.load(); veto && veto(site)) {
        return true; // B2: the guest stays (nothing runs)
    }
    std::optional<TravelIntent> dest;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        dest = g_guest.Destination();
    }
    if (!dest) {
        return false;
    }
    const ReplayPlan p = ChooseReplay(*dest);
    if (p.method == ReplayMethod::None) {
        Log("send-home at +0x%llx: travel %s has no usable destination; going home as usual", ull(site),
            DescribeTravel(*dest).c_str());
        return false;
    }
    Log("send-home at +0x%llx retargeted to %s via %s", ull(site), DescribeTravel(*dest).c_str(),
        ReplayMethodName(p.method));
    Replay(*dest, p);
    std::lock_guard<std::mutex> lk(g_guest_mu);
    if (g_guest.LastSeq() == dest->seq) {
        g_guest.MarkReplayed(Now());
    }
    return true;
}

/// A guest's own death (PartyGhostDeath_2) or Hunter's Mark (OnReviveMagic_1) at `site`: the
/// party's travel destination, else the host's last lamp, instead of home (phantom_limits.md 3.2).
/// Both callers already set the forced-placement byte (+0x1520 = pre-summon spot) and
/// PartyGhostDeath_2 also GameDataMan+0x70 (full recover after the next load), which stays.
bool RedirectGuest(u64 site, TravelKind kind) {
    if (!g_travel_on.load() || !g_death_redirect_on.load() || g_in_replay.load() || Role() != PartyRole::Guest) {
        return false;
    }
    // #22: in its own world (no host connected, or between sessions) a guest's Hunter's Mark /
    // death is its own, as in single player: never to the host's lamp or a stale party travel.
    if (const int sr = ReadGameState(false).session_role, lm = progress::FlagLoadMode();
        !deathrules::GuestInHostWorld(sr, lm)) {
        Log("%s at +0x%llx in the guest's own world (session role %d, load mode %d): its own, as in single player",
            TravelKindName(kind), ull(site), sr, lm);
        return false;
    }
    bool have_dest;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        have_dest = g_guest.Destination().has_value();
    }
    u32 lamp = g_death_lamp.load();
    if (!LampUsable(lamp)) {
        Log("%s at +0x%llx: the host's last lamp %u is in a chalice dungeon this game does not have",
            TravelKindName(kind), ull(site), lamp);
        lamp = kTravelNone;
    }
    const GuestRedirect how = ChooseGuestRedirect(kind, have_dest, lamp);
    bool done = false;
    GuestRedirect used = how;
    if (how == GuestRedirect::TravelDestination) {
        done = RetargetSendHome(site);
    }
    if (!done && lamp != kTravelNone && lamp != 0 && how != GuestRedirect::None) {
        used = GuestRedirect::HostLamp;
        TravelIntent t;
        t.kind = kind;
        t.lamp_id = lamp;
        Log("%s at +0x%llx: to the host's last lamp %u via %s", TravelKindName(kind), ull(site), lamp,
            ReplayMethodName(ReplayMethod::LampWarp));
        done = Replay(t, {ReplayMethod::LampWarp, lamp});
    }
    if (!done) {
        Log("%s at +0x%llx: no host lamp known and no travel pending; going home as usual", TravelKindName(kind),
            ull(site));
        return false;
    }
    if (GuestRedirectFn cb = g_redirect_cb.load()) {
        cb(kind, used, lamp);
    }
    return true;
}

// ---- Hooks ----

// Replaces 0x13CDE30 (jumped to from its entry, so the return address is the game caller's).
__attribute__((noinline)) BB_COOP_SYSV u64 FunnelHook() {
    const u64 ret = reinterpret_cast<u64>(__builtin_return_address(0));
    perf::Scope z(perf::Zone::HookFunnel);
    if (g_travel_on.load() && !g_in_replay.load()) {
        if (const u64 site = ret - Guest(0) - 5; !TravelKindBroadcast(TravelKindFromCallSite(site))) {
            // Not a travel the host broadcasts: logged so an unlisted path shows up in live runs.
            Log("funnel: %s from +0x%llx (role %s, not a party travel)", TravelKindName(TravelKindFromCallSite(site)),
                ull(site), Role() == PartyRole::Host ? "host" : Role() == PartyRole::Guest ? "guest" : "none");
        }
        if (const TravelKind k = TravelKindFromCallSite(ret - Guest(0) - 5);
            k == TravelKind::ScriptedWarp || k == TravelKind::LuaStageWarp) {
            // A scripted stage warp to a point of the loaded map: an in-world move (no loading screen).
            const u64 wts = Wts();
            if (SameMapStageFn fn = g_same_map_stage.load();
                fn && wts && fn(WtsField<u32>(wts, kWtsMap, kTravelNone), WtsField<u32>(wts, kWtsWarpPoint, kTravelNone))) {
                return 1;
            }
        }
        if (ret - Guest(0) - 5 == kMarkItemCall) {
            // Split travel: the Hunter's Mark asks "Travel with the party?" first (the answer runs
            // a decided lamp warp to the same last lamp). Consumed: no stage request now.
            const u32 lamp = static_cast<u32>(WtsField<u64>(Wts(), kWtsLastLamp, ~0ull));
            if (MarkInterceptor fn = g_mark_intercept.load(); fn && lamp != kTravelNone && fn(lamp)) {
                return 1;
            }
        }
        if (g_block_clear3.load() > 0) {
            if (RetargetSendHome(kBlockClear3Jump)) {
                return 1;
            }
        } else if (Role() == PartyRole::Host) {
            const u64 site = ret - Guest(0) - 5;
            const TravelKind kind = TravelKindFromCallSite(site);
            // OnReviveMagic_1's mark is captured at OnReviveMagic's entry (it left the session first).
            if (kind == TravelKind::HostDeath) {
                if (HostDeathInterceptFn fn = g_host_death_intercept.load(); fn && fn()) {
                    return 1;  // B2: the host is revived in place (no party move)
                }
            }
            if (TravelKindBroadcast(kind) && site != kMarkTailJump) {
                CaptureHost(site, kind);
            }
        } else if (ret - Guest(0) - 5 == kGuestDiedCall) {
            if (GuestDeathHoldFn hold = g_guest_death_hold.load(); hold && Role() == PartyRole::Guest && hold()) {
                return 1;  // B2: held (the host's travel may bring the guest along)
            }
            if (RedirectGuest(kGuestDiedCall, TravelKind::GuestDied)) {
                return 1;
            }
        } else if (ret - Guest(0) - 5 == kMarkItemCall) {
            // A guest's Hunter's Mark (the item): to the host's lamp instead of its own, as for
            // OnReviveMagic_1 (MarkTailHook). The funnel would leave the session either way.
            if (RedirectGuest(kMarkItemCall, TravelKind::HuntersMark)) {
                return 1;
            }
        }
    }
    z.End(); // the game's own transition is not ours
    return reinterpret_cast<FunnelFn>(g_funnel_orig)();
}

// The retargeted `call 0x13CDE30` at OnLeave_Limit / HostDead_1 / BlockClear2_1.
__attribute__((noinline)) BB_COOP_SYSV u64 SendHomeHook() {
    const u64 site = reinterpret_cast<u64>(__builtin_return_address(0)) - Guest(0) - 5;

    if (RetargetSendHome(site)) {
        return 1;
    }
    return reinterpret_cast<FunnelFn>(Guest(kFunnel))();
}

// OnReviveMagic_1's tail jump to the funnel (a guest's Hunter's Mark; the host's is captured at
// OnReviveMagic's entry). Entered by `jmp`, so returning returns to OnReviveMagic_1's caller.
__attribute__((noinline)) BB_COOP_SYSV u64 MarkTailHook() {
    if (RedirectGuest(kMarkTailJump, TravelKind::HuntersMark)) {
        return 1;
    }
    return reinterpret_cast<FunnelFn>(Guest(kFunnel))();
}

// BlockClear2_3 reaches the funnel by a tail jump: mark the call so FunnelHook knows it.
__attribute__((noinline)) BB_COOP_SYSV u64 BlockClear3Hook(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    g_block_clear3.fetch_add(1);
    const u64 r = reinterpret_cast<Lua6Fn>(g_block_clear3_orig)(a0, a1, a2, a3, a4, a5);
    g_block_clear3.fetch_sub(1);
    return r;
}

// ---- Data / code patches ----

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

struct PatchSite {
    u64 off;
    std::vector<u8> original;
    std::vector<u8> patched;
};

/// Writes a group of sites together: all original (write), all patched already (the XML patch
/// did it), else nothing.
void PatchGroup(const char* name, const std::vector<PatchSite>& sites) {
    bool all_orig = true, all_patched = true;
    for (const PatchSite& s : sites) {
        all_orig = all_orig && Matches(s.off, s.original.data(), s.original.size());
        all_patched = all_patched && Matches(s.off, s.patched.data(), s.patched.size());
    }
    if (all_patched) {
        Log("%s: already applied", name);
        return;
    }
    if (!all_orig) {
        for (const PatchSite& s : sites) {
            if (!Matches(s.off, s.original.data(), s.original.size()) && !Matches(s.off, s.patched.data(), s.patched.size())) {
                Log("%s: +0x%llx holds neither the 1.09 bytes nor the patch; skipped", name, ull(s.off));
            }
        }
        Log("%s: MISMATCH, not applied", name);
        return;
    }
    for (const PatchSite& s : sites) {
        if (!WriteImage(s.off, s.patched.data(), s.patched.size())) {
            Log("%s: +0x%llx could not be written", name, ull(s.off));
            return;
        }
    }
    Log("%s: applied", name);
}

bool JumpsTo(u64 off, u64 target) {
    const unsigned char* img = Image();
    if (!img || off + 5 > ImageSize() || img[off] != 0xe9) {
        return false;
    }
    std::int32_t rel;
    std::memcpy(&rel, img + off + 1, 4);
    return off + 5 + std::int64_t(rel) == target;
}

} // namespace

void InstallTravelPatches() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) {
        return;
    }
    if (!Image()) {
        Log("no image; travel off");
        return;
    }
    if (EnvOff("BB_PARTY_DREAM_GATE")) {
        Log("BB_PARTY_DREAM_GATE=0: Hunter's Dream stays closed to co-op");
    } else {
        // Area restriction table TBL1[m21][0][0] (read by the area validator 0x131D7B0 for all 14
        // bell / SOS / summon consumers): the Dream's flag 2100 (0x834), always on there -> -1
        // ("no flag"), so the Dream is no longer a restricted area (travel.md section 2).
        PatchGroup("Hunter's Dream area gate (0x47304B0 = -1)",
                   {{0x47304b0, {0x34, 0x08, 0x00, 0x00}, {0xff, 0xff, 0xff, 0xff}}});
        // Small Resonant / Sinister availability in 0x157F200: `mov bl, al` (responder search
        // result) -> `mov cl, 1`, then `jmp 0x15800C1` (the canary-checked epilogue returning cl)
        // over `mov rdi, r14; call 0x18C96D0`. The same bytes as the XML "Party: Bells anywhere".
        PatchGroup("Small Resonant Bell in the Dream (0x157F6D1 / 0x157F6D3)",
                   {{0x157f6d1, {0x88, 0xc3}, {0xb1, 0x01}},
                    {0x157f6d3, {0x4c, 0x89, 0xf7, 0xe8, 0xf5, 0x9f, 0x34, 0x00},
                     {0xe9, 0xe9, 0x09, 0x00, 0x00, 0x90, 0x90, 0x90}}});
    }
    if (EnvOff("BB_PARTY_TRAVEL")) {
        Log("BB_PARTY_TRAVEL=0: no follow travel");
        return;
    }
    // push rbp; mov rbp, rsp; push r15, r14, r13, r12, rbx; push rax (14 bytes; next is the
    // rip-relative load of the WorldTransitionState slot).
    const bool funnel = ReplacePrologue(
        kFunnel, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50},
        reinterpret_cast<const void*>(&FunnelHook), &g_funnel_orig, "travel funnel (SessionWorldTransition)");
    // push rbp; mov rbp, rsp; push r15, r14, rbx; sub rsp, 0x38 (13 bytes; then mov ebx, edi).
    ReplacePrologue(kLampWarp, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x38},
                    reinterpret_cast<const void*>(&LampWarpHook), &g_lamp_warp_orig, "travel lamp warp (0x13CDF30)");
    // push rbp; mov rbp, rsp; push r15, r14, r13, r12, rbx; push rax (14 bytes; then mov rbx, rsi).
    HookPrologue(kOnReviveMagic, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50},
                 &ReviveMagicEntry, "travel Hunter's Mark (OnReviveMagic)");
    // Guest send-home sites: `call 0x13CDE30` right after the forced-placement selector.
    if (funnel) {
        HookCallSite(0x138a51c, kFunnel, reinterpret_cast<const void*>(&SendHomeHook), "send-home OnLeave_Limit");
        HookCallSite(0x1381a93, kFunnel, reinterpret_cast<const void*>(&SendHomeHook), "send-home HostDead_1");
        HookCallSite(0x13856f4, kFunnel, reinterpret_cast<const void*>(&SendHomeHook), "send-home BlockClear2_1");
        // BlockClear2_3: push rbp; mov rbp, rsp; push rbx; push rax (6 bytes), and its funnel tail
        // jump; FunnelHook is what sees that call.
        if (JumpsTo(kBlockClear3Jump, kFunnel)) {
            ReplacePrologue(kBlockClear3, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50},
                            reinterpret_cast<const void*>(&BlockClear3Hook), &g_block_clear3_orig,
                            "send-home BlockClear2_3");
        } else {
            Log("send-home BlockClear2_3: +0x%llx is not a jmp to the funnel; not installed", ull(kBlockClear3Jump));
        }
        // Guest death: the direct call 0x1382663 reaches FunnelHook with its own return address.
        if (!CallsTo(kGuestDiedCall, kFunnel)) {
            Log("guest death redirect: +0x%llx is not a call to the funnel; off", ull(kGuestDiedCall));
        } else {
            Log("guest death redirect: PartyGhostDeath_2 call +0x%llx seen by the funnel hook", ull(kGuestDiedCall));
        }
        HookTailJump(kMarkTailJump, kFunnel, reinterpret_cast<const void*>(&MarkTailHook),
                     "guest Hunter's Mark redirect (OnReviveMagic_1)");
    } else {
        Log("no funnel hook: the host cannot report travel and send-home retargets are off");
    }
    // The guest replay needs no hook (it runs from the coop tick).
    g_travel_on = true;
}

bool PopHostTravel(TravelIntent* out) {
    std::lock_guard<std::mutex> lk(g_host_mu);
    return g_host_queue.Pop(out);
}

bool HostInOtherChalice() {
    const std::uint64_t host = g_host_chalice.load();
    return host != 0 && host != chalice::LocalChannel();
}

bool GuestSkipsChaliceTravel(const TravelIntent& t) {
    g_host_chalice = t.chalice_channel;
    // Chalice Dungeons: only into the dungeon this game has (docs/party/chalice.md).
    if (!ChaliceReplayAllowed(t, chalice::LocalChannel())) {
        Log("guest travel %s not followed: the host enters chalice dungeon %llu, this game has %llu "
            "(the summon brings it in: quick join by ChannelId)",
            DescribeTravel(t).c_str(), static_cast<unsigned long long>(t.chalice_channel),
            static_cast<unsigned long long>(chalice::LocalChannel()));
        return true;
    }
    // Into a dungeon from outside: its own copy would only be left again for the summon (two loads,
    // run c20); staying put, the summon's quick join brings it in with one load.
    if (TravelIntoChalice(t)) {
        const u32 here = g_guest_map.load();
        if (here && (here >> 24) != 29) {
            Log("guest travel %s not followed: the host enters chalice dungeon %llu from outside one; the summon "
                "brings this game in (one load)",
                DescribeTravel(t).c_str(), static_cast<unsigned long long>(t.chalice_channel));
            return true;
        }
    }
    return false;
}

void RequestGuestTravel(const TravelIntent& t) {
    if (!g_travel_on.load()) {
        return;
    }
    // Any intent with a last-lamp record also tells where the host respawns now.
    if (const u32 lamp = HostLampFromTravel(t); lamp != kTravelNone) {
        g_death_lamp = lamp;
    }
    if (GuestSkipsChaliceTravel(t)) {
        return;
    }
    // Only a member in the host's session follows (summoned, joining, or just sent home); one in
    // its prologue or alone in its own world stays (ERSC_PARITY.md B3).
    {
        const GameSnapshot g = ReadGameState(false);
        if (g.session_role == 6) {
            g_last_client.store(Now());
        }
        const TravelFollow f = FollowNow(g.session_role, Now());
        if (f != TravelFollow::Follow) {
            Log("travel #%llu ignored: %s (%s; session role %d)", static_cast<unsigned long long>(t.seq),
                TravelFollowName(f), DescribeTravel(t).c_str(), g.session_role);
            return;
        }
    }
    bool taken;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        taken = g_guest.Offer(t, Now());
    }
    if (taken) {
        g_offer_at.store(Now());
    }
    Log("guest travel %s %s", DescribeTravel(t).c_str(), taken ? "queued" : "ignored (not newer than the last)");
}

void TravelTick() {
    if (!g_travel_on.load()) {
        return;
    }
    if (Role() == PartyRole::Host) {
        // A host load no hook captured (no party travel): the stage request's fields, to find the
        // path (live: the Hunter's Mark, SpEffect 2100, did not reach the funnel's listed sites).
        static bool was_loading = false;
        const GameSnapshot h = ReadGameState(false);
        const bool loading = h.loading || !h.world_up;
        if (loading && !was_loading && Now() - g_last_capture.load() > 3.0) {
            const u64 wts = Wts();
            Log("host load without a party travel: map 0x%08x, point %d, forced %u, mode %u, respawn 0x%016llx, last lamp "
                "0x%016llx",
                WtsField<u32>(wts, kWtsMap, 0), static_cast<int>(WtsField<u32>(wts, kWtsWarpPoint, 0)),
                unsigned(WtsField<u8>(wts, kWtsForced, 0)), WtsField<u32>(wts, kWtsRespawnMode, 0),
                ull(WtsField<u64>(wts, kWtsRespawn, 0)), ull(WtsField<u64>(wts, kWtsLastLamp, 0)));
        }
        was_loading = loading;
        HostCloseEmptyRoom();
        HostHeldLampTick();
        return;
    }
    const GameSnapshot g = ReadGameState(false);
    if (g.world_up && !g.loading && g.map_id && g.map_id != 0xffffffffu) {
        g_guest_map.store(g.map_id);
    }
    if (g.session_role == 6) {
        g_last_client.store(Now());
    }
    ReplayState s;
    s.world_up = g.world_up;
    s.loading = g.loading;
    s.session_role = g.session_role;
    s.transition_requested = WtsField<u8>(Wts(), kWtsRequested, 1) != 0;
    std::optional<TravelIntent> now;
    GuestTravel::Phase before, after;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        before = g_guest.GetPhase();
        now = g_guest.Step(s, Now());
        after = g_guest.GetPhase();
    }
    const bool same_map = now && now->packed_map != kTravelNone && g.map_id && g.map_id != 0xffffffffu &&
                          (now->packed_map >> 16) == (g.map_id >> 16);
    if (now && g.session_role != 6 && (now->kind == TravelKind::HostDeath || same_map)) {
        // A guest that is not in the host's world (waiting to join, or apart) does not follow the
        // host's death, nor a host travel within the map it already stands in: no loading screen
        // for a player it does not concern (live: a joiner waiting outside a boss fight loaded when
        // the host died in it, and again for the host's lamp warp in the same m24_01). Only a
        // summoned guest moves with the host here; a joiner in another map is gathered before it
        // rings (GatherGuest).
        Log("replay %s: this guest is not in the host's world; staying", DescribeTravel(*now).c_str());
        std::lock_guard<std::mutex> lk(g_guest_mu);
        g_guest.Drop();
        now.reset();
    }
    if (now) {
        // Still a member of the host's session? (It may have been queued while it was.)
        const TravelFollow f = FollowNow(g.session_role, g_offer_at.load());
        if (f != TravelFollow::Follow) {
            Log("travel #%llu ignored: %s (%s; session role %d)", static_cast<unsigned long long>(now->seq),
                TravelFollowName(f), DescribeTravel(*now).c_str(), g.session_role);
            std::lock_guard<std::mutex> lk(g_guest_mu);
            g_guest.Drop();
            now.reset();
        }
    }
    if (now) {
        const ReplayPlan p = ChooseReplay(*now);
        if (p.method == ReplayMethod::None) {
            Log("replay %s: no usable destination; staying", DescribeTravel(*now).c_str());
        } else {
            Log("replay %s via %s (%s)", DescribeTravel(*now).c_str(), ReplayMethodName(p.method),
                Describe(g).c_str());
            Replay(*now, p);
        }
    } else if (before != after && after == GuestTravel::Phase::Idle) {
        Log(before == GuestTravel::Phase::Pending ? "travel dropped: never replayable within %.0f s"
                                                  : "travel finished (arrived, or %.0f s passed)",
            before == GuestTravel::Phase::Pending ? GuestTravel::kPendingTimeout : GuestTravel::kActiveTimeout);
    }
}

void SetLocalStartReady(bool ready) {
    g_start_ready.store(ready);
}

bool TravelEnabled() {
    return g_travel_on.load();
}

bool TravelWarpForTest(std::uint32_t map, std::uint32_t point) {
    if (!TravelMapKnown(map) || !ids::WarpPointKnown(map, point)) {
        return false;
    }
    TravelIntent t;
    t.kind = TravelKind::ScriptedWarp;
    t.packed_map = map;
    t.warp_point = point;
    Log("test: warp the player to m%02u_%02u_%02u_%02u point %u", (map >> 24) & 0xff, (map >> 16) & 0xff,
        (map >> 8) & 0xff, map & 0xff, point);
    return Replay(t, {ReplayMethod::StageWarp, kTravelNone});
}

bool TravelPlaceForTest(std::uint32_t map, const float pos[3], float yaw) {
    if (!TravelMapKnown(map)) {
        return false;
    }
    TravelIntent t;
    t.kind = TravelKind::Transform;
    t.has_pos = true;
    t.pos_map = map;
    for (int i = 0; i < 3; ++i) {
        t.pos[i] = pos[i];
    }
    t.rot[1] = yaw;
    Log("test: place the player at m%02u_%02u_%02u_%02u (%.2f, %.2f, %.2f) yaw %.2f", (map >> 24) & 0xff,
        (map >> 16) & 0xff, (map >> 8) & 0xff, map & 0xff, pos[0], pos[1], pos[2], yaw);
    return Replay(t, {ReplayMethod::Transform, kTravelNone});
}

bool HostTravelTransformNow(std::uint32_t map, const float pos[4], const float rot[4], const char* why) {
    if (!g_travel_on.load() || Role() != PartyRole::Host || !TravelMapKnown(map)) {
        return false;
    }
    const u64 wts = Wts();
    if (!wts || WtsField<u8>(wts, kWtsOwnWorld, 0) == 0 || WtsField<u8>(wts, kWtsRequested, 0) != 0) {
        return false;  // not in its own world, or a stage request is armed already
    }
    TravelIntent t;
    t.seq = NextSeq();
    t.kind = TravelKind::Transform;
    t.packed_map = map;
    t.last_lamp = WtsField<u64>(wts, kWtsLastLamp, ~0ull);
    t.has_pos = true;
    t.pos_map = map;
    for (int i = 0; i < 4; ++i) {
        t.pos[i] = pos[i];
        t.rot[i] = rot[i];
    }
    t.pos[3] = 1.0f;
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        g_host_queue.Push(t);
    }
    g_host_left_at = Now();
    g_last_capture = Now();
    Log("host travel %s -> forced transform (%s: a load where the host stands; the party follows)",
        DescribeTravel(t).c_str(), why);
    return Replay(t, {ReplayMethod::Transform, kTravelNone});
}

void SetGuestDeathHold(GuestDeathHoldFn fn) {
    g_guest_death_hold = fn;
}

void GuestDeathRedirectNow() {
    if (RedirectGuest(kGuestDiedCall, TravelKind::GuestDied)) {
        return;
    }
    Log("held guest death: no redirect; the stock funnel (home)");
    reinterpret_cast<FunnelFn>(g_funnel_orig ? g_funnel_orig : reinterpret_cast<void*>(Guest(kFunnel)))();
}

void SetHostDeathIntercept(HostDeathInterceptFn fn) {
    g_host_death_intercept = fn;
}

void HostDeathFunnelNow() {
    if (g_travel_on.load() && Role() == PartyRole::Host) {
        CaptureHost(0x1381ede, TravelKind::HostDeath);
    }
    Log("host death: the stock funnel runs now (respawn load; the party follows)");
    reinterpret_cast<FunnelFn>(g_funnel_orig ? g_funnel_orig : reinterpret_cast<void*>(Guest(kFunnel)))();
}

void AllowNextSendHome() {
    g_allow_send_home = true;
}

void SetSendHomeVeto(SendHomeVetoFn fn) {
    g_send_home_veto = fn;
}

bool SendHomeVetoForOwnLeave() {
    return g_own_leave_veto.load();
}

bool ReplayTravelNow(const TravelIntent& t) {
    if (!g_travel_on.load()) {
        return false;
    }
    const ReplayPlan p = ChooseReplay(t);
    Log("replay now %s via %s", DescribeTravel(t).c_str(), ReplayMethodName(p.method));
    return Replay(t, p);
}

void SetGuestDeathRedirect(std::uint32_t host_lamp_id) {
    const u32 was = g_death_lamp.exchange(host_lamp_id);
    if (was != host_lamp_id) {
        Log("guest death redirect: host's last lamp %d (was %d)", static_cast<int>(host_lamp_id), static_cast<int>(was));
    }
}

std::uint32_t GuestDeathRedirectLamp() {
    return g_death_lamp.load();
}

void SetLampUseObserver(LampUseObserver fn) {
    g_lamp_observer = fn;
}

void LampWarpCallers(std::uint64_t out[6]) {
    for (int i = 0; i < 6; ++i) {
        out[i] = t_lamp_callers[i];
    }
}

void SetLampWarpInterceptor(LampWarpInterceptor fn) {
    g_lamp_intercept = fn;
}

void RunLampWarp(std::uint32_t id) {
    reinterpret_cast<LampWarpFn>(Guest(kLampWarp))(id);
}

void RunLampWarpNotRest(std::uint32_t id, const char* why) {
    g_warp_not_rest = why ? why : "not a rest";
    reinterpret_cast<LampWarpFn>(Guest(kLampWarp))(id);
    g_warp_not_rest = nullptr;
}

void RunLampWarpDecided(std::uint32_t id, bool alone) {
    if (Role() == PartyRole::Host) {
        g_next_alone = alone;
    }
    g_lamp_decided = true;
    reinterpret_cast<LampWarpFn>(Guest(kLampWarp))(id);
    g_lamp_decided = false;
    g_next_alone = false;  // a warp the funnel did not capture: nothing carries over
}

void RunLampWarpPartyNotRest(std::uint32_t id, const char* why) {
    g_warp_not_rest = why ? why : "not a rest";
    RunLampWarpDecided(id, false);
    g_warp_not_rest = nullptr;
}

void SetSameMapWarp(SameMapWarpFn fn) {
    g_same_map = fn;
}

void SetSameMapStage(SameMapStageFn fn) {
    g_same_map_stage = fn;
}

void SetMarkInterceptor(MarkInterceptor fn) {
    g_mark_intercept = fn;
}

void SetNextHostTravelAlone(bool alone) {
    g_next_alone = alone;
}

void SetHostDeathAlone(bool alone) {
    g_death_alone = alone;
}

double LastAloneCaptureAgo() {
    const double at = g_last_alone_at.load();
    return at > 0 ? Now() - at : 1e9;
}

void RunLampWarpReplay(std::uint32_t id) {
    g_in_replay = true;
    ClearForcedPlacement();
    reinterpret_cast<LampWarpFn>(Guest(kLampWarp))(id);
    g_in_replay = false;
}

void SetGuestRedirectCallback(GuestRedirectFn fn) {
    g_redirect_cb = fn;
}

void SetGuestDeathRedirectEnabled(bool on) {
    g_death_redirect_on = on;
}

#endif // BB_PARTY_TRAVEL_NO_GAME

} // namespace coop
