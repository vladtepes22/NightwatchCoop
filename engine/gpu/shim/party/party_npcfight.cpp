// SPDX-License-Identifier: GPL-3.0-or-later
// ERSC parity D11: scripted NPC hunter fights with the whole party (party_npcfight.h; the rules in
// party_npcfight_rules.h). Offsets are ours (raw ELF VA, 1.09):
//   0x13c97a0 GetChrByEntityId(entity, entity, 0)
//   chr vfunc +0x3f0 ApplySpEffect(chr, id, chr, 0, 1, 0, 0, 1.0 x5) (party_director / party_balance)
//   0x17e1d90 RemoveSpEffect(list, node, 1) (EMEVD 2004[21]); ChrIns +0x1c8 SpEffect list (+8 first
//             node; node +0x40 id, +0x58 next)
//   ChrIns +0x1e1 (u32) bit 14 EnableInvincible (the director's `god`, the A11 fixture)
#include "party_npcfight.h"

#include "party_npcfight_rules.h"

#include "../bbport_settings.h"
#include "coop_hooks.h"
#include "game_state.h"
#include "party_status.h"
#include "seamless_rules.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <iterator>

namespace coop::npcfight {
namespace {

using u64 = std::uint64_t;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("[coop] Party NPC fights: %s\n", line);
    std::fflush(stdout);
}

constexpr int kRoleClient = 6;

/// The rule in force on this game: the host's own setting, a guest's copy of the host's.
bool RuleOn(bool client) {
    return client ? party::status::HostRuleOn(party::status::HostRule::NpcFights)
                  : BbSettings::Get().party_npc_fights.load(std::memory_order_relaxed);
}

// Logged once per (event, what) pair change; a busy map runs these events every load.
std::atomic<std::uint32_t> g_passed_logs{0}, g_skipped_logs{0};
std::atomic<std::int32_t> g_last_passed{0}, g_last_restored{0};
std::atomic<std::uint32_t> g_team_logs{0};
std::atomic<std::int32_t> g_last_team{0};

// 2004[2] of a TeamEvent: team 27 is no foe for a phantom (party_npcfight_rules.h); a guest's copy
// writes 23 into the argument, any other game's dispatch puts the data's 27 back.
bool TeamSwitch(int event_id, int index, Act act, u64 event, u64 instr) {
    std::uint32_t size = 0;
    const u64 args = EmevdInstructionArgs(event, instr, &size);
    std::uint8_t have = 0;
    if (!args || size <= std::uint32_t(kTeamArg) || !SafeGet(args + kTeamArg, &have)) return false;
    const std::uint8_t want = TeamFor(act, have);
    if (want == have || !SafeWrite(args + kTeamArg, &want, 1)) return false;
    const std::int32_t key = act == Act::foe_team ? event_id : -event_id;
    if (g_last_team.exchange(key) != key && g_team_logs.fetch_add(1) < 32) {
        std::int32_t entity = 0;
        SafeGet(args, &entity);
        Log("event %d's team switch at %d (entity %d): team %u -> %u (%s)", event_id, index, entity, unsigned(have),
            unsigned(want),
            act == Act::foe_team ? "a guest's copy: a foe the phantom can lock on to and hit"
                                 : "the game's own again");
    }
    return false;
}

// The dispatching thread, before the game reads the instruction's arguments. The gate's argument
// bytes are the loaded script's (a later run, a role change or the rule going off must see the
// data's own value again), so every dispatch of the gate writes the value for now.
bool Predicate(int event_id, int bank, int id, u64 event, u64 instr) {
    if (!TeamEvent(event_id)) return false;
    std::int32_t index = -1;
    SafeGet(event + 0xa0, &index);
    const bool client = LocalSessionRole() == kRoleClient;
    const Act act = Decide(event_id, index, bank, id, client, client && RuleOn(true));
    if (act == Act::none) return false;
    if (act == Act::foe_team || act == Act::restore_team) return TeamSwitch(event_id, index, act, event, instr);
    const GuestRun* g = GuestRunFor(event_id);
    if (!g) return false;
    if (act == Act::skip) {
        if (g_skipped_logs.fetch_add(1) < 24)
            Log("guest copy of event %d (%s): %d[%02d] at %d skipped (the host's copy writes the progress)", event_id,
                g->who, bank, id, index);
        return true;
    }
    std::uint32_t size = 0;
    const u64 args = EmevdInstructionArgs(event, instr, &size);
    const bool chr_type = g->gate == Gate::ChrTypeHost;
    const std::uint32_t at = chr_type ? kChrTypeArg : kMpStateArg;
    if (!args || size <= at) return false;
    std::uint8_t want = 0;
    if (act == Act::pass_gate) {
        // 4[3]: the guest's own chr type (a summoned phantom: 1; read live, as the Doll's rewriter does);
        // 1003[6]: a state the game never reports, so "end if client" does not end it.
        std::uint8_t mine = 1;
        u64 wcm = 0, player = 0;
        if (SafeGet(Guest(0x553e878), &wcm) && wcm && SafeGet(wcm + 0x60, &player) && player) {
            std::int32_t t = -1;
            if (SafeGet(player + 0x78, &t) && t >= 0 && t < 0x7f) mine = std::uint8_t(t);
        }
        want = chr_type ? mine : kNoState;
    } else {
        want = chr_type ? 0 : kClientState;  // the data's own (every GuestRun gate holds these)
    }
    std::uint8_t have = 0;
    if (!SafeGet(args + at, &have) || have == want) return false;
    if (!SafeWrite(args + at, &want, 1)) return false;
    if (act == Act::pass_gate) {
        if (g_last_passed.exchange(event_id) != event_id && g_passed_logs.fetch_add(1) < 32)
            Log("guest copy of event %d runs as the host's: %s (%s %u -> %u at %d)", event_id, g->who,
                chr_type ? "4[3] chr type" : "1003[6] state", unsigned(have), unsigned(want), index);
    } else if (g_last_restored.exchange(event_id) != event_id) {
        Log("event %d's gate back to the game's own (%s): %u -> %u", event_id, client ? "the rule is off" : "own world",
            unsigned(have), unsigned(want));
    }
    return false;
}

// ---- assassins (main thread) ----

u64 ChrByEntity(std::uint32_t entity) {
    using Fn = u64(BB_COOP_SYSV*)(int, u64, u64);
    return reinterpret_cast<Fn>(Guest(0x13c97a0))(int(entity), u64(entity), 0);
}

u64 FindSpEffect(u64 chr, int id, u64* list_out) {
    u64 list = 0, node = 0;
    if (!chr || !SafeGet(chr + 0x1c8, &list) || !list || !SafeGet(list + 8, &node)) return 0;
    for (int guard = 0; node && guard < 512; ++guard) {
        std::int32_t nid = 0;
        if (!SafeGet(node + 0x40, &nid)) return 0;
        if (nid == id) {
            if (list_out) *list_out = list;
            return node;
        }
        if (!SafeGet(node + 0x58, &node)) return 0;
    }
    return 0;
}

void AddSpEffect(u64 chr, int id) {
    using Fn = void(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64, u64, float, float, float, float, float);
    u64 vt = 0, fn = 0;
    if (chr && SafeGet(chr, &vt) && vt && SafeGet(vt + 0x3f0, &fn) && fn)
        reinterpret_cast<Fn>(fn)(chr, u64(id), chr, 0, 1, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
}

void DropSpEffect(u64 chr, int id) {
    u64 list = 0;
    if (const u64 node = FindSpEffect(chr, id, &list)) {
        using Fn = void(BB_COOP_SYSV*)(u64, u64, std::uint32_t);
        reinterpret_cast<Fn>(Guest(0x17e1d90))(list, node, 1);
    }
}

void SetInvincible(u64 chr, bool on) {
    std::uint32_t f = 0;
    if (chr && SafeRead(chr + 0x1e1, &f, 4)) {
        f = on ? (f | (1u << 14)) : (f & ~(1u << 14));
        SafeWrite(chr + 0x1e1, &f, 4);
    }
}


// ---- bell helpers and the boss fog (main thread, the host's game) ----

struct FogState {
    double since[std::size(kFogHelpers)] = {};  ///< seconds on the clock when the host-entered flag was first seen on
    bool seen[std::size(kFogHelpers)] = {};
    bool handled[std::size(kFogHelpers)] = {};
    std::uint32_t map = 0xffffffffu;
};

bool ChrPos(u64 chr, float pos[4], float rot[4]) {
    u64 a = 0, b = 0, c = 0, phys = 0;
    if (!SafeGet(chr + 0x58, &a) || !a || !SafeGet(a + 8, &b) || !b || !SafeGet(b + 0x3b0, &c) || !c ||
        !SafeGet(c + 0x68, &phys) || !phys)
        return false;
    return SafeRead(phys + 0x1e0, pos, 16) && SafeRead(phys + 0x1d0, rot, 16);
}

bool ChrAlive(u64 chr) {
    u64 a = 0, b = 0, vis = 0;
    std::int32_t hp = 0;
    std::uint8_t off = 0;
    if (!SafeGet(chr + 0x3b0, &a) || !a || !SafeGet(a + 0x20, &b) || !b || !SafeGet(b + 0xf8, &hp)) return false;
    if (!SafeGet(chr + 0x18, &vis) || !vis || !SafeGet(vis + 0x20, &off)) return false;
    return hp > 0 && (off & 1) == 0;
}

/// The chr's own warp (vfunc +0x5f8, the bare warp the director uses for the player: 0x18CD600).
bool ChrWarp(u64 chr, const float pos3[3], float yaw_rad) {
    alignas(16) float pos[4] = {pos3[0], pos3[1], pos3[2], 1.0f};
    alignas(16) float rot[4] = {0, yaw_rad, 0, 0};
    u64 vt = 0, fn = 0;
    if (!chr || !SafeGet(chr, &vt) || !vt || !SafeGet(vt + 0x5f8, &fn) || !fn) return false;
    using WarpFn = void(BB_COOP_SYSV*)(u64, const float*, const float*, std::uint64_t);
    reinterpret_cast<WarpFn>(fn)(chr, pos, rot, 1);
    return true;
}

void FogFollowTick(const GameSnapshot& s, double now) {
    static FogState st;
    if (s.loading || !s.world_up || s.map_id != st.map) {
        st = FogState{};
        st.map = s.map_id;
        if (s.loading || !s.world_up) return;
    }
    const bool on = RuleOn(false);
    for (std::size_t i = 0; i < std::size(kFogHelpers); ++i) {
        const FogHelper& h = kFogHelpers[i];
        if (h.npc / 100000 != (s.map_id >> 24) || (h.npc / 10000) % 10 != ((s.map_id >> 16) & 0xff)) continue;
        bool fight = false, joined = false, left = false;
        ReadEventFlag(h.host_flag, &fight);
        ReadEventFlag(h.join, &joined);
        ReadEventFlag(h.leave, &left);
        if (!fight) {
            st.seen[i] = st.handled[i] = false;  // the fight is over (or a load put the flag back)
            continue;
        }
        if (!st.seen[i]) {
            st.seen[i] = true;
            st.since[i] = now;
        }
        if (!joined || left || st.handled[i]) continue;
        const u64 chr = ChrByEntity(h.npc);
        float pos[4] = {}, rot[4] = {};
        const bool alive = chr && ChrAlive(chr) && ChrPos(chr, pos, rot);
        float dist = 0.0f;
        if (alive) {
            const float dx = pos[0] - h.x, dy = pos[1] - h.y, dz = pos[2] - h.z;
            dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        switch (PlanFogFollow(on, fight, joined, left, alive, now - st.since[i], dist, st.handled[i])) {
            case FogFollow::done:
                st.handled[i] = true;
                Log("bell helper %u is in the arena (%.1f m from the entry spot): nothing to do", h.npc, dist);
                break;
            case FogFollow::warp: {
                const float to[3] = {h.x, h.y, h.z};
                const bool ok = ChrWarp(chr, to, h.yaw * 3.14159265f / 180.0f);
                st.handled[i] = true;
                Log("bell helper %u is still outside the boss room %.0f s after the fight began (%.1f m from the "
                    "entry spot, region %u; its fog walk needs the host at the fog): put at (%.1f, %.1f, %.1f)%s",
                    h.npc, now - st.since[i], dist, h.region, h.x, h.y, h.z, ok ? "" : " - the warp is not available");
                break;
            }
            default:
                break;
        }
    }
}

struct TickState {
    bool shown[std::size(kAssassins)] = {};
    std::uint32_t map = 0xffffffffu;
};

}  // namespace

void Init() {
    SetEmevdSkipPredicate2(&Predicate);
    EnsureEmevdFilter();
    Log("on: %zu host-only NPC hunter transition(s) a guest's copy may run, %zu assassin(s); the host's rule "
        "party_npc_fights %s here", std::size(kGuestRuns), std::size(kAssassins),
        BbSettings::Get().party_npc_fights.load() ? "on" : "off");
}

void Tick(const GameSnapshot& s, bool host) {
    static TickState st;
    if (s.loading || !s.world_up || s.map_id != st.map) {
        for (bool& b : st.shown) b = false;  // a load rebuilds the chrs (the events dress them again)
        st.map = s.map_id;
        if (s.loading || !s.world_up) return;
    }
    if (host) {
        if (!s.loading && s.world_up) {
            using Clock = std::chrono::steady_clock;
            FogFollowTick(s, std::chrono::duration<double>(Clock::now().time_since_epoch()).count());
        }
        return;
    }
    if (s.session_role != kRoleClient) return;
    const bool on = RuleOn(true);
    for (std::size_t i = 0; i < std::size(kAssassins); ++i) {
        const Assassin& a = kAssassins[i];
        if (a.entity / 100000 != (s.map_id >> 24) || (a.entity / 10000) % 10 != ((s.map_id >> 16) & 0xff))
            continue;  // not this map's (entity AABnnnn: area AA, block B)
        const u64 chr = ChrByEntity(a.entity);
        if (!chr) continue;
        bool appeared = false, done = false;
        ReadEventFlag(a.appeared, &appeared);
        ReadEventFlag(a.done, &done);
        const bool hidden = FindSpEffect(chr, kHiddenEffect, nullptr) != 0;
        const AssassinPlan p = PlanAssassin(on, appeared, done, hidden, st.shown[i]);
        if (p.show) {
            DropSpEffect(chr, kHiddenEffect);
            AddSpEffect(chr, kShownEffect);
            SetInvincible(chr, false);
            st.shown[i] = true;
            Log("%s (entity %u) appeared in the host's world (flag %u): shown here, it can be fought", a.who, a.entity,
                a.appeared);
        } else if (p.hide) {
            DropSpEffect(chr, kShownEffect);
            AddSpEffect(chr, kHiddenEffect);
            SetInvincible(chr, true);
            st.shown[i] = false;
            Log("%s (entity %u) left the host's world (flag %u off%s): hidden here again", a.who, a.entity, a.appeared,
                on ? "" : ", or the rule went off");
        }
    }
}

}  // namespace coop::npcfight
