// SPDX-License-Identifier: GPL-3.0-or-later
// hit_effects: the one owner of the hit-overlay hooks (docs/modding/hit_effects.md; reading notes in
// work/0.3.0/ITEMS.md "F3 reading"). Offsets are ours (bbhost - 0x400000), 1.09 eboot.
//
// The game (1.09, disassembly):
// - 0x1A2A770 is the hit handler (rdi = the defender's damage module, its chr at +8; rsi = the other chr of the hit,
//   the attacker, may be null; rdx = the hit record). It is the only caller of the overlay starters 0x1897400 (slots
//   0-2), 0x1897AD0 (Bolt, slots 10-12) and 0x1897760 (slots 20-22), which pick a free overlay record on the
//   defender (chr+0x2A0) and tail-jump to 0x189CE60.
// - 0x189CE60 writes the overlay's FX id to record+0x20 (three stores, one per slot range: 800 / 880 / 806 families,
//   variant by a byte at +0x140 of a chr sub-object, +1 from a chr vfunc) and spawns the FX from it before it returns.
//
// So: a prologue replacement on 0x1A2A770 keeps the attacker for the length of the hit (thread-local), and a value
// filter right after each id store swaps record+0x20 when a table row matches the attacker's active SpEffects.
// Visual class: every client runs this for the hits its own game draws, from SpEffects it already has.
//
// Draw rows (an overlay for a spAttribute the game draws nothing for, e.g. 3 Arcane): 0x1A2A770 tests the hit
// record's merged spAttribute ([rbx+0x24], rbx = the hit record = its rdx) only for 2 (0x1A2DD5F -> 0x1897400) and 6
// (0x1A2DE3D -> 0x1897AD0), each behind the same gate: [rec+0x22C] > 0, then ([rbp-0x31C] > 0 or [rec+0x7C] >= 0)
// ([rbp-0x31C] = defender chr ->+0x3B0 ->+0x20 ->+0xF8, loaded at 0x1A2A7A8), then byte [rec+0x1B4] == 0; the starter
// gets (chr+0x2A0, chr, rec+0x180) with chr = defender chr ([rdi+8] of the handler). Inside the dispatch no 5-byte
// site keeps the cmp/jne flags (HookValueFilter does not preserve them) and jumps land on its instructions, so the
// draw runs after the handler returns, with the same gate on the same record, through the Bolt starter 0x1897AD0;
// its id store (slots 10-19) then takes the row's fx, and the filter's result (stored at record+0x24, played at
// 0x189E3DD as sound type 5 with no "none" check) is the row's sound.
#include "hit_effects.h"
#include "hit_effects_rules.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_async_io.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace coop::hiteffects {
namespace {

constexpr u64 kHitHandler = 0x1a2a770;
// The three `mov [r12+0x20], r13d; add eax, imm32` in 0x189CE60; the filter resumes at the next `mov [r12+0x24], eax`.
struct Store {
    u64 off;
    u8 add_imm[4];
    const char* name;
};
constexpr Store kStores[] = {
    {0x189cf70, {0xdc, 0x1e, 0x00, 0x00}, "hit_effects overlay id (slots 0-9, fire 800)"},
    {0x189d0ab, {0x60, 0x22, 0x00, 0x00}, "hit_effects overlay id (slots 10-19, bolt 880)"},
    {0x189d209, {0xdc, 0x1e, 0x00, 0x00}, "hit_effects overlay id (slots 20-29, fire 806)"},
};
// HookValueFilter's stub saves rax..r11 and then the game's r12 (last push), and uses r12 as its own frame pointer
// (coop_hooks.cpp SaveVolatile): load_args must read the game's r12 from [r12], not r12 itself (alpha.1 passed the
// stub's save area and read r8's saved -1 as the "overlay id").
constexpr u8 kLoadRecord[] = {0x49, 0x8b, 0x3c, 0x24, 0x48, 0x89, 0xc6};  // mov rdi, [r12]; mov rsi, rax

constexpr u64 kBoltStarter = 0x1897ad0;

std::vector<Row> g_rows;  // set once before the game runs, read-only after
bool g_draw_rows = false;
thread_local u64 t_attacker = 0;
thread_local const Row* t_draw = nullptr;  // the draw row while our own call of the Bolt starter runs
using HandlerFn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64);
HandlerFn g_handler = nullptr;
using StarterFn = u64(BB_COOP_SYSV*)(u64, u64, u64);
bool g_verbose = false;
std::atomic<int> g_logged{0};

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("hit_effects: ") + line + "\n");
}

/// The chr has SpEffect `id` on (ChrIns +0x1C8: first node at +8, id +0x40, next +0x58; the hit handler walks the
/// same list at 0x1A2DD94).
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

u64 LocalPlayer() {
    u64 wcm = 0, player = 0;
    if (SafeGet(Guest(0x553e878), &wcm) && wcm) SafeGet(wcm + 0x60, &player);
    return player;
}

/// After the game's hit handler: a draw row for the record's spAttribute, behind the game's own element gate
/// (header comment), starts the overlay through the Bolt starter.
void DrawAfterHit(u64 damage_module, u64 attacker, u64 rec) {
    std::int32_t sp = 0, dealt = 0, guard = 0;
    u8 skip = 0;
    if (!rec || !SafeGet(rec + 0x24, &sp)) return;
    const Row* row = DrawRow(g_rows, sp);
    if (!row) return;
    u64 chr = 0, a = 0, b = 0;
    std::int32_t level = 0;
    if (!SafeGet(rec + 0x22c, &dealt) || dealt <= 0) return;
    if (!SafeGet(damage_module + 8, &chr) || !chr) return;
    const bool level_ok = SafeGet(chr + 0x3b0, &a) && a && SafeGet(a + 0x20, &b) && b && SafeGet(b + 0xf8, &level);
    if (!level_ok) return;
    if (level <= 0 && (!SafeGet(rec + 0x7c, &guard) || guard < 0)) return;
    if (!SafeGet(rec + 0x1b4, &skip) || skip != 0) return;
    if (g_verbose && g_logged.fetch_add(1) < 200) {
        Log("draw: spAttribute %d, dealt %d -> overlay %d, sound %d (attacker 0x%llx%s)", sp, dealt, row->fx, row->sound,
            static_cast<unsigned long long>(attacker), attacker && attacker == LocalPlayer() ? ", local player" : "");
    }
    const Row* outer = t_draw;
    t_draw = row;
    reinterpret_cast<StarterFn>(Guest(kBoltStarter))(chr + 0x2a0, chr, rec + 0x180);
    t_draw = outer;
}

BB_COOP_SYSV u64 HitHandler(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    const u64 outer = t_attacker;
    t_attacker = a1;
    const u64 r = g_handler(a0, a1, a2, a3, a4, a5);
    if (g_draw_rows) DrawAfterHit(a0, a1, a2);
    t_attacker = outer;
    return r;
}

/// After `mov [r12+0x20], r13d; add eax, imm`: rdi = the overlay record (the game's r12), rsi = rax (returned as it
/// was). `Slot` is the store (0: slots 0-9, 1: slots 10-19, 2: slots 20-29) for the log.
template <int Slot>
BB_COOP_SYSV u64 PickFilter(u64 record, u64 rax) {
    static const char* const kSlot[] = {"0-9", "10-19", "20-29"};
    std::int32_t id = 0;
    if (!SafeGet(record + 0x20, &id)) return rax;
    if (Slot == 1 && t_draw) {  // our own draw (DrawAfterHit): the row's overlay and sound
        const std::int32_t fx = t_draw->fx;
        SafeWrite(record + 0x20, &fx, sizeof fx);
        if (g_verbose && g_logged.fetch_add(1) < 200) {
            Log("store %s: draw overlay %d -> %d, sound %llu -> %d", kSlot[Slot], id, fx,
                static_cast<unsigned long long>(rax & 0xffffffffu), t_draw->sound);
        }
        return static_cast<u64>(static_cast<std::uint32_t>(t_draw->sound));
    }
    const u64 attacker = t_attacker;
    if (!Watched(g_rows, id)) {
        if (g_verbose && g_logged.fetch_add(1) < 200) {
            Log("store %s: overlay %d, attacker 0x%llx%s: no row", kSlot[Slot], id, static_cast<unsigned long long>(attacker),
                attacker && attacker == LocalPlayer() ? " (local player)" : "");
        }
        return rax;
    }
    std::int32_t speffect = 0;
    const std::int32_t fx = Pick(g_rows, id, [&](std::int32_t sp) { return HasSpEffect(attacker, sp); }, &speffect);
    if (fx) SafeWrite(record + 0x20, &fx, sizeof fx);
    if ((fx || g_verbose) && g_logged.fetch_add(1) < (g_verbose ? 200 : 32)) {
        if (fx) {
            Log("store %s: overlay %d -> %d (attacker 0x%llx%s has SpEffect %d)", kSlot[Slot], id, fx,
                static_cast<unsigned long long>(attacker),
                attacker == LocalPlayer() ? ", local player" : "", speffect);
        } else {
            Log("store %s: overlay %d, attacker 0x%llx%s: no row's SpEffect is on", kSlot[Slot], id,
                static_cast<unsigned long long>(attacker),
                attacker && attacker == LocalPlayer() ? " (local player)" : "");
        }
    }
    return rax;
}

std::string TablePath() {
    const char* o = std::getenv("BB_OUT_DIR");
    const char* d = std::getenv("BB_DATA_DIR");
    return (o && o[0] ? std::string(o) : std::string(d && d[0] ? d : ".") + "/out") + "/nightwatch_hit_effects.txt";
}

void LoadTable() {
    const char* mods = std::getenv("BB_MODS_ENABLED");
    if (mods && mods[0] == '0') return;
    const std::string path = TablePath();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    std::size_t n = 0;
    while (text.size() <= (1u << 20) && (n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    std::size_t bad = 0;
    if (text.size() > (1u << 20) || !ParseTable(text, &g_rows, &bad)) {
        Log("%s line %zu is not \"<attacker_speffect> <base_fx> <fx>\": no rows used", path.c_str(), bad);
        g_rows.clear();
    }
}

}  // namespace

void Install() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* off = std::getenv("BB_HIT_EFFECTS");
    if (off && off[0] == '0') {
        Log("feature hit_effects: not installed (BB_HIT_EFFECTS=0)");
        std::printf("[coop] feature hit_effects: not installed (BB_HIT_EFFECTS=0)\n");
        return;
    }
    const char* v = std::getenv("BB_HIT_EFFECTS_LOG");
    g_verbose = v && v[0] == '1';
    LoadTable();
    g_draw_rows = AnyDrawRow(g_rows);
    // Installed with or without rows: the party version check hashes the installed hooks, and a visual feature must
    // not split players whose mods differ.
    void* original = nullptr;
    if (!ReplacePrologue(kHitHandler, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, reinterpret_cast<const void*>(&HitHandler),
                         &original, "hit_effects hit handler")) {
        std::printf("[coop] feature hit_effects: not installed (hit handler 0x1a2a770 bytes differ)\n");
        Log("feature hit_effects: not installed (hit handler 0x1a2a770 bytes differ)");
        return;
    }
    g_handler = reinterpret_cast<HandlerFn>(original);
    int filters = 0;
    const ValueFilter fns[] = {&PickFilter<0>, &PickFilter<1>, &PickFilter<2>};
    for (int i = 0; i < 3; ++i) {
        const Store& s = kStores[i];
        const auto& a = s.add_imm;
        const auto& l = kLoadRecord;
        // original: mov [r12+0x20], r13d; add eax, imm32
        if (HookValueFilter(s.off, {0x45, 0x89, 0x6c, 0x24, 0x20, 0x05, a[0], a[1], a[2], a[3]},
                            {l[0], l[1], l[2], l[3], l[4], l[5], l[6]}, fns[i], s.off + 10, s.name)) {
            ++filters;
        }
    }
    if (filters == 0) {
        std::printf("[coop] feature hit_effects: not installed (overlay id stores in 0x189ce60 differ)\n");
        Log("feature hit_effects: not installed (overlay id stores in 0x189ce60 differ)");
        return;
    }
    std::printf("[coop] feature hit_effects: installed (%d of 3 id stores, %zu rows)\n", filters, g_rows.size());
    Log("feature hit_effects: installed (%d of 3 id stores, %zu rows from %s)%s", filters, g_rows.size(),
        g_rows.empty() ? "no mod table" : TablePath().c_str(), g_verbose ? ", BB_HIT_EFFECTS_LOG=1" : "");
}

}  // namespace coop::hiteffects
