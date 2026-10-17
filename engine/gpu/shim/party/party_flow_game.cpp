// SPDX-License-Identifier: GPL-3.0-or-later
// The in-game party flow's game glue (party_flow.h): party.json in the user folder, the current
// context, the relaunch, the text box and the test driver. Nothing here touches the game's code:
// the only game reads are two pointers (is a character loaded, its save slot), through the
// party library's fault-safe guest_read.
#include "party_flow.h"
#include "party_async_io.h"

#include "party_code.h"
#include "party_runtime.h"
#include "party_status.h"
#include "test_newgame.h"
#include "../bbport_game_menu.h"
#include "../net/bbnet_internal.h"
#include "../../bbnet.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

extern "C" {
const char* runtime_file_user_dir(void);    // src/runtime_file.c
const char* runtime_savepolicy_dir(void);   // src/runtime_savepolicy.c
void runtime_relaunch(int exit_code);       // src/probe.c
int bbgpu_text_input_begin(const char* initial_utf8, const char* prompt_utf8);
int bbgpu_text_input_poll(char* out_utf8, unsigned long long size);
int bbgpu_text_input_is_active(void);
int bbgpu_text_input_test_type(const char* keys);
void runtime_pad_director_press(std::uint32_t buttons);  // src/runtime_pad.c
#include "../../../src/save_policy.h"            // runtime_savepolicy_check: slots in the party saves
}

namespace party::flow {

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kWorldChrMan = 0x553e878;  // game_state.h: +0x60 the local PlayerIns
constexpr std::uint64_t kGameDataMan = 0x553b130;  // +0x50 -> +2000: the last used save slot

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party flow: ") + buf + "\n"); // never blocks the caller (party_async_io.h)
}

const char* Env(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? v : nullptr;
}

std::string UserDir() {
    const char* u = runtime_file_user_dir();
    if (u && *u) return u;
    if (const char* e = Env("BB_USER_DIR")) return e;
    return "user";
}
std::string SettingsPath() { return (fs::path(UserDir()) / kSettingsFile).string(); }

std::mutex g_mu;
bool g_loaded = false;
Saved g_saved;
std::string g_message;
std::atomic<bool> g_relaunching{false};

Saved LoadedLocked() {
    if (!g_loaded) {
        g_loaded = true;
        std::string err;
        if (!Load(SettingsPath(), &g_saved, &err)) {
            g_saved = Saved{};
        } else if (g_saved.password_plain) {
            // A hand-written (or launcher) plain password: protected from now on.
            std::string e2;
            Store(SettingsPath(), g_saved, &e2);
            g_saved.password_plain = false;
        }
        // A party character exists when the party saves hold one, also when party.json does not
        // know it (a launcher-started party game: NoteCharacterInWorld only runs in the menu flow's
        // own processes): Continue party game shows (party.json keeps it with its next change).
        if (!g_saved.has_character) {
            const std::string folder = (fs::path(UserDir()) / "savedata_party").string();
            SavePolicyCheck check{};
            std::error_code ec;
            if (fs::is_directory(folder, ec) && runtime_savepolicy_check(folder.c_str(), nullptr, 0, &check) == 0 &&
                check.slots_used > 0) {
                g_saved.has_character = true;
                Log("the party saves hold %d character(s): Continue party game is offered", check.slots_used);
            }
        }
    }
    return g_saved;
}

void StoreLocked(const Saved& s) {
    g_saved = s;
    g_loaded = true;
    std::string err;
    if (!Store(SettingsPath(), s, &err)) Log("cannot save %s: %s", SettingsPath().c_str(), err.c_str());
}

template <class T>
bool Read(std::uint64_t off, T* out) {
    const std::uintptr_t base = bbnet::guest_image_base();
    return base && bbnet::guest_read(base + off, out, sizeof(T));
}

bool WorldUp() {
    std::uint64_t man = 0, player = 0;
    return Read(kWorldChrMan, &man) && man && bbnet::guest_read(man + 0x60, &player, 8) && player;
}

int LastSlot() {
    std::uint64_t gdm = 0, p = 0;
    std::uint32_t slot = 0xffffffffu;
    if (!Read(kGameDataMan, &gdm) || !gdm || !bbnet::guest_read(gdm + 0x50, &p, 8) || !p ||
        !bbnet::guest_read(p + 2000, &slot, 4) || slot >= 10)
        return -1;
    return int(slot);
}

void SetEnv(const std::string& k, const std::string& v) {
#if defined(_WIN32)
    _putenv_s(k.c_str(), v.c_str());  // an empty value removes it
#else
    if (v.empty()) unsetenv(k.c_str());
    else setenv(k.c_str(), v.c_str(), 1);
#endif
}

bool MenuRun() {
    const char* m = Env("BB_PARTY_MENU");
    return m && m[0] == '1';
}

// The text box.
std::atomic<int> g_edit{-1};
// BB_PARTY_FLOW_TEST ran this action: the next start gets BB_PARTY_FLOW_TEST_NEXT instead (once).
std::atomic<bool> g_test_action{false};

}  // namespace

bool Enabled() {
    const char* e = Env("BB_PARTY_FLOW");
    return !e || e[0] != '0';
}

Saved Current() {
    std::lock_guard<std::mutex> lk(g_mu);
    return LoadedLocked();
}

Context CurrentContext() {
    Context c;
    const bbnet::Settings& s = bbnet::settings();
    c.party_process = s.party;
    c.host = s.host;
    const char* dir = runtime_savepolicy_dir();
    c.party_save = dir && std::strcmp(dir, "savedata_party") == 0;
    c.in_world = WorldUp();
    if (c.party_process) {
        const LinkState st = runtime_status().state;
        c.link_active = st == LinkState::Hosting || st == LinkState::Connecting || st == LinkState::Connected ||
                        st == LinkState::Reconnecting;
    }
    c.saved = Current();
    return c;
}

std::string LastMessage() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_message;
}

Plan Run(Action a) {
    Context c = CurrentContext();
    if (c.saved.name.empty() && !Env("BB_PARTY_NAME")) {
        // A name of its own (party.json keeps it): the default "Hunter" was the same for everyone.
        const char* user = Env("BB_USER_NAME");
        if (!user) user = Env("USERNAME");
        if (!user) user = Env("USER");
        c.saved.name = DefaultName(user ? user : "", std::random_device{}());
        Log("party name: %s (automatic; Party name changes it)", c.saved.name.c_str());
    }
    Plan p = PlanAction(a, c);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_message = p.ok() ? p.message : p.error;
    }
    Log("%s (%s, %s, %s): %s", ActionName(a), c.party_process ? (c.host ? "party host" : "party guest") : "solo",
        c.party_save ? "party save" : "single-player save", c.in_world ? "in the world" : "title",
        p.ok() ? p.message.c_str() : ("refused: " + p.error).c_str());
    if (!p.ok()) return p;
    if (p.local == Plan::Local::Rejoin) {
        party::status::RequestRejoin();
        return p;
    }
    if (p.local == Plan::Local::Leave) {
        party::status::RequestLeave();
        return p;
    }
    if (!p.relaunch || g_relaunching.exchange(true)) return p;
    if (g_test_action) {
        const char* next = Env("BB_PARTY_FLOW_TEST_NEXT");
        p.env.push_back({"BB_PARTY_FLOW_TEST", next ? next : ""});
        p.env.push_back({"BB_PARTY_FLOW_TEST_NEXT", ""});
    }
    std::string bat, err;
    if (!RelaunchBat(p, &bat, &err)) {
        Log("relaunch refused: %s", err.c_str());
        g_relaunching = false;
        return p;
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        StoreLocked(p.saved);
    }
    const fs::path file = fs::path(UserDir()) / kRelaunchFile;
    if (const char* loop = Env("BB_RELAUNCH_LOOP"); loop && loop[0] == '1') {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f << bat;
    }
    // This process's environment too: the fallback restart (runtime_restart, no run.bat loop)
    // inherits it.
    for (const auto& [k, v] : p.env) SetEnv(k, v);
    Log("relaunching (exit code %d, %s)", kRelaunchExitCode, file.string().c_str());
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));  // the page shows the message
        bbnet_shutdown();  // BYE, UPnP unmapped, the crash marker removed: a clean end
        runtime_relaunch(kRelaunchExitCode);
    }).detach();
    return p;
}

bool Editing() { return g_edit.load() >= 0; }

bool BeginEdit(Field f) {
    if (Editing() || bbgpu_text_input_is_active()) return false;
    const Saved s = Current();
    const char* prompt = f == Field::Address ? "Party code from the host, like " PARTY_CODE_EXAMPLE ", or host:port (Ctrl+V pastes)"
                         : f == Field::Password ? "Party password (empty = none; stored protected)"
                                                : "Party name (1-16 of A-Z a-z 0-9 _ -)";
    const std::string initial = f == Field::Address ? s.host_address : f == Field::Name ? s.name : std::string();
    if (!bbgpu_text_input_begin(initial.c_str(), prompt)) return false;
    g_edit = int(f);
    return true;
}

void Poll() {
    const int f = g_edit.load();
    if (f < 0) return;
    char text[512];
    const int state = bbgpu_text_input_poll(text, sizeof text);
    if (state == 0) return;
    g_edit = -1;
    if (state != 1) return;  // cancelled
    std::lock_guard<std::mutex> lk(g_mu);
    Saved s = LoadedLocked();
    switch (Field(f)) {
    case Field::Address: {
        const std::string a = NormalizeAddress(text);
        PartyCode code;
        std::string err;
        if (!a.empty() && !decode_party_code(a, &code, &err)) {
            g_message = "Not a party code or address: " + err;
            return;
        }
        s.host_address = a;
        g_message = a.empty() ? "Host address cleared" : "Host address saved";
        if (!a.empty() && !code.plain && (code.flags & kCodeFlagPasswordRequired) && s.password.empty())
            g_message = "Saved. This party needs a password: set Password";
        break;
    }
    case Field::Password:
        s.password = text;
        g_message = s.password.empty() ? "No password" : "Password saved (protected)";
        break;
    case Field::Name: {
        std::string n = text;
        const bool ok = !n.empty() && n.size() <= 16 &&
                        std::all_of(n.begin(), n.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '-'; });
        if (!ok) {
            g_message = "A name is 1-16 of A-Z a-z 0-9 _ -";
            return;
        }
        s.name = n;
        g_message = "Party name saved";
        break;
    }
    }
    StoreLocked(s);
    Log("%s", g_message.c_str());
}

std::string RuntimeCode() { return MenuRun() ? Current().host_address : std::string(); }
std::string RuntimePassword() { return MenuRun() ? Current().password : std::string(); }
std::string RuntimeName() { return MenuRun() ? Current().name : std::string(); }
std::string RuntimeHostSecret() { return MenuRun() ? Current().host_secret : std::string(); }

void StoreHostSecret(const std::string& hex) {
    if (!MenuRun()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    Saved s = LoadedLocked();
    if (s.host_secret == hex) return;
    s.host_secret = hex;
    StoreLocked(s);
}

int CurrentSaveSlot() { return LastSlot(); }

void NoteCharacterInWorld() {
    const char* dir = runtime_savepolicy_dir();
    if (!bbnet::settings().party || !dir || std::strcmp(dir, "savedata_party") != 0) return;
    const int slot = LastSlot();
    std::lock_guard<std::mutex> lk(g_mu);
    Saved s = LoadedLocked();
    if (s.has_character && s.slot == slot) return;
    if (s.slot >= 0 && slot >= 0 && s.slot != slot) Log("the party character is now in save slot %d (was %d)", slot, s.slot);
    s.has_character = true;
    s.slot = slot;
    if (s.role.empty()) s.role = bbnet::settings().host ? "host" : "join";
    StoreLocked(s);
    Log("party character in the world (save slot %d): Continue party game will load it", slot);
}

namespace {

/// BB_TEST_MENU_FILE (test aid, tools/mp/flow_live.py): a menu walk through the REAL menus for this
/// process. Each start of the game takes the file's first line (and removes it), so the run.bat
/// relaunch loop's next process gets the next one: "<title|world|joined> <seconds> <MenuScript>"
/// (after that many seconds at the title / in the world / in the world with the party together;
/// MenuScript tokens, test_newgame.h) or "-" (no
/// walk this time). type= / enter / esc go to the text box as SDL key events (no OS input).
struct MenuTest {
    bool loaded = false, active = false, in_world = false, joined = false, done = false;
    double delay = 0;
    coop::MenuScript script;
    std::chrono::steady_clock::time_point since{};
};
MenuTest g_menu_test;

void LoadMenuTest() {
    MenuTest& m = g_menu_test;
    m.loaded = true;
    const char* path = Env("BB_TEST_MENU_FILE");
    if (!path) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::string first, line, rest;
    bool got = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!got) {
            first = line;
            got = true;
        } else {
            rest += line + "\n";
        }
    }
    in.close();
    if (!got) return;
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << rest;
    }
    if (first.empty() || first == "-") {
        Log("test menu: no walk for this start");
        return;
    }
    char where[16] = {0};
    double delay = 0;
    int used = 0;
    if (std::sscanf(first.c_str(), "%15s %lf %n", where, &delay, &used) < 2 || used <= 0) {
        Log("test menu: bad line '%s'", first.c_str());
        return;
    }
    std::string bad;
    if (!m.script.Parse(first.substr(std::size_t(used)), &bad)) {
        Log("test menu: unknown token '%s'", bad.c_str());
        return;
    }
    m.joined = std::strcmp(where, "joined") == 0;  // in the world and together (summoned / members in)
    m.in_world = m.joined || std::strcmp(where, "world") == 0;
    m.delay = delay;
    m.active = true;
    Log("test menu: %zu steps %s after %.0f s: %s", m.script.size(), m.in_world ? "in the world" : "at the title",
        delay, first.c_str() + used);
}

void MenuTestTick(bool at_title, bool world_up) {
    MenuTest& m = g_menu_test;
    if (!m.loaded) LoadMenuTest();
    if (!m.active || m.done) return;
    const auto now = std::chrono::steady_clock::now();
    if (m.since == std::chrono::steady_clock::time_point{}) {
        // The title: once its menu is up (its texts looked up; the boot, logos and the
        // "Creating save data" / network checks come before).
        const bool title_menu = BbGameMenu::TextSeenMs("New Game") > 0 || BbGameMenu::TextSeenMs("Continue") > 0;
        const bool together = party::status::Snapshot().state == party::status::State::Joined;
        if (!(m.in_world ? world_up && (!m.joined || together) : at_title && title_menu)) return;
        m.since = now;
        Log("test menu: %s; starting in %.0f s", m.in_world ? "in the world" : "title menu up", m.delay);
    }
    if (std::chrono::duration<double>(now - m.since).count() < m.delay) return;
    std::uint64_t seq = 0;
    std::int64_t last_ms = 0;
    BbGameMenu::TextActivity(&seq, &last_ms);
    const std::int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const double t = double(now_ms) / 1000.0;
    std::string line;
    const std::uint32_t held = m.script.Step(t, seq, seq ? double(now_ms - last_ms) / 1000.0 : 1e9, &line,
                                             [](const std::string& text) {
                                                 const std::int64_t ms = BbGameMenu::TextSeenMs(text.c_str());
                                                 return ms > 0 ? double(ms) / 1000.0 : -1.0;
                                             });
    runtime_pad_director_press(held);
    if (!line.empty()) Log("test menu: %s", line.c_str());
    std::string keys;
    if (m.script.TakeTyped(&keys)) {
        const bool box = bbgpu_text_input_is_active() != 0;
        Log("test menu: %zu key(s) into the text box%s", keys.size(), box ? "" : " (NOT open)");
        bbgpu_text_input_test_type(keys.c_str());
    }
    if (m.script.finished()) {
        runtime_pad_director_press(0);
        m.done = true;
        Log("test menu: %s", m.script.stuck() ? "STUCK" : "done");
    }
}

}  // namespace

void Tick() {
    Poll();
    static bool was_up = false;
    const bool up = WorldUp();
    if (up && !was_up && bbnet::settings().party) NoteCharacterInWorld();
    was_up = up;
    TestTick(!up, up);
    MenuTestTick(!up, up);
}

void TestTick(bool at_title, bool world_up) {
    static bool done = false;
    static std::chrono::steady_clock::time_point since{};
    if (done) return;
    const char* t = Env("BB_PARTY_FLOW_TEST");
    if (!t) {
        done = true;
        return;
    }
    // "<action>[@seconds]": once, after that many seconds at the title (default 15); "world:"
    // prefix: in the world instead.
    std::string spec = t;
    bool in_world = false;
    if (spec.rfind("world:", 0) == 0) {
        in_world = true;
        spec = spec.substr(6);
    }
    double delay = 15;
    if (const auto at = spec.find('@'); at != std::string::npos) {
        delay = std::atof(spec.c_str() + at + 1);
        spec.resize(at);
    }
    const bool ready = in_world ? world_up : (at_title && !world_up);
    const auto now = std::chrono::steady_clock::now();
    if (!ready) {
        since = {};
        return;
    }
    if (since == std::chrono::steady_clock::time_point{}) since = now;
    if (std::chrono::duration<double>(now - since).count() < delay) return;
    done = true;
    static const struct {
        const char* name;
        Action a;
    } kActions[] = {{"new", Action::NewParty},   {"join", Action::JoinParty},   {"continue", Action::ContinueParty},
                    {"host", Action::HostParty}, {"leave", Action::LeaveParty}, {"solo", Action::SinglePlayer}};
    if (spec == "auto") spec = Current().role == "join" ? "join" : "new";  // per party.json's role
    for (const auto& k : kActions) {
        if (spec == k.name) {
            Log("test: %s", k.name);
            g_test_action = true;
            Run(k.a);
            g_test_action = false;
            return;
        }
    }
    Log("test: unknown action '%s'", spec.c_str());
}

}  // namespace party::flow
