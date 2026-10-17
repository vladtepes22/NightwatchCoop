// SPDX-License-Identifier: GPL-3.0-or-later
// Party status board: see party_status.h.
#include "party_status.h"

#include "../../party_env.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace party::status {

namespace {

constexpr std::size_t kMaxCommands = 16;

std::mutex mutex;
Board board;  // guarded by mutex
std::deque<Command> commands;  // guarded by mutex
std::atomic<std::uint64_t> event_seq{0};
std::atomic<std::uint64_t> alert_seq{0};
std::atomic<int> enabled_override{-1};  // -1: from BB_PARTY

bool EnvEnabled() {
    static const bool on = [] {
        return bb_party_enabled() != 0;
    }();
    return on;
}

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Queue(Command cmd) {
    // Coalesce: the same request already waiting is not queued twice.
    for (const Command& c : commands) {
        if (c.type == cmd.type && c.name == cmd.name) {
            return;
        }
    }
    if (commands.size() >= kMaxCommands) {
        commands.pop_front();
    }
    commands.push_back(std::move(cmd));
}

}  // namespace

const char* RoleName(Role role) {
    switch (role) {
    case Role::Host: return "host";
    case Role::Guest: return "guest";
    default: return "off";
    }
}

const char* StateName(State state) {
    switch (state) {
    case State::Off: return "off";
    case State::Starting: return "starting";
    case State::Hosting: return "hosting";
    case State::Connecting: return "connecting";
    case State::WaitingForWorld: return "waiting_for_world";
    case State::RingingBell: return "ringing_bell";
    case State::Joined: return "joined";
    case State::Travelling: return "travelling";
    case State::Reconnecting: return "reconnecting";
    case State::Error: return "error";
    }
    return "?";
}

const char* CommandName(CommandType type) {
    switch (type) {
    case CommandType::Leave: return "leave";
    case CommandType::Rejoin: return "rejoin";
    case CommandType::Kick: return "kick";
    case CommandType::AddGuestCode: return "add_guest_code";
    }
    return "?";
}

void SetRole(Role role) {
    std::scoped_lock lock{mutex};
    if (board.role != role) {
        board.role = role;
        ++board.version;
    }
}

void SetCode(const std::string& code) {
    std::scoped_lock lock{mutex};
    if (board.code != code) {
        board.code = code;
        ++board.version;
    }
}

void SetLanCode(const std::string& code) {
    std::scoped_lock lock{mutex};
    if (board.lan_code != code) {
        board.lan_code = code;
        ++board.version;
    }
}

void SetNetwork(const std::string& text) {
    std::scoped_lock lock{mutex};
    if (board.network != text) {
        board.network = text;
        ++board.version;
    }
}

void SetGuestCode(const std::string& code) {
    std::scoped_lock lock{mutex};
    if (board.guest_code != code) {
        board.guest_code = code;
        ++board.version;
    }
}

void SetState(State state, const std::string& detail) {
    std::scoped_lock lock{mutex};
    if (board.state != state || board.detail != detail) {
        board.state = state;
        board.detail = detail;
        ++board.version;
    }
}

void SetMembers(const std::vector<Member>& members) {
    std::scoped_lock lock{mutex};
    board.members = members;
    ++board.version;
}

void SetLastEvent(const std::string& text) {
    std::scoped_lock lock{mutex};
    board.last_event = text;
    board.event_ms = NowMs();
    board.event_seq = event_seq.fetch_add(1) + 1;
    ++board.version;
}

void SetAlert(const std::string& text) {
    std::scoped_lock lock{mutex};
    board.alert = text;
    board.alert_seq = alert_seq.fetch_add(1) + 1;
    ++board.version;
}

Board Snapshot() {
    Board copy;
    {
        std::scoped_lock lock{mutex};
        copy = board;
    }
    copy.enabled = Enabled();
    return copy;
}

std::uint64_t EventSeq() {
    return event_seq.load(std::memory_order_acquire);
}

namespace {
std::atomic<bool> g_friendly_fire{false};
}  // namespace
void SetFriendlyFire(bool on) {
    g_friendly_fire.store(on);
}
bool FriendlyFire() {
    return g_friendly_fire.load();
}
namespace {
std::atomic<bool> g_share_lamps{true};
}  // namespace
void SetShareLamps(bool on) {
    g_share_lamps.store(on);
}
bool ShareLamps() {
    return g_share_lamps.load();
}

std::uint64_t AlertSeq() {
    return alert_seq.load(std::memory_order_acquire);
}

bool Enabled() {
    const int forced = enabled_override.load();
    return forced >= 0 ? forced != 0 : EnvEnabled();
}

void RequestLeave() {
    std::scoped_lock lock{mutex};
    Queue({CommandType::Leave, {}});
}

void RequestRejoin() {
    std::scoped_lock lock{mutex};
    Queue({CommandType::Rejoin, {}});
}

bool KickMember(const std::string& name) {
    std::scoped_lock lock{mutex};
    if (board.role != Role::Host || name.empty()) {
        return false;
    }
    for (const Member& m : board.members) {
        if (m.name == name && !m.local && m.slot != 0) {
            Queue({CommandType::Kick, name});
            return true;
        }
    }
    return false;
}

namespace {
bool CopyText(const std::string& code);
}  // namespace

bool RequestCopyCode() {
    std::string code;
    {
        std::scoped_lock lock{mutex};
        code = board.code;
    }
    return CopyText(code);
}

bool RequestCopyLanCode() {
    std::string code;
    {
        std::scoped_lock lock{mutex};
        code = board.lan_code;
    }
    return CopyText(code);
}

bool RequestCopyGuestCode() {
    std::string code;
    {
        std::scoped_lock lock{mutex};
        code = board.guest_code;
    }
    return CopyText(code);
}

bool RequestAddGuestCodeFromClipboard(std::string* got) {
    std::string text;
#ifdef _WIN32
    if (OpenClipboard(nullptr)) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* w = static_cast<const wchar_t*>(GlobalLock(h))) {
                const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
                if (n > 1 && n < 4096) {
                    text.resize(std::size_t(n));
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, text.data(), n, nullptr, nullptr);
                    text.resize(std::size_t(n - 1));
                }
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
#endif
    if (got) *got = text;
    return RequestAddGuestCode(text);
}

bool RequestAddGuestCode(const std::string& text) {
    if (text.size()>4096) return false;
    std::string upper;
    for (char c : text) upper.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c));
    if (upper.find("BBG1") == std::string::npos) return false;
    std::scoped_lock lock{mutex};
    if (board.role!=Role::Host) return false;
    Queue({CommandType::AddGuestCode, text});
    return true;
}

namespace {
bool CopyText(const std::string& code) {
    if (code.empty()) {
        return false;
    }
#ifdef _WIN32
    // The code is ASCII (BBP1-... or host:port); UTF-16 for CF_UNICODETEXT all the same.
    const int wide = MultiByteToWideChar(CP_UTF8, 0, code.c_str(), -1, nullptr, 0);
    if (wide <= 0) {
        return false;
    }
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, std::size_t(wide) * sizeof(wchar_t));
    if (!mem) {
        return false;
    }
    if (auto* dst = static_cast<wchar_t*>(GlobalLock(mem))) {
        MultiByteToWideChar(CP_UTF8, 0, code.c_str(), -1, dst, wide);
        GlobalUnlock(mem);
    } else {
        GlobalFree(mem);
        return false;
    }
    // Another process may hold the clipboard for a moment: a few short tries, never long.
    bool opened = false;
    for (int attempt = 0; attempt < 5 && !opened; ++attempt) {
        opened = OpenClipboard(nullptr) != 0;
        if (!opened) {
            Sleep(2);
        }
    }
    if (!opened) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    CloseClipboard();
    if (!ok) {
        GlobalFree(mem);  // the clipboard owns it only on success
    }
    return ok;
#else
    return false;
#endif
}
}  // namespace

bool PopCommand(Command* out) {
    std::scoped_lock lock{mutex};
    if (commands.empty()) {
        return false;
    }
    if (out) {
        *out = std::move(commands.front());
    }
    commands.pop_front();
    return true;
}

void ResetForTest(bool enabled) {
    std::scoped_lock lock{mutex};
    board = Board{};
    commands.clear();
    event_seq = 0;
    alert_seq = 0;
    enabled_override = enabled ? 1 : 0;
}

}  // namespace party::status
