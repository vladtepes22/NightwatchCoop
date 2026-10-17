// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_game_menu_pages.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

#include "bbport_settings.h"
#include "party/party_balance_rules.h"
#include "party/party_link.h"  // kMaxPartyPlayers
#include "party/party_npctalk.h"
#include "party/party_status.h"

namespace BbGameMenu::Pages {
namespace {

// ---- Texts ----
struct Dynamic {
    std::size_t limit = MaxValueLength;
    std::array<char16_t, DynamicCapacity> buffer[2]{}; ///< English, Russian
};
struct TextEntry {
    std::u16string english, russian;
    int dynamic = -1; ///< index into `dynamics`, or -1
};
std::vector<TextEntry> texts;
std::vector<std::unique_ptr<Dynamic>> dynamics; // unique_ptr: buffers never move

u32 AddText(const char* english, const char* russian) {
    texts.push_back({Utf16(english), Utf16(russian), -1});
    return IdBase + u32(texts.size() - 1);
}

u32 AddDynamic(std::size_t limit) {
    auto d = std::make_unique<Dynamic>();
    d->limit = std::min(limit, DynamicCapacity - 1);
    dynamics.push_back(std::move(d));
    texts.push_back({{}, {}, int(dynamics.size() - 1)});
    return IdBase + u32(texts.size() - 1);
}

/// Writes a run-time text in place (the game may hold the pointer): cut to the text's limit
/// with an ellipsis, rewritten only when it changed.
std::atomic<std::uint64_t> dynamic_changes{0};

void SetDynamic16(u32 id, const std::u16string& english, const std::u16string& russian) {
    Dynamic& d = *dynamics[std::size_t(texts[id - IdBase].dynamic)];
    for (int lang = 0; lang < 2; ++lang) {
        std::u16string text = lang ? russian : english;
        if (text.size() > d.limit) {
            text.resize(d.limit - 3);
            if (!text.empty() && text.back() >= 0xd800 && text.back() < 0xdc00) {
                text.pop_back(); // half a surrogate pair
            }
            text += u"..."; // the game's fonts: plain ASCII is safe
        }
        auto& buffer = d.buffer[lang];
        if (std::u16string_view(buffer.data()) == text) {
            continue;
        }
        buffer[text.size()] = 0; // shorter first: a reader never runs past the new end
        std::copy(text.begin(), text.end(), buffer.begin());
        buffer[text.size()] = 0;
        dynamic_changes.fetch_add(1, std::memory_order_release);
    }
}
void SetDynamic(u32 id, const std::string& english, const std::string& russian) {
    SetDynamic16(id, Utf16(english.c_str()), Utf16(russian.c_str()));
}

// ---- Fields: what the rows edit ----
using Values = BbSettings::Values;
struct Field {
    Kind kind = Kind::List;
    int steps = 0; ///< Steps rows: the number of choices
    std::function<int(const Values&)> get;          ///< null: a button (0 when the page opens)
    std::function<void(int)> set;                    ///< applies a setting
    std::function<void()> run;                       ///< a button's action (null: a read-only line)
    bool confirm = false;  ///< a button that arms on the first press and runs on the second
    bool debounce = false; ///< applied once the value rests for 0.4 s (re-creates targets)
};
std::vector<Field> fields;
std::vector<int> values, defaults, applied, resting;
std::vector<u8> bytes, default_bytes;
std::vector<std::int64_t> rested_since, armed_at;
std::atomic<bool> loaded{false};
/// Counts the values taken from the settings while a page may be open (changed elsewhere: the
/// overlay menu, bbport.ini): the glue moves the rows' widgets to them.
std::atomic<std::uint64_t> value_changes{0};
std::atomic<std::int64_t> last_refresh{-1000000};
void (*restart_handler)() = nullptr;
void (*quit_handler)() = nullptr;

Values& DefaultSettings() {
    static Values* v = [] {
        auto* d = new Values;
        for (int e = 0; e < BbSettings::EffectCount; ++e) {
            d->effects[e] = BbSettings::Effects[e].default_on;
        }
        return d;
    }();
    return *v;
}

int AddField(Field f) {
    fields.push_back(std::move(f));
    return int(fields.size() - 1);
}

int Setting(Kind kind, std::function<int(const Values&)> get, std::function<void(int)> set,
            int steps = 0) {
    Field f;
    f.kind = kind;
    f.get = std::move(get);
    f.set = std::move(set);
    f.steps = steps;
    return AddField(std::move(f));
}

int Flag(std::atomic<bool> Values::*member) {
    return Setting(
        Kind::Toggle, [member](const Values& v) { return int((v.*member).load()); },
        [member](int i) { (BbSettings::Get().*member) = i != 0; });
}

/// A 0..10 slider over an 11-entry table of a float setting.
int Scaled(std::atomic<float> Values::*member, const float* table) {
    return Setting(
        Kind::Slider, [member, table](const Values& v) { return int(NearestPosition(table, (v.*member).load())); },
        [member, table](int i) { (BbSettings::Get().*member) = table[std::clamp(i, 0, 10)]; });
}

/// A button (Kind::Button): `fn` on every press (Press).
int Button(std::function<void()> fn) {
    Field f;
    f.kind = Kind::Button;
    f.run = std::move(fn);
    return AddField(std::move(f));
}

/// A confirmed button: the first press arms it (its help line says "press again to confirm"),
/// a second press within ConfirmMs runs `fn`.
constexpr std::int64_t ConfirmMs = 10000;
int Confirmed(std::function<void()> fn) {
    Field f;
    f.kind = Kind::Button;
    f.run = std::move(fn);
    f.confirm = true;
    return AddField(std::move(f));
}

/// A read-only line: a button without an action (one value, no arrows).
int Info() {
    Field f;
    f.kind = Kind::Button;
    return AddField(std::move(f));
}

bool Armed(int field, std::int64_t now) {
    return now - armed_at[std::size_t(field)] <= ConfirmMs;
}

// ---- Pages ----
std::vector<Page> pages;
u32 split_text[1] = {0};
u32 notice_text[1] = {0};
u32 ending_text[1] = {0};

/// A confirmed button's help line: the row's own help, or "press again to confirm" while armed
/// (a run-time text of its own wrapping the row's).
struct ConfirmHelp {
    int field = 0;
    u32 base = 0, shown = 0;
};
std::vector<ConfirmHelp> confirm_helps;
u32 HelpFor(int field, u32 help);

void AddPage(const char* en, const char* ru, const char* help_en, const char* help_ru) {
    const u32 name = AddText(en, ru);
    pages.push_back({name, AddText(help_en, help_ru), {}});
}
void AddRow(int field, u32 name, u32 help, std::vector<u32> choices, Kind kind,
            std::function<bool()> visible = nullptr) {
    pages.back().rows.push_back({field, name, HelpFor(field, help), std::move(choices), kind, std::move(visible)});
}
void AddRow(int field, const char* en, const char* ru, const char* help_en, const char* help_ru,
            std::vector<u32> choices, Kind kind) {
    AddRow(field, AddText(en, ru), AddText(help_en, help_ru), std::move(choices), kind);
}

u32 HelpFor(int field, u32 help) {
    if (!fields[std::size_t(field)].confirm) return help;
    const u32 shown = AddDynamic(MaxHelpLength);
    confirm_helps.push_back({field, help, shown});
    return shown;
}
/// The confirmed buttons' help lines: "press again" while armed, else the row's own help.
void RefreshConfirmHelps() {
    const std::int64_t now = last_refresh.load();
    for (const ConfirmHelp& h : confirm_helps) {
        if (Armed(h.field, now)) {
            SetDynamic(h.shown, "Press again to confirm", "Нажмите ещё раз для подтверждения");
        } else {
            const char16_t* en = Text(h.base, false);
            const char16_t* ru = Text(h.base, true);
            SetDynamic16(h.shown, en ? en : u"", ru ? ru : u"");
        }
    }
}

constexpr float SharpnessStep = 0.2f; // Display's slider: 0 off, 1..10 -> 0.2 .. 2.0
constexpr int LodValues[] = {-2, 0, 1, 2};
const float OpacityTable[11] = {0.10f, 0.20f, 0.30f, 0.40f, 0.50f, 0.60f, 0.70f, 0.75f, 0.80f, 0.90f, 1.00f};
const float ScaleTable[11] = {0.50f, 0.60f, 0.70f, 0.80f, 0.90f, 1.00f, 1.20f, 1.40f, 1.60f, 1.80f, 2.00f};
const float SensTable[11] = {0.25f, 0.40f, 0.50f, 0.60f, 0.80f, 1.00f, 1.25f, 1.50f, 2.00f, 3.00f, 5.00f};
const float SmoothTable[11] = {0.0f, 0.05f, 0.10f, 0.15f, 0.20f, 0.30f, 0.40f, 0.50f, 0.60f, 0.70f, 0.80f};
const float DeadTable[11] = {0.0f, 0.01f, 0.02f, 0.03f, 0.04f, 0.05f, 0.06f, 0.08f, 0.10f, 0.15f, 0.20f};
const float MaskScaleTable[11] = {0.0f, 0.25f, 0.50f, 0.75f, 1.00f, 1.25f, 1.50f, 2.00f, 2.50f, 3.00f, 4.00f};
const float BoostTable[11] = {1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f, 1.8f, 2.0f, 2.5f, 3.0f};
const float TenthsTable[11] = {0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f};
// HDR output (bbport_hdr.h): peak 0 = Auto (the display's), paper white, strength, saturation.
const float HdrPeakTable[11] = {0.0f, 400.0f, 600.0f, 800.0f, 1000.0f, 1200.0f, 1500.0f, 2000.0f, 2500.0f, 3000.0f, 4000.0f};
const float HdrPaperTable[11] = {80.0f, 100.0f, 120.0f, 150.0f, 180.0f, 200.0f, 250.0f, 300.0f, 350.0f, 400.0f, 500.0f};
const float HdrStrengthTable[11] = {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f};
const float HdrSaturationTable[11] = {0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f};

bool UpscalerAvailable(int i) {
    const auto& s = BbSettings::Get();
    return i == BbSettings::UpscalerFsr4     ? s.fsr4_supported.load()
           : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
           : i == BbSettings::UpscalerDlss   ? s.dlss_supported.load()
                                             : true;
}

int upscaler_field = -1;

// ---- Party (party_status.h): the run-time texts and the members the rows stand for ----
constexpr int MaxMembers = party::kMaxPartyPlayers; // the host and the guests (party_link.h)
struct MemberTexts {
    u32 member_name[MaxMembers], member_value[MaxMembers], member_help[MaxMembers];
    u32 restart_help;
    u32 event_value, event_help;
    u32 mode_none;
} party;
/// The HDR page's run-time help lines (whether HDR runs, and why not; the automatic peak).
struct HdrTexts {
    u32 status_help = 0, peak_help = 0;
} hdr_texts;
struct BalanceTexts {
    u32 rot_help = 0;
    u32 scale_help[6]{};
    u32 npc_range_help = 0;
    u32 ff_help = 0, ff_value = 0, ff_guest_help = 0;
    u32 lamps_help = 0, lamps_value = 0, lamps_guest_help = 0;
} balance;
std::string member_names[MaxMembers]; // the member row k stands for (kick)
bool member_kickable[MaxMembers]{};
int member_rows = 1;                  // rows the Members page shows (one when nobody is there)

/// The Party page: its action table (built-in rows and AddPartyRow's), each with its run-time
/// texts and the field its game row edits.
struct PartyEntry {
    PartyRow spec;
    u32 name = 0, help = 0, value = 0;
    int field = -1;
};
PartyStatusHooks status_hooks;
std::vector<PartyRow> pending_party_rows; // AddPartyRow before Define
std::vector<PartyEntry> party_rows;       // sorted by order, fixed after Define
std::vector<char> party_row_visible;      // per entry, at the last refresh

const char* L(bool ru, const char* en, const char* ru_text) {
    return ru ? ru_text : en;
}

std::string StateShort(party::status::State st, bool ru) {
    using party::status::State;
    switch (st) {
    case State::Off: return L(ru, "Not running", "Не запущена");
    case State::Starting: return L(ru, "Starting", "Запуск");
    case State::Hosting: return L(ru, "Waiting for hunters", "Ждём охотников");
    case State::Connecting: return L(ru, "Connecting", "Подключение");
    case State::WaitingForWorld: return L(ru, "Loading the world", "Загрузка мира");
    case State::RingingBell: return L(ru, "Ringing the bell", "Звон колокола");
    case State::Joined: return L(ru, "Together", "Вместе");
    case State::Travelling: return L(ru, "Travelling", "Переход");
    case State::Reconnecting: return L(ru, "Reconnecting", "Переподключение");
    case State::Error: return L(ru, "Error", "Ошибка");
    }
    return "";
}

std::string StateLong(party::status::State st, party::status::Role role, bool ru) {
    using party::status::State;
    switch (st) {
    case State::Hosting: return L(ru, "Hosting: waiting for hunters", "Хост: ожидание охотников");
    case State::Connecting: return L(ru, "Connecting to the host", "Подключение к хосту");
    case State::WaitingForWorld: return L(ru, "Waiting for the world to load", "Ожидание загрузки мира");
    case State::RingingBell:
        return role == party::status::Role::Host
                   ? L(ru, "Ringing the Beckoning Bell", "Звон манящего колокола")
                   : L(ru, "Ringing the Small Resonant Bell", "Звон малого резонирующего колокола");
    case State::Travelling: return L(ru, "Travelling with the party", "Переход вместе с группой");
    case State::Reconnecting: return L(ru, "Connection lost: reconnecting", "Связь потеряна: переподключение");
    default: return StateShort(st, ru);
    }
}

void RefreshParty() {
    namespace ps = party::status;
    const ps::Board b = ps::Snapshot();
    const bool on = b.enabled;
    const bool host = b.role == ps::Role::Host;
    std::string en, ru;
    const auto both = [&](const auto& make) {
        en = make(false);
        ru = make(true);
    };
    for (std::size_t i = 0; i < party_rows.size(); ++i) {
        PartyEntry& e = party_rows[i];
        const PartyRow& r = e.spec;
        const Texts name = r.name ? r.name(b) : Texts{};
        const Texts help = r.help ? r.help(b) : Texts{};
        const Texts value = r.value ? r.value(b) : Texts{"-", "-"};
        SetDynamic(e.name, name.en, name.ru);
        SetDynamic(e.help, help.en, help.ru);
        SetDynamic(e.value, value.en, value.ru);
        party_row_visible[i] = !r.visible || r.visible(b);
    }

    SetDynamic(party.mode_none, "Single player", "Одиночная игра");

    // Last event (Party members page).
    both([&](bool r) { return b.last_event.empty() ? std::string(L(r, "None", "Нет")) : b.last_event; });
    SetDynamic(party.event_value, en, ru);
    both([&](bool r) {
        return b.last_event.empty() ? std::string(L(r, "No party events yet", "Событий группы ещё не было"))
                                    : std::string(L(r, "Last: ", "Последнее: ")) + b.last_event;
    });
    SetDynamic(party.event_help, en, ru);

    // Members.
    member_rows = std::clamp(int(b.members.size()), 1, MaxMembers);
    for (int k = 0; k < MaxMembers; ++k) {
        if (k >= int(b.members.size())) {
            member_names[k].clear();
            member_kickable[k] = false;
            both([&](bool r) { return std::string(k == 0 ? L(r, "No one yet", "Пока никого") : "-"); });
            SetDynamic(party.member_name[k], en, ru);
            SetDynamic(party.member_value[k], "-", "-");
            both([&](bool r) {
                return std::string(on ? L(r, "Hunters appear here once they connect",
                                          "Охотники появятся здесь после подключения")
                                      : L(r, "Party mode is off", "Режим группы выключен"));
            });
            SetDynamic(party.member_help[k], en, ru);
            continue;
        }
        const ps::Member& m = b.members[std::size_t(k)];
        member_names[k] = m.name;
        member_kickable[k] = host && !m.local && m.slot != 0;
        both([&](bool r) {
            std::string text = m.name;
            if (m.slot == 0) text += L(r, " (host)", " (хост)");
            if (m.local) text += L(r, " (you)", " (вы)");
            return text;
        });
        SetDynamic(party.member_name[k], en, ru);
        const auto where = [&](bool r) {
            std::string text = !m.connected  ? L(r, "Connection lost", "Связь потеряна")
                               : m.slot == 0 ? L(r, "Host's world", "Мир хоста")
                               : m.in_world  ? L(r, "In world", "В мире") // short: the value column is narrow
                                             : L(r, "In own world", "В своём мире");
            if (m.connected && !m.local && m.ping_ms >= 0) {
                text += ", " + std::to_string(m.ping_ms) + L(r, " ms", " мс");
            }
            return text;
        };
        both(where);
        SetDynamic(party.member_value[k], en, ru);
        both([&](bool r) {
            std::string text = where(r);
            if (!m.connected) text += L(r, ", slot kept", ", место сохранено");
            if (!m.area.empty()) text += ", " + m.area;
            if (member_kickable[k]) {
                text += L(r, ". Press twice: kick", ". Нажмите дважды: исключить");
            }
            return text;
        });
        SetDynamic(party.member_help[k], en, ru);
    }
}

void RefreshRestart() {
    if (BbSettings::ResolutionNeedsRestart()) {
        SetDynamic(party.restart_help, "Changes wait for a restart: press twice to restart the game now",
                   "Изменения ждут перезапуска: нажмите дважды, чтобы перезапустить игру");
    } else {
        SetDynamic(party.restart_help, "Press twice: restart the game (title screen; progress is saved)",
                   "Нажмите дважды: перезапустить игру (титульный экран; прогресс сохранён)");
    }
}

Texts T2(const char* en, const char* ru) {
    return {en, ru};
}
bool Active(const party::status::Board& b) {
    return b.enabled && b.role != party::status::Role::Off;
}
/// The link runs (not after Leave): Copy code / Rejoin / Leave only then. After Leave the party
/// flow's Host party / Join party take over (live: the page then had seven rows, Join party showed
/// next to Rejoin the host and the sixth-row limit cut Single player).
bool Running(const party::status::Board& b) {
    return Active(b) && b.state != party::status::State::Off;
}

/// The copy rows: which code was copied last (0 Internet / the only one, 1 LAN) and when (the
/// Poll clock, ms): its row says "Copied" for a moment.
std::atomic<int> copied_which{-1};
std::atomic<std::int64_t> copied_at{0};
bool TwoCodes(const party::status::Board& b) {
    return b.role == party::status::Role::Host && !b.code.empty() && !b.lan_code.empty();
}
Texts CopyValue(const std::string& code, int which) {
    if (copied_which.load() == which && last_refresh.load() - copied_at.load() < 2500)
        return T2("Copied", "Скопирован");
    // Not the code itself: the value column shows ~15 characters of it (live), the help line all.
    return code.empty() ? T2("Not known yet", "Ещё неизвестен") : T2("Copy", "Копировать");
}

/// The Party page's own rows (orders 10..40): status, copy code (two rows for a host with an
/// Internet and a LAN code), rejoin, leave. The join/leave notices switch and the last event are
/// on the Party members page (room for the party flow's rows here).
void DefineBuiltinPartyRows() {
    namespace ps = party::status;
    using B = ps::Board;
    PartyRow status;
    status.order = 10;
    status.name = [](const B&) { return T2("Status", "Состояние"); };
    status.value = [](const B& b) {
        Texts value, help;
        if (status_hooks.describe && status_hooks.describe(b, &value, &help)) return value;
        return b.enabled ? Texts{StateShort(b.state, false), StateShort(b.state, true)}
                         : T2("Single player", "Одиночная игра");
    };
    status.help = [](const B& b) {
        Texts value, help;
        if (status_hooks.describe && status_hooks.describe(b, &value, &help)) return help;
        if (!b.enabled) {
            // The party flow's rows (party_flow_rows.cpp) start or join a party game on the title.
            return T2("Single-player saves. Party games start on the title screen (System > Party)",
                      "Одиночные сохранения. Игра группой начинается на титульном экране (Система > Группа)");
        }
        Texts t;
        for (bool r : {false, true}) {
            std::string text = b.role == ps::Role::Host    ? L(r, "Host (your world): ", "Хост (ваш мир): ")
                               : b.role == ps::Role::Guest ? L(r, "Guest: ", "Гость: ")
                                                           : "";
            text += StateLong(b.state, b.role, r);
            if (!b.detail.empty()) text += " (" + b.detail + ")";
            (r ? t.ru : t.en) = text;
        }
        return t;
    };
    status.visible = [](const B&) { return !(status_hooks.hidden && status_hooks.hidden()); };
    party_rows.push_back({status});

    // Copy the party code: one row per code, the value shows the code ("Copied" for a moment).
    // A host has two (party_status.h lan_code): Internet (friends elsewhere) and LAN (same network).
    PartyRow copy;
    copy.order = 20;
    copy.name = [](const B& b) {
        return TwoCodes(b) ? T2("Copy Internet code", "Копировать интернет-код") : T2("Copy party code", "Копировать код");
    };
    copy.value = [](const B& b) { return CopyValue(b.code, 0); };
    copy.help = [](const B& b) {
        if (b.code.empty()) return T2("The party code is not known yet", "Код группы ещё неизвестен");
        if (TwoCodes(b))
            return Texts{"Press: copy the code for friends elsewhere: " + b.code,
                         "Нажмите: копировать код для друзей из других сетей: " + b.code};
        return Texts{"Press: copy the party code: " + b.code, "Нажмите: копировать код группы: " + b.code};
    };
    copy.visible = [](const B& b) { return Running(b); }; // party flow: the solo title's rows instead
    copy.press = PartyRow::Press::Once;
    copy.run = [](const B& b) {
        if (!b.code.empty() && ps::RequestCopyCode()) {
            copied_which = 0;
            copied_at = last_refresh.load();
        }
    };
    party_rows.push_back({copy});

    PartyRow lan;
    lan.order = 21;
    lan.name = [](const B&) { return T2("Copy LAN code", "Копировать код LAN"); };
    lan.value = [](const B& b) { return CopyValue(b.lan_code, 1); };
    lan.help = [](const B& b) {
        return Texts{"Press: copy the code for players on your network: " + b.lan_code,
                     "Нажмите: копировать код для игроков в вашей сети: " + b.lan_code};
    };
    lan.visible = [](const B& b) { return Running(b) && TwoCodes(b); };
    lan.press = PartyRow::Press::Once;
    lan.run = [](const B& b) {
        if (TwoCodes(b) && ps::RequestCopyLanCode()) {
            copied_which = 1;
            copied_at = last_refresh.load();
        }
    };
    party_rows.push_back({lan});

    PartyRow rejoin;
    rejoin.order = 30;
    rejoin.name = [](const B& b) {
        return b.role == ps::Role::Host ? T2("Summon again", "Призвать снова") : T2("Rejoin the host", "Вернуться к хосту");
    };
    rejoin.value = [](const B& b) { return b.role == ps::Role::Host ? T2("Ring the bell", "Позвонить") : T2("Rejoin", "Вернуться"); };
    rejoin.help = [](const B& b) {
        return b.role == ps::Role::Host
                   ? T2("Press: ring the Beckoning Bell again for waiting hunters",
                        "Нажмите: снова позвонить в манящий колокол для ожидающих")
               : b.role == ps::Role::Guest
                   ? T2("Press: reconnect to the host now and ring the bell",
                        "Нажмите: переподключиться к хосту и позвонить в колокол")
                   : T2("Party mode is off", "Режим группы выключен");
    };
    rejoin.visible = [](const B& b) { return Running(b); };
    rejoin.press = PartyRow::Press::Once;
    rejoin.run = [](const B& b) {
        if (Active(b)) ps::RequestRejoin();
    };
    party_rows.push_back({rejoin});

    PartyRow leave;
    leave.order = 40;
    leave.name = [](const B&) { return T2("Leave the party", "Покинуть группу"); };
    leave.value = [](const B& b) { return b.role == ps::Role::Host ? T2("End the party", "Завершить") : T2("Leave", "Покинуть"); };
    leave.help = [](const B& b) {
        return b.role == ps::Role::Host
                   ? T2("Press twice: end the party, every guest returns home",
                        "Нажмите дважды: завершить группу, гости вернутся в свои миры")
               : b.role == ps::Role::Guest
                   ? T2("Press twice: return to your world, stop rejoining",
                        "Нажмите дважды: вернуться в свой мир и не возвращаться к хосту")
                   : T2("Party mode is off", "Режим группы выключен");
    };
    leave.visible = [](const B& b) { return Running(b); };
    leave.press = PartyRow::Press::Confirm;
    leave.run = [](const B& b) {
        if (Active(b)) ps::RequestLeave();
    };
    party_rows.push_back({leave});
}

void DefineAll() {
    const u32 off = AddText("Off", "Выкл"), on = AddText("On", "Вкл");
    const std::vector<u32> toggle = {off, on};
    auto& s = BbSettings::Get();
    (void)s;

    party.restart_help = AddDynamic(MaxHelpLength);

    // ---- Party (party_status.h): the action table's rows, on two pages: Party (status, codes,
    // rejoin, leave; the solo title's party game rows) and Party mode (switching role, single
    // player). Each keeps within the six rows in every state (tests/test_game_menu_rows.cpp). ----
    DefineBuiltinPartyRows();
    for (PartyRow& r : pending_party_rows) party_rows.push_back({std::move(r)});
    pending_party_rows.clear();
    std::stable_sort(party_rows.begin(), party_rows.end(),
                     [](const PartyEntry& a, const PartyEntry& b) { return a.spec.order < b.spec.order; });
    party_row_visible.assign(party_rows.size(), 1);
    for (std::size_t i = 0; i < party_rows.size(); ++i) {
        PartyEntry& e = party_rows[i];
        e.name = AddDynamic(MaxNameLength);
        e.help = AddDynamic(MaxHelpLength);
        e.value = AddDynamic(MaxValueLength);
        const PartyRow& r = e.spec;
        if (r.toggle) {
            e.field = Setting(
                Kind::Toggle, [get = r.toggle_get](const Values&) { return int(get()); },
                [set = r.toggle_set](int v) { set(v != 0); });
        } else if (r.press == PartyRow::Press::None || !r.run) {
            e.field = Info();
        } else {
            const auto run = r.run;
            const auto act = [run] { run(party::status::Snapshot()); };
            e.field = r.press == PartyRow::Press::Confirm ? Confirmed(act) : Button(act);
        }
    }
    const u32 off_on[2] = {off, on};
    for (int page = 0; page < 2; ++page) {
        if (page == 0) {
            AddPage("Party", "Группа", "Party co-op: status, codes, rejoin, leave (bbport)",
                    "Совместная игра: состояние, коды, возврат, выход (bbport)");
        } else {
            AddPage("Party mode", "Режим группы", "Party co-op: host, join, back to single player (bbport)",
                    "Совместная игра: хост, вход, одиночная игра (bbport)");
        }
        for (std::size_t i = 0; i < party_rows.size(); ++i) {
            const PartyEntry& e = party_rows[i];
            if (e.spec.page != page) continue;
            if (e.spec.toggle) {
                AddRow(e.field, e.name, e.help, {off_on[0], off_on[1]}, Kind::Toggle,
                       [i] { return party_row_visible[i] != 0; });
            } else {
                AddRow(e.field, e.name, e.help, {e.value}, Kind::Button, [i] { return party_row_visible[i] != 0; });
            }
        }
    }
    // The Party mode page in a single-player process (no mode rows): one line says where to go.
    party.mode_none = AddDynamic(MaxValueLength);
    AddRow(Info(), AddText("Party mode", "Режим группы"),
           AddText("Single player. Party games start on the title screen (Party)",
                   "Одиночная игра. Игра группой начинается на титульном экране (Группа)"),
           {party.mode_none}, Kind::Button, [] {
               for (std::size_t i = 0; i < party_rows.size(); ++i)
                   if (party_rows[i].spec.page == 1 && party_row_visible[i]) return false;
               return true;
           });

    // ---- Party members: one row each (state, ping); the host kicks with two presses. ----
    AddPage("Party members", "Участники группы", "The hunters in the party, their state and ping (bbport)",
            "Охотники группы, их состояние и пинг (bbport)");
    for (int k = 0; k < MaxMembers; ++k) {
        party.member_name[k] = AddDynamic(MaxNameLength);
        party.member_value[k] = AddDynamic(MaxValueLength);
        party.member_help[k] = AddDynamic(MaxHelpLength);
        AddRow(Confirmed([k] {
                   if (member_kickable[k] && !member_names[k].empty()) party::status::KickMember(member_names[k]);
               }),
               party.member_name[k], party.member_help[k], {party.member_value[k]}, Kind::Button,
               [k] { return k < member_rows; });
    }
    // One row per member (kMaxPartyPlayers = 4), then the switches, within the page's six rows:
    // a full party of four shows the notices and party HUD switches, three also guest lamps
    // (also on the overlay's Party tab), two or fewer also the last event.
    AddRow(Flag(&Values::party_toasts), "Join/leave notices", "Уведомления",
           "A short notice in a top corner when a hunter joins or leaves",
           "Короткое уведомление в углу, когда охотник входит или выходит", toggle, Kind::Toggle);
    // Party HUD (party_hud.h): party mode only. With four members the last event row gives way.
    AddRow(Flag(&Values::party_hud), AddText("Party HUD", "Панель группы"),
           AddText("Companions' names, health and where they are, in the bottom left corner",
                   "Имена, здоровье и местоположение спутников в левом нижнем углу"),
           toggle, Kind::Toggle, [] { return party::status::Enabled(); });
    // Guests use lamps and headstones (party_guestsvc; read at the next start). Shown while fewer
    // than four hunters are in the party (the page holds six rows).
    AddRow(Flag(&Values::party_guest_lamps), AddText("Guests use lamps", "Фонари гостям"),
           AddText("Guests may use lamps and headstones; the whole party travels (after a restart)",
                   "Гости могут пользоваться фонарями и надгробиями; группа идёт вместе (после перезапуска)"),
           toggle, Kind::Toggle, [] { return party::status::Enabled() && member_rows <= 3; });
    party.event_value = AddDynamic(MaxValueLength);
    party.event_help = AddDynamic(MaxHelpLength);
    AddRow(Info(), AddText("Last event", "Последнее"), party.event_help, {party.event_value}, Kind::Button,
           [] { return !party::status::Enabled() || member_rows <= 2; });

    // The split-travel question (party_split.cpp): a game dialog with YES / NO after a lamp,
    // headstone or Hunter's Mark in a party (docs/party/split_travel.md).
    split_text[0] = AddText("Travel with the party?\nYES: everyone travels together.  NO: you travel alone.\n"
                            "A rest at a lamp resets the world for the party.",
                            "Путешествовать всей группой?\nДА: вся группа вместе.  НЕТ: только вы.\n"
                            "Отдых у лампы сбрасывает мир для всей группы.");

    // D10 story notices a guest's own game never shows (party_story StoryNoticeMessage): the
    // game's EventText 10012014, as its English / Russian text.
    notice_text[0] = AddText("Ritual secret broken.\nSeek the nightmare newborn.",
                             "Тайна ритуала раскрыта. Ищите новорождённого кошмара.");

    // The host's ending (party_story GuestStory::AnswerEnding): each guest is asked first.
    ending_text[0] = AddText("The host has reached an ending.\nYES: play this ending now (staff roll, then New Game+ in your save).\n"
                             "NO: not now - you return to your Hunter's Dream and keep your current journey.",
                             "Хозяин достиг концовки.\nДА: увидеть эту концовку сейчас (титры, затем Новая игра+ в вашем сохранении).\n"
                             "НЕТ: не сейчас - вы вернётесь в свой Сон охотника и продолжите своё путешествие.");

    // ---- Party rules (party_balance.h): the death debuff and the overhead display. ----
    AddPage("Party rules", "Правила группы", "Death debuff, shared NPC dialogue, what shows above companions, DHT (bbport)",
            "Штраф за смерть, общие диалоги NPC, что видно над спутниками (bbport)");
    balance.rot_help = AddDynamic(MaxHelpLength);
    AddRow(Flag(&Values::party_rot), AddText("Rot on death", "Гниль при смерти"), balance.rot_help, toggle,
           Kind::Toggle);
    // Party name tags (party_hud_game.cpp): ours instead of the game's phantom plate, or the game's.
    AddRow(Flag(&Values::party_name_tags), AddText("Party name tags", "Имена над спутниками"),
           AddText("On: our name plates above companions (never hidden by walls). Off: the game's own",
                   "Вкл.: наши таблички над спутниками (не прячутся за стенами). Выкл.: игровые"),
           toggle, Kind::Toggle);
    // Shared NPC dialogue (party_npctalk.h): a companion's NPC conversation near you, and how near.
    AddRow(Flag(&Values::party_npc_talk), AddText("Share NPC dialogue", "Общие диалоги NPC"),
           AddText("See and hear the NPC conversations of companions near you (subtitles, choices, gifts)",
                   "Видеть и слышать разговоры спутников с NPC рядом с вами (субтитры, выбор, подарки)"),
           toggle, Kind::Toggle);
    balance.npc_range_help = AddDynamic(MaxHelpLength);
    AddRow(Setting(
               Kind::Slider,
               [](const Values& v) { return coop::npctalk::RangePosition(v.party_npc_talk_range.load()); },
               [](int i) { BbSettings::Get().party_npc_talk_range = coop::npctalk::RangeAtPosition(i); }),
           AddText("NPC dialogue range", "Дальность диалогов NPC"), balance.npc_range_help, {}, Kind::Slider);
    // F2: the DHT rendezvous (party_rdv.h), opt-in: it posts this PC's IP on the public BitTorrent DHT.
    AddRow(Flag(&Values::party_rendezvous), AddText("Find via public DHT", "Поиск через DHT"),
           AddText("Auto hole punching via the public BitTorrent DHT (shares your IP). Both PCs; next party start",
                   "Пробивка NAT через публичный DHT BitTorrent (виден ваш IP). На обоих ПК; со следующей группы"),
           toggle, Kind::Toggle);
    // A list opens a pop-up over the rows below it: last on the page.
    AddRow(Setting(
               Kind::List, [](const Values& v) { return std::clamp(v.party_overhead.load(), 0, 5); },
               [](int i) { BbSettings::Get().party_overhead = std::clamp(i, 0, 5); }),
           AddText("Above companions", "Над спутниками"),
           AddText("What shows above party members in view (only you see it)",
                   "Что показывать над спутниками в поле зрения (только у вас)"),
           {AddText("Nothing", "Ничего"), AddText("Name", "Имя"), AddText("Name and level", "Имя и уровень"),
            AddText("Name and health", "Имя и здоровье"), AddText("Name and deaths", "Имя и смерти"),
            AddText("Name and ping", "Имя и пинг")},
           Kind::List);

    // ---- Pings (I2, party_ping.h): Party rules holds six rows already, so a page of its own. ----
    AddPage("Pings", "Метки", "Mark a spot for the party, see companions through walls (bbport)",
            "Отметить место для группы, видеть спутников сквозь стены (bbport)");
    AddRow(Flag(&Values::party_pings), AddText("Pings", "Метки"),
           AddText("Hold the touchpad and press R3 (keyboard: V) to mark where you look; everyone sees it 10 s",
                   "Зажмите тачпад и нажмите R3 (клавиатура: V), чтобы отметить место; группа видит его 10 с"),
           toggle, Kind::Toggle);
    AddRow(Flag(&Values::party_through_walls), AddText("Show party through walls", "Группа сквозь стены"),
           AddText("A small marker with the distance on each companion on your map, also behind walls",
                   "Маленькая метка с расстоянием на каждом спутнике на вашей карте, и за стенами"),
           toggle, Kind::Toggle);

    // ---- Friendly fire / PvP (I1, party_pvp.h): a party rule only the host sets (ERSC's
    // Judicator's Rulebook). The host gets the toggle; a guest sees the host's rule, read-only. ----
    AddPage("Friendly fire", "Огонь по своим", "PvP and shared lamps: rules the host decides (bbport)",
            "PvP и общие фонари: правила, которые решает хост (bbport)");
    balance.ff_help = AddDynamic(MaxHelpLength);
    AddRow(Flag(&Values::party_friendly_fire), AddText("Friendly fire / PvP", "Огонь по своим / PvP"), balance.ff_help,
           toggle, Kind::Toggle, [] { return party::status::Snapshot().role != party::status::Role::Guest; });
    balance.ff_value = AddDynamic(MaxValueLength);
    balance.ff_guest_help = AddDynamic(MaxHelpLength);
    AddRow(Info(), AddText("Friendly fire / PvP", "Огонь по своим / PvP"), balance.ff_guest_help, {balance.ff_value},
           Kind::Button, [] { return party::status::Snapshot().role == party::status::Role::Guest; });

    // Shared lamps (statsaudit finding 5): a party rule only the host sets, on by default.
    balance.lamps_help = AddDynamic(MaxHelpLength);
    AddRow(Flag(&Values::party_share_lamps), AddText("Shared lamps", "Общие фонари"), balance.lamps_help, toggle,
           Kind::Toggle, [] { return party::status::Snapshot().role != party::status::Role::Guest; });
    balance.lamps_value = AddDynamic(MaxValueLength);
    balance.lamps_guest_help = AddDynamic(MaxHelpLength);
    AddRow(Info(), AddText("Shared lamps", "Общие фонари"), balance.lamps_guest_help, {balance.lamps_value},
           Kind::Button, [] { return party::status::Snapshot().role == party::status::Role::Guest; });

    // ---- Enemy scaling (party_balance.h): per extra hunter, the host's values count. ----
    AddPage("Enemy scaling", "Сила врагов", "Boss and enemy health, damage and poise per extra hunter (bbport)",
            "Здоровье, урон и стойкость боссов и врагов за каждого охотника (bbport)");
    const auto scale_row = [&](std::atomic<int> Values::*member, const int* table, const char* en, const char* ru,
                               int index) {
        balance.scale_help[index] = AddDynamic(MaxHelpLength);
        AddRow(Setting(
                   Kind::Slider,
                   [member, table](const Values& v) { return coop::balance::Position(table, (v.*member).load()); },
                   [member, table](int i) { (BbSettings::Get().*member) = coop::balance::AtPosition(table, i); }),
               AddText(en, ru), balance.scale_help[index], {}, Kind::Slider);
    };
    using namespace coop::balance;
    scale_row(&Values::party_boss_hp, kHpTable, "Boss health", "Здоровье боссов", 0);
    scale_row(&Values::party_boss_damage, kDamageTable, "Boss damage", "Урон боссов", 1);
    scale_row(&Values::party_boss_poise, kPoiseTable, "Boss poise", "Стойкость боссов", 2);
    scale_row(&Values::party_enemy_hp, kHpTable, "Enemy health", "Здоровье врагов", 3);
    scale_row(&Values::party_enemy_damage, kDamageTable, "Enemy damage", "Урон врагов", 4);
    scale_row(&Values::party_enemy_poise, kPoiseTable, "Enemy poise", "Стойкость врагов", 5);
}

/// The party rules' help lines: the current value, and on a guest that the host's rules count.
void RefreshBalance() {
    if (!balance.rot_help) return;
    const auto& v = BbSettings::Get();
    const bool guest = party::status::Snapshot().role == party::status::Role::Guest;
    const std::string g_en = guest ? " (host decides)" : "", g_ru = guest ? " (решает хост)" : "";
    SetDynamic(balance.rot_help, "Each death: max HP -10 % (up to 5), a lamp rest cleanses it" + g_en,
               "Каждая смерть: -10 % здоровья (до 5 раз), отдых у фонаря снимает" + g_ru);
    if (balance.npc_range_help) {
        const int r = coop::npctalk::RangeAtPosition(coop::npctalk::RangePosition(v.party_npc_talk_range.load()));
        SetDynamic(balance.npc_range_help, "Companions' NPC dialogue reaches you " + coop::npctalk::DescribeRange(r),
                   r > 0 ? "Диалоги спутников с NPC: в пределах " + std::to_string(r) + " м от NPC"
                         : std::string("Диалоги спутников с NPC: на всей карте"));
    }
    if (balance.ff_help) {
        SetDynamic(balance.ff_help,
                   "Party members can hurt each other (both ways); a party kill is a normal death. Party rule",
                   "Члены группы могут ранить друг друга; убийство в группе - обычная смерть. Правило группы");
        const bool ff = party::status::FriendlyFire();
        SetDynamic(balance.ff_value, ff ? "On" : "Off", ff ? "Вкл" : "Выкл");
        SetDynamic(balance.ff_guest_help, ff ? "The host turned friendly fire on: party members can hurt each other"
                                             : "The host decides; off: party members cannot hurt each other",
                   ff ? "Хост включил огонь по своим: члены группы могут ранить друг друга"
                      : "Решает хост; выкл.: члены группы не могут ранить друг друга");
    }
    if (balance.lamps_help) {
        SetDynamic(balance.lamps_help, "A lamp any party member lights is lit for everyone, in each save. Party rule",
                   "Фонарь, зажжённый любым членом группы, горит у всех, в каждом сохранении. Правило группы");
        const bool on = party::status::ShareLamps();
        SetDynamic(balance.lamps_value, on ? "On" : "Off", on ? "Вкл" : "Выкл");
        SetDynamic(balance.lamps_guest_help, on ? "The host shares lamps: every lamp lit in the party is lit for you too"
                                                : "The host turned shared lamps off: your own lamps stay yours",
                   on ? "Хост делит фонари: каждый фонарь группы горит и у вас"
                      : "Хост выключил общие фонари: ваши фонари остаются вашими");
    }
    using namespace coop::balance;
    const int values[6] = {v.party_boss_hp, v.party_boss_damage, v.party_boss_poise,
                           v.party_enemy_hp, v.party_enemy_damage, v.party_enemy_poise};
    for (int k = 0; k < 6; ++k) {
        const int kind = k % 3;
        const std::string en = kind == 0 ? DescribeHp(values[k], false)
                               : kind == 1 ? DescribeDamage(values[k], false) : DescribePoise(values[k], false);
        const std::string ru = kind == 0 ? DescribeHp(values[k], true)
                               : kind == 1 ? DescribeDamage(values[k], true) : DescribePoise(values[k], true);
        const char* ref_en = k < 3 ? "; 100 % = Bloodborne" : "; vanilla 0";
        const char* ref_ru = k < 3 ? "; 100 % = как в игре" : "; в игре 0";
        if (!balance.scale_help[k]) continue;  // poise: overlay Party tab / bbport.ini only
        SetDynamic(balance.scale_help[k], en + (kind == 1 ? "" : ref_en) + g_en, ru + (kind == 1 ? "" : ref_ru) + g_ru);
    }
}

} // namespace

u32 EndingQuestionText() {
    Define();
    return ending_text[0];
}

u32 StoryNoticeText(u32 game_message) {
    Define();
    return game_message == 10012014u ? notice_text[0] : 0;
}

u32 SplitTravelText(int which) {
    Define();
    return which == 0 ? split_text[0] : 0;
}

std::u16string Utf16(const char* utf8) {
    std::u16string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8); *p;) {
        u32 c = *p++;
        if (c >= 0xf0) {
            c = (c & 7) << 18 | (p[0] & 0x3f) << 12 | (p[1] & 0x3f) << 6 | (p[2] & 0x3f);
            p += 3;
        } else if (c >= 0xe0) {
            c = (c & 15) << 12 | (p[0] & 0x3f) << 6 | (p[1] & 0x3f);
            p += 2;
        } else if (c >= 0xc0) {
            c = (c & 31) << 6 | (p[0] & 0x3f);
            p += 1;
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(char16_t(0xd800 + (c >> 10)));
            out.push_back(char16_t(0xdc00 + (c & 0x3ff)));
        } else {
            out.push_back(char16_t(c));
        }
    }
    return out;
}

int ChoiceFromPosition(u8 position, int count) {
    return std::clamp((int(position) * (count - 1) + 5) / 10, 0, count - 1);
}

u8 ChoicePosition(int choice, int count) {
    return u8((std::clamp(choice, 0, count - 1) * 10 * 2 + (count - 1)) / (2 * (count - 1)));
}

u8 NearestPosition(const float* table, float value) {
    int best = 0;
    for (int i = 1; i < 11; ++i) {
        if (std::fabs(table[i] - value) < std::fabs(table[best] - value)) best = i;
    }
    return u8(best);
}

void Define() {
    if (!pages.empty()) {
        return;
    }
    DefineAll();
    const std::size_t n = fields.size();
    values.assign(n, 0);
    defaults.assign(n, 0);
    applied.assign(n, 0);
    resting.assign(n, -1);
    bytes.assign(n, 0);
    default_bytes.assign(n, 0);
    rested_since.assign(n, 0);
    armed_at.assign(n, -1000000);
    for (const Page& p : pages) {
        for (const Row& r : p.rows) {
            Field& f = fields[std::size_t(r.field)];
            f.kind = r.kind;
            if (r.kind == Kind::Steps) f.steps = int(r.choices.size());
        }
    }
    const Values& d = DefaultSettings();
    for (std::size_t i = 0; i < n; ++i) {
        const Field& f = fields[i];
        defaults[i] = f.get ? f.get(d) : 0;
        default_bytes[i] = f.steps ? ChoicePosition(defaults[i], f.steps) : u8(defaults[i]);
    }
    RefreshDynamic();
}

const std::vector<Page>& All() {
    return pages;
}

int FieldCount() {
    return int(fields.size());
}

const char16_t* Text(u32 id, bool russian) {
    if (id < IdBase || id >= IdBase + texts.size()) {
        return nullptr;
    }
    const TextEntry& t = texts[id - IdBase];
    if (t.dynamic >= 0) {
        return dynamics[std::size_t(t.dynamic)]->buffer[russian ? 1 : 0].data();
    }
    return russian ? t.russian.c_str() : t.english.c_str();
}

std::size_t TextCount() {
    return texts.size();
}

std::uint64_t DynamicChanges() {
    return dynamic_changes.load(std::memory_order_acquire);
}

std::uint64_t ValueChanges() {
    return value_changes.load(std::memory_order_acquire);
}

bool IsDynamic(u32 id) {
    return id >= IdBase && id < IdBase + texts.size() && texts[id - IdBase].dynamic >= 0;
}

int* Value(int field) {
    return &values[std::size_t(field)];
}
u8* Byte(int field) {
    return &bytes[std::size_t(field)];
}
const int* DefaultValue(int field) {
    return &defaults[std::size_t(field)];
}
const u8* DefaultByte(int field) {
    return &default_bytes[std::size_t(field)];
}
bool ByteField(int field) {
    return fields[std::size_t(field)].kind != Kind::List;
}

bool ChoiceAvailable(int field, int choice) {
    return field != upscaler_field || UpscalerAvailable(choice);
}

/// Load (the game's menu thread, when a page opens) and Poll / RefreshDynamic (the present
/// thread) share the values and the run-time texts' sources (member names, the visible rows):
/// one of them at a time. Recursive: a button's action may refresh the texts.
std::recursive_mutex& StateMutex() {
    static std::recursive_mutex m;
    return m;
}

void Load() {
    std::scoped_lock lock{StateMutex()};
    const Values& s = BbSettings::Get();
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const Field& f = fields[i];
        values[i] = f.get ? f.get(s) : 0;
        bytes[i] = f.steps ? ChoicePosition(values[i], f.steps) : u8(values[i]);
        applied[i] = values[i];
        resting[i] = values[i];
        if (!f.get) {
            default_bytes[i] = 0;
        }
    }
    RefreshDynamic();
    loaded = true;
}

/// The HDR page: whether the output runs (or why not) and the peak the curve uses.
void RefreshDynamic() {
    std::scoped_lock lock{StateMutex()};
    RefreshParty();
    RefreshRestart();
    RefreshBalance();
    RefreshConfirmHelps();
}

bool IsButton(int field) {
    return field >= 0 && std::size_t(field) < fields.size() && fields[std::size_t(field)].kind == Kind::Button;
}

void Press(int field, std::int64_t now_ms) {
    std::scoped_lock lock{StateMutex()};
    if (!IsButton(field)) return;
    const Field& f = fields[std::size_t(field)];
    if (!f.run) return;
    if (f.confirm && !Armed(field, now_ms)) {
        armed_at[std::size_t(field)] = now_ms;
        RefreshConfirmHelps();
        return;
    }
    armed_at[std::size_t(field)] = -1000000;
    RefreshConfirmHelps();
    f.run();
    RefreshDynamic(); // the row's new state ("Copied", the flow's message) right away
}

std::vector<const Row*> ShownRows(const Page& page) {
    std::vector<const Row*> rows;
    for (const Row& row : page.rows) {
        if (!row.visible || row.visible()) rows.push_back(&row);
    }
    if (rows.empty() && !page.rows.empty()) rows.push_back(&page.rows.front());
    return rows;
}

void SetPartyStatusHooks(PartyStatusHooks hooks) {
    status_hooks = std::move(hooks);
}

void Poll(std::int64_t now_ms) {
    std::scoped_lock lock{StateMutex()};
    if (!loaded) {
        return;
    }
    if (now_ms - last_refresh.load() >= 250) {
        last_refresh = now_ms;
        RefreshDynamic();
    }
    bool save = false, adopted = false;
    const Values& settings = BbSettings::Get();
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const Field& f = fields[i];
        if (f.kind != Kind::List) {
            // A slider with choices: its position, not the choice's number.
            values[i] = f.steps ? ChoiceFromPosition(bytes[i], f.steps) : bytes[i];
        }
        const int value = values[i];
        if (value == applied[i]) {
            resting[i] = value;
            // Not changed on the page: the setting may have been changed elsewhere (the overlay
            // menu; a setter that falls back, e.g. an upscaler this GPU lacks). The row follows
            // it, so the page never shows (or later re-applies) a stale value.
            const int now_set = f.get ? f.get(settings) : value;
            if (now_set != value) {
                values[i] = applied[i] = resting[i] = now_set;
                bytes[i] = f.steps ? ChoicePosition(now_set, f.steps) : u8(now_set);
                adopted = true;
            }
            continue;
        }
        if (f.debounce) {
            // The output resolution and the preset re-create the scene targets and the upscaler
            // (a few grey frames): applied once the slider rests for 0.4 s, not at each step.
            if (resting[i] != value) {
                resting[i] = value;
                rested_since[i] = now_ms;
                continue;
            }
            if (now_ms - rested_since[i] < 400) {
                continue;
            }
        }
        applied[i] = value;
        if (f.set) {
            f.set(value);
            save = true;
        }
    }
    if (save) {
        BbSettings::Save();
    }
    if (adopted) value_changes.fetch_add(1, std::memory_order_release);
    // The run-time texts that describe values (the enemy scaling lines) at once, not at the next
    // 250 ms refresh: the glue redraws the help line in the same frame.
    if (save || adopted) RefreshDynamic();
}

void AddPartyRow(PartyRow row) {
    pending_party_rows.push_back(std::move(row));
}

int PartyRowCount() {
    return int(party_rows.size());
}

void SetRestartHandler(void (*restart)()) {
    restart_handler = restart;
}

void SetQuitHandler(void (*quit)()) {
    quit_handler = quit;
}

} // namespace BbGameMenu::Pages
