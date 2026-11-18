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
// The setting "Chalice Glyph Browser" (bbport.ini chalice_glyph_browser, on by default; off falls back to the text list; it needs the community list,
// chalice_glyphdb), read each time the altar's glyph item is chosen: the hooks are always in and do nothing while it
// is off. BB_GLYPH_SCREEN_TEST=1 turns it on whatever the setting (the test scripts).
#include "bbport_glyph_screen.h"
#include "bbport_settings_screen.h"
#include "bbport_game_menu.h"
#include "bbport_settings.h"
#include "bbport_menu_widgets.h"
#include "bbport_osk.h"
#include "input/input.h"
#include "input/menu_pointer.h"
#include "../bbgpu.h"
#include "input/bindings.h"

#include "party/coop_hooks.h"
#include "party/game_state.h"
#include "party/party_chalice_glyphdb.h"
#include "party/party_chalice_glyphs.h"
#include "net/bbnet_internal.h"
#include "net/from_api.h"
#include "net/party_util.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
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

// Nightwatch: the Settings screen (bbport_settings_screen.h) is the same book in a second mode: set by OpenSettings
// (the pause menu's System), its menu object taken by MakeTabs, cleared by any other opening of the book. While it is
// up the glyph screen's own state stays off. Under g_mu, except the flag.
std::atomic<bool> g_settings{false};
bool g_settings_opening = false;
bool g_from_settings = false;
u64 g_settings_menu = 0;
u64 g_settings_list = 0;  // the tab's fresh list (after the screen filter), its rows turned away by TabCollect
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

// The glyph screen's own card list (in the generated itemlist_normalitem.gfx: tools/engine/menu_assets.cpp
// GlyphListMovie): six cards under the search bar. An item screen's list widget is made by 0x1b10dd0(holder = the
// screen + 0xf10, type; 0 the item cards), which names the widget's display object with 0x1acd650(out, printf-style
// format) at call 0x1b10e61 ("ItemList/NormalItem"; the glyph screen's "ItemList/GlyphNormalItem"). Its first lookup
// (0x1f688a0 from holder + 0x68) finds nothing: the list movie is not that root's child (checked on every screen).
// The widget then finds its list lazily: 0x1ad8580(out, widget + 0x40) fills widget + 0x70 by the movie's name
// (0x1ad8750) and fetches the path from the root 0x1adb030(root, widget + 0x48) resolves. ShowList finds the list
// parts the same way, once widget + 0x70 is set. The list movie is shared, as the panel movie is: the glyph screen
// shows its list parts and hides NormalItem, every other screen the reverse; none of them is moved by a timeline
// (ItemList, their parent, is).
constexpr char kGuestList[] = "/app0/dvdroot_ps4/menu/itemlist_normalitem.gfx";
constexpr u64 kListFactory = 0x1b10dd0;
constexpr u64 kListPathCall = 0x1b10e61;
constexpr u64 kFormat = 0x1acd650;
// The widget is then made by 0x1b10500(widget, list wrapper, name) at call 0x1b10f14, `name` a 24-byte record passed
// on the stack ({4, u"ItemList_NormalItem", "ItemList/NormalItem"}) that it keeps at +0x58; the glyph screen's names
// its own list there too.
constexpr u64 kListMakeCall = 0x1b10f14;
constexpr u64 kListMake = 0x1b10500;
struct ListName {
    u64 kind;
    u64 wide;
    const char* path;
};
constexpr u64 kListHolder = 0xf10;
constexpr u64 kWidgetMovie = 0x70, kWidgetRoot = 0x48;  // the widget's movie (0 until found) and its root handle
constexpr const char* kGlyphListPath = "ItemList/GlyphNormalItem";
constexpr const char* kGlyphListParts[] = {"ItemList/GlyphListBack", "ItemList/GlyphNormalItem", "ItemList/GlyphSearch"};
bool g_list = false;                 // the generated list movie is mounted and its hooks in
thread_local u64 t_list_holder = 0;  // the list factory now running: its holder
thread_local bool t_list_ours = false;  // ... and it makes the glyph screen's card list
std::unordered_map<u64, u64> g_list_widgets;  // screen -> its card list widget (menu thread)
u64 g_list_screen = 0;               // the screen the list parts were last set for (menu thread)

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

// The search's filter (under g_mu): while the bar holds text, a tab's rows are only its glyphs whose code holds it
// (lowercase), those whose code begins with it first, then by where it is (the cache's order within); empty: all.
// Kept after Done (the screen's next search starts from it), cleared by a cancel, an emptied bar or the screen opening.
std::string g_filter;
std::string g_search_text;               // the bar's text as typed, for the window's next search (under g_mu)
std::atomic<unsigned> g_rows_serial{0};  // the glyph tabs' rows built so far (TabCollect)
std::string Lower(const std::string& text) {
    std::string out;
    for (const unsigned char c : text) out += static_cast<char>(std::tolower(c));
    return out;
}
// Where `want` (lowercase) is in `code`: its first place (0: the code begins with it), or -1.
int MatchAt(const std::string& code, const std::string& want) {
    if (want.empty()) return 0;
    const std::size_t at = Lower(code).find(want);
    return at == std::string::npos ? -1 : static_cast<int>(at);
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

// The tabs' icons. A tab tile shows the frame of inventory.gfx's tab tile (sprite 228) named by the descriptor's +8
// (0x1c6a400 -> 0x1ad9ab0 -> gotoAndStop; the picker's own tabs: 0x39..0x3c, its four chalices). On the glyph
// screen: Farming the ritual materials (2), Equipment the weapon hand (4), Blood Gems the blood gem (8); Testing and
// Your Dungeons the generated movie's grey System and Status icons (61, 62: tools/engine/menu_assets.cpp
// GlyphTabsMovie), or, without that movie, the picker's chalice (60) and first chalice (57) as before.
constexpr u64 kTabFrame = 0x8;
constexpr char kGuestInventory[] = "/app0/dvdroot_ps4/menu/inventory.gfx";
bool g_tab_icons = false;  // the generated inventory.gfx (frames 61, 62) is mounted
u32 TabFrame(u32 tab) {
    switch (tab) {
        case 0x24: return 2;
        case 0x25: return 4;
        case 0x26: return 8;
        case 0x27: return g_tab_icons ? 61 : 0x3c;
        case kCustomTab: return g_tab_icons ? 62 : 0x39;
        default: return 0;
    }
}
void SetTabIcons(u64 menu, bool settings = false) {
    const u64 tabs = menu + ((0 - menu) & 0xf);
    const u64 count = std::min<u64>(*reinterpret_cast<const u64*>(menu + kTabCount), kMostTabs);
    for (u64 i = 0; i < count; ++i) {
        const u64 t = tabs + kTabSize * i;
        const u32 id = *reinterpret_cast<const u32*>(t);
        u32 frame = settings ? 0 : TabFrame(id);
        if (settings)
            for (const auto& s : BbSettingsScreen::Tabs())
                if (s.id == id) frame = g_tab_icons ? s.frame : s.fallback;
        if (frame) *reinterpret_cast<u32*>(t + kTabFrame) = frame;
    }
}

// A tab after the book's own: the first tab's descriptor (its maker) with the id `id`, added at the end of the menu's
// tab array. False when the array is full.
bool AddTabLike(u64 menu, u32 id) {
    const u64 count = *reinterpret_cast<const u64*>(menu + kTabCount);
    if (count + 1 >= kMostTabs) return false;
    alignas(16) std::uint8_t tab[kTabSize]{};
    const u64 t = reinterpret_cast<u64>(tab);
    reinterpret_cast<u64(BB_COOP_SYSV*)(u64)>(coop::Guest(kFirstTabMaker))(t);
    *reinterpret_cast<u32*>(t) = id;
    const u64 tabs = menu + ((0 - menu) & 0xf);
    reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64)>(coop::Guest(kAddTab))(tabs + kTabSize * count, t);
    *reinterpret_cast<u64*>(menu + kTabCount) = count + 1;
    for (const u64 at : {0x80, 0xb0, 0xe0, 0x110, 0x140}) {  // the maker's std::functions, as 0x1c77a50 ends them
        const u64 f = *reinterpret_cast<const u64*>(t + at);
        if (!f) continue;
        const u64 destroy = *reinterpret_cast<const u64*>(*reinterpret_cast<const u64*>(f) + 0x20);
        reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64)>(destroy)(f, f != t + at - 0x20 ? 1 : 0);
    }
    return true;
}

BB_COOP_SYSV u64 MakeTabs(u64 a0, u64 menu) {
    const u64 r = g_make_tabs_original(a0, menu);
    bool opening = false, settings = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        opening = g_opening;
        if (g_settings_opening) {  // the Settings screen's book: this menu is it
            g_settings_opening = false;
            g_settings_menu = menu;
            settings = true;
        }
    }
    if (settings) {
        std::size_t added = 0;
        const auto tabs = BbSettingsScreen::Tabs();
        for (std::size_t i = BbSettingsScreen::kBookTabs; i < tabs.size(); ++i) added += AddTabLike(menu, tabs[i].id);
        SetTabIcons(menu, true);
        std::printf("Settings screen: menu 0x%llx, %llu tabs (%zu added)\n", static_cast<unsigned long long>(menu),
                    static_cast<unsigned long long>(*reinterpret_cast<const u64*>(menu + kTabCount)), added);
        std::fflush(stdout);
        return r;
    }
    if (!opening) return r;
    struct TabIcons {  // the icons once the tabs are all in, whichever way MakeTabs leaves
        u64 menu;
        ~TabIcons() { SetTabIcons(menu); }
    } icons{menu};
    const u64 count = *reinterpret_cast<const u64*>(menu + kTabCount);
    if (!AddTabLike(menu, kCustomTab)) return r;
    std::printf("Glyph screen: the Custom tab added (tab %llu)\n", static_cast<unsigned long long>(count + 1));
    std::fflush(stdout);
    return r;
}
BB_COOP_SYSV u64 TabName(u64 out, u64 tab) {
    const u32 id = *reinterpret_cast<const u32*>(tab);
    if (g_settings.load(std::memory_order_acquire)) {
        const auto tabs = BbSettingsScreen::Tabs();
        for (std::size_t i = BbSettingsScreen::kBookTabs; i < tabs.size(); ++i)
            if (tabs[i].id == id)
                return reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64, u64)>(coop::Guest(0x1ae8cc0))(out, 0xc8,
                                                                                                    tabs[i].text);
    }
    if (id == kCustomTab && g_screen.load(std::memory_order_acquire))
        return reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64, u64)>(coop::Guest(0x1ae8cc0))(out, 0xc8, kCustomTabText);
    return g_tab_name_original(out, tab);
}

// The setting (or the test switch): the glyph screen replaces the altar's community glyph list.
bool BrowserOn() {
    static const bool forced = [] {
        const char* e = std::getenv("BB_GLYPH_SCREEN_TEST");
        return e && e[0] == '1';
    }();
    // A hook that failed its byte check leaves the ones before it in: they do nothing then (the game's altar).
    if (!g_installed.load(std::memory_order_acquire)) return false;
    if (forced) return true;
    const auto& s = BbSettings::Get();
    return s.chalice_glyph_browser.load(std::memory_order_relaxed) && s.chalice_glyphdb.load(std::memory_order_relaxed);
}
// The altar's "Search by Chalice Glyph": with community glyphs, the chalice picker opens as the glyph screen.
BB_COOP_SYSV u64 GlyphItem(u64 out, u64 ctx) {
    if (!BrowserOn()) return g_glyph_item_original(out, ctx);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        LoadGlyphs();
        if (!g_glyphs || g_glyphs->empty()) {
            std::printf("Glyph screen: no community glyphs; the glyph item opens the game's glyph box\n");
            std::fflush(stdout);
            return g_glyph_item_original(out, ctx);
        }
        g_opening = true;
        g_filter.clear();
        g_search_text.clear();
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
using BbMenuWidgets::Wrapper;// The parts of one panel (`panel`, a display object wrapper) shown or hidden; how many were found.
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
// The controller's keyboard (GlyphDetailText/GlyphKeyboard, tools/engine/menu_assets.cpp): shown in the column's
// place while a controller types in the search bar; the column's parts but its background hidden meanwhile (the
// text fills show them again: PanelFetch hides them after).
constexpr const char* kKeyboardHides[] = {"GlyphPart_15", "GlyphPart_19", "GlyphPart_23", "GlyphPart_34",
                                          "DetailDesc",   "ItemName",     "GlyphDepth",   "GlyphArea"};
std::atomic<bool> g_keyboard_shown{false};
std::atomic<bool> g_panel_blank{false};  // the game asked the glyph screen's panel hidden: no card to show (PanelShown)
// The keyboard's cursor (any thread): its view (<< 8: 0 the A-Z grid, 1 QWERTY; kept for the session, through
// searches and the screen closing) and its key in that view (& 0xff), in one value (one store moves both).
std::atomic<int> g_key{0};
std::atomic<bool> g_keyboard_wanted{false};   // a controller types (any thread)

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
        if (ours && (g_keyboard_shown.load(std::memory_order_acquire) || g_panel_blank.load(std::memory_order_acquire)) &&
            glyph_parts)
            ShowParts(glyph.at(), kKeyboardHides, false);
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

// The glyph screen's controls: Display is always on, Toggle Stats is not offered and Display's control is Search.
//
// The item screen's controls setup 0x1b999c0 (rdi = the screen) adds each control with 0x1ad5730(screen, control,
// action, ...): Display at call 0x1b9a660, Toggle Stats at call 0x1b9a7f0; the glyph screen skips Toggle Stats (its
// key and its hint). Display's key (0x1ae1340: button code 0x14, its label the text made at call 0x1b9a5d5) is
// Search's on the glyph screen (the search bar's section). It starts the screen in display mode 0 (+0x1168; +0x1300 the number of modes, 3) and applies it with
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
using BbMenuWidgets::SetFieldText;
using BbMenuWidgets::SetValueText;
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
// The list factory: which screen's list is being made (ListPath).
BB_COOP_SYSV void ListFactory(u64 holder, u64, u64, u64, u64, u64) {
    t_list_holder = holder;
    t_list_ours = false;
}
BB_COOP_SYSV u64 ListMake(u64 widget, u64 list, ListName name) {
    if (t_list_ours) name.path = kGlyphListPath;
    if (t_list_holder >= kListHolder) {
        const u64 screen = t_list_holder - kListHolder;
        g_list_widgets[screen] = widget;
        if (g_list_screen == screen) g_list_screen = 0;  // a new list (or a new screen at a freed one's address)
    }
    return reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64, ListName)>(coop::Guest(kListMake))(widget, list, name);
}
BB_COOP_SYSV u64 ListPath(u64 out, const char* format) {
    const bool ours = t_list_holder >= kListHolder && IsGlyphScreen(t_list_holder - kListHolder);
    t_list_ours = ours;
    if (ours) {
        std::printf("Glyph screen: its card list is %s\n", kGlyphListPath);
        std::fflush(stdout);
    }
    return reinterpret_cast<u64(BB_COOP_SYSV*)(u64, const char*, ...)>(coop::Guest(kFormat))(out, ours ? kGlyphListPath
                                                                                                   : format);
}
// The list parts for `screen`: on the glyph screen its own, on any other NormalItem. Once per screen, as soon as its
// list widget has found its movie.
// The root of `screen`'s list movie, once its list widget has found the movie.
bool ListRoot(u64 screen, Wrapper& root) {
    const auto it = g_list_widgets.find(screen);
    if (it == g_list_widgets.end()) return false;
    u64 movie = 0;
    if (!coop::SafeGet(it->second + kWidgetMovie, &movie) || !movie) return false;  // not found yet
    reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64)>(coop::Guest(kResolveHandle))(root.at(), it->second + kWidgetRoot);
    return true;
}
// A display object wrapper's object (its value's data + 0x88), or 0.
u64 DisplayObject(Wrapper& w) {
    u64 data = 0, object = 0;
    if ((w.Type() & 0x8f) != 10 || !coop::SafeGet(w.at() + 0x50, &data) || !data) return 0;
    return coop::SafeGet(data + 0x88, &object) ? object : 0;
}

// The search bar's focus. The glyph screen's card list is one of the game's generic lists (0x1acff30 updates the
// focused one each frame: rdi the list, rsi its input, whose first byte is the focus); a list's +0x74 is its cursor
// (+0x78 an override when >= 0), +0x24c the first line shown, +0x258 its item provider (vtable +0x18 makes the
// wrapper of a shown cell), +0xc8 / +0xd0 its root value (type, data; the display object at data + 0x88). A move is
// made in one of two places: a one-column list's early step 0x1ad10f0(list, &{x, y}) at call 0x1ad0062, or the
// step 0x1ad2860(list + 0x290, input, functor) at call 0x1ad00a1 returns ({x, y} in its low and high halves). On the
// glyph list, Up on its first card gives the bar the focus (the cards' highlight hidden with 0x1acf0e0, the bar's
// Cursor shown); while the bar has it, no move reaches the list and Down gives the focus back to the first card.
// The screen opens, and every tab starts, with the bar's focus. The mouse: over the bar it takes the focus, over the
// cards it gives the cards the focus (the game's pointer code then moves their highlight; a click is an OK press).
// OK: an item screen's controls (its controls setup 0x1b999c0, AddControl 0x1ad5730(screen, key, action,
// condition), each a std::function) begin with Back (action 0x1ba9b00: the screen ends with 3 through its vtable
// +0x40; on while no child screen is open) and then OK (action 0x1ba80f0, the screen at functor + 8: decides the
// list's current item; on while the list has one, vtable +0x48). On the glyph screen with the bar's focus OK decides
// nothing (the first card would be picked); the bar takes it. Back is left as it is.
constexpr u64 kOkAction = 0x1ba80f0, kBackAction = 0x1ba9b00;
// The item screen's "panel shown" listener (a std::function's call: functor + 8 = the screen, the argument a bool):
// SetVisible(bool) on the description panel (the wrapper at screen + 0xdd8, else the panel's root, screen + 0xda8, at
// 0x1ba9fd1 / 0x1baa04f). The game hides the panel when the list has no item to show: on the glyph screen, a search
// that leaves the tab empty; the controller's keyboard lives in that panel.
constexpr u64 kPanelShown = 0x1ba9f30;
// Search: Display's control on the glyph screen (its label our text, its action and condition replaced there: on
// while no child screen is open, the search bar taking the focus and the typing starting at once). On the keyboard
// a letter or digit starts the search with itself (TypeStartsSearch, from the window before the game sees the key),
// but for Search's own key.
constexpr u64 kControlsSetup = 0x1b999c0, kDisplayLabelCall = 0x1b9a5d5, kTextOf = 0x1ae8cc0;
constexpr u64 kDisplayAction = 0x1ba6760, kDisplayCondition = 0x1ba6400;
constexpr u32 kSearchLabel = 263191;  // a menu text id of ours (0xca), not one the game has
constexpr u64 kChildScreen = 0x40, kChildScreen2 = 0x10;  // as OK's condition: no screen over this one
constexpr u64 kEarlyStepCall = 0x1ad0062, kEarlyStep = 0x1ad10f0;
constexpr u64 kStepCall = 0x1ad00a1, kStep = 0x1ad2860, kStepOffset = 0x290;
constexpr u64 kSetCursor = 0x1acf6b0;
constexpr u64 kBoundsSlot = 0x1f0, kStageMatrix = 0x2040;  // a display object's bounds; its matrix to the stage
constexpr u64 kHideCursors = 0x1acf0e0, kCursorName = 0x4934044;  // kCursorName: the item's highlight, "Cursor"
constexpr u64 kListCursor = 0x74, kListCursorOverride = 0x78, kListFirstLine = 0x24c, kListProvider = 0x258;
constexpr u64 kListRootType = 0xc8, kListRootData = 0xd0;
std::atomic<u64> g_glyph_list_object{0};  // GlyphNormalItem's display object while the glyph screen is up
std::atomic<bool> g_bar_focus{false};
std::atomic<u64> g_glyph_list{0};             // the glyph screen's card list, once a step has seen it
std::atomic<bool> g_typing{false};            // the bar is typed in (a quiet text box is open)
std::atomic<bool> g_swallow{false};           // OK / Back after the typing ended, until their buttons are let go
std::atomic<long long> g_search_ready_ms{0};  // the last frame the glyph screen could start its search (any thread)
std::atomic<bool> g_search_started{false};    // the window started the search with a letter (taken by the screen)
thread_local u64 t_controls_screen = 0;       // the screen whose controls are being set up
bool g_bar_cursor_shown = false;              // the bar's Cursor as last set (menu thread)
bool g_bar_active_shown = false;              // the bar's Active (typed in) as last set (menu thread)
bool g_pointer_seen = false;                  // the pointer's last place (menu thread; reset when the screen opens)
float g_pointer_x = 0, g_pointer_y = 0;

bool IsGlyphList(u64 list) {
    const u64 ours = g_glyph_list_object.load(std::memory_order_acquire);
    u32 type = 0;
    u64 data = 0, object = 0;
    const bool is = ours && coop::SafeGet(list + kListRootType, &type) && (type & 0x8f) == 10 &&
                    coop::SafeGet(list + kListRootData, &data) && data && coop::SafeGet(data + 0x88, &object) &&
                    object == ours;
    if (is) g_glyph_list.store(list, std::memory_order_release);
    return is;
}
// Whether the stage point (x, y) is on `object` (its bounds through its matrix to the stage, in twips), as the
// game's pointer code tests its list items.
bool PointOn(u64 object, float x, float y) {
    u64 vtable = 0, bounds_fn = 0;
    if (!object || !coop::SafeGet(object, &vtable) || !coop::SafeGet(vtable + kBoundsSlot, &bounds_fn) || !bounds_fn)
        return false;
    alignas(16) float identity[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    alignas(16) float b[4] = {};
    reinterpret_cast<void(BB_COOP_SYSV*)(float*, u64, float*)>(bounds_fn)(b, object, identity);
    if (!(b[2] > b[0] && b[3] > b[1])) return false;
    alignas(16) float m[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, float*)>(coop::Guest(kStageMatrix))(object, m);
    const float det = m[0] * m[5] - m[1] * m[4];
    if (det > -1e-12f && det < 1e-12f) return false;
    const float px = x * 20 - m[3], py = y * 20 - m[7];
    const float lx = (m[5] * px - m[1] * py) / det, ly = (m[0] * py - m[4] * px) / det;
    return lx >= b[0] && lx <= b[2] && ly >= b[1] && ly <= b[3];
}
// Where the menu pointer is on the bar's screen: 1 on the bar, -1 on the cards, 0 elsewhere or no menu pointer.
int PointerPlace(u64 screen, bool* moved) {
    float x = 0, y = 0;
    if (moved) *moved = false;
    if (!BbInput::MouseIsMenuPointer() || !BbInput::PointerOnStage(x, y) || BbInput::TextEntryOpen()) return 0;
    if (moved) {
        *moved = g_pointer_seen && (x != g_pointer_x || y != g_pointer_y);
        g_pointer_seen = true;
        g_pointer_x = x;
        g_pointer_y = y;
    }
    Wrapper root, bar;
    if (!ListRoot(screen, root) || !bar.Fetch(root.at(), "ItemList/GlyphSearch")) return 0;
    if (PointOn(DisplayObject(bar), x, y)) return 1;
    return PointOn(g_glyph_list_object.load(std::memory_order_acquire), x, y) ? -1 : 0;
}
using BbMenuWidgets::CancelSound;
using BbMenuWidgets::CursorMoveSound;
using BbMenuWidgets::DecideSound;
using BbMenuWidgets::EnterSound;
using BbMenuWidgets::SwitchSound;
using BbMenuWidgets::TypeSound;
// The highlight of the card shown at `line` (one column: the cell is {0, line}).
void ShowCardCursor(u64 list, u32 line) {
    u64 provider = 0, vtable = 0, make = 0;
    if (!coop::SafeGet(list + kListProvider, &provider) || !provider || !coop::SafeGet(provider, &vtable) ||
        !coop::SafeGet(vtable + 0x18, &make) || !make)
        return;
    Wrapper item;
    u64 cell = u64(line) << 32;
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64, u64*)>(make)(item.at(), provider, &cell);
    if (!DisplayObject(item)) return;
    Wrapper highlight;
    if (highlight.Fetch(item.at(), reinterpret_cast<const char*>(coop::Guest(kCursorName)))) highlight.SetVisible(true);
}
// The vertical move `dy` on `list` once the bar has had its say (0: no move).
int BarStep(u64 list, int dy, const char* path) {
    if (!IsGlyphList(list)) return dy;
    std::int32_t cursor = -1, over = -1, first = -1;
    coop::SafeGet(list + kListCursor, &cursor);
    coop::SafeGet(list + kListCursorOverride, &over);
    coop::SafeGet(list + kListFirstLine, &first);
    const bool on_first = (over >= 0 ? over : cursor) == 0 && first == 0;
    if (g_bar_focus.load(std::memory_order_acquire)) {
        // the game shows the first card's highlight again when it fills the list (a new tab, the screen opening);
        // while the bar is typed in, the highlight is the matching card's
        if (!g_typing.load(std::memory_order_acquire))
            reinterpret_cast<void(BB_COOP_SYSV*)(u64)>(coop::Guest(kHideCursors))(list);
        if (dy > 0) {
            g_bar_focus.store(false, std::memory_order_release);
            if (!on_first) reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64)>(coop::Guest(kSetCursor))(list, 0);
            reinterpret_cast<void(BB_COOP_SYSV*)(u64)>(coop::Guest(kHideCursors))(list);
            ShowCardCursor(list, 0);
            CursorMoveSound();
            std::printf("Glyph screen: the cards have the focus again (Down, %s)\n", path);
            std::fflush(stdout);
        }
        return 0;
    }
    if (dy >= 0 || !on_first) return dy;
    g_bar_focus.store(true, std::memory_order_release);
    reinterpret_cast<void(BB_COOP_SYSV*)(u64)>(coop::Guest(kHideCursors))(list);
    CursorMoveSound();
    std::printf("Glyph screen: the search bar has the focus (Up on the first card, %s)\n", path);
    std::fflush(stdout);
    return 0;
}
BB_COOP_SYSV u64 EarlyStep(u64 list, u64 step) {
    std::int32_t xy[2] = {};
    if (coop::SafeGet(step, &xy) && IsGlyphList(list)) {
        const int dy = BarStep(list, xy[1], "early step");
        if (g_bar_focus.load(std::memory_order_acquire) || (dy == 0 && xy[1] != 0)) return 0;  // no move
    }
    return reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64)>(coop::Guest(kEarlyStep))(list, step);
}
BB_COOP_SYSV u64 Step(u64 at, u64 input, u64 functor) {
    const u64 r = reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64, u64)>(coop::Guest(kStep))(at, input, functor);
    const u64 list = at - kStepOffset;
    const int dy = static_cast<std::int32_t>(r >> 32);
    if (!IsGlyphList(list)) return r;
    const int now = BarStep(list, dy, "step");
    if (g_bar_focus.load(std::memory_order_acquire)) return 0;
    return (r & 0xffffffffull) | (u64(static_cast<u32>(now)) << 32);
}
// The cards take the focus from the bar (the mouse): the highlight shown on the card the list's cursor is on.
void CardsTakeFocus(u64 list, const char* why) {
    g_bar_focus.store(false, std::memory_order_release);
    std::int32_t cursor = -1, over = -1, first = 0;
    coop::SafeGet(list + kListCursor, &cursor);
    coop::SafeGet(list + kListCursorOverride, &over);
    coop::SafeGet(list + kListFirstLine, &first);
    const std::int32_t at = (over >= 0 ? over : cursor) - first;
    if (at >= 0) ShowCardCursor(list, static_cast<u32>(at));
    std::printf("Glyph screen: the cards have the focus again (%s)\n", why);
    std::fflush(stdout);
}
void StartSearch(u64 screen, bool open_box);
// The mouse on the glyph screen (each frame): moved onto the bar, the bar takes the focus; onto the cards, they do.
// A click on the bar (no list item: the menu pointer leaves it waiting) starts the search, as OK on the bar does.
void BarHover(u64 screen) {
    bool moved = false;
    const int place = PointerPlace(screen, &moved);
    const u64 list = g_glyph_list.load(std::memory_order_acquire);
    if (place > 0 && MenuPointer::TakeClick()) {
        if (g_search_ready_ms.load(std::memory_order_acquire)) {
            std::printf("Glyph screen: a click on the search bar\n");
            std::fflush(stdout);
            StartSearch(screen, true);
        }
        return;
    }
    if (!moved || !place || !list || !IsGlyphList(list)) return;
    const bool focus = g_bar_focus.load(std::memory_order_acquire);
    if (place > 0 && !focus) {
        g_bar_focus.store(true, std::memory_order_release);
        reinterpret_cast<void(BB_COOP_SYSV*)(u64)>(coop::Guest(kHideCursors))(list);
        CursorMoveSound();
        std::printf("Glyph screen: the search bar has the focus (the mouse)\n");
        std::fflush(stdout);
    } else if (place < 0 && focus) {
        CardsTakeFocus(list, "the mouse");
    }
}
using OkFn = void(BB_COOP_SYSV*)(u64);
OkFn g_ok_original = nullptr;
void BeginTyping(u64 screen);
BB_COOP_SYSV void OkAction(u64 functor) {
    u64 screen = 0;
    if (g_swallow.load(std::memory_order_acquire) && coop::SafeGet(functor + 8, &screen) && IsGlyphScreen(screen))
        return;  // the key that ended the typing
    if (g_bar_focus.load(std::memory_order_acquire) && coop::SafeGet(functor + 8, &screen) && IsGlyphScreen(screen)) {
        // a click on a card the pointer rests on (not moved since the screen opened) is the cards' OK
        const u64 list = g_glyph_list.load(std::memory_order_acquire);
        if (PointerPlace(screen, nullptr) < 0 && list && IsGlyphList(list)) {
            CardsTakeFocus(list, "a click");
        } else {
            BeginTyping(screen);
            return;
        }
    }
    g_ok_original(functor);
}
// On the glyph screen the panel is never hidden: with no card to show (a search matching nothing, an empty tab) it
// stays, blank (GlyphDetailText's parts but its background hidden, as under the controller's keyboard), so the
// keyboard and the panel's frame stay where they are whichever way the search is typed.
using PanelShownFn = void(BB_COOP_SYSV*)(u64, const std::uint8_t*);
PanelShownFn g_panel_shown_original = nullptr;
// GlyphDetailText's parts shown or hidden (its background stays).
void ShowPanelParts(u64 screen, bool visible) {
    Wrapper root, column;
    reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64)>(coop::Guest(kResolveHandle))(root.at(), screen + kPanelHandle);
    if (*reinterpret_cast<const u64*>(root.at()) && column.Fetch(root.at(), "GlyphDetailText"))
        ShowParts(column.at(), kKeyboardHides, visible);
}
BB_COOP_SYSV void PanelShown(u64 functor, const std::uint8_t* shown) {
    u64 screen = 0;
    std::uint8_t want = 1;
    if (!coop::SafeGet(functor + 8, &screen) || !IsGlyphScreen(screen) ||
        !coop::SafeGet(reinterpret_cast<u64>(shown), &want)) {
        g_panel_shown_original(functor, shown);
        return;
    }
    static const std::uint8_t kOn = 1;
    g_panel_shown_original(functor, &kOn);
    const bool blank = !want;
    if (blank != g_panel_blank.exchange(blank, std::memory_order_acq_rel)) {
        std::printf("Glyph screen: the panel %s\n", blank ? "stays shown, blank (no card to show)" : "shows a card again");
        std::fflush(stdout);
    }
    if (!g_keyboard_shown.load(std::memory_order_acquire)) ShowPanelParts(screen, !blank);
}
void StartSearch(u64 screen, bool open_box);
using BackFn = void(BB_COOP_SYSV*)(u64);
BackFn g_back_original = nullptr;
BackFn g_display_original = nullptr;
using ConditionFn = bool(BB_COOP_SYSV*)(u64);
ConditionFn g_display_condition_original = nullptr;
bool NoChildScreen(u64 screen) {
    u64 a = 1, b = 1;
    return coop::SafeGet(screen + kChildScreen, &a) && coop::SafeGet(screen + kChildScreen2, &b) && !a && !b;
}
BB_COOP_SYSV void DisplayAction(u64 functor) {
    u64 screen = 0;
    if (coop::SafeGet(functor + 8, &screen) && IsGlyphScreen(screen)) {
        if (!g_swallow.load(std::memory_order_acquire) && !g_typing.load(std::memory_order_acquire))
            StartSearch(screen, true);
        return;
    }
    g_display_original(functor);
}
BB_COOP_SYSV bool DisplayCondition(u64 functor) {
    u64 screen = 0;
    if (coop::SafeGet(functor + 8, &screen) && IsGlyphScreen(screen)) return NoChildScreen(screen);
    return g_display_condition_original(functor);
}
BB_COOP_SYSV void ControlsSetup(u64 screen, u64, u64, u64, u64, u64) { t_controls_screen = screen; }
BB_COOP_SYSV u64 DisplayLabel(u64 out, u32 category, u32 id) {
    if (IsGlyphScreen(t_controls_screen)) id = kSearchLabel;
    return reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u32, u32)>(coop::Guest(kTextOf))(out, category, id);
}
BB_COOP_SYSV void BackAction(u64 functor) {
    u64 screen = 0;
    if (g_swallow.load(std::memory_order_acquire) && coop::SafeGet(functor + 8, &screen) && IsGlyphScreen(screen))
        return;  // the key that ended the typing
    g_back_original(functor);
}

// Typing in the bar. OK on the bar opens a quiet text box (the window takes the keys and the game none: Enter or
// Cross ends it with the cards' focus on the match, Esc or Circle with the bar's; Backspace or Square deletes). Each
// frame the bar shows the text with a blinking cursor, and a changed text jumps to the first glyph whose code
// begins with it (none: the highlight stays), looked for from the shown tab on in tab order (a tab is a category:
// tab index = category). Another tab is chosen as Left / Right choose one: the screen's tab list (a generic list
// at screen + 0xae0, its cursor at +0xb54 as OK's action reads it) moved with SetCursor, then its change listener's call
// (0x1baa470: the functor's screen) 0x1b9b690(screen, tab, 0), the screen's show-tab (the tab's name, its list
// and rows). A jump plays the cards' cursor sound. The card once the shown rows are that tab's: the
// card list's rows are the list holder's (screen + 0xf10) widget (+0xc0) source (+0x3a0): 0xd8-byte rows from
// source + 0x18, in the cursor's order.
constexpr u64 kTabList = 0xae0, kListItems = 0x70, kShowTab = 0x1b9b690;
constexpr u64 kHolderWidget = 0xc0, kWidgetSource = 0x3a0, kSourceRows = 0x18;
constexpr double kJumpWait = 1.5;  // seconds a jump waits for its tab's rows
std::string g_typed;               // the bar's text (game thread)
std::string g_matched_for;         // the text the last jump was for
int g_jump_glyph = -1;             // a jump waiting for its tab's rows: the glyph, its tab
int g_jump_tab = -1;
double g_jump_at = 0;
int g_swallow_frames = 0;
int g_match_glyph = -1;            // the glyph the cards' highlight is on (the last jump's)
unsigned g_jump_serial = 0;        // g_rows_serial when the jump asked for its rows
const Glyph* GlyphOfRow(u64 row);

std::u16string Utf16(const std::string& s) {  // the codes are ASCII; anything else shows as '?'
    std::u16string out;
    for (const unsigned char c : s) {
        if (c < 0x80) out += static_cast<char16_t>(c);
        else if ((c & 0xc0) != 0x80) out += u'?';
    }
    return out;
}
void ShowTyped(u64 screen, bool cursor) {
    Wrapper root, bar;
    if (!ListRoot(screen, root) || !bar.Fetch(root.at(), "ItemList/GlyphSearch")) return;
    const std::u16string shown = Utf16(g_typed) + (cursor ? u"|" : u"");
    SetFieldText(bar.at(), "Query", shown.c_str());
}
void BeginTyping(u64 screen) {
    if (!bbgpu_text_input_begin_quiet(g_typed.c_str())) {
        std::printf("Glyph screen: OK on the search bar, but there is no window to type in\n");
        std::fflush(stdout);
        return;
    }
    g_typing.store(true, std::memory_order_release);
    EnterSound();
    if (host_input_device() == InputDevice::Pad) {
        g_key.store(g_key.load(std::memory_order_acquire) & ~0xff, std::memory_order_release);  // the view kept
        g_keyboard_wanted.store(true, std::memory_order_release);
    }
    std::int32_t tabs = 0, tab = -1;
    coop::SafeGet(screen + kTabList + kListItems, &tabs);
    coop::SafeGet(screen + kTabList + kListCursor, &tab);
    std::printf("Glyph screen: typing in the search bar (tab list: %d tabs, on %d)\n", tabs, tab);
    std::fflush(stdout);
}
int ShownTab(u64 screen) {
    std::int32_t tab = -1, over = -1;
    coop::SafeGet(screen + kTabList + kListCursor, &tab);
    coop::SafeGet(screen + kTabList + kListCursorOverride, &over);
    return over >= 0 ? over : tab;
}
// The card of glyph `k` in the shown card list, or -1 (the rows are not that tab's yet). Under g_mu.
int CardOf(u64 screen, int k) {
    u64 widget = 0, source = 0, begin = 0, end = 0;
    if (!coop::SafeGet(screen + kListHolder + kHolderWidget, &widget) || !widget ||
        !coop::SafeGet(widget + kWidgetSource, &source) || !source || !coop::SafeGet(source + kSourceRows, &begin) ||
        !coop::SafeGet(source + kSourceRows + 8, &end) || end < begin)
        return -1;
    const u64 n = (end - begin) / kRowSize;
    for (u64 i = 0; i < n && i < 4096; ++i) {
        const Glyph* g = GlyphOfRow(begin + i * kRowSize);
        if (g && g - g_glyphs->data() == k) return static_cast<int>(i);
    }
    return -1;
}
// The glyph a text jumps to: on the shown tab when it has a match, else on the first tab after it that has one; that
// tab's best (its filtered rows' first: the code beginning with the text, else the earliest place). Under g_mu.
int MatchOf(const std::string& typed, int from_tab) {
    if (typed.empty() || !g_glyphs) return -1;
    const std::string want = Lower(typed);
    const int tabs = static_cast<int>(std::size(kCategories));
    for (int t = 0; t < tabs; ++t) {
        const int cat = ((from_tab < 0 ? 0 : from_tab) + t) % tabs;
        int best = -1, best_at = 0;
        for (std::size_t k = 0; k < g_glyphs->size(); ++k) {
            const Glyph& g = (*g_glyphs)[k];
            if (g.category != cat) continue;
            const int at = MatchAt(g.code, want);
            if (at >= 0 && (best < 0 || at < best_at)) best = static_cast<int>(k), best_at = at;
        }
        if (best >= 0) return best;
    }
    return -1;
}
// The filter's matches on `tab`. Under g_mu.
int MatchesOn(int tab) {
    if (!g_glyphs) return 0;
    int n = 0;
    for (const Glyph& g : *g_glyphs)
        if (g.category == tab && MatchAt(g.code, g_filter) >= 0) ++n;
    return n;
}
// The cards' highlight on card `card` of the shown list.
void HighlightCard(u64 list, int card) {
    reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64)>(coop::Guest(kSetCursor))(list, static_cast<u64>(card));
    std::int32_t first = 0;
    coop::SafeGet(list + kListFirstLine, &first);
    reinterpret_cast<void(BB_COOP_SYSV*)(u64)>(coop::Guest(kHideCursors))(list);
    if (card - first >= 0) ShowCardCursor(list, static_cast<u32>(card - first));
}
// A waiting jump, once the shown rows are its tab's.
void FinishJump(u64 screen) {
    if (g_jump_glyph < 0) return;
    const u64 list = g_glyph_list.load(std::memory_order_acquire);
    int card = -1;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        card = CardOf(screen, g_jump_glyph);
    }
    const bool rebuilt = g_rows_serial.load(std::memory_order_acquire) != g_jump_serial;
    const bool late = Seconds() - g_jump_at > kJumpWait;
    if (card >= 0 && list && IsGlyphList(list) && ShownTab(screen) == g_jump_tab && (rebuilt || late)) {
        if (!rebuilt) {
            std::printf("Glyph screen: \"%s\": tab %d's rows were not built again (the filter not applied)\n",
                        g_typed.c_str(), g_jump_tab);
        }
        HighlightCard(list, card);  // silent: the typing that moved it has its own sound
        g_match_glyph = g_jump_glyph;
        std::printf("Glyph screen: \"%s\" is on tab %d, card %d\n", g_typed.c_str(), g_jump_tab, card);
        std::fflush(stdout);
        g_jump_glyph = -1;
    } else if (late) {
        std::printf("Glyph screen: \"%s\": tab %d's rows never came (shown tab %d, card %d)\n", g_typed.c_str(),
                    g_jump_tab, ShownTab(screen), card);
        std::fflush(stdout);
        g_jump_glyph = -1;
    }
}
// The bar's text changed: the filter follows it, the best match's tab (or the shown one, without a match) shows its
// rows again, filtered, and the best card takes the highlight once they are built (FinishJump).
void Jump(u64 screen) {
    if (g_typed == g_matched_for) return;
    g_matched_for = g_typed;
    const int shown = ShownTab(screen);
    int k = -1, tab = -1;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_filter = Lower(g_typed);
        g_search_text = g_typed;
        k = MatchOf(g_typed, shown);
        if (k >= 0) tab = (*g_glyphs)[static_cast<std::size_t>(k)].category;
    }
    if (tab < 0) tab = shown;
    if (tab < 0) return;
    if (tab != shown)
        reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64)>(coop::Guest(kSetCursor))(screen + kTabList,
                                                                                 static_cast<u64>(tab));
    g_jump_serial = g_rows_serial.load(std::memory_order_acquire);
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, u32, u32)>(coop::Guest(kShowTab))(screen, static_cast<u32>(tab), 0);
    std::printf("Glyph screen: \"%s\": tab %d filtered (%s; shown tab now %d)\n", g_typed.c_str(), tab,
                k >= 0 ? "a match" : g_typed.empty() ? "no filter" : "no match", ShownTab(screen));
    std::fflush(stdout);
    if (k < 0) {
        g_jump_glyph = g_match_glyph = -1;
        return;
    }
    g_jump_glyph = k;
    g_jump_tab = tab;
    g_jump_at = Seconds();
    FinishJump(screen);
}
// The search starts: the bar takes the focus (the cards' highlight hidden) and the typing starts (`open_box`: the
// quiet text box opened here; else the window has opened it with the first letter).
void StartSearch(u64 screen, bool open_box) {
    const u64 list = g_glyph_list.load(std::memory_order_acquire);
    g_bar_focus.store(true, std::memory_order_release);
    if (list && IsGlyphList(list)) reinterpret_cast<void(BB_COOP_SYSV*)(u64)>(coop::Guest(kHideCursors))(list);
    if (open_box) {
        BeginTyping(screen);
        return;
    }
    g_typing.store(true, std::memory_order_release);
    EnterSound();
    std::printf("Glyph screen: typing in the search bar (started by a letter)\n");
    std::fflush(stdout);
}
// The controller's keyboard (GlyphDetailText/GlyphKeyboard): two views of 38 keys, the A-Z grid (Grid: a..z, 0..9 in
// 6 rows of 6) and QWERTY (Qwerty: 1..0, q..p, a..l, z..m, its rows staggered), each Key_0 .. Key_35 in its letters'
// order, then Delete (Key_36) and Done (Key_37) under them. The window thread moves its cursor, switches its view
// (Triangle / Y) and presses its keys (KeyboardPress); the screen shows it (from the first controller press, or when
// a controller started the typing), its view and its key's highlight (Hot).
const BbOsk::Layout& Keys() { return BbOsk::GlyphLayout(); }
int KeyPacked(int view, int key) { return view << 8 | key; }
int g_key_shown = -1;   // the key whose Hot shows (menu thread)
int g_view_shown = -1;  // the view shown (menu thread)
std::string HotPath(int view, int key) {
    return Keys().views[std::size_t(view)].name + "/Key_" + std::to_string(key) + "/Hot";
}
// The glyph screen's keyboard part (`path` under GlyphDetailText/GlyphKeyboard) shown or hidden.
void ShowKeyboardPart(u64 screen, const std::string& path, bool visible) {
    Wrapper root, part;
    reinterpret_cast<u64(BB_COOP_SYSV*)(u64, u64)>(coop::Guest(kResolveHandle))(root.at(), screen + kPanelHandle);
    if (!*reinterpret_cast<const u64*>(root.at())) return;
    if (part.Fetch(root.at(), ("GlyphDetailText/GlyphKeyboard" + (path.empty() ? "" : "/" + path)).c_str()))
        part.SetVisible(visible);
}
void HideKeyHot(u64 screen) {
    if (g_key_shown >= 0 && g_view_shown >= 0) ShowKeyboardPart(screen, HotPath(g_view_shown, g_key_shown), false);
    g_key_shown = -1;
}
// The keyboard shown or hidden (the column's parts the other way); shown, with the enter sound; hidden, with the
// cancel sound (Done's decide sound, queued after it at the same priority, takes its place).
void ShowKeyboard(u64 screen, bool visible) {
    if (visible == g_keyboard_shown.load(std::memory_order_acquire)) return;
    g_keyboard_shown.store(visible, std::memory_order_release);
    // the column's parts: hidden under the keyboard; back with it gone, unless the panel is blank (no card to show)
    ShowPanelParts(screen, !visible && !g_panel_blank.load(std::memory_order_acquire));
    HideKeyHot(screen);
    g_view_shown = -1;
    ShowKeyboardPart(screen, "", visible);
    // shown: the sound of the search starting (a controller taking over from the keyboard sounds as it would have)
    if (visible) EnterSound();
    else CancelSound();
    std::printf("Glyph screen: the controller's keyboard %s\n", visible ? "shown" : "hidden");
    std::fflush(stdout);
}
// Each frame while typed in: the keyboard, its view and its key's highlight.
void DrawKeyboard(u64 screen) {
    const bool want = g_typing.load(std::memory_order_acquire) && g_keyboard_wanted.load(std::memory_order_acquire);
    ShowKeyboard(screen, want);
    if (!want) return;
    const int packed = g_key.load(std::memory_order_acquire);
    const int view = packed >> 8 & 1, key = packed & 0xff;
    if (view != g_view_shown) {
        HideKeyHot(screen);
        for (int i = 0; i < 2; ++i) ShowKeyboardPart(screen, Keys().views[std::size_t(i)].name, i == view);
        if (g_view_shown >= 0) {
            SwitchSound();
            std::printf("Glyph screen: the controller's keyboard: the %s view\n", view ? "QWERTY" : "A-Z grid");
            std::fflush(stdout);
        }
        g_view_shown = view;
    } else if (key == g_key_shown) {
        return;
    } else if (g_key_shown >= 0) {
        HideKeyHot(screen);
        CursorMoveSound();
    }
    ShowKeyboardPart(screen, HotPath(view, key), true);
    g_key_shown = key;
}
// The list's "No Chalice Glyphs match ..." line: shown while the filter leaves the shown tab no card.
std::string g_no_match_shown;  // the text it shows, "" hidden (menu thread)
void ShowNoMatch(u64 screen) {
    std::string want;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_filter.empty() && MatchesOn(ShownTab(screen)) == 0) want = g_search_text;
    }
    if (want == g_no_match_shown) return;
    Wrapper root, line;
    if (!ListRoot(screen, root) || !line.Fetch(root.at(), "ItemList/GlyphNoMatch")) return;
    if (!want.empty()) {
        const std::u16string text = u"No Chalice Glyphs match \"" + Utf16(want) + u"\"";
        SetFieldText(root.at(), "ItemList/GlyphNoMatch", text.c_str());
    }
    line.SetVisible(!want.empty());
    g_no_match_shown = want;
}
// While the search bar is typed in, the screen's help line and button prompts are the typing's: the help line set by
// 0x1ad6210(screen, text) into the field at screen + 0x1f8; the prompts rebuilt by 0x1ad6450(screen) (every screen's
// vtable +0x28; the item screen's too) from the controls whose condition holds, as the key guide's markup ("<font
// color='#A5A58C'>%s(U+FF1A)%s</font>", U+3000 between) into the field at screen + 0x2a8. Both replaced: the game's
// own runs, then ours over it while typing. The keys: the game's button images (the engine's key prompts draw them
// for the device; with a text box open on the keyboard Cross reads Enter, Circle Esc and Square Bksp). Options
// (Done on a controller) has no image in the item screen's movies: not listed.
using BbMenuWidgets::kHelpField;
using BbMenuWidgets::kPromptsField;
using BbMenuWidgets::Prompt;
int g_prompts_mode = 0;  // the prompts shown: 0 the screen's, 1 typing on the keyboard, 2 on the controller's keyboard
int PromptsMode() {
    if (!g_typing.load(std::memory_order_acquire)) return 0;
    return g_keyboard_wanted.load(std::memory_order_acquire) ? 2 : 1;
}
void TypingPrompts(u64 screen, int mode) {
    u64 help = 0, prompts = 0;
    coop::SafeGet(screen + kHelpField, &help);
    coop::SafeGet(screen + kPromptsField, &prompts);
    SetValueText(help, mode ? u"Type a glyph code to narrow the list." : Text(0xc9, 263000));
    if (!mode) return;
    const std::u16string gap = u"\x3000";
    const std::u16string row =
        mode == 2 ? Prompt(u"KG_L_UDLR", u"Move") + gap + Prompt(u"KG_OK", u"Type") + gap + Prompt(u"KG_R_L", u"Delete") +
                        gap + Prompt(u"KG_Cancel", u"Cancel")
                  : Prompt(u"KG_OK", u"Done") + gap + Prompt(u"KG_Cancel", u"Cancel") + gap + Prompt(u"KG_R_L", u"Delete");
    SetValueText(prompts, row.c_str());
}
// After the game sets any screen's help line or redraws its prompts (BbMenuWidgets::AddPromptsHook): the glyph
// screen's typing ones over them.
void GlyphPrompts(u64 screen) {
    if (IsGlyphScreen(screen) && g_prompts_mode) TypingPrompts(screen, g_prompts_mode);
}
// Each frame: the prompts follow the typing (started, ended, the controller's keyboard shown or hidden).
void ShowTypingPrompts(u64 screen) {
    const int mode = PromptsMode();
    if (mode == g_prompts_mode) return;
    g_prompts_mode = mode;
    BbMenuWidgets::RefreshPrompts(screen);  // ours over the game's, or the game's
    if (!mode) TypingPrompts(screen, 0);  // the screen's help line back
    std::printf("Glyph screen: the button prompts: %s\n",
                mode == 2 ? "the controller's keyboard" : mode == 1 ? "typing on the keyboard" : "the screen's");
    std::fflush(stdout);
}
// Each frame on the glyph screen: the typed text, the typing's end, and the keys that ended it.
void Typing(u64 screen) {
    if (g_search_started.exchange(false, std::memory_order_acq_rel)) StartSearch(screen, false);
    const bool ready = !g_typing.load(std::memory_order_acquire) && !g_swallow.load(std::memory_order_acquire) &&
                       g_glyph_list_object.load(std::memory_order_acquire) && NoChildScreen(screen);
    g_search_ready_ms.store(ready ? static_cast<long long>(Seconds() * 1000) : 0, std::memory_order_release);
    if (g_swallow.load(std::memory_order_acquire)) {
        const std::uint32_t held = BbInput::LastPad().buttons;
        g_swallow_frames = held & (PadBits::kCross | PadBits::kCircle) ? 0 : g_swallow_frames + 1;
        if (g_swallow_frames >= 10) g_swallow.store(false, std::memory_order_release);
    }
    FinishJump(screen);
    if (!g_typing.load(std::memory_order_acquire)) return;
    char text[64] = {};
    const int state = bbgpu_text_input_poll(text, sizeof text);
    if (state != 0) g_keyboard_wanted.store(false, std::memory_order_release);
    DrawKeyboard(screen);
    const bool changed = state == 0 && std::strlen(text) != g_typed.size();
    g_typed = text;
    Jump(screen);
    // typed or deleted (any way), as the name entry sounds it: after the list's refresh (the game's tab switch), so
    // that the typing's sound is the one this frame plays
    if (changed) TypeSound();
    const bool blink = static_cast<long long>(Seconds() * 2) % 2 == 0;
    ShowTyped(screen, state == 0 && blink);
    if (state == 0) return;
    g_typing.store(false, std::memory_order_release);
    if (state == 1) DecideSound();
    g_swallow_frames = 0;
    g_swallow.store(true, std::memory_order_release);
    const u64 list = g_glyph_list.load(std::memory_order_acquire);
    const bool to_card = state == 1 && g_match_glyph >= 0 && g_jump_glyph < 0 && list && IsGlyphList(list);
    if (to_card) {
        g_bar_focus.store(false, std::memory_order_release);
    } else if (list && IsGlyphList(list)) {
        reinterpret_cast<void(BB_COOP_SYSV*)(u64)>(coop::Guest(kHideCursors))(list);
    }
    std::printf("Glyph screen: typing ended (%s): \"%s\"; the %s has the focus\n",
                state == 1 ? "confirmed" : "cancelled", g_typed.c_str(), to_card ? "matching card" : "search bar");
    std::fflush(stdout);
}

void ShowList(u64 screen) {
    if (!g_list || screen == g_list_screen) return;
    Wrapper root;
    if (!ListRoot(screen, root)) return;  // not found yet: the next frame
    Wrapper normal;
    if (!normal.Fetch(root.at(), "ItemList/NormalItem")) {
        std::printf("Glyph screen: screen 0x%llx: its list movie has no ItemList/NormalItem (type 0x%x)\n",
                    static_cast<unsigned long long>(screen), normal.Type());
        std::fflush(stdout);
        g_list_screen = screen;  // once
        return;
    }
    const bool ours = IsGlyphScreen(screen);
    int found = 0;
    for (const char* path : kGlyphListParts) {
        Wrapper part;
        if (!part.Fetch(root.at(), path)) continue;
        part.SetVisible(ours);
        ++found;
    }
    normal.SetVisible(!ours);
    g_list_screen = screen;
    if (found != static_cast<int>(std::size(kGlyphListParts))) {
        std::printf("Glyph screen: only %d of the %zu glyph list parts found\n", found, std::size(kGlyphListParts));
        std::fflush(stdout);
    }
    Wrapper cards, bar_cursor;
    g_glyph_list_object.store(ours && cards.Fetch(root.at(), kGlyphListPath) ? DisplayObject(cards) : 0,
                              std::memory_order_release);
    // the glyph screen opens with the bar's focus (the cards' highlight hidden at the list's next update)
    g_bar_focus.store(g_glyph_list_object.load(std::memory_order_acquire) != 0, std::memory_order_release);
    if (bar_cursor.Fetch(root.at(), "ItemList/GlyphSearch/Cursor")) bar_cursor.SetVisible(false);
    Wrapper bar_active;
    if (bar_active.Fetch(root.at(), "ItemList/GlyphSearch/Active")) bar_active.SetVisible(false);
    g_bar_active_shown = false;
    g_bar_cursor_shown = false;
    // The "No Chalice Glyphs match ..." line starts hidden on every screen of this movie (Inventory, Chalice Ritual,
    // the glyph screen again): the last search's line would otherwise stay over their lists.
    Wrapper no_match;
    if (no_match.Fetch(root.at(), "ItemList/GlyphNoMatch")) no_match.SetVisible(false);
    g_pointer_seen = false;
    g_typed.clear();
    g_matched_for.clear();
    g_no_match_shown.clear();
    g_panel_blank.store(false, std::memory_order_release);
    g_prompts_mode = 0;
    g_jump_glyph = g_match_glyph = -1;
    if (ours) {
        Wrapper bar;
        if (bar.Fetch(root.at(), "ItemList/GlyphSearch")) SetFieldText(bar.at(), "Query", u"");
        std::printf("Glyph screen: its card list and search bar shown%s\n",
                    g_glyph_list_object.load() ? "" : " (its cards' list NOT found: the bar cannot take the focus)");
        std::fflush(stdout);
    }
}
// The bar's highlight follows its focus (on the glyph screen).
// The bar's look follows its state: focused, its Cursor (the cards' pulsing highlight); typed in, Active (a steady
// gold frame) instead.
void ShowBarCursor(u64 screen) {
    const bool typing = g_typing.load(std::memory_order_acquire);
    const bool focus = g_bar_focus.load(std::memory_order_acquire) && !typing;
    if (focus == g_bar_cursor_shown && typing == g_bar_active_shown) return;
    Wrapper root, cursor, active;
    if (!ListRoot(screen, root) || !cursor.Fetch(root.at(), "ItemList/GlyphSearch/Cursor") ||
        !active.Fetch(root.at(), "ItemList/GlyphSearch/Active"))
        return;
    cursor.SetVisible(focus);
    active.SetVisible(typing);
    g_bar_cursor_shown = focus;
    g_bar_active_shown = typing;
}
BB_COOP_SYSV void ScreenUpdate(u64 screen, u64, u64, u64, u64, u64) {
    ShowList(screen);
    if (IsGlyphScreen(screen)) {
        Typing(screen);
        ShowTypingPrompts(screen);
        ShowNoMatch(screen);
        BarHover(screen);
        ShowBarCursor(screen);
    }
    ExpandDisplay(screen, "after the screen opened", true);
    if (IsGlyphScreen(screen)) WriteRows(screen);
}

// A generated movie, when bb-start made it (BB_PC_MENU_ASSETS): the game reads every menu movie once, at start.
bool MountMovie(const char* guest, const char* relative) {
    const char* assets = std::getenv("BB_PC_MENU_ASSETS");
    if (!assets || !*assets) return false;
    std::error_code ec;
    const std::filesystem::path movie = std::filesystem::path(assets) / relative;
    if (!std::filesystem::is_regular_file(movie, ec)) return false;
    return runtime_file_mount(guest, movie.string().c_str()) == 0;
}
bool MountPanel() { return MountMovie(kGuestPanel, "dvdroot_ps4/menu/itemdetailtext.gfx"); }

// The altar menu (builder 0x1b66470) adds its rows with 0x1b76150(list, texts, handler, 0), each followed by setting
// the list's LAST row's network rule. Its rows: 0x180 bytes from the list's +0x68 to +0x70; it adds Chalice Ritual
// (262000), Chalice Dungeon Search (262001) and Search by Chalice Glyph (262002) unconditionally, in that order.
// While the browser is on, Chalice Dungeon Search goes, as the game erases a row (the System builder, 0x1bb403d):
// each later row moved down with 0x1bab760(to, from), the last one destroyed by its vtable +8, the end moved back.
// Done right after the glyph row is added and before its network rule is set (that rule then lands on it, the last
// row again): the filter below runs there.
constexpr u64 kAltarRowsBegin = 0x68, kAltarRowsEnd = 0x70, kAltarRowSize = 0x180, kRowMove = 0x1bab760;
void DropDungeonSearch(u64 list) {
    u64& end = *reinterpret_cast<u64*>(list + kAltarRowsEnd);
    const u64 begin = *reinterpret_cast<const u64*>(list + kAltarRowsBegin);
    if (end < begin || (end - begin) != 3 * kAltarRowSize) {  // not the three rows the builder makes first: left alone
        std::printf("Glyph screen: the altar menu has %llu rows here, not 3: Chalice Dungeon Search kept\n",
                    static_cast<unsigned long long>(end >= begin ? (end - begin) / kAltarRowSize : 0));
        std::fflush(stdout);
        return;
    }
    const u64 search = begin + kAltarRowSize, glyph = begin + 2 * kAltarRowSize;
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, u64)>(coop::Guest(kRowMove))(search, glyph);
    const u64 vtable = *reinterpret_cast<const u64*>(glyph);
    reinterpret_cast<void(BB_COOP_SYSV*)(u64)>(*reinterpret_cast<const u64*>(vtable + 8))(glyph);
    end -= kAltarRowSize;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        std::printf("Glyph screen: the altar menu without Chalice Dungeon Search\n");
        std::fflush(stdout);
    }
}

// The altar menu greys "Search by Chalice Glyph" without the network (0x553b120: +8, +0x9f8, +0x9f6, +0xa50 -> a byte
// at the builder's rbp - 0x1458, stored on the row at 0x1b66823). The glyph screen needs no network: with community
// glyphs the row stays usable. `list`: the altar's row list (r12, just returned by the glyph row's add).
BB_COOP_SYSV u64 GlyphRowGrey(u64 frame, u64 list) {
    if (!BrowserOn()) return 0;
    DropDungeonSearch(list);
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
    if (g_from_settings) return;  // the Settings screen's book: the glyph screen is off, Settings stays
    g_settings_opening = false;
    g_settings_menu = 0;
    g_settings_list = 0;
    g_settings.store(false, std::memory_order_release);
}

// After the screen filter: `tab` = the tab descriptor (the game's r12), `frame` = the builder's rbp (the fresh list
// at -0x2d8), `menu` = the menu object (r15).
BB_COOP_SYSV u64 AfterScreenFilter(u64 tab, u64 frame, u64 menu) {
    {  // the Settings screen: no rows of the game's (TabCollect turns this list's rows away)
        std::lock_guard<std::mutex> lk(g_mu);
        if (menu && menu == g_settings_menu) {
            u64 list = 0;
            g_settings_list = coop::SafeGet(frame - 0x2d8, &list) ? list : 0;
            return 0;
        }
    }
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
    std::vector<std::pair<int, std::size_t>> order;  // (where the filter is in the code, the glyph)
    for (std::size_t i = 0; i < g_glyphs->size(); ++i) {
        const Glyph& g = (*g_glyphs)[i];
        if (g.category != category) continue;
        const int at = MatchAt(g.code, g_filter);
        if (at >= 0) order.emplace_back(at, i);
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [at, i] : order) {
        const Glyph& g = (*g_glyphs)[i];
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
    std::printf("Glyph screen: tab 0x%x (%s): %zu glyph row(s)%s%s%s\n", tab_id, kCategories[category],
                g_pending.glyph.size(), g_filter.empty() ? "" : " matching \"", g_filter.c_str(),
                g_filter.empty() ? "" : "\"");
    std::fflush(stdout);
    return 0;
}

// The tab filter's collector on the glyph screen: the player's rows are turned away, ours pass and get their mark.
BB_COOP_SYSV u64 TabCollect(u64 out, u64 src_ref, u64 filter) {
    // a new tab on the glyph screen: the bar has the focus (its cards' list is known while the screen is up)
    if (g_glyph_list_object.load(std::memory_order_acquire) && !g_bar_focus.exchange(true))
        std::printf("Glyph screen: the search bar has the focus (a new tab)\n");
    const auto collect = reinterpret_cast<CollectFn>(coop::Guest(kCollect));
    std::unique_lock<std::mutex> lk(g_mu);
    u64 src = 0;
    // The Settings screen's tab: every row turned away (kind 0 while the collector runs), so the list is empty.
    if (g_settings_list && coop::SafeGet(src_ref, &src) && src == g_settings_list) {
        g_settings_list = 0;
        u64 begin = 0, end = 0;
        coop::SafeGet(src + 0x18, &begin);
        coop::SafeGet(src + 0x20, &end);
        const std::size_t rows = end > begin ? (end - begin) / kRowSize : 0;
        std::vector<u32> kinds(rows, 0);
        for (std::size_t i = 0; i < rows; ++i) {
            u32* kind = reinterpret_cast<u32*>(begin + i * kRowSize + 0x50);
            kinds[i] = *kind;
            *kind = 0;
        }
        lk.unlock();
        const u64 r = collect(out, src_ref, filter);
        lk.lock();
        for (std::size_t i = 0; i < rows; ++i) *reinterpret_cast<u32*>(begin + i * kRowSize + 0x50) = kinds[i];
        return r;
    }
    src = 0;
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
    g_rows_serial.fetch_add(1, std::memory_order_acq_rel);
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

bool SettingsReady() { return g_installed.load(std::memory_order_acquire); }

std::uint64_t OpenSettings(std::uint64_t out, std::uint64_t arg) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_settings_opening = true;
        g_from_settings = true;
        g_settings_menu = 0;
        g_settings_list = 0;
        g_settings.store(true, std::memory_order_release);  // before the book's setup asks for its title
    }
    std::printf("Settings screen: the pause menu's System opens the Settings screen\n");
    std::fflush(stdout);
    const u64 r = reinterpret_cast<HandlerFn>(coop::Guest(kRitualItem))(out, arg);
    std::lock_guard<std::mutex> lk(g_mu);
    g_from_settings = false;
    if (g_settings_opening) {  // the book made no tabs: not ours after all
        g_settings_opening = false;
        std::printf("Settings screen: the book made no tabs\n");
        std::fflush(stdout);
    }
    return r;
}

const char16_t* Text(std::uint32_t category, std::uint32_t id) {
    if (!g_installed.load(std::memory_order_acquire)) return nullptr;
    if (g_settings.load(std::memory_order_acquire)) return BbSettingsScreen::Text(category, id);
    if (!g_screen.load(std::memory_order_acquire)) return nullptr;
    // The chalice picker's own texts (the title and help at 0x1c74d2a / 0x1c74db8, the tab names at 0x1c69a05..):
    // names 0xc8, help lines 0xc9.
    if (category == 0xc8) {
        switch (id) {
        case 263100: return u"Chalice Glyph Browser";
        case 40435: return u"Farming";
        case 40436: return u"Equipment";
        case 40437: return u"Blood Gems";
        case 40438: return u"Testing";
        case kCustomTabText: return u"Your Dungeons";
        default: return nullptr;
        }
    }
    if (category == 0xc9 && id == 263000) return u"Select a Chalice Glyph to join its dungeon.";
    if (category == 0xca && id == kSearchLabel) return u"Search";
    return nullptr;
}

int KeyboardPress(PadKey key) {
    if (key == PadKey::kNone) return 0;
    if (!g_keyboard_wanted.exchange(true, std::memory_order_acq_rel)) {  // the first press shows the keyboard
        g_key.store(g_key.load(std::memory_order_acquire) & ~0xff, std::memory_order_release);  // its view kept
        if (key != PadKey::kDelete && key != PadKey::kDone && key != PadKey::kCancel) return 0;
    }
    const int packed = g_key.load(std::memory_order_acquire);
    const int view = packed >> 8 & 1, k = packed & 0xff;
    switch (key) {
    case PadKey::kUp:
    case PadKey::kDown:
    case PadKey::kLeft:
    case PadKey::kRight: g_key.store(KeyPacked(view, BbOsk::Moved(Keys(), view, k, key)), std::memory_order_release); return 0;
    case PadKey::kToggle:
        g_key.store(KeyPacked(1 - view, BbOsk::InView(Keys(), view, 1 - view, k)), std::memory_order_release);
        return 0;
    case PadKey::kPress:
        return k < Keys().Letters() ? Keys().views[std::size_t(view)].letters[std::size_t(k)]
               : k == Keys().Wide("Delete") ? kTypedDelete : kTypedDone;
    case PadKey::kDelete: return kTypedDelete;
    case PadKey::kDone: return kTypedDone;
    case PadKey::kCancel: return kTypedCancel;
    default: return 0;
    }
}
void KeyboardKeyTyped() { g_keyboard_wanted.store(false, std::memory_order_release); }

std::string SearchText() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_search_text;
}

bool TypeStartsSearch(int scancode) {
    const long long ready = g_search_ready_ms.load(std::memory_order_acquire);
    if (!ready || static_cast<long long>(Seconds() * 1000) - ready > 100) return false;  // not up, or gone
    if (scancode == host_binding_key(kBindUseItem)) return false;  // Search's own key (Square): its action
    g_search_ready_ms.store(0, std::memory_order_release);
    g_search_started.store(true, std::memory_order_release);
    return true;
}

bool KeepsGameText(std::uint32_t id) {
    // The altar names its items before it builds them (before the glyph list is first read), so: the screen's hooks
    // are in and the community list's file is there (the glyph item then opens the glyph screen).
    if (id != kGlyphSearchText || !g_installed.load(std::memory_order_acquire) || !BrowserOn()) return false;
    if (g_have_glyphs.load(std::memory_order_acquire)) return true;
    std::error_code ec;
    return std::filesystem::is_regular_file(CachePath(), ec);
}

const char16_t* AltarItemText(std::uint32_t category, std::uint32_t id) {
    // The same id is the item's name (0xc8), its help line (0xc9) and its offline notice (0xcc: the game's, kept).
    if (!KeepsGameText(id)) return nullptr;
    if (category == 0xc8) return u"Chalice Glyph Browser";
    if (category == 0xc9) return u"Browse community-favorite Chalice Dungeons made using Glyphs.";
    return nullptr;
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
    bool ok = coop::ReplacePrologue(kGlyphItem, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57},
                                    reinterpret_cast<const void*>(&GlyphItem),
                                    reinterpret_cast<void**>(&g_glyph_item_original), "glyph screen: the glyph item");
    ok = ok && coop::HookValueFilter(kGlyphRowGrey, {0x88, 0x8d, 0xa8, 0xeb, 0xff, 0xff},  // mov [rbp-0x1458], cl
                                     // mov rdi, rbp; mov rsi, [r12]: the stub keeps r12 for itself, the game's r12
                                     // (the altar's row list) is the value it pushed there (coop_hooks SaveVolatile)
                                     {0x48, 0x89, 0xef, 0x49, 0x8b, 0x34, 0x24},
                                     &GlyphRowGrey, kGlyphRowGrey + 6,
                                     "glyph screen: the glyph row usable, no Chalice Dungeon Search");
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
    if (g_panel && MountMovie(kGuestList, "dvdroot_ps4/menu/itemlist_normalitem.gfx")) {
        g_list = coop::HookPrologue(kListFactory, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &ListFactory,
                                    "glyph screen: item screen list factory");
        g_list = g_list && coop::HookCallSite(kListPathCall, kFormat, reinterpret_cast<const void*>(&ListPath),
                                              "glyph screen: its card list");
        g_list = g_list && coop::HookCallSite(kListMakeCall, kListMake, reinterpret_cast<const void*>(&ListMake),
                                              "glyph screen: its card list's name");
        g_list = g_list && coop::HookCallSite(kEarlyStepCall, kEarlyStep, reinterpret_cast<const void*>(&EarlyStep),
                                              "glyph screen: search bar focus (one-column step)");
        g_list = g_list && coop::HookCallSite(kStepCall, kStep, reinterpret_cast<const void*>(&Step),
                                              "glyph screen: search bar focus (list step)");
        g_list = g_list && coop::ReplacePrologue(kOkAction, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56},
                                                 reinterpret_cast<const void*>(&OkAction),
                                                 reinterpret_cast<void**>(&g_ok_original),
                                                 "glyph screen: OK on the search bar");
        g_list = g_list && BbMenuWidgets::AddPromptsHook(&GlyphPrompts);  // the help line and prompts while typing
        g_list = g_list && coop::ReplacePrologue(kPanelShown, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56},
                                                 reinterpret_cast<const void*>(&PanelShown),
                                                 reinterpret_cast<void**>(&g_panel_shown_original),
                                                 "glyph screen: the panel kept for the controller's keyboard");
        g_list = g_list && coop::HookPrologue(kControlsSetup, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &ControlsSetup,
                                              "glyph screen: item screen controls setup");
        g_list = g_list && coop::HookCallSite(kDisplayLabelCall, kTextOf, reinterpret_cast<const void*>(&DisplayLabel),
                                              "glyph screen: Search's label (Display's control)");
        g_list = g_list && coop::ReplacePrologue(kDisplayAction, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56},
                                                 reinterpret_cast<const void*>(&DisplayAction),
                                                 reinterpret_cast<void**>(&g_display_original),
                                                 "glyph screen: Search (Display's action)");
        g_list = g_list && coop::ReplacePrologue(kDisplayCondition, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56},
                                                 reinterpret_cast<const void*>(&DisplayCondition),
                                                 reinterpret_cast<void**>(&g_display_condition_original),
                                                 "glyph screen: Search's condition (Display's)");
        g_list = g_list && coop::ReplacePrologue(kBackAction, {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8b, 0x7f, 0x08},
                                                 reinterpret_cast<const void*>(&BackAction),
                                                 reinterpret_cast<void**>(&g_back_original),
                                                 "glyph screen: Back after typing in the search bar");
        if (!g_list) runtime_file_unmount(kGuestList);
    }
    // The tab icons' movie: the game's inventory.gfx and two frames more, which only the glyph screen's tabs name.
    g_tab_icons = ok && MountMovie(kGuestInventory, "dvdroot_ps4/menu/inventory.gfx");
    g_installed.store(ok, std::memory_order_release);
    std::printf("Glyph screen: %s%s%s%s (Chalice Glyph Browser %s)\n", ok ? "installed" : "FAILED to install",
                ok ? (g_panel ? ", its own panel" : ", the game's panel") : "", ok && g_list ? ", its own card list" : "",
                ok ? (g_tab_icons ? ", its own tab icons" : ", the chalice tab icons for Testing and Your Dungeons") : "",
                BrowserOn() ? "on" : "off: the hooks do nothing");
    std::fflush(stdout);
}

} // namespace BbGlyphScreen
