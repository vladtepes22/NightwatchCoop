// SPDX-License-Identifier: GPL-2.0-or-later
// The port's pages in the game's System menu (gpu/shim/bbport_game_menu_pages.h): the game's
// limits (rows per page, the pop-up list last, text lengths, both languages), every overlay
// setting reachable and saved to bbport.ini, the party page's buttons (rejoin, leave and kick
// with a second press), the restart button and the run-time texts.
// cmake --build out/gpu --target game-menu-pages-test && out/gpu/game-menu-pages-test
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gpu/shim/bbport_game_menu_pages.h"
#include "gpu/shim/bbport_settings.h"
#include "gpu/shim/party/party_balance_rules.h"
#include "gpu/shim/party/party_status.h"

namespace P = BbGameMenu::Pages;

static std::filesystem::path ini;
static int restarts = 0;
static int quits = 0;
static int extension_runs = 0;
static std::int64_t now = 1000000;

static std::map<std::string, std::string> ReadIni() {
    std::map<std::string, std::string> keys;
    std::ifstream file(ini);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        keys[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return keys;
}

static std::size_t Length(P::u32 id, bool russian) {
    const char16_t* text = P::Text(id, russian);
    assert(text);
    std::size_t n = 0;
    while (text[n]) ++n;
    return n;
}

static void CheckText(P::u32 id, std::size_t limit, const char* what) {
    if (P::IsDynamic(id)) return; // cut to its limit when written (checked below)
    for (bool ru : {false, true}) {
        const std::size_t n = Length(id, ru);
        if (n == 0 || n > limit) {
            std::fprintf(stderr, "%s text %#x (%s): length %zu, limit %zu\n", what, id, ru ? "ru" : "en", n, limit);
            assert(false);
        }
    }
}

static void Advance(std::int64_t ms) {
    now += ms;
    P::Poll(now);
}

static void Limits() {
    const auto& pages = P::All();
    // Every page gets a factory in bbport_game_menu.cpp (P::MaxPages of them): none is dropped, and
    // a few more fit (0.6.2: the list had reached the old cap of 16).
    std::printf("pages: %zu of at most %zu\n", pages.size(), P::MaxPages);
    assert(pages.size() >= 10 && pages.size() <= P::MaxPages);
    assert(P::MaxPages >= 32 && pages.size() + 4 <= P::MaxPages);
    for (const P::Page& page : pages) {
        CheckText(page.name, P::MaxNameLength, "page name");
        CheckText(page.help, P::MaxHelpLength, "page help");
        bool hidden_rows = false;
        for (const P::Row& row : page.rows) hidden_rows |= bool(row.visible);
        assert(!page.rows.empty() && (hidden_rows || page.rows.size() <= P::RowsPerPage));
        for (std::size_t r = 0; r < page.rows.size(); ++r) {
            const P::Row& row = page.rows[r];
            CheckText(row.name, P::MaxNameLength, "row name");
            CheckText(row.help, P::MaxHelpLength, "row help");
            for (P::u32 c : row.choices) CheckText(c, P::MaxValueLength, "value");
            if (row.kind == P::Kind::List) {
                assert(r + 1 == page.rows.size()); // a pop-up list opens over the rows below it
                assert(row.choices.size() >= 2 && row.choices.size() <= 32);
            }
            if (row.kind == P::Kind::Toggle) assert(row.choices.size() == 2);
            // A button: one value (no arrows); no "press again" value pretending to be a choice.
            if (row.kind == P::Kind::Button) assert(row.choices.size() == 1 && P::IsButton(row.field));
            for (P::u32 c : row.choices)
                assert(P::IsDynamic(c) || std::u16string(P::Text(c, false)).find(u"Press again") == std::u16string::npos);
            if (row.kind == P::Kind::Steps) assert(row.choices.size() >= 2 && row.choices.size() <= 11);
            assert(P::ByteField(row.field) == (row.kind != P::Kind::List));
        }
    }
    assert(P::Text(P::IdBase - 1, false) == nullptr && P::Text(P::IdBase + P::TextCount(), true) == nullptr);
}

static void Helpers() {
    for (int count = 2; count <= 5; ++count) {
        for (int c = 0; c < count; ++c) assert(P::ChoiceFromPosition(P::ChoicePosition(c, count), count) == c);
    }
    const float table[11] = {0.f, .1f, .2f, .3f, .4f, .5f, .6f, .7f, .8f, .9f, 1.f};
    assert(P::NearestPosition(table, 0.33f) == 3 && P::NearestPosition(table, 7.f) == 10);
    assert(P::Utf16("Вкл") == u"Вкл");
}

/// Changes every setting row once (a different value) and checks the key it saved.
static void EverySettingSaved() {
    std::set<std::string> changed;
    const auto& pages = P::All();
    for (const P::Page& page : pages) {
        for (const P::Row& row : page.rows) {
            if (row.kind == P::Kind::Button) continue; // buttons save nothing
            const auto before = ReadIni();
            P::Load();
            const int f = row.field;
            if (row.kind == P::Kind::List) {
                int& v = *P::Value(f);
                v = (v + 1) % int(row.choices.size());
            } else if (row.kind == P::Kind::Toggle) {
                *P::Byte(f) = P::u8(1 - *P::Byte(f));
            } else if (row.kind == P::Kind::Slider) {
                *P::Byte(f) = P::u8(*P::Byte(f) == 10 ? 9 : *P::Byte(f) + 1);
            } else {
                const int count = int(row.choices.size());
                const int c = P::ChoiceFromPosition(*P::Byte(f), count);
                *P::Byte(f) = P::ChoicePosition((c + 1) % count, count);
            }
            Advance(16);
            Advance(500); // the debounced rows (resolution, preset)
            const auto after = ReadIni();
            for (const auto& [key, value] : after) {
                const auto it = before.find(key);
                if (it == before.end() || it->second != value) changed.insert(key);
            }
        }
    }
    // Everything the overlay menu sets (bbport_overlay.cpp tabs).
    const char* overlay[] = {
        "upscaler", "preset", "sharpen", "sharpness", "jitter", "object_motion", "reactive", "reactive_scale",
        "reactive_threshold", "reactive_max", "debug_view", "show_fps", "show_hud", "hud_position", "hud_opacity",
        "hud_scale", "hud_graph", "hud_vram", "hud_gpu", "hud_cpu", "hud_ram", "mk_enabled", "mk_sens_x",
        "mk_sens_y", "mk_invert_x", "mk_invert_y", "mk_deadzone", "mk_smoothing", "mk_wheel", "async_pipelines", "gpl",
        "compile_indicator", "party_toasts", "party_hud", "fsr4_auto_exposure", "fsr4_invert_jitter", "model_lod",
        "output_res", "live_resolution", "menu_language", "volumetric_res"};
    for (const char* key : overlay) {
        if (!changed.count(key)) {
            std::fprintf(stderr, "no game menu row changes %s\n", key);
            assert(false);
        }
    }
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        if (!changed.count(BbSettings::Effects[e].key)) {
            std::fprintf(stderr, "no game menu row changes %s\n", BbSettings::Effects[e].key);
            assert(false);
        }
    }
}

static const P::Page& PageNamed(const char16_t* english) {
    for (const P::Page& page : P::All()) {
        if (std::u16string(P::Text(page.name, false)) == english) return page;
    }
    assert(false);
    return P::All().front();
}

/// The row whose (English) name is `name`.
static const P::Row& RowNamed(const P::Page& page, const char16_t* name) {
    for (const P::Row& row : page.rows) {
        if (std::u16string(P::Text(row.name, false)) == name) return row;
    }
    assert(false);
    return page.rows.front();
}

/// The texts a row shows follow its value at once: a help line describing the value (enemy
/// scaling) is rewritten in the same Poll that applies the change, and a setting changed elsewhere
/// (the overlay menu) while a page is open moves the row's value (Byte / Value) with it.
static void LiveValues() {
    using namespace coop::balance;
    P::Load();
    Advance(300); // the periodic refresh is done: what follows must not wait for the next one
    const P::Page& scaling = PageNamed(u"Enemy scaling");
    const P::Row& boss_hp = RowNamed(scaling, u"Boss health");
    assert(P::IsDynamic(boss_hp.help));
    auto& s = BbSettings::Get();
    const auto help = [&] { return std::u16string(P::Text(boss_hp.help, false)); };
    const auto described = [&](int percent) { return P::Utf16(DescribeHp(percent, false).c_str()); };
    const int start = *P::Byte(boss_hp.field);
    assert(help().find(described(AtPosition(kHpTable, start))) == 0);

    // Changed on the page: the help line text is new after one frame (no 250 ms refresh between).
    const int next = start == 10 ? 9 : start + 1;
    std::uint64_t texts = P::DynamicChanges();
    *P::Byte(boss_hp.field) = P::u8(next);
    Advance(16);
    assert(s.party_boss_hp == AtPosition(kHpTable, next));
    assert(P::DynamicChanges() != texts);
    assert(help().find(described(AtPosition(kHpTable, next))) == 0);

    // Changed elsewhere (the overlay's Party tab): the slider's value and its help follow.
    const int other = next == 0 ? 5 : 0;
    std::uint64_t values = P::ValueChanges();
    texts = P::DynamicChanges();
    s.party_boss_hp = AtPosition(kHpTable, other);
    Advance(16);
    assert(*P::Byte(boss_hp.field) == other);
    assert(P::ValueChanges() != values && P::DynamicChanges() != texts);
    assert(help().find(described(AtPosition(kHpTable, other))) == 0);
    // ... and stays: the page does not write its old value back.
    values = P::ValueChanges();
    Advance(16);
    Advance(16);
    assert(s.party_boss_hp == AtPosition(kHpTable, other) && P::ValueChanges() == values);

    // A toggle (rot on death), a pop-up list (above companions) and a steps row (model detail).
    const P::Page& rules = PageNamed(u"Party rules");
    const P::Row& rot = RowNamed(rules, u"Rot on death");
    const P::Row& above = RowNamed(rules, u"Above companions");
    const P::Row& detail = RowNamed(PageNamed(u"Display"), u"Model detail");
    const bool rot_was = s.party_rot;
    s.party_rot = !rot_was;
    s.party_overhead = (s.party_overhead + 1) % 5;
    s.model_lod = s.model_lod == 2 ? -2 : 2;
    Advance(16);
    assert(*P::Byte(rot.field) == P::u8(!rot_was));
    assert(*P::Value(above.field) == s.party_overhead);
    assert(P::ChoiceFromPosition(*P::Byte(detail.field), 4) == (s.model_lod == 2 ? 3 : 0));
    // The page's own change wins over an outside one made in the same frame.
    *P::Byte(rot.field) = P::u8(rot_was);
    Advance(16);
    assert(s.party_rot == rot_was);
    s.party_boss_hp = 150;
    s.party_rot = true;
    Advance(16);

    // I2: the Pings page's two switches follow the settings (pings on, through walls off by default).
    const P::Page& pings = PageNamed(u"Pings");
    const P::Row& pings_row = RowNamed(pings, u"Pings");
    const P::Row& walls = RowNamed(pings, u"Show party through walls");
    s.party_pings = false;
    s.party_through_walls = true;
    Advance(16);
    s.party_pings = true;
    s.party_through_walls = false;
    Advance(16);
    assert(*P::Byte(pings_row.field) == 1 && *P::Byte(walls.field) == 0);
    s.party_through_walls = true;
    Advance(16);
    assert(*P::Byte(walls.field) == 1);
    *P::Byte(pings_row.field) = 0;
    Advance(16);
    assert(!s.party_pings);
    s.party_pings = true;
    s.party_through_walls = false;
    Advance(16);
}

/// Cross on a button row (the glue calls Press while the row has the cursor).
static void Press(const P::Row& row) {
    assert(row.kind == P::Kind::Button);
    P::Press(row.field, now);
    Advance(16);
}

static void Party() {
    namespace ps = party::status;
    ps::ResetForTest(true);
    ps::SetRole(ps::Role::Host);
    ps::SetCode("BBP1-ABCD-EFGH-IJKL");
    ps::SetState(ps::State::Joined, "2/3");
    ps::SetMembers({{"Hunter0", true, true, -1, 0, "Hunter's Dream", true},
                    {"Hunter1", true, true, 42, 1, "Cathedral Ward", false},
                    {"Hunter2", false, false, -1, 2, "", false}});
    P::Load();
    const P::Page& party = PageNamed(u"Party");
    const P::Page& members = PageNamed(u"Party members");
    assert(std::u16string(P::Text(party.rows[0].choices[0], false)) == u"Together");
    assert(std::u16string(P::Text(party.rows[0].help, false)).find(u"Host (your world)") == 0);
    // One code (no LAN code): one copy row, the code as its value; the LAN row hidden.
    assert(std::u16string(P::Text(party.rows[1].name, false)) == u"Copy party code");
    assert(std::u16string(P::Text(party.rows[1].choices[0], false)) == u"Copy");
    assert(std::u16string(P::Text(party.rows[1].help, true)).find(u"BBP1-ABCD-EFGH-IJKL") != std::u16string::npos);
    assert(!party.rows[2].visible());
    ps::SetLanCode("BBP1-LLLL-AAAA-NNNN");
    P::Load();
    assert(std::u16string(P::Text(party.rows[1].name, false)) == u"Copy Internet code" && party.rows[2].visible());
    assert(std::u16string(P::Text(party.rows[2].name, false)) == u"Copy LAN code");
    assert(std::u16string(P::Text(party.rows[2].help, false)).find(u"BBP1-LLLL-AAAA-NNNN") != std::u16string::npos);

    // Member rows: one per member, name, state and ping; the host's own row cannot be kicked.
    int visible = 0;
    for (const P::Row& row : members.rows) visible += !row.visible || row.visible();
    // three members, the notices, party HUD and guest lamps switches (the last event gives way with
    // three or more members: six rows)
    assert(visible == 6);
    const P::Row& hud = RowNamed(members, u"Party HUD");
    assert(hud.visible && hud.visible());
    const P::Row& lamps = RowNamed(members, u"Guests use lamps");
    assert(lamps.visible && lamps.visible());
    assert(std::u16string(P::Text(members.rows[0].name, false)) == u"Hunter0 (host) (you)");
    assert(std::u16string(P::Text(members.rows[1].choices[0], false)) == u"In world, 42 ms");
    assert(std::u16string(P::Text(members.rows[2].choices[0], true)) == u"Связь потеряна");

    ps::Command cmd;
    while (ps::PopCommand(&cmd)) {}
    // Kick: the first press arms (the help line says "press again"), the second kicks.
    const std::u16string kick_help = P::Text(members.rows[1].help, false);
    Press(members.rows[1]);
    assert(!ps::PopCommand(&cmd));
    assert(std::u16string(P::Text(members.rows[1].help, false)) == u"Press again to confirm");
    assert(std::u16string(P::Text(members.rows[1].choices[0], false)) == u"In world, 42 ms"); // one value
    Press(members.rows[1]);
    assert(std::u16string(P::Text(members.rows[1].help, false)) == kick_help);
    assert(ps::PopCommand(&cmd) && cmd.type == ps::CommandType::Kick && cmd.name == "Hunter1");
    // Not the host's own row.
    Press(members.rows[0]);
    Press(members.rows[0]);
    assert(!ps::PopCommand(&cmd));
    // A second press after 10 s only disarms.
    Press(members.rows[2]);
    Advance(11000);
    Press(members.rows[2]);
    assert(!ps::PopCommand(&cmd));

    // Rejoin: at once; Leave: confirmed.
    const P::Row& rejoin = RowNamed(party, u"Summon again");
    Press(rejoin);
    assert(ps::PopCommand(&cmd) && cmd.type == ps::CommandType::Rejoin);
    Press(rejoin); // every press runs a Once row (the text box rows: each press opens it)
    assert(ps::PopCommand(&cmd) && cmd.type == ps::CommandType::Rejoin);
    const P::Row& leave = RowNamed(party, u"Leave the party");
    Press(leave);
    assert(!ps::PopCommand(&cmd));
    Press(leave);
    assert(ps::PopCommand(&cmd) && cmd.type == ps::CommandType::Leave);

    // Run-time texts are cut to their limits.
    ps::SetLastEvent(std::string(300, 'x'));
    Advance(300);
    const P::Row& event = members.rows.back();
    assert(std::u16string(P::Text(event.name, false)) == u"Last event");
    assert(Length(event.choices[0], false) == P::MaxValueLength);
    assert(std::u16string(P::Text(event.choices[0], false)).substr(P::MaxValueLength - 3) == u"...");
    assert(Length(event.help, true) == P::MaxHelpLength);

    // Nobody: one row says so.
    ps::SetMembers({});
    P::Load();
    visible = 0;
    for (const P::Row& row : members.rows) visible += !row.visible || row.visible();
    assert(visible == 5 && std::u16string(P::Text(members.rows[0].name, false)) == u"No one yet");

    // A full party (host + three, A7): the game shows the first six visible rows: the four members,
    // the notices and the party HUD switch.
    ps::SetMembers({{"Hunter0", true, true, -1, 0, "", true},
                    {"Hunter1", true, true, 11, 1, "", false},
                    {"Hunter2", true, false, 12, 2, "", false},
                    {"Hunter3", true, true, 13, 3, "", false}});
    P::Load();
    std::vector<const P::Row*> full;
    for (const P::Row& row : members.rows) if (!row.visible || row.visible()) full.push_back(&row);
    assert(full.size() == 6 && P::RowsPerPage == 6);
    assert(std::u16string(P::Text(full[3]->name, false)) == u"Hunter3");
    assert(std::u16string(P::Text(full[3]->choices[0], false)) == u"In world, 13 ms");
    assert(std::u16string(P::Text(full[2]->choices[0], false)) == u"In own world, 12 ms");
    assert(std::u16string(P::Text(full[4]->name, false)) == u"Join/leave notices");
    assert(std::u16string(P::Text(full[5]->name, false)) == u"Party HUD");
    while (ps::PopCommand(&cmd)) {}
    Press(*full[3]);
    Press(*full[3]);
    assert(ps::PopCommand(&cmd) && cmd.type == ps::CommandType::Kick && cmd.name == "Hunter3");

    // Party mode off (single player): no commands from the buttons; the party-only rows (copy,
    // rejoin, leave) are hidden, the extension row shows (in order).
    ps::ResetForTest(false);
    P::Load();
    assert(P::PartyRowCount() == 6); // status, copy, LAN copy, rejoin, leave + the extension row
    assert(!hud.visible()); // the party HUD switch: party mode only
    std::vector<const P::Row*> shown;
    for (const P::Row& row : party.rows) if (!row.visible || row.visible()) shown.push_back(&row);
    assert(shown.size() == 2 && std::u16string(P::Text(shown[1]->name, true)) == u"Создать группу");
    const int runs = extension_runs;
    Press(*shown[1]);
    assert(extension_runs == runs + 1);
    Press(*shown[1]); // every press runs it
    assert(extension_runs == runs + 2);
    assert(std::u16string(P::Text(party.rows[0].choices[0], false)) == u"Single player");
    Press(rejoin);
    Press(leave);
    Press(leave);
    assert(!ps::PopCommand(&cmd));
}

static void Restart() {
    P::Load();
    const P::Row& row = RowNamed(PageNamed(u"Game patches"), u"Restart the game");
    Press(row);
    assert(restarts == 0);
    Press(row);
    assert(restarts == 1);
}

// Upstream 0.51 (#74, #116): "Quit the game" asks for a second press, as Restart does.
static void Quit() {
    P::Load();
    const P::Row& row = RowNamed(PageNamed(u"Game patches"), u"Quit the game");
    Press(row);
    assert(quits == 0);
    Press(row);
    assert(quits == 1);
}

/// --labels FILE: the English page / row names and help lines, for test drivers that read the
/// focused item off the screen (tools: the QA menu driver matches OCR text against them).
static void DumpLabels(const char* path) {
    std::ofstream out(path);
    const auto ascii = [](const char16_t* t) {
        std::string s;
        for (; t && *t; ++t) s.push_back(*t < 128 ? char(*t) : '?');
        return s;
    };
    const char tab = '\t', end = '\n';
    for (const P::Page& page : P::All()) {
        out << "page" << tab << ascii(P::Text(page.name, false)) << tab << ascii(P::Text(page.help, false)) << end;
        for (const P::Row& row : page.rows) {
            out << "row" << tab << ascii(P::Text(page.name, false)) << tab << ascii(P::Text(row.name, false)) << tab
                << ascii(P::Text(row.help, false)) << end;
        }
    }
}

int main(int argc, char** argv) {
    ini = std::filesystem::temp_directory_path() / "bbport-game-menu-pages-test.ini";
    {
        std::ofstream file(ini);
        file << "# test\nkey.cross=X, Space\nupscaler=fsr3\n";
    }
#ifdef _WIN32
    _putenv_s("BB_CONFIG", ini.string().c_str());
    _putenv_s("BB_MENU_LANGUAGE", "en");
#else
    setenv("BB_CONFIG", ini.string().c_str(), 1);
    setenv("BB_MENU_LANGUAGE", "en", 1);
#endif
    BbSettings::Load();
    BbSettings::ConfigureUpscalerSupport(true, true);
    BbSettings::ConfigureDlssSupport(true, nullptr);
    party::status::ResetForTest(false);
    P::SetRestartHandler([] { ++restarts; });
    P::SetQuitHandler([] { ++quits; });
    // An extension row (party flow): hidden unless party mode is off, a confirmed action.
    {
        P::PartyRow row;
        row.order = 35;
        row.name = [](const P::PartyRow::Board&) { return P::Texts{"Host party", "Создать группу"}; };
        row.value = [](const P::PartyRow::Board&) { return P::Texts{"Host", "Создать"}; };
        row.visible = [](const P::PartyRow::Board& b) { return !b.enabled; };
        row.press = P::PartyRow::Press::Once;
        row.run = [](const P::PartyRow::Board&) { ++extension_runs; };
        P::AddPartyRow(row);
    }
    P::Define();
    P::Define(); // once
    if (argc == 3 && std::string(argv[1]) == "--labels") {
        DumpLabels(argv[2]);
        return 0;
    }
    Limits();
    Helpers();
    EverySettingSaved();
    LiveValues();
    assert(ReadIni()["key.cross"] == "X, Space"); // the launcher's keys stay
    Party();
    Restart();
    Quit();
    std::filesystem::remove(ini);
    std::printf("game menu pages: %zu pages, %d fields, %zu texts: ok\n", P::All().size(), P::FieldCount(),
                P::TextCount());
    return 0;
}
