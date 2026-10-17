// SPDX-License-Identifier: GPL-3.0-or-later
// D4b: one shared enemy drop roll for the party (see party_droproll.h).
#include "party_droproll.h"

#include "net/json.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace coop::droproll {

namespace {

std::uint64_t Mix(std::uint64_t h, std::uint64_t v) {
    // splitmix64 over the running hash and the next field.
    std::uint64_t z = h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

} // namespace

std::uint32_t DropRollSeed(std::uint64_t salt, std::uint32_t gen, const DropKey& k) {
    std::uint64_t h = Mix(0x6472'6f70'726f'6c6cull /* "droproll" */, salt);
    h = Mix(h, gen);
    h = Mix(h, k.map);
    h = Mix(h, static_cast<std::uint32_t>(k.entity));
    // The chr table slot only for an enemy without a map entity id (its MSB order is the same on
    // every game; the handle of an enemy with an id is not needed and may differ). The census key
    // (+0x3B8) is never used: it differs between games.
    h = Mix(h, k.entity > 0 ? 0u : (k.handle & 0x3fffffu));
    h = Mix(h, static_cast<std::uint32_t>(k.lot));
    return static_cast<std::uint32_t>(h ^ (h >> 32));
}

void SeedMt19937(std::uint32_t (&state)[624], std::uint32_t seed) {
    state[0] = seed;
    for (std::uint32_t i = 1; i < 624; ++i) {
        state[i] = 1812433253u * (state[i - 1] ^ (state[i - 1] >> 30)) + i;
    }
}

std::uint32_t Mt19937First(std::uint32_t seed) {
    std::uint32_t mt[624];
    SeedMt19937(mt, seed);
    // One refill (0x181E5C0 does the same for all 624 words); only word 0 is needed here.
    const std::uint32_t y0 = (mt[0] & 0x80000000u) | (mt[1] & 0x7fffffffu);
    std::uint32_t y = mt[397] ^ (y0 >> 1) ^ ((mt[1] & 1u) ? 0x9908b0dfu : 0u);
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= y >> 18;
    return y;
}

bool SharedRollFromEnv(const char* v) {
    return !(v && v[0] == '0');
}

bool HostDiscoveryFromEnv(const char* v) {
    return v && std::strcmp(v, "host") == 0;
}

std::uint32_t LotRowWeights(const LotSlot (&slots)[8], float cumulate_frac, float discovery,
                            std::uint32_t (&weights)[8]) {
    // 0x17CEAF0, in float like the game: base and cumulate points x100, blended by the drop
    // counter, x discovery for a luck slot (x1.0 otherwise), truncated, at least 1.
    std::uint32_t total = 0;
    for (int i = 0; i < 8; ++i) {
        weights[i] = 0;
        const float base = static_cast<float>(static_cast<std::uint32_t>(slots[i].base) * 100u);
        const float cum = static_cast<float>(static_cast<std::uint32_t>(slots[i].cumulate) * 100u);
        if (!(base > 0.0f || cum > 0.0f) || slots[i].taken) {
            continue;
        }
        float w = base + cumulate_frac * (cum - base);
        w = w * (slots[i].luck ? discovery : 1.0f);
        std::uint32_t u = 0;
        if (w > 0.0f) {
            u = static_cast<std::uint32_t>(static_cast<std::int64_t>(w));
            u = u < 2 ? 1 : u;
        }
        weights[i] = u;
        total += u;
    }
    return total;
}

int LotRowPick(const LotSlot (&slots)[8], float cumulate_frac, float discovery, std::uint32_t mt_output) {
    std::uint32_t w[8];
    const std::uint32_t total = LotRowWeights(slots, cumulate_frac, discovery, w);
    if (static_cast<std::int32_t>(total) <= 0) {
        return -1;
    }
    const float scaled = static_cast<float>(static_cast<std::int32_t>(total)) *
                         static_cast<float>(mt_output >> 9) * 1.1920928955078125e-07f;  // 2^-23
    std::int32_t r = static_cast<std::int32_t>(static_cast<std::uint64_t>(scaled));
    int last = -1;
    for (int i = 0; i < 8; ++i) {
        const bool listed = (slots[i].base > 0 || slots[i].cumulate > 0) && !slots[i].taken;
        if (!listed) {
            continue;
        }
        const std::int32_t wi = static_cast<std::int32_t>(w[i]);
        if (wi != 0) {
            last = i;
        }
        if (r < wi) {
            return i;
        }
        r -= wi;
    }
    return last;  // past the end (float rounding): the last slot with a weight
}

std::string DropSeedToJson(const DropSeed& s) {
    json::Value v = json::Value::make_object();
    v.set("v", 1);
    v.set("salt", json::hex(s.salt));
    v.set("gen", s.gen);
    v.set("disc", json::f32(s.discovery));
    return json::dump(v, 0);
}

bool DropSeedFromJson(const std::string& text, DropSeed* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) {
        if (error) *error = "drop_seed: " + err;
        return false;
    }
    try {
        DropSeed s;
        s.salt = json::u64(v, "salt");
        const std::uint64_t gen = json::u64(v, "gen");
        s.discovery = json::read_f32(v, "disc");
        if (s.salt == 0 || gen > 0xffffffffull) {
            throw std::runtime_error("salt 0 or generation out of range");
        }
        if (!std::isfinite(s.discovery) || (s.discovery != 0.0f && (s.discovery < 1.0f || s.discovery > 1000.0f))) {
            throw std::runtime_error("item discovery out of range");
        }
        s.gen = static_cast<std::uint32_t>(gen);
        *out = s;
        return true;
    } catch (const std::exception& e) {
        if (error) *error = std::string("drop_seed: ") + e.what();
        return false;
    }
}

bool DropRollBeforeArrival(bool world_up, bool loading, std::uint32_t map) {
    return !world_up || loading || map == 0xffffffffu || map == 0;
}

} // namespace coop::droproll

#ifndef BB_PARTY_DROPROLL_NO_GAME

#include "coop_hooks.h"
#include "game_state.h"
#include "party_balance.h"
#include "party_link.h"
#include "seamless_rules.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace coop::droproll {
namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;

constexpr u64 kDropRoll = 0x17DD0B0;     // (MapItemMan*, u32 chr handle, NpcParam row ref*)
constexpr u64 kLotRoll = 0x1A8B080;      // (request*): +0 lot, +4 float item discovery; rolls 0x17CEAF0
constexpr u64 kDropInsert = 0x17D5320;   // (MapItemMan*, drop record*): one dropped item bundle
constexpr u64 kDiscovery = 0x1581830;    // float (float stat, SpEffect list): item discovery
constexpr u64 kGsm = 0x5556678;          // GameStateMan slot; +0xA78 the game RNG (MT19937)
constexpr u64 kRngOff = 0xA78;
constexpr std::size_t kRngSize = 0x10 + 624 * 4;
constexpr u64 kWorldChrMan = 0x553E878;
constexpr u64 kGameDataMan = 0x553B130;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[640];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("Party: drops: %s\n", line);
    std::fflush(stdout);
}

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

u64 Ptr(u64 slot) {
    u64 p = 0;
    return SafeGet(Guest(slot), &p) ? p : 0;
}

using RollFn = void(BB_COOP_SYSV*)(u64, u64, u64);
using DiscoveryFn = float(BB_COOP_SYSV*)(float, u64);
RollFn g_roll_original = nullptr;
bool g_on = false, g_log_rolls = false, g_shared = true;
bool g_host_disc = false;    // BB_PARTY_DROP_DISCOVERY=host: guests roll with the host's discovery
float g_test_disc = 0;       // BB_PARTY_DROP_DISCOVERY_TEST: test aid, this game's discovery forced
i32 g_test_lot = 0;          // BB_PARTY_DROP_LOT_TEST: test aid, every enemy rolls this lot
std::atomic<int> g_mode{0};  // 0 none, 1 host, 2 guest (worldact::Tick)
std::mutex g_mu;
DropSeed g_seed;             // host: its own; guest: the host's (salt 0 = none yet)
bool g_in_drop = false;      // main thread: inside our roll wrapper
bool g_in_own_drop = false;  // main thread: inside an own (unshared) roll (C4 discovery rot only)
float g_disc_before_rot = 0; // the discovery before the C4 rot took its stacks off (0: no rot)
float g_disc_override = 0;   // the discovery written into the request (0 = the game's own)
float g_disc_used = 0;       // the discovery the request rolled with (logged)
std::string g_result;        // the items the wrapped roll dropped
int g_result_n = 0;

/// The chr of handle `h`, as 0x17DD0B0 resolves it (WorldChrMan +0x850 tables).
u64 ChrOfHandle(u32 h) {
    const u64 wcm = Ptr(kWorldChrMan);
    u64 inner = 0;
    i32 sets = 0;
    if (!wcm || h == 0xffffffffu || !SafeGet(wcm + 8, &inner) || !inner || !SafeGet(inner + 0x18, &sets)) {
        return 0;
    }
    const u32 set = (h >> 14) & 0x3f, idx = h & 0x3fff;
    u64 tab = 0, entries = 0, chr = 0;
    i32 n = 0;
    if (i32(set) >= sets + 2 || !SafeGet(wcm + 0x850 + u64(set) * 8, &tab) || !tab || !SafeGet(tab, &n) ||
        i32(idx) >= n || !SafeGet(tab + 8, &entries) || !entries || !SafeGet(entries + u64(idx) * 0x38, &chr)) {
        return 0;
    }
    return chr;
}

const char* Category(u32 bits) {
    switch (bits >> 28) {
    case 0: return "weapon";
    case 1: return "armor";
    case 2: return "rune";
    case 4: return "goods";
    case 8: return "gem";
    }
    return "item";
}

void NoteInsert(u64 rec) {
    if (!g_in_drop || !rec) {
        return;
    }
    i32 n = 0;
    if (!SafeGet(rec + 0x34, &n) || n < 0 || n > 4) {
        return;
    }
    for (i32 i = 0; i < n; ++i) {
        u32 e[4] = {};
        if (!SafeRead(rec + 0x38 + 16ull * u64(i), e, sizeof e)) {
            continue;
        }
        char b[64];
        std::snprintf(b, sizeof b, "%s %u x%u", Category(e[1]), e[1] & 0x0fffffffu, e[2]);
        g_result += (g_result.empty() ? "" : ", ") + std::string(b);
        ++g_result_n;
    }
}

BB_COOP_SYSV void DropInsertEntry(u64, u64 rec, u64, u64, u64, u64) {
    NoteInsert(rec);
}

BB_COOP_SYSV void LotRollEntry(u64 req, u64, u64, u64, u64, u64) {
    if ((!g_in_drop && !g_in_own_drop) || !req) {
        return;
    }
    float mine = 0;
    if (!SafeGet(req + 4, &mine)) {
        return;
    }
    g_disc_before_rot = 0;
    if (g_in_own_drop) {
        // C4 item discovery rot on an unshared roll: only the stacks, nothing else changes.
        const float rot = coop::balance::RotDropDiscovery(mine);
        if (rot != mine) {
            g_disc_before_rot = mine;
            SafeWrite(req + 4, &rot, sizeof rot);
        }
        g_disc_used = rot;
        return;
    }
    if (g_test_lot > 0) {
        SafeWrite(req, &g_test_lot, sizeof g_test_lot);
    }
    if (g_disc_override > 0.0f && mine != g_disc_override) {
        SafeWrite(req + 4, &g_disc_override, sizeof g_disc_override);
        mine = g_disc_override;
    }
    // C4 item discovery rot: this player's stacks come off its own roll (after the test aid / the
    // host's value, so a forced discovery shows the rot too).
    if (const float rot = coop::balance::RotDropDiscovery(mine); rot != mine) {
        g_disc_before_rot = mine;
        SafeWrite(req + 4, &rot, sizeof rot);
        mine = rot;
    }
    g_disc_used = mine;
}

/// The first row of `lot` as the roll sees it (flags not checked: logged prediction only).
bool ReadLotRow(i32 lot, LotSlot (&slots)[8]) {
    const u64 row = lot > 0 ? GameParamRow("ItemLotParam", "ITEMLOT_PARAM_ST", static_cast<u32>(lot)) : 0;
    std::uint16_t bits = 0;
    if (!row || !SafeGet(row + 0x92, &bits)) {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        slots[i] = LotSlot{};
        if (!SafeGet(row + 0x40 + 2u * i, &slots[i].base) || !SafeGet(row + 0x50 + 2u * i, &slots[i].cumulate)) {
            return false;
        }
        slots[i].luck = (bits >> i) & 1u;
    }
    return true;
}

BB_COOP_SYSV void DropRollReplace(u64 mim, u64 handle_arg, u64 npc) {
    const u32 handle = u32(handle_arg);
    const int mode = g_mode.load();
    DropKey k;
    k.handle = handle;
    u64 row = 0;
    if (npc && SafeGet(npc + 8, &row) && row) {
        SafeGet(row + 0x2c, &k.lot);
    }
    DropSeed seed;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        seed = g_seed;
    }
    const u64 gsm = Ptr(kGsm);
    const bool guest_ok = mode == 2 && LocalSessionRole() == 6;
    if (guest_ok && g_on && g_shared) {
        // dropdisc (live): an enemy the host killed while this guest was still loading in (title
        // join) dies again on the guest's game as its world is built; that roll read map "-" and
        // so another seed than the host's. The kill was before this player was there: no drop on
        // this game (a joiner in vanilla gets nothing from enemies already dead).
        const GameSnapshot gs = ReadGameState(false);
        if (DropRollBeforeArrival(gs.world_up, gs.loading, gs.map_id)) {
            Log("enemy drop roll on this game (role 6): chr handle %#x, lot %d: killed before this guest's world "
                "was up (map %s): no drop here", handle, k.lot, MapName(gs.map_id).c_str());
            return;
        }
    }
    const bool shared = g_on && g_shared && gsm && seed.salt != 0 && k.lot > 0 && (mode == 1 || guest_ok);
    if (!shared) {
        g_disc_used = 0;
        g_in_own_drop = true;
        g_roll_original(mim, handle_arg, npc);
        g_in_own_drop = false;
        if (g_log_rolls || g_disc_before_rot > 0.0f) {
            char rot[96] = "";
            if (g_disc_before_rot > 0.0f) {
                std::snprintf(rot, sizeof rot, ", item discovery %.2f (rot: %.2f before)", double(g_disc_used),
                              double(g_disc_before_rot));
            }
            Log("enemy drop roll on this game (role %d): chr handle %#x, lot %d, own roll%s%s", LocalSessionRole(),
                handle, k.lot, g_shared ? (seed.salt ? "" : " (no host seed yet)") : " (shared roll off)", rot);
        }
        return;
    }
    if (const u64 chr = ChrOfHandle(handle)) {
        SafeGet(chr + 0x1f0, &k.entity);
        SafeGet(chr + 0x3b8, &k.entity2);
    }
    k.map = ReadGameState(false).map_id;
    const u32 s = DropRollSeed(seed.salt, seed.gen, k);
    // The game RNG: saved, seeded for this enemy, restored after the roll.
    static unsigned char saved[kRngSize];
    const u64 rng = gsm + kRngOff;
    if (!SafeRead(rng, saved, kRngSize)) {
        g_roll_original(mim, handle_arg, npc);
        return;
    }
    u32 state[624];
    SeedMt19937(state, s);
    const u32 left = 1;  // the next draw refills from `state` (0x181E5C0) and reads word 0
    SafeWrite(rng + 0x10, state, sizeof state);
    SafeWrite(rng + 4, &left, sizeof left);
    g_in_drop = true;
    // Item discovery: each game its own player's (default), the host's with
    // BB_PARTY_DROP_DISCOVERY=host, or the test aid's.
    const bool host_disc = g_host_disc && mode == 2 && seed.discovery > 0.0f;
    g_disc_override = host_disc ? seed.discovery : g_test_disc;
    g_disc_used = 0;
    g_result.clear();
    g_result_n = 0;
    g_roll_original(mim, handle_arg, npc);
    g_in_drop = false;
    SafeWrite(rng, saved, kRngSize);
    // The first row's pick (the first draw of the seeded RNG), at this game's discovery and at
    // the host's: the same dice, each player's own discovery.
    const u32 draw = Mt19937First(s);
    char pick[160] = "";
    LotSlot lot_row[8];
    if (g_disc_used > 0.0f && ReadLotRow(g_test_lot > 0 ? g_test_lot : k.lot, lot_row)) {
        const int own = LotRowPick(lot_row, 0.0f, g_disc_used, draw);
        if (seed.discovery > 0.0f && seed.discovery != g_disc_used) {
            std::snprintf(pick, sizeof pick, "; row 1 slot %d (at the host's %.2f: slot %d)", own + 1,
                          double(seed.discovery), LotRowPick(lot_row, 0.0f, seed.discovery, draw) + 1);
        } else {
            std::snprintf(pick, sizeof pick, "; row 1 slot %d", own + 1);
        }
    }
    Log("shared drop roll (role %d): map %s entity %d (key %d) lot %d gen %u -> seed %08x, draw %.4f, item "
        "discovery %.2f (%s%s)%s%s: %s",
        LocalSessionRole(), MapName(k.map).c_str(), k.entity, k.entity2, k.lot, seed.gen, s,
        double(draw >> 9) / 8388608.0, double(g_disc_used),
        host_disc ? "host's" : g_test_disc > 0.0f ? "test aid" : "own",
        g_disc_before_rot > 0.0f ? (std::string(", rot: ") + std::to_string(g_disc_before_rot).substr(0, 4) + " before").c_str() : "",
        g_test_lot > 0 ? " (test lot)" : "", pick,
        g_result_n ? g_result.c_str() : "nothing dropped");
}

float HostDiscovery() {
    const u64 gdm = Ptr(kGameDataMan);
    u64 pgd = 0, list = 0;
    i32 stat = 0;
    if (!gdm || !SafeGet(gdm + 8, &pgd) || !pgd || !SafeGet(pgd + 0x70, &stat) || !SafeGet(pgd + 0x670, &list)) {
        return 0.0f;
    }
    const float d = reinterpret_cast<DiscoveryFn>(Guest(kDiscovery))(float(stat), list);
    return std::isfinite(d) && d >= 1.0f && d <= 1000.0f ? d : 0.0f;
}

} // namespace

void Init(bool log_rolls) {
    g_log_rolls = log_rolls;
    g_shared = SharedRollFromEnv(std::getenv("BB_PARTY_SHARED_DROP_ROLL"));
    g_host_disc = HostDiscoveryFromEnv(std::getenv("BB_PARTY_DROP_DISCOVERY"));
    if (const char* t = std::getenv("BB_PARTY_DROP_DISCOVERY_TEST")) {
        const float v = std::strtof(t, nullptr);
        g_test_disc = std::isfinite(v) && v > 0.0f && v <= 100.0f ? v : 0.0f;
    }
    if (const char* t = std::getenv("BB_PARTY_DROP_LOT_TEST")) {
        g_test_lot = std::atoi(t);
    }
    const std::initializer_list<u8> pro = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
    if (!g_shared && !log_rolls) {
        Log("shared enemy drop roll off (BB_PARTY_SHARED_DROP_ROLL=0): every player rolls its own");
        return;
    }
    void* orig = nullptr;
    if (!ReplacePrologue(kDropRoll, pro, reinterpret_cast<const void*>(&DropRollReplace), &orig, "enemy drop roll")) {
        Log("enemy drop roll hook NOT installed: every player rolls its own");
        return;
    }
    g_roll_original = reinterpret_cast<RollFn>(orig);
    if (g_shared) {
        HookPrologue(kLotRoll, pro, &LotRollEntry, "lot roll request (shared drop discovery)");
        if (!std::getenv("BB_PARTY_WORLDACT_TEST")) {  // the fixture hooks the insert itself
            HookPrologue(kDropInsert, pro, &DropInsertEntry, "drop insert (shared drop log)");
        }
    }
    g_on = true;
    Log("shared enemy drop roll %s (one roll per enemy kill for the whole party; BB_PARTY_SHARED_DROP_ROLL=0: "
        "own roll per player), item discovery %s%s", g_shared ? "on" : "off",
        g_host_disc ? "the host's (BB_PARTY_DROP_DISCOVERY=host)" : "per player",
        g_test_disc > 0.0f ? " (test aid: this game's forced)" : "");
}

bool Enabled() {
    return g_on && g_shared;
}

void Tick(const GameSnapshot& s, party::PartyLink* link, bool host) {
    g_mode = host ? 1 : (s.session_role == 6 || LocalSessionRole() == 6) ? 2 : 0;
    if (!g_on || !g_shared || !host) {
        return;
    }
    static u64 salt = 0;
    static u32 gen = 0;
    static bool was_steady = false;
    static int last_coop = -1;
    static double last_disc = 0, last_sent = 0;
    static float disc = 0;
    static DropSeed sent;
    if (!salt) {
        salt = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
               (reinterpret_cast<std::uintptr_t>(&salt) << 16) ^ 0x5bd1e995ull;
        salt = salt ? salt : 1;
    }
    const bool steady = s.world_up && !s.loading;
    if (steady && !was_steady) {
        ++gen;  // a load ended: the world's enemies respawned
    }
    was_steady = steady;
    const double now = Now();
    if (steady && now - last_disc >= 2.0) {
        last_disc = now;
        disc = HostDiscovery();
    }
    DropSeed cur;
    cur.salt = salt;
    cur.gen = gen;
    cur.discovery = disc;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_seed = cur;
    }
    const bool joined = s.cooperators > last_coop && s.cooperators > 0;
    if (s.cooperators >= 0) {
        last_coop = s.cooperators;
    }
    if (!link || gen == 0) {
        return;
    }
    if (!(cur == sent) || joined || now - last_sent >= 60.0) {
        const bool news = cur.gen != sent.gen || cur.salt != sent.salt || joined;
        sent = cur;
        last_sent = now;
        link->post_event(party::kBroadcast, kEventName, DropSeedToJson(cur));
        if (news) {
            Log("seed sent: generation %u, item discovery %.1f", cur.gen, double(cur.discovery));
        }
    }
}

bool OnEvent(const std::string& body, std::string* why) {
    DropSeed s;
    if (!DropSeedFromJson(body, &s, why)) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (s.salt != g_seed.salt || s.gen != g_seed.gen) {
        Log("host seed: generation %u, item discovery %.1f", s.gen, double(s.discovery));
    }
    g_seed = s;
    return true;
}

void FixtureInsert(u64 rec) {
    NoteInsert(rec);
}

} // namespace coop::droproll

#endif // BB_PARTY_DROPROLL_NO_GAME
