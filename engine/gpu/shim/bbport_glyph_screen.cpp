// SPDX-License-Identifier: GPL-3.0-or-later
// The community Chalice glyph list as a native item-list screen (bbport_glyph_screen.h).
//
// The altar's "Search by Chalice Glyph" item (handler 0x1b687f0) opens the game's chalice picker (the "Chalice Ritual"
// handler 0x1b66ad0: the inventory frame, 4 tabs) as a screen of its own: the rows are the community glyphs, never the
// player's chalices, and the real Chalice Ritual stays as the game has it.
//
// The game's item-list builder 0x1c6ffc0 (shops, storage, inventory, the altar's chalice picker; the menu object in
// r15) makes each tab's rows in steps: the screen's source list (std::function at menu +0x1e50), the screen filter
// (menu +0x1e80) through the collector 0x1c5ffd0 into a fresh list, a per-row pass (menu +0x1eb0), then the tab filter
// (the tab descriptor's +0xf0) through the collector again (call 0x1c703e4). A tab descriptor: +0 the tab id (the
// picker's 0x24..0x27), +0x110 its filter functor, whose +8 is a plain function (0x1c686d0 / 750 / 7d0 / 850: the
// chalice's HolygrailParam family byte +4 == 0 / 1 / 2 / 3). A list object: +0x10 the row vector (begin +0x18, end
// +0x20), rows of 0xd8 bytes, push_back 0x1c64a10(list + 0x10, row). A row: +0x48 inventory position, +0x4c item id
// (0x40000000 | goods id), +0x50 kind (0xc for goods), +0x58 a dword the copies keep, +0x70 item handle
// (0xb0000000 | goods id; goods handles are plain numbers).
//
// On the glyph screen (the menu object the builder first sees after the glyph item opened the picker):
//  - after the screen filter, one row per glyph of the tab's category (farming / equipment / bloodgems / testing in
//    tab order): a copy of the player's first chalice row pointed at the glyph's chalice;
//  - around the tab filter's collector: the player's rows are turned away (kind 0 while it runs), ours carry a mark
//    in +0x58 (kMark | glyph index, kept by every later copy) and pass as a chalice of the tab's family; the kept
//    copies get their real chalice back;
//  - a card with the mark: the glyph code as its name (0x1c5d340 -> call 0x1c5d4cf to 0x17ca200), the glyph's text as
//    its long description (0x1c5dd70 -> call 0x1c5df17 to 0x1f01180).
// Test stage: BB_GLYPH_SCREEN_TEST=1 only.
#include "bbport_glyph_screen.h"

#include "party/coop_hooks.h"
#include "party/game_state.h"
#include "party/party_chalice_glyphdb.h"
#include "net/bbnet_internal.h"
#include "net/from_api.h"
#include "net/party_util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace BbGlyphScreen {
namespace {

using coop::u64;
using u32 = std::uint32_t;

constexpr u64 kGlyphItem = 0x1b687f0;          // (out, ctx): the altar's "Search by Chalice Glyph"
constexpr u64 kRitualItem = 0x1b66ad0;         // (out, ctx): the altar's "Chalice Ritual" (the chalice picker)
constexpr u64 kGlyphRowGrey = 0x1b667af;       // 88 8d a8 eb ff ff  mov [rbp-0x1458], cl (the glyph row greyed)
constexpr u64 kGlyphNetCheck = 0x1b6881f;      // 48 8b 03 48 85 c0  mov rax, [rbx]; test rax, rax (0x553b120)
constexpr u64 kGlyphNetChecks = 0x1b6884b;     // cmp byte [rax + 8], 0 (then +0x9f8, +0x9f6, +0xa50)
constexpr u64 kPicked = 0x1b748f0;             // (out, functor, ?, row, ctx): the picker's chosen chalice
constexpr u64 kSearchResults = 0x1b6a360;      // (out, ctx, ?, results): the Search Results screen step
constexpr u64 kJoinQuestion = 0x1b6bcf0;       // (out, -, ctx, result): "Join the selected Chalice Dungeon?"
constexpr u64 kResultDtor = 0x1b06760;         // a search result's destructor
constexpr u64 kResultSize = 0x398;
constexpr u64 kSearchNotice = 0x1b710e0;       // (out, functor): "Search using Chalice Glyph: <glyph>" (OK box)
constexpr u32 kGlyphSearchText = 262002;       // "Search by Chalice Glyph" (the altar item and its help line)
constexpr u64 kAfterScreenFilter = 0x1c701cc;  // 49 8b bf d0 1e 00 00  mov rdi, [r15 + 0x1ed0]
constexpr u64 kResume = 0x1c701d3;             // test rdi, rdi
constexpr u64 kTabCollectCall = 0x1c703e4;     // call 0x1c5ffd0 (the tab filter's collector)
constexpr u64 kCollect = 0x1c5ffd0;
constexpr u64 kNameCall = 0x1c5d4cf;           // call 0x17ca200 (a card's item name)
constexpr u64 kItemName = 0x17ca200;
constexpr u64 kDescBinder = 0x1c5dd70;         // (out, row): a row's long description
constexpr u64 kDescCall = 0x1c5df17;           // call 0x1f01180 (a goods row's description text)
constexpr u64 kGoodsDesc = 0x1f01180;
constexpr u64 kPushRow = 0x1c64a10;
constexpr u64 kHolygrailLookup = 0x1f1f720;    // (&{u32, u64 row}, goods id): the chalice's HolygrailParam row
constexpr u64 kTabFilters[4] = {0x1c686d0, 0x1c68750, 0x1c687d0, 0x1c68850};  // family 0..3
constexpr u64 kRowSize = 0xd8;
constexpr u32 kFirstPickerTab = 0x24, kLastPickerTab = 0x27;
constexpr u32 kMark = 0x47000000u;  // row +0x58 of our rows: kMark | glyph index (into *g_glyphs)
constexpr const char* kCategories[4] = {"farming", "equipment", "bloodgems", "testing"};  // tab order

using HandlerFn = u64(BB_COOP_SYSV*)(u64 out, u64 ctx);
using PickedFn = u64(BB_COOP_SYSV*)(u64 out, u64 functor, u64 a2, u64 row, u64 ctx);
using ResultsFn = u64(BB_COOP_SYSV*)(u64 out, u64 a1, u64 a2, u64 a3);
using ConfirmFn = u64(BB_COOP_SYSV*)(u64 out, u64 functor);
using PushRowFn = void(BB_COOP_SYSV*)(u64 vec, u64 row);
using CollectFn = u64(BB_COOP_SYSV*)(u64 out, u64 src_ref, u64 filter);
using NameFn = u64(BB_COOP_SYSV*)(u64 iid_at);
using DescFn = u64(BB_COOP_SYSV*)(u64 msg, u64 iid);

struct Glyph {
    u32 goods = 0;
    int category = 0;     // index into kCategories
    std::u16string name;  // the card's name: the glyph code (8 or 9 letters / digits)
    std::u16string desc;  // the long description
    std::string code;     // the glyph (8 letters / digits: the game's glyph box holds 8)
    std::string form;     // its FormData, base64 (what the search board holds)
};
using Generation = std::vector<Glyph>;

std::mutex g_mu;
// Every list read so far is kept: a card may still show a name of an older read (the pointers stay valid).
std::vector<std::unique_ptr<Generation>> g_generations;
const Generation* g_glyphs = nullptr;
std::atomic<bool> g_have_glyphs{false};  // the glyph screen is on (hooks in, community glyphs read); any thread
std::atomic<bool> g_installed{false};    // every hook of the glyph screen is in
std::atomic<bool> g_screen{false};       // the chalice picker is the glyph screen (from the glyph item to a real Chalice
                                         // Ritual): its title, help line and tab names are ours (Text)
std::filesystem::file_time_type g_seen{};
bool g_have_seen = false;
u32 g_family_goods[4] = {};  // a chalice of each HolygrailParam family (our rows pass the tab filters as one)

// The glyph screen: set by the glyph item, taken by the builder's first picker tab, cleared by a real Chalice Ritual.
bool g_opening = false;
bool g_from_glyph = false;
u64 g_glyph_menu = 0;
HandlerFn g_glyph_item_original = nullptr;
PickedFn g_picked_original = nullptr;

// A picked glyph: the game's glyph box (opened by the Search by Chalice Glyph step) is filled with it and confirmed.
std::string g_pick;           // "" when none
double g_pick_at = 0;         // when (seconds), the preset is good for kPickSeconds
constexpr double kPickSeconds = 15;
double g_skip_results_until = 0;      // the next Search Results screen within this time is our pick's (SearchResults)
constexpr double kSkipSeconds = 60;
ResultsFn g_results_original = nullptr;
thread_local bool t_picking = false;  // our call of the Search by Chalice Glyph step: its network check passes

double Seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Rows added for the tab being built, until its tab filter ran.
struct Pending {
    u64 list = 0;
    std::size_t first = 0;   // row index of the first added row (the rows before it are the player's)
    std::vector<int> glyph;  // per added row: index into *g_glyphs
    int family = -1;         // the tab filter's family
} g_pending;

struct ParamRef {
    u32 id = 0xffffffffu;
    u32 pad = 0;
    u64 row = 0;
};
using LookupFn = void(BB_COOP_SYSV*)(ParamRef* out, u32 id);
// The chalice's HolygrailParam family (0..3), -1 when the goods is no chalice. Game thread only.
int ChaliceFamily(u32 goods) {
    if (goods == 0 || goods >= 0x0fffffffu) return -1;
    ParamRef ref;
    reinterpret_cast<LookupFn>(coop::Guest(kHolygrailLookup))(&ref, goods);
    std::uint8_t family = 0xff;
    return ref.row && coop::SafeGet(ref.row + 4, &family) && family < 4 ? family : -1;
}

std::u16string Utf8To16(const std::string& s, std::size_t max_units) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size() && out.size() < max_units;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        u32 cp = '?';
        std::size_t n = 1;
        if (c < 0x80) cp = c;
        else if ((c & 0xe0) == 0xc0 && i + 1 < s.size()) cp = (c & 0x1fu) << 6 | (s[i + 1] & 0x3fu), n = 2;
        else if ((c & 0xf0) == 0xe0 && i + 2 < s.size())
            cp = (c & 0x0fu) << 12 | (s[i + 1] & 0x3fu) << 6 | (s[i + 2] & 0x3fu), n = 3;
        else if ((c & 0xf8) == 0xf0 && i + 3 < s.size()) cp = '?', n = 4;  // outside the game's fonts
        i += n;
        out.push_back(static_cast<char16_t>(cp));
    }
    return out;
}

std::filesystem::path CachePath() {
    if (const char* e = std::getenv("BB_CHALICE_GLYPHDB_FILE"); e && e[0]) return e;
    std::string dir = bbnet::settings().user_dir;
    if (dir.empty()) {
        if (const char* u = std::getenv("BB_GPU_USER_DIR")) dir = u;
        else if (const char* u2 = std::getenv("BB_USER_DIR")) dir = u2;
        else dir = std::string(std::getenv("BB_DATA_DIR") ? std::getenv("BB_DATA_DIR") : ".") + "/user";
    }
    return std::filesystem::path(dir) / "community" / "chalice_glyphs.json";
}

// Re-read when the cache's time stamp changes (the launcher may refresh it). Under g_mu, game thread.
void LoadGlyphs() {
    if (!g_family_goods[0]) {
        for (u32 goods = 6000; goods < 7000; ++goods) {
            const int f = ChaliceFamily(goods);
            if (f >= 0 && !g_family_goods[f]) g_family_goods[f] = goods;
        }
        std::printf("Glyph screen: a chalice per area: %u %u %u %u\n", g_family_goods[0], g_family_goods[1],
                    g_family_goods[2], g_family_goods[3]);
    }
    const std::filesystem::path p = CachePath();
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(p, ec);
    if (ec || (g_have_seen && t == g_seen)) return;
    g_seen = t;
    g_have_seen = true;
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::vector<coop::chalice::glyphdb::Entry> db;
    std::string err;
    int skipped = 0;
    if (!coop::chalice::glyphdb::ParseCache(ss.str(), &db, &err, &skipped)) {
        std::printf("Glyph screen: community glyph cache unusable (%s)\n", err.c_str());
        return;
    }
    auto gen = std::make_unique<Generation>();
    int not_chalice = 0, per[4] = {};
    for (const auto& e : db) {
        const u32 goods = coop::chalice::glyphdb::RecordGoods(e.rec);
        if (ChaliceFamily(goods) < 0) {
            ++not_chalice;
            continue;
        }
        if (e.glyph.size() != 8) continue;  // the game's glyph box takes 8 characters
        int cat = 0;
        while (cat < 4 && e.category != kCategories[cat]) ++cat;
        if (cat == 4) continue;  // ParseCache only lets the four through
        ++per[cat];
        const std::vector<std::uint8_t> fd = coop::chalice::glyphdb::RecordToFormData(e.rec);
        gen->push_back({goods, cat, Utf8To16(e.glyph, 16), Utf8To16(e.desc, 400), e.glyph,
                        bbnet::party::b64_encode(fd.data(), fd.size())});
    }
    std::printf("Glyph screen: %zu glyph(s) from the community cache (%d skipped, %d without a known chalice): "
                "farming %d, equipment %d, bloodgems %d, testing %d\n",
                gen->size(), skipped, not_chalice, per[0], per[1], per[2], per[3]);
    g_glyphs = gen.get();
    g_have_glyphs.store(!gen->empty(), std::memory_order_release);
    g_generations.push_back(std::move(gen));
}

// The altar's "Search by Chalice Glyph": with community glyphs, the chalice picker opens as the glyph screen.
BB_COOP_SYSV u64 GlyphItem(u64 out, u64 ctx) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        LoadGlyphs();
        if (!g_glyphs || g_glyphs->empty()) {
            std::printf("Glyph screen: no community glyphs; the glyph item opens the game's glyph box\n");
            std::fflush(stdout);
            return g_glyph_item_original(out, ctx);
        }
        g_opening = true;
        g_from_glyph = true;
        g_glyph_menu = 0;
        g_screen.store(true, std::memory_order_release);  // before the picker's setup asks for its title
    }
    std::printf("Glyph screen: the glyph item opens the glyph screen (%zu glyphs)\n", g_glyphs->size());
    std::fflush(stdout);
    const u64 r = reinterpret_cast<HandlerFn>(coop::Guest(kRitualItem))(out, ctx);
    std::lock_guard<std::mutex> lk(g_mu);
    g_from_glyph = false;
    return r;
}

// The altar menu greys "Search by Chalice Glyph" without the network (0x553b120: +8, +0x9f8, +0x9f6, +0xa50 -> a byte
// at the builder's rbp - 0x1458, stored on the row at 0x1b66823). The glyph screen needs no network: with community
// glyphs the row stays usable.
BB_COOP_SYSV u64 GlyphRowGrey(u64 frame, u64) {
    std::lock_guard<std::mutex> lk(g_mu);
    LoadGlyphs();
    if (g_glyphs && !g_glyphs->empty()) *reinterpret_cast<std::uint8_t*>(frame - 0x1458) = 0;
    return 0;
}

// The real "Chalice Ritual": its screen is the game's own.
BB_COOP_SYSV void RitualEntry(u64, u64, u64, u64, u64, u64) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_from_glyph) return;
    g_opening = false;
    g_glyph_menu = 0;
    g_screen.store(false, std::memory_order_release);
}

// After the screen filter: `tab` = the tab descriptor (the game's r12), `frame` = the builder's rbp (the fresh list
// at -0x2d8), `menu` = the menu object (r15).
BB_COOP_SYSV u64 AfterScreenFilter(u64 tab, u64 frame, u64 menu) {
    u32 tab_id = 0;
    if (!coop::SafeGet(tab, &tab_id) || tab_id < kFirstPickerTab || tab_id > kLastPickerTab) return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    g_pending = {};
    if (g_opening) {
        g_glyph_menu = menu;
        g_opening = false;
    }
    if (!menu || menu != g_glyph_menu) return 0;  // the real Chalice Ritual
    u64 list = 0, begin = 0, end = 0;
    if (!coop::SafeGet(frame - 0x2d8, &list) || !list || !coop::SafeGet(list + 0x18, &begin) ||
        !coop::SafeGet(list + 0x20, &end) || end < begin) {
        return 0;
    }
    // The tab's family: its filter functor's function.
    u64 functor = 0, fn = 0;
    int family = -1;
    if (coop::SafeGet(tab + 0x110, &functor) && functor && coop::SafeGet(functor + 8, &fn)) {
        for (int f = 0; f < 4; ++f) {
            if (fn == coop::Guest(kTabFilters[f])) family = f;
        }
    }
    const int category = static_cast<int>(tab_id - kFirstPickerTab);
    // The template: the player's first chalice row (a goods row, kind 0xc).
    u64 tmpl = 0;
    for (u64 row = begin; row + kRowSize <= end; row += kRowSize) {
        u32 iid = 0, kind = 0;
        if (coop::SafeGet(row + 0x4c, &iid) && coop::SafeGet(row + 0x50, &kind) && kind == 0xc &&
            (iid & 0xf0000000u) == 0x40000000u && ChaliceFamily(iid & 0x0fffffffu) >= 0) {
            tmpl = row;
            break;
        }
    }
    LoadGlyphs();
    if (!tmpl || !g_glyphs || family < 0 || !g_family_goods[family]) {
        std::printf("Glyph screen: tab 0x%x: %s; no rows\n", tab_id,
                    !tmpl ? "the player has no chalice to copy" : family < 0 ? "unknown tab filter" : "no glyphs");
        std::fflush(stdout);
        return 0;
    }
    const std::size_t original_rows = (end - begin) / kRowSize;
    const std::size_t tmpl_index = (tmpl - begin) / kRowSize;
    const auto push = reinterpret_cast<PushRowFn>(coop::Guest(kPushRow));
    g_pending.list = list;
    g_pending.first = original_rows;
    g_pending.family = family;
    for (std::size_t i = 0; i < g_glyphs->size(); ++i) {
        const Glyph& g = (*g_glyphs)[i];
        if (g.category != category) continue;
        u64 b = 0;
        coop::SafeGet(list + 0x18, &b);
        push(list + 0x10, b + tmpl_index * kRowSize);  // the push takes care of a source inside the vector
        u64 e = 0;
        coop::SafeGet(list + 0x20, &e);
        if (e < kRowSize) break;
        const u64 row = e - kRowSize;
        *reinterpret_cast<u32*>(row + 0x4c) = 0x40000000u | g.goods;
        *reinterpret_cast<u32*>(row + 0x70) = 0xb0000000u | g.goods;
        g_pending.glyph.push_back(static_cast<int>(i));
    }
    std::printf("Glyph screen: tab 0x%x (%s): %zu glyph row(s)\n", tab_id, kCategories[category],
                g_pending.glyph.size());
    std::fflush(stdout);
    return 0;
}

// The tab filter's collector on the glyph screen: the player's rows are turned away, ours pass and get their mark.
BB_COOP_SYSV u64 TabCollect(u64 out, u64 src_ref, u64 filter) {
    const auto collect = reinterpret_cast<CollectFn>(coop::Guest(kCollect));
    std::unique_lock<std::mutex> lk(g_mu);
    u64 src = 0;
    if (!g_pending.list || !coop::SafeGet(src_ref, &src) || src != g_pending.list) {
        lk.unlock();
        return collect(out, src_ref, filter);
    }
    Pending p = std::move(g_pending);
    g_pending = {};
    u64 begin = 0, end = 0;
    coop::SafeGet(src + 0x18, &begin);
    coop::SafeGet(src + 0x20, &end);
    const std::size_t rows = end > begin ? (end - begin) / kRowSize : 0;
    std::vector<u32> kinds(std::min(p.first, rows), 0);
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        u32* kind = reinterpret_cast<u32*>(begin + i * kRowSize + 0x50);
        kinds[i] = *kind;
        *kind = 0;
    }
    const u32 pass_as = 0x40000000u | g_family_goods[p.family];
    std::vector<u32> iids(p.glyph.size(), 0);
    for (std::size_t k = 0; k < p.glyph.size() && p.first + k < rows; ++k) {
        const u64 row = begin + (p.first + k) * kRowSize;
        *reinterpret_cast<u32*>(row + 0x58) = kMark | static_cast<u32>(p.glyph[k]);
        iids[k] = *reinterpret_cast<u32*>(row + 0x4c);
        *reinterpret_cast<u32*>(row + 0x4c) = pass_as;
    }
    lk.unlock();
    const u64 r = collect(out, src_ref, filter);
    lk.lock();
    for (std::size_t i = 0; i < kinds.size(); ++i) *reinterpret_cast<u32*>(begin + i * kRowSize + 0x50) = kinds[i];
    for (std::size_t k = 0; k < p.glyph.size() && p.first + k < rows; ++k) {
        *reinterpret_cast<u32*>(begin + (p.first + k) * kRowSize + 0x4c) = iids[k];
    }
    u64 list = 0, ob = 0, oe = 0;
    std::size_t kept = 0, others = 0;
    if (coop::SafeGet(out, &list) && list && coop::SafeGet(list + 0x18, &ob) && coop::SafeGet(list + 0x20, &oe)) {
        for (u64 row = ob; row + kRowSize <= oe; row += kRowSize) {
            u32 mark = 0;
            if (!coop::SafeGet(row + 0x58, &mark) || (mark & 0xff000000u) != kMark || !g_glyphs ||
                (mark & 0x00ffffffu) >= g_glyphs->size()) {
                ++others;
                continue;
            }
            *reinterpret_cast<u32*>(row + 0x4c) = 0x40000000u | (*g_glyphs)[mark & 0x00ffffffu].goods;
            ++kept;
        }
    }
    std::printf("Glyph screen: tab shows %zu glyph row(s), %zu other row(s)\n", kept, others);
    std::fflush(stdout);
    return r;
}

// Our glyph for a row (its mark, checked against the row's item id), or nullptr. Under g_mu.
const Glyph* GlyphOfRow(u64 row) {
    u32 iid = 0, mark = 0;
    if (!row || !g_glyphs || !coop::SafeGet(row + 0x4c, &iid) || !coop::SafeGet(row + 0x58, &mark) ||
        (mark & 0xff000000u) != kMark) {
        return nullptr;
    }
    const std::size_t k = mark & 0x00ffffffu;
    return k < g_glyphs->size() && (iid & 0x0fffffffu) == (*g_glyphs)[k].goods ? &(*g_glyphs)[k] : nullptr;
}

// A card's item name (and the description panel's title): our rows get the glyph code.
BB_COOP_SYSV u64 ItemName(u64 iid_at) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (const Glyph* g = GlyphOfRow(iid_at - 0x4c)) return reinterpret_cast<u64>(g->name.c_str());
    }
    return reinterpret_cast<NameFn>(coop::Guest(kItemName))(iid_at);
}

// The long description: 0x1c5dd70(out, row) asks 0x1f01180(msg, item id) for a goods row's text (call 0x1c5df17);
// the row is not kept for that call, so the entry hook notes it for this thread.
thread_local u64 t_desc_row = 0;
BB_COOP_SYSV void DescEntry(u64, u64 row, u64, u64, u64, u64) { t_desc_row = row; }
BB_COOP_SYSV u64 DescText(u64 msg, u64 iid) {
    const u64 row = t_desc_row;
    t_desc_row = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (const Glyph* g = GlyphOfRow(row)) return reinterpret_cast<u64>(g->desc.c_str());
    }
    return reinterpret_cast<DescFn>(coop::Guest(kGoodsDesc))(msg, iid);
}

// A glyph card picked: in place of the Chalice Details step (ChannelGenerate) the picker gets the Search by Chalice
// Glyph step, whose glyph box our preset fills (PickedGlyph). The glyph's dungeon is put on the search board first
// (as the glyph box's close handler does for a typed community glyph), so the game's own search finds it.
BB_COOP_SYSV u64 Picked(u64 out, u64 functor, u64 a2, u64 row, u64 ctx) {
    std::string code, form;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (const Glyph* g = GlyphOfRow(row)) {
            code = g->code;
            form = g->form;
        }
    }
    if (code.empty()) return g_picked_original(out, functor, a2, row, ctx);
    bbnet::party::ChannelBoard& board = bbnet::party::FromApi::instance().channels();
    bbnet::party::ChannelBoard::Channel c;
    const char* how = "already on the board";
    if (!(board.find_glyph(code, &c) && !c.form_data.empty())) {
        const std::uint64_t id = bbnet::party::ChannelBoard::IdOf(form);
        const bool ok = board.known(id) ? board.rename_glyph(id, code) : board.adopt(id, form, "curated", code);
        how = ok ? "put on the board" : "REFUSED by the board";
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pick = code;
        g_pick_at = Seconds();
        g_skip_results_until = g_pick_at + kSkipSeconds;
    }
    std::printf("Glyph screen: glyph %s picked (%s); the glyph box opens with it\n", code.c_str(), how);
    std::fflush(stdout);
    t_picking = true;
    const u64 r = g_glyph_item_original(out, ctx);
    t_picking = false;
    return r;
}

// After a picked glyph's search, the game opens its Search Results screen (0x1b6a360(out, ctx, ?, results): movie
// ChannelSearchResult; `results` a std::function (+0x20) that fills {?, begin, end, ?, allocator} with results of
// 0x398 bytes). The glyph screen already was the choice: the first result goes straight to the game's
// "Join the selected Chalice Dungeon?" step (0x1b6bcf0(out, -, ctx, result), what picking it on that screen does).
// Once, within kSkipSeconds of the pick; any other search shows the screen as the game has it.
BB_COOP_SYSV u64 SearchResults(u64 out, u64 ctx, u64 a2, u64 results) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const bool ours = Seconds() < g_skip_results_until;
        g_skip_results_until = 0;
        if (!ours) return g_results_original(out, ctx, a2, results);
    }
    u64 functor = 0, vtable = 0, fill = 0;
    if (!results || !coop::SafeGet(results + 0x20, &functor) || !functor || !coop::SafeGet(functor, &vtable) ||
        !coop::SafeGet(vtable + 0x10, &fill) || !fill) {
        return g_results_original(out, ctx, a2, results);
    }
    alignas(16) u64 buf[12] = {};
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64)>(fill)(reinterpret_cast<u64>(buf), functor);
    const u64 begin = buf[1], end = buf[2], alloc = buf[4];
    const std::size_t count = end > begin ? (end - begin) / kResultSize : 0;
    u64 r = 0;
    if (count) {
        std::printf("Glyph screen: %zu search result(s); the first goes straight to the join question\n", count);
        r = reinterpret_cast<ResultsFn>(coop::Guest(kJoinQuestion))(out, 0, ctx, begin);
    }
    for (u64 item = begin; item && item < end; item += kResultSize) {
        reinterpret_cast<void(BB_COOP_SYSV*)(u64)>(coop::Guest(kResultDtor))(item);
    }
    u64 alloc_vt = 0, release = 0;
    if (begin && alloc && coop::SafeGet(alloc, &alloc_vt) && coop::SafeGet(alloc_vt + 0x70, &release) && release) {
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64)>(release)(alloc, begin);
    }
    std::fflush(stdout);
    return count ? r : g_results_original(out, ctx, a2, results);
}

// The glyph step's "Search using Chalice Glyph: <glyph>" notice (0x1b710e0(out, functor): an OK box, 267001): for our
// pick the glyph screen already said which, so no step (an empty one) and the search runs on.
ConfirmFn g_confirm_original = nullptr;
BB_COOP_SYSV u64 SearchNotice(u64 out, u64 functor) {
    bool ours = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ours = Seconds() < g_skip_results_until;
    }
    if (!ours) return g_confirm_original(out, functor);
    *reinterpret_cast<u64*>(out) = 0;
    std::printf("Glyph screen: the search notice is skipped for the picked glyph\n");
    std::fflush(stdout);
    return out;
}

// The Search by Chalice Glyph step's network check (0x553b120 +8 set, +0x9f8 / +0x9f6 / +0xa50 clear): for our pick
// it reads a stand-in that passes (the glyph screen and the board need no network); any other caller sees the game's.
alignas(16) std::uint8_t g_net_ok[0xb00] = {};
BB_COOP_SYSV u64 GlyphNetCheck(u64 net, u64) {
    if (!t_picking && net) return net;
    g_net_ok[8] = 1;
    return reinterpret_cast<u64>(g_net_ok);
}

} // namespace

const char16_t* Text(std::uint32_t category, std::uint32_t id) {
    if (!g_installed.load(std::memory_order_acquire) || !g_screen.load(std::memory_order_acquire)) return nullptr;
    // The chalice picker's own texts (the title and help at 0x1c74d2a / 0x1c74db8, the tab names at 0x1c69a05..):
    // names 0xc8, help lines 0xc9.
    if (category == 0xc8) {
        switch (id) {
        case 263100: return u"Search by Chalice Glyph";
        case 40435: return u"Farming";
        case 40436: return u"Equipment";
        case 40437: return u"Blood Gems";
        case 40438: return u"Testing";
        default: return nullptr;
        }
    }
    if (category == 0xc9 && id == 263000) return u"Select a Chalice Glyph to join its dungeon.";
    return nullptr;
}

bool KeepsGameText(std::uint32_t id) {
    // The altar names its items before it builds them (before the glyph list is first read), so: the screen's hooks
    // are in and the community list's file is there (the glyph item then opens the glyph screen).
    if (id != kGlyphSearchText || !g_installed.load(std::memory_order_acquire)) return false;
    if (g_have_glyphs.load(std::memory_order_acquire)) return true;
    std::error_code ec;
    return std::filesystem::is_regular_file(CachePath(), ec);
}

const char* PickedGlyph(std::uint32_t max_length) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_pick.empty() || max_length < g_pick.size() || Seconds() - g_pick_at > kPickSeconds) return nullptr;
    static std::string keep;  // the caller copies it at once (runtime_services.c ime_init)
    keep = g_pick;
    g_pick.clear();
    std::printf("Glyph screen: the glyph box gets the picked glyph %s\n", keep.c_str());
    std::fflush(stdout);
    return keep.c_str();
}

void Install() {
    if (const char* e = std::getenv("BB_GLYPH_SCREEN_TEST"); !e || e[0] != '1') return;
    bool ok = coop::ReplacePrologue(kGlyphItem, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57},
                                    reinterpret_cast<const void*>(&GlyphItem),
                                    reinterpret_cast<void**>(&g_glyph_item_original), "glyph screen: the glyph item");
    ok = ok && coop::HookValueFilter(kGlyphRowGrey, {0x88, 0x8d, 0xa8, 0xeb, 0xff, 0xff},  // mov [rbp-0x1458], cl
                                     {0x48, 0x89, 0xef},                                    // mov rdi, rbp
                                     &GlyphRowGrey, kGlyphRowGrey + 6, "glyph screen: the glyph row usable");
    ok = ok && coop::HookValueFilter(kGlyphNetCheck, {0x48, 0x8b, 0x03, 0x48, 0x85, 0xc0},  // mov rax,[rbx]; test
                                     {0x48, 0x89, 0xc7},                                    // mov rdi, rax
                                     &GlyphNetCheck, kGlyphNetChecks, "glyph screen: the glyph box's network check");
    ok = ok && coop::ReplacePrologue(kPicked, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, reinterpret_cast<const void*>(&Picked),
                                     reinterpret_cast<void**>(&g_picked_original), "glyph screen: a picked glyph");
    ok = ok && coop::ReplacePrologue(kSearchResults, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57},
                                     reinterpret_cast<const void*>(&SearchResults),
                                     reinterpret_cast<void**>(&g_results_original),
                                     "glyph screen: a picked glyph's search results");
    ok = ok && coop::ReplacePrologue(kSearchNotice, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57},
                                     reinterpret_cast<const void*>(&SearchNotice),
                                     reinterpret_cast<void**>(&g_confirm_original),
                                     "glyph screen: a picked glyph's search notice");
    ok = ok && coop::HookPrologue(kRitualItem, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &RitualEntry,
                                  "glyph screen: the real Chalice Ritual");
    ok = ok && coop::HookValueFilter(kAfterScreenFilter, {0x49, 0x8b, 0xbf, 0xd0, 0x1e, 0x00, 0x00},
                                     // mov rdi, [r12] (the stub keeps its stack pointer in r12; the game's r12 is the
                                     // push it points at); mov rsi, rbp; mov rdx, r15
                                     {0x49, 0x8b, 0x3c, 0x24, 0x48, 0x89, 0xee, 0x4c, 0x89, 0xfa},
                                     reinterpret_cast<coop::ValueFilter>(&AfterScreenFilter), kResume,
                                     "glyph screen: rows after the screen filter");
    ok = ok && coop::HookCallSite(kTabCollectCall, kCollect, reinterpret_cast<const void*>(&TabCollect),
                                  "glyph screen: the tab filter's collector");
    ok = ok && coop::HookCallSite(kNameCall, kItemName, reinterpret_cast<const void*>(&ItemName),
                                  "glyph screen: card names");
    ok = ok && coop::HookPrologue(kDescBinder, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &DescEntry,
                                  "glyph screen: description rows");
    ok = ok && coop::HookCallSite(kDescCall, kGoodsDesc, reinterpret_cast<const void*>(&DescText),
                                  "glyph screen: descriptions");
    g_installed.store(ok, std::memory_order_release);
    std::printf("Glyph screen: %s\n", ok ? "on (BB_GLYPH_SCREEN_TEST)" : "FAILED to install");
    std::fflush(stdout);
}

} // namespace BbGlyphScreen
