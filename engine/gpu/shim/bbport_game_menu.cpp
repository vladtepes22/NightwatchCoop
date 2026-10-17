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
// The title screen's main menu (builder 0x1b4a3d0, docs/party/ingame_flow.md §3): its command items
// (Continue 0x61f95, Load Game 0x61f96, New Game 0x61f97, ...) go through AddCommandItem; the
// party flow adds the Party page as a title item right after New Game (BB_PARTY_TITLE_ENTRY=0: not).
constexpr u64 AddCommandItem = 0x1b4c9a0; ///< (command list, name+help texts, functor, 0) -> the list
constexpr u32 TitleNewGame = 0x61f97;
const u8 AddCommandItemPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};

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
std::atomic<const char16_t*> title_override{nullptr}; // while the game builds one of our pages (its menu thread)
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

BB_SYSV void* AddHook(void* menu, void* pair, void* function, void* out) {
    void* result = add_original(menu, pair, function, out);
    const void* screen_sound = screen_sound_text.load();
    if (screen_sound && *reinterpret_cast<const void* const*>(pair) == screen_sound) {
        const auto& pages = P::All();
        for (std::size_t i = 0; i < pages.size() && i < MaxPages; ++i) {
            AddItem(menu, pages[i].name, pages[i].help, factories[i]);
        }
        if (Trace()) {
            std::printf("Game menu: System menu built\n");
        }
    }
    return result;
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

/// The title's main menu: after its New Game item, the Party page as a title item.
BB_SYSV void* CommandHook(void* list, void* pair, void* functor, void* x) {
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
#ifdef _WIN32
    DWORD old_protect = 0;
    VirtualProtect(stubs, page, PAGE_EXECUTE_READ, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), stubs, page);
#else
    mprotect(stubs, page, PROT_READ | PROT_EXEC);
#endif
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
