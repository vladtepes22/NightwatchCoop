// SPDX-License-Identifier: GPL-3.0-or-later
// Party text chat, the game side (party_chat.h): the chat key / pad combo -> the overlay's text box (bbgpu_text_input_*) ->
// EVENT party_chat; incoming events -> the line log the overlay draws. Main thread (the director's tick, once a frame);
// OnEvent on the link's callback thread. The text of a line is never logged (privacy): only slots and lengths.
#include "party_chat.h"
#include "party_hud.h"
#include "party_link.h"
#include "party_status.h"
#include "party_async_io.h"
#include "party_display_name.h"
#include "game_state.h"
#include "../bbport_settings.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

extern "C" {
int bbgpu_text_input_begin(const char* initial_utf8, const char* prompt_utf8);
int bbgpu_text_input_begin_list(const char* initial_utf8, const char* prompt_utf8, const char* choices);
int bbgpu_text_input_poll(char* out_utf8, std::uint64_t size);
int bbgpu_text_input_is_active(void);
int bbgpu_text_input_test_queue(const char* keys);
}

namespace coop::chat {

namespace {

namespace C = party::chat;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    asyncio::Print(std::string("Party: chat: ") + line + "\n");
}

std::mutex g_mu;
std::deque<std::pair<int, std::string>> g_in;
std::deque<std::string> g_test_send;
std::atomic<int> g_open_request{0};  // 0 none, 1 keyboard box, 2 pad list
std::atomic<const char*> g_source{"?"};

struct State {
    bool editing = false;
    bool had_link = false;
    bool was_on = false;
    std::int64_t world_since_ms = 0;  // BB_PARTY_CHAT_TEST clock
    C::SendGap gap;
    C::Rate relay{C::kRelayBurst, C::kRelayWindowS};
    C::Rate receive{C::kRelayBurst + 2, C::kRelayWindowS};
};
State& S() {
    static State s;
    return s;
}

/// BB_PARTY_CHAT_TEST="S:type=Hello_there,S:pick=2,S:recv=0,S:hide=0": S seconds after this game is in a world with a party,
/// type: opens the box and types the text (_ is a space) + Enter; pick: opens the pad's preset list, N Down presses +
/// Enter; recv / hide: sets "Show others' chat" / "Hide offensive names" (not saved). Smoke aid (smoke_party.py pc).
struct TestAct {
    double at_s = 0;
    std::string kind, arg;
    bool done = false;
};
std::vector<TestAct>& TestActs() {
    static std::vector<TestAct> acts = [] {
        std::vector<TestAct> v;
        const char* env = std::getenv("BB_PARTY_CHAT_TEST");
        std::string text = env ? env : "";
        std::size_t i = 0;
        while (i < text.size()) {
            std::size_t j = text.find(',', i);
            if (j == std::string::npos) j = text.size();
            const std::string item = text.substr(i, j - i);
            i = j + 1;
            const std::size_t colon = item.find(':'), eq = item.find('=');
            if (colon == std::string::npos || eq == std::string::npos || eq < colon) continue;
            TestAct a;
            a.at_s = std::atof(item.substr(0, colon).c_str());
            a.kind = item.substr(colon + 1, eq - colon - 1);
            a.arg = item.substr(eq + 1);
            v.push_back(a);
        }
        return v;
    }();
    return acts;
}

void RunTestActs(State& st, double elapsed_s) {
    for (TestAct& a : TestActs()) {
        if (a.done || elapsed_s < a.at_s) continue;
        a.done = true;
        if (a.kind == "type") {
            std::string keys = a.arg;
            for (char& c : keys)
                if (c == '_') c = ' ';
            keys += '\n';
            bbgpu_text_input_test_queue(keys.c_str());
            g_source.store("test");
            g_open_request.store(1);
        } else if (a.kind == "pick") {
            std::string keys(std::size_t(std::max(0, std::atoi(a.arg.c_str()))), '\x02');
            keys += '\n';
            bbgpu_text_input_test_queue(keys.c_str());
            g_source.store("test");
            g_open_request.store(2);
        } else if (a.kind == "recv") {
            BbSettings::Get().party_chat_receive = a.arg != "0";
        } else if (a.kind == "hide") {
            BbSettings::Get().party_hide_bad_names = a.arg != "0";
        }
        Log("test action %s done", a.kind.c_str());
        (void)st;
    }
}

std::string SlotName(party::PartyLink* link, int slot) {
    if (link) {
        for (const party::RosterEntry& r : link->roster())
            if (r.slot == slot) return r.name;
    }
    return std::string();
}

/// How many words MaskWords changed (for the log: a count, never the text).
std::size_t MaskedWords(const std::string& a, const std::string& b) {
    std::size_t n = 0, i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        std::size_t ie = a.find(' ', i), je = b.find(' ', j);
        if (ie == std::string::npos) ie = a.size();
        if (je == std::string::npos) je = b.size();
        if (a.compare(i, ie - i, b, j, je - j) != 0) ++n;
        i = ie + 1;
        j = je + 1;
    }
    return n;
}

void Show(const C::ChatEvent& e, std::int64_t left_ms, bool mine) {
    C::Line l;
    l.slot = e.slot;
    l.mine = mine;
    std::snprintf(l.name, sizeof l.name, "%s", e.who.c_str());
    l.text = e.text;
    l.masked = C::MaskWords(e.text);
    l.until_ms = party::hud::NowMs() + left_ms;
    C::Push(l);
}

void Send(party::PartyLink* link, State& st, const std::string& raw, const char* how) {
    const std::string text = C::Sanitise(raw);
    if (text.empty()) {
        Log("nothing to send (%s)", how);
        return;
    }
    if (!st.gap.Take(party::hud::NowMs())) {
        Log("line (%s) dropped: less than %lld ms after the last one", how, static_cast<long long>(C::kSendGapMs));
        return;
    }
    C::ChatEvent e;
    e.text = text;
    e.t = link->host_clock_ms();
    e.slot = link->local_slot();
    e.who = SlotName(link, e.slot);
    if (e.who.empty()) e.who = link->is_host() ? "Host" : "Hunter";
    Log("sent a line of %zu characters (%s)", C::CharCount(text), how);
    if (link->is_host()) {
        link->post_event(party::kBroadcast, C::kEventName, C::ToJson(e));
    } else {
        C::ChatEvent up = e;  // the host fills in the slot and the name from its roster (never trusted from a guest)
        up.slot = -1;
        up.who.clear();
        link->post_event(party::kHostSlot, C::kEventName, C::ToJson(up));
    }
    Show(e, C::kLifeMs, true);
}

void Incoming(party::PartyLink* link, State& st) {
    std::deque<std::pair<int, std::string>> in;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        in.swap(g_in);
    }
    const double now_s = double(party::hud::NowMs()) / 1000.0;
    for (const auto& [slot, body] : in) {
        C::ChatEvent e;
        std::string err;
        if (!C::FromJson(body, &e, &err)) {
            Log("event from slot %d refused: %s", slot, err.c_str());
            continue;
        }
        if (link->is_host()) {
            if (slot == party::kHostSlot || slot < 0 || slot >= C::kMaxSlots) continue;
            if (!Enabled()) continue;  // the host's setting decides whether the party has chat
            if (!st.relay.Take(slot, now_s)) {
                Log("line from slot %d dropped: more than %d in %.0f s", slot, C::kRelayBurst, C::kRelayWindowS);
                continue;
            }
            e.slot = slot;  // the host names the sender
            e.who = SlotName(link, slot);
            if (e.who.empty()) continue;  // not in the roster (any more)
            const std::string relay = C::ToJson(e);
            for (const party::RosterEntry& r : link->roster()) {
                if (r.slot != slot && r.slot != party::kHostSlot && r.connected) {
                    link->post_event(r.slot, C::kEventName, relay);
                }
            }
        } else {
            if (slot != party::kHostSlot) continue;
            if (e.slot < 0) e.slot = party::kHostSlot;
            if (e.slot == link->local_slot()) continue;  // our own back
            if (e.who.empty()) e.who = SlotName(link, e.slot);
            if (e.who.empty()) continue;
            if (!st.receive.Take(e.slot, now_s)) {
                Log("line from slot %d dropped: more than %d in %.0f s", e.slot, C::kRelayBurst + 2, C::kRelayWindowS);
                continue;
            }
        }
        if (!Enabled() || !Receiving()) {
            Log("line from slot %d (%zu characters) not shown: %s", e.slot, C::CharCount(e.text),
                Enabled() ? "others' chat is muted" : "chat is off");
            continue;
        }
        const std::int64_t left = C::LifeLeft(e.t, link->host_clock_ms());
        if (left <= 0) {
            Log("line from slot %d dropped: older than %lld ms", e.slot, static_cast<long long>(C::kLifeMs));
            continue;
        }
        const std::size_t masked = MaskedWords(e.text, C::MaskWords(e.text));
        if (masked && BbSettings::Get().party_hide_bad_names)
            Log("line from slot %d (%zu characters, %zu word(s) masked)", e.slot, C::CharCount(e.text), masked);
        else
            Log("line from slot %d (%zu characters)", e.slot, C::CharCount(e.text));
        Show(e, left, false);
    }
}

}  // namespace

bool Enabled() {
    static const char* const env = std::getenv("BB_PARTY_CHAT");  // over the setting (tests)
    const bool on = env && *env ? env[0] != '0' : BbSettings::Get().party_chat.load();
    return on && party::status::Enabled();
}

bool Receiving() {
    return BbSettings::Get().party_chat_receive.load();
}

std::uint32_t ChatKey() {
    static const std::uint32_t key = [] {
        std::string name;
        if (const char* env = std::getenv("BB_PARTY_CHAT_KEY"); env && *env) {
            name = env;
        } else {
            for (const auto& [k, v] : BbSettings::ReadLines("party_chat_key"))
                if (k == "party_chat_key") name = v;
        }
        if (name.empty()) return std::uint32_t(SDLK_T);
        SDL_Keycode code = SDL_GetKeyFromName(name.c_str());
        if (code == SDLK_UNKNOWN) {
            Log("unknown chat key '%s': using T", name.c_str());
            return std::uint32_t(SDLK_T);
        }
        return std::uint32_t(code);
    }();
    return key;
}

bool RequestOpen(bool with_presets, const char* source) {
    if (!Enabled()) return false;
    g_source.store(source ? source : "?");
    g_open_request.store(with_presets ? 2 : 1);
    return true;
}

void RequestSend(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_test_send.size() < 8) g_test_send.push_back(text);
}

void OnEvent(int slot, const std::string& body) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_in.size() < 64) g_in.emplace_back(slot, body);
}

void Tick(const GameSnapshot& s, party::PartyLink* link, bool in_world) {
    State& st = S();
    if (!link) {
        if (st.had_link) C::ClearAll();
        st.had_link = false;
        st.editing = false;
        g_open_request.store(0);
        std::lock_guard<std::mutex> lk(g_mu);
        g_in.clear();
        g_test_send.clear();
        return;
    }
    st.had_link = true;
    const bool on = Enabled();
    if (st.was_on && !on) C::ClearAll();
    st.was_on = on;
    Incoming(link, st);
    if (!on) {
        g_open_request.store(0);
        st.editing = false;
        return;
    }

    // The box is open: its answer.
    if (st.editing) {
        char text[512];
        const int state = bbgpu_text_input_poll(text, sizeof text);
        if (state == 0) return;
        st.editing = false;
        if (state == 1) Send(link, st, text, "typed");
        else Log("cancelled");
    }

    if (!in_world || !s.world_up || s.loading) {
        st.world_since_ms = 0;
    } else {
        const std::int64_t now = party::hud::NowMs();
        if (!st.world_since_ms) st.world_since_ms = now;
        RunTestActs(st, double(now - st.world_since_ms) / 1000.0);
    }

    std::deque<std::string> tests;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        tests.swap(g_test_send);
    }
    for (const std::string& t : tests) Send(link, st, t, "test");

    const int req = g_open_request.exchange(0);
    if (!req) return;
    const char* source = g_source.load();
    if (!in_world || !s.world_up || s.loading) {
        Log("chat (%s) ignored: not in a world", source);
        return;
    }
    if (party::hud::GameMenu()) {
        Log("chat (%s) ignored: a game menu is open", source);
        return;
    }
    if (bbgpu_text_input_is_active()) return;
    bool ok;
    if (req == 2) {
        std::string choices;
        for (const std::string& p : C::PresetList()) choices += p + "\n";
        ok = bbgpu_text_input_begin_list("", "Party chat - D-pad picks, Cross sends, Circle cancels (keyboard: type, Enter sends)",
                                         choices.c_str()) != 0;
    } else {
        ok = bbgpu_text_input_begin("", "Party chat - type a line, Enter sends, Esc cancels") != 0;
    }
    if (ok) {
        st.editing = true;
        Log("box opened (%s)", source);
    }
}

}  // namespace coop::chat

extern "C" int bbgpu_chat_enabled(void) {
    return coop::chat::Enabled() ? 1 : 0;
}

extern "C" void bbgpu_chat_request_pad(const char* source) {
    coop::chat::RequestOpen(true, source);
}
