// SPDX-License-Identifier: GPL-3.0-or-later
// animated_attachments: the one owner of the in-world part slots 15 and 16 (docs/modding/animated_attachments.md).
// Offsets are ours (bbhost - 0x400000), 1.09 eboot; read from llvm-objdump of engine/out/eboot.elf (20 Nov 2026).
//
// How the game shows a character's equipment (disassembly):
// - ChrIns +0x4C0 is the parts manager (22 entries of 0x58 bytes from +0x30: +0 current id, +4 wanted id, +8 the
//   current resource cap, +0x10 the pending one). 0x1CA9790(manager, ChrAsm (ChrIns +0x4B8), ...) runs every frame
//   (a frame countdown at +8; headless: 600 calls in 3.9 s for the local player): for a changed entry it builds the
//   name (0x1F29D30: "HD_" + "A_" / "M_" / "F_" + %04d, + "_L" when its 6th argument (r9b) is set, for armor) and
//   requests "parts:/<name>.partsbnd" through 0x1422A80(*0x553B118, wchar16 path, listener): a refcounted cap (+0x60
//   count, +0x88 the parts resource once loaded). An old cap is released by 0xF5D2B0(*0x54B1D10, cap, 0x556D578, 1)
//   (deferred), else the repository's vfunc +0x30.
// - Right after it, the same ChrIns step (0x18F84F0 at 0x18F875A; also 0x18F42F3, 0x1DABF6A, 0x1DAC56B) runs
//   0x1C9CD20(parts = ChrIns +0x4D0, ChrAsm, manager): 24 calls of 0x1C9D070(parts, slot, ChrAsm, manager, entry),
//   slot -> entry 0-0, 1-14, 2..6-1..5, 7..10-6, 11..14-7, 15-8, 16-9, 17..23-15..21. The slot names (table
//   0x534AC80): Face, Head, Helmet, Armor, Gauntlet, Leggings, Hair, Left0-3, Right0-3, ArrowCase, BoltPouch, EyeL,
//   EyeR, Eyebrow, Eyelash, Beard, Accessories, Decal. Entries 8 / 9 read ChrAsm equip index 4 / 5 (quiver models
//   Bloodborne never equips), so slots 15 and 16 stay empty.
// - 0x1C9D070 reads only the entry's cap from the manager ([manager +0x38 + 0x58*entry]; the weapon entries 6 / 7 also
//   +0x28C / +0x2E4): no cap or no resource -> the slot's model (parts +0x28 + 8*slot) is destroyed (0x1C9EF60, vdtor,
//   free); else slots 15 / 16 take the general path the Helmet slot takes: a model of 0x170 bytes (vtable 0x539D100)
//   for model index 0 with its animation player when the part has an anibnd (0x1D65690), kept while +0x148 (the
//   resource) and +0x150 (index) stay the same.
//
// So: a pre-hook on 0x1CA9790 (the game's own request context) requests and releases our caps per player character;
// the calls of 0x1C9D070 for slots 15 and 16 (0x1C9CEB8, 0x1C9CED1) see our cap in their entry for the length of the
// call (written there and put back), so the game builds, animates and destroys the model itself.
// Cosmetic: every client does this for the characters its own game has, from the SpEffects it already sees.
#include "animated_attachments.h"
#include "animated_attachments_rules.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_async_io.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace coop::attachments {
namespace {

using Clock = std::chrono::steady_clock;

constexpr u64 kPartsManagerUpdate = 0x1ca9790;
constexpr u64 kSlotLoad = 0x1c9d070;     // (parts, slot, ChrAsm, manager, entry)
constexpr u64 kRequest = 0x1422a80;      // (repository, wchar16 path, listener) -> cap
constexpr u64 kDeferRelease = 0xf5d2b0;  // (*0x54b1d10, cap, 0x556d578, 1) -> posted
constexpr u64 kRepositorySlot = 0x553b118;
constexpr u64 kReleaseQueueSlot = 0x54b1d10;
constexpr u64 kReleaseType = 0x556d578;
constexpr u64 kWorldChrMan = 0x553e878;  // +0x60 the local PlayerIns, +0x30 count, +0x38 the player set (0x38 apart)
constexpr int kSlots[kSlotsPerChr] = {15, 16};
constexpr int kEntries[kSlotsPerChr] = {8, 9};  // the manager entries 0x1C9CD20 gives those slots
constexpr u64 kSlotCalls[kSlotsPerChr] = {0x1c9ceb8, 0x1c9ced1};
// mov esi, slot; mov r8d, entry; mov rdi, r14; mov rdx, r15; mov rcx, r12 (then the call)
constexpr u8 kSlotCallSetup[kSlotsPerChr][20] = {
    {0xbe, 0x0f, 0, 0, 0, 0x41, 0xb8, 0x08, 0, 0, 0, 0x4c, 0x89, 0xf7, 0x4c, 0x89, 0xfa, 0x4c, 0x89, 0xe1},
    {0xbe, 0x10, 0, 0, 0, 0x41, 0xb8, 0x09, 0, 0, 0, 0x4c, 0x89, 0xf7, 0x4c, 0x89, 0xfa, 0x4c, 0x89, 0xe1},
};
constexpr std::size_t kSlotCallSetupLen = 20;

using RequestFn = u64(BB_COOP_SYSV*)(u64, const char16_t*, u64);
using DeferFn = bool(BB_COOP_SYSV*)(u64, u64, u64, std::uint32_t);
using ReleaseFn = void(BB_COOP_SYSV*)(u64, u64);
using SlotLoadFn = void(BB_COOP_SYSV*)(u64, u64, u64, u64, u64);

enum class St { Empty, Requested, Loaded, Unloading, Releasing };

struct Slot {
    St st = St::Empty;
    int row = -1;
    u64 cap = 0;
    bool low = false;
    bool warned = false;
    Clock::time_point since{};
};

struct Chr {
    u64 mgr = 0, chr = 0, parts = 0;
    bool local = false;
    Clock::time_point seen{};
    Slot slot[kSlotsPerChr];
};

std::vector<Row> g_rows;  // set once before the game runs, read-only after
bool g_test = false;      // BB_ANIMATED_ATTACHMENTS_TEST: row 0 (speffect 0) is on for the local player
int g_test_seconds = 0;   // BB_ANIMATED_ATTACHMENTS_TEST_SECONDS: > 0 turns row 0 off that long after its first use
bool g_verbose = false;   // BB_ANIMATED_ATTACHMENTS_LOG=1: each link of the chain (requests, slot loader sightings)
std::atomic<int> g_verbose_logged{0};
std::recursive_mutex g_mu;
std::vector<Chr> g_chrs;  // under g_mu
std::atomic<int> g_no_slot_logged{0};

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("animated_attachments: ") + line + "\n");
}

/// The chr has SpEffect `id` on (ChrIns +0x1C8: first node at +8, id +0x40, next +0x58; hit_effects.cpp).
bool HasSpEffect(u64 chr, std::int32_t id) {
    u64 list = 0, node = 0;
    if (!chr || !SafeGet(chr + 0x1c8, &list) || !list || !SafeGet(list + 8, &node)) return false;
    for (int guard = 0; node && guard < 512; ++guard) {
        std::int32_t nid = 0;
        if (!SafeGet(node + 0x40, &nid)) return false;
        if (nid == id) return true;
        if (!SafeGet(node + 0x58, &node)) return false;
    }
    return false;
}

/// The player characters this game has (local first), at most 8.
int Players(u64* out, u64* local) {
    u64 wcm = 0, me = 0, ents = 0;
    int n = 0, count = 0;
    *local = 0;
    if (!SafeGet(Guest(kWorldChrMan), &wcm) || !wcm) return 0;
    if (SafeGet(wcm + 0x60, &me) && me) out[n++] = *local = me;
    if (SafeGet(wcm + 0x30, &count) && SafeGet(wcm + 0x38, &ents) && ents) {
        for (int i = 0; i < count && i < 8 && n < 8; ++i) {
            u64 chr = 0;
            if (!SafeGet(ents + u64(i) * 0x38, &chr) || !chr || chr == me) continue;
            out[n++] = chr;
        }
    }
    return n;
}

const char* Who(const Chr& c) { return c.local ? "local player" : "remote player"; }

/// The cap's parts resource once loaded (0x1C9D070 tests the same).
u64 Resource(u64 cap) {
    u64 res = 0;
    return cap && SafeGet(cap + 0x88, &res) ? res : 0;
}

void Release(u64 cap) {
    if (!cap) return;
    u64 queue = 0, repo = 0, vt = 0, fn = 0;
    if (SafeGet(Guest(kReleaseQueueSlot), &queue) && queue &&
        reinterpret_cast<DeferFn>(Guest(kDeferRelease))(queue, cap, Guest(kReleaseType), 1))
        return;
    if (SafeGet(Guest(kRepositorySlot), &repo) && repo && SafeGet(repo, &vt) && vt && SafeGet(vt + 0x30, &fn) && fn)
        reinterpret_cast<ReleaseFn>(fn)(repo, cap);
}

/// Releases the slot's cap and empties it. `why` non-empty: one log line (a cap dropped before or without an unload).
void ReleaseSlot(Chr& c, Slot& s, const char* why) {
    if (s.cap) {
        Release(s.cap);
        if (why[0])
            Log("%s (chr 0x%llx) slot %d: %s%s -> released while %s (%s)", Who(c), static_cast<unsigned long long>(c.chr),
                kSlots[&s - c.slot], g_rows[s.row].part.c_str(), s.low ? "_L" : "",
                s.st == St::Requested ? "still loading" : "in the slot", why);
    }
    s = Slot{};
}

/// Row 0 of BB_ANIMATED_ATTACHMENTS_TEST: on for the local player, for BB_ANIMATED_ATTACHMENTS_TEST_SECONDS if set.
bool TestRowOn(bool local, Clock::time_point now) {
    if (!g_test || !local) return false;
    static Clock::time_point first{};
    if (first == Clock::time_point{}) first = now;
    return g_test_seconds <= 0 || now - first < std::chrono::seconds(g_test_seconds);
}

/// 0x1CA9790's pre-hook: rdi = the parts manager, r9 = its low-set flag. Requests and releases our caps.
BB_COOP_SYSV void PartsManagerUpdate(u64 mgr, u64, u64, u64, u64, u64 low_flag) {
    if (g_rows.empty() || !mgr) return;
    u64 players[8], local = 0;
    const int n = Players(players, &local);
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    const Clock::time_point now = Clock::now();
    // Characters that are gone (not a player any more, or the address now has another manager) and not updated for
    // 3 s: their part models were torn down with them, only the caps are left to release.
    for (std::size_t i = 0; i < g_chrs.size();) {
        Chr& c = g_chrs[i];
        bool alive = false;
        for (int k = 0; k < n && !alive; ++k) {
            u64 m = 0;
            alive = players[k] == c.chr && SafeGet(c.chr + 0x4c0, &m) && m == c.mgr;
        }
        if (alive || now - c.seen < std::chrono::seconds(3)) {
            ++i;
            continue;
        }
        for (Slot& s : c.slot) ReleaseSlot(c, s, "character gone");
        g_chrs.erase(g_chrs.begin() + std::ptrdiff_t(i));
    }
    u64 chr = 0;
    for (int k = 0; k < n && !chr; ++k) {
        u64 m = 0;
        if (SafeGet(players[k] + 0x4c0, &m) && m == mgr) chr = players[k];
    }
    if (!chr) return;
    Chr* c = nullptr;
    for (Chr& e : g_chrs)
        if (e.mgr == mgr) c = &e;
    if (c && c->chr != chr) {  // the manager now belongs to another chr: start over
        for (Slot& s : c->slot) ReleaseSlot(*c, s, "character changed");
        c->parts = 0;
    }
    int dropped = 0;
    const std::vector<int> want = Wanted(
        g_rows,
        [&](std::int32_t sp) { return sp == 0 ? TestRowOn(chr == local, now) : HasSpEffect(chr, sp); }, &dropped);
    if (!c) {
        if (want.empty()) return;
        g_chrs.push_back(Chr{});
        c = &g_chrs.back();
        c->mgr = mgr;
    }
    c->chr = chr;
    c->local = chr == local;
    c->seen = now;
    if (g_test && c->local) {  // diagnostic: how often the game runs the manager (the requests ride on it)
        static int calls = 0;
        static Clock::time_point first = now;
        if (++calls == 600)
            Log("test: parts manager ran 600 times for the local player in %.1f s",
                std::chrono::duration<double>(now - first).count());
    }
    const bool low = (low_flag & 0xff) != 0;
    for (Slot& s : c->slot) {
        if (s.st == St::Empty) continue;
        if (s.st == St::Releasing) {  // the slot hook unloaded it
            ReleaseSlot(*c, s, "");
            continue;
        }
        bool listed = false;
        for (int r : want) listed = listed || r == s.row;
        const bool keep = listed && s.low == low;
        if (keep) continue;
        if (s.st == St::Requested) ReleaseSlot(*c, s, s.low != low ? "low set changed" : "SpEffect off");
        else if (s.st == St::Loaded) s.st = St::Unloading;  // the slot hook unloads it, then we release
    }
    for (int r : want) {
        bool has = false;
        for (const Slot& s : c->slot) has = has || (s.st != St::Empty && s.row == r);
        if (has) continue;
        Slot* free_slot = nullptr;
        for (Slot& s : c->slot)
            if (!free_slot && s.st == St::Empty) free_slot = &s;
        if (!free_slot) continue;  // a slot is still unloading: next frame
        u64 repo = 0;
        if (!SafeGet(Guest(kRepositorySlot), &repo) || !repo) return;
        const std::u16string path = ResourcePath(g_rows[r].part, low);
        const u64 cap = reinterpret_cast<RequestFn>(Guest(kRequest))(repo, path.c_str(), 0);
        if (!cap) {
            Log("%s (chr 0x%llx) slot %d: parts:/%s%s.partsbnd -> request failed (no resource)", Who(*c),
                static_cast<unsigned long long>(chr), kSlots[free_slot - c->slot], g_rows[r].part.c_str(),
                low ? "_L" : "");
            continue;
        }
        *free_slot = Slot{St::Requested, r, cap, low, false, now};
        if (g_verbose && g_verbose_logged.fetch_add(1) < 200)
            Log("%s (chr 0x%llx, manager 0x%llx) slot %d: requested parts:/%s%s.partsbnd -> cap 0x%llx", Who(*c),
                static_cast<unsigned long long>(chr), static_cast<unsigned long long>(mgr), kSlots[free_slot - c->slot],
                g_rows[r].part.c_str(), low ? "_L" : "", static_cast<unsigned long long>(cap));
    }
    if (dropped > 0 && g_no_slot_logged.fetch_add(1) < 8)
        Log("%s (chr 0x%llx): %d more attachment row(s) have their SpEffect on, but only %d slots exist: not shown",
            Who(*c), static_cast<unsigned long long>(chr), dropped, kSlotsPerChr);
    bool empty = true;
    for (const Slot& s : c->slot) empty = empty && s.st == St::Empty;
    if (empty) g_chrs.erase(g_chrs.begin() + (c - g_chrs.data()));
}

/// A call of 0x1C9D070 for slot 15 / 16 (`I` = 0 / 1): our cap in the manager entry for the length of the call.
template <int I>
BB_COOP_SYSV void SlotLoad(u64 parts, u64 slot, u64 chrasm, u64 mgr, u64 entry) {
    const auto original = reinterpret_cast<SlotLoadFn>(Guest(kSlotLoad));
    if (g_rows.empty() || !mgr || !parts) return original(parts, slot, chrasm, mgr, entry);
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    Chr* c = nullptr;
    for (Chr& e : g_chrs)
        if (e.mgr == mgr) c = &e;
    if (!c) return original(parts, slot, chrasm, mgr, entry);
    if (g_verbose && c->parts != parts && g_verbose_logged.fetch_add(1) < 200)
        Log("%s (chr 0x%llx): slot %d call on parts 0x%llx (manager 0x%llx)", Who(*c),
            static_cast<unsigned long long>(c->chr), kSlots[I], static_cast<unsigned long long>(parts),
            static_cast<unsigned long long>(mgr));
    c->parts = parts;
    Slot& s = c->slot[I];
    const u64 cell = mgr + 0x38 + 0x58 * u64(kEntries[I]);
    const u64 model_cell = parts + 0x28 + 8 * u64(kSlots[I]);
    if (s.st == St::Unloading) {
        original(parts, slot, chrasm, mgr, entry);  // the entry as the game has it (empty): the game destroys the model
        u64 model = 0;
        SafeGet(model_cell, &model);
        s.st = St::Releasing;
        Log("%s (chr 0x%llx) slot %d: %s%s -> unloaded (SpEffect %d off%s)", Who(*c),
            static_cast<unsigned long long>(c->chr), kSlots[I], g_rows[s.row].part.c_str(), s.low ? "_L" : "",
            g_rows[s.row].speffect, model ? "; the game kept a model there: its own entry is not empty" : "");
        return;
    }
    const u64 res = (s.st == St::Requested || s.st == St::Loaded) ? Resource(s.cap) : 0;
    if (!res) {
        if (s.st == St::Requested && !s.warned && Clock::now() - s.since > std::chrono::seconds(10)) {
            s.warned = true;
            std::uint8_t state = 0;
            SafeGet(s.cap + 0x78, &state);
            Log("%s (chr 0x%llx) slot %d: parts:/%s%s.partsbnd -> not loaded after 10 s (cap state %u; is the file in "
                "the mod's parts folder?)",
                Who(*c), static_cast<unsigned long long>(c->chr), kSlots[I], g_rows[s.row].part.c_str(),
                s.low ? "_L" : "", unsigned(state));
        }
        return original(parts, slot, chrasm, mgr, entry);
    }
    u64 own = 0;
    if (!SafeGet(cell, &own)) return original(parts, slot, chrasm, mgr, entry);
    SafeWrite(cell, &s.cap, sizeof s.cap);
    original(parts, slot, chrasm, mgr, entry);
    SafeWrite(cell, &own, sizeof own);
    if (s.st == St::Requested) {
        u64 model = 0, model_res = 0;
        const bool made = SafeGet(model_cell, &model) && model && SafeGet(model + 0x148, &model_res) && model_res == res;
        s.st = St::Loaded;
        Log("%s (chr 0x%llx) slot %d: parts:/%s%s.partsbnd -> %s (SpEffect %d%s)", Who(*c),
            static_cast<unsigned long long>(c->chr), kSlots[I], g_rows[s.row].part.c_str(), s.low ? "_L" : "",
            made ? "loaded" : "NOT built (the slot stayed empty)", g_rows[s.row].speffect,
            own ? "; the game's own entry held a cap, hidden while ours shows" : "");
    }
}

std::string TablePath() {
    const char* o = std::getenv("BB_OUT_DIR");
    const char* d = std::getenv("BB_DATA_DIR");
    return (o && o[0] ? std::string(o) : std::string(d && d[0] ? d : ".") + "/out") +
           "/nightwatch_animated_attachments.txt";
}

void LoadTable() {
    const char* mods = std::getenv("BB_MODS_ENABLED");
    if (!(mods && mods[0] == '0')) {
        const std::string path = TablePath();
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            std::string text;
            char buf[4096];
            std::size_t n = 0;
            while (text.size() <= (1u << 20) && (n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
            std::size_t bad = 0;
            if (text.size() > (1u << 20) || !ParseTable(text, &g_rows, &bad)) {
                Log("%s line %zu is not \"<speffect> <PART>\": no rows used", path.c_str(), bad);
                g_rows.clear();
            }
        }
    }
    // Diagnostic: BB_ANIMATED_ATTACHMENTS_TEST=<part> wears that part on the local player at all times (row 0).
    const char* test = std::getenv("BB_ANIMATED_ATTACHMENTS_TEST");
    if (test && test[0]) {
        std::string part;
        for (const char* p = test; *p; ++p) part += (*p >= 'a' && *p <= 'z') ? char(*p - 'a' + 'A') : *p;
        if (ValidPart(part)) {
            g_rows.insert(g_rows.begin(), Row{0, part});
            if (g_rows.size() > kMaxRows) g_rows.resize(kMaxRows);
            g_test = true;
            if (const char* t = std::getenv("BB_ANIMATED_ATTACHMENTS_TEST_SECONDS")) g_test_seconds = std::atoi(t);
        }
    }
}

}  // namespace

void Install() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* off = std::getenv("BB_ANIMATED_ATTACHMENTS");
    if (off && off[0] == '0') {
        Log("feature animated_attachments: not installed (BB_ANIMATED_ATTACHMENTS=0)");
        std::printf("[coop] feature animated_attachments: not installed (BB_ANIMATED_ATTACHMENTS=0)\n");
        return;
    }
    const char* v = std::getenv("BB_ANIMATED_ATTACHMENTS_LOG");
    g_verbose = v && v[0] == '1';
    LoadTable();
    // Every code check first, so a 1.09 image that differs gets no hook at all.
    bool ok = MatchesOriginal(kPartsManagerUpdate, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57});
    for (int i = 0; i < kSlotsPerChr; ++i)
        ok = ok && CallsTo(kSlotCalls[i], kSlotLoad) &&
             Matches(kSlotCalls[i] - kSlotCallSetupLen, kSlotCallSetup[i], kSlotCallSetupLen);
    if (!ok) {
        std::printf("[coop] feature animated_attachments: not installed (code differs)\n");
        Log("feature animated_attachments: not installed (parts manager / part slot code differs)");
        return;
    }
    // Installed with or without rows: the party version check hashes the installed hooks, and a cosmetic feature must
    // not split players whose mods differ.
    int hooks = 0;
    hooks += HookPrologue(kPartsManagerUpdate, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &PartsManagerUpdate,
                          "animated_attachments parts manager") ? 1 : 0;
    hooks += HookCallSite(kSlotCalls[0], kSlotLoad, reinterpret_cast<const void*>(&SlotLoad<0>),
                          "animated_attachments slot 15 (ArrowCase)") ? 1 : 0;
    hooks += HookCallSite(kSlotCalls[1], kSlotLoad, reinterpret_cast<const void*>(&SlotLoad<1>),
                          "animated_attachments slot 16 (BoltPouch)") ? 1 : 0;
    if (hooks != 3) {
        g_rows.clear();  // a partial set: keep it inert
        std::printf("[coop] feature animated_attachments: not installed (%d of 3 hooks)\n", hooks);
        Log("feature animated_attachments: not installed (%d of 3 hooks; rows dropped)", hooks);
        return;
    }
    std::string list;
    for (const Row& r : g_rows) list += (list.empty() ? "" : ", ") + std::to_string(r.speffect) + " " + r.part;
    std::printf("[coop] feature animated_attachments: installed (3 hooks, %zu rows)\n", g_rows.size());
    Log("feature animated_attachments: installed (3 hooks, slots 15 16, %zu rows%s%s%s)", g_rows.size(),
        g_rows.empty() ? ": no mod table" : ": ", list.c_str(),
        !g_test ? "" : g_test_seconds > 0 ? "; BB_ANIMATED_ATTACHMENTS_TEST: row 0 on for the local player for a while"
                                        : "; BB_ANIMATED_ATTACHMENTS_TEST: row 0 always on for the local player");
}

}  // namespace coop::attachments
