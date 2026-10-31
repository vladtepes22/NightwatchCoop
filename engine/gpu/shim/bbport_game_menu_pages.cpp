// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_game_menu_pages.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
u32 gather_text[1] = {0};
u32 guest_gather_text[1] = {0};  // CL-a3a9e6: a guest gathers the party (run-time: who)
u32 vote_text[1] = {0};  // 0.7.3: the travel vote's question (run-time: who, where)

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
constexpr int LodValues[] = {0, 1};
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
    u32 net_value, net_help;  // Party network (F2, 0.7.1)
    u32 mode_none;
} party;
/// The HDR page's run-time help lines (whether HDR runs, and why not; the automatic peak).
struct HdrTexts {
    u32 status_help = 0, peak_help = 0;
} hdr_texts;
struct BalanceTexts {
    u32 scale_help[6]{};
    /// guestrules (0.7.3): a guest's read-only lines with the host's scaling (value, help).
    u32 guest_scale_value[6]{}, guest_scale_help[6]{};
    u32 npc_range_help = 0;
} balance;
/// A host's rule on a guest: the read-only line's value and help, from the rule in force.
struct GuestRule {
    u32 value = 0, help = 0;
    std::function<bool()> on;
    Texts on_help, off_help;
    /// 0.7.4: a rule with more than on / off (Insight for bosses): its value text (`on` then
    /// picks the help line only).
    std::function<Texts()> value_text;
};
std::vector<GuestRule> guest_rules;
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

    // Party network: which path the link took (party_netroute: IPv6, a member relay) and the advice.
    {
        const std::string& n = b.network;
        const bool relayed = n.find("'s relay") != std::string::npos && n.find("Connected over") != std::string::npos;
        const bool v6 = n.find("via IPv6") != std::string::npos;
        both([&](bool r) {
            if (!on || n.empty()) return std::string("-");
            if (host) return std::string(n.find("Through a member's relay") != std::string::npos ? L(r, "Host, relayed guest", "Хост, гость через ретр.")
                                                                                          : L(r, "Host", "Хост"));
            if (relayed) return std::string(L(r, "Member relay", "Через участника"));
            if (v6) return std::string("IPv6");
            if (n.find("Connected over UDP") != std::string::npos) return std::string("UDP");
            if (n.find("Connected over TCP") != std::string::npos) return std::string("TCP");
            return std::string(L(r, "Not connected", "Нет связи"));
        });
        SetDynamic(party.net_value, en, ru);
        const std::string text = n.empty() ? std::string("No network information yet") : n;
        const std::size_t cut = MaxHelpLength - 3;
        const std::string help = text.size() > MaxHelpLength ? text.substr(0, cut) + "..." : text;
        SetDynamic(party.net_help, help, help);
    }

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
        // CL-adeb50 "Stay in my world": this guest (its own switch) or, on the host, a guest that reported it.
        const bool staying = m.slot != 0 && m.connected && !m.in_world &&
                             (m.local ? BbSettings::Get().party_stay.load() : ((ps::StayMask() >> m.slot) & 1u) != 0);
        const auto where = [&](bool r) {
            std::string text = !m.connected  ? L(r, "Connection lost", "Связь потеряна")
                               : m.slot == 0 ? L(r, "Host's world", "Мир хоста")
                               : m.in_world  ? L(r, "In world", "В мире") // short: the value column is narrow
                               : staying     ? L(r, "Staying home", "Остаётся дома") // CL-adeb50
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

/// The Party page's own rows (orders 10..40): status, rot, copy code (two rows for a host with an
/// Internet and a LAN code), rejoin, leave; New party code (host) is on the Party mode page. The
/// join/leave notices switch and the last event are on the Party members page (room for the party
/// flow's rows here).
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

    // C4: this player's rot now and what it does (party_balance; each player has its own, under
    // the host's "Rot on death" rule).
    PartyRow rot;
    rot.order = 15;
    rot.name = [](const B&) { return T2("Rot", "Гниль"); };
    rot.value = [](const B&) {
        int kind[ps::kRotKinds];
        ps::Rot(kind);
        int n = 0;
        for (int k : kind) n += k;
        if (n == 0) return T2("None", "Нет");
        const std::string v = std::to_string(std::min(n, coop::balance::kRotMaxStacks)) + "/" +
                              std::to_string(coop::balance::kRotMaxStacks);
        return Texts{v, v};
    };
    rot.help = [](const B&) {
        int kind[ps::kRotKinds];
        ps::Rot(kind);
        const std::string en = coop::balance::DescribeRot(kind, false), ru = coop::balance::DescribeRot(kind, true);
        if (en.empty())
            return T2("No rot. Each death adds a stack (Deaths & revives > Rot on death)",
                      "Гнили нет. Каждая смерть добавляет её (Правила группы > Гниль при смерти)");
        // All five kinds fill the help line: the lamp hint only while it fits.
        const auto with = [](const std::string& t, const char* hint) {
            return Utf16((t + hint).c_str()).size() <= MaxHelpLength ? t + hint : t;
        };
        return Texts{with(en, ". A lamp rest cleanses it"), with(ru, ". Отдых у фонаря снимает")};
    };
    rot.visible = [](const B& b) { return Running(b); };
    party_rows.push_back({rot});

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
    lan.page = 1;  // the Party page is full with Gather the party (only a host has two codes)
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

    // I5: the host brings every guest to it (party_gather.h): each guest is asked first; an
    // in-world warp in the host's map, one load from another map. The Party page holds six rows
    // for a running host: the LAN code row is on the Party mode page (below).
    PartyRow gather;
    gather.order = 32;
    gather.name = [](const B&) { return T2("Gather the party", "Собрать группу"); };
    gather.value = [](const B&) { return T2("Gather", "Собрать"); };
    gather.help = [](const B& b) {
        return b.role == ps::Role::Guest
                   ? T2("Bring the host and the other guests to you; each is asked, 30 s apart. The host allows it",
                        "Перенести хоста и других гостей к вам; каждого спросят, раз в 30 с. Если хост разрешил")
                   : T2("Bring every guest to you; each is asked, not in boss fights, 30 s apart. Also touchpad + L3 / H",
                        "Перенести всех гостей к вам; каждого спросят, не в бою с боссом, раз в 30 с. Ещё тачпад + L3 / H");
    };
    // CL-a3a9e6: a guest sees it only while the host allows guests to gather (the host's rule, synced).
    gather.visible = [](const B& b) {
        return Running(b) && (b.role == ps::Role::Host ||
                              (b.role == ps::Role::Guest && ps::HostRuleOn(ps::HostRule::GuestGather)));
    };
    gather.press = PartyRow::Press::Once;
    gather.run = [](const B& b) {
        if (Running(b) && (b.role == ps::Role::Host || b.role == ps::Role::Guest)) ps::RequestGather();
    };
    party_rows.push_back({gather});

    // The host's code stays the same from one launch to the next until it asks for a new one here
    // (or in the launcher's Party tab): for a code that went to someone it should not have. On the
    // Party mode page (hosting): the Party page already shows six rows for a running host.
    PartyRow newcode;
    newcode.order = 27;
    newcode.page = 1;
    newcode.name = [](const B&) { return T2("New party code", "Новый код группы"); };
    newcode.value = [](const B&) { return T2("Make new", "Создать"); };
    newcode.help = [](const B&) {
        return T2("Press twice: the old code stops working; send friends the new one (a password change also gives a new code)",
                  "Нажмите дважды: старый код перестанет работать; отправьте друзьям новый (смена пароля тоже меняет код)");
    };
    newcode.visible = [](const B& b) { return Running(b) && b.role == ps::Role::Host; };
    newcode.press = PartyRow::Press::Confirm;
    newcode.run = [](const B& b) {
        if (Running(b) && b.role == ps::Role::Host) ps::RequestNewCode();
    };
    party_rows.push_back({newcode});

    // CL-adeb50 "Stay in my world": a guest pauses the automatic joining (party_stayhome_rules.h) and plays on in
    // its own world; a toggle row for guests only (the host's world is the world). Off by default.
    PartyRow stay;
    stay.order = 35;
    stay.toggle = true;
    stay.name = [](const B&) { return T2("Stay in my world", "Остаться в моём мире"); };
    stay.help = [](const B&) {
        return T2("Guest: no automatic joining while on: your Chalice ritual or shopping first. Off: join again",
                  "Гость: пока вкл., вы не заходите к хосту сами: свой ритуал или покупки. Выкл.: снова к хосту");
    };
    stay.visible = [](const B& b) { return Running(b) && b.role == ps::Role::Guest; };
    stay.toggle_get = [] { return BbSettings::Get().party_stay.load(); };
    stay.toggle_set = [](bool on) { BbSettings::Get().party_stay = on; };
    party_rows.push_back({stay});

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
            AddPage("Party", "Группа", "Party co-op: status, codes, rejoin, gather, leave (bbport)",
                    "Совместная игра: состояние, коды, возврат, сбор, выход (bbport)");
        } else {
            AddPage("Party mode", "Режим группы", "Party co-op: host, join, LAN code, back to single player (bbport)",
                    "Совместная игра: хост, вход, код LAN, одиночная игра (bbport)");
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
    AddPage("Party members", "Участники группы", "The hunters in the party, their state and ping, the last event (bbport)",
            "Охотники группы, их состояние и пинг, последнее событие (bbport)");
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
    // One row per member (kMaxPartyPlayers = 4) and the last event: five rows at most. (0.7.3: the
    // join/leave notices are on My party: Look & notices, the DHT switch on Party network.)
    party.event_value = AddDynamic(MaxValueLength);
    party.event_help = AddDynamic(MaxHelpLength);
    // 0.7.8 (fp5): a page holds RowsPerPage rows; with 5 hunters the member rows, this one and the toggle
    // below made 7 and the toggle was dropped: the info row steps aside (the event is also a notice).
    AddRow(Info(), AddText("Last event", "Последнее"), party.event_help, {party.event_value}, Kind::Button,
           [] { return member_rows + 2 <= int(RowsPerPage); });
    // 0.7.6: connection warnings (party/party_connwarn.h): the firewall / router / address hints (a player's
    // own switch; Look & notices has no free slot).
    AddRow(Flag(&Values::party_conn_warnings), "Connection warnings", "Предупреждения о связи",
           "A notice with the fix when the firewall, router or address keeps the party from working",
           "Уведомление с решением, когда брандмауэр, роутер или адрес мешают группе", toggle, Kind::Toggle);

    // ---- Party network (F2, 0.7.1, party_netroute.h): the link's path, the member relay, IPv6 ----
    AddPage("Party network", "Сеть группы", "The link's path, the public DHT, relaying for members, IPv6 (bbport)",
            "Путь связи, публичный DHT, ретрансляция для участников, IPv6 (bbport)");
    party.net_value = AddDynamic(MaxValueLength);
    party.net_help = AddDynamic(MaxHelpLength);
    AddRow(Info(), AddText("Network", "Сеть"), party.net_help, {party.net_value}, Kind::Button);
    // F2: the DHT rendezvous (party_rdv.h), opt-in: it posts this PC's IP on the public BitTorrent DHT.
    AddRow(Flag(&Values::party_rendezvous), AddText("Find via public DHT", "Поиск через DHT"),
           AddText("Auto hole punching via the public BitTorrent DHT (shares your IP). Both PCs; next party start",
                   "Пробивка NAT через публичный DHT BitTorrent (виден ваш IP). На обоих ПК; со следующей группы"),
           toggle, Kind::Toggle);
    AddRow(Flag(&Values::party_member_relay), AddText("Relay for members", "Ретранслятор"),
           AddText("Relays a party member that cannot reach the host (or you through one). Party members only",
                   "Ретрансляция для участника, не достающего до хоста (или вас через него). Только группа"),
           toggle, Kind::Toggle);
    AddRow(Flag(&Values::party_ipv6), AddText("IPv6", "IPv6"),
           AddText("IPv6: players reach you over it; the party code then holds your IPv6 address, changing when Windows rotates it",
                   "IPv6: связь по нему; код группы тогда содержит ваш адрес IPv6 и меняется вместе с ним"),
           toggle, Kind::Toggle);
    AddRow(Button([] { party::status::RequestAddRelayCodeFromClipboard(); }), AddText("Join via a member", "Через участника"),
           AddText("Guest: copy a member's guest code (BBG1-...) and press: its relay if the host is out of reach",
                   "Гость: скопируйте гостевой код участника (BBG1-...) и нажмите: его ретранслятор, если хост недоступен"),
           {AddText("Paste", "Вставить")}, Kind::Button,
           [] { return party::status::Snapshot().role == party::status::Role::Guest; });
    // 0.7.7: Nightwatch VPN lobby (net/party_vpnlobby.h; the list is in the launcher's Join page). A host's row:
    // a guest has "Join via a member" in its place, so the page stays within the six rows.
    AddRow(Flag(&Values::party_vpn_lobby), AddText("Nightwatch VPN lobby", "Лобби Nightwatch VPN"),
           AddText("Host with a password: list your character name and slots for Nightwatch VPN players (no address)",
                   "Хост с паролем: имя героя и места видны игрокам Nightwatch VPN (без адреса)"),
           toggle, Kind::Toggle, [] { return party::status::Snapshot().role != party::status::Role::Guest; });
    // 0.7.5: host over a VPN. A list opens a pop-up over the rows below it: last on the page.
    AddRow(Setting(
               Kind::List, [](const Values& v) { return std::clamp(v.party_vpn.load(), 0, 2); },
               [](int i) { BbSettings::Get().party_vpn = std::clamp(i, 0, 2); }),
           AddText("Host over a VPN", "Хост через VPN"),
           AddText("Radmin, Hamachi, ZeroTier, Tailscale: your code holds the VPN address. Auto: if one is up. Next start",
                   "Radmin, Hamachi, ZeroTier, Tailscale: в код попадёт адрес VPN. Авто: если он есть. Со след. запуска"),
           {AddText("Auto", "Авто"), AddText("Off", "Выкл."), AddText("Named adapter", "Адаптер по имени")},
           Kind::List);

    // The split-travel question (party_split.cpp): a game dialog with YES / NO after a lamp,
    // headstone or Hunter's Mark in a party (docs/party/split_travel.md).
    split_text[0] = AddText("Travel with the party?\nYES: everyone travels together and the world resets.\n"
                            "NO: you travel alone; the others stay where they are.",
                            "Путешествовать всей группой?\nДА: вся группа вместе, мир сбрасывается.\n"
                            "НЕТ: только вы; остальные остаются на месте.");

    // D10 story notices a guest's own game never shows (party_story StoryNoticeMessage): the
    // game's EventText 10012014, as its English / Russian text.
    notice_text[0] = AddText("Ritual secret broken.\nSeek the nightmare newborn.",
                             "Тайна ритуала раскрыта. Ищите новорождённого кошмара.");

    // I5: the host gathers the party (party_gather.cpp): each guest is asked first.
    // 0.7.3: lamp travel voting (party_vote.cpp): who wants to travel where, set before it opens.
    vote_text[0] = AddDynamic(DynamicCapacity - 1);

    guest_gather_text[0] = AddDynamic(DynamicCapacity - 1);
    gather_text[0] = AddText("The host gathers the party.\nYES: go to the host now.  NO: stay where you are.",
                             "Хозяин собирает группу.\nДА: перейти к хозяину сейчас.  НЕТ: остаться здесь.");

    // The host's ending (party_story GuestStory::AnswerEnding): each guest is asked first.
    ending_text[0] = AddText("The host has reached an ending.\nYES: play this ending now (staff roll, then New Game+ in your save).\n"
                             "NO: not now - you return to your Hunter's Dream and keep your current journey.",
                             "Хозяин достиг концовки.\nДА: увидеть эту концовку сейчас (титры, затем Новая игра+ в вашем сохранении).\n"
                             "НЕТ: не сейчас - вы вернётесь в свой Сон охотника и продолжите своё путешествие.");

    // ---- The party rules (user decision 2026-10-21): every party gameplay option has a row, on
    // pages by theme, the rows a page holds ordered by how often players change them. A host's rule
    // (party_balance Rules, sent to every guest) is a toggle for the host and a solo player and a
    // read-only line with the host's value for a guest. The other rows are this player's own
    // (bbport.ini; BbSettings::PartySwitches); "next start": a party module reads it at start. ----
    const auto is_guest = [] { return party::status::Snapshot().role == party::status::Role::Guest; };
    const auto host_rule = [&](std::atomic<bool> Values::*member, const char* en, const char* ru, u32 help,
                               std::function<bool()> in_force, Texts guest_on, Texts guest_off) {
        AddRow(Flag(member), AddText(en, ru), help, toggle, Kind::Toggle, [is_guest] { return !is_guest(); });
        GuestRule g{AddDynamic(MaxValueLength), AddDynamic(MaxHelpLength), std::move(in_force),
                    std::move(guest_on), std::move(guest_off), {}};
        AddRow(Info(), AddText(en, ru), g.help, {g.value}, Kind::Button, is_guest);
        guest_rules.push_back(std::move(g));
    };
    const auto own = [&](std::atomic<bool> Values::*member, const char* en, const char* ru, const char* help_en,
                         const char* help_ru) {
        AddRow(Flag(member), AddText(en, ru), AddText(help_en, help_ru), toggle, Kind::Toggle);
    };
    namespace ps = party::status;

    // ---- 0.7.3 (user request): the party's settings in two groups of pages, named as the launcher's
    // tabs. "Party rules: ..." holds what the HOST decides for the party (party_balance Rules, sent to
    // every guest; a guest sees read-only lines with the host's values); "My party: ..." what each
    // player chooses for itself. New rows: a host rule (host_rule) on a Party rules page, a player's
    // own (own) on a My party page; at most six rows a page. 0.7.4 (partyrules, user spec): every
    // setting about how the session plays is a Party rule (the guests' deaths, travel, rewards); My
    // party keeps what a party brings into this player's own save (Your save, Your items) and what
    // only this screen shows (Display, Look & notices). ----

    // ---- Party rules: World (0.7.1: party_kill_record, party_npc_mirror, party_gather; 0.7.3:
    // covenants, party_travel_vote, party_npc_fights) ----
    AddPage("Party rules: World", "Правила: мир",
            "Host's rules: enemies stay dead, gathering, NPCs as in the host's world, covenants, votes, NPC hunters",
            "Правила хоста: враги не оживают, сбор, NPC как у хоста, ковенанты, голосование, охотники-NPC");
    host_rule(&Values::party_kill_record, "Enemies stay dead", "Враги не оживают",
              AddText("A party kill is dead for everyone, also as maps stream in again, until a rest. Party rule",
                      "Убитый группой враг мёртв у всех, и при новой подгрузке карты, до отдыха. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::KillRecord); },
              {"The host's rule: an enemy the party killed stays dead for you too, until a rest",
               "Правило хоста: убитый группой враг мёртв и у вас, до отдыха"},
              {"The host decides; off: an enemy killed out of your sight may stand again here",
               "Решает хост; выкл.: враг, убитый не у вас на глазах, может стоять у вас"});
    host_rule(&Values::party_gather, "Host can gather", "Хост собирает группу",
              AddText("Party > Gather the party brings every guest to you (each is asked). Party rule",
                      "Группа > Собрать группу переносит гостей к вам (каждого спросят). Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::Gather); },
              {"The host may gather the party: you are asked each time and may say no",
               "Хост может собрать группу: вас каждый раз спросят, можно отказаться"},
              {"The host turned gathering off: you are never moved to the host",
               "Хост выключил сбор группы: вас не переносят к нему"});
    host_rule(&Values::party_npc_mirror, "NPCs as in host's world", "NPC как в мире хоста",
              AddText("An NPC gone from your world is gone for the guests too (the clinic's door). Party rule",
                      "NPC, которого нет в вашем мире, нет и у гостей (дверь клиники). Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::NpcMirror); },
              {"The host's rule: an NPC gone from the host's world is gone here too",
               "Правило хоста: NPC, которого нет в мире хоста, нет и у вас"},
              {"The host decides; off: you may see NPCs the host's world no longer has",
               "Решает хост; выкл.: вы можете видеть NPC, которых нет в мире хоста"});
    // 0.7.3 (user decision 2026-10-22): covenant outcomes (party_progress covenant_outcome).
    host_rule(&Values::party_covenant_per_player, "Covenants per player", "Ковенанты у каждого свои",
              AddText("Annalise's, Alfred's and Valtr's quest outcomes stay in each player's own save. Party rule",
                      "Исходы квестов Аннализы, Альфреда и Валтора у каждого в своём сохранении. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::CovenantPerPlayer); },
              {"The host's rule: covenant outcomes in the host's world stay out of your save",
               "Правило хоста: исходы ковенантов в мире хоста не попадают в ваше сохранение"},
              {"The host shares covenant outcomes: they reach your save like other quests",
               "Хост делит исходы ковенантов: они попадают в ваше сохранение, как другие квесты"});
    // 0.7.3 (user: "everybody has to vote yes"): a party travel waits for every member's YES.
    host_rule(&Values::party_travel_vote, "Party votes on travel", "Голосование за путь",
              AddText("A party lamp, headstone or Mark travel needs every member's YES; one NO cancels it. Party rule",
                      "Путь группы через фонарь, надгробие или Метку - только если все за; одно НЕТ отменяет. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::TravelVote); },
              {"The host's rule: the party travels only when everyone votes YES",
               "Правило хоста: группа идёт, только если все проголосовали ДА"},
              {"The host decides; off: the one who travels takes the party along",
               "Решает хост; выкл.: кто путешествует, ведёт за собой группу"});
    // 0.7.3 (D11, party_npcfight_rules.h): a scripted NPC hunter fight is the whole party's.
    host_rule(&Values::party_npc_fights, "NPC hunters fight all", "Охотники-NPC против всех",
              AddText("A hunter NPC turning hostile does so for the guests too: all can fight it. Party rule",
                      "Охотник-NPC становится врагом и у гостей: сражаться могут все. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::NpcFights); },
              {"The host's rule: NPC hunters (Alfred, Eileen, Valtr, Brador ...) fight you too",
               "Правило хоста: охотники-NPC (Альфред, Эйлин, Валтор, Брадор ...) сражаются и с вами"},
              {"The host decides; off: a hunter that turns on the host may stay friendly here",
               "Решает хост; выкл.: охотник, напавший на хоста, может остаться у вас мирным"});


    // ---- Party rules: PvP & lamps (party_pvp, party_balance) ----
    AddPage("Party rules: PvP & lamps", "Правила: PvP и фонари",
            "Host's rules: friendly fire, full health in PvP, shared lamps, guests' lamps in the host's world (bbport)",
            "Правила хоста: огонь по своим, полное здоровье в PvP, общие фонари, фонари гостей (bbport)");
    host_rule(&Values::party_friendly_fire, "Friendly fire / PvP", "Огонь по своим / PvP",
              AddText("Party members can hurt each other (both ways); a party kill is a normal death. Party rule",
                      "Члены группы могут ранить друг друга; убийство в группе - обычная смерть. Правило группы"),
              [] { return ps::FriendlyFire(); },
              {"The host turned friendly fire on: party members can hurt each other",
               "Хост включил огонь по своим: члены группы могут ранить друг друга"},
              {"The host decides; off: party members cannot hurt each other",
               "Решает хост; выкл.: члены группы не могут ранить друг друга"});
    // 0.7.6 (pvphp, owner's decision): a 70 % hunter against a 100 % host is no duel.
    host_rule(&Values::party_pvp_full_hp, "Full health in PvP", "Полное здоровье в PvP",
              AddText("While friendly fire is on, joined hunters keep full maximum health (no phantom cut). Party rule",
                      "При огне по своим присоединившиеся охотники сохраняют полное макс. здоровье. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::PvpFullHp); },
              {"The host's rule: with friendly fire on, hunters who joined keep their full maximum health",
               "Правило хоста: при огне по своим присоединившиеся охотники сохраняют полное максимальное здоровье"},
              {"The host decides; off: the phantom health cut stays in PvP too",
               "Решает хост; выкл.: штраф здоровья призрака действует и в PvP"});
    host_rule(&Values::party_share_lamps, "Shared lamps", "Общие фонари",
              AddText("A lamp any party member lights is lit for everyone, in each save. Party rule",
                      "Фонарь, зажжённый любым членом группы, горит у всех, в каждом сохранении. Правило группы"),
              [] { return ps::ShareLamps(); },
              {"The host shares lamps: every lamp lit in the party is lit for you too",
               "Хост делит фонари: каждый фонарь группы горит и у вас"},
              {"The host turned shared lamps off: your own lamps stay yours",
               "Хост выключил общие фонари: ваши фонари остаются вашими"});
    // 0.7.2 (user decision 2026-10-21): the host's world keeps its own lamps unless this is on.
    host_rule(&Values::party_guests_bring_lamps, "Guests bring their lamps", "Фонари гостей",
              AddText("A joining guest's own lamps light up in your world too (needs Shared lamps). Party rule",
                      "Фонари из мира гостя загораются и в вашем мире (нужны общие фонари). Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::GuestsBringLamps); },
              {"The host lets guests bring their lamps: yours light up in the host's world",
               "Хост принимает фонари гостей: ваши фонари горят и в его мире"},
              {"The host's world keeps its own lamps; any lamp you light there is saved for all",
               "Мир хоста хранит свои фонари; зажжённый вами там фонарь сохраняется у всех"});

    // CL-48ee07: a Chalice Dungeon lamp lists the dungeon's lit lamps (layers); the host's choice takes the party there.
    host_rule(&Values::party_layer_travel, "Chalice layer travel", "Путь по слоям чаши",
              AddText("The host's Chalice Dungeon lamp lists the dungeon's other lit lamps; the party travels with the host. Party rule",
                      "Фонарь подземелья хоста показывает другие зажжённые фонари; группа идёт с хостом. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::LayerTravel); },
              {"The host allows it: its dungeon lamp offers the other lit lamps and the party follows its choice",
               "Хост разрешил: его фонарь подземелья предлагает другие фонари, группа идёт за выбором хоста"},
              {"The host decides; off: a dungeon lamp only returns to the Hunter's Dream",
               "Решает хост; выкл.: фонарь подземелья только возвращает в Сон охотника"});

    // ---- Party rules: Deaths (party_b2): the host's deaths ----
    AddPage("Party rules: Deaths", "Правила: смерти",
            "Host's rules: the party's deaths: getting up without a load, guests' respawn and refill (bbport)",
            "Правила хоста: смерти в группе: подъём без загрузки, возврат и пополнение гостей (bbport)");
    host_rule(&Values::party_host_revive, "Host gets up, no load", "Хост встаёт сразу",
              AddText("You get up at your lamp without a loading screen for the party. Off: the respawn load. Host's",
                      "Вы встаёте у своего фонаря без экрана загрузки для группы. Выкл.: загрузка. Решает хост"),
              [] { return ps::HostRuleOn(ps::HostRule::HostRevive); },
              {"The host gets up at its lamp without a loading screen for the party (host decides)",
               "Хост встаёт у своего фонаря без экрана загрузки для группы (решает хост)"},
              {"The host decides; off: the whole party gets the respawn loading screen at its death",
               "Решает хост; выкл.: при его смерти вся группа видит экран загрузки"});
    host_rule(&Values::party_host_limbo_revive, "Up after boss kill", "Встать после босса",
              AddText("Dead in a boss fight the others win: you get up in the arena, no load. Host's",
                      "Погибли в бою, а другие убили босса: вы встаёте на арене без загрузки. Решает хост"),
              [] { return ps::HostRuleOn(ps::HostRule::HostLimboRevive); },
              {"Dead in a boss fight the others win: you get up in the arena, no load (host decides)",
               "Погибли в бою, а другие убили босса: вы встаёте на арене без загрузки (решает хост)"},
              {"The host decides; off: a boss kill after a death brings the respawn load / the host's lamp",
               "Решает хост; выкл.: после победы над боссом - загрузка возрождения / фонарь хоста"});
    // 0.7.3 (kidnapall, party_kidnap_rules.h): a Snatcher's grab of a guest takes the party to the Gaol.
    host_rule(&Values::party_any_kidnap, "Anyone can be abducted", "Похитить могут любого",
              AddText("A Snatcher grabbing a guest takes the whole party to the Hypogean Gaol, as the host. Party rule",
                      "Похититель, схвативший гостя, уносит всю группу в Подземную тюрьму, как хоста. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::AnyKidnap); },
              {"The host's rule: a Snatcher grabbing you takes the whole party to the Hypogean Gaol",
               "Правило хоста: если вас схватит похититель, вся группа окажется в Подземной тюрьме"},
              {"The host decides; off: only the host's grab takes the party there, a grabbed guest just dies",
               "Решает хост; выкл.: группу уносит только схваченный хост, схваченный гость просто гибнет"});
    // 0.7.4 (partyrules): the guests' deaths, the host's session rules (were My party: Deaths).
    host_rule(&Values::party_guest_revive_inplace, "Guest gets up, no load", "Гость встаёт сразу",
              AddText("A dead guest gets up at your lamp without a loading screen. Off: the respawn load. Party rule",
                      "Погибший гость встаёт у вашего фонаря без экрана загрузки. Выкл.: загрузка. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::GuestReviveInplace); },
              {"The host's rule: you get up at the host's lamp without a loading screen",
               "Правило хоста: вы встаёте у фонаря хоста без экрана загрузки"},
              {"The host decides; off: every death of yours takes the respawn loading screen",
               "Решает хост; выкл.: каждая ваша смерть - экран загрузки возрождения"});
    host_rule(&Values::party_guest_respawn, "Respawn at host's lamp", "Возврат к фонарю хоста",
              AddText("A dead guest comes back at your lamp and is summoned again, not sent home. Party rule",
                      "Погибший гость возвращается к вашему фонарю и снова призван, а не домой. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::GuestRespawn); },
              {"The host's rule: a death brings you back at the host's lamp, still in the party",
               "Правило хоста: после смерти вы у фонаря хоста, всё ещё в группе"},
              {"The host decides; off: a death sends you home to your own world, as in the game",
               "Решает хост; выкл.: после смерти вы дома, в своём мире, как в игре"});
    host_rule(&Values::party_guest_refill, "Refill vials, bullets", "Пополнять склянки, пули",
              AddText("Guests' vials and bullets refill when you rest, die or travel to the Dream. Party rule",
                      "Склянки и пули гостей пополняются, когда вы отдыхаете, гибнете или уходите во Сон. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::GuestRefill); },
              {"The host's rule: your vials and bullets refill when the host rests, dies or travels",
               "Правило хоста: ваши склянки и пули пополняются, когда хост отдыхает, гибнет или уходит"},
              {"The host decides; off: your vials and bullets refill only as in the game",
               "Решает хост; выкл.: склянки и пули пополняются только как в игре"});

    // ---- Party rules: Travel (0.7.4, partyrules; were My party: World / Story): how the guests move
    // through the host's world. Lamps and the Mark are read at start: a guest must start with the
    // host's (the join check says "set ... like the host"). ----
    AddPage("Party rules: Travel", "Правила: пути",
            "Host's rules: guests' lamps and Hunter's Mark, open world, Chalice Dungeons, cutscenes live (bbport)",
            "Правила хоста: фонари и Метка гостям, открытый мир, Подземелья Чаши, ролики вместе (bbport)");
    host_rule(&Values::party_guest_lamps, "Guests use lamps", "Фонари гостям",
              AddText("Guests may use lamps and headstones; the party travels. Everyone the same. Next start",
                      "Гости пользуются фонарями и надгробиями; группа идёт вместе. У всех одинаково. Со след. запуска"),
              [] { return ps::HostRuleOn(ps::HostRule::GuestLamps); },
              {"The host's rule: you may use lamps and headstones; the whole party travels",
               "Правило хоста: вы можете пользоваться фонарями и надгробиями; группа идёт вместе"},
              {"The host decides; off: only the host uses lamps and headstones",
               "Решает хост; выкл.: фонарями и надгробиями пользуется только хост"});
    host_rule(&Values::party_guest_mark, "Guests use Hunter's Mark", "Метка охотника гостям",
              AddText("A guest's Hunter's Mark asks: alone or with the party. Everyone the same. Next start",
                      "Метка охотника гостя: одному или всей группой. У всех одинаково. Со следующего запуска"),
              [] { return ps::HostRuleOn(ps::HostRule::GuestMark); },
              {"The host's rule: your Hunter's Mark asks: travel alone or with the party",
               "Правило хоста: ваша Метка охотника спрашивает: одному или всей группой"},
              {"The host decides; off: guests cannot use the Hunter's Mark, as in the game",
               "Решает хост; выкл.: гости не могут пользоваться Меткой охотника, как в игре"});
    host_rule(&Values::party_open_world, "Open world", "Открытый мир",
              AddText("No co-op walls on area borders: the party walks anywhere you can. Party rule",
                      "Без стен совместной игры на границах зон: группа ходит везде, где вы. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::OpenWorld); },
              {"The host's rule: no co-op walls on area borders, the party walks anywhere",
               "Правило хоста: без стен на границах зон, группа ходит везде"},
              {"The host decides; off: the game's co-op walls stay on area borders",
               "Решает хост; выкл.: стены совместной игры на границах зон остаются"});
    host_rule(&Values::party_chalice_rejoin, "Stay in Chalice Dungeons", "Остаться в Чаше",
              AddText("A guest out of your dungeon by a death or forced trip is summoned back. Party rule",
                      "Гость вне вашего подземелья после смерти или переноса призывается обратно. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::ChaliceRejoin); },
              {"The host's rule: out of the host's dungeon by a death or forced trip, you are summoned back",
               "Правило хоста: вне подземелья хоста после смерти или переноса - вас призовут обратно"},
              {"The host decides; off: a death in the host's dungeon takes you to your own lamp",
               "Решает хост; выкл.: смерть в подземелье хоста ведёт к вашему фонарю"});
    // CL-a3a9e6: guests may gather the party too; off by default (the guest's Gather the party row follows it).
    host_rule(&Values::party_guest_gather, "Guests can gather", "Гости собирают группу",
              AddText("A guest's Gather the party brings you and the other guests to it (each is asked). Party rule",
                      "Сбор группы гостем переносит вас и других гостей к нему (каждого спросят). Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::GuestGather); },
              {"The host allows it: a guest may gather the party; you are asked each time and may say no",
               "Хост разрешил: гость может собрать группу; вас каждый раз спросят, можно отказаться"},
              {"The host decides; off: guests cannot gather the party",
               "Решает хост; выкл.: гости не могут собирать группу"});
    host_rule(&Values::party_story_mirror, "Cutscenes live with host", "Ролики вместе с хостом",
              AddText("A guest in your world watches a cutscene with you. Off: later, in its own world. Party rule",
                      "Гость в вашем мире смотрит ролик вместе с вами. Выкл.: позже, в своём мире. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::StoryMirror); },
              {"The host's rule: in the host's world you watch its cutscenes with it",
               "Правило хоста: в мире хоста вы смотрите его ролики вместе с ним"},
              {"The host decides; off: you see the host's cutscenes later, in your own world",
               "Решает хост; выкл.: ролики хоста вы увидите позже, в своём мире"});

    // ---- Party rules: Rewards (0.7.4, partyrules; were My party: Rewards): what guests earn in the
    // host's world, the same balance for every player. Read at start: the join check. ----
    AddPage("Party rules: Rewards", "Правила: награды",
            "Host's rules: guests' Blood Echoes, drops and boss Insight (bbport)",
            "Правила хоста: отголоски крови, добыча и озарение за боссов у гостей (bbport)");
    host_rule(&Values::party_full_rewards, "Full rewards for guests", "Полные награды гостям",
              AddText("Guests get full Blood Echoes and enemy drops, like you. Everyone the same. Next start",
                      "Гости получают полные отголоски и добычу, как вы. У всех одинаково. Со следующего запуска"),
              [] { return ps::HostRuleOn(ps::HostRule::FullRewards); },
              {"The host's rule: you get full Blood Echoes and enemy drops, like the host",
               "Правило хоста: вы получаете полные отголоски крови и добычу, как хост"},
              {"The host decides; off: the game's co-op rewards (half echoes, only vials and bullets drop)",
               "Решает хост; выкл.: награды как в игре (половина отголосков, падают только склянки и пули)"});
    // 0.7.6 (bossdrops, owner's decision 2026-10-25): a boss's item drop reaches the guests present at the kill.
    host_rule(&Values::party_boss_drops, "Boss drops for guests", "Добыча с боссов гостям",
              AddText("An item a world boss drops for you also goes to each guest present at the kill, once. Party rule",
                      "Предмет, выпавший с босса, получает и каждый гость, бывший при убийстве, один раз. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::BossDrops); },
              {"The host's rule: the item a boss drops for the host is yours too if you are there at the kill",
               "Правило хоста: предмет с босса достаётся и вам, если вы были при убийстве"},
              {"The host decides; off: you get only what the game gives a guest from a boss",
               "Решает хост; выкл.: вы получаете с босса только то, что даёт игра гостю"});
    {   // A list opens a pop-up over the rows below it: last on the page (after the guest's line). A guest's line shows its own
        // value: the join check makes it the host's.
        GuestRule g{AddDynamic(MaxValueLength), AddDynamic(MaxHelpLength), [] { return true; },
                    {"The host's rule: the Insight you get when the host kills a boss",
                     "Правило хоста: озарение, которое вы получаете, когда хост убивает босса"},
                    {}, [] {
                        static const Texts names[3] = {{"Same as the host", "Как у хоста"},
                                                       {"Full bonus", "Полный бонус"},
                                                       {"Game's own (+1)", "Как в игре (+1)"}};
                        return names[std::clamp(BbSettings::Get().party_guest_insight.load(), 0, 2)];
                    }};
        AddRow(Info(), AddText("Insight for bosses", "Озарение за боссов"), g.help, {g.value}, Kind::Button, is_guest);
        guest_rules.push_back(std::move(g));
        AddRow(Setting(
                   Kind::List, [](const Values& v) { return std::clamp(v.party_guest_insight.load(), 0, 2); },
                   [](int i) { BbSettings::Get().party_guest_insight = std::clamp(i, 0, 2); }),
               AddText("Insight for bosses", "Озарение за боссов"),
               AddText("Insight a guest gets when you kill a boss. Everyone the same. Next start",
                       "Озарение гостю, когда вы убиваете босса. У всех одинаково. Со следующего запуска"),
               {AddText("Same as the host", "Как у хоста"), AddText("Full bonus", "Полный бонус"),
                AddText("Game's own (+1)", "Как в игре (+1)")},
               Kind::List, [is_guest] { return !is_guest(); });
    }

    // ---- Party rules: Rot (party_balance, party_rot_rules.h): the death debuff and what else cleanses it ----
    AddPage("Party rules: Rot", "Правила: гниль",
            "Host's rules: rot on death and what cleanses it: a boss, your killer, your Blood Echoes (bbport)",
            "Правила хоста: гниль при смерти и что её снимает: босс, убийца, отголоски крови (bbport)");
    host_rule(&Values::party_rot, "Rot on death", "Гниль при смерти",
              AddText("Each death stacks a penalty (up to 5), a lamp rest cleanses it. Off by default. Party rule",
                      "Каждая смерть - штраф (до 5 раз), отдых у фонаря снимает. По умолч. выкл. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::Rot); },
              {"The host's rule: each death stacks a penalty (up to 5), a lamp rest cleanses it",
               "Правило хоста: каждая смерть - штраф (до 5 раз), отдых у фонаря снимает"},
              {"The host has rot off: deaths cost nothing more",
               "У хоста гниль выключена: смерти ничего не отнимают"});
    host_rule(&Values::party_rot_cleanse_boss, "Rot: boss kill cleanses", "Гниль: босс снимает",
              AddText("A boss's death cleanses all rot of everyone in the host's world. Needs Rot. Party rule",
                      "Смерть босса снимает всю гниль у всех в мире хоста. Нужна гниль. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::RotCleanseBoss); },
              {"The host's rule: a boss's death cleanses all rot of everyone in its world",
               "Правило хоста: смерть босса снимает всю гниль у всех в его мире"},
              {"The host's rule: off, a boss's death leaves the rot",
               "Правило хоста: выкл., смерть босса гниль не снимает"});
    host_rule(&Values::party_rot_cleanse_killer, "Rot: kill your killer", "Гниль: убить убийцу",
              AddText("The enemy that killed you dies: that death's rot goes. Needs Rot. Party rule",
                      "Враг, убивший вас, погиб: гниль той смерти уходит. Нужна гниль. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::RotCleanseKiller); },
              {"The host's rule: the enemy that killed you dies, that death's rot goes",
               "Правило хоста: враг, убивший вас, погиб - гниль той смерти уходит"},
              {"The host's rule: off, your killer's death leaves the rot",
               "Правило хоста: выкл., смерть убийцы гниль не снимает"});
    host_rule(&Values::party_rot_cleanse_echoes, "Rot: echoes back", "Гниль: вернуть отголоски",
              AddText("Picking up your Blood Echoes removes the rot of the death that dropped them. Needs Rot",
                      "Подобранные отголоски крови снимают гниль той смерти. Нужна гниль. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::RotCleanseEchoes); },
              {"The host's rule: your Blood Echoes picked up remove that death's rot",
               "Правило хоста: подобранные отголоски снимают гниль той смерти"},
              {"The host's rule: off, picking up your Blood Echoes leaves the rot",
               "Правило хоста: выкл., подобранные отголоски гниль не снимают"});


    // ---- Party rules: Enemies (party_balance.h): per extra hunter, the host's values count. ----
    AddPage("Party rules: Enemies", "Правила: сила врагов",
            "Host's rules: boss and enemy health, damage and poise per extra hunter (bbport)",
            "Правила хоста: здоровье, урон и стойкость боссов и врагов за каждого охотника (bbport)");
    const auto scale_row = [&](std::atomic<int> Values::*member, const int* table, const char* en, const char* ru,
                               int index) {
        balance.scale_help[index] = AddDynamic(MaxHelpLength);
        AddRow(Setting(
                   Kind::Slider,
                   [member, table](const Values& v) { return coop::balance::Position(table, (v.*member).load()); },
                   [member, table](int i) { (BbSettings::Get().*member) = coop::balance::AtPosition(table, i); }),
               AddText(en, ru), balance.scale_help[index], {}, Kind::Slider, [is_guest] { return !is_guest(); });
        // guestrules (0.7.3): a guest's read-only line with the host's value
        balance.guest_scale_value[index] = AddDynamic(MaxValueLength);
        balance.guest_scale_help[index] = AddDynamic(MaxHelpLength);
        AddRow(Info(), AddText(en, ru), balance.guest_scale_help[index], {balance.guest_scale_value[index]},
               Kind::Button, is_guest);
    };
    using namespace coop::balance;
    // Health and damage first (changed most), poise last.
    scale_row(&Values::party_boss_hp, kHpTable, "Boss health", "Здоровье боссов", 0);
    scale_row(&Values::party_boss_damage, kDamageTable, "Boss damage", "Урон боссов", 1);
    scale_row(&Values::party_enemy_hp, kHpTable, "Enemy health", "Здоровье врагов", 3);
    scale_row(&Values::party_enemy_damage, kDamageTable, "Enemy damage", "Урон врагов", 4);
    scale_row(&Values::party_boss_poise, kPoiseTable, "Boss poise", "Стойкость боссов", 2);
    scale_row(&Values::party_enemy_poise, kPoiseTable, "Enemy poise", "Стойкость врагов", 5);

    // ---- My party: Bosses (0.7.7, bosstarget): a boss turns on every player in reach in turn. The
    // switch acts on the host's game (the boss AI's owner); a guest's value is unused. ----
    AddPage("My party: Bosses", "Моя группа: боссы",
            "Host's rule: bosses turn on every hunter in reach in turn, not only on one (bbport)",
            "Правило хоста: боссы поочерёдно атакуют каждого охотника поблизости (bbport)");
    own(&Values::party_boss_share, "Bosses share attention", "Боссы делят внимание",
        "A boss in a party fight turns on each hunter in reach in turn instead of keeping to one. Party rule (host)",
        "Босс в бою группы поочерёдно атакует каждого охотника поблизости. Правило группы (хост)");

    // ---- Party rules: Phantoms (0.7.5, levelsync, party_levelsync_rules.h): what a hunter keeps of its
    // stats when it joins a party. Both ON = Bloodborne's own co-op. ----
    AddPage("Party rules: Phantoms", "Правила: призраки",
            "Host's rules: level sync and the phantom health cut, as in the game's own co-op (bbport)",
            "Правила хоста: уравнивание уровней и штраф к здоровью призрака, как в кооперативе самой игры (bbport)");
    host_rule(&Values::party_level_sync, "Level sync", "Уравнивание уровней",
              AddText("A hunter far above the host's level is scaled down, as in the game. Off: full stats. Party rule",
                      "Охотник намного выше уровнем хоста ослабляется, как в игре. Выкл.: полные характеристики. "
                      "Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::LevelSync); },
              {"The host's rule: a hunter above the host's level is scaled down (a blue icon by its name)",
               "Правило хоста: охотник выше уровнем, чем хост, ослабляется (синий значок у имени)"},
              {"The host decides; off: every hunter keeps its full stats whatever its level",
               "Решает хост; выкл.: у каждого охотника полные характеристики, каким бы ни был уровень"});
    host_rule(&Values::party_phantom_hp, "Phantom health cut", "Штраф здоровья призрака",
              AddText("Joining hunters get 30 % less maximum health, as in the game. Off: full health. Party rule",
                      "Присоединившиеся охотники теряют 30 % макс. здоровья, как в игре. Выкл.: полное. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::PhantomHp); },
              {"The host's rule: hunters who join the party have 30 % less maximum health",
               "Правило хоста: у присоединившихся охотников на 30 % меньше максимального здоровья"},
              {"The host decides; off: hunters who join keep their full maximum health",
               "Решает хост; выкл.: присоединившиеся охотники сохраняют полное максимальное здоровье"});
    host_rule(&Values::party_skip_prologue, "Skip the prologue", "Пропустить пролог",
              AddText("A NEW character skips the clinic and first Dream visit: weapons, the first lamp. Party rule",
                      "НОВЫЙ персонаж пропускает клинику и первый Сон: оружие, первый фонарь. Правило группы"),
              [] { return ps::HostRuleOn(ps::HostRule::SkipPrologue); },
              {"The host's rule: a new character skips the prologue (the starting weapons, the first lamp in the city)",
               "Правило хоста: новый персонаж пропускает пролог (начальное оружие, первый фонарь в городе)"},
              {"The host decides; off: every new character plays the prologue. A character past it is never touched",
               "Решает хост; выкл.: каждый новый персонаж проходит пролог. Прошедшего пролог это не затрагивает"});

    // ---- My party: Your save (0.7.4, partyrules): what a party brings into this player's own save
    // (its character): each player's own choice, also as a guest. ----
    AddPage("My party: Your save", "Моя группа: ваше сохр.",
            "Yours: what the party brings into your own save: story progress, NPC quests, cutscenes, endings (bbport)",
            "Ваше: что группа приносит в ваше сохранение: прогресс сюжета, квесты NPC, ролики и концовки (bbport)");
    own(&Values::party_progress, "Share story progress", "Общий прогресс сюжета",
        "Bosses, lamps, shortcuts and key events copied into your save. Host and guests. Next start",
        "Боссы, фонари, срезки и события копируются в ваше сохранение. Хост и гости. Со след. запуска");
    own(&Values::party_progress_npc, "Share NPC quests", "Общие квесты NPC",
        "The first NPC quest choice anyone makes applies to your save too (needs story progress). Next start",
        "Первый выбор в квесте NPC действует и в вашем сохранении (нужен общий прогресс). Со след. запуска");
    own(&Values::party_story, "Share cutscenes, endings", "Общие ролики и концовки",
        "The host's cutscenes, time of day and ending reach your save too. Host and guests. Next start",
        "Ролики хоста, время суток и концовка попадают и в ваше сохранение. Хост и гости. Со след. запуска");

    AddPage("My party: Your items", "Моя группа: предметы",
            "Yours: what the party brings into your own save: the host's item rewards, bells, key items (bbport)",
            "Ваше: что группа приносит в ваше сохранение: предметы хоста, колокольчики, ключи (bbport)");
    own(&Values::party_items, "Host's item rewards", "Предметы хоста гостям",
        "Key and unique items the host gets are given to you too, once (not vials, bullets or materials). Next start",
        "Ключевые и уникальные предметы хоста выдаются и вам, один раз (не флаконы, пули, материалы). Со след. запуска");
    own(&Values::party_grant_bells, "Give the bells", "Выдать колокольчики",
        "The Beckoning Bell and Small Resonant Bell after the prologue (for ringing by hand). Next start",
        "Колокол призыва и Малый резонирующий колокол после пролога (звонить вручную). Со следующего запуска");
    // H4 (0.7.1, party_items): the own key section while summoned, and host-world keys given at once.
    own(&Values::party_key_view, "Own key items as guest", "Свои ключи у гостя",
        "While you are a guest: your own keys and badges show in Key Items. Off: the game's empty list. Next start",
        "Пока вы гость: ваши ключи и значки видны в ключевых предметах. Выкл.: пусто, как в игре. Со след. запуска");
    own(&Values::party_key_items_summoned, "Key items at once", "Ключи сразу",
        "While you are a guest: a key the host's world gives you is yours at once (needs own key items). Off: at home",
        "Пока вы гость: ключ из мира хоста ваш сразу (нужны свои ключи). Выкл.: дома, в своём мире");
    // 0.7.7: "<item>: shared with the party" notice. Not on Party members: four hunters + the last event +
    // Connection warnings already fill its six rows.
    AddRow(Flag(&Values::party_item_notices), "Shared item notices", "Увед. об общих предметах",
           "A short notice when an item is shared with the party (key and unique items, weapons, runes)",
           "Короткое уведомление, когда предмет передан группе (ключевые и уникальные предметы, оружие, руны)", toggle,
           Kind::Toggle);


    // ---- My party: Display: what this player sees of the party (local) ----
    AddPage("My party: Display", "Моя группа: отображение", "Yours: party HUD, name tags, pings, markers, what shows above companions (bbport)",
            "Ваше: панель группы, имена, метки, маркеры, что видно над спутниками (bbport)");
    // Party HUD (party_hud.h): party mode only.
    AddRow(Flag(&Values::party_hud), AddText("Party HUD", "Панель группы"),
           AddText("Companions' names, health and where they are, in the bottom left corner",
                   "Имена, здоровье и местоположение спутников в левом нижнем углу"),
           toggle, Kind::Toggle, [] { return party::status::Enabled(); });
    // Party name tags (party_hud_game.cpp): ours instead of the game's phantom plate, or the game's.
    own(&Values::party_name_tags, "Party name tags", "Имена над спутниками",
        "On: our name plates above companions (never hidden by walls). Off: the game's own",
        "Вкл.: наши таблички над спутниками (не прячутся за стенами). Выкл.: игровые");
    // I2 pings (party_ping.h).
    own(&Values::party_pings, "Pings", "Метки",
        "Touchpad + R3 or the Ping key (default B, Controls) marks where you look; everyone sees it 10 s",
        "Тачпад + R3 или клавиша метки (по умолч. B, Управление) отмечает место; группа видит её 10 с");
    own(&Values::party_through_walls, "Show party through walls", "Группа сквозь стены",
        "A small marker with the distance on each companion on your map, also behind walls",
        "Маленькая метка с расстоянием на каждом спутнике на вашей карте, и за стенами");
    // The marker on the own lost-echoes bloodstain (bbport_overlay PartyStainMarker), off by default.
    own(&Values::party_stain_marker, "Bloodstain marker", "Метка пятна крови",
        "A marker with Blood Echoes and distance on your lost echoes' bloodstain, also behind walls",
        "Метка с отголосками и расстоянием на пятне крови с вашими отголосками, и за стенами");
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


    // ---- My party: Look & notices: how companions are drawn on this screen, the join / leave notices ----
    AddPage("My party: Look & notices", "Моя группа: вид, оповещ.", "Yours: how the other hunters look on your screen, join/leave notices, NPC dialogue (bbport)",
            "Ваше: как другие охотники выглядят у вас, уведомления о входе и выходе, диалоги NPC (bbport)");
    // party_glow.h: the other players' chrs without the phantom glow (display only).
    own(&Values::party_glow_off, "No companion glow", "Спутники без свечения",
        "On: the other hunters look like normal hunters on your screen. Off: the game's blue phantom glow",
        "Вкл.: другие охотники выглядят как обычные. Выкл.: синее свечение фантомов, как в игре");

    own(&Values::party_hud_ping, "Ping in the party list", "Пинг в списке группы",
        "Each companion's connection delay in ms after the name in the bottom left list",
        "Задержка связи каждого спутника в мс после имени в списке слева внизу");
    AddRow(Flag(&Values::party_toasts), "Join/leave notices", "Уведомления",
           "A short notice in a top corner when a hunter joins or leaves",
           "Короткое уведомление в углу, когда охотник входит или выходит", toggle, Kind::Toggle);
    // Shared NPC dialogue (party_npctalk.h): a companion's NPC conversation near you, and how near
    // (0.7.4: from My party: Story; only what you see and hear, nothing of it reaches your save).
    own(&Values::party_npc_talk, "Share NPC dialogue", "Общие диалоги NPC",
        "See and hear the NPC conversations of companions near you (subtitles, choices, gifts)",
        "Видеть и слышать разговоры спутников с NPC рядом с вами (субтитры, выбор, подарки)");
    balance.npc_range_help = AddDynamic(MaxHelpLength);
    AddRow(Setting(
               Kind::Slider,
               [](const Values& v) { return coop::npctalk::RangePosition(v.party_npc_talk_range.load()); },
               [](int i) { BbSettings::Get().party_npc_talk_range = coop::npctalk::RangeAtPosition(i); }),
           AddText("NPC dialogue range", "Дальность диалогов NPC"), balance.npc_range_help, {}, Kind::Slider);
    // 0.7.5: party discovery (net/party_discovery.h; the Join list is in the launcher).
    AddRow(Flag(&Values::party_discovery), "Party discovery", "Поиск групп",
           "Host: announce the party on your VPN / LAN (never the Internet) for the launcher's Join list",
           "Хост: объявлять группу в VPN и LAN (не в Интернет) для списка лаунчера", toggle, Kind::Toggle);

    // ---- My party: Splitscreen (0.7.7, CL-2eec68): several game windows on one screen (Nucleus Co-op). Each
    // player's own; the rules are in party/party_splitscreen_rules.h. ----
    AddPage("My party: Splitscreen", "Моя группа: сплитскрин",
            "Yours: splitscreen mode and which party messages show (bbport)",
            "Ваше: режим раздельного экрана и какие сообщения группы показывать (bbport)");
    own(&Values::party_splitscreen, "Splitscreen mode", "Раздельный экран",
        "For windows sharing a screen: party HUD shows only the compass, no name tags, errors-only messages",
        "Для окон на одном экране: в панели группы только компас, без имён, сообщения только об ошибках");
    // A list opens a pop-up over the rows below it: last on the page.
    AddRow(Setting(
               Kind::List, [](const Values& v) { return std::clamp(v.party_messages.load(), 0, 2); },
               [](int i) { BbSettings::Get().party_messages = std::clamp(i, 0, 2); }),
           AddText("Party messages", "Сообщения группы"),
           AddText("Which party notices show: all, errors only (lost link, failed join) or none. Splitscreen: errors",
                   "Какие сообщения группы видны: все, только ошибки (связь, вход) или никакие. Раздельный экран: ошибки"),
           {AddText("All", "Все"), AddText("Errors only", "Только ошибки"), AddText("Off", "Выкл.")}, Kind::List);
}

/// The party rules' help lines: the current value, and on a guest that the host's rules count.
void RefreshBalance() {
    if (!balance.npc_range_help) return;
    const auto& v = BbSettings::Get();
    {
        const int r = coop::npctalk::RangeAtPosition(coop::npctalk::RangePosition(v.party_npc_talk_range.load()));
        SetDynamic(balance.npc_range_help, "Companions' NPC dialogue reaches you " + coop::npctalk::DescribeRange(r),
                   r > 0 ? "Диалоги спутников с NPC: в пределах " + std::to_string(r) + " м от NPC"
                         : std::string("Диалоги спутников с NPC: на всей карте"));
    }
    for (const GuestRule& g : guest_rules) {
        const bool on = g.on();
        if (g.value_text) {
            const Texts t = g.value_text();
            SetDynamic(g.value, t.en, t.ru);
        } else {
            SetDynamic(g.value, on ? "On" : "Off", on ? "Вкл" : "Выкл");
        }
        const Texts& t = on ? g.on_help : g.off_help;
        SetDynamic(g.help, t.en, t.ru);
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
        SetDynamic(balance.scale_help[k], en + (kind == 1 ? "" : ref_en), ru + (kind == 1 ? "" : ref_ru));
    }
    // A guest's read-only lines: the host's values in force (party_status HostScaling).
    int host[party::status::kScalingValues];
    party::status::HostScaling(host);
    for (int k = 0; k < 6; ++k) {
        if (!balance.guest_scale_value[k]) continue;
        const int kind = k % 3;
        const int value = host[k];
        char shown[16];
        std::snprintf(shown, sizeof shown, kind == 1 ? "+%d %%" : "%d %%", value);
        SetDynamic(balance.guest_scale_value[k], shown, shown);
        const std::string en = kind == 0 ? DescribeHp(value, false)
                               : kind == 1 ? DescribeDamage(value, false) : DescribePoise(value, false);
        const std::string ru = kind == 0 ? DescribeHp(value, true)
                               : kind == 1 ? DescribeDamage(value, true) : DescribePoise(value, true);
        SetDynamic(balance.guest_scale_help[k], "The host's setting: " + en, "Настройка хоста: " + ru);
    }
}

} // namespace

u32 EndingQuestionText() {
    Define();
    return ending_text[0];
}

u32 GatherQuestionText() {
    Define();
    return gather_text[0];
}

u32 GuestGatherQuestionText() {
    Define();
    return guest_gather_text[0];
}

void SetGuestGatherQuestion(const std::string& who) {
    Define();
    const std::u16string w = Utf16(who.c_str());
    SetDynamic16(guest_gather_text[0], w + u" gathers the party.\nYES: go to " + w + u" now.  NO: stay where you are.",
                 w + u" собирает группу.\nДА: перейти к игроку сейчас.  НЕТ: остаться здесь.");
}

u32 TravelVoteQuestionText() {
    Define();
    return vote_text[0];
}

void SetTravelVoteQuestion(const std::string& who, const std::u16string& lamp) {
    Define();
    const std::u16string w = Utf16(who.c_str());
    SetDynamic16(vote_text[0], w + u" wants the party to travel to " + lamp + u".\nYES: travel together.  NO: the party stays.",
                 w + u" хочет вести группу: " + lamp + u".\nДА: идти вместе.  НЕТ: группа остаётся.");
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

namespace {
/// BB_GAME_MENU_TRACE=1 (scripted menu tests, tools/mp/smoke_party.py "rules"): each change a
/// page applies, as "Game menu: row 'NAME' -> VALUE".
bool TraceChanges() {
    static const bool on = [] {
        const char* env = std::getenv("BB_GAME_MENU_TRACE");
        return env && env[0] == '1';
    }();
    return on;
}
std::string Ascii(u32 id) {
    std::string out;
    for (const char16_t* c = Text(id, false); c && *c; ++c) out.push_back(*c < 128 ? char(*c) : '?');
    return out;
}
/// What a row shows in its value column now (English).
std::string ValueText(const Row& r) {
    const int f = r.field;
    switch (r.kind) {
    case Kind::Slider: return std::to_string(int(bytes[std::size_t(f)]));
    case Kind::List: {
        const int v = values[std::size_t(f)];
        return v >= 0 && std::size_t(v) < r.choices.size() ? Ascii(r.choices[std::size_t(v)]) : "?";
    }
    case Kind::Toggle:
    case Kind::Steps: {
        const int count = int(r.choices.size());
        const int c = r.kind == Kind::Toggle ? std::clamp(int(bytes[std::size_t(f)]), 0, count - 1)
                                             : ChoiceFromPosition(bytes[std::size_t(f)], count);
        return count ? Ascii(r.choices[std::size_t(c)]) : "?";
    }
    case Kind::Button: return r.choices.empty() ? "" : Ascii(r.choices[0]);
    }
    return "?";
}
/// A text past its column (MaxNameLength / MaxValueLength) or cut by SetDynamic ("...").
bool Cut(u32 id, std::size_t limit) {
    const char16_t* t = Text(id, false);
    std::u16string_view v = t ? t : u"";
    return v.size() > limit || (IsDynamic(id) && v.size() >= 3 && v.substr(v.size() - 3) == u"...");
}
const Row* RowOfField(int field) {
    for (const Page& p : pages)
        for (const Row& r : p.rows)
            if (r.field == field && (!r.visible || r.visible())) return &r;
    return nullptr;
}
} // namespace

std::string ShownSummary(const Page& page) {
    std::scoped_lock lock{StateMutex()};
    std::string out;
    for (const Row* r : ShownRows(page)) {
        if (!out.empty()) out += " | ";
        out += Ascii(r->name) + "=" + ValueText(*r);
        bool cut = Cut(r->name, MaxNameLength);
        for (u32 c : r->choices) cut = cut || Cut(c, MaxValueLength);
        if (cut) out += " (CUT)";
    }
    return out;
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
            if (TraceChanges())
                if (const Row* r = RowOfField(int(i)))
                    std::printf("Game menu: row '%s' -> %s\n", Ascii(r->name).c_str(), ValueText(*r).c_str());
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
