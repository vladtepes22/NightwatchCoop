// SPDX-License-Identifier: GPL-3.0-or-later
// The port's System menu pages with the party flow's rows (gpu/shim/party/party_flow_rows.cpp):
// in every party / title / link state no page shows more rows than the game's layout has widget
// slots (RowsPerPage; a row past them has nowhere to draw: 'Party name' and 'Single player' once
// vanished that way), every party row is a button (one value, no arrows), and the states the
// players reported show what they should (Continue party game from the party saves, the Status
// line in a launcher-started party process).
// ninja -C out/gpu game-menu-rows-test && out/gpu/game-menu-rows-test
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gpu/shim/bbport_game_menu_pages.h"
#include "gpu/shim/bbport_settings.h"
#include "gpu/shim/party/party_flow.h"
#include "gpu/shim/party/party_status.h"

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
                                    for (int members = 0; members <= 4; ++members) {
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
}

/// The states players reported.
static void Reported() {
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
    std::filesystem::remove(ini);
    std::printf("game menu rows: ok\n");
    return 0;
}
