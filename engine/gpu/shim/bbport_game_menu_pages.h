// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: what the port's pages in the game's System menu hold (bbport_game_menu.cpp builds them
// with the game's own rows): the texts in English and Russian, the pages and their rows, the
// values the rows edit and how a changed value is applied (bbport_settings, party_status
// commands). No game code here: unit-tested by tests/test_game_menu_pages.cpp.
//
// The game's limits, kept by every page (the test checks them):
//  - at most RowsPerPage rows (the "ControllSetting" layout the pages use has six widget slots;
//    a row past them has nowhere to draw): every page's visible rows stay within it in every
//    state (tests/test_game_menu_rows.cpp walks the party states);
//  - a pop-up list only as the last row (it opens downwards over the rows below it);
//  - short row names (the name column is narrow), one-line help texts.
// Rows: Toggle (left/right between two values), Slider (0..10), Steps (a slider standing for a
// few ordered choices), List (pop-up), Button (the game's choice row with a single value: no
// arrows, left/right do nothing; the glue runs it on Cross while the row has the cursor, see
// Press). Read-only lines are buttons without an action. A confirmed button (Leave, Kick,
// Restart, Quit ...) arms on the first press (its help line says "press again to confirm") and
// runs on a second press within 10 s.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "party/party_status.h"

namespace BbGameMenu::Pages {

using u8 = std::uint8_t;
using u32 = std::uint32_t;

constexpr u32 IdBase = 0x7f0000;     ///< the port's text ids (the game's own are far below)
constexpr std::size_t RowsPerPage = 6;
/// The port's pages in the System menu at most (bbport_game_menu.cpp: one compiled page factory
/// each; pages past it are dropped with a log line). Our own cap, not the game's: the System list
/// scrolls. 0.6.2: 16 -> 32 (the list had reached 16 and a new page would have been dropped); 0.7.7: 40.
constexpr std::size_t MaxPages = 48;
/// Text limits in UTF-16 units (the game draws longer texts over the next column or clips them).
constexpr std::size_t MaxNameLength = 24, MaxValueLength = 26, MaxHelpLength = 110;
constexpr std::size_t DynamicCapacity = 160; ///< a run-time text's buffer (UTF-16 units)

enum class Kind { List, Toggle, Slider, Steps, Button };

struct Row {
    int field = 0;
    u32 name = 0, help = 0;
    std::vector<u32> choices; ///< value texts, value = index (a toggle: two, a button: one)
    Kind kind = Kind::List;
    std::function<bool()> visible; ///< null: always; checked when the page opens
};
struct Page {
    u32 name = 0, help = 0;
    std::vector<Row> rows;
};

/// Builds the texts, fields and pages (once; later calls do nothing).
void Define();
/// Split travel's question text id (0: "Travel with the party?" YES / NO), for a game dialog.
u32 SplitTravelText(int which);
/// D10: the port text of a relayed story notice (the game's EventText id), 0 when not one.
u32 StoryNoticeText(u32 game_message);
/// The ending question a guest gets when the host reaches an ending.
u32 EndingQuestionText();
/// I5: the question a guest gets when the host gathers the party (party_gather.cpp).
u32 GatherQuestionText();
/// 0.7.8: the item menu's Give command (party_give.h): its name and help line.
u32 GiveCommandText();
u32 GiveCommandHelp();
/// 0.7.8: run-time labels of the Give pickers (members / amounts), slots 0..7: the text id, and its text (UTF-8, both languages).
u32 PickerText(int slot);
void SetPickerText(int slot, const std::string& utf8);
/// CL-a3a9e6: the question when a guest gathers the party ("Ann gathers the party. ..."); `SetGuestGatherQuestion` fills
/// in the name first.
u32 GuestGatherQuestionText();
void SetGuestGatherQuestion(const std::string& who);
/// 0.7.3: the travel vote's question (party_vote.cpp), a run-time text: SetTravelVoteQuestion
/// writes who wants the party to travel where (UTF-8 name, the lamp's game text) before it opens.
u32 TravelVoteQuestionText();
void SetTravelVoteQuestion(const std::string& who, const std::u16string& lamp);
const std::vector<Page>& All();
int FieldCount();

/// Nightwatch: the System menu groups the pages whose English name has "party" in it (any case) in one
/// "Party Settings" list (bbport_game_menu.cpp builds it with the game's own System list). An entry of
/// SystemEntries() is a page index into All(), or PartySettingsEntry for the Party Settings item, which
/// takes the place of the first party page.
constexpr int PartySettingsEntry = -1;
bool IsPartyPage(const Page& page);
std::vector<int> SystemEntries();
std::vector<int> PartySettingsEntries(); ///< the party pages' indices, in All() order
u32 PartySettingsName();
u32 PartySettingsHelp();

/// Nightwatch: the lists built with the game's System list builder, and where a row the game (110000
/// Controls ... 110005 Language) or the PC menus (110006..110014 their pages, 110015 Quit to Desktop) add
/// goes, by its name's text id. System: Game Settings, Party Settings and the pages SystemEntries() keeps
/// go in at Controls, before every game row. Game Settings: Controls, Environment, Brightness, then the PC
/// pages. Party Settings: none of them (its pages go in at Environment).
enum class MenuList { System, PartySettings, GameSettings };
enum class GameRow { Keep, Skip };
constexpr u32 ControlsRow = 110000, EnvironmentRow = 110001, BrightnessRow = 110002, NetworkRow = 110003,
              ExitGameRow = 110004, LanguageRow = 110005, FirstPcRow = 110006, LastPcRow = 110014,
              QuitToDesktopRow = 110015;
GameRow PlaceGameRow(MenuList list, u32 id);
/// The System builder (0x1bb3ad0) drops a row it does not show right after adding it, as "the list's
/// last item": a skipped row (nothing added) would cost the item before it. A skipped row the builder
/// will drop goes in anyway, so the builder drops it, not ours. Its rules (1.09): Environment and
/// Brightness when `full` (0x1ae9a90) is false, Network when `online` (0x1ae9b40) is false, Exit Game
/// when the builder's flag (its third argument: 1 in the game's in-game System, 0 from the factory the
/// Nightwatch lists use) is 0, Language when the flag is set or `online` is false. The PC menus' rows
/// come after all of them (at the list's finish): never dropped.
bool BuilderDrops(u32 id, bool flag, bool full, bool online);
u32 GameSettingsName();
u32 GameSettingsHelp();

/// The port's text `id` in English or Russian, or null when it is not one of ours.
const char16_t* Text(u32 id, bool russian);
std::size_t TextCount();
/// For the test: whether `id` is made at run time.
bool IsDynamic(u32 id);
/// Counts the run-time text changes (a value column or the help line to redraw while its page
/// is open).
std::uint64_t DynamicChanges();
/// Counts the times Poll took row values from the settings (changed elsewhere while a page may be
/// open): the glue moves the open page's widgets to Byte / Value.
std::uint64_t ValueChanges();

/// What the game's rows bind (stable addresses for the run).
int* Value(int field);
u8* Byte(int field);
const int* DefaultValue(int field);
const u8* DefaultByte(int field);
bool ByteField(int field); ///< toggle, slider and button rows bind bytes, lists ints
/// A button row's field (Kind::Button): Press runs it.
bool IsButton(int field);
/// Cross on a button row (the glue: the row has the page's cursor). A plain button runs at once;
/// a confirmed one arms first and runs on a second press within 10 s; a read-only line does
/// nothing. Present thread (BbGameMenu::Poll). `now_ms`: the Poll clock.
void Press(int field, std::int64_t now_ms);
/// A list row's choice `choice` can be offered (the upscalers this GPU and build run).
bool ChoiceAvailable(int field, int choice);

/// The page is opening (the game's menu thread): the rows' values from the settings, the
/// run-time texts refreshed.
void Load();
/// Each presented frame: applies what the player changed (saved to bbport.ini) and runs the
/// buttons; refreshes the run-time texts a few times a second. `now_ms`: a steady clock.
void Poll(std::int64_t now_ms);
/// Refreshes the run-time texts (party status, member rows, the restart line).
void RefreshDynamic();

// ---- The Party page's action table (extension point: party flow rows, e.g. Host / Join) ----
/// A text in both languages (UTF-8).
struct Texts {
    std::string en, ru;
};
/// One row of the Party page (or the Party mode page). Every text is made from the party board
/// when the page opens and a few times a second while it is open (Poll), so a row can change its
/// name, value and help with the party's state. The row is a button (Kind::Button: one value, no
/// arrows) or, with `toggle`, an On/Off row.
struct PartyRow {
    using Board = party::status::Board;
    int order = 0; ///< position on the page; built-in rows: status 10, rot 15, copy code 20/21, rejoin
                   ///< 30, leave 40 (notices and last event: Party members page); Party mode page:
                   ///< new party code 27 (host)
    int page = 0;  ///< 0: the Party page, 1: the Party mode page (switching role / single player)
    std::function<Texts(const Board&)> name;    ///< at most MaxNameLength units (cut otherwise)
    std::function<Texts(const Board&)> help;    ///< the bottom line, at most MaxHelpLength
    std::function<Texts(const Board&)> value;   ///< the value column (null: "-")
    std::function<bool(const Board&)> visible;  ///< null: always shown
    /// None: a read-only line. Once: `run` on every press. Confirm: the first press arms (the
    /// help line says "press again to confirm"), a second press within 10 s runs.
    enum class Press { None, Once, Confirm } press = Press::None;
    std::function<void(const Board&)> run; ///< menu thread / present thread: queue, never block
    /// A settings toggle instead of a button (value texts Off / On): saved to bbport.ini through
    /// BbSettings::Save after toggle_set.
    bool toggle = false;
    std::function<bool()> toggle_get;
    std::function<void(bool)> toggle_set;
};
/// Adds a row to the Party page. Call before Define() (BbGameMenu::PatchImage, at start). The
/// game shows at most RowsPerPage rows: keep the visible ones within it in every state.
void AddPartyRow(PartyRow row);
int PartyRowCount();
/// The party flow's say on the Party page's Status line (before Define): `hidden` (the solo
/// title, where the flow's own rows fill the page) and `describe` (a party process before its
/// link runs: the role and what happens next instead of "Single player"; false: the default).
struct PartyStatusHooks {
    std::function<bool()> hidden;
    std::function<bool(const party::status::Board&, Texts* value, Texts* help)> describe;
};
void SetPartyStatusHooks(PartyStatusHooks hooks);
/// The rows the page `page` shows now (visible ones, in order): what the game is given.
std::vector<const Row*> ShownRows(const Page& page);
/// The shown rows as "NAME=VALUE | ..." (English; " (CUT)" after a text longer than its column):
/// BB_GAME_MENU_TRACE's page line, asserted by tools/mp/smoke_party.py "rules".
std::string ShownSummary(const Page& page);

/// The "Restart the game" button (runtime_restart in the game; tests count calls).
void SetRestartHandler(void (*restart)());
/// The "Quit the game" button (bbgpu_request_quit in the game; tests count calls).
void SetQuitHandler(void (*quit)());

// ---- helpers, exposed for the test ----
int ChoiceFromPosition(u8 position, int count);
u8 ChoicePosition(int choice, int count);
/// The 0..10 position of `value` on an 11-entry table (the nearest entry).
u8 NearestPosition(const float* table, float value);
std::u16string Utf16(const char* utf8);
} // namespace BbGameMenu::Pages
