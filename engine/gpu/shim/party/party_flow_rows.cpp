// SPDX-License-Identifier: GPL-3.0-or-later
// The party flow's rows on the game menu's Party page (System > Party, also on the title screen):
// registered through the page's action table (BbGameMenu::Pages::AddPartyRow); the page layout,
// the row kinds and the press handling are the game menu's (bbport_game_menu_pages.h). What the
// rows do is party_flow.h.
//
//   single-player process, title:  New party game, Join party game, Continue party game (once the
//                                  party saves hold a character), Host address, Password, Party
//                                  name (no Status line: six rows, the page's limit)
//   single-player process, world:  the status line explains how to start a party game
//   party process:                 Party page: the status (role, before the link runs too);
//                                  Party mode page: Host party / Join party (the other role, or
//                                  again after Leave), Single player (back to the single-player game)
// Every row is a button (one value, no arrows; Cross runs it); the ones that restart the game ask
// for a second press (the help line says so).
#include "party_flow.h"
#include "party_code.h"

#include "../bbport_game_menu_pages.h"

#include <string>

namespace party::flow {

namespace {

namespace P = BbGameMenu::Pages;
using Texts = P::Texts;
using Board = party::status::Board;

Texts T(const char* en, const char* ru) { return {en, ru}; }

/// A value column text of at most P::MaxValueLength units (ASCII here).
std::string Short(const std::string& s) {
    if (s.size() <= P::MaxValueLength) return s;
    return s.substr(0, P::MaxValueLength - 3) + "...";
}

/// The row's help, followed by the last action's outcome when there is one.
Texts WithMessage(Texts t) {
    const std::string m = LastMessage();
    if (!m.empty()) {
        t.en = m;
        t.ru = m;  // the outcome lines are English only (they also go to the log)
    }
    return t;
}

bool SoloTitle() {
    const Context c = CurrentContext();
    return !c.party_process && !c.in_world;
}
bool Party() { return CurrentContext().party_process; }

}  // namespace

void RegisterMenuRows() {
    using Row = P::PartyRow;
    if (!Enabled()) return;

    // The Status line: not on the solo title (the rows below fill its six); in a party process
    // before the link runs (the title, the load) it says which party game this is instead of
    // "Not running".
    P::PartyStatusHooks hooks;
    hooks.hidden = [] { return SoloTitle(); };
    hooks.describe = [](const Board& b, Texts* value, Texts* help) {
        const Context c = CurrentContext();
        if (!c.party_process || b.state != party::status::State::Off) return false;
        if (c.in_world) {  // after Leave
            *value = T("Not in a party", "Не в группе");
            *help = WithMessage(T("Party mode page: host again, join a host, or back to single player",
                                  "Страница «Режим группы»: снова хост, вход к хосту или одиночная игра"));
        } else if (c.host) {
            *value = T("Host, not open yet", "Хост, ещё не открыта");
            *help = WithMessage(T("Party game as host (party saves): the party opens with your hunter in the world",
                                  "Игра группой, хост (сохранения группы): группа откроется, когда охотник в мире"));
        } else {
            const Saved s = Current();
            *value = T("Guest, not joined yet", "Гость, ещё не вошли");
            *help = WithMessage(s.host_address.empty()
                                    ? T("Party game as guest: no host address set (title screen, Party)",
                                        "Игра группой, гость: адрес хоста не задан (титульный экран, Группа)")
                                    : Texts{"Party game as guest: joins " + s.host_address + " with your hunter in the world",
                                            "Игра группой, гость: вход к " + s.host_address + ", когда охотник в мире"});
        }
        return true;
    };
    P::SetPartyStatusHooks(std::move(hooks));

    Row fresh;
    fresh.order = 12;
    fresh.name = [](const Board&) { return T("New party game", "Новая игра группой"); };
    fresh.value = [](const Board&) { return T("Host", "Хост"); };
    fresh.help = [](const Board&) {
        return WithMessage(T("Press twice: restart as the party's host with the party saves, then New Game",
                             "Нажмите дважды: перезапуск хостом группы на сохранениях группы, затем Новая игра"));
    };
    fresh.visible = [](const Board&) { return SoloTitle(); };
    fresh.press = Row::Press::Confirm;
    fresh.run = [](const Board&) { Run(Action::NewParty); };
    P::AddPartyRow(fresh);

    Row join;
    join.order = 14;
    join.name = [](const Board&) { return T("Join party game", "Присоединиться"); };
    join.value = [](const Board&) {
        const Saved s = Current();
        return s.host_address.empty() ? T("Set address first", "Сначала адрес") : T("Join", "Войти");
    };
    join.help = [](const Board&) {
        const Saved s = Current();
        return WithMessage(s.has_character
                               ? T("Press twice: restart and join the saved host with your party character",
                                   "Нажмите дважды: перезапуск и вход к хосту с персонажем группы")
                               : T("Press twice: restart and join the saved host; then New Game for your hunter",
                                   "Нажмите дважды: перезапуск и вход к хосту; затем Новая игра"));
    };
    join.visible = [](const Board&) { return SoloTitle(); };
    join.press = Row::Press::Confirm;
    join.run = [](const Board&) { Run(Action::JoinParty); };
    P::AddPartyRow(join);

    Row cont;
    cont.order = 16;
    cont.name = [](const Board&) { return T("Continue party game", "Продолжить группой"); };
    cont.value = [](const Board&) {
        return Current().role == "join" ? T("As guest", "Гостем") : T("As host", "Хостом");
    };
    cont.help = [](const Board&) {
        return WithMessage(T("Press twice: restart with the party saves and continue your party character",
                             "Нажмите дважды: перезапуск на сохранениях группы и продолжение персонажа группы"));
    };
    cont.visible = [](const Board&) { return SoloTitle() && Current().has_character; }; // party.json or the party saves
    cont.press = Row::Press::Confirm;
    cont.run = [](const Board&) { Run(Action::ContinueParty); };
    P::AddPartyRow(cont);

    Row addr;
    addr.order = 17;
    addr.name = [](const Board&) { return T("Host address", "Адрес хоста"); };
    addr.value = [](const Board&) {
        const Saved s = Current();
        return s.host_address.empty() ? T("Not set", "Не задан") : Texts{Short(s.host_address), Short(s.host_address)};
    };
    addr.help = [](const Board&) {
        return WithMessage(T("Press to type the host's party code, like " PARTY_CODE_EXAMPLE ", or host:port (Ctrl+V pastes)",
                             "Нажмите, чтобы ввести код группы от хоста, например " PARTY_CODE_EXAMPLE
                             ", или хост:порт (Ctrl+V вставляет)"));
    };
    addr.visible = [](const Board&) { return SoloTitle(); };
    addr.press = Row::Press::Once;
    addr.run = [](const Board&) { BeginEdit(Field::Address); };
    P::AddPartyRow(addr);

    Row pass;
    pass.order = 18;
    pass.name = [](const Board&) { return T("Password", "Пароль"); };
    pass.value = [](const Board&) { return Current().password.empty() ? T("None", "Нет") : T("Set", "Задан"); };
    pass.help = [](const Board&) {
        return WithMessage(T("Press to type the party's password (the same for all); saved protected for this Windows user",
                             "Нажмите, чтобы ввести пароль группы (у всех одинаковый); хранится защищённым"));
    };
    pass.visible = [](const Board&) { return SoloTitle(); };
    pass.press = Row::Press::Once;
    pass.run = [](const Board&) { BeginEdit(Field::Password); };
    P::AddPartyRow(pass);

    Row name;
    name.order = 19;
    name.name = [](const Board&) { return T("Party name", "Имя в группе"); };
    name.value = [](const Board&) {
        const Saved s = Current();
        return s.name.empty() ? T("Automatic", "Автоматически") : Texts{Short(s.name), Short(s.name)};
    };
    name.help = [](const Board&) {
        return WithMessage(T("Press to type your name in the party (1-16 of A-Z a-z 0-9 _ -); each player needs their own",
                             "Нажмите, чтобы ввести имя в группе (1-16 из A-Z a-z 0-9 _ -); у каждого своё"));
    };
    name.visible = [](const Board&) { return SoloTitle(); };
    name.press = Row::Press::Once;
    name.run = [](const Board&) { BeginEdit(Field::Name); };
    P::AddPartyRow(name);

    Row host;
    host.order = 25;
    host.name = [](const Board&) { return T("Host party", "Стать хостом"); };
    host.value = [](const Board&) { return CurrentContext().host ? T("Host again", "Снова хостом") : T("Switch", "Сменить"); };
    host.help = [](const Board&) {
        return WithMessage(CurrentContext().host
                               ? T("Press twice: host the party again", "Нажмите дважды: снова открыть группу")
                               : T("Press twice: restart as the host (your party character continues)",
                                   "Нажмите дважды: перезапуск хостом (персонаж группы продолжает)"));
    };
    host.visible = [](const Board&) {
        const Context c = CurrentContext();
        return c.party_process && (!c.host || !c.link_active);
    };
    host.page = 1;
    host.press = Row::Press::Confirm;
    host.run = [](const Board&) { Run(Action::HostParty); };
    P::AddPartyRow(host);

    Row guest;
    guest.order = 26;
    guest.name = [](const Board&) { return T("Join party", "Войти в группу"); };
    guest.value = [](const Board&) { return CurrentContext().host ? T("Switch", "Сменить") : T("Join again", "Снова войти"); };
    guest.help = [](const Board&) {
        const Saved s = Current();
        if (s.host_address.empty())
            return WithMessage(T("No host address saved: set it on the title screen (System > Party)",
                                 "Адрес хоста не задан: укажите его на титульном экране"));
        return WithMessage(Texts{"Press twice: join " + s.host_address, "Нажмите дважды: войти к " + s.host_address});
    };
    guest.visible = [](const Board&) {
        const Context c = CurrentContext();
        return c.party_process && (c.host || !c.link_active);
    };
    guest.page = 1;
    guest.press = Row::Press::Confirm;
    guest.run = [](const Board&) { Run(Action::JoinParty); };
    P::AddPartyRow(guest);

    Row solo;
    solo.order = 45;
    solo.name = [](const Board&) { return T("Single player", "Одиночная игра"); };
    solo.value = [](const Board&) { return T("Switch", "Перейти"); };
    solo.help = [](const Board&) {
        return WithMessage(T("Press twice: leave and restart into the single-player game and saves",
                             "Нажмите дважды: выйти и перезапустить одиночную игру и её сохранения"));
    };
    solo.visible = [](const Board&) { return Party(); };
    solo.page = 1;
    solo.press = Row::Press::Confirm;
    solo.run = [](const Board&) { Run(Action::SinglePlayer); };
    P::AddPartyRow(solo);
}

}  // namespace party::flow
