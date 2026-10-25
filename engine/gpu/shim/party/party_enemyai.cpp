// SPDX-License-Identifier: GPL-3.0-or-later
// See party_enemyai.h. Offsets are ours (raw ELF VA, 1.09):
//   0x18E6540 NpcIns::SetManipulator(chr, force_local): +0x60 (active manipulator) := +0x3B8
//             (ComManipulator, own AI) or +0x3C0 (NetAIManipulator, the owner's AI replayed)
//   NpcIns +0x3B8 Com, +0x3C0 NetAI, +0x60 current, +0x90 handle, +0x1F0 entity id
#include "party_enemyai.h"

#include "party_enemyai_rules.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_b2.h"
#include "party_balance.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace coop::enemyai {
namespace {

using u64 = std::uint64_t;

constexpr u64 kSetManipulator = 0x18e6540;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("[coop] Party enemy AI: %s\n", line);
    std::fflush(stdout);
}

template <class T>
T Field(u64 base, u64 off, T def = T{}) {
    T v{};
    return base && SafeGet(base + off, &v) ? v : def;
}

bool EnvOn() {
    static const bool on = !(std::getenv("BB_PARTY_LONE_AI") && std::getenv("BB_PARTY_LONE_AI")[0] == '0');
    return on;
}

using SetManipFn = void(BB_COOP_SYSV*)(u64 chr, u64 force_local);
SetManipFn g_orig = nullptr;
std::atomic<bool> g_own{false};      // this game runs the AI of its enemies now (main-thread tick)
std::atomic<int> g_forced{0};        // SetManipulator calls the hook turned into a local choice

void BB_COOP_SYSV SetManipHook(u64 chr, u64 force_local) {
    if (!(force_local & 0xff) && g_own.load(std::memory_order_relaxed)) {
        force_local = 1;
        g_forced.fetch_add(1, std::memory_order_relaxed);
    }
    g_orig(chr, force_local);
}

/// The enemies this game switched to the local manipulator since it took the AI over, by chr
/// pointer (main thread). Cleared by a load: the chrs are freed.
std::set<u64> g_flipped;
bool g_owning = false;

}  // namespace

void Init() {
    if (!EnvOn()) {
        Log("off (BB_PARTY_LONE_AI=0)");
        return;
    }
    void* orig = nullptr;
    if (ReplacePrologue(kSetManipulator, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, reinterpret_cast<const void*>(&SetManipHook),
                        &orig, "enemy AI: NpcIns::SetManipulator (lone guest)")) {
        g_orig = reinterpret_cast<SetManipFn>(orig);
    }
}

void Tick(const GameSnapshot& s, bool guest) {
    if (!g_orig) return;
    const bool in_world = s.world_up && !s.loading && s.player;
    if (!in_world) {
        if (s.loading && (g_owning || !g_flipped.empty())) {
            g_flipped.clear();
            g_owning = enemyai_rules::OwningAfterLoad(g_owning);
            g_own = false;
        }
        return;
    }
    enemyai_rules::View v;
    v.enabled = true;
    v.client = guest && s.session_role == RoleClient;
    v.world_up = true;
    v.host_apart = v.client && b2::MemberApart(0);
    v.members_apart = b2::MembersApart();
    bool wants = enemyai_rules::GuestRunsAi(v);
    // The host's vitals older than 5 s (a link outage of a few seconds) say nothing: keep what this game does
    // instead of handing the enemies back (they would freeze) and taking them again.
    if (v.client && g_owning && b2::MemberMap(0) == 0) wants = true;
    static int last_forced = 0;
    const enemyai_rules::Step step = enemyai_rules::Next(g_owning, wants);
    if (step == enemyai_rules::Step::Keep && !wants) return;
    g_own = wants;
    // every second while owning (new enemies stream in; the hook covers the game's own calls)
    static std::uint64_t tick = 0;
    ++tick;
    if (step == enemyai_rules::Step::Keep && (tick % 30) != 0) return;
    int flipped = 0, back = 0;
    const auto list = balance::LoadedEnemies();
    if (step == enemyai_rules::Step::HandBack) {
        g_owning = false;
        for (const auto& e : list) {
            if (g_flipped.count(e.chr)) {
                g_orig(e.chr, 0);
                ++back;
            }
        }
        g_flipped.clear();
        Log("the host is in this area again: %d enemies handed back to the host's AI (%s)", back,
            MapName(s.map_id).c_str());
        return;
    }
    for (const auto& e : list) {
        if (!e.alive) continue;
        const u64 cur = Field<u64>(e.chr, 0x60), com = Field<u64>(e.chr, 0x3b8);
        if (!com || cur == com) continue;
        g_orig(e.chr, 1);
        g_flipped.insert(e.chr);
        ++flipped;
    }
    // the game asks for the net manipulator for every enemy every frame; the count only goes in the
    // line at a take-over and then about once a minute
    const int forced = g_forced.load();
    static std::uint64_t last_line = 0;
    const bool due = tick - last_line >= 1800;
    if (step == enemyai_rules::Step::Take || flipped > 0 || due) {
        last_line = tick;
        Log("%s in %s while the host is in another area: %d enemies switched to this game's AI now, the game chose the "
            "host's for %d more since the last line (%zu of %zu loaded enemies on this game's AI)",
            step == enemyai_rules::Step::Take ? "a lone guest takes the enemies' AI" : "enemies of a lone guest", MapName(s.map_id).c_str(),
            flipped, forced - last_forced, g_flipped.size(), list.size());
        last_forced = forced;
    }
    g_owning = true;
}

}  // namespace coop::enemyai
