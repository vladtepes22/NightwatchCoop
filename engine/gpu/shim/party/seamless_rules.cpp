// SPDX-License-Identifier: GPL-3.0-or-later
// Seamless party rules (see seamless_rules.h).
//
// Sources: the bell / boss / map-reload byte patches follow Wozzardman/shadp2p
// (GPL-2.0-or-later; documents/bloodborne-seamless-re.md, src/core/bloodborne_re.cpp,
// InstallSeamlessCoopPatches). The param repository walk follows droogie/bbhost
// (GPL-3.0-or-later) src/engine/params.cpp @8f2746c; row layouts from its
// include/bbhost/params.hpp, the EMEVD event layout from include/bbhost/engine/sprj/emk_system.hpp.
// Every offset was checked against our 1.09 eboot (docs/party/seamless_rules.md).
#include <algorithm>
#include "seamless_rules.h"
#include "party_async_io.h"
#include "party_perf.h"

#include "../../party_env.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_guestsvc.h"
#include "party_npcghost.h"
#include "party_death_rules.h"
#include "party_phantom.h"
#include "party_levelsync_rules.h"
#include "party_status.h"
#include "party_split.h"
#include "party_status.h"
#include "party_travel.h"
#include "party_story.h"
#include "party_worldact.h"
#include "../net/from_api.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using Clock = std::chrono::steady_clock;
using ull = unsigned long long;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    perf::Scope z(perf::Zone::Log);
    coop::asyncio::Print(std::string("Party seamless: ") + line + "\n"); // never blocks the caller (party_async_io.h)
}

bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0' && !v[1];
}

bool EnvOn(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && !(v[0] == '0' && !v[1]);
}

// ---- The XML party patches: what the image holds at each site ----

struct Site {
    const char* patch;
    u64 off;
    const char* original;
    const char* patched;
};

// Our offsets (XML Address - 0x400000), 1.09 original and patched bytes (patches/Bloodborne.xml).
constexpr Site kSites[] = {
    {"Party: Bells anywhere", 0x157f8c8, "0f8790000000", "0f8f90000000"},
    {"Party: Bells anywhere", 0x157f960, "3401", "31c0"},
    {"Party: Bells anywhere", 0x157f6d1, "88c3", "b101"},
    {"Party: Bells anywhere", 0x157f6d3, "4c89f7e8f59f3400", "e9e9090000909090"},
    {"Party: Bells anywhere", 0x15068bb, "774a", "7f4a"},
    {"Party: Bells anywhere", 0x15068fc, "88c380f301", "b301909090"},
    {"Party: Bells anywhere", 0x191a8c3, "18c920c1eb02", "7d0488c1eb02"},
    {"Party: Bells anywhere", 0x18700d3, "84c041bdffffffff440f44eb41c1ed1f", "4531ed90909090909090909090909090"},
    {"Party: Bells after boss defeated", 0x18749e8, "0f8548010000", "909090909090"},
    {"Party: Bells after boss defeated", 0x18749f0, "0f8840010000", "909090909090"},
    {"Party: Bells after boss defeated", 0x14b714a, "0f85c0030000", "909090909090"},
    {"Party: Bells after boss defeated", 0x1874b79, "740d", "eb0d"},
    {"Party: Keep session on map reload", 0x19471b1, "e8ea955800", "e8ea8e5800"},
    {"Party: SOS sign timeout 30s", 0x4927a5c, "00003443", "0000f041"},
    {"Party: Bells without Insight", 0x157fa5c, "0f9fc0", "30c090"},
    {"Party: Bells without Insight", 0x18c92bb, "410fb64738f7d8", "31c00f1f440000"},
    // Join timing (docs/party/join_timing.md; BB_PARTY_FAST_JOIN / BB_PARTY_FAST_MESSAGES).
    {"Party: Fast join", 0x196d3a8, "c7051e77c0030000a040", "c7051e77c003cdcccc3d"},
    {"Party: Fast join", 0x196d3b2, "c7051c77c0030000a040", "c7051c77c0030000803f"},
    {"Party: Fast join", 0x196d3c6, "c7051077c0030000a040", "c7051077c0030000803f"},
    {"Party: Fast join", 0x196d3d0, "c7050e77c00300000040", "c7050e77c0030000003f"},
    {"Party: Fast join", 0x14b80c4, "41c7451c00004040", "41c7451c0000003f"},
    {"Party: Fast join", 0x14b4975, "41c744241801000500", "41c744241801001000"},
    {"Party: Fast join", 0x14bb067, "c7431801000500", "c7431801001000"},
    {"Party: Fast join", 0xcdb2e8, "c7842488000000b80b0000", "c78424880000002c010000"},
    {"Party: Fast bell sign", 0x18a9264, "c747280000a040", "c7472800000040"},
    {"Party: Fast bell sign", 0x18a9324, "c747280000a040", "c7472800000040"},
    {"Party: Fast bell sign", 0x189a095, "c743280000a040", "c7432800000040"},
    {"Party: Fast bell sign", 0x189a1a5, "c743280000a040", "c7432800000040"},
    {"Party: Fast bell sign", 0x189a2b5, "c743280000a040", "c7432800000040"},
    {"Party: Fast bell sign", 0x189a3c5, "c743280000a040", "c7432800000040"},
    {"Party: Fast bell sign", 0x189a4d5, "c743280000a040", "c7432800000040"},
    {"Party: Fast bell sign", 0x189a5e5, "c743280000a040", "c7432800000040"},
    {"Party: Fast bell sign", 0x189a6f5, "c743280000a040", "c7432800000040"},
    {"Party: Fast bell sign", 0x189a805, "c743280000a040", "c7432800000040"},
    {"Party: Fast bell sign", 0x189a915, "c743280000a040", "c7432800000040"},
    {"Party: Fast bell sign", 0x189aa25, "c743280000a040", "c7432800000040"},
    {"Party: Fast rejoin", 0x147afe4, "41c78424e80000000000f041", "41c78424e800000000002041"},
    {"Party: Fast rejoin", 0x147b0a7, "c781e80000000000f041", "c781e800000000002041"},
    {"Party: Fast rejoin", 0x1480cf7, "c787e80000000000f041", "c787e800000000002041"},
    {"Party: Fast rejoin", 0x1480e14, "c780e80000000000f041", "c780e800000000002041"},
    {"Party: Fast multiplayer messages", 0x1bfface, "41c745000000a040", "41c745000000c03f"},
    {"Party: Fast multiplayer messages", 0x4928a98, "0000a040", "0000c03f"},
    {"Party: Hide bell searching messages", 0x1389cfc, "e97f618700", "c390909090"},
    {"Party: Hide bell searching messages", 0x17c3cea, "e891c14300", "0f1f440000"},
};

std::vector<u8> Hex(const char* h) {
    std::vector<u8> out;
    for (std::size_t i = 0; h[i] && h[i + 1]; i += 2) {
        char b[3] = {h[i], h[i + 1], 0};
        out.push_back(static_cast<u8>(std::strtoul(b, nullptr, 16)));
    }
    return out;
}

void ReportSites() {
    const char* current = nullptr;
    int applied = 0, off = 0, bad = 0;
    auto flush = [&] {
        if (current) {
            Log("%s: %s (%d sites applied, %d original, %d mismatched)", current,
                bad ? "MISMATCH" : (applied && !off ? "applied" : (!applied ? "off" : "PARTLY applied")), applied, off,
                bad);
        }
    };
    for (const Site& s : kSites) {
        if (!current || std::strcmp(current, s.patch) != 0) {
            flush();
            current = s.patch;
            applied = off = bad = 0;
        }
        const std::vector<u8> o = Hex(s.original), p = Hex(s.patched);
        if (Matches(s.off, p.data(), p.size())) {
            ++applied;
        } else if (Matches(s.off, o.data(), o.size())) {
            ++off;
        } else {
            ++bad;
            Log("%s: +0x%llx holds neither the 1.09 bytes nor the patch", s.patch, ull(s.off));
        }
    }
    flush();
}

// ---- Params (SoloParamRepository) ----
// Slot 0x5540340 (bbhost 0x5940340, SoloParamRepository_ptr): holders at +0x70, stride 0x48
// ({u32 count, ParamResCap* caps[8]}); cap +0x70 FD4ParamResCap, whose +0x70 is the param file.
// File: +0x0a u16 rows, +0x0c type name, +0x2d format, +0x2e flags; records at +0x40 (format
// >= 4 with flags & 2: 0x18 bytes {u32 id, pad, u64 data offset}; else 0xc {u32 id, u32 offset}).
// The 1.09 gameparam files are format 4 / flags 7 (checked offline in gameparam.parambnd.dcx).
constexpr u64 kParamRepository = 0x5540340;
constexpr unsigned kHolders = 63, kCapsPerHolder = 8;

struct ParamTable {
    u64 file = 0;
    u32 rows = 0;
    u8 format = 0, flags = 0;
};

std::string CapName(u64 cap) {
    u64 capacity = 0;
    if (!SafeGet(cap + 0x30, &capacity)) {
        return "";
    }
    u64 at = cap + 0x18;
    if (capacity >= 8 && !SafeGet(cap + 0x18, &at)) {
        return "";
    }
    std::string s;
    for (int i = 0; i < 64; ++i) {
        u16 c = 0;
        if (!SafeGet(at + 2u * unsigned(i), &c) || !c) {
            break;
        }
        s.push_back(c < 128 ? char(c) : '?');
    }
    return s;
}

bool FindParam(const char* name, const char* type, ParamTable* out) {
    u64 repo = 0;
    const u64 slot = Guest(kParamRepository);
    if (!slot || !SafeGet(slot, &repo) || !repo) {
        return false;
    }
    for (unsigned h = 0; h < kHolders; ++h) {
        const u64 holder = repo + 0x70 + u64(h) * 0x48;
        i32 count = 0;
        if (!SafeGet(holder, &count) || count <= 0) {
            continue;
        }
        for (unsigned k = 0; k < unsigned(count) && k < kCapsPerHolder; ++k) {
            u64 cap = 0, res = 0, file = 0;
            if (!SafeGet(holder + 8 + 8 * k, &cap) || !cap || !SafeGet(cap + 0x70, &res) || !res ||
                !SafeGet(res + 0x70, &file) || !file) {
                continue;
            }
            if (CapName(cap) != name) {
                continue;
            }
            char t[33] = {};
            u16 rows = 0;
            ParamTable p;
            if (!SafeRead(file + 0xc, t, 32) || std::strncmp(t, type, 32) != 0 || !SafeGet(file + 0xa, &rows) ||
                !SafeGet(file + 0x2d, &p.format) || !SafeGet(file + 0x2e, &p.flags)) {
                continue;
            }
            p.file = file;
            p.rows = rows;
            *out = p;
            return true;
        }
    }
    return false;
}

bool Record(const ParamTable& t, u32 i, u32* id, u64* data) {
    if (i >= t.rows) {
        return false;
    }
    if (t.format == 2) {
        u32 off = 0;
        if (!SafeGet(t.file + 0x34 + 0xc * u64(i), id) || !SafeGet(t.file + 0x34 + 0xc * u64(i) + 4, &off)) {
            return false;
        }
        *data = t.file + off;
    } else if (t.format <= 3 || !(t.flags & 2)) {
        u32 off = 0;
        if (!SafeGet(t.file + 0x40 + 0xc * u64(i), id) || !SafeGet(t.file + 0x40 + 0xc * u64(i) + 4, &off)) {
            return false;
        }
        *data = t.file + off;
    } else {
        u64 off = 0;
        if (!SafeGet(t.file + 0x40 + 0x18 * u64(i), id) || !SafeGet(t.file + 0x40 + 0x18 * u64(i) + 8, &off)) {
            return false;
        }
        *data = t.file + off;
    }
    return true;
}

std::size_t RowSize(const ParamTable& t) {
    u32 a = 0, b = 0;
    u64 da = 0, db = 0;
    if (t.rows < 2 || !Record(t, 0, &a, &da) || !Record(t, 1, &b, &db) || db <= da) {
        return 0;
    }
    return std::size_t(db - da);
}

/// The row with `id` (binary search; ids are sorted), or 0.
u64 Row(const ParamTable& t, u32 id) {
    u32 lo = 0, hi = t.rows;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2;
        u32 rid = 0;
        u64 data = 0;
        if (!Record(t, mid, &rid, &data)) {
            return 0;
        }
        if (rid == id) {
            return data;
        }
        if (rid < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 0;
}

// One field write: the game's value must be `from` (or already `to`).
struct ParamRule {
    const char* what;
    const char* param;
    const char* type;
    std::size_t row_size;
    u32 row;
    u32 field;
    std::size_t width; // 1 (u8) or 4 (f32)
    float from, to;
    const char* env_off; // the env switch that turns this rule off
    bool opt_in = false; // off unless env_off is set to a non-"0" value
    bool enabled = true;
    int state = 0; // 0 waiting, 1 written / holds, 2 refused (unexpected value or layout)
    std::uint64_t writes = 0;
};

const ParamRule kFixedParamRules[] = {
    // 0.7.5: the cooperator / invader phantom max-HP cut (SpEffect 9006 / 9026 maxHpRate 0.7) is a host rule
    // now (party_phantom_hp, PhantomHpTick below), no longer always lifted.
    {"Beckoning Bell without Insight (EquipParamGoods 200 consumeHeroPoint)", "EquipParamGoods",
     "EQUIP_PARAM_GOODS_ST", 0x6c, 200, 0x38, 1, 1.0f, 0.0f, "BB_PARTY_BELL_NO_INSIGHT"},
    // Hunter's Mark for guests (phantom_limits.md 1.1 / 3.2): byte 0x44 |= enable_white (bit 2) |
    // enable_multi (bit 4); 1.09 data 0x43 / 0xC3. On by default (BB_PARTY_GUEST_MARK=0 off): a
    // guest's Mark asks "Travel with the party?" (party_split: alone to its own last lamp, or the
    // party to the host's last lamp), the host's likewise.
    {"Hunter's Mark for phantoms (EquipParamGoods 100 enable_white / enable_multi)", "EquipParamGoods",
     "EQUIP_PARAM_GOODS_ST", 0x6c, 100, 0x44, 1, 67.0f, 87.0f, "BB_PARTY_GUEST_MARK"},
    {"Bold Hunter's Mark for phantoms (EquipParamGoods 1400 enable_white / enable_multi)", "EquipParamGoods",
     "EQUIP_PARAM_GOODS_ST", 0x6c, 1400, 0x44, 1, 195.0f, 215.0f, "BB_PARTY_GUEST_MARK"},
};

// Guest services (guest_services.md 2.4, playtest 1 bugs 11 / 17): on a client, chr creation
// 0x18E4F94..0x18E4FDC turns an NPC whose NpcParam has isChangeWanderGhost (byte 0x145 bit 7)
// into a wandering ghost (chr type 4: translucent, no collision, no talk). Every talk NPC's row
// (party_npcghost.h: the Doll, Gehrman, Iosefka, Alfred, Eileen, the Chapel Dweller ...) loses
// the bit, so a guest in the host's world sees them solid and can talk to them. Only clients read
// the bit. BB_PARTY_GUEST_SERVICES=0 leaves them vanilla.
std::vector<ParamRule> BuildParamRules() {
    std::vector<ParamRule> v(std::begin(kFixedParamRules), std::end(kFixedParamRules));
    static std::vector<std::string> names;  // ParamRule::what points into it
    names.reserve(npcghost::kTalkNpcRowCount);
    for (const npcghost::GhostRow& r : npcghost::kTalkNpcRows) {
        std::uint8_t want = 0;
        npcghost::Decide(r, r.byte, &want);
        names.push_back(std::string(r.who) + " stays solid for guests (NpcParam " + std::to_string(r.row) +
                        " isChangeWanderGhost)");
        v.push_back({names.back().c_str(), "NpcParam", "NPC_PARAM_ST", 0x184, r.row, npcghost::kField, 1,
                     float(r.byte), float(want), "BB_PARTY_GUEST_SERVICES"});
    }
    return v;
}
std::vector<ParamRule> g_param_rules = BuildParamRules();

bool ReadField(u64 at, std::size_t width, float* v) {
    if (width == 1) {
        u8 b = 0;
        if (!SafeGet(at, &b)) {
            return false;
        }
        *v = float(b);
        return true;
    }
    return SafeGet(at, v);
}

bool WriteField(u64 at, std::size_t width, float v) {
    if (width == 1) {
        const u8 b = u8(v);
        return SafeWrite(at, &b, 1);
    }
    return SafeWrite(at, &v, 4);
}

/// True once every enabled rule holds (written or found written).
bool ApplyParamRules() {
    bool all = true;
    const char* last_param = nullptr;
    const char* last_type = nullptr;
    bool last_found = false;
    ParamTable t;
    for (ParamRule& r : g_param_rules) {
        if (!r.enabled || r.state == 2) {
            continue;
        }
        // Consecutive rules of one table (the talk-NPC rows) share one repository walk.
        if (r.param != last_param || r.type != last_type) {
            last_param = r.param;
            last_type = r.type;
            last_found = FindParam(r.param, r.type, &t);
        }
        if (!last_found) {
            all = false;
            continue;
        }
        const std::size_t size = RowSize(t);
        const u64 row = Row(t, r.row);
        if ((size && size != r.row_size) || !row) {
            Log("%s: %s %s (row size %zu, expected %zu); left alone", r.what, r.param,
                row ? "has another layout" : "has no such row", size, r.row_size);
            r.state = 2;
            continue;
        }
        float v = 0;
        if (!ReadField(row + r.field, r.width, &v)) {
            all = false;
            continue;
        }
        if (v == r.to) {
            if (r.state == 0) {
                Log("%s: already %g", r.what, double(r.to));
            }
            r.state = 1;
            continue;
        }
        if (v != r.from) {
            Log("%s: the game's value is %g, not %g; left alone", r.what, double(v), double(r.from));
            r.state = 2;
            continue;
        }
        if (!WriteField(row + r.field, r.width, r.to)) {
            all = false;
            continue;
        }
        ++r.writes;
        Log("%s: %g -> %g (row at 0x%llx%s)", r.what, double(v), double(r.to), ull(row),
            r.writes > 1 ? ", again: the params were reloaded" : "");
        r.state = 1;
    }
    return all;
}

// ---- EMEVD instruction dispatch filter (0x17b93a0) ----
// SprjEmkInstructionDispatcher::Dispatch(this, SprjEmkEventIns* event, float dt) -> bool. Its one
// caller is SprjEmkEventIns::Update (0x12ecdc0), which ignores the result and then advances the
// event (0x12ef2e0, instruction_delta defaults to 1): a skipped instruction is simply passed.
// Event: +0x28 i32 event id, +0x68 map id, +0xa0 instruction index, +0xa8 EVD runtime data,
// +0xb0 the instruction record {i32 bank, i32 id, u32 argument size, pad, i64 argument offset},
// +0xb8 the copied arguments (else EVD base + *(base + 0x78) + argument offset).
constexpr u64 kEmevdDispatch = 0x17b93a0;

using DispatchFn = u8(BB_COOP_SYSV*)(u64 self, u64 event, float dt);
DispatchFn g_dispatch_original = nullptr;
bool g_emevd_trace = false;
std::atomic<EmevdRewriter> g_rewriter{nullptr};
std::atomic<EmevdRewriter> g_rewriter2{nullptr};
std::atomic<EmevdRewriter> g_rewriter3{nullptr};

// Rules "event:bank:id[@index]" (party_phantom.h). Built in: the confinement walls of common
// event 7600, its Chalice Dungeon copy 12907600 and m34 13400010 (only while B1 travel is on and
// the party's Open world rule, the host's since 0.7.4, is on). The A6
// "send phantoms home" instructions are still to be identified with BB_PARTY_EMEVD_TRACE=1.
std::vector<EmevdSkipRule> g_skips;
bool g_emevd_observe = false; // party_phantom's boss-Insight observer

std::mutex g_emevd_mu;
std::unordered_map<u64, u64> g_emevd_seen; // key: event << 32 ^ bank << 16 ^ id
std::atomic<u64> g_emevd_calls{0}, g_emevd_skipped{0};

void ParseSkips() {
    for (const EmevdSkipRule& r : PhantomSkipRules()) {
        g_skips.push_back(r);
        Log("EMEVD rule (built in, confinement walls): skip %s", DescribeSkipRule(r).c_str());
    }
    if (bb_party_enabled()) {
        for (const EmevdSkipRule& r : BossRoomRules()) {
            g_skips.push_back(r);
            Log("EMEVD rule (built in, boss rooms): skip %s", DescribeSkipRule(r).c_str());
        }
    }
    for (const EmevdSkipRule& r : GuestServicesSkipRules()) {
        g_skips.push_back(r);
        Log("EMEVD rule (built in, guest services): skip %s", DescribeSkipRule(r).c_str());
    }
    // D5 / D10 (pard4): host-only action buttons a guest may press (party_story.h
    // GuestPressedHostOnlyEvent): Amelia's skull 12401803 ends a client's copy ([6] 1003[6]) and
    // waits for chr type 0 ([7] 4[3]) next to its 3[24] button. A guest's copy skips both, so the
    // guest's press runs its 2002[6] (scene + evening -> night) and the host plays it as its own.
    // BB_PARTY_GUEST_HOST_ACTIONS=0: off (the host presses it, as before).
    if (const char* party = std::getenv("BB_PARTY"); party && party[0] && !EnvOff("BB_PARTY_GUEST_HOST_ACTIONS")) {
        EmevdSkipRule end_client, host_chr;
        end_client.event = 12401803, end_client.bank = 1003, end_client.id = 6, end_client.index = 6;
        host_chr.event = 12401803, host_chr.bank = 4, host_chr.id = 3, host_chr.index = 7;
        end_client.client_only = host_chr.client_only = true;
        for (const EmevdSkipRule& r : {end_client, host_chr}) {
            g_skips.push_back(r);
            Log("EMEVD rule (built in, host-only action a guest may press): skip %s (guests only)",
                DescribeSkipRule(r).c_str());
        }
    }
    const char* e = std::getenv("BB_PARTY_EMEVD_SKIP");
    if (!e) {
        return;
    }
    std::vector<EmevdSkipRule> rules;
    std::vector<std::string> errors;
    ParseEmevdSkipRules(e, &rules, &errors);
    for (const std::string& err : errors) {
        Log("BB_PARTY_EMEVD_SKIP: %s; ignored", err.c_str());
    }
    for (const EmevdSkipRule& r : rules) {
        g_skips.push_back(r);
        Log("EMEVD rule: skip %s", DescribeSkipRule(r).c_str());
    }
}

void TraceInstruction(u64 event, i32 event_id, i32 bank, i32 id, u64 instr, bool skipped) {
    const u64 key = (u64(u32(event_id)) << 32) ^ (u64(u16(bank)) << 16) ^ u64(u16(id));
    u64 n;
    {
        std::lock_guard<std::mutex> lk(g_emevd_mu);
        n = ++g_emevd_seen[key];
    }
    // BB_PARTY_EMEVD_TRACE_ALL=ID[,ID...]: every call of these events (a lift's second ride).
    static const std::vector<i32> all = [] {
        std::vector<i32> v;
        if (const char* e = std::getenv("BB_PARTY_EMEVD_TRACE_ALL")) {
            for (const char* p = e; *p;) {
                char* end = nullptr;
                const long x = std::strtol(p, &end, 10);
                if (end == p) break;
                v.push_back(i32(x));
                p = *end == ',' ? end + 1 : end;
            }
        }
        return v;
    }();
    const bool every = std::find(all.begin(), all.end(), event_id) != all.end();
    if (!every && n != 1 && n != 1000 && n != 100000) {
        return;
    }
    u32 map = 0, arg_size = 0;
    i32 index = -1;
    SafeGet(event + 0x68, &map);
    SafeGet(event + 0xa0, &index);
    const u64 args = EmevdInstructionArgs(event, instr, &arg_size);
    char hex[3 * 24 + 1] = "";
    u8 bytes[24] = {};
    const std::size_t shown = arg_size < sizeof bytes ? arg_size : sizeof bytes;
    if (args && shown && SafeRead(args, bytes, shown)) {
        for (std::size_t i = 0; i < shown; ++i) {
            std::snprintf(hex + 3 * i, 4, "%02x ", bytes[i]);
        }
    }
    Log("EMEVD event %d [%d] %d[%02d] map %08x args(%u) %s(call #%llu)%s", event_id, index, bank, id, map, arg_size,
        hex, ull(n), skipped ? " SKIPPED (rule)" : "");
}

std::atomic<EmevdSkipPredicate> g_skip_pred{nullptr};
std::atomic<EmevdSkipPredicate> g_skip_pred2{nullptr};  // party_npcfight (D11)
// B2: a guest may go through a boss fog before the host (party_worldact.h BossGate);
// BB_PARTY_GUEST_FOG_FIRST=0 turns it off.
bool g_fog_first = false;
std::atomic<std::uint32_t> g_fog_first_logged{0};

// Guest (role 6): the host-entered condition (3[0] IfEventFlag, u32 flag at +4) of a guest-entry
// event is passed, so the fog's action prompt works before the host has gone through.
bool FogFirstSkip(u64 event, u64 instr, i32 event_id, i32 bank, i32 id) {
    if (!g_fog_first || bank != 3 || id != 0 ||
        (!worldact::BossGateByEvent(event_id) && !worldact::BossGateByLandingEvent(event_id)) ||
        LocalSessionRole() != 6) {
        return false;
    }
    u8 args[8] = {};
    u32 arg_size = 0;
    const u64 at = EmevdInstructionArgs(event, instr, &arg_size);
    if (!at || arg_size < 8 || !SafeRead(at, args, 8)) {
        return false;
    }
    u32 flag = 0;
    std::memcpy(&flag, args + 4, 4);
    const bool first = worldact::GuestFirstFightEnabled() && worldact::BossGateSkipFirstFight(event_id, bank, id, flag);
    if (!first && !worldact::BossGateSkipCondition(event_id, bank, id, flag)) {
        return false;
    }
    // D3 (pard4): not while the host is in another area (a lone guest's fight has no start / defeat).
    if (!worldact::GuestFogSkipAllowed(event_id)) {
        return false;
    }
    static std::atomic<std::uint32_t> first_logged{0};
    if (first ? first_logged.exchange(u32(event_id)) != u32(event_id)
              : g_fog_first_logged.exchange(u32(event_id)) != u32(event_id)) {
        if (first) {
            // P5 / D10: the fog has its prompt before the host's first fight; going through starts
            // the host's first-fight event (party_worldact HostFogAction start_first).
            Log("boss fog: guest-entry event %d does not wait for the host's first fight (first-fight flag %u "
                "condition passed): this guest may start it", event_id, flag);
        } else {
            Log("boss fog: guest-entry event %d does not wait for the host (host-entered flag %u condition passed; "
                "args %02x %02x %02x)", event_id, flag, args[0], args[1], args[2]);
        }
    }
    return true;
}

// A11: the boss health bar (EMEVD 2003[11] DisplayBossHealthBar: first argument 1 on, 0 off).
std::atomic<bool> g_boss_bar{false};
// The bar slots (2003[11] third argument, 0 the bottom bar) shown since the bar came up: the party
// HUD keeps clear of every stacked bar (Shadows of Yharnam: three).
std::atomic<u32> g_boss_bar_slots{0};
// The last text banner (2007[2], e.g. PREY SLAUGHTERED) or boss defeat (2003[12/15/53]) this game
// ran, steady-clock seconds: an item notice opened under the banner had no OK row (live pard3 O1).
std::atomic<double> g_banner_at{-1e9};
double SteadyNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

BB_COOP_SYSV u8 EmevdDispatch(u64 self, u64 event, float dt) {
    perf::Scope z(perf::Zone::HookEmevd);
    g_emevd_calls.fetch_add(1, std::memory_order_relaxed);
    u64 instr = 0;
    i32 event_id = 0, bank = -1, id = -1;
    if (event && SafeGet(event + 0xb0, &instr) && instr) {
        SafeGet(event + 0x28, &event_id);
        SafeGet(instr, &bank);
        SafeGet(instr + 4, &id);
    }
    bool skip = false;
    if (instr && !g_skips.empty()) {
        i32 index = -1;
        SafeGet(event + 0xa0, &index);
        for (const EmevdSkipRule& r : g_skips) {
            if (EmevdSkipMatches(r, event_id, bank, id, index) && (!r.needs_travel || TravelEnabled()) &&
                (!r.client_only || LocalSessionRole() == 6) &&
                (!r.open_world || party::status::HostRuleOn(party::status::HostRule::OpenWorld))) {
                skip = true;
                break;
            }
        }
    }
    if (!skip && instr && g_fog_first) {
        skip = FogFirstSkip(event, instr, event_id, bank, id);
    }
    if (!skip && instr) {
        if (const EmevdSkipPredicate pred = g_skip_pred.load(std::memory_order_relaxed)) {
            skip = pred(event_id, bank, id, event, instr);
        }
    }
    if (!skip && instr) {
        if (const EmevdSkipPredicate pred = g_skip_pred2.load(std::memory_order_relaxed)) {
            skip = pred(event_id, bank, id, event, instr);
        }
    }
    if (instr && !skip && ((bank == 2007 && id == 2) || (bank == 2003 && (id == 12 || id == 15 || id == 53)))) {
        g_banner_at.store(SteadyNow(), std::memory_order_relaxed);
    }
    if (instr && !skip && bank == 2003 && (id == 12 || id == 15 || id == 53) && g_boss_bar.exchange(false)) {
        g_boss_bar_slots.store(0, std::memory_order_relaxed);
        Log("boss health bar: off (a boss defeat, event %d 2003[%d])", event_id, id);
    }
    if (instr && !skip && bank == 2003 && id == 11) {
        u8 args[16] = {};
        u32 arg_size = 0;
        const u64 at = EmevdInstructionArgs(event, instr, &arg_size);
        const std::size_t n = arg_size < sizeof args ? arg_size : sizeof args;
        if (at && n && SafeRead(at, args, n)) {
            const bool on = args[0] != 0;
            const int slot = n >= 10 ? *reinterpret_cast<const std::int16_t*>(args + 8) : 0;
            if (!on && !g_boss_bar.load()) {
                g_boss_bar_slots.store(0, std::memory_order_relaxed);
            } else if (on && slot >= 0 && slot < 8) {
                if (!g_boss_bar.load()) g_boss_bar_slots.store(0, std::memory_order_relaxed);
                if (!(g_boss_bar_slots.fetch_or(1u << slot) & (1u << slot)) && slot > 0) {
                    Log("boss health bar: slot %d shown too (event %d)", slot, event_id);
                }
            }
            if (g_boss_bar.exchange(on) != on) {
                Log("boss health bar %s (event %d, entity %d, slot %d)", on ? "shown: a boss fight" : "hidden",
                    event_id, n >= 8 ? *reinterpret_cast<const i32*>(args + 4) : -1, slot);
                if (!on) g_boss_bar_slots.store(0, std::memory_order_relaxed);
            }
        }
    }
    if (instr && !skip && bank == 2007 && id == 1 && LocalSessionRole() != 6) {
        // D10: a story notice only the host's event shows (party_story StoryNoticeMessage).
        u8 args[4] = {};
        u32 arg_size = 0;
        const u64 at = EmevdInstructionArgs(event, instr, &arg_size);
        u32 msg = 0;
        if (at && arg_size >= 4 && SafeRead(at, args, 4)) {
            std::memcpy(&msg, args, 4);
            if (StoryNoticeMessage(msg)) {
                NoteHostStoryNotice(msg, static_cast<u32>(event_id));
            }
        }
    }
    if (instr && !skip && bank == 2003 && id == 49 &&
        deathrules::StoryWarpEvent(event_id, LocalSessionRole() != 6)) {
        // A story warp (Blood Moon, carriage, Amygdala grab, ...) runs only on the game of the
        // player who triggered it: that player's own use for the travel question (party_split).
        u8 args[4] = {};
        u32 arg_size = 0;
        const u64 at = EmevdInstructionArgs(event, instr, &arg_size);
        u32 lamp = 0;
        if (at && arg_size >= 4 && SafeRead(at, args, 4)) {
            std::memcpy(&lamp, args, 4);
        }
        split::NoteStoryWarp(lamp);
        Log("story warp: event %d warps this player to %u (2003[49]): this player's own travel", event_id, lamp);
    }
    if (g_emevd_observe && instr && !skip &&
        ((bank == 2003 && (id == 12 || id == 15 || id == 53)) || (bank == 2000 && id == 0) ||
         (bank == 2004 && id == 8))) {
        u8 args[12] = {};
        u32 arg_size = 0;
        const u64 at = EmevdInstructionArgs(event, instr, &arg_size);
        const std::size_t n = arg_size < sizeof args ? arg_size : sizeof args;
        const bool have = at && n && SafeRead(at, args, n);
        PhantomEmevdInstruction(event_id, bank, id, have ? args : nullptr, have ? n : 0);
    }
    if (g_emevd_trace && instr) {
        TraceInstruction(event, event_id, bank, id, instr, skip);
    }
    if (skip) {
        g_emevd_skipped.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    if (instr) {
        if (const EmevdRewriter rw = g_rewriter.load(std::memory_order_relaxed)) {
            rw(event_id, bank, id, event, instr);
        }
        if (const EmevdRewriter rw = g_rewriter2.load(std::memory_order_relaxed)) {
            rw(event_id, bank, id, event, instr);
        }
        if (const EmevdRewriter rw = g_rewriter3.load(std::memory_order_relaxed)) {
            rw(event_id, bank, id, event, instr);
        }
    }
    z.End(); // the game's own dispatch is not ours
    return g_dispatch_original(self, event, dt);
}

struct State {
    bool init = false;
    bool params_done = false;
    Clock::time_point last_param{};
    bool param_logged_wait = false;
    Clock::time_point start{};
    Clock::time_point last_emevd_log{};
};

State& S() {
    static State s;
    return s;
}

double Seconds(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

// A4: the bell sign sender's first-sign timer. The XML patch "Party: Fast bell sign" writes 2.0 s
// (5.0 in 1.09). With the direct summon the guest's game needs its bell's own passive summon entry,
// which comes up only when that timer fires, so the timer paces every join after the room is there.
// Default 0.5 s since 0.6.2 (parab3, live: 23 of 24 direct-summon joins in 1.5-1.6 s instead of
// 3.0 s; the one miss was the host's empty-room timeout, fixed in party_b2). The stall parab2 saw at
// 0.5 s (client (6), flow 2, no load) is the game's SummonReloadStart clearing the summon type; the
// guest's flow-2 watchdog (party_b2 Tick) restarts that join start (live with
// BB_PARTY_TEST_JOIN_STALL). Below 2.0 s the host's sign board holds a sign back until it is
// (2.0 - S) s old (from_api sign_get), so the bell sign path keeps its proven 2 s from the ring.
// BB_PARTY_SIGN_DELAY=S (0.1 .. 5.0 s) overrides; 2 keeps the XML patch's 2.0 s.
void ApplySignDelay() {
    const char* env = std::getenv("BB_PARTY_SIGN_DELAY");
    if (!env || !env[0]) {
        env = "0.5";
    }
    const float secs = std::strtof(env, nullptr);
    if (!(secs >= 0.1f && secs <= 5.0f)) {
        Log("bell sign timer: BB_PARTY_SIGN_DELAY='%s' is not 0.1 .. 5.0; left as patched", env);
        return;
    }
    if (secs == 2.0f) {
        Log("bell sign timer: 2.0 s (the XML patch)");
        return;
    }
    u8 want[4];
    std::memcpy(want, &secs, 4);
    int done = 0, sites = 0;
    for (const Site& s : kSites) {
        if (std::strcmp(s.patch, "Party: Fast bell sign") != 0) {
            continue;
        }
        ++sites;
        // c7 4? 28 <imm32>: the float is the last 4 bytes; only over the patch's own 2.0 s.
        if (PatchBytes(s.off + 3, {0x00, 0x00, 0x00, 0x40}, {want[0], want[1], want[2], want[3]}, "Party: bell sign timer")) {
            ++done;
        }
    }
    const long long hold_ms = secs < 2.0f && done > 0 ? static_cast<long long>((2.0f - secs) * 1000.0f + 0.5f) : 0;
    bbnet::party::FromApi::instance().set_min_sign_age_ms(hold_ms);
    Log("bell sign timer: %.2f s at %d of %d sites (BB_PARTY_SIGN_DELAY); the host shows a sign %lld ms after it went up",
        double(secs), done, sites, hold_ms);
}

} // namespace

bool SeamlessRequested() {
    return bb_party_enabled() && !EnvOff("BB_PARTY_SEAMLESS");
}

void SeamlessRulesInit() {
    State& st = S();
    if (st.init) {
        return;
    }
    if (!SeamlessRequested()) {
        Log("off (%s)", EnvOff("BB_PARTY_SEAMLESS") ? "BB_PARTY_SEAMLESS=0" : "BB_PARTY not set");
        ReportSites(); // shows the XML patches are off too
        return;
    }
    if (!Image()) {
        Log("no image; off");
        return;
    }
    st.init = true;
    st.start = Clock::now();
    ReportSites();
    ApplySignDelay();
    for (ParamRule& r : g_param_rules) {
        r.enabled = r.opt_in ? EnvOn(r.env_off) : !EnvOff(r.env_off);
        if (!r.enabled) {
            Log("%s: off (%s%s)", r.what, r.env_off, r.opt_in ? " not set; opt-in" : "=0");
        }
    }
    g_emevd_trace = EnvOn("BB_PARTY_EMEVD_TRACE");
    ParseSkips();
    g_emevd_observe = PhantomWantsEmevd();
    g_fog_first = !EnvOff("BB_PARTY_GUEST_FOG_FIRST");
    Log("boss fogs: a guest may go through first %s", g_fog_first ? "on" : "off (BB_PARTY_GUEST_FOG_FIRST=0)");
    if (g_emevd_trace || !g_skips.empty() || g_emevd_observe || g_fog_first) {
        if (EnsureEmevdFilter()) {
            Log("EMEVD filter: %s, %zu skip rules, boss-Insight observer %s", g_emevd_trace ? "trace on" : "trace off",
                g_skips.size(), g_emevd_observe ? "on" : "off");
        } else {
            g_emevd_trace = false;
            g_emevd_observe = false;
            g_skips.clear();
        }
    }
}

void SetEmevdSkipPredicate(EmevdSkipPredicate fn) {
    g_skip_pred = fn;
}

void SetEmevdSkipPredicate2(EmevdSkipPredicate fn) {
    g_skip_pred2 = fn;
}

void SetEmevdRewriter(EmevdRewriter fn) {
    g_rewriter.store(fn);
}

void SetEmevdRewriter2(EmevdRewriter fn) {
    g_rewriter2.store(fn);
}

void SetEmevdRewriter3(EmevdRewriter fn) {
    g_rewriter3.store(fn);
}

double BannerAgeSeconds() {
    return SteadyNow() - g_banner_at.load(std::memory_order_relaxed);
}

bool BossBarShown() {
    return g_boss_bar.load(std::memory_order_relaxed);
}

int BossBarRows() {
    if (!g_boss_bar.load(std::memory_order_relaxed)) return 0;
    const u32 slots = g_boss_bar_slots.load(std::memory_order_relaxed);
    int rows = 1;
    for (int i = 1; i < 8; ++i) {
        if (slots & (1u << i)) rows = i + 1;
    }
    return rows;
}

void BossBarReset() {
    g_boss_bar_slots.store(0, std::memory_order_relaxed);
    if (g_boss_bar.exchange(false)) Log("boss health bar: reset (a load)");
}

bool EnsureEmevdFilter() {
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    if (g_dispatch_original) {
        return true;
    }
    // push rbp; mov rbp, rsp; push rbx; push rax - 6 bytes, no relative operands.
    void* original = nullptr;
    if (!ReplacePrologue(kEmevdDispatch, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50},
                         reinterpret_cast<const void*>(&EmevdDispatch), &original, "EMEVD instruction dispatch filter")) {
        return false;
    }
    g_dispatch_original = reinterpret_cast<DispatchFn>(original);
    return true;
}

std::uint64_t EmevdInstructionArgs(std::uint64_t event, std::uint64_t instr, std::uint32_t* size) {
    u32 arg_size = 0;
    i64 arg_off = 0;
    if (size) {
        *size = 0;
    }
    if (!event || !instr || !SafeGet(instr + 8, &arg_size) || !SafeGet(instr + 0x10, &arg_off)) {
        return 0;
    }
    u64 args = 0;
    SafeGet(event + 0xb8, &args);
    if (!args) {
        u64 runtime = 0, base = 0, section = 0;
        if (SafeGet(event + 0xa8, &runtime) && runtime && SafeGet(runtime + 8, &base) && base &&
            SafeGet(base + 0x78, &section)) {
            args = base + section + u64(arg_off);
        }
    }
    if (args && size) {
        *size = arg_size;
    }
    return args;
}

// 0.7.6 (pvphp): the local hunter's HP across a phantom-rate change. The game recomputes the max HP from
// the SpEffect rows (0x17e46e0) within a frame and adds the max difference to the HP when the max grows
// (a guest at 245/490 stands at 455/700 the moment PvP lifts the cut; live: 260/503 -> 476/719) but keeps
// the points when it shrinks (700/700 is clamped to 490/490, 350/700 stays 350/490). The keeper (pure:
// party_levelsync_rules.h HpRatioKeeper) remembers the HP before the write and, once the max moved, writes
// the HP at the same fraction (245/490 -> 350/700 and back). Local chr only: a remote hunter's HP is its own
// game's, sent over the game's own player sync. BB_PARTY_PVP_HP_KEEP=0 leaves the points as the game has
// them (test aid).
levelsync::HpRatioKeeper g_hp_keep;
bool HpKeepDisabled() {
    static const bool off = [] {
        const char* v = std::getenv("BB_PARTY_PVP_HP_KEEP");
        return v && v[0] == '0';
    }();
    return off;
}
/// The local player's data module (PlayerIns +0x3b0 -> +0x20: HP +0xf8, max +0xfc; party_hud_game).
u64 LocalDataModule() {
    constexpr u64 kSlotWorldChrMan = 0x553e878;  // +0x60 the local player chr
    u64 wcm = 0, chr = 0, a = 0, dm = 0;
    if (!SafeGet(Guest(kSlotWorldChrMan), &wcm) || !wcm || !SafeGet(wcm + 0x60, &chr) || !chr) return 0;
    if (!SafeGet(chr + 0x3b0, &a) || !a || !SafeGet(a + 0x20, &dm)) return 0;
    return dm;
}
bool LocalHp(int* hp, int* max) {
    const u64 dm = LocalDataModule();
    return dm && SafeGet(dm + 0xf8, hp) && SafeGet(dm + 0xfc, max);
}
double Now() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}
/// Once a frame: the keeper's watch (nothing while it is not armed).
void PhantomHpKeepTick() {
    if (!g_hp_keep.armed) return;
    int hp = 0, max = 0;
    if (!LocalHp(&hp, &max)) hp = max = 0;
    const int before_hp = g_hp_keep.hp, before_max = g_hp_keep.max;
    const int want = g_hp_keep.Track(hp, max, Now());
    if (want < 0) return;
    if (want == hp) {
        Log("phantom health: HP %d/%d -> %d/%d, the fraction is kept already", before_hp, before_max, hp, max);
        return;
    }
    const u64 dm = LocalDataModule();
    const bool ok = dm && SafeWrite(dm + 0xf8, &want, sizeof want);
    Log("phantom health: HP kept at its fraction: %d/%d -> %d/%d (the game had %d/%d)%s", before_hp, before_max, want, max,
        hp, max, ok ? "" : " WRITE FAILED");
}

/// 0.7.5 "Phantom health" (party_phantom_hp, the host's rule; both ON = the game's 0.7): SpEffect 9006
/// (cooperator) and 9026 (invader phantom) maxHpRate follows the rule on every peer. 0.7.6 "Full health in
/// PvP" (party_pvp_full_hp, the host's rule, default on): while friendly fire is on the cut is lifted
/// (levelsync::PhantomCutOn), and the local hunter's HP fraction is kept across the change (the keeper above).
/// A row holding neither 0.7 nor 1.0 is left alone (logged once). Returns true once both rows hold what the
/// rules say.
bool ApplyPhantomHp() {
    static int logged_rule = -1;
    static bool refused = false;
    if (refused) return true;
    const bool rule = party::status::HostRuleOn(party::status::HostRule::PhantomHp);
    const bool pvp = party::status::FriendlyFire();
    const bool pvp_full = party::status::HostRuleOn(party::status::HostRule::PvpFullHp);
    const bool on = levelsync::PhantomCutOn(rule, pvp, pvp_full);
    const int state = on ? 1 : pvp && pvp_full && rule ? 2 : 0;  // for the log: 2 = lifted by PvP
    const float want = levelsync::PhantomHpRate(on);
    ParamTable t;
    if (!FindParam("SpEffectParam", "SP_EFFECT_PARAM_ST", &t)) return false;
    bool all = true, armed = false;
    for (const u32 id : {9006u, 9026u}) {
        const u64 row = Row(t, id);
        float v = 0;
        if (RowSize(t) != 0x210 || !row || !ReadField(row + 0x10, 4, &v)) {
            all = false;
            continue;
        }
        if (v == want) continue;
        if (v != 0.7f && v != 1.0f) {
            Log("phantom health: SpEffect %u maxHpRate is %g, neither 0.7 nor 1.0; left alone", id, double(v));
            refused = true;
            return true;
        }
        if (!armed && !HpKeepDisabled()) {  // the HP before the rate changes (the max follows within a frame)
            armed = true;
            int hp = 0, max = 0;
            if (LocalHp(&hp, &max)) g_hp_keep.Arm(hp, max, Now());
        }
        if (!WriteField(row + 0x10, 4, want)) {
            all = false;
            continue;
        }
        if (logged_rule != state) {
            Log("phantom health: %s (SpEffect %u maxHpRate %g -> %g)",
                state == 1 ? "on, the game's 30 % cut" : state == 2 ? "lifted: full health in PvP" : "off, full health", id,
                double(v), double(want));
        }
    }
    if (all) logged_rule = state;
    return all;
}

void SeamlessRulesTick() {
    State& st = S();
    if (!st.init) {
        return;
    }
    const auto now = Clock::now();
    PhantomHpKeepTick();
    // Params: every 0.5 s until they are written, then every 2 s (a reload restores them).
    if (Seconds(st.last_param, now) >= (st.params_done ? 2.0 : 0.5)) {
        st.last_param = now;
        perf::Scope z(perf::Zone::SeamlessParams);
        const bool done = ApplyParamRules();
        ApplyPhantomHp();
        if (done && !st.params_done) {
            Log("param rules hold (%.1f s after start)", Seconds(st.start, now));
        }
        if (!done && !st.param_logged_wait && Seconds(st.start, now) > 60.0) {
            st.param_logged_wait = true;
            Log("params not found 60 s after start; still trying");
        }
        st.params_done = done;
    }
    if (g_dispatch_original && Seconds(st.last_emevd_log, now) >= 60.0) {
        st.last_emevd_log = now;
        std::size_t kinds;
        {
            std::lock_guard<std::mutex> lk(g_emevd_mu);
            kinds = g_emevd_seen.size();
        }
        Log("EMEVD: %llu instructions dispatched, %llu skipped, %zu distinct (event, instruction)",
            ull(g_emevd_calls.load()), ull(g_emevd_skipped.load()), kinds);
    }
}

std::uint64_t GameParamRow(const char* name, const char* type, std::uint32_t id) {
    ParamTable t;
    return FindParam(name, type, &t) ? Row(t, id) : 0;
}

std::vector<std::pair<std::uint32_t, std::uint64_t>> GameParamRows(const char* name, const char* type) {
    std::vector<std::pair<std::uint32_t, std::uint64_t>> out;
    ParamTable t;
    if (!FindParam(name, type, &t)) {
        return out;
    }
    out.reserve(t.rows);
    for (u32 i = 0; i < t.rows; ++i) {
        u32 id = 0;
        u64 data = 0;
        if (Record(t, i, &id, &data)) {
            out.emplace_back(id, data);
        }
    }
    return out;
}

} // namespace coop
