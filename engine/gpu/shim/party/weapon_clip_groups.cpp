// SPDX-License-Identifier: GPL-3.0-or-later
// weapon_clip_groups: the one owner of the clip-group hook (docs/modding/weapon_clip_groups.md). Offsets are ours
// (bbhost - 0x400000), 1.09 eboot.
//
// The game (1.09, disassembly, every branch of the cases used here read):
// - 0x1A1AF00 (SprjHkBehAnimIdOffsetAdapter) gives a CustomManualSelectorGenerator its anim id offset: edi = the
//   selector's offsetType, esi = its anim id, rdx = the chr (0: returns 0). offsetType 13 reads the right weapon's
//   category from the chr's weapon record ([[[chr+0x3B0]+0x30]+8] = the ChrIns, record at +0xF0, right at +8, left at
//   +4; no record: -1), 14 the left one. A category 20..48 goes through the legacy table at 0x1A1B384 / 0x1A1B310,
//   any other is used as the group: eax = category * 1000000 (0x1A1B05C).
// - The record's categories are written at 0x18F5615 (left: ChrAsm (ChrIns+0x4B8) id at +0x64+4*(2*[+0xC])) and from
//   0x1894990(ChrAsm, -1) (right: the override weapon [ChrAsm+0xB8] when >= 0, else id at +0x64+4*(2*[+0x10]+1)),
//   each the EquipParamWeapon row's wepmotionCategory (byte +0xE3).
//
// So: a prologue replacement on 0x1A1AF00 runs the game's function, then for offsetType 13 / 14 reads that hand's
// weapon id the same way and, when the mod table lists it, returns clip_group * 1000000 instead (what a category of
// that number would give). Everything else that reads the category (the hks, the trick code) still sees the row's.
#include "weapon_clip_groups.h"
#include "weapon_clip_groups_rules.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_async_io.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace coop::clipgroups {
namespace {

constexpr u64 kAdapter = 0x1a1af00;

std::vector<Row> g_rows;  // set once before the game runs, read-only after
using AdapterFn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64);
AdapterFn g_adapter = nullptr;
bool g_verbose = false;
std::atomic<int> g_logged{0};

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("weapon_clip_groups: ") + line + "\n");
}

/// The weapon id the game reads for offsetType 13 (right) / 14 (left) of `chr`, or -1.
std::int32_t HandWeapon(u64 chr, int type) {
    u64 beh = 0, sub = 0, ins = 0, record = 0, chrasm = 0;
    if (!SafeGet(chr + 0x3b0, &beh) || !beh || !SafeGet(beh + 0x30, &sub) || !sub || !SafeGet(sub + 8, &ins) || !ins ||
        !SafeGet(ins + 0xf0, &record) || !record || !SafeGet(ins + 0x4b8, &chrasm) || !chrasm) {
        return -1;  // no weapon record: the game reads -1 too
    }
    std::int32_t slot = 0, id = -1;
    if (type == 13) {
        std::int32_t override_id = -1;
        if (SafeGet(chrasm + 0xb8, &override_id) && override_id >= 0) return override_id;
        if (!SafeGet(chrasm + 0x10, &slot) || slot < 0 || slot > 2) return -1;
        slot = 2 * slot + 1;
    } else {
        if (!SafeGet(chrasm + 0xc, &slot) || slot < 0 || slot > 2) return -1;
        slot = 2 * slot;
    }
    if (!SafeGet(chrasm + 0x64 + 4 * u64(slot), &id)) return -1;
    return id;
}

BB_COOP_SYSV u64 Adapter(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    const u64 r = g_adapter(a0, a1, a2, a3, a4, a5);
    const int type = int(std::int32_t(a0));
    if ((type != 13 && type != 14) || !a2 || g_rows.empty()) return r;
    const std::int32_t weapon = HandWeapon(a2, type);
    const std::int32_t group = GroupOf(g_rows, weapon);
    if (!group) return r;
    const std::int32_t offset = group * 1000000;
    if (g_logged.fetch_add(1) < (g_verbose ? 200 : 16)) {
        Log("chr 0x%llx %s weapon %d, anim %d: offset %d -> %d (clip group %d)", static_cast<unsigned long long>(a2),
            type == 13 ? "right" : "left", weapon, int(std::int32_t(a1)), int(std::int32_t(r)), offset, group);
    }
    return (r & ~0xffffffffull) | u64(std::uint32_t(offset));
}

std::string TablePath() {
    const char* o = std::getenv("BB_OUT_DIR");
    const char* d = std::getenv("BB_DATA_DIR");
    return (o && o[0] ? std::string(o) : std::string(d && d[0] ? d : ".") + "/out") + "/nightwatch_weapon_clip_groups.txt";
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
        Log("%s line %zu is not \"<weapon> <clip_group>\": no rows used", path.c_str(), bad);
        g_rows.clear();
    }
}

}  // namespace

void Install() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* off = std::getenv("BB_WEAPON_CLIP_GROUPS");
    if (off && off[0] == '0') {
        Log("feature weapon_clip_groups: not installed (BB_WEAPON_CLIP_GROUPS=0)");
        std::printf("[coop] feature weapon_clip_groups: not installed (BB_WEAPON_CLIP_GROUPS=0)\n");
        return;
    }
    const char* v = std::getenv("BB_WEAPON_CLIP_GROUPS_LOG");
    g_verbose = v && v[0] == '1';
    LoadTable();
    // Installed with or without rows (same hooks for every player; the rows are in the party hash, bb-start).
    void* original = nullptr;
    if (!ReplacePrologue(kAdapter, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, reinterpret_cast<const void*>(&Adapter),
                         &original, "weapon_clip_groups anim id offset adapter")) {
        std::printf("[coop] feature weapon_clip_groups: not installed (adapter 0x1a1af00 bytes differ)\n");
        Log("feature weapon_clip_groups: not installed (adapter 0x1a1af00 bytes differ)");
        return;
    }
    g_adapter = reinterpret_cast<AdapterFn>(original);
    std::printf("[coop] feature weapon_clip_groups: installed (%zu rows)\n", g_rows.size());
    Log("feature weapon_clip_groups: installed (%zu rows from %s)%s", g_rows.size(),
        g_rows.empty() ? "no mod table" : TablePath().c_str(), g_verbose ? ", BB_WEAPON_CLIP_GROUPS_LOG=1" : "");
}

}  // namespace coop::clipgroups
