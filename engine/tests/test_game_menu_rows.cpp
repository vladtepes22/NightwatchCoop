// SPDX-License-Identifier: GPL-3.0-or-later
// The port's System menu pages with the party flow's rows (gpu/shim/party/party_flow_rows.cpp):
// in every party / title / link state no page shows more rows than the game's layout has widget
// slots (RowsPerPage; a row past them has nowhere to draw: 'Party name' and 'Single player' once
// vanished that way), every party row is a button (one value, no arrows), and the states the
// players reported show what they should (Continue party game from the party saves, the Status
// line in a launcher-started party process).
// ninja -C out/gpu game-menu-rows-test && out/gpu/game-menu-rows-test
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gpu/shim/bbport_game_menu_pages.h"
#include "gpu/shim/bbport_settings.h"
#include "gpu/shim/party/party_flow.h"
#include "gpu/shim/party/party_link.h"
#include "gpu/shim/party/party_status.h"

#include "party/party_invade.h"

// The game menu pages call the invasion module (the Block the invader row); the unit test has no game: stubs.
namespace coop::invade {
std::string InvaderName() { return {}; }
void RequestBlock() {}
}  // namespace coop::invade

namespace P = BbGameMenu::Pages;
namespace ps = party::status;

// ---- The party flow's game glue, as the rows see it (party_flow_game.cpp in the game). ----
static party::flow::Context context;
static int runs = 0, edits = 0;
namespace party::flow {
bool Enabled() { return true; }
Context CurrentContext() { return context; }
Saved Current() { return context.saved; }
std::string LastMessage() { return {}; }
Plan Run(Action) {
    ++runs;
    return {};
}
bool BeginEdit(Field) {
    ++edits;
    return true;
}
} // namespace party::flow

static std::int64_t now = 1000000;

static std::string Ascii(const char16_t* t) {
    std::string s;
    for (; t && *t; ++t) s.push_back(*t < 128 ? char(*t) : '?');
    return s;
}

static const P::Page& PageNamed(const char* english) {
    for (const P::Page& page : P::All())
        if (Ascii(P::Text(page.name, false)) == english) return page;
    assert(false);
    return P::All().front();
}

static std::vector<std::string> Names(const P::Page& page) {
    std::vector<std::string> names;
    for (const P::Row* row : P::ShownRows(page)) names.push_back(Ascii(P::Text(row->name, false)));
    return names;
}

static bool Has(const std::vector<std::string>& names, const char* name) {
    for (const auto& n : names)
        if (n == name) return true;
    return false;
}

/// Every page within its slots in the current state.
static void CheckAllPages(const char* what) {
    P::Load();
    for (const P::Page& page : P::All()) {
        const auto shown = P::ShownRows(page);
        if (shown.size() > P::RowsPerPage) {
            std::fprintf(stderr, "%s: page '%s' shows %zu rows (slots: %zu):", what, Ascii(P::Text(page.name, false)).c_str(),
                         shown.size(), P::RowsPerPage);
            for (const auto& n : Names(page)) std::fprintf(stderr, " '%s'", n.c_str());
            std::fprintf(stderr, "\n");
            assert(false);
        }
    }
}

/// Walks the party states: process kind and role, title / world, link, saved character and
/// address, the board (state, role, codes, members).
static void EveryState() {
    int states = 0;
    for (int process = 0; process < 2; ++process)
        for (int host = 0; host < 2; ++host)
            for (int world = 0; world < 2; ++world)
                for (int link = 0; link < 2; ++link)
                    for (int character = 0; character < 2; ++character)
                        for (int address = 0; address < 2; ++address)
                            for (int board = 0; board < 4; ++board)
                                for (int codes = 0; codes < 3; ++codes)
                                    for (int members = 0; members <= party::kMaxPartyPlayers; ++members) {
                                        context = {};
                                        context.party_process = process;
                                        context.host = process && host;
                                        context.party_save = process;
                                        context.in_world = world;
                                        context.link_active = process && link;
                                        context.saved.has_character = character;
                                        context.saved.host_address = address ? "BBP1-ABCD-EFGH-IJKL" : "";
                                        ps::ResetForTest(process);
                                        ps::SetRole(board == 0 ? ps::Role::Off : host ? ps::Role::Host : ps::Role::Guest);
                                        ps::SetState(board == 0   ? ps::State::Off
                                                     : board == 1 ? ps::State::Hosting
                                                     : board == 2 ? ps::State::Joined
                                                                  : ps::State::Reconnecting);
                                        if (codes >= 1) ps::SetCode("BBP1-AAAA-BBBB-CCCC");
                                        if (codes >= 2) ps::SetLanCode("BBP1-DDDD-EEEE-FFFF");
                                        std::vector<ps::Member> list;
                                        for (int k = 0; k < members; ++k)
                                            list.push_back({"Hunter" + std::to_string(k), true, true, 10, k, "", k == 0});
                                        ps::SetMembers(list);
                                        CheckAllPages("party state");
                                        ++states;
                                    }
    std::printf("game menu rows: %d party states within %zu rows a page\n", states, P::RowsPerPage);
    // 0.7.1: the switches that had only an env variable (Shared world: the host's rules, one row
    // each for host and guest; Party rewards: the key item rows, full at six with the Insight list).
    for (const ps::Role role : {ps::Role::Host, ps::Role::Guest, ps::Role::Off}) {
        ps::ResetForTest(true);
        ps::SetRole(role);
        P::Load();
        const auto world = Names(PageNamed("Party rules: World"));
        assert(world.size() == 6 && Has(world, "Enemies stay dead") && Has(world, "Host can gather") &&
               Has(world, "NPCs as in host's world") && Has(world, "Covenants per player") &&
               Has(world, "Party votes on travel") &&
               Has(world, "NPC hunters fight all"));  // 0.7.3
        // CL-a3a9e6: Guests can gather, a host rule on the Travel page (host toggle / guest read-only line)
        assert(Has(Names(PageNamed("Party rules: Travel")), "Guests can gather"));
        assert(Has(Names(PageNamed("Party rules: PvP & lamps")), "Chalice layer travel"));  // CL-48ee07
        // 0.7.5 partyrules: the item rows stay each player's own; the Insight list is the host's rule
        const auto items = Names(PageNamed("My party: Your items"));
        assert(Has(items, "Own key items as guest") && Has(items, "Key items at once"));
        assert(Has(Names(PageNamed("My party: Giving")), "Give items to the party") &&
               Has(Names(PageNamed("My party: Giving")), "Give weapons and armor"));
        const auto rewards = Names(PageNamed("Party rules: Rewards"));
        assert(Has(rewards, "Full rewards for guests") && rewards.back() == "Insight for bosses");
    }
}

/// The states players reported.
static void Reported() {
    {
        // A running host with both codes: New party code (two presses) is on the Party mode page
        // (the Party page is full); a guest never sees it.
        context = {};
        context.party_process = context.party_save = context.host = context.in_world = context.link_active = true;
        ps::ResetForTest(true);
        ps::SetRole(ps::Role::Host);
        ps::SetState(ps::State::Hosting);
        ps::SetCode("BBP1-AAAA-BBBB-CCCC");
        ps::SetLanCode("BBP1-DDDD-EEEE-FFFF");
        P::Load();
        const auto names = Names(PageNamed("Party"));
        assert(names.size() <= P::RowsPerPage && Has(names, "Leave the party") && !Has(names, "New party code"));
        const P::Page& page = PageNamed("Party mode");
        assert(Has(Names(page), "New party code") && Names(page).size() <= P::RowsPerPage);
        ps::ResetForTest(true);
        ps::SetRole(ps::Role::Guest);
        ps::SetState(ps::State::Joined);
        P::Load();
        assert(!Has(Names(page), "New party code"));
        // The host's press queues the command (the board says host); a guest's would not.
        ps::ResetForTest(true);
        ps::SetRole(ps::Role::Host);
        assert(ps::RequestNewCode());
        ps::Command c;
        assert(ps::PopCommand(&c) && c.type == ps::CommandType::NewCode);
        ps::SetRole(ps::Role::Guest);
        assert(!ps::RequestNewCode());
    }
    const P::Page& party = PageNamed("Party");
    const P::Page& mode = PageNamed("Party mode");
    // The solo title with a party character: all six flow rows, 'Party name' too (no Status line).
    context = {};
    context.saved.has_character = true;
    ps::ResetForTest(false);
    P::Load();
    auto names = Names(party);
    assert(names.size() == 6 && Has(names, "Continue party game") && Has(names, "Party name") && !Has(names, "Status"));
    // Every party row is a button: one value, no arrows.
    for (const P::Row* row : P::ShownRows(party)) assert(row->kind == P::Kind::Button && row->choices.size() == 1);
    // The text rows show their value and open the text box on every press.
    const P::Row* address = nullptr;
    for (const P::Row* row : P::ShownRows(party))
        if (Ascii(P::Text(row->name, false)) == "Host address") address = row;
    assert(address && Ascii(P::Text(address->choices[0], false)) == "Not set");
    P::Press(address->field, now);
    P::Press(address->field, now + 100);
    assert(edits == 2);
    // A confirmed row: the first press arms, the second runs (once).
    const P::Row* fresh = P::ShownRows(party)[0];
    assert(Ascii(P::Text(fresh->name, false)) == "New party game");
    P::Press(fresh->field, now);
    assert(runs == 0 && Ascii(P::Text(fresh->help, false)) == "Press again to confirm");
    P::Press(fresh->field, now + 500);
    assert(runs == 1);
    P::Press(fresh->field, now + 20000); // arms again, does not run
    assert(runs == 1);

    // An unlinked guest in a party process (the link dropped): Party mode keeps Single player.
    context = {};
    context.party_process = context.party_save = true;
    context.in_world = true;
    ps::ResetForTest(true);
    ps::SetRole(ps::Role::Guest);
    ps::SetState(ps::State::Reconnecting);
    ps::SetCode("BBP1-AAAA-BBBB-CCCC");
    P::Load();
    names = Names(mode);
    assert(Has(names, "Host party") && Has(names, "Join party") && Has(names, "Single player"));
    assert(Names(party).size() <= P::RowsPerPage);

    // A launcher-started party process on the title: the Status line says which party game.
    context = {};
    context.party_process = context.party_save = context.host = true;
    ps::ResetForTest(true);
    P::Load();
    names = Names(party);
    assert(names.size() == 1 && names[0] == "Status");
    assert(Ascii(P::Text(P::ShownRows(party)[0]->choices[0], false)) == "Host, not open yet");
    names = Names(mode);
    assert(Has(names, "Join party") && Has(names, "Single player"));

    // A single-player process: Party mode says where party games start.
    context = {};
    context.in_world = true;
    ps::ResetForTest(false);
    P::Load();
    names = Names(mode);
    assert(names.size() == 1 && names[0] == "Party mode");

    // 0.7.8 Party access: the host has the Open party and invaders switches (live), a guest one read-only line.
    const P::Page& access = PageNamed("Party access");
    context = {};
    context.party_process = context.party_save = context.host = context.in_world = true;
    ps::ResetForTest(true);
    ps::SetRole(ps::Role::Host);
    ps::SetState(ps::State::Hosting);
    P::Load();
    names = Names(access);
    assert(names.size() == 4 && names[0] == "Open party (no password)" && names[1] == "Any level can join" &&
           names[2] == "Allow invaders" && names[3] == "Max invaders");  // lobby: the launcher's Show; Max invaders: visy 0.7.9 (CL-7e748c), host only; Any level: visy fc532488
    context = {};
    context.party_process = context.party_save = context.in_world = true;
    ps::ResetForTest(true);
    ps::SetRole(ps::Role::Guest);
    ps::SetState(ps::State::Joined);
    P::Load();
    names = Names(access);
    assert(names.size() == 1 && names[0] == "Host's access");
}

static std::size_t Length(P::u32 id, bool russian) {
    std::size_t n = 0;
    for (const char16_t* t = P::Text(id, russian); t && *t; ++t) ++n;
    return n;
}

/// Nightwatch: System holds one Party Settings item in place of the pages named "party" (any case), and
/// Party Settings holds those pages in order: every page is in exactly one of the two lists.
static void PartySettings() {
    const auto& pages = P::All();
    for (bool ru : {false, true}) {
        assert(Length(P::PartySettingsName(), ru) > 0 && Length(P::PartySettingsName(), ru) <= P::MaxNameLength);
        assert(Length(P::PartySettingsHelp(), ru) > 0 && Length(P::PartySettingsHelp(), ru) <= P::MaxHelpLength);
    }
    assert(std::u16string(P::Text(P::PartySettingsName(), false)) == u"Party Settings");
    assert(std::u16string(P::Text(P::PartySettingsName(), true)) == u"Настройки группы");

    const std::vector<int> system = P::SystemEntries();
    const std::vector<int> party = P::PartySettingsEntries();
    auto english = [&](int i) { return Ascii(P::Text(pages[std::size_t(i)].name, false)); };
    auto count = [](const std::vector<int>& v, int x) { return std::count(v.begin(), v.end(), x); };
    assert(count(system, P::PartySettingsEntry) == 1);
    assert(system.front() == P::PartySettingsEntry); // Party is the first page: the item stays after Screen/Sound
    assert(!party.empty() && english(party.front()) == "Party" && std::is_sorted(party.begin(), party.end()));
    std::vector<int> seen(pages.size(), 0);
    for (const int i : party) {
        std::string lower = english(i);
        for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
        assert(lower.find("party") != std::string::npos && P::IsPartyPage(pages[std::size_t(i)]));
        ++seen[std::size_t(i)];
    }
    for (const int i : system) {
        if (i == P::PartySettingsEntry) continue;
        assert(!P::IsPartyPage(pages[std::size_t(i)]));
        ++seen[std::size_t(i)];
    }
    for (const int n : seen) assert(n == 1);
    // Both kinds of party page move; Game HUD stays in System.
    auto index_of = [&](const char* name) { return int(&PageNamed(name) - pages.data()); };
    for (const char* name : {"Party mode", "Party rules: World", "My party: Loading"}) assert(count(party, index_of(name)) == 1);
    assert(count(system, index_of("Game HUD")) == 1);
    std::printf("party settings: %zu pages grouped, %zu entries left in System\n", party.size(), system.size());
}

/// Nightwatch: the three lists the System builder makes, from the rows it adds in 1.09's order (0x1bb3ad0:
/// Controls, Environment, Brightness, Network, Exit Game, Language; then the PC menus' pages and Quit to
/// Desktop), placed the way AddHook / CommandHook place them (P::PlaceGameRow).
static void GameSettings() {
    for (bool ru : {false, true}) {
        assert(Length(P::GameSettingsName(), ru) > 0 && Length(P::GameSettingsName(), ru) <= P::MaxNameLength);
        assert(Length(P::GameSettingsHelp(), ru) > 0 && Length(P::GameSettingsHelp(), ru) <= P::MaxHelpLength);
    }
    assert(std::u16string(P::Text(P::GameSettingsName(), false)) == u"Game Settings");
    assert(std::u16string(P::Text(P::GameSettingsName(), true)) == u"Настройки игры");
    // PC Enhancements, PC Graphics, PC Effects, PC Colour, PC Controls, Key Bindings, PC Camera (option_menu.cpp).
    const std::vector<P::u32> pc = {110013, 110007, 110010, 110014, 110008, 110009, 110011};
    std::vector<P::u32> rows = {P::ControlsRow, P::EnvironmentRow, P::BrightnessRow, P::NetworkRow, P::ExitGameRow,
                                P::LanguageRow};
    rows.insert(rows.end(), pc.begin(), pc.end());
    rows.push_back(P::QuitToDesktopRow);
    constexpr P::u32 GameSettingsItem = 1, PartySettingsItem = 2, GameHudItem = 3, PartyPages = 4; // the port's
    // The builder as 1.09 runs it: each game row added (or skipped by the hook), then dropped as the list's last
    // item when it is not shown (P::BuilderDrops); the PC menus' rows at the finish, never dropped. The hook adds
    // its own items before the row at hand and lets a skipped row in when the builder will drop it.
    auto build = [&](P::MenuList list, bool flag, bool full, bool online) {
        std::vector<P::u32> shown;
        for (const P::u32 row : rows) {
            if (list == P::MenuList::System && row == P::ControlsRow) {
                shown.insert(shown.end(), {GameSettingsItem, PartySettingsItem, GameHudItem});
            }
            if (list == P::MenuList::PartySettings && row == P::EnvironmentRow) shown.push_back(PartyPages);
            const bool drops = P::BuilderDrops(row, flag, full, online);
            const bool skipped_drops = P::BuilderDrops(row, false, full, online); // what the hook predicts (flag 0)
            if (P::PlaceGameRow(list, row) == P::GameRow::Keep || skipped_drops) shown.push_back(row);
            if (drops && !shown.empty()) shown.pop_back();
        }
        return shown;
    };
    // The game's in-game System (flag 1): Exit Game shown, Language not (as before the port's lists).
    const std::vector<P::u32> system = {GameSettingsItem, PartySettingsItem, GameHudItem, P::ExitGameRow,
                                        P::QuitToDesktopRow};
    assert(build(P::MenuList::System, true, true, true) == system);
    // The Nightwatch lists: the System list factory's flag 0 drops Exit Game; nothing of ours is lost.
    std::vector<P::u32> game = {P::ControlsRow, P::EnvironmentRow, P::BrightnessRow};
    game.insert(game.end(), pc.begin(), pc.end());
    assert(build(P::MenuList::GameSettings, false, true, true) == game);
    assert(build(P::MenuList::PartySettings, false, true, true) == std::vector<P::u32>{PartyPages});
    // Builds that hide more (no full game, offline): the builder drops its own rows, never one of ours.
    for (bool full : {false, true}) {
        for (bool online : {false, true}) {
            const auto s = build(P::MenuList::System, true, full, online);
            assert(std::vector<P::u32>(s.begin(), s.begin() + 3) ==
                   (std::vector<P::u32>{GameSettingsItem, PartySettingsItem, GameHudItem}));
            const auto g = build(P::MenuList::GameSettings, false, full, online);
            assert(g.front() == P::ControlsRow && g.size() == (full ? 3 : 1) + pc.size());
            assert(build(P::MenuList::PartySettings, false, full, online) == std::vector<P::u32>{PartyPages});
        }
    }
    // Network is in none of the lists; Game HUD is the one port page left in System (until Environment has its row).
    for (auto list : {P::MenuList::System, P::MenuList::GameSettings, P::MenuList::PartySettings})
        assert(P::PlaceGameRow(list, P::NetworkRow) == P::GameRow::Skip);
    const std::vector<int> left = P::SystemEntries();
    assert(left.size() == 2 && left[0] == P::PartySettingsEntry &&
           Ascii(P::Text(P::All()[std::size_t(left[1])].name, false)) == "Game HUD");
    std::printf("game settings: System %zu items, Game Settings %zu rows\n", system.size(), game.size());
}

int main() {
    const auto ini = std::filesystem::temp_directory_path() / "bbport-game-menu-rows-test.ini";
    { std::ofstream file(ini); }
#ifdef _WIN32
    _putenv_s("BB_CONFIG", ini.string().c_str());
    _putenv_s("BB_MENU_LANGUAGE", "en");
#else
    setenv("BB_CONFIG", ini.string().c_str(), 1);
    setenv("BB_MENU_LANGUAGE", "en", 1);
#endif
    BbSettings::Load();
    ps::ResetForTest(false);
    party::flow::RegisterMenuRows();
    P::Define();
    EveryState();
    Reported();
    PartySettings();
    GameSettings();
    std::filesystem::remove(ini);
    std::printf("game menu rows: ok\n");
    return 0;
}
