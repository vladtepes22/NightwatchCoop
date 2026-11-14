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
#include "bbport_game_menu.h"

#include "party/coop_hooks.h"
#include "party/game_state.h"
#include "party/party_chalice_glyphdb.h"
#include "party/party_chalice_glyphs.h"
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

extern "C" int runtime_file_mount(const char*, const char*);
extern "C" void runtime_file_unmount(const char*);

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
constexpr u32 kFirstPickerTab = 0x24, kLastPickerTab = 0x28;  // 0x28: the Custom tab (AddCustomTab)
constexpr u32 kMark = 0x47000000u;  // row +0x58 of our rows: kMark | glyph index (into *g_glyphs)
// tab order; "custom": the party board's dungeons that are not community glyphs (the player's own, the party's)
constexpr const char* kCategories[5] = {"farming", "equipment", "bloodgems", "testing", "custom"};
constexpr int kCustom = 4;

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

// The glyph screen's own description panel (GlyphDetailText, a second panel in the generated itemdetailtext.gfx:
// tools/engine/menu_assets.cpp GlyphPanelMovie). An item screen finds its panel by name with 0x1adb220(out, parent,
// printf-style path) twice: once in its setup 0x1b98fb0 (rdi = the screen; call 0x1b9914f) and again whenever its
// text is filled in, through the getter 0x1b98d80(out, screen + 0xda0) (call 0x1b98e93). The first setup after the
// glyph item is the glyph screen's; both lookups of that screen ask for GlyphDetailText.
constexpr u64 kScreenSetup = 0x1b98fb0;
constexpr u64 kSetupFetchCall = 0x1b9914f;
constexpr u64 kPanelGetter = 0x1b98d80;
constexpr u64 kPanelGetterOffset = 0xda0;
constexpr u64 kGetterFetchCall = 0x1b98e93;
constexpr u64 kChildFetch = 0x1adb220;
constexpr char kGuestPanel[] = "/app0/dvdroot_ps4/menu/itemdetailtext.gfx";
using FetchFn = u64(BB_COOP_SYSV*)(u64 out, u64 parent, const char* path);
bool g_panel = false;                // the generated panel movie is mounted and its hooks in
bool g_panel_pending = false;        // the next item screen's setup is the glyph screen's
std::atomic<u64> g_panel_screen{0};  // the glyph screen's item screen object (0: none)
thread_local bool t_panel_ours = false;  // the lookup now running is the glyph screen's
thread_local bool t_in_getter = false;  // the getter (a text fill) is running
thread_local u64 t_getter_screen = 0;    // the item screen whose text is being filled
constexpr u64 kScreenMovie = 0xa70;     // an item screen's wrapper of its own movie's root (set in its setup)

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
// The Custom category: the party board's dungeons (the host's own, which the party's glyph list puts there, and the
// guests') that can be entered and are not community glyphs. A new generation only when they change (the cards of
// an open screen keep pointing at the one they were made from).
Generation g_community;
std::string g_custom_key;
void AddBoardGlyphs() {
    if (g_community.empty()) return;
    Generation custom;
    std::string key;
    for (const bbnet::party::ChannelBoard::Channel& c : bbnet::party::FromApi::instance().channels().channels()) {
        if (c.form_data.empty() || c.glyph.size() != 8) continue;
        std::string code = c.glyph;
        for (char& ch : code) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const bool community = std::any_of(g_community.begin(), g_community.end(), [&](const Glyph& g) { return g.code == code; });
        const bool twice = std::any_of(custom.begin(), custom.end(), [&](const Glyph& g) { return g.code == code; });
        const u32 goods = coop::chalice::glyphs::GoodsOfForm(c.form_data);
        if (community || twice || ChaliceFamily(goods) < 0) continue;
        const std::string desc = c.owner.empty() ? std::string("A Chalice Dungeon on the party's board.")
                                                 : "A Chalice Dungeon on the party's board, from " + c.owner + ".";
        custom.push_back({goods, kCustom, Utf8To16(code, 16), Utf8To16(desc, 400), code, c.form_data});
        key += code + ";";
    }
    if (key == g_custom_key) return;
    g_custom_key = key;
    auto gen = std::make_unique<Generation>(g_community);
    gen->insert(gen->end(), custom.begin(), custom.end());
    std::printf("Glyph screen: %zu Custom glyph(s) from the party board\n", custom.size());
    std::fflush(stdout);
    g_glyphs = gen.get();
    g_generations.push_back(std::move(gen));
}

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
    if (ec || (g_have_seen && t == g_seen)) {
        AddBoardGlyphs();
        return;
    }
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
    g_community = *gen;
    g_custom_key.clear();
    g_glyphs = gen.get();
    g_have_glyphs.store(!gen->empty(), std::memory_order_release);
    g_generations.push_back(std::move(gen));
    AddBoardGlyphs();
}

// The Custom tab. The picker's tabs are made by 0x1c77a50(?, menu): each by its own maker (0x1c84ad0 the first, a
// 0x150-byte descriptor: +0 the tab id, +0xf0 the filter, std::function slots at +0x60 / +0x90 / +0xc0 / +0xf0 / +0x120
// whose pointers are at +0x80 / +0xb0 / +0xe0 / +0x110 / +0x140), then added with 0x1baadf0(slot, descriptor) to the
// menu's tab array (16-byte aligned from the menu, 0x150 apart, up to 24; the count at menu +0x1e40). On the glyph
// screen a fifth tab follows: the first tab's descriptor with the id 0x28 (its rows: the Custom category; its filter
// the first tab's, which our rows pass as one of its chalices). A tab's name is made by 0x1c69700(out, descriptor):
// for 0x28 it is our text (kCustomTabText), the other tabs keep theirs.
constexpr u64 kMakeTabs = 0x1c77a50, kFirstTabMaker = 0x1c84ad0, kAddTab = 0x1baadf0, kTabName = 0x1c69700;
constexpr u64 kTabCount = 0x1e40, kTabSize = 0x150, kMostTabs = 0x18;
constexpr u32 kCustomTab = 0x28;
constexpr u32 kCustomTabText = 263190;  // a menu text id of ours (0xc8), not one the game has
using MakeTabsFn = u64(BB_COOP_SYSV*)(u64, u64);
using TabNameFn = u64(BB_COOP_SYSV*)(u64 out, u64 tab);
MakeTabsFn g_make_tabs_original = nullptr;
TabNameFn g_tab_name_original = nullptr;
BB_COOP_SYSV u64 MakeTabs(u64 a0, u64 menu) {
    const u64 r = g_make_tabs_original(a0, menu);
    bool opening = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        opening = g_opening;
    }
    const u64 count = *reinterpret_cast<const u64*>(menu + kTabCount);
    if (!opening || count + 1 >= kMostTabs) return r;
    alignas(16) std::uint8_t tab[kTabSize]{};
    const u64 t = reinterpret_cast<u64>(tab);
    reinterpret_cast<u64(BB_COOP_SYSV*)(u64)>(coop::Guest(kFirstTabMaker))(t);
    *reinterpret_cast<u32*>(t) = kCustomTab;
    const u64 tabs = menu + ((0 - menu) & 0xf);
    reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64)>(coop::Guest(kAddTab))(tabs + kTabSize * count, t);
    *reinterpret_cast<u64*>(menu + kTabCount) = count + 1;
    for (const u64 at : {0x80, 0xb0, 0xe0, 0x110, 0x140}) {  // the maker's std::functions, as 0x1c77a50 ends them
        const u64 f = *reinterpret_cast<const u64*>(t + at);
        if (!f) continue;
        const u64 destroy = *reinterpret_cast<const u64*>(*reinterpret_cast<const u64*>(f) + 0x20);
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64)>(destroy)(f, f != t + at - 0x20 ? 1 : 0);
    }
    std::printf("Glyph screen: the Custom tab added (tab %llu)\n", static_cast<unsigned long long>(count + 1));
    std::fflush(stdout);
    return r;
}
BB_COOP_SYSV u64 TabName(u64 out, u64 tab) {
    if (*reinterpret_cast<const u32*>(tab) == kCustomTab && g_screen.load(std::memory_order_acquire))
        return reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64, u64)>(coop::Guest(0x1ae8cc0))(out, 0xc8, kCustomTabText);
    return g_tab_name_original(out, tab);
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
        g_panel_pending = g_panel;
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

// An item screen's setup: the first after the glyph item is the glyph screen's; a later setup of the same object is
// another screen's.
BB_COOP_SYSV void ScreenSetup(u64 screen, u64, u64, u64, u64, u64) {
    bool ours = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ours = g_panel_pending;
        g_panel_pending = false;
    }
    if (ours) {
        g_panel_screen.store(screen, std::memory_order_release);
        std::printf("Glyph screen: item screen 0x%llx uses GlyphDetailText\n", static_cast<unsigned long long>(screen));
        std::fflush(stdout);
    } else if (g_panel_screen.load(std::memory_order_acquire) == screen) {
        g_panel_screen.store(0, std::memory_order_release);
    }
    t_panel_ours = ours;
    t_in_getter = false;
}
BB_COOP_SYSV void PanelGetter(u64, u64 src, u64, u64, u64, u64) {
    const u64 screen = g_panel_screen.load(std::memory_order_acquire);
    t_panel_ours = screen && src == screen + kPanelGetterOffset;
    t_in_getter = true;
    t_getter_screen = src - kPanelGetterOffset;
}

// Both panels live in the one panel movie that every item screen shares, and each keeps the text last written to it:
// the glyph screen shows only GlyphDetailText's parts (hidden in the movie), every other screen only ItemDetailText's.
// Only the parts inside the panels are shown or hidden, never the panels: a display object changed by code no longer
// follows the timeline, and the panels fade in and out by the timeline. The unnamed parts are named GlyphPart_<depth>
// in the generated movie (tools/engine/menu_assets.cpp GlyphPanelMovie).
//
// A display object is a wrapper from 0x1adb220(out, parent wrapper, name): its vtable's +8 sets its visibility; its
// value at +0x30 (interface +0x40, type +0x48, data +0x50; type & 0x8f == 10 a display object, & 0x40 counted,
// released by the interface's +0x18).
constexpr const char* kGlyphParts[] = {"GlyphPart_1", "GlyphPart_15", "GlyphPart_19", "GlyphPart_23", "GlyphPart_34",
                                       "DetailDesc",  "ItemName",     "GlyphDepth",   "GlyphArea"};
constexpr const char* kGameParts[] = {"GlyphPart_1",  "GlyphPart_11",   "ItemKnowledgeIcon", "GlyphPart_15",
                                      "GlyphPart_19", "GlyphPart_23",   "GlyphPart_26",      "GlyphPart_30",
                                      "GlyphPart_34", "Item_1_TP_Add",  "Item_1_TP_Base",    "Item_0_TP_Add",
                                      "Item_0_TP_Base", "DetailDesc",   "ItemName"};
struct Wrapper {
    alignas(16) std::uint8_t bytes[0x100]{};
    u64 at() { return reinterpret_cast<u64>(bytes); }
    bool Fetch(u64 parent, const char* name) {
        reinterpret_cast<FetchFn>(coop::Guest(kChildFetch))(at(), parent, name);
        return (Type() & 0x8f) == 10;
    }
    u32 Type() { return *reinterpret_cast<const u32*>(at() + 0x48); }
    void SetVisible(bool visible) {
        const u64 vtable = *reinterpret_cast<const u64*>(at());
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u32)>(*reinterpret_cast<const u64*>(vtable + 8))(at(), visible ? 1 : 0);
    }
    ~Wrapper() {
        if (!(Type() & 0x40)) return;
        const u64 iface = *reinterpret_cast<const u64*>(at() + 0x40);
        const u64 release = *reinterpret_cast<const u64*>(*reinterpret_cast<const u64*>(iface) + 0x18);
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64, u64)>(release)(iface, at() + 0x30,
                                                                       *reinterpret_cast<const u64*>(at() + 0x50));
    }
};
// The parts of one panel (`panel`, a display object wrapper) shown or hidden; how many were found.
template <std::size_t N>
int ShowParts(u64 panel, const char* const (&parts)[N], bool visible) {
    int found = 0;
    for (const char* name : parts) {
        Wrapper part;
        if (!part.Fetch(panel, name)) continue;
        part.SetVisible(visible);
        ++found;
    }
    return found;
}
bool g_glyph_panel_shown = false;  // the movie now shows GlyphDetailText's parts (menu thread)

// The stats panel (PlayerStatus, root depth 10 of the item screen's own movie, found from screen + 0xa70; faded by
// its timeline) has two parts,
// Normal (the stats) and BodyGemList. The glyph screen hides both; the visibility each had is put back on the next
// other screen. A display object's visibility: the wrapper's vtable +0 copies its value out (value +8, interface
// +0x18, type +0x20, data +0x28); the interface's +0xe0 (interface, data, info) fills a display info whose +0xd6 is
// the visible byte.
constexpr const char* kStatsParts[] = {"Normal", "BodyGemList"};
bool g_stats_hidden = false;
bool g_stats_were_visible[std::size(kStatsParts)] = {};
bool IsVisible(Wrapper& w) {
    alignas(16) std::uint8_t value[0x40]{};
    const u64 v = reinterpret_cast<u64>(value);
    const u64 vtable = *reinterpret_cast<const u64*>(w.at());
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64)>(*reinterpret_cast<const u64*>(vtable))(v, w.at());
    const u64 iface = *reinterpret_cast<const u64*>(v + 0x18);
    const u32 type = *reinterpret_cast<const u32*>(v + 0x20);
    const u64 data = *reinterpret_cast<const u64*>(v + 0x28);
    bool visible = true;
    if ((type & 0x8f) == 10 && iface) {
        alignas(16) std::uint8_t info[0x100]{};
        const u64 get = *reinterpret_cast<const u64*>(*reinterpret_cast<const u64*>(iface) + 0xe0);
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64, u64)>(get)(iface, data, reinterpret_cast<u64>(info));
        visible = info[0xd6] != 0;
    }
    if ((type & 0x40) && iface) {
        const u64 release = *reinterpret_cast<const u64*>(*reinterpret_cast<const u64*>(iface) + 0x18);
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64, u64)>(release)(iface, v + 8, data);
    }
    return visible;
}
void ShowStats(u64 parent, bool ours) {
    if (!ours && !g_stats_hidden) return;
    Wrapper stats;
    if (!stats.Fetch(parent, "PlayerStatus")) {
        if (ours && !g_stats_hidden) std::printf("Glyph screen: no stats panel (PlayerStatus) to hide\n");
        return;
    }
    int found = 0;
    for (std::size_t i = 0; i < std::size(kStatsParts); ++i) {
        Wrapper part;
        if (!part.Fetch(stats.at(), kStatsParts[i])) continue;
        ++found;
        if (ours) {
            if (!g_stats_hidden) g_stats_were_visible[i] = IsVisible(part);
            part.SetVisible(false);
        } else {
            part.SetVisible(g_stats_were_visible[i]);
        }
    }
    if (ours != g_stats_hidden) {
        std::printf("Glyph screen: the stats panel %s (%d / %zu parts; before: Normal %d, BodyGemList %d)\n",
                    ours ? "hidden" : "back", found, std::size(kStatsParts), int(g_stats_were_visible[0]),
                    int(g_stats_were_visible[1]));
        std::fflush(stdout);
    }
    g_stats_hidden = ours;
}

// The item screen's stats refresh 0x1b9c010 (rdi = the screen; at its start and on Display) fills the rune list with
// 0x1c88290(list, BodyGemList wrapper) (call 0x1b9c799), which shows BodyGemList again. After it, the stats panel is
// hidden again on the glyph screen, or put back on another screen.
constexpr u64 kStatsRefresh = 0x1b9c010;
constexpr u64 kRuneFillCall = 0x1b9c799;
constexpr u64 kRuneFill = 0x1c88290;
thread_local u64 t_stats_screen = 0;
BB_COOP_SYSV void StatsRefresh(u64 screen, u64, u64, u64, u64, u64) { t_stats_screen = screen; }
BB_COOP_SYSV u64 RuneFill(u64 list, u64 wrapper) {
    const u64 r = reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64)>(coop::Guest(kRuneFill))(list, wrapper);
    const u64 screen = t_stats_screen;
    const bool ours = screen && screen == g_panel_screen.load(std::memory_order_acquire);
    if (screen && (ours || g_stats_hidden)) ShowStats(screen + kScreenMovie, ours);
    return r;
}

BB_COOP_SYSV u64 PanelFetch(u64 out, u64 parent, u64 path) {
    const bool ours = t_panel_ours;
    const bool getter = t_in_getter;
    t_in_getter = false;
    const u64 r = reinterpret_cast<FetchFn>(coop::Guest(kChildFetch))(
        out, parent, ours ? "GlyphDetailText" : reinterpret_cast<const char*>(path));
    // a text fill (the getter, the movie ready): the screen's own panel's parts shown, the other's hidden
    if (getter && (ours || g_glyph_panel_shown) && (*reinterpret_cast<const u32*>(out + 0x48) & 0x8f) == 10) {
        Wrapper glyph, game;
        const int glyph_parts = glyph.Fetch(parent, "GlyphDetailText") ? ShowParts(glyph.at(), kGlyphParts, ours) : 0;
        const int game_parts = game.Fetch(parent, "ItemDetailText") ? ShowParts(game.at(), kGameParts, !ours) : 0;
        if (g_glyph_panel_shown != ours) {
            std::printf("Glyph screen: the panel movie shows %s (parts %d / %zu shown, %d / %zu hidden)\n",
                        ours ? "GlyphDetailText" : "ItemDetailText", ours ? glyph_parts : game_parts,
                        ours ? std::size(kGlyphParts) : std::size(kGameParts), ours ? game_parts : glyph_parts,
                        ours ? std::size(kGameParts) : std::size(kGlyphParts));
            std::fflush(stdout);
        }
        g_glyph_panel_shown = ours;
    }
    if (getter && (ours || g_stats_hidden) && (*reinterpret_cast<const u32*>(out + 0x48) & 0x8f) == 10)
        ShowStats(t_getter_screen + kScreenMovie, ours);
    return r;
}

// The glyph screen's controls: Display is always on and neither Display nor Toggle Stats is offered.
//
// The item screen's controls setup 0x1b999c0 (rdi = the screen) adds each control with 0x1ad5730(screen, control,
// action, ...): Display at call 0x1b9a660, Toggle Stats at call 0x1b9a7f0; the glyph screen skips both (their keys and
// their hints). It starts the screen in display mode 0 (+0x1168; +0x1300 the number of modes, 3) and applies it with
// 0x1b9ac30(screen, animate) (call 0x1b99e04). The mode's listeners (std::function-like objects from +0x1190, 16-byte
// aligned, 0x30 apart; call: vtable +0x10 (listener, &selected)) set the screen up for each mode: 0 and 1 the card
// list's parts (0x1baa1f0), 2 the description panel (0x1ba9f30, its show / hide). Mode 2 is Display's expanded view.
//
// The glyph screen goes to mode 2 itself, without Display's action (0x1ba6760, which also plays the Display sound) and
// without the apply (which puts the mode back to 0 while no card is selected): right before the screen opens (its open
// 0x1b9b2c0, rdi = the screen, fades every shown part in together), so the panel opens with the rest of the screen. The
// per-frame update 0x1b9bfa0 (vtable slot 3; rdi = the screen) does the same if the open came first. The screen's setup
// puts the mode back to 0 on every open (also for Chalice Ritual).
constexpr u64 kAddControl = 0x1ad5730;
constexpr u64 kAddDisplayCall = 0x1b9a660;
constexpr u64 kAddToggleStatsCall = 0x1b9a7f0;
constexpr u64 kApplyMode = 0x1b9ac30;
constexpr u64 kStartModeCall = 0x1b99e04;
constexpr u64 kScreenOpen = 0x1b9b2c0;
constexpr u64 kScreenUpdate = 0x1b9bfa0;
constexpr u64 kDisplayMode = 0x1168, kDisplayModes = 0x1300, kSavedMode = 0x1380, kModeListeners = 0x1190;
constexpr u32 kExpanded = 2;
std::atomic<u64> g_display_pending{0};  // the glyph screen, until its Display mode is on
bool IsGlyphScreen(u64 screen) { return screen && screen == g_panel_screen.load(std::memory_order_acquire); }
BB_COOP_SYSV u64 AddControl(u64 screen, u64 control, u64 action, u64 extra) {
    if (IsGlyphScreen(screen)) return 0;
    return reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64, u64, u64)>(coop::Guest(kAddControl))(screen, control, action, extra);
}
BB_COOP_SYSV u64 StartMode(u64 screen, u64 animate) {
    const u64 r = reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64)>(coop::Guest(kApplyMode))(screen, animate);
    g_display_pending.store(IsGlyphScreen(screen) ? screen : 0, std::memory_order_release);
    return r;
}
// The panel movie's FadeIn, as the screen's open plays it on a panel that is shown when it opens (0x1b9b3dc: the panel
// handle at +0xda8 resolved by 0x1adb030, its value copied by the wrapper's vtable +0 (value +8, interface +0x18, type
// +0x20, data +0x28), then the interface's +0x150 (interface, data, label, 0): gotoAndPlay).
constexpr u64 kPanelHandle = 0xda8;
constexpr u64 kResolveHandle = 0x1adb030;
constexpr u64 kFadeInLabel = 0x4934476;  // "FadeIn"
void PanelFadeIn(u64 screen) {
    Wrapper panel;
    reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64)>(coop::Guest(kResolveHandle))(panel.at(), screen + kPanelHandle);
    const u64 vtable = *reinterpret_cast<const u64*>(panel.at());
    if (!vtable) return;
    alignas(16) std::uint8_t value[0x40]{};
    const u64 v = reinterpret_cast<u64>(value);
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64)>(*reinterpret_cast<const u64*>(vtable))(v, panel.at());
    const u64 iface = *reinterpret_cast<const u64*>(v + 0x18);
    const u32 type = *reinterpret_cast<const u32*>(v + 0x20);
    const u64 data = *reinterpret_cast<const u64*>(v + 0x28);
    if (iface && (type & 0x8f)) {
        const u64 play = *reinterpret_cast<const u64*>(*reinterpret_cast<const u64*>(iface) + 0x150);
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64, u64, u64)>(play)(iface, data, coop::Guest(kFadeInLabel), 0);
    }
    if ((type & 0x40) && iface) {
        const u64 release = *reinterpret_cast<const u64*>(*reinterpret_cast<const u64*>(iface) + 0x18);
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64, u64)>(release)(iface, v + 8, data);
    }
}
// The glyph screen into Display's expanded view: the mode set and its listeners told, as Display's action does; after
// the screen's open, the panel is faded in as the open would have.
void ExpandDisplay(u64 screen, const char* when, bool fade_in) {
    if (!screen || g_display_pending.load(std::memory_order_acquire) != screen) return;
    g_display_pending.store(0, std::memory_order_release);
    const u64 modes = *reinterpret_cast<const u64*>(screen + kDisplayModes);
    if (!IsGlyphScreen(screen) || modes <= kExpanded || modes > 8) return;
    const u64 listeners = screen + kModeListeners + ((0 - (screen + kModeListeners - 0x20)) & 0xf);
    for (u64 i = 0; i < modes; ++i)
        if (!*reinterpret_cast<const u64*>(listeners + 0x30 * i)) return;  // the game's own call would fail here too
    *reinterpret_cast<u32*>(screen + kDisplayMode) = kExpanded;
    *reinterpret_cast<u32*>(screen + kSavedMode) = kExpanded;
    for (u64 i = 0; i < modes; ++i) {
        const u64 listener = *reinterpret_cast<const u64*>(listeners + 0x30 * i);
        bool selected = i == kExpanded;
        const u64 call = *reinterpret_cast<const u64*>(*reinterpret_cast<const u64*>(listener) + 0x10);
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64)>(call)(listener, reinterpret_cast<u64>(&selected));
    }
    if (fade_in) PanelFadeIn(screen);
    std::printf("Glyph screen: Display on (%s)\n", when);
    std::fflush(stdout);
}
BB_COOP_SYSV void ScreenOpen(u64 screen, u64, u64, u64, u64, u64) { ExpandDisplay(screen, "as the screen opens", false); }
// The panel's Depth and Area rows (GlyphDepth / GlyphArea: Text_0 the label, Value_0 the value), written on the frame
// after the selected glyph changes: labels "Depth" / "Area" and the area's name are the game's own menu texts (category
// 0xc8: 17001, 17002, 17100 + the chalice's HolygrailParam family: Pthumeru, Hintertomb, Loran, Isz). A text field's
// text: its wrapper's value (vtable +0), then the interface's +0x138 (interface, data, UTF-16 text, 1).
constexpr u32 kMenuText = 0xc8, kDepthLabel = 17001, kAreaLabel = 17002, kAreaNames = 17100;
std::atomic<u32> g_shown_goods{0};  // the chalice of the glyph whose description was filled last (0: none)
u32 g_rows_goods = 0;               // the chalice the rows were written for (menu thread)
u64 g_rows_screen = 0;
void SetFieldText(u64 root, const char* path, const char16_t* text) {
    Wrapper field;
    if (!text || !field.Fetch(root, path)) return;
    alignas(16) std::uint8_t value[0x40]{};
    const u64 v = reinterpret_cast<u64>(value);
    const u64 vtable = *reinterpret_cast<const u64*>(field.at());
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64)>(*reinterpret_cast<const u64*>(vtable))(v, field.at());
    const u64 iface = *reinterpret_cast<const u64*>(v + 0x18);
    const u32 type = *reinterpret_cast<const u32*>(v + 0x20);
    const u64 data = *reinterpret_cast<const u64*>(v + 0x28);
    if (iface && (type & 0x8f)) {
        const u64 set = *reinterpret_cast<const u64*>(*reinterpret_cast<const u64*>(iface) + 0x138);
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64, u64, u32)>(set)(iface, data, reinterpret_cast<u64>(text), 1);
    }
    if ((type & 0x40) && iface) {
        const u64 release = *reinterpret_cast<const u64*>(*reinterpret_cast<const u64*>(iface) + 0x18);
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64, u64)>(release)(iface, v + 8, data);
    }
}
void WriteRows(u64 screen) {
    const u32 goods = g_shown_goods.load(std::memory_order_acquire);
    if (!goods || (goods == g_rows_goods && screen == g_rows_screen)) return;
    Wrapper root;
    reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64)>(coop::Guest(kResolveHandle))(root.at(), screen + kPanelHandle);
    if (!*reinterpret_cast<const u64*>(root.at())) return;
    g_rows_goods = goods;
    g_rows_screen = screen;
    const int family = ChaliceFamily(goods);
    // the depth: the HolygrailParam row's first byte (checked against the cards on every tab)
    std::u16string depth;
    {
        ParamRef ref;
        reinterpret_cast<LookupFn>(coop::Guest(kHolygrailLookup))(&ref, goods);
        std::uint8_t d = 0;
        if (ref.row && coop::SafeGet(ref.row, &d) && d > 0 && d < 100)
            for (const char c : std::to_string(d)) depth.push_back(char16_t(c));
    }
    SetFieldText(root.at(), "GlyphDetailText/GlyphDepth/Text_0", BbGameMenu::GameText(kMenuText, kDepthLabel));
    SetFieldText(root.at(), "GlyphDetailText/GlyphDepth/Value_0", depth.c_str());
    SetFieldText(root.at(), "GlyphDetailText/GlyphArea/Text_0", BbGameMenu::GameText(kMenuText, kAreaLabel));
    SetFieldText(root.at(), "GlyphDetailText/GlyphArea/Value_0",
                 family >= 0 ? BbGameMenu::GameText(kMenuText, kAreaNames + u32(family)) : u"");
}
BB_COOP_SYSV void ScreenUpdate(u64 screen, u64, u64, u64, u64, u64) {
    ExpandDisplay(screen, "after the screen opened", true);
    if (IsGlyphScreen(screen)) WriteRows(screen);
}

// The generated panel movie, when bb-start made it (BB_PC_MENU_ASSETS): the game reads every menu movie once, at start.
bool MountPanel() {
    const char* assets = std::getenv("BB_PC_MENU_ASSETS");
    if (!assets || !*assets) return false;
    std::error_code ec;
    const std::filesystem::path panel = std::filesystem::path(assets) / "dvdroot_ps4/menu/itemdetailtext.gfx";
    if (!std::filesystem::is_regular_file(panel, ec)) return false;
    return runtime_file_mount(kGuestPanel, panel.string().c_str()) == 0;
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

// A card's icon: 0x1c5d680(out, row) makes an item row's icon descriptor (0x38 bytes: +0 the sheet frame, +4..+0x13
// the cell rectangle; all zero for no icon) from its item's icon id. A glyph's chalice can be one without an icon
// (the game has no icon for it); its card then gets the icon of its area's chalice (g_family_goods).
constexpr u64 kRowIcon = 0x1c5d680;
using RowIconFn = u64(BB_COOP_SYSV*)(u64 out, u64 row);
RowIconFn g_row_icon_original = nullptr;
BB_COOP_SYSV u64 RowIcon(u64 out, u64 row) {
    const u64 r = g_row_icon_original(out, row);
    std::uint8_t descriptor[0x14]{};
    std::memcpy(descriptor, reinterpret_cast<const void*>(out), sizeof descriptor);
    if (std::any_of(std::begin(descriptor), std::end(descriptor), [](std::uint8_t b) { return b != 0; })) return r;
    u32 fallback = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (const Glyph* g = GlyphOfRow(row)) {
            const int family = ChaliceFamily(g->goods);
            if (family >= 0 && g_family_goods[family] && g_family_goods[family] != g->goods) fallback = g_family_goods[family];
            static std::vector<u32> logged;
            if (fallback && std::find(logged.begin(), logged.end(), g->goods) == logged.end()) {
                logged.push_back(g->goods);
                std::printf("Glyph screen: chalice %u has no icon; its cards show chalice %u's\n", g->goods, fallback);
                std::fflush(stdout);
            }
        }
    }
    if (!fallback) return r;
    alignas(16) std::uint8_t copy[0xd8];
    std::memcpy(copy, reinterpret_cast<const void*>(row), sizeof copy);
    const u32 iid = 0x40000000u | fallback;
    std::memcpy(copy + 0x4c, &iid, 4);
    return g_row_icon_original(out, reinterpret_cast<u64>(copy));
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
        if (const Glyph* g = GlyphOfRow(row)) {
            g_shown_goods.store(g->goods, std::memory_order_release);  // the panel's Depth / Area rows (WriteRows)
            return reinterpret_cast<u64>(g->desc.c_str());
        }
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
        case kCustomTabText: return u"Your Dungeons";
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
    ok = ok && coop::ReplacePrologue(kMakeTabs, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, reinterpret_cast<const void*>(&MakeTabs),
                                     reinterpret_cast<void**>(&g_make_tabs_original), "glyph screen: the Custom tab");
    ok = ok && coop::ReplacePrologue(kTabName, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, reinterpret_cast<const void*>(&TabName),
                                     reinterpret_cast<void**>(&g_tab_name_original), "glyph screen: the Custom tab's name");
    ok = ok && coop::HookCallSite(kTabCollectCall, kCollect, reinterpret_cast<const void*>(&TabCollect),
                                  "glyph screen: the tab filter's collector");
    ok = ok && coop::HookCallSite(kNameCall, kItemName, reinterpret_cast<const void*>(&ItemName),
                                  "glyph screen: card names");
    ok = ok && coop::ReplacePrologue(kRowIcon, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, reinterpret_cast<const void*>(&RowIcon),
                                     reinterpret_cast<void**>(&g_row_icon_original), "glyph screen: card icons");
    ok = ok && coop::HookPrologue(kDescBinder, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &DescEntry,
                                  "glyph screen: description rows");
    ok = ok && coop::HookCallSite(kDescCall, kGoodsDesc, reinterpret_cast<const void*>(&DescText),
                                  "glyph screen: descriptions");
    if (ok && MountPanel()) {
        g_panel = coop::HookPrologue(kScreenSetup, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &ScreenSetup,
                                     "glyph screen: item screen setup");
        g_panel = g_panel && coop::HookPrologue(kPanelGetter, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &PanelGetter,
                                                "glyph screen: item screen panel getter");
        g_panel = g_panel && coop::HookCallSite(kSetupFetchCall, kChildFetch, reinterpret_cast<const void*>(&PanelFetch),
                                                "glyph screen: its description panel (setup)");
        g_panel = g_panel && coop::HookCallSite(kGetterFetchCall, kChildFetch, reinterpret_cast<const void*>(&PanelFetch),
                                                "glyph screen: its description panel (text)");
        g_panel = g_panel && coop::HookPrologue(kStatsRefresh, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &StatsRefresh,
                                                "glyph screen: item screen stats refresh");
        g_panel = g_panel && coop::HookCallSite(kRuneFillCall, kRuneFill, reinterpret_cast<const void*>(&RuneFill),
                                                "glyph screen: the stats panel's rune list");
        g_panel = g_panel && coop::HookCallSite(kAddDisplayCall, kAddControl, reinterpret_cast<const void*>(&AddControl),
                                                "glyph screen: no Display control");
        g_panel = g_panel && coop::HookCallSite(kAddToggleStatsCall, kAddControl,
                                                reinterpret_cast<const void*>(&AddControl),
                                                "glyph screen: no Toggle Stats control");
        g_panel = g_panel && coop::HookCallSite(kStartModeCall, kApplyMode, reinterpret_cast<const void*>(&StartMode),
                                                "glyph screen: opens with Display on");
        g_panel = g_panel && coop::HookPrologue(kScreenOpen, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &ScreenOpen,
                                                "glyph screen: item screen open");
        g_panel = g_panel && coop::HookPrologue(kScreenUpdate, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56}, &ScreenUpdate,
                                                "glyph screen: item screen update");
        if (!g_panel) runtime_file_unmount(kGuestPanel);
    }
    g_installed.store(ok, std::memory_order_release);
    std::printf("Glyph screen: %s%s\n", ok ? "on (BB_GLYPH_SCREEN_TEST)" : "FAILED to install",
                ok ? (g_panel ? ", its own panel" : ", the game's panel") : "");
    std::fflush(stdout);
}

} // namespace BbGlyphScreen
