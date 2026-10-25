// SPDX-License-Identifier: GPL-2.0-or-later
// The port's pages in the game's System menu (gpu/shim/bbport_game_menu_pages.h): the game's
// limits (rows per page, the pop-up list last, text lengths, both languages), every overlay
// setting reachable and saved to bbport.ini, the party page's buttons (rejoin, leave and kick
// with a second press), the restart button and the run-time texts.
// cmake --build out/gpu --target game-menu-pages-test && out/gpu/game-menu-pages-test
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
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
    // Every party gameplay switch (the launcher's Party Rules tab) has a row (2026-10-21).
    for (int k = 0; k < BbSettings::PartySwitchCount; ++k) {
        if (!changed.count(BbSettings::PartySwitches[k].key)) {
            std::fprintf(stderr, "no game menu row changes %s\n", BbSettings::PartySwitches[k].key);
            assert(false);
        }
    }
    assert(changed.count("party_guest_insight") && changed.count("party_guest_lamps") && changed.count("party_rot"));
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
    const P::Page& scaling = PageNamed(u"Party rules: Enemies");
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

    // Poise starts at 0 (user decision 2026-10-21: below 0 enemies staggered less): the slider's
    // leftmost position is the game's own 0 %, the rightmost +300 %.
    const P::Row& enemy_poise = RowNamed(scaling, u"Enemy poise");
    *P::Byte(enemy_poise.field) = 0;
    Advance(16);
    assert(s.party_enemy_poise == 0);
    assert(std::u16string(P::Text(enemy_poise.help, false)).find(P::Utf16(DescribePoise(0, false).c_str())) == 0);
    *P::Byte(enemy_poise.field) = 10;
    Advance(16);
    assert(s.party_enemy_poise == 300);
    s.party_enemy_poise = 0;
    Advance(16);

    // A toggle (rot on death), a pop-up list (above companions) and a steps row (model detail).
    const P::Page& rules = PageNamed(u"Party rules: Rot");
    const P::Row& rot = RowNamed(rules, u"Rot on death");
    const P::Row& above = RowNamed(PageNamed(u"My party: Display"), u"Above companions");
    const P::Row& detail = RowNamed(PageNamed(u"Display"), u"Model detail");
    const bool rot_was = s.party_rot;
    s.party_rot = !rot_was;
    s.party_overhead = (s.party_overhead + 1) % 5;
    s.model_lod = s.model_lod == 1 ? 0 : 1;
    Advance(16);
    assert(*P::Byte(rot.field) == P::u8(!rot_was));
    assert(*P::Value(above.field) == s.party_overhead);
    assert(P::ChoiceFromPosition(*P::Byte(detail.field), 2) == (s.model_lod == 1 ? 1 : 0));
    // The page's own change wins over an outside one made in the same frame.
    *P::Byte(rot.field) = P::u8(rot_was);
    Advance(16);
    assert(s.party_rot == rot_was);
    s.party_boss_hp = 150;
    s.party_rot = true;
    Advance(16);

    // I2: the My party: Display page's ping switches follow the settings (pings on, through walls off).
    const P::Page& pings = PageNamed(u"My party: Display");
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

    // My party: Look & notices: Companions without glow follows party_glow_off both ways (on by default).
    const P::Row& glow = RowNamed(PageNamed(u"My party: Look & notices"), u"No companion glow");
    assert(glow.kind == P::Kind::Toggle);
    s.party_glow_off = true;  // (the sweep above may have flipped it)
    Advance(16);
    assert(*P::Byte(glow.field) == 1);
    *P::Byte(glow.field) = 0;
    Advance(16);
    assert(!s.party_glow_off);
    s.party_glow_off = true;
    Advance(16);
    assert(*P::Byte(glow.field) == 1);
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
    // This player's rot (party_balance via party_status): none, then what the stacks do.
    assert(std::u16string(P::Text(party.rows[1].name, false)) == u"Rot");
    assert(std::u16string(P::Text(party.rows[1].choices[0], false)) == u"None");
    {
        const int kinds[ps::kRotKinds] = {2, 0, 1, 0, 0};
        ps::SetRot(kinds);
        P::Load();
        assert(std::u16string(P::Text(party.rows[1].choices[0], false)) == u"3/5");
        assert(std::u16string(P::Text(party.rows[1].help, false)) ==
               u"Rot 3/5: max HP -20 %, stamina costs +10 %. A lamp rest cleanses it");
        const int none[ps::kRotKinds] = {};
        ps::SetRot(none);
    }
    // One code (no LAN code): one copy row, the code as its value; the LAN row (on the Party mode
    // page: the Party page is full with Gather the party) hidden.
    const P::Page& mode = PageNamed(u"Party mode");
    const P::Row& lan = RowNamed(mode, u"Copy LAN code");
    assert(std::u16string(P::Text(party.rows[2].name, false)) == u"Copy party code");
    assert(std::u16string(P::Text(party.rows[2].choices[0], false)) == u"Copy");
    assert(std::u16string(P::Text(party.rows[2].help, true)).find(u"BBP1-ABCD-EFGH-IJKL") != std::u16string::npos);
    assert(!lan.visible());
    ps::SetLanCode("BBP1-LLLL-AAAA-NNNN");
    P::Load();
    assert(std::u16string(P::Text(party.rows[2].name, false)) == u"Copy Internet code" && lan.visible());
    assert(std::u16string(P::Text(lan.help, false)).find(u"BBP1-LLLL-AAAA-NNNN") != std::u16string::npos);
    // I5: Gather the party, the host's row (one press queues the command; a guest's board has none).
    {
        const P::Row& gather = RowNamed(party, u"Gather the party");
        assert(gather.visible && gather.visible());
        ps::Command c;
        while (ps::PopCommand(&c)) {}
        Press(gather);
        assert(ps::PopCommand(&c) && c.type == ps::CommandType::Gather);
        ps::SetRole(ps::Role::Guest);
        P::Load();
        assert(!gather.visible());
        assert(!ps::RequestGather());
        ps::SetRole(ps::Role::Host);
        P::Load();
    }

    // Member rows: one per member, name, state and ping; the host's own row cannot be kicked.
    int visible = 0;
    for (const P::Row& row : members.rows) visible += !row.visible || row.visible();
    // three members and the last event (0.7.3: the notices on My party: Look & notices, the DHT
    // switch on Party network)
    assert(visible == 4);
    const P::Row& hud = RowNamed(PageNamed(u"My party: Display"), u"Party HUD");
    assert(hud.visible && hud.visible());
    RowNamed(PageNamed(u"Party network"), u"Find via public DHT");
    RowNamed(PageNamed(u"My party: Look & notices"), u"Join/leave notices");
    RowNamed(PageNamed(u"Party rules: Travel"), u"Guests use lamps");
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
    assert(visible == 2 && std::u16string(P::Text(members.rows[0].name, false)) == u"No one yet");

    // A full party (host + three, A7): the four members and the last event.
    ps::SetMembers({{"Hunter0", true, true, -1, 0, "", true},
                    {"Hunter1", true, true, 11, 1, "", false},
                    {"Hunter2", true, false, 12, 2, "", false},
                    {"Hunter3", true, true, 13, 3, "", false}});
    P::Load();
    std::vector<const P::Row*> full;
    for (const P::Row& row : members.rows) if (!row.visible || row.visible()) full.push_back(&row);
    assert(full.size() == 5 && full.size() <= P::RowsPerPage);
    assert(std::u16string(P::Text(full[3]->name, false)) == u"Hunter3");
    assert(std::u16string(P::Text(full[3]->choices[0], false)) == u"In world, 13 ms");
    assert(std::u16string(P::Text(full[2]->choices[0], false)) == u"In own world, 12 ms");
    assert(std::u16string(P::Text(full[4]->name, false)) == u"Last event");
    while (ps::PopCommand(&cmd)) {}
    Press(*full[3]);
    Press(*full[3]);
    assert(ps::PopCommand(&cmd) && cmd.type == ps::CommandType::Kick && cmd.name == "Hunter3");

    // Party mode off (single player): no commands from the buttons; the party-only rows (copy,
    // rejoin, leave) are hidden, the extension row shows (in order).
    ps::ResetForTest(false);
    P::Load();
    assert(P::PartyRowCount() == 9); // status, rot, copy, LAN copy, rejoin, gather, new party code, leave + the extension row
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

/// The party rules pages (2026-10-21): the layout, page -> rows as a host and as a guest (a host's
/// rule: the toggle for the host, a read-only line with the host's value for a guest).
static void RulesPages() {
    namespace ps = party::status;
    const auto names = [](const char16_t* page) {
        std::vector<std::u16string> out;
        for (const P::Row* row : P::ShownRows(PageNamed(page))) out.push_back(P::Text(row->name, false));
        return out;
    };
    const auto kinds = [](const char16_t* page) {
        std::vector<P::Kind> out;
        for (const P::Row* row : P::ShownRows(PageNamed(page))) out.push_back(row->kind);
        return out;
    };
    using V = std::vector<std::u16string>;
    // 0.7.3: the host's rules (Party rules: ...) and each player's own (My party: ...) apart.
    // 0.7.5 partyrules: every session rule is the host's (also the guests' deaths, travel and rewards).
    const V deaths = {u"Host gets up, no load", u"Up after boss kill", u"Anyone can be abducted",
                      u"Guest gets up, no load", u"Respawn at host's lamp", u"Refill vials, bullets"};
    // 0.7.2: rot and what else cleanses it, the host's rules, on a page of their own.
    const V rot = {u"Rot on death", u"Rot: boss kill cleanses", u"Rot: kill your killer", u"Rot: echoes back"};
    const V world = {u"Friendly fire / PvP", u"Shared lamps", u"Guests bring their lamps"};
    const V travel = {u"Guests use lamps", u"Guests use Hunter's Mark", u"Open world", u"Stay in Chalice Dungeons",
                      u"Cutscenes live with host"};
    const V look = {u"No companion glow", u"Ping in the party list", u"Join/leave notices", u"Share NPC dialogue",
                    u"NPC dialogue range", u"Party discovery"};
    const V save = {u"Share story progress", u"Share NPC quests", u"Share cutscenes, endings"};
    const V shared = {u"Enemies stay dead", u"Host can gather", u"NPCs as in host's world", u"Covenants per player",
                      u"Party votes on travel", u"NPC hunters fight all"};
    const V rewards = {u"Full rewards for guests", u"Insight for bosses"};
    const V items = {u"Host's item rewards", u"Give the bells", u"Own key items as guest", u"Key items at once"};
    const V display = {u"Party HUD", u"Party name tags", u"Pings", u"Show party through walls", u"Bloodstain marker",
                       u"Above companions"};
    for (const ps::Role role : {ps::Role::Host, ps::Role::Guest}) {
        ps::ResetForTest(true);
        ps::SetRole(role);
        ps::SetHostRule(ps::HostRule::HostRevive, false);
        ps::SetHostRule(ps::HostRule::HostLimboRevive, true);
        ps::SetHostRule(ps::HostRule::KillRecord, true);
        ps::SetHostRule(ps::HostRule::Gather, false);
        ps::SetHostRule(ps::HostRule::NpcMirror, true);
        ps::SetHostRule(ps::HostRule::CovenantPerPlayer, true);
        {   // guestrules: the host's scaling differs from this player's own bbport.ini values
            const int host_scaling[ps::kScalingValues] = {300, 50, 200, 100, 20, 0};
            ps::SetHostScaling(host_scaling);
            BbSettings::Get().party_boss_hp = 150;
            BbSettings::Get().party_boss_damage = 10;
        }
        BbSettings::Get().party_host_revive = true;  // EverySettingSaved changed every row once
        BbSettings::Get().party_covenant_per_player = true;
        BbSettings::Get().party_gather = true;
        BbSettings::Get().party_kill_record = true;
        BbSettings::Get().party_guests_bring_lamps = false;
        BbSettings::Get().party_travel_vote = false;
        P::Load();
        Advance(300);
        assert(names(u"Party rules: World") == shared);
        assert(names(u"Party rules: PvP & lamps") == world);
        assert(names(u"Party rules: Deaths") == deaths);
        assert(names(u"Party rules: Rot") == rot);
        assert(names(u"Party rules: Enemies").size() == 6);
        assert(names(u"Party rules: Travel") == travel);
        assert(names(u"My party: Your save") == save);
        assert(names(u"My party: Your items") == items);
        assert(names(u"Party rules: Rewards") == rewards);
        assert(names(u"My party: Display") == display);
        assert(names(u"My party: Look & notices") == look);
        const bool guest = role == ps::Role::Guest;
        // Every row of a Party rules page is a host's rule (a toggle for the host, a read-only line
        // for a guest; the Enemies sliders: the host's values); every My party row a toggle / slider /
        // list for everyone.
        // guestrules (0.7.3): the enemy scaling is the host's rule: sliders for the host, read-only
        // lines with the host's values for a guest (its own bbport.ini values are kept).
        assert(names(u"Party rules: Enemies") == (V{u"Boss health", u"Boss damage", u"Enemy health", u"Enemy damage",
                                             u"Boss poise", u"Enemy poise"}));
        for (const P::Kind kind : kinds(u"Party rules: Enemies")) assert(kind == (guest ? P::Kind::Button : P::Kind::Slider));
        if (guest) {
            const auto scale = P::ShownRows(PageNamed(u"Party rules: Enemies"));
            assert(std::u16string(P::Text(scale[0]->choices[0], false)) == u"300 %");
            assert(std::u16string(P::Text(scale[1]->choices[0], false)) == u"+50 %");
            assert(std::u16string(P::Text(scale[2]->choices[0], false)) == u"100 %");
            assert(std::u16string(P::Text(scale[0]->help, false)).find(u"The host's setting: 300 %") == 0);
            assert(std::u16string(P::Text(scale[1]->help, true)).find(u"Настройка хоста") == 0);
            for (const P::Row* row : scale) {  // a press changes nothing (a read-only line)
                assert(P::IsButton(row->field));
                Press(*row);
            }
            Advance(16);
            const std::string line = P::ShownSummary(PageNamed(u"Party rules: Enemies"));
            assert(line.find("Boss health=300 %") != std::string::npos);
            assert(BbSettings::Get().party_boss_hp == 150 && BbSettings::Get().party_boss_damage == 10);
        }
        const P::Kind host_kind = guest ? P::Kind::Button : P::Kind::Toggle;
        for (const char16_t* page : {u"Party rules: World", u"Party rules: PvP & lamps", u"Party rules: Deaths",
                                     u"Party rules: Rot", u"Party rules: Travel"})
            for (const P::Kind kind : kinds(page)) assert(kind == host_kind);
        for (const char16_t* page : {u"My party: Your save", u"My party: Your items", u"My party: Display",
                                     u"My party: Look & notices"})
            for (const P::Kind kind : kinds(page)) assert(kind != P::Kind::Button);
        for (const P::Kind kind : kinds(u"Party rules: Rewards")) assert(kind == host_kind || kind == P::Kind::List);
        assert(kinds(u"Party rules: Rewards").back() == (guest ? P::Kind::Button : P::Kind::List) &&
               kinds(u"My party: Display").back() == P::Kind::List);
        // The group's pages follow each other in the System menu: Party rules, then My party.
        {
            std::vector<std::u16string> order;
            for (const P::Page& page : P::All()) order.push_back(P::Text(page.name, false));
            const auto at = [&](const char16_t* n) { return std::find(order.begin(), order.end(), n) - order.begin(); };
            const auto first = at(u"Party rules: World");
            const char16_t* group[] = {u"Party rules: World", u"Party rules: PvP & lamps", u"Party rules: Deaths",
                                       u"Party rules: Travel", u"Party rules: Rewards", u"Party rules: Rot",
                                       u"Party rules: Enemies", u"Party rules: Phantoms", u"My party: Your save",
                                       u"My party: Your items", u"My party: Display", u"My party: Look & notices"};
            for (std::size_t i = 0; i < std::size(group); ++i) assert(at(group[i]) == first + long(i));
        }
        const std::string shared_line = P::ShownSummary(PageNamed(u"Party rules: World"));
        assert(shared_line.find("Host can gather=" + std::string(guest ? "Off" : "On")) != std::string::npos);
        assert(shared_line.find("Enemies stay dead=On") != std::string::npos);
        // 0.7.3: covenant outcomes per player, the host's rule, on by default (a guest: the host's value).
        assert(shared_line.find("Covenants per player=On") != std::string::npos);
        // 0.7.3: Party votes on travel, the host's rule, off by default (a guest: the host's value).
        assert(shared_line.find("Party votes on travel=Off") != std::string::npos);
        // 0.7.2: Guests bring their lamps, the host's rule, off by default (a guest: the host's value).
        const std::string world_line = P::ShownSummary(PageNamed(u"Party rules: PvP & lamps"));
        assert(world_line.find("Guests bring their lamps=Off") != std::string::npos);
        // The trace's page line (smoke_party "rules" asserts it): every row, its value, nothing cut.
        for (const P::Page& page : P::All()) assert(P::ShownSummary(page).find("(CUT)") == std::string::npos);
        const std::string deaths_line = P::ShownSummary(PageNamed(u"Party rules: Deaths"));
        assert(deaths_line.find("Host gets up, no load=" + std::string(guest ? "Off" : "On")) != std::string::npos);
        assert(P::ShownSummary(PageNamed(u"Party rules: Rewards")).find("Insight for bosses=") != std::string::npos);
        if (guest) {
            const auto shown = P::ShownRows(PageNamed(u"Party rules: Deaths"));
            assert(std::u16string(P::Text(shown[0]->choices[0], false)) == u"Off");
            assert(std::u16string(P::Text(shown[1]->choices[0], true)) == u"Вкл");
            assert(std::u16string(P::Text(shown[0]->help, false)).find(u"The host decides") == 0);
            // 0.7.2: rot as the host has it (off: the default) and its cleanse rules (on), read-only.
            const auto rot_rows = P::ShownRows(PageNamed(u"Party rules: Rot"));
            assert(std::u16string(P::Text(rot_rows[0]->choices[0], false)) == u"Off");
            assert(std::u16string(P::Text(rot_rows[0]->help, false)).find(u"The host has rot off") == 0);
            assert(std::u16string(P::Text(rot_rows[2]->choices[0], false)) == u"On");
            assert(std::u16string(P::Text(rot_rows[2]->help, false)).find(u"The host's rule: the enemy") == 0);
            const auto world = P::ShownRows(PageNamed(u"Party rules: World"));
            assert(std::u16string(P::Text(world[1]->help, false)).find(u"The host turned gathering off") == 0);
            assert(std::u16string(P::Text(world[0]->help, false)).find(u"The host's rule") == 0);
            assert(std::u16string(P::Text(world[4]->help, false)).find(u"The host decides; off") == 0);
        }
    }
    // A row writes its bbport.ini key (the launcher reads the same key).
    auto& s = BbSettings::Get();
    ps::ResetForTest(true);
    ps::SetRole(ps::Role::Host);
    s.party_host_limbo_revive = true; // EverySettingSaved changed every row once
    P::Load();
    const P::Row& limbo = RowNamed(PageNamed(u"Party rules: Deaths"), u"Up after boss kill");
    assert(s.party_host_limbo_revive && *P::Byte(limbo.field) == 1);
    *P::Byte(limbo.field) = 0;
    Advance(16);
    assert(!s.party_host_limbo_revive && ReadIni()["party_host_limbo_revive"] == "0");
    *P::Byte(limbo.field) = 1;
    Advance(16);
    assert(s.party_host_limbo_revive && ReadIni()["party_host_limbo_revive"] == "1");
    // 0.7.1: the new rows write their keys too (the launcher's Party Rules tab reads them).
    for (const auto& [page, row, key, member] :
         {std::tuple{u"Party rules: World", u"Enemies stay dead", "party_kill_record", &BbSettings::Values::party_kill_record},
          std::tuple{u"Party rules: Rot", u"Rot: boss kill cleanses", "party_rot_cleanse_boss",
                     &BbSettings::Values::party_rot_cleanse_boss},
          std::tuple{u"Party rules: Rot", u"Rot: kill your killer", "party_rot_cleanse_killer",
                     &BbSettings::Values::party_rot_cleanse_killer},
          std::tuple{u"Party rules: Rot", u"Rot: echoes back", "party_rot_cleanse_echoes",
                     &BbSettings::Values::party_rot_cleanse_echoes},
          std::tuple{u"Party rules: World", u"Host can gather", "party_gather", &BbSettings::Values::party_gather},
          std::tuple{u"Party rules: World", u"NPCs as in host's world", "party_npc_mirror", &BbSettings::Values::party_npc_mirror},
          std::tuple{u"Party rules: World", u"Party votes on travel", "party_travel_vote", &BbSettings::Values::party_travel_vote},
          std::tuple{u"Party rules: World", u"NPC hunters fight all", "party_npc_fights", &BbSettings::Values::party_npc_fights},
          std::tuple{u"Party rules: Deaths", u"Anyone can be abducted", "party_any_kidnap",
                     &BbSettings::Values::party_any_kidnap},
          std::tuple{u"Party rules: Phantoms", u"Skip the prologue", "party_skip_prologue", &BbSettings::Values::party_skip_prologue},
          std::tuple{u"Party rules: Phantoms", u"Level sync", "party_level_sync", &BbSettings::Values::party_level_sync},
          std::tuple{u"Party rules: Phantoms", u"Phantom health cut", "party_phantom_hp",
                     &BbSettings::Values::party_phantom_hp},
          std::tuple{u"My party: Your items", u"Own key items as guest", "party_key_view", &BbSettings::Values::party_key_view},
          std::tuple{u"My party: Your items", u"Key items at once", "party_key_items_summoned",
                     &BbSettings::Values::party_key_items_summoned}}) {
        s.*member = true;
        P::Load();
        const P::Row& r = RowNamed(PageNamed(page), row);
        assert(*P::Byte(r.field) == 1);
        *P::Byte(r.field) = 0;
        Advance(16);
        assert(!(s.*member) && ReadIni()[key] == "0");
        *P::Byte(r.field) = 1;
        Advance(16);
        assert((s.*member) && ReadIni()[key] == "1");
    }
    const P::Row* insight_row = nullptr;  // the host's list (the guest's read-only line has the same name)
    for (const P::Row& row : PageNamed(u"Party rules: Rewards").rows)
        if (row.kind == P::Kind::List) insight_row = &row;
    assert(insight_row);
    const P::Row& insight = *insight_row;
    *P::Value(insight.field) = 2;
    Advance(16);
    assert(ReadIni()["party_guest_insight"] == "0");
    *P::Value(insight.field) = 0;
    Advance(16);
    assert(ReadIni()["party_guest_insight"] == "parity");
    ps::ResetForTest(false);
    P::Load();
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
    RulesPages();
    Restart();
    Quit();
    std::filesystem::remove(ini);
    std::printf("game menu pages: %zu pages, %d fields, %zu texts: ok\n", P::All().size(), P::FieldCount(),
                P::TextCount());
    return 0;
}
