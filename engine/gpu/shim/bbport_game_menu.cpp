// SPDX-License-Identifier: GPL-2.0-or-later
// The port's pages in the game's System menu: the glue to the game's menu code. What the pages
// hold is in bbport_game_menu_pages.{h,cpp}; this file only builds them with the game's rows.
//
// Row / page API (bbport_game_menu_pages.h):
//  - Pages::All(): the pages, in System menu order (after "Screen/Sound"); each Page has a
//    name, a help text and at most RowsPerPage Rows (more may be defined when some are hidden:
//    Row::visible is checked when the page opens; the first RowsPerPage visible rows are shown).
//  - Row: a field (index of the value it edits), name / help text ids, value text ids
//    (`choices`) and a Kind: Toggle (PairRow: two values, left/right), Slider (SliderRow 0..10),
//    Steps (SliderRow standing for a few choices; the help line says which positions), List
//    (ListRow pop-up; only as a page's last row, it opens over the rows below), Button (PairRow
//    with ONE value: the game draws no arrows and left/right have nothing to step to, as
//    droogie/bbhost's key binding rows; Cross on it while it has the page's cursor runs it,
//    PollButtons below).
//  - Texts: ids from Pages::IdBase, answered by LookupHook in the game's language. Run-time
//    ("dynamic") texts are fixed buffers rewritten in place by Pages::RefreshDynamic (party
//    state, member rows), so a row can show live values.
//  - Values: Pages::Value / Byte (what the rows bind) and DefaultValue / DefaultByte (the
//    game's "Default"). Pages::Load() fills them from bbport_settings when a page opens (Rows<N>),
//    Pages::Poll() (each presented frame, BbGameMenu::Poll) applies changes, saves bbport.ini and
//    runs buttons (a toggle row whose value change is an action; Confirm buttons on the second
//    press).
//  - Party page: an action table. Pages::AddPartyRow(PartyRow) before PatchImage adds a row
//    (order, texts made from party::status::Board, Press None / Once / Confirm, run(board), or a
//    settings toggle); built-in rows use the same table (status 10, copy code 20, rejoin 30,
//    leave 40; the notices switch and the last event are on Party members).
//  - New pages: add them in Pages::DefineAll (MaxPages below caps the System items).
#include "bbport_game_menu.h"
#include "party/party_give.h"
#include "party/party_flow.h"
#include "net/bbnet_internal.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <malloc.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "bbport_game_menu_pages.h"
#include "bbport_settings.h"

extern "C" void* runtime_low_map(size_t size, int prot); // src/runtime_memory.c
extern "C" void runtime_restart(void);                  // src/probe.c
extern "C" void bbgpu_request_quit(void);              // bbgpu.cpp
extern "C" std::uint32_t runtime_pad_cross_presses(void); // src/runtime_pad.c

// The game's code is called, and calls the hooks below, with the System V ABI (the PS4's): the
// default on Linux, said explicitly for Windows.
#define BB_SYSV __attribute__((sysv_abi))

namespace BbGameMenu {
namespace {

// ---- Platform (bbport: Windows port) ----
void* AlignedAlloc(std::size_t size) {
#ifdef _WIN32
    return _aligned_malloc(size, 16); // no C11 aligned_alloc in the Windows C runtime
#else
    return std::aligned_alloc(16, size);
#endif
}
void AlignedFree(void* p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}
std::size_t PageSize() {
#ifdef _WIN32
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return std::size_t(info.dwPageSize);
#else
    return std::size_t(sysconf(_SC_PAGESIZE));
#endif
}

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// The game's menu code (eboot 1.09 vaddrs; found from the System menu builder 0x1bb3ad0).
constexpr u64 MsgLookup = 0xf6f340;      ///< (repository, table, category, id) -> UTF-16 text
constexpr u64 AddPageItem = 0x1b4e2a0;   ///< (menu, name+help texts, page factory, out)
constexpr u64 MakeText = 0x1ae8cc0;      ///< (out 0x40 bytes, category, id): a text entry
constexpr u64 FactoryToFunction = 0x1b6ed90; ///< ({vtable, fn}, out std::function) -> its __f_
constexpr u64 CreatePage = 0x1bb4c70;    ///< (out, arg, content std::function): an options page
constexpr u64 OpenPage = 0x1b20900;      ///< (a, b, layout name, rows fn, 0, 0): the page's content
constexpr u64 ListPush = 0x1b1bbd0;      ///< (value list, {u32 value, text}): up to 32 values
constexpr u64 ListRow = 0x1b78ab0;   ///< (page, texts, int* value, value list, int* default): pop-up list
constexpr u64 PairPush = 0x1b2c200;  ///< (two-value list, {u8 value, text})
constexpr u64 PairRow = 0x1b2a100;   ///< (page, texts, u8* value, two-value list, u8* default): left/right
constexpr u64 SliderRow = 0x1b2ac00; ///< (page, texts, u8* value 0..10, u8* default)
constexpr u64 FactoryVtable = 0x533d120; ///< a System item's {vtable, page factory fn}
constexpr u64 ContentVtable = 0x5343880; ///< an options page's {vtable, content fn}
constexpr u64 ControlsLayout = 0x4934065; ///< "ControllSetting": six plain rows, the layout used
constexpr u32 TextName = 0xc8, TextHelp = 0xc9; // SP_menu text / SP_one-line help
constexpr u32 ScreenSoundItem = 110001;         // "Screen/Sound" in the System menu: ours follow it
constexpr u32 ControlsTitle = 113020;           // the ControllSetting layout's title ("Controls")

const u8 MsgLookupPrologue[] = {0x89, 0xf0, 0x48, 0x8b, 0x77, 0x08, 0x48, 0x8b, 0x34, 0xc6};
const u8 AddPageItemPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
// Nightwatch: the pause menu's System item factory (out, arg): it runs the System menu builder (0x1bb3ad0:
// an "OptionSetting" list, the game's rows through AddPageItem, which returns the menu it was given) and
// stores the list in *out. Party Settings is the same list, built while AddHook swaps in the party pages.
constexpr u64 SystemListFactory = 0x1bb3a40;
const u8 SystemListFactoryPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x10};
// The title screen's main menu (builder 0x1b4a3d0, docs/party/ingame_flow.md §3): its command items
// (Continue 0x61f95, Load Game 0x61f96, New Game 0x61f97, ...) go through AddCommandItem; the
// party flow adds the Party page as a title item right after New Game (BB_PARTY_TITLE_ENTRY=0: not).
constexpr u64 AddCommandItem = 0x1b4c9a0; ///< (command list, name+help texts, functor, 0) -> the list
constexpr u32 TitleNewGame = 0x61f97;
const u8 AddCommandItemPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
// 0.7.8 give items (party_give.h): the inventory's item command menu (FUN_01ba0e70, "CommandList": Use, Discard ...)
// builds its entries with AddCommandItem; each entry's functor is a polymorphic object {vtable, item context copy
// at +8 (the item's inventory index at +0x50, type | id at +0x54)}; the Discard entry's operator() opens the
// quantity dialog and returns it as a ref-counted menu object. A "Give" entry is added after Discard's own set-up,
// just before the builder's finalize call (0x1bea630), with a vtable copied from the Discard command's and its
// clone / operator() slots replaced (the game clones the functor into the entry).
constexpr u64 ItemMenuBuilder = 0x1ba0e70, ItemMenuBuilderEnd = 0x1ba215a;
constexpr u64 ItemMenuFinalize = 0x1bea630;     ///< (out, command list, finalizer fn)
constexpr u64 CopyContext = 0x1c5da00;          ///< (dst, src): the item context's copy constructor
constexpr u64 GameNew = 0x2bbe9a8;               ///< operator new
constexpr u64 DiscardCommandVtable = 0x533fea0;  ///< the Discard command's functor class (8 slots)
constexpr u32 ItemMenuDiscard = 102011;          ///< SP_menu text "Discard"
const u8 FinalizePrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};

u64 image_base = 0;
template <typename F>
F Game(u64 va) {
    return reinterpret_cast<F>(image_base + va);
}

// ---- Texts: the port's ids (Pages::IdBase up), looked up by the game like its own messages. ----
namespace P = BbGameMenu::Pages;
using P::Kind;
using P::Row;

/// The game's own language decides (its "Screen/Sound" text in Cyrillic: Russian, else English);
/// the port's menu language until the game has looked that text up.
enum class GameLanguage { Unknown, English, Russian };
std::atomic<GameLanguage> game_language{GameLanguage::Unknown};

const char16_t* TextOf(u32 id) {
    const GameLanguage game = game_language.load();
    const bool russian = game == GameLanguage::Unknown
                             ? BbSettings::Get().menu_language == BbSettings::MenuLanguage::Russian
                             : game == GameLanguage::Russian;
    return P::Text(id, russian);
}

// ---- The game's structures, as its own code builds them. ----
constexpr std::size_t TextSize = 0x40;     // text entry (MakeText): text pointer, string, flag
constexpr std::size_t ValueSize = 0x48;    // {u32 value; text}
constexpr std::size_t ListSize = 0x920;    // 32 values + count at +0x908 (+ alignment slack)
constexpr std::size_t PairListSize = 0xb0; // 2 values + count at +0x98 (+ alignment slack)
constexpr std::size_t ListCountOffset = 0x908, PairCountOffset = 0x98;
constexpr std::size_t FunctionSize = 0x28; // std::function: 32-byte buffer, __f_ at +0x20

/// Destroys a std::function the way the game's code does (its __f_ at +0x20).
void DestroyFunction(u8* f) {
    void* impl = *reinterpret_cast<void**>(f + 0x20);
    if (!impl) {
        return;
    }
    using Destroy = void(BB_SYSV*)(void*, int);
    Destroy destroy = (*reinterpret_cast<Destroy**>(impl))[4];
    destroy(impl, impl != f);
    *reinterpret_cast<void**>(f + 0x20) = nullptr;
}

/// Frees a text entry's string the way the game's code does (heap buffer once capacity >= 8).
void DestroyText(u8* text) {
    if (*reinterpret_cast<u64*>(text + 0x28) >= 8) {
        void* allocator = *reinterpret_cast<void**>(text + 0x30);
        using Free = void(BB_SYSV*)(void*, void*);
        (*reinterpret_cast<Free**>(allocator))[0x70 / 8](allocator, *reinterpret_cast<void**>(text + 0x10));
    }
    *reinterpret_cast<u64*>(text + 0x28) = 7;
    *reinterpret_cast<u64*>(text + 0x20) = 0;
    *reinterpret_cast<char16_t*>(text + 0x10) = 0;
}
void DestroyTexts(u8* pair) {
    DestroyText(pair);
    DestroyText(pair + TextSize);
}
/// A value list's entries (count at `count_offset`), after the row has copied them.
void DestroyValues(u8* list, std::size_t count_offset) {
    const u64 count = *reinterpret_cast<u64*>(list + count_offset);
    for (u64 i = 0; i < count; ++i) {
        DestroyText(list + i * ValueSize + 8);
    }
}

void MakeTexts(u8* pair, u32 name, u32 help) {
    using Make = void*(BB_SYSV*)(void*, u32, u32);
    Game<Make>(MakeText)(pair, TextName, name);
    Game<Make>(MakeText)(pair + TextSize, TextHelp, help);
}

bool Trace();
// ---- Live rows: values, value columns and the help line ----
// A left/right row's widget (a ComboBox, 0x1B2A100) copies its two value texts' pointers (our
// run-time buffers) when built, and draws them again only when its value changes: a value that
// changes while the page is open (a saved party address, the party status) stayed old until the
// page was opened again. The help line (the page's bottom line) is set from the row that has the
// cursor only when the cursor moves (0x1B1AB20), so a help text describing the row's value (the
// enemy scaling lines, "press again to confirm") also stayed old. And a widget shows the value it
// was built with: a setting changed elsewhere (the overlay menu) while the page is open did not
// show. So every row's widget is kept (a reference each), and from the game's own menu thread
// (the next text lookup) while the page still holds them:
//  - a run-time text changed: the left/right rows redraw their items (0x1ACFB20 on the widget's
//    list at +0xA0) and the help line is set again from the focused row (0x1B1AB20 on the page);
//  - a value was taken from the settings (Pages::ValueChanges): each widget is moved to its
//    bound value (left/right: the list's index, 0x1ACF6B0 + redraw; slider: its value at +0xBF8
//    and 0x1ADCC10; pop-up list: its kept value at +0x3D0 and 0x1B77890, the current item).
constexpr u64 PageRows = 0xe50;          ///< page + this: {?, begin, end, capacity} of 0x90-byte rows
constexpr std::size_t PageRowSize = 0x90, RowWidget = 0x80; // a row: name+help texts, widget at +0x80
constexpr u64 ListRedraw = 0x1acfb20;    ///< (widget + 0xA0): redraws the items from their texts
constexpr u64 ListSelect = 0x1acf6b0;    ///< (list, index): the list's current item (1: selectable)
constexpr u64 WidgetList = 0xa0;
constexpr u64 SetHelpLine = 0x1b1ab20;   ///< (page): the help line from the focused row's help text
constexpr u64 PairWidgetVtable = 0x5339d50;   ///< PairRow's widget (bound u8* at +0x360)
constexpr u64 SliderWidgetVtable = 0x5339c60; ///< SliderRow's widget (bound u8* at +0xCD0)
constexpr u64 SliderValue = 0xbf8;       ///< widget + 0xA0 (the slider) + this: its int value 0..10
constexpr u64 SliderShow = 0x1adcc10;    ///< (slider): number and bar from its value
constexpr u64 DropdownVtable = 0x533d790;  ///< ListRow's widget (bound int* at +0x3C8)
constexpr u64 DropdownKept = 0x3d0;      ///< its copy of the value (as built)
constexpr u64 DropdownShow = 0x1b77890;  ///< (widget): the current item's text from the bound value
struct Kept {
    u64 widget = 0;
    Kind kind = Kind::List;
    int field = 0;
    bool dynamic = false; ///< left/right row with a run-time value text
};
std::vector<Kept> live_widgets;          // menu thread builds / redraws, present thread checks (widgets_mu)
std::uint64_t live_seen_changes = 0, live_seen_values = 0;
// Buttons: the open page (the dialog its rows were built into), the field of each of its rows and
// a kept widget of it (alive while the page holds its other reference), the Cross presses counted
// when it was built (the press that opened it is not a press on a row).
std::recursive_mutex widgets_mu;
u64 open_dialog = 0, open_widget = 0;
std::vector<int> open_fields;
std::vector<u32> open_helps;    // each row's help text id (the help line)
std::u16string help_shown;      // the focused row's help text when the help line was last set
std::uint32_t open_presses = 0;
thread_local bool in_redraw = false;
/// The page's rows list (dialog + 0xA70, the cursor at +0x74, a pointer's override at +0x78:
/// droogie/bbhost engine/menu_pointer.cpp): which row has the cursor (-1: none known).
constexpr u64 DialogRowsList = 0xa70, ListCursor = 0x74, ListCursorOverride = 0x78;
int FocusedRow() {
    std::int32_t cursor = -1, cursor_override = -1;
    if (!open_dialog || !bbnet::guest_read(open_dialog + DialogRowsList + ListCursor, &cursor, 4) ||
        !bbnet::guest_read(open_dialog + DialogRowsList + ListCursorOverride, &cursor_override, 4))
        return -1;
    return cursor_override >= 0 ? cursor_override : cursor;
}

int RefCount(u64 obj) { return *reinterpret_cast<volatile int*>(obj + 8); }
void Release(u64 obj) {
    if (__atomic_sub_fetch(reinterpret_cast<int*>(obj + 8), 1, __ATOMIC_ACQ_REL) == 0) {
        using Destroy = void(BB_SYSV*)(u64);
        reinterpret_cast<Destroy>(**reinterpret_cast<u64**>(obj))(obj);
    }
}
bool LiveValuesOn() {
    static const bool on = [] {
        const char* env = std::getenv("BB_GAME_MENU_LIVE_VALUES");
        return !env || env[0] != '0';
    }();
    return on;
}
/// After a row was added: the page's newest row's widget, referenced.
void KeepNewestWidget(void* page, const Row& row, bool dynamic) {
    if (!LiveValuesOn()) return;
    const u64 rows = reinterpret_cast<u64>(page) + PageRows;
    u64 begin = 0, end = 0;
    if (!bbnet::guest_read(rows + 8, &begin, 8) || !bbnet::guest_read(rows + 0x10, &end, 8) || end < begin + PageRowSize ||
        (end - begin) % PageRowSize) {
        if (Trace()) std::printf("Game menu: live values: page rows %#llx..%#llx not as expected\n",
                                 (unsigned long long)begin, (unsigned long long)end);
        return;
    }
    u64 widget = 0, vtable = 0;
    if (!bbnet::guest_read(end - PageRowSize + RowWidget, &widget, 8) || !widget || !bbnet::guest_read(widget, &vtable, 8) ||
        vtable < image_base || RefCount(widget) < 1 || RefCount(widget) > 64) {
        if (Trace()) std::printf("Game menu: live values: no widget in the newest row (%#llx)\n", (unsigned long long)widget);
        return;
    }
    if (Trace()) std::printf("Game menu: live values: row widget %#llx (vtable +%#llx, refs %d)\n", (unsigned long long)widget,
                             (unsigned long long)(vtable - image_base), RefCount(widget));
    __atomic_add_fetch(reinterpret_cast<int*>(widget + 8), 1, __ATOMIC_ACQ_REL);
    live_widgets.push_back({widget, row.kind, row.field, dynamic});
}
/// A page is being built: the previous page's widgets are let go.
void DropWidgets() {
    for (const Kept& k : live_widgets) Release(k.widget);
    live_widgets.clear();
    live_seen_changes = P::DynamicChanges();
    live_seen_values = P::ValueChanges();
    open_dialog = open_widget = 0;
    open_fields.clear();
    open_helps.clear();
    help_shown.clear();
}
bool IsA(u64 widget, u64 vtable) {
    u64 v = 0;
    return bbnet::guest_read(widget, &v, 8) && v == image_base + vtable;
}
/// Moves a kept widget to the value its row binds (Pages::Byte / Value).
bool ShowValue(const Kept& k) {
    using Call = void(BB_SYSV*)(u64);
    if (k.kind == Kind::Toggle && IsA(k.widget, PairWidgetVtable)) {
        const int value = *P::Byte(k.field); // items 0, 1: index = value
        if (*reinterpret_cast<volatile int*>(k.widget + WidgetList + ListCursor) == value) return false;
        using Select = int(BB_SYSV*)(u64, u32);
        Game<Select>(ListSelect)(k.widget + WidgetList, u32(value));
        Game<Call>(ListRedraw)(k.widget + WidgetList);
        return true;
    }
    if ((k.kind == Kind::Slider || k.kind == Kind::Steps) && IsA(k.widget, SliderWidgetVtable)) {
        const u64 slider = k.widget + WidgetList;
        const int value = *P::Byte(k.field);
        if (*reinterpret_cast<volatile int*>(slider + SliderValue) == value) return false;
        *reinterpret_cast<volatile int*>(slider + SliderValue) = value;
        Game<Call>(SliderShow)(slider);
        return true;
    }
    if (k.kind == Kind::List && IsA(k.widget, DropdownVtable)) {
        *reinterpret_cast<volatile int*>(k.widget + DropdownKept) = *P::Value(k.field);
        Game<Call>(DropdownShow)(k.widget);
        return true;
    }
    return false;
}
/// The menu thread, between lookups: redraw what changed while the page is open.
void RedrawChangedValues() {
    if (in_redraw || live_widgets.empty()) return;
    std::lock_guard<std::recursive_mutex> lk(widgets_mu);
    const std::uint64_t changes = P::DynamicChanges(), values = P::ValueChanges();
    if (changes == live_seen_changes && values == live_seen_values) return;
    const bool texts = changes != live_seen_changes, moved = values != live_seen_values;
    live_seen_changes = changes;
    live_seen_values = values;
    in_redraw = true;
    std::size_t redrawn = 0, shown = 0;
    for (auto it = live_widgets.begin(); it != live_widgets.end();) {
        if (RefCount(it->widget) <= 1) {  // the page let it go: only our reference is left
            Release(it->widget);
            it = live_widgets.erase(it);
            continue;
        }
        if (moved && ShowValue(*it)) ++shown;
        if (texts && it->dynamic) {
            using Redraw = void(BB_SYSV*)(u64);
            Game<Redraw>(ListRedraw)(it->widget + WidgetList);
            ++redrawn;
        }
        ++it;
    }
    // The page still holds its rows and the focused row's help text is a run-time one that
    // changed: the help line again from it (not on every other change: a long help line that
    // scrolls would start over each time a ping changes).
    bool help = false;
    if (texts && open_dialog && !live_widgets.empty()) {
        const int row = FocusedRow();
        if (row >= 0 && row < int(open_helps.size()) && P::IsDynamic(open_helps[std::size_t(row)])) {
            const char16_t* text = TextOf(open_helps[std::size_t(row)]);
            if (text && help_shown != text) {
                help_shown = text;
                using Help = void(BB_SYSV*)(u64);
                Game<Help>(SetHelpLine)(open_dialog);
                help = true;
            }
        }
    }
    in_redraw = false;
    static int logs = 0;
    if (logs < 40 && Trace() && (shown || redrawn || help)) {
        ++logs;
        std::printf("Game menu: live rows: %zu value column(s) redrawn, %zu widget(s) moved to their value%s\n",
                    redrawn, shown, help ? ", help line set" : "");
    }
}

/// The present thread, each frame: a Cross press (counted by the pad sampling, so a short tap is
/// not missed) while one of our pages is open runs the button row that has its cursor (FocusedRow).
void PollButtons(std::int64_t now_ms) {
    static std::uint32_t seen = runtime_pad_cross_presses();
    const std::uint32_t presses = runtime_pad_cross_presses();
    if (presses == seen) return;
    seen = presses;
    if (party::flow::Editing()) return; // the text box has the keyboard (its Enter is not ours)
    int field = -1, index = -1;
    {
        std::lock_guard<std::recursive_mutex> lk(widgets_mu);
        // Not the press that opened the page; the page still holds its rows (our reference is not
        // the last one).
        if (!open_dialog || !open_widget || std::int32_t(presses - open_presses) <= 0 || RefCount(open_widget) <= 1)
            return;
        index = FocusedRow();
        if (index < 0 || index >= int(open_fields.size())) return;
        field = open_fields[std::size_t(index)];
    }
    if (!P::IsButton(field)) return;
    if (Trace()) std::printf("Game menu: button row %d pressed (field %d)\n", index, field);
    P::Press(field, now_ms);
}

void AddRows(void* page, const P::Page& from) {
    std::lock_guard<std::recursive_mutex> lk(widgets_mu);
    DropWidgets();
    // Rows shown now (the party's member rows: one per member).
    std::vector<Row> rows;
    for (const Row* row : P::ShownRows(from)) rows.push_back(*row);
    if (rows.size() > P::RowsPerPage) {
        // The layout's widget slots (a row past them has nowhere to draw); the pages keep within
        // them (tests/test_game_menu_rows.cpp), so this is a bug when it shows.
        std::printf("Game menu: %zu rows on a page of %zu, the last ones dropped\n", rows.size(), P::RowsPerPage);
        rows.resize(P::RowsPerPage);
    }
    open_dialog = reinterpret_cast<u64>(page);
    open_presses = runtime_pad_cross_presses();
    using Push = void(BB_SYSV*)(void*, void*);
    using AddRow = void(BB_SYSV*)(void*, void*, int*, void*, const int*);
    using Make = void*(BB_SYSV*)(void*, u32, u32);
    for (const Row& row : rows) {
        alignas(16) u8 pair[2 * TextSize] = {};
        MakeTexts(pair, row.name, row.help);
        if (row.kind == Kind::Slider || row.kind == Kind::Steps) {
            open_fields.push_back(row.field);
            open_helps.push_back(row.help);
            using Slider = void(BB_SYSV*)(void*, void*, u8*, const u8*);
            Game<Slider>(SliderRow)(page, pair, P::Byte(row.field), P::DefaultByte(row.field));
            KeepNewestWidget(page, row, false);
            DestroyTexts(pair);
            continue;
        }
        open_fields.push_back(row.field);
        open_helps.push_back(row.help);
        if (row.kind == Kind::Toggle || row.kind == Kind::Button) {
            using PairRowFn = void(BB_SYSV*)(void*, void*, u8*, void*, const u8*);
            u8* two = static_cast<u8*>(AlignedAlloc(PairListSize));
            std::memset(two, 0, PairListSize);
            const std::size_t count = row.kind == Kind::Button ? 1 : 2;
            for (std::size_t i = 0; i < count; ++i) {
                alignas(16) u8 value[ValueSize] = {};
                value[0] = u8(i);
                Game<Make>(MakeText)(value + 8, TextName, row.choices[i]);
                Game<Push>(PairPush)(two, value);
                DestroyText(value + 8);
            }
            Game<PairRowFn>(PairRow)(page, pair, P::Byte(row.field), two, P::DefaultByte(row.field));
            bool dynamic = false;
            for (std::size_t i = 0; i < count; ++i) dynamic = dynamic || P::IsDynamic(row.choices[i]);
            const std::size_t kept = live_widgets.size();
            KeepNewestWidget(page, row, dynamic);
            if (row.kind == Kind::Button && !open_widget && live_widgets.size() > kept) open_widget = live_widgets.back().widget;
            DestroyValues(two, PairCountOffset);
            AlignedFree(two);
            DestroyTexts(pair);
            continue;
        }
        u8* list = static_cast<u8*>(AlignedAlloc(ListSize));
        std::memset(list, 0, ListSize);
        for (std::size_t i = 0; i < row.choices.size(); ++i) {
            if (!P::ChoiceAvailable(row.field, int(i))) {
                continue; // the list's values are the upscaler numbers, not positions
            }
            alignas(16) u8 value[ValueSize] = {};
            *reinterpret_cast<u32*>(value) = u32(i);
            Game<Make>(MakeText)(value + 8, TextName, row.choices[i]);
            Game<Push>(ListPush)(list, value);
            DestroyText(value + 8);
        }
        Game<AddRow>(ListRow)(page, pair, P::Value(row.field), list, P::DefaultValue(row.field));
        KeepNewestWidget(page, row, false);
        DestroyValues(list, ListCountOffset);
        AlignedFree(list);
        DestroyTexts(pair);
    }
}

std::atomic<TextOverrideFn> text_override{nullptr};
std::atomic<ImePresetFn> ime_preset{nullptr};
std::atomic<ImeChoicesFn> ime_choices{nullptr};
std::atomic<ImeClosedFn> ime_closed{nullptr};
std::atomic<const char16_t*> title_override{nullptr}; // while the game builds one of our pages (its menu thread)
// Nightwatch lists (AddHook below): on, and which list the System builder runs for (its menu thread).
bool lists_ok = false;
thread_local P::MenuList building = P::MenuList::System;
constexpr u32 SystemTitle = 110020; // the System list's title ("System"), looked up while it is built
// Called by the game's menu code (guest threads, System V ABI like the game), one set per page.
bool Trace();
template <int N>
BB_SYSV void Rows(void* page, void*) {
    if (Trace()) {
        const char16_t* name = P::Text(P::All()[N].name, false);
        std::string ascii;
        for (const char16_t* c = name; c && *c; ++c) ascii.push_back(*c < 128 ? char(*c) : '?');
        std::printf("Game menu: page %d %s open\n", N, ascii.c_str());
    }
    P::Load();
    if (Trace()) {
        std::printf("Game menu: page %d rows (%zu): %s\n", N, P::ShownRows(P::All()[N]).size(),
                    P::ShownSummary(P::All()[N]).c_str());
    }
    AddRows(page, P::All()[N]);
}
template <int N>
BB_SYSV void* Content(void* a, void* b) {
    using Open = void*(BB_SYSV*)(void*, void*, const char*, void*, u64, u64);
    title_override = nullptr; // the title was looked up just before
    return Game<Open>(OpenPage)(a, b, Game<const char*>(ControlsLayout),
                                reinterpret_cast<void*>(&Rows<N>), 0, 0);
}
void* Factory(void* out, void* arg, void* content, const char16_t* title) {
    using Create = void*(BB_SYSV*)(void*, void*, void*);
    alignas(16) u8 function[FunctionSize] = {};
    *reinterpret_cast<u64*>(function) = image_base + ContentVtable;
    *reinterpret_cast<void**>(function + 8) = content;
    *reinterpret_cast<void**>(function + 0x20) = function;
    // The page is built later (the game's menu task): its layout title is looked up right before
    // the content function runs, which takes the override back.
    title_override = title;
    Game<Create>(CreatePage)(out, arg, function);
    DestroyFunction(function);
    return out;
}
template <int N>
BB_SYSV void* PageFactory(void* out, void* arg) {
    return Factory(out, arg, reinterpret_cast<void*>(&Content<N>), TextOf(P::All()[N].name));
}
// Our own cap (one factory per page; P::MaxPages), not the game's: the System list scrolls
// (Screen/Sound, our pages, Brightness, Network, Exit Game). 13 pages live-checked 2026-10-11
// (agent/balance), 17+ in 0.6.2.
using P::MaxPages;
template <std::size_t... I>
std::array<void*, sizeof...(I)> MakeFactories(std::index_sequence<I...>) {
    return {reinterpret_cast<void*>(&PageFactory<int(I)>)...};
}
const std::array<void*, MaxPages> factories = MakeFactories(std::make_index_sequence<MaxPages>{});

// ---- Hooks ----
using LookupFn = const char16_t*(BB_SYSV*)(void*, u32, u32, u32);
using AddFn = void*(BB_SYSV*)(void*, void*, void*, void*);
LookupFn lookup_original = nullptr;
// The repository / table of the game's own lookups (GameText looks other messages up with them).
std::atomic<void*> last_repository{nullptr};
std::atomic<u32> last_table{0};
AddFn add_original = nullptr;
AddFn command_original = nullptr;
std::atomic<const void*> screen_sound_text{nullptr};
std::atomic<const void*> new_game_text{nullptr}; // the title's "New Game" text, as looked up
std::atomic<const void*> discard_text{nullptr};  // the item menu's "Discard" text (give items, below)
// Nightwatch: the System list's row names (110000 Controls .. 110015 the PC menus' Quit to Desktop) as
// looked up: AddHook / CommandHook tell the game's rows apart by them (P::PlaceGameRow).
std::array<std::atomic<const void*>, P::QuitToDesktopRow - P::ControlsRow + 1> system_row_text{};
/// The System row id whose name `pair` holds, or 0 when it is none of them.
u32 SystemRowOf(const void* pair) {
    const void* name = *static_cast<const void* const*>(pair);
    for (std::size_t i = 0; i < system_row_text.size(); ++i)
        if (name && system_row_text[i].load() == name) return P::ControlsRow + u32(i);
    return 0;
}

/// BB_GAME_MENU_TRACE=1: menu events in the log for scripted tests (tools driving the menu with
/// BB_PAD_FILE wait for them): "Game menu: new text", "Game menu: System menu built",
/// "Game menu: page N <name> open".
bool Trace() {
    static const bool trace = [] {
        const char* env = std::getenv("BB_GAME_MENU_TRACE");
        return env && env[0] == '1';
    }();
    return trace;
}

// Screen changes (TextActivity): a message id looked up that was not looked up in the last second.
std::atomic<u64> fresh_seq{0};
std::atomic<std::int64_t> fresh_ms{0};
struct SeenText {
    std::atomic<u64> key{0}; // category << 32 | id, + 1 (0 = empty)
    std::atomic<std::int64_t> ms{0};
};
SeenText seen_texts[4096];
struct SeenId {
    std::atomic<u32> id{0};
    std::atomic<std::int64_t> ms{0};
};
SeenId seen_ids[4096]; // by id only (LastLookupMs)
// The last lookups with their texts (TextSeenMs: test aids wait for a given screen's text).
struct RecentText {
    std::atomic<const char16_t*> text{nullptr};
    std::atomic<std::int64_t> ms{0};
};
RecentText recent_texts[4096 + 3];
void NoteText(const char16_t* text) {
    if (!text || !text[0]) {
        return;
    }
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    // Repeats (a screen looks its texts up every frame) refresh their slot. Four slots per hash,
    // the oldest replaced: with one, texts looked up every frame evicted a dialog's text that was
    // looked up once (live: "Session lost" after Leave never matched).
    const u32 h = u32((reinterpret_cast<std::uintptr_t>(text) >> 3) * 0x9E3779B1u) >> 20; // 0..4095
    RecentText* slot = &recent_texts[h];
    for (u32 k = 0; k < 4; ++k) {
        RecentText& r = recent_texts[h + k];
        if (r.text.load(std::memory_order_relaxed) == text) {
            slot = &r;
            break;
        }
        if (r.ms.load(std::memory_order_relaxed) < slot->ms.load(std::memory_order_relaxed)) {
            slot = &r;
        }
    }
    slot->text.store(text, std::memory_order_relaxed);
    slot->ms.store(now, std::memory_order_release);
}
void NoteLookup(u32 category, u32 id) {
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    {
        SeenId& e = seen_ids[(u64(id) * 0x9E3779B97F4A7C15ull) >> 52];
        e.id.store(id, std::memory_order_relaxed);
        e.ms.store(now, std::memory_order_relaxed);
    }
    const u64 key = (u64(category) << 32 | id) + 1;
    SeenText& s = seen_texts[(key * 0x9E3779B97F4A7C15ull) >> 52];
    if (s.key.load(std::memory_order_relaxed) == key && now - s.ms.load(std::memory_order_relaxed) < 1000) {
        s.ms.store(now, std::memory_order_relaxed);
        return;
    }
    s.key.store(key, std::memory_order_relaxed);
    s.ms.store(now, std::memory_order_relaxed);
    fresh_ms.store(now, std::memory_order_relaxed);
    fresh_seq.fetch_add(1, std::memory_order_release);
    if (Trace()) {
        std::printf("Game menu: new text %#x/%u\n", category, id);
    }
}

BB_SYSV const char16_t* LookupHook(void* repository, u32 table, u32 category, u32 id) {
    if (id >= P::IdBase && id < P::IdBase + P::TextCount()) {
        // BB_GAME_MENU_TRACE=1: how often the game asks for our texts (per 2 s).
        static const bool count = [] {
            const char* env = std::getenv("BB_GAME_MENU_TRACE");
            return env && env[0] == '1';
        }();
        if (count) {
            static std::atomic<u64> lookups{0};
            static std::atomic<long long> printed{0};
            ++lookups;
            const long long now = std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
            if (now - printed.load() >= 2) {
                printed = now;
                std::printf("Game menu: %llu lookups of the port's texts in 2 s (last id %u)\n",
                            (unsigned long long)lookups.exchange(0), id - P::IdBase);
            }
        }
        const char16_t* ours = TextOf(id);
        RedrawChangedValues();  // the game's menu thread, our page on screen
        NoteText(ours); // the port's page texts too (TextSeenMs: scripted walks wait for its rows)
        return ours;
    }
    if (title_override && category == TextName && id == ControlsTitle) {
        return title_override;
    }
    if (building != P::MenuList::System && category == TextName && id == SystemTitle) {
        return TextOf(building == P::MenuList::GameSettings ? P::GameSettingsName() : P::PartySettingsName());
    }
    last_table.store(table, std::memory_order_relaxed);
    last_repository.store(repository, std::memory_order_release);
    const char16_t* text = lookup_original(repository, table, category, id);
    if (const TextOverrideFn fn = text_override.load(std::memory_order_acquire)) {
        if (const char16_t* other = fn(category, id, text)) text = other;
    }
    RedrawChangedValues();  // the game's menu code (our page's own texts are not looked up again)
    NoteLookup(category, id);
    NoteText(text);
    static const bool trace = [] {
        const char* env = std::getenv("BB_GAME_MENU_TRACE");
        return env && env[0] == '1';
    }();
    // BB_GAME_MENU_TRACE_TEXT=<ASCII text>: which message (category / id) and which caller show a
    // text containing it (reverse engineering a dialog; each id + caller once).
    static const char* trace_text = [] {
        const char* env = std::getenv("BB_GAME_MENU_TRACE_TEXT");
        return env && env[0] ? env : nullptr;
    }();
    if (trace_text && text) {
        const std::size_t n = std::strlen(trace_text);
        for (const char16_t* p = text; *p; ++p) {
            std::size_t k = 0;
            while (k < n && p[k] && std::tolower(static_cast<unsigned char>(p[k] < 128 ? p[k] : 0)) ==
                                        std::tolower(static_cast<unsigned char>(trace_text[k]))) {
                ++k;
            }
            if (k == n) {
                static std::mutex mu;
                static std::vector<std::pair<u64, u64>> seen;
                const u64 from = reinterpret_cast<u64>(__builtin_return_address(0)) - image_base;
                std::lock_guard<std::mutex> lk(mu);
                if (std::find(seen.begin(), seen.end(), std::make_pair(u64(id), from)) == seen.end() && seen.size() < 64) {
                    seen.emplace_back(u64(id), from);
                    // The callers above (the game keeps frame pointers: rbp chain).
                    char chain[160] = {0};
                    std::size_t used = 0;
                    const u64* fp = static_cast<const u64*>(__builtin_frame_address(0));
                    for (int depth = 0; depth < 6 && fp; ++depth) {
                        u64 next = 0, ret = 0;
                        if (!bbnet::guest_read(reinterpret_cast<std::uintptr_t>(fp), &next, 8) ||
                            !bbnet::guest_read(reinterpret_cast<std::uintptr_t>(fp) + 8, &ret, 8))
                            break;
                        if (depth > 0 && used < sizeof chain - 24)
                            used += std::snprintf(chain + used, sizeof chain - used, " %#llx",
                                                  (unsigned long long)(ret - image_base));
                        if (next <= reinterpret_cast<u64>(fp)) break;
                        fp = reinterpret_cast<const u64*>(next);
                    }
                    std::printf("Game menu: '%s' is text %#x/%u (%#x) from %#llx; callers%s\n", trace_text, category,
                                id, id, (unsigned long long)from, chain);
                }
                break;
            }
        }
    }
    if (trace && id >= 110000 && id < 120000) {
        std::printf("Game menu: text %#x/%u%s from %#llx\n", category, id, title_override ? " (our page)" : "",
                    (unsigned long long)(reinterpret_cast<u64>(__builtin_return_address(0)) - image_base));
    }
    if (trace && id >= 401000 && id < 402000) {
        std::printf("Game menu: title text %#x/%#x from %#llx\n", category, id,
                    (unsigned long long)(reinterpret_cast<u64>(__builtin_return_address(0)) - image_base));
    }
    if (category == TextName && id == TitleNewGame && text) {
        new_game_text = text;
    }
    if (category == TextName && id == ItemMenuDiscard && text) {
        discard_text = text;
    }
    if (category == TextName && id >= P::ControlsRow && id <= P::QuitToDesktopRow && text) {
        system_row_text[id - P::ControlsRow] = text;
    }
    if (category == TextName && id == ScreenSoundItem && text) {
        screen_sound_text = text;
        bool cyrillic = false;
        for (const char16_t* c = text; *c; ++c) {
            cyrillic = cyrillic || (*c >= 0x400 && *c < 0x500);
        }
        game_language = cyrillic ? GameLanguage::Russian : GameLanguage::English;
    }
    return text;
}

void AddItem(void* menu, u32 name, u32 help, void* factory) {
    using ToFunction = void*(BB_SYSV*)(void*, void*);
    alignas(16) u8 pair[2 * TextSize] = {};
    MakeTexts(pair, name, help);
    alignas(16) u8 source[FunctionSize] = {};
    *reinterpret_cast<u64*>(source) = image_base + FactoryVtable;
    *reinterpret_cast<void**>(source + 8) = factory;
    *reinterpret_cast<void**>(source + 0x20) = source;
    alignas(16) u8 function[FunctionSize] = {};
    *reinterpret_cast<void**>(function + 0x20) = Game<ToFunction>(FactoryToFunction)(source, function);
    alignas(16) u8 out[16] = {};
    add_original(menu, pair, function, out);
    DestroyFunction(function);
    DestroyTexts(pair);
}

// Nightwatch: Game Settings and Party Settings, lists of their own built with the game's System list
// (P::PlaceGameRow, P::SystemEntries). Off (the port's pages flat after Environment, the game's rows as
// they were) when the game's System list factory is not the expected code or CommandHook is not in.
// The builder's own show / drop checks (P::BuilderDrops): reads of the game's build flags, no side effects.
constexpr u64 FullGameCheck = 0x1ae9a90, OnlineCheck = 0x1ae9b40;
const u8 FullGameCheckCode[] = {0x83, 0x3d, 0x41, 0xf5, 0xa8, 0x03, 0x00, 0x0f, 0x9e, 0xc0, 0xc3};
const u8 OnlineCheckCode[] = {0x31, 0xc0, 0x83, 0x3d, 0x8f, 0xf4, 0xa8, 0x03, 0x00, 0x7f, 0x14, 0x8b, 0x0d, 0xe7, 0xee, 0xa8,
                              0x03, 0x85, 0xc9, 0x7f, 0x0a, 0x83, 0x3d, 0xac, 0xf1, 0xa8, 0x03, 0x00, 0x0f, 0x9e, 0xc0, 0xc3};
/// Will the builder drop this skipped row right after adding it? With the builder's flag 0: the Nightwatch
/// lists come from the System list factory, which passes 0; the rows System skips do not depend on it.
bool BuilderDrops(u32 row) {
    using Check = bool(BB_SYSV*)();
    return P::BuilderDrops(row, false, Game<Check>(FullGameCheck)(), Game<Check>(OnlineCheck)());
}

template <P::MenuList L>
BB_SYSV void* ListFactory(void* out, void* arg) {
    using Factory = void*(BB_SYSV*)(void*, void*);
    building = L;
    void* result = Game<Factory>(SystemListFactory)(out, arg);
    building = P::MenuList::System;
    if (Trace()) {
        std::printf("Game menu: %s list built\n", L == P::MenuList::GameSettings ? "Game Settings" : "Party Settings");
    }
    return result;
}

void AddPage(void* menu, int index) {
    const auto& pages = P::All();
    if (index >= 0 && std::size_t(index) < pages.size() && std::size_t(index) < MaxPages) {
        AddItem(menu, pages[std::size_t(index)].name, pages[std::size_t(index)].help, factories[std::size_t(index)]);
    }
}

/// System: Game Settings, Party Settings and the port's pages left in System (P::SystemEntries).
void AddSystemItems(void* menu) {
    AddItem(menu, P::GameSettingsName(), P::GameSettingsHelp(),
            reinterpret_cast<void*>(&ListFactory<P::MenuList::GameSettings>));
    for (const int entry : P::SystemEntries()) {
        if (entry == P::PartySettingsEntry) {
            AddItem(menu, P::PartySettingsName(), P::PartySettingsHelp(),
                    reinterpret_cast<void*>(&ListFactory<P::MenuList::PartySettings>));
        } else {
            AddPage(menu, entry);
        }
    }
}

BB_SYSV void* AddHook(void* menu, void* pair, void* function, void* out) {
    const void* screen_sound = screen_sound_text.load();
    const bool after_screen_sound = screen_sound && *reinterpret_cast<const void* const*>(pair) == screen_sound;
    if (!lists_ok) {
        void* result = add_original(menu, pair, function, out);
        if (after_screen_sound) {
            for (std::size_t i = 0; i < P::All().size(); ++i) AddPage(menu, int(i));
            if (Trace()) std::printf("Game menu: System menu built\n");
        }
        return result;
    }
    // Our items go in before the row at hand: a row the builder drops right after it must be the last item.
    const u32 row = SystemRowOf(pair);
    if (building == P::MenuList::PartySettings && after_screen_sound) {
        for (const int index : P::PartySettingsEntries()) AddPage(menu, index);
    }
    if (building == P::MenuList::System && row == P::ControlsRow) {
        AddSystemItems(menu);
        if (Trace()) std::printf("Game menu: System menu built\n");
    }
    const bool keep = P::PlaceGameRow(building, row ? row : P::QuitToDesktopRow + 1) == P::GameRow::Keep;
    if (!keep && !BuilderDrops(row)) {
        return menu; // skipped as AddPageItem returns: the menu
    }
    return add_original(menu, pair, function, out);
}

/// The Party page's index in P::All() (-1 when there is none).
int PartyPageIndex() {
    const auto& pages = P::All();
    for (std::size_t i = 0; i < pages.size() && i < MaxPages; ++i) {
        const char16_t* name = P::Text(pages[i].name, false);
        if (name && std::u16string(name) == u"Party") return int(i);
    }
    return -1;
}

// ---- Give items: the item menu's command (0.7.8) ----
constexpr std::size_t GiveObjectSize = 0x88;
using FinalizeFn = void*(BB_SYSV*)(void*, void*, void*);
FinalizeFn finalize_original = nullptr;
std::array<void*, 12> give_vtable{};  ///< a copy of the Discard command's vtable, three slots ours
thread_local u8* give_pending = nullptr;  ///< the Give functor made at Discard, added at finalize
thread_local u32 give_pending_item = 0;

bool InItemMenuBuilder(const void* return_address) {
    const u64 at = reinterpret_cast<u64>(return_address) - image_base;
    return at >= ItemMenuBuilder && at < ItemMenuBuilderEnd;
}
/// Copy-constructs a Give functor (`dst` null: a new one) from any command functor of the item menu (context at +8).
BB_SYSV void* GiveClone(void* self, void* dst) {
    using New = void*(BB_SYSV*)(u64);
    using Copy = void*(BB_SYSV*)(void*, const void*);
    if (!dst) {
        dst = Game<New>(GameNew)(GiveObjectSize);
        if (!dst) return nullptr;
    }
    std::memset(dst, 0, GiveObjectSize);
    *reinterpret_cast<void**>(dst) = give_vtable.data();
    Game<Copy>(CopyContext)(static_cast<u8*>(dst) + 8, static_cast<const u8*>(self) + 8);
    return dst;
}
// ---- The pickers: native command menus of our own (target member, amount) ----
/// Is `obj` a polymorphic object whose vtable slots 0 (clone) and 4 (delete) are readable code pointers? (Every native
/// call below is behind this guard: a failure leaves the menu without the Give entry or closes the picker.)
bool VtableOk(const void* obj) {
    u64 vt = 0, s0 = 0, s4 = 0;
    return obj && bbnet::guest_read(reinterpret_cast<std::uintptr_t>(obj), &vt, 8) && vt &&
           bbnet::guest_read(vt, &s0, 8) && s0 && bbnet::guest_read(vt + 0x20, &s4, 8) && s4;
}
constexpr u64 BuilderCtor = 0x1be8a30, BuilderDtor = 0x1b4cbb0, GameFree = 0x2bbe588;
constexpr std::size_t BuilderSize = 0x1400;  // the game's builder struct is 0x1190 bytes
struct PickState {
    void* finalizer = nullptr;  ///< a clone of the item menu's finalizer functor (owns the item context)
    u64 owner = 0;              ///< the builder's +0x1180 field (the item menu's owner)
    u32 item = 0;               ///< type bits | goods id (0.7.9: or a weapon / armor id)
    u32 index = 0;              ///< the item menu's inventory index (gear: tells two copies of one weapon apart)
    int slot = -1;              ///< the chosen member
} pick;
enum PickKind : u32 { PickMember = 1, PickAmount = 2 };
std::array<void*, 12> pick_vtable{};

BB_SYSV void* PickClone(void* self, void* dst) {
    using New = void*(BB_SYSV*)(u64);
    if (!dst) {
        dst = Game<New>(GameNew)(GiveObjectSize);
        if (!dst) return nullptr;
    }
    std::memcpy(dst, self, GiveObjectSize);  // no refcounted members: a plain copy
    return dst;
}
BB_SYSV void PickDtor(void*) {}
BB_SYSV void PickDelete(void* self, char dealloc) {
    using Free = void(BB_SYSV*)(void*);
    if (dealloc) Game<Free>(GameFree)(self);
}
void BuildPickVtable();
BB_SYSV void* PickInvoke(void* out, void* self, void* event);

/// A picker functor object (kind, a, b stored at +0x78 / +0x7c / +0x80); null when the game's allocator fails.
u8* MakePickObject(PickKind kind, u32 a, u32 b) {
    using New = void*(BB_SYSV*)(u64);
    BuildPickVtable();
    u8* o = static_cast<u8*>(Game<New>(GameNew)(GiveObjectSize));
    if (!o) return nullptr;
    std::memset(o, 0, GiveObjectSize);
    *reinterpret_cast<void**>(o) = pick_vtable.data();
    std::memcpy(o + 0x78, &kind, 4);
    std::memcpy(o + 0x7c, &a, 4);
    std::memcpy(o + 0x80, &b, 4);
    return o;
}

struct PickEntry {
    u32 text;
    PickKind kind;
    u32 a, b;
};
/// Builds a command menu of `entries` the way the item menu does (builder, AddCommandItem per entry, the item menu's
/// own finalizer) and returns it in `out` (a ref-counted menu pointer). False: nothing built (out untouched or null).
bool BuildCommandMenu(void* out, const std::vector<PickEntry>& entries) {
    using Ctor = void(BB_SYSV*)(void*, void*);
    using Dtor = void(BB_SYSV*)(void*);
    if (!out || !VtableOk(pick.finalizer) || !finalize_original || !command_original || entries.empty()) return false;
    alignas(16) u8 builder[BuilderSize] = {};
    alignas(16) u32 zero[4] = {};
    Game<Ctor>(BuilderCtor)(builder, zero);
    static const char16_t layout[] = u"CommandList";
    *reinterpret_cast<const char16_t**>(builder + 8) = layout;
    *reinterpret_cast<u64*>(builder + 0x1180) = pick.owner;
    builder[0x1188] = 0;
    for (const PickEntry& e : entries) {
        u8* object = MakePickObject(e.kind, e.a, e.b);
        if (!object) continue;
        alignas(16) u8 pair[2 * TextSize] = {};
        MakeTexts(pair, e.text, P::GiveCommandHelp());
        alignas(16) u8 function[FunctionSize] = {};
        *reinterpret_cast<void**>(function + 0x20) = object;
        command_original(builder, pair, function, nullptr);
        DestroyFunction(function);
        DestroyTexts(pair);
    }
    // the finalizer: a fresh clone of the item menu's (it holds the item context the menu view needs)
    void* proto = pick.finalizer;
    using Clone = void*(BB_SYSV*)(void*, void*);
    void* fin_object = (*reinterpret_cast<Clone**>(proto))[0](proto, nullptr);
    alignas(16) u8 fin[FunctionSize] = {};
    *reinterpret_cast<void**>(fin + 0x20) = fin_object;
    finalize_original(out, builder, fin);
    DestroyFunction(fin);
    Game<Dtor>(BuilderDtor)(builder);
    std::printf("Game menu: Give picker built (%zu entries)\n", entries.size());
    return true;
}

/// The Give command was chosen: the member picker.
void OpenMemberPicker(void* out, u32 item, u32 index) {
    const auto members = coop::give::MenuMembers();
    std::vector<PickEntry> entries;
    pick.item = item;
    pick.index = index;
    for (std::size_t i = 0; i < members.size() && i < 4; ++i) {
        P::SetPickerText(int(i), members[i].second);
        entries.push_back({P::PickerText(int(i)), PickMember, u32(members[i].first), 0});
    }
    if (!BuildCommandMenu(out, entries)) *static_cast<void**>(out) = nullptr;
}
void OpenAmountPicker(void* out, int slot) {
    const std::vector<int> amounts = coop::give::MenuAmounts(pick.item);
    std::vector<PickEntry> entries;
    pick.slot = slot;
    for (std::size_t i = 0; i < amounts.size() && i < 7; ++i) {
        const bool all = i + 1 == amounts.size() && amounts.size() > 1;
        P::SetPickerText(int(i), all ? "All (" + std::to_string(amounts[i]) + ")" : std::to_string(amounts[i]));
        entries.push_back({P::PickerText(int(i)), PickAmount, u32(amounts[i]), 0});
    }
    if (!BuildCommandMenu(out, entries)) *static_cast<void**>(out) = nullptr;
}
/// The game calls a command's operator() twice for one press (seen in the logs: same millisecond); the second call of
/// the same command within 0.5 s does nothing (and returns no menu), so a pick never opens two menus or gives twice.
bool RepeatedInvoke(u32 kind, u32 a) {
    static u64 last_key = 0;
    static std::chrono::steady_clock::time_point last_at{};
    const auto now = std::chrono::steady_clock::now();
    const u64 key = (u64(kind) << 32) | a;
    const bool again = key == last_key && now - last_at < std::chrono::milliseconds(500);
    last_key = key;
    if (!again) last_at = now;
    return again;
}
BB_SYSV void* PickInvoke(void* out, void* self, void* /*event*/) {
    if (!out) return out;
    *static_cast<void**>(out) = nullptr;
    u32 kind = 0, a = 0;
    std::memcpy(&kind, static_cast<const u8*>(self) + 0x78, 4);
    std::memcpy(&a, static_cast<const u8*>(self) + 0x7c, 4);
    if (RepeatedInvoke(kind, a)) return out;
    if (kind == PickMember && coop::give::MenuIsGear(pick.item)) {
        coop::give::MenuGiveGear(pick.item, pick.index, int(a));  // gear: one entry, no amount picker
    } else if (kind == PickMember) {
        OpenAmountPicker(out, int(a));
    } else if (kind == PickAmount && pick.slot >= 0) {
        coop::give::MenuGive(pick.item, int(a), pick.slot);
    }
    return out;
}
void BuildPickVtable() {
    if (pick_vtable[0]) return;
    const void* const* src = reinterpret_cast<const void* const*>(image_base + DiscardCommandVtable);
    for (std::size_t i = 0; i < pick_vtable.size(); ++i) pick_vtable[i] = const_cast<void*>(src[i]);
    pick_vtable[0] = reinterpret_cast<void*>(&PickClone);
    pick_vtable[1] = reinterpret_cast<void*>(&PickClone);
    pick_vtable[2] = reinterpret_cast<void*>(&PickInvoke);
    pick_vtable[4] = reinterpret_cast<void*>(&PickDelete);
    pick_vtable[5] = reinterpret_cast<void*>(&PickDtor);
}

/// operator(): the player chose Give. Returns the menu object to show in `out` (a ref-counted pointer): none yet.
BB_SYSV void* GiveInvoke(void* out, void* self, void* /*event*/) {
    const u8* o = static_cast<const u8*>(self);
    u32 item = 0, index = 0;
    std::memcpy(&item, o + 0x54, 4);
    std::memcpy(&index, o + 0x50, 4);
    std::printf("Game menu: GiveInvoke self %p\n", self);
    if (out) *static_cast<void**>(out) = nullptr;
    if (RepeatedInvoke(0, item)) return out;
    coop::give::MenuChosen(item, index);
    if (out) OpenMemberPicker(out, item, index);
    return out;
}
void BuildGiveVtable() {
    if (give_vtable[0]) return;
    const void* const* src = reinterpret_cast<const void* const*>(image_base + DiscardCommandVtable);
    for (std::size_t i = 0; i < give_vtable.size(); ++i) give_vtable[i] = const_cast<void*>(src[i]);
    give_vtable[0] = reinterpret_cast<void*>(&GiveClone);
    give_vtable[1] = reinterpret_cast<void*>(&GiveClone);
    give_vtable[2] = reinterpret_cast<void*>(&GiveInvoke);
}

/// The item command menu's Cross handler (view class 0x539be10, vtable slot 11, FUN_01ba2600) is text based: a row whose text is
/// "Discard" activates at once, any other row first asks FUN_016e4f00(item index) ("can this item be used now"), and a row that
/// fails it (every goods tab but the consumables: materials, key items) only makes the error sound, the row's functor is never
/// called. Our Give row is such a row. So when the highlighted row is ours the check is skipped and the row activated the way
/// the handler does it (FUN_01beaad0).
constexpr u64 ItemMenuViewVtable = 0x539be10, ItemMenuCrossSlot = 11;
constexpr u64 CurrentRowCopy = 0x1bea2e0, ActivateRow = 0x1beaad0;
constexpr std::size_t ItemViewIndexField = 0x2068;
using CrossFn = void(BB_SYSV*)(void*);
CrossFn cross_original = nullptr;

BB_SYSV void CrossHook(void* view) {
    using Copy = void(BB_SYSV*)(void*, void*);
    using Act = void(BB_SYSV*)(void*);
    bool give = false;
    void* dbg_f = nullptr;
    void* dbg_vt = nullptr;
    if (view && give_vtable[0]) {
        alignas(16) u8 row[0x300] = {};
        Game<Copy>(CurrentRowCopy)(row, view);
        void* f = *reinterpret_cast<void**>(row + 0xb0);
        dbg_f = f;
        if (f && VtableOk(f)) dbg_vt = *reinterpret_cast<void**>(f);
        // our own rows: the Give command and the picker entries (they too are rows the handler would refuse on a tab
        // whose items cannot be used)
        give = f && VtableOk(f) &&
               (*reinterpret_cast<void* const*>(f) == static_cast<void*>(give_vtable.data()) ||
                *reinterpret_cast<void* const*>(f) == static_cast<void*>(pick_vtable.data()));
        (*reinterpret_cast<void(BB_SYSV***)(void*)>(row))[1](row);  // the row copy's in-place destructor
    }
    if (std::getenv("BB_GIVE_VTRACE")) {
        int ix = -9;
        std::memcpy(&ix, static_cast<const u8*>(view) + ItemViewIndexField, 4);
        std::printf("Game menu: Cross: view %p give %d index %d f %p vt %p givevt %p\n", view, give ? 1 : 0, ix, dbg_f, dbg_vt,
                    static_cast<void*>(give_vtable.data()));
    }
    int index = -1;
    if (give) std::memcpy(&index, static_cast<const u8*>(view) + ItemViewIndexField, 4);
    if (give && index >= 0) {
        std::printf("Game menu: Cross on the Give row (item index %d)\n", index);
        Game<Act>(ActivateRow)(view);
        return;
    }
    cross_original(view);
}
bool InstallCrossHook() {
    void** slot = reinterpret_cast<void**>(image_base + ItemMenuViewVtable) + ItemMenuCrossSlot;
#ifdef _WIN32
    DWORD old = 0;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return false;
#endif
    cross_original = reinterpret_cast<CrossFn>(*slot);
    *slot = reinterpret_cast<void*>(&CrossHook);
    return cross_original != nullptr;
}

/// The item menu's finalize: the Give entry goes in just before it (after Discard's own set-up).
BB_SYSV void* FinalizeHook(void* out, void* list, void* fin) {
    u8* object = give_pending;
    if (object && InItemMenuBuilder(__builtin_return_address(0))) {
        // The picker menus reuse this menu's finalizer (a clone of it) and owner.
        void* fobj = nullptr;
        bbnet::guest_read(reinterpret_cast<std::uintptr_t>(fin) + 0x20, &fobj, 8);
        if (VtableOk(fobj)) {
            using Clone = void*(BB_SYSV*)(void*, void*);
            using Del = void(BB_SYSV*)(void*, char);
            if (pick.finalizer) (*reinterpret_cast<Del**>(pick.finalizer))[4](pick.finalizer, 1);
            pick.finalizer = (*reinterpret_cast<Clone**>(fobj))[0](fobj, nullptr);
            pick.owner = *reinterpret_cast<const u64*>(static_cast<const u8*>(list) + 0x1180);
        }
        give_pending = nullptr;
        alignas(16) u8 pair[2 * TextSize] = {};
        MakeTexts(pair, P::GiveCommandText(), P::GiveCommandHelp());
        alignas(16) u8 function[FunctionSize] = {};
        *reinterpret_cast<void**>(function + 0x20) = object;
        command_original(list, pair, function, nullptr);
        DestroyFunction(function);
        DestroyTexts(pair);
        std::printf("Game menu: Give added to the item menu (item %#x)\n", give_pending_item);
    }
    return finalize_original(out, list, fin);
}

// ---- Diagnostic: BB_GIVE_VTRACE=1 logs every virtual call of the item menu's command / predicate / view classes ----
template <int C, int N>
struct VTrace {
    static inline void* orig = nullptr;
    static BB_SYSV void* Thunk(void* a, void* b, void* c, void* d, void* e, void* f) {
        std::printf("VTRACE class %d slot %d self %p\n", C, N, a);
        return reinterpret_cast<void*(BB_SYSV*)(void*, void*, void*, void*, void*, void*)>(orig)(a, b, c, d, e, f);
    }
};
template <int C, int... Ns>
void VTraceInstall(void** vt, std::integer_sequence<int, Ns...>) {
    ((VTrace<C, Ns>::orig = vt[Ns], vt[Ns] = reinterpret_cast<void*>(&VTrace<C, Ns>::Thunk)), ...);
}
template <int C, int N>
void VTraceClass(u64 va) {
    void** vt = reinterpret_cast<void**>(image_base + va);
#ifdef _WIN32
    DWORD old = 0;
    VirtualProtect(vt, N * 8, PAGE_READWRITE, &old);
#endif
    VTraceInstall<C>(vt, std::make_integer_sequence<int, N>{});
    std::printf("Game menu: vtrace class %d = %#llx (%d slots)\n", C, (unsigned long long)va, N);
}
void VTraceAll() {
    VTraceClass<0, 8>(0x533fea0);   // Discard functor
    VTraceClass<1, 8>(0x5340030);   // Use functor
    VTraceClass<2, 8>(0x533ffe0);   // Use predicate
    VTraceClass<3, 8>(0x533fe50);   // Discard predicate
    VTraceClass<4, 14>(0x539be10);  // the view
    VTraceClass<5, 8>(0x533fe00);   // finalizer
}

/// The title's main menu: after its New Game item, the Party page as a title item.
BB_SYSV void* CommandHook(void* list, void* pair, void* functor, void* x) {
    // Nightwatch lists (AddHook): Game / Party Settings have no exit rows (the game's Exit Game, the PC menus'
    // Quit to Desktop), skipped as AddCommandItem returns (the list); Exit Game, which the builder drops
    // there itself (its flag 0), goes in for it to drop (P::BuilderDrops).
    if (lists_ok && building != P::MenuList::System) {
        const u32 row = SystemRowOf(pair);
        if (!(row && BuilderDrops(row))) return list;
    }
    if (const void* discard = discard_text.load();
        discard && *reinterpret_cast<const void* const*>(pair) == discard && functor &&
        InItemMenuBuilder(__builtin_return_address(0))) {
        // The item menu's Discard entry is about to go in: make a Give functor for this item from Discard's.
        void* object = *reinterpret_cast<void* const*>(static_cast<const u8*>(functor) + 0x20);
        u32 item = 0, index = 0;
        if (object && bbnet::guest_read(reinterpret_cast<std::uintptr_t>(object) + 0x54, &item, 4) &&
            bbnet::guest_read(reinterpret_cast<std::uintptr_t>(object) + 0x50, &index, 4) &&
            (coop::give::MenuOffers(item) || coop::give::MenuOffersGear(item, index))) {
            BuildGiveVtable();
            give_pending = static_cast<u8*>(GiveClone(object, nullptr));
            give_pending_item = item;
        }
    }
    void* result = command_original(list, pair, functor, x);
    const void* new_game = new_game_text.load();
    if (new_game && *reinterpret_cast<const void* const*>(pair) == new_game) {
        static const int party = PartyPageIndex();
        if (party >= 0) {
            const auto& page = P::All()[std::size_t(party)];
            AddItem(list, page.name, page.help, factories[std::size_t(party)]);
            // A party process: also Party mode (host / join / single player), the page after it.
            const std::size_t mode = std::size_t(party) + 1;
            if (party::flow::CurrentContext().party_process && mode < P::All().size() && mode < MaxPages)
                AddItem(list, P::All()[mode].name, P::All()[mode].help, factories[mode]);
        }
    }
    return result;
}

/// `jmp hook` at the function (via a near thunk); its moved prologue + `jmp` back is the original.
bool Detour(unsigned char* image, u64 va, const u8* prologue, std::size_t length, void* hook,
            void** original, u8*& cursor) {
    u8* site = image + va;
    if (std::memcmp(site, prologue, length) != 0) {
        std::printf("Game menu: unexpected code at %#llx, not hooked\n", (unsigned long long)va);
        return false;
    }
    u8* trampoline = cursor;
    std::memcpy(trampoline, prologue, length);
    trampoline[length] = 0xe9;
    const std::int64_t back = std::int64_t(site + length) - std::int64_t(trampoline + length + 5);
    const std::int32_t back32 = std::int32_t(back);
    std::memcpy(trampoline + length + 1, &back32, 4);
    u8* thunk = trampoline + 32;
    thunk[0] = 0x48;
    thunk[1] = 0xb8; // movabs rax, hook
    std::memcpy(thunk + 2, &hook, 8);
    thunk[10] = 0xff;
    thunk[11] = 0xe0; // jmp rax
    const std::int64_t to = std::int64_t(thunk) - std::int64_t(site + 5);
    if (back != back32 || to != std::int32_t(to)) {
        std::printf("Game menu: stubs out of jump range, not hooked\n");
        return false;
    }
    const std::int32_t to32 = std::int32_t(to);
    site[0] = 0xe9;
    std::memcpy(site + 1, &to32, 4);
    for (std::size_t i = 5; i < length; ++i) {
        site[i] = 0xcc;
    }
    *original = trampoline;
    cursor += 64;
    return true;
}

/// A mapping within +-2 GiB of the image, for rel32 jumps both ways.
u8* MapNear(unsigned char* image, u64 image_size, std::size_t size) {
    const u64 base = reinterpret_cast<u64>(image);
#ifdef _WIN32
    // The image is the first block of the runtime's low guest area (0x800000000 up, all of it
    // reserved for the guest): the next low block is right after it, within rel32 reach.
    if (void* p = runtime_low_map(size, 3 /* read | write; VirtualProtect later */)) {
        const u64 at = reinterpret_cast<u64>(p);
        if (at + size <= base + (2ull << 30) - (1ull << 20) && at >= base) {
            return static_cast<u8*>(p);
        }
    }
    // Otherwise walk the actual free regions instead of guessing addresses: the guest address space
    // reservation and the loader leave few fixed hints free. Every byte of the stubs must be
    // within rel32 reach of every byte of the image.
    {
        constexpr u64 reach = (2ull << 30) - (1ull << 20);
        constexpr u64 granularity = 0x10000;
        const u64 lo = base + image_size > reach ? base + image_size - reach : granularity;
        const u64 hi = base + reach - size;
        MEMORY_BASIC_INFORMATION info;
        for (u64 at = lo; at < hi && VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info));) {
            const u64 region = reinterpret_cast<u64>(info.BaseAddress);
            const u64 region_end = region + info.RegionSize;
            if (info.State == MEM_FREE) {
                const u64 start = (std::max(region, at) + granularity - 1) & ~(granularity - 1);
                if (start + size <= region_end && start < hi) {
                    if (void* p = VirtualAlloc(reinterpret_cast<void*>(start), size,
                                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                        return static_cast<u8*>(p);
                    }
                }
            }
            if (region_end <= at) {
                break;
            }
            at = region_end;
        }
    }
#endif
    for (u64 k = 1; k <= 64; ++k) {
        for (const u64 hint : {base - k * (24ull << 20), base + image_size + k * (24ull << 20)}) {
#ifdef _WIN32
            // Reservations start on the 64 KiB allocation granularity.
            void* p = VirtualAlloc(reinterpret_cast<void*>(hint & ~u64(0xffff)), size,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (p) {
                return static_cast<u8*>(p);
            }
#else
            void* p = mmap(reinterpret_cast<void*>(hint), size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED) {
                return static_cast<u8*>(p);
            }
#endif
        }
    }
    return nullptr;
}

} // namespace

void PatchImage(unsigned char* image, std::uint64_t size) {
    if (const char* env = std::getenv("BB_GAME_MENU"); env && env[0] == '0') {
        return;
    }
    image_base = reinterpret_cast<u64>(image);
    const std::size_t page = PageSize();
    u8* stubs = MapNear(image, size, page);
    if (!stubs) {
        std::printf("Game menu: no memory near the image (%p, %llu MiB), the port's pages are off\n",
                    static_cast<void*>(image), static_cast<unsigned long long>(size >> 20));
        return;
    }
    party::flow::RegisterMenuRows(); // the party flow's rows on the Party page (party_flow_rows.cpp)
    P::Define();
    P::SetRestartHandler(&runtime_restart);
    P::SetQuitHandler(&bbgpu_request_quit);
    if (P::All().size() > MaxPages) {
        std::printf("Game menu: %zu pages, only %zu shown\n", P::All().size(), MaxPages);
    }
    u8* cursor = stubs;
    // The lookup first: the menu hook only acts once it has seen the Screen/Sound text.
    const bool ok =
        Detour(image, MsgLookup, MsgLookupPrologue, sizeof(MsgLookupPrologue),
               reinterpret_cast<void*>(&LookupHook), reinterpret_cast<void**>(&lookup_original), cursor) &&
        Detour(image, AddPageItem, AddPageItemPrologue, sizeof(AddPageItemPrologue),
               reinterpret_cast<void*>(&AddHook), reinterpret_cast<void**>(&add_original), cursor);
    // The title entry (party flow): needs the Party page and AddPageItem's original.
    const char* title_entry = std::getenv("BB_PARTY_TITLE_ENTRY");
    if (ok && (!title_entry || title_entry[0] != '0') && PartyPageIndex() >= 0 &&
        Detour(image, AddCommandItem, AddCommandItemPrologue, sizeof(AddCommandItemPrologue),
               reinterpret_cast<void*>(&CommandHook), reinterpret_cast<void**>(&command_original), cursor)) {
        std::printf("Game menu: the Party page is also on the title screen (after New Game)\n");
    }
    // Game / Party Settings need the System list factory as expected and CommandHook (the exit rows).
    lists_ok = ok && command_original &&
               std::memcmp(image + SystemListFactory, SystemListFactoryPrologue, sizeof(SystemListFactoryPrologue)) == 0 &&
               std::memcmp(image + FullGameCheck, FullGameCheckCode, sizeof(FullGameCheckCode)) == 0 &&
               std::memcmp(image + OnlineCheck, OnlineCheckCode, sizeof(OnlineCheckCode)) == 0;
    if (ok) {
        std::printf("Game menu: Game Settings and Party Settings %s (%zu party pages)\n",
                    lists_ok ? "in System" : "off (pages flat in System): no command hook or unexpected code",
                    P::PartySettingsEntries().size());
    }
    if (ok && command_original && !std::getenv("BB_PARTY_GIVE_MENU_OFF") &&
        Detour(image, ItemMenuFinalize, FinalizePrologue, sizeof(FinalizePrologue),
               reinterpret_cast<void*>(&FinalizeHook), reinterpret_cast<void**>(&finalize_original), cursor)) {
        std::printf("Game menu: the item menu can offer Give\n");
        if (!InstallCrossHook()) std::printf("Game menu: the item menu's Cross handler is not hooked\n");
    }
#ifdef _WIN32
    DWORD old_protect = 0;
    VirtualProtect(stubs, page, PAGE_EXECUTE_READ, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), stubs, page);
#else
    mprotect(stubs, page, PROT_READ | PROT_EXEC);
#endif
    if (const char* vt = std::getenv("BB_GIVE_VTRACE"); vt && vt[0] == '1') VTraceAll();
    std::printf("Game menu: %s (%zu pages in System)\n", ok ? "the port's pages are in" : "hooks failed, off",
                P::All().size());
}


std::int64_t TextSeenMs(const char* needle) {
    std::int64_t best = 0;
    const std::size_t n = std::strlen(needle);
    if (!n) {
        return 0;
    }
    for (RecentText& r : recent_texts) {
        const std::int64_t ms = r.ms.load(std::memory_order_acquire);
        const char16_t* t = r.text.load(std::memory_order_relaxed);
        if (!t || ms <= best) {
            continue;
        }
        // ASCII, case-insensitive substring.
        for (const char16_t* p = t; *p; ++p) {
            std::size_t k = 0;
            while (k < n && p[k] && std::tolower(static_cast<unsigned char>(p[k] < 128 ? p[k] : 0)) ==
                                        std::tolower(static_cast<unsigned char>(needle[k]))) {
                ++k;
            }
            if (k == n) {
                best = ms;
                break;
            }
        }
    }
    return best;
}

void RecentTexts(std::int64_t since_ms, void (*fn)(const char16_t* text, std::int64_t ms, void* user), void* user) {
    for (RecentText& r : recent_texts) {
        const std::int64_t ms = r.ms.load(std::memory_order_acquire);
        const char16_t* t = r.text.load(std::memory_order_relaxed);
        if (t && ms > since_ms) {
            fn(t, ms, user);
        }
    }
}

void SetTextOverride(TextOverrideFn fn) {
    text_override.store(fn, std::memory_order_release);
}

void SetImePreset(ImePresetFn fn) {
    ime_preset.store(fn, std::memory_order_release);
}

void SetImeChoices(ImeChoicesFn fn) {
    ime_choices.store(fn, std::memory_order_release);
}

const char* ImeChoices(std::uint32_t max_length) {
    const ImeChoicesFn fn = ime_choices.load(std::memory_order_acquire);
    return fn ? fn(max_length) : nullptr;
}

void SetImeClosed(ImeClosedFn fn) {
    ime_closed.store(fn, std::memory_order_release);
}

void ImeClosed(std::uint32_t max_length, const char* text, bool cancelled) {
    const ImeClosedFn fn = ime_closed.load(std::memory_order_acquire);
    if (fn) fn(max_length, text, cancelled);
}

const char* ImePreset(std::uint32_t max_length) {
    const ImePresetFn fn = ime_preset.load(std::memory_order_acquire);
    return fn ? fn(max_length) : nullptr;
}

const char16_t* GameText(std::uint32_t category, std::uint32_t id) {
    void* repo = last_repository.load(std::memory_order_acquire);
    if (!repo || !lookup_original) {
        return nullptr;
    }
    const char16_t* text = lookup_original(repo, last_table.load(std::memory_order_relaxed), category, id);
    return text && text[0] ? text : nullptr;
}

std::int64_t LastLookupMs(std::uint32_t id) {
    const SeenId& e = seen_ids[(u64(id) * 0x9E3779B97F4A7C15ull) >> 52];
    return e.id.load(std::memory_order_relaxed) == id ? e.ms.load(std::memory_order_relaxed) : 0;
}

int RecentLookups(std::int64_t within_ms, std::uint32_t* out, int max) {
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    int n = 0;
    for (const SeenId& e : seen_ids) {
        const std::int64_t ms = e.ms.load(std::memory_order_relaxed);
        if (ms && now - ms < within_ms && n < max) {
            out[n++] = e.id.load(std::memory_order_relaxed);
        }
    }
    return n;
}

void TextActivity(std::uint64_t* seq, std::int64_t* last_ms) {
    *seq = fresh_seq.load(std::memory_order_acquire);
    *last_ms = fresh_ms.load(std::memory_order_relaxed);
}

void Poll() {
    party::flow::Tick(); // the party flow: text box, party character note (party_flow.h)
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    P::Poll(now);
    PollButtons(now);
}

} // namespace BbGameMenu
