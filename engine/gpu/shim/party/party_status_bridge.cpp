// SPDX-License-Identifier: GPL-3.0-or-later
// Party status bridge (party_status_bridge.h): party runtime / director -> status board, and the
// Party tab's commands back.
#include "party_status_bridge.h"
#include "party_async_io.h"

#ifndef BB_PARTY_BRIDGE_NO_GAME
#include "party_runtime.h"
#include "party_director.h"
#endif

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace party::bridge {

namespace {

using Clock = std::chrono::steady_clock;
using status::Role;
using status::State;

// coop::SessionRole (game_state.h), without the game header.
constexpr int kRoleIdle = 0, kRoleTryingToHost = 1, kRoleHost = 3, kRoleTryingToJoin = 4, kRoleClient = 6,
              kRoleLeaving = 7;

constexpr double kBellWindowS = 35.0;      // a bell counts as "up" this long after it was rung
constexpr double kTravelTimeoutS = 90.0;   // Travelling at most this long
constexpr double kTravelNoLoadS = 20.0;    // ... or this long when no loading screen came
constexpr double kRosterEveryS = 3.0;
constexpr double kLogEveryS = 10.0;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party: ") + line + "\n"); // never blocks the caller (party_async_io.h)
}

double Since(Clock::time_point t, Clock::time_point now) { return std::chrono::duration<double>(now - t).count(); }

std::string AreaText(std::uint32_t map_id) {
    if (map_id == 0 || map_id == 0xffffffffu) return {};
    char text[24];
    std::snprintf(text, sizeof text, "m%02u_%02u_%02u_%02u", (map_id >> 24) & 0xff, (map_id >> 16) & 0xff,
                  (map_id >> 8) & 0xff, map_id & 0xff);
    return text;
}

struct Bridge {
    std::mutex mu;
    Facts f;
    std::string local_name;
    Mapped published;
    bool have_published = false;
    // Main thread (Tick).
    bool have_game = false;
    bool was_loading = false;
    bool travel_load_seen = false;
    Clock::time_point travel_at{}, bell_at{}, last_roster{}, last_log{};
    bool rung = false;
    bool log_on = false, log_checked = false;
    // A guest's Leave after the game's own session leave: the link stops once the game's session
    // is idle (at most 3 s on), so the game never sees the host vanish under a live session.
    bool leave_pending = false;
    Clock::time_point leave_at{};
    // BB_PARTY_STATUS_TEST: scripted Party tab presses (main thread).
    struct Scripted {
        double at = 0;
        status::CommandType type = status::CommandType::Leave;
        std::string name;
    };
    std::vector<Scripted> script;
    bool script_parsed = false;
    Clock::time_point first_tick{};
    Actions actions;
    bool actions_set = false;
};

Bridge& B() {
    static Bridge* b = new Bridge;  // used from exit paths and callback threads
    return *b;
}

bool LogOn(Bridge& b) {
    if (!b.log_checked) {
        b.log_checked = true;
        const char* v = std::getenv("BB_PARTY_STATUS_LOG");
        b.log_on = v && v[0] && v[0] != '0';
    }
    return b.log_on;
}

// mu held: the board's state from the facts, published when it changed.
void PublishLocked(Bridge& b) {
    const Mapped m = MapState(b.f);
    if (b.have_published && m == b.published) return;
    b.have_published = true;
    b.published = m;
    status::SetState(m.state, m.detail);
    if (LogOn(b)) Log("status: %s%s%s", status::StateName(m.state), m.detail.empty() ? "" : ": ", m.detail.c_str());
}

template <class F>
void Update(F&& change) {
    Bridge& b = B();
    std::lock_guard<std::mutex> lk(b.mu);
    change(b.f);
    PublishLocked(b);
}

#ifndef BB_PARTY_BRIDGE_NO_GAME
Actions DefaultActions() {
    Actions a;
    a.leave = [] { party::runtime_leave(); };
    a.leave_session = [] { return coop::LeaveGameSession(); };
    a.rejoin = [] { return party::runtime_rejoin(); };
    a.kick = [](const std::string& name) {
        PartyLink* link = party::runtime_link();
        return link && link->is_host() && link->kick_name(name, "kicked by the host");
    };
    a.add_guest_code = [](const std::string& text) { return party::runtime_add_guest_code(text); };
    return a;
}
#else
Actions DefaultActions() { return {}; }
#endif

void HandleCommand(Bridge& b, const status::Command& c, const TickIn& in, TickOut* out) {
    Actions& a = b.actions;
    switch (c.type) {
    case status::CommandType::Leave:
        Log("status: Leave pressed (%s)", in.host ? "host: ending the party" : "guest: leaving the party");
        // The game's own leave first - a guest summoned (6: client) goes home at once; a host with
        // guests in (3: host) ends the session the way its Silencing Blank would - then the link
        // stops once the game's session is idle.
        if (((!in.host && in.session_role == 6) || (in.host && in.session_role == 3 && in.cooperators > 0)) &&
            a.leave_session) {
            const bool left = a.leave_session();
            Log("status: Leave: the game's own session leave first: %s",
                left ? "done; the link stops once the session is idle" : "not available (code check)");
            if (left) {
                b.leave_pending = true;
                b.leave_at = Clock::now();
                break;
            }
        }
        if (a.leave) a.leave();
        break;
    case status::CommandType::Rejoin: {
        const int r = a.rejoin ? a.rejoin() : -1;
        Log("status: Rejoin pressed: %s", r == 0   ? "link up, ringing now"
                                          : r == 1 ? "reconnecting now"
                                          : r == 2 ? "starting a fresh link"
                                                   : "nothing to do");
        if (r == 0) {
            out->ring_now = true;
            status::SetLastEvent(in.host ? "Ringing the Beckoning Bell now" : "Ringing the Small Resonant Bell now");
        } else if (r == 1) {
            status::SetLastEvent("Reconnecting now");
        }
        break;
    }
    case status::CommandType::AddGuestCode: {
        if (!in.host) break;
        const int n = a.add_guest_code ? a.add_guest_code(c.name) : 0;
        Log("status: Add guest code: %d valid code(s)", n);
        status::SetLastEvent(n > 0 ? "Guest code added: opening the way for that guest"
                                   : "No valid guest code (BBG1-XXXX-XXXX-XXXX) on the clipboard");
        break;
    }
    case status::CommandType::Kick: {
        if (!in.host) break;
        const bool ok = a.kick && a.kick(c.name);
        Log("status: Kick %s: %s", c.name.c_str(), ok ? "kicked" : "no such member");
        if (ok) status::SetLastEvent("Kicked " + c.name);
        break;
    }
    }
}

}  // namespace

// ---- pure part ----

std::string RejectText(RejectCode code, const std::string& reason) {
    switch (code) {
    case RejectCode::Kicked: return "kicked by the host";
    case RejectCode::Shutdown: return reason.empty() ? "the host ended the party" : reason;
    case RejectCode::Auth: return reason.empty() ? "wrong password or party code" : reason;
    case RejectCode::Full: return reason.empty() ? "the party is full" : reason;
    case RejectCode::Version: return reason.empty() ? "different party protocol version" : reason;
    case RejectCode::Mismatch: return "host refused: " + (reason.empty() ? std::string("different game") : reason);
    case RejectCode::Name: return reason.empty() ? "name refused" : reason;
    default: return reason.empty() ? "refused by the host" : reason;
    }
}

std::string HostWaitText(int seconds) {
    return "Waiting for the host to come back (" + std::to_string(seconds < 0 ? 0 : seconds) + " s)...";
}

std::string HostGoneNotice(int cap_s) {
    const std::string limit = cap_s % 60 == 0 ? std::to_string(cap_s / 60) + " min" : std::to_string(cap_s) + " s";
    return "Party: the host did not come back within " + limit +
           "\nYou play on alone in your own world. When the host is back, System > Party > Rejoin the host "
           "joins again.";
}

std::string AuthRejectText(bool coded, bool code_needs_password, bool sent_password) {
    if (!coded) return "Wrong or missing party password";
    if (code_needs_password)
        return sent_password ? "Wrong party password (or an old party code: ask the host for the current one)"
                             : "This party needs a password: enter the host's password";
    return "Old party code: ask the host for the current one";
}

bool RefusalPlaysSolo(RejectCode code) {
    return code != RejectCode::None && code != RejectCode::Shutdown;
}

std::string RefusalNotice(RejectCode code, const std::string& reason) {
    if (!RefusalPlaysSolo(code)) return {};
    std::string title, todo;
    switch (code) {
    case RejectCode::Auth:
        title = reason.empty() ? "Wrong or missing party password" : reason;
        todo = "Check the password and code with the host, set them in System > Party, then choose Rejoin the host.";
        break;
    case RejectCode::Full:
        title = "The party is full";
        todo = "Ask the host to make room, then choose System > Party > Rejoin the host.";
        break;
    case RejectCode::Version:
    case RejectCode::Mismatch:
        title = "The host's game differs from yours";
        if (!reason.empty()) title += ": " + reason;
        todo = "Use the same bbport version, patches and mods as the host (launcher), then restart.";
        break;
    case RejectCode::Kicked:
        title = "The host removed you from the party";
        todo = "System > Party > Rejoin the host asks to join again.";
        break;
    case RejectCode::Name:
        title = reason.empty() ? "Your party name was refused" : "Party name refused: " + reason;
        todo = "Choose another Party name in System > Party, then restart to join.";
        break;
    default:
        title = "The host refused to let you join";
        if (!reason.empty()) title += ": " + reason;
        todo = "System > Party > Rejoin the host tries again.";
        break;
    }
    return "Party: " + title + "\n" + todo + "\nYou play online alone in your own world until then.";
}

std::string RejoinPassword(const std::string& joined_with, const std::string& flow_at_start,
                           const std::string& flow_now, bool code_ignores_password) {
    if (code_ignores_password) return {};
    return flow_now != flow_at_start ? flow_now : joined_with;
}

Mapped MapState(const Facts& f) {
    const bool host = f.role == Role::Host;
    if (f.role == Role::Off) return {State::Off, {}};
    if (f.left) return {State::Off, host ? "you ended the party" : "you left the party"};
    if (!f.have_link) {
        if (!f.error.empty()) return {State::Error, f.error};
        return {State::Starting, {}};
    }
    // Host crash recovery: a guest waiting for its lost host (link down, or up while the host's
    // restarted game loads its world).
    if (!host && f.host_wait_s >= 0 && f.link != LinkState::Rejected && f.link != LinkState::Stopped)
        return {State::Reconnecting, HostWaitText(f.host_wait_s)};
    const std::string count = std::to_string(f.members_connected) + "/" + std::to_string(f.max_players);
    switch (f.link) {
    case LinkState::Idle: return f.error.empty() ? Mapped{State::Starting, {}} : Mapped{State::Error, f.error};
    case LinkState::Rejected:  // the host ending the party is no error
        return {f.reject == RejectCode::Shutdown ? State::Off : State::Error, RejectText(f.reject, f.reject_reason)};
    case LinkState::Stopped: return {State::Off, "party stopped"};
    case LinkState::Connecting: return {State::Connecting, f.host_address};
    case LinkState::Reconnecting: return {State::Reconnecting, f.link_detail};
    case LinkState::Hosting:
    case LinkState::Connected: break;
    }
    // The link is up: the game decides.
    if (f.travelling) return {State::Travelling, {}};
    if (!f.world_up || f.loading) {
        const std::string why = f.loading ? "loading" : "title / menu";
        return {host ? State::Hosting : State::WaitingForWorld, host ? count + ", " + why : "connected; " + why};
    }
    if (f.session_role == kRoleLeaving) return {State::WaitingForWorld, "leaving the session"};
    // C1: no summons before this player finished its prologue (party_start.h).
    if (f.prologue && f.session_role == kRoleIdle) {
        const std::string why = "prologue" + (f.prologue_step.empty() ? std::string() : " (" + f.prologue_step + ")") +
                                ", solo until ready";
        return {host ? State::Hosting : State::WaitingForWorld, host ? count + ", " + why : why};
    }
    // #19: nobody is summoned while another member is still in its prologue (whoever finished
    // first waits; the Party page says for whom).
    if (!f.others_prologue.empty() && f.session_role == kRoleIdle) {
        const std::string why = "Waiting: " + f.others_prologue + (f.others_prologue.find(" and ") != std::string::npos
                                                                         ? " are in the prologue"
                                                                         : " is in the prologue");
        return {host ? State::Hosting : State::WaitingForWorld, host ? count + ", " + why : why};
    }
    if (host) {
        const std::string busy = f.host_busy.empty() ? std::string() : ", " + f.host_busy;  // A11
        if (f.session_role == kRoleHost && f.cooperators > 0)
            return {State::Joined, std::to_string(f.cooperators + 1) + " in your world, " + count + " in the party" + busy};
        if (!busy.empty()) return {State::Hosting, count + busy};
        if (f.session_role == kRoleTryingToHost || (f.bell_recent && f.session_role == kRoleIdle))
            return {State::RingingBell, count};
        return {State::Hosting, count};
    }
    if (f.session_role == kRoleClient) return {State::Joined, "in the host's world"};
    if (!f.host_busy.empty()) return {State::WaitingForWorld, f.host_busy};  // A11: the summon follows
    if (f.session_role == kRoleTryingToJoin || (f.bell_recent && f.session_role == kRoleIdle))
        return {State::RingingBell, {}};
    return {State::WaitingForWorld, "connected; waiting for the host's bell"};
}

std::vector<status::Member> MapMembers(const std::vector<RosterEntry>& roster, const std::string& local_name,
                                       bool host, const std::function<std::string(std::uint32_t)>& area) {
    std::vector<status::Member> out;
    out.reserve(roster.size());
    for (const RosterEntry& e : roster) {
        status::Member m;
        m.name = e.name;
        m.connected = e.connected;
        m.slot = e.slot;
        m.local = e.name == local_name;
        // In the host's world: the host itself once its world is up, a guest once summoned.
        m.in_world = e.slot == kHostSlot ? (e.state == MemberState::Home || e.state == MemberState::InHostWorld)
                                         : e.state == MemberState::InHostWorld;
        // Pings are the host's measurements: none for the host itself, none while lost.
        m.ping_ms = (e.slot == kHostSlot || !e.connected) ? -1 : static_cast<int>(e.ping_ms);
        if (!host && m.local) m.ping_ms = -1;
        m.area = area ? area(e.map_id) : AreaText(e.map_id);
        if (e.state == MemberState::Prologue) m.area += m.area.empty() ? "prologue" : " (prologue)";
        out.push_back(std::move(m));
    }
    return out;
}

std::string SnapshotLine(const status::Board& b) {
    std::string s = std::string("status ") + status::RoleName(b.role) + " " + status::StateName(b.state);
    if (!b.detail.empty()) s += " (" + b.detail + ")";
    if (!b.code.empty()) s += " code " + b.code;
    if (!b.lan_code.empty()) s += " lan " + b.lan_code;
    s += "; members " + std::to_string(b.members.size());
    if (!b.members.empty()) {
        s += ":";
        for (const status::Member& m : b.members) {
            s += " " + m.name + "[" + std::to_string(m.slot) + (m.local ? ",me" : "") + (m.connected ? "" : ",lost") +
                 (m.in_world ? ",in world" : "");
            if (m.ping_ms >= 0) s += "," + std::to_string(m.ping_ms) + "ms";
            if (!m.area.empty()) s += "," + m.area;
            s += "]";
        }
    }
    if (!b.last_event.empty()) s += "; last event \"" + b.last_event + "\"";
    return s;
}

// ---- producers ----

void OnRuntimeStart(bool host, const std::string& name) {
    status::SetRole(host ? Role::Host : Role::Guest);
    {
        std::lock_guard<std::mutex> lk(B().mu);
        B().local_name = name;
    }
    Update([&](Facts& f) {
        f.role = host ? Role::Host : Role::Guest;
        f.error.clear();
    });
}

void OnCode(const std::string& code) { status::SetCode(code); }
void OnGuestCode(const std::string& code) { status::SetGuestCode(code); }
void OnNetwork(const std::string& text) { status::SetNetwork(text); }
void OnLanCode(const std::string& code) { status::SetLanCode(code); }

void OnRuntimeError(const std::string& error) {
    bool have_link;
    {
        Bridge& b = B();
        std::lock_guard<std::mutex> lk(b.mu);
        b.f.error = error;
        have_link = b.f.have_link;
        PublishLocked(b);
    }
    // With a link, its rejection already made the event (OnLinkState).
    if (!have_link) status::SetLastEvent("Party error: " + error);
}

void OnHostAddress(const std::string& address) {
    Update([&](Facts& f) { f.host_address = address; });
}

void OnLinkState(LinkState state, const std::string& detail, RejectCode reject, const std::string& reason) {
    LinkState was;
    bool host;
    {
        Bridge& b = B();
        std::lock_guard<std::mutex> lk(b.mu);
        was = b.f.link;
        host = b.f.role == Role::Host;
        b.f.have_link = true;
        b.f.link = state;
        b.f.link_detail = detail;
        if (state == LinkState::Rejected) {
            b.f.reject = reject;
            b.f.reject_reason = reason;
        } else if (state == LinkState::Connected || state == LinkState::Hosting) {
            b.f.reject = RejectCode::None;
            b.f.reject_reason.clear();
            b.f.error.clear();
        }
        PublishLocked(b);
    }
    // A refusal is told every time (parx2 fin, live: a guest that had left, rejoined and then saw the
    // host end the party kept "You left the party": its rejoined link reported Rejected again,
    // which equalled the stale state of the first link).
    if (state == was && state != LinkState::Rejected) return;
    // Out of the party: the old roster is no longer ours.
    if (state == LinkState::Rejected || state == LinkState::Stopped) status::SetMembers({});
    if (state == LinkState::Connected) {
        status::SetAlert({});  // a refusal's box: in now
        status::SetLastEvent(was == LinkState::Reconnecting ? "Rejoined the party" : "Joined the party");
    } else if (state == LinkState::Reconnecting && (was == LinkState::Connected)) {
        status::SetLastEvent("Connection to the host lost");
    } else if (state == LinkState::Rejected) {
        status::SetLastEvent(RejectText(reject, reason));
        if (!host) status::SetAlert(RefusalNotice(reject, reason));
    } else if (state == LinkState::Hosting && host) {
        status::SetLastEvent("Party open");
    }
}

void OnRoster(const std::vector<RosterEntry>& roster, int max_players) {
    Bridge& b = B();
    std::string name;
    bool host;
    int connected = 0;
    for (const RosterEntry& e : roster) connected += e.connected ? 1 : 0;
    {
        std::lock_guard<std::mutex> lk(b.mu);
        name = b.local_name;
        host = b.f.role == Role::Host;
        b.f.members_connected = connected;
        if (max_players > 0) b.f.max_players = max_players;
        PublishLocked(b);
    }
    status::SetMembers(MapMembers(roster, name, host));
}

void OnOthersPrologue(const std::string& names) {
    bool changed = false;
    {
        Bridge& b = B();
        std::lock_guard<std::mutex> lk(b.mu);
        changed = b.f.others_prologue != names;
        b.f.others_prologue = names;
        // A title join refused for the host's prologue left "Waiting: the host is in its prologue":
        // gone once nobody is in the prologue any more (live: shown after both were ready).
        if (names.empty() && b.f.host_busy.find("prologue") != std::string::npos) {
            b.f.host_busy.clear();
            changed = true;
        }
        if (changed) PublishLocked(b);
    }
    if (changed && !names.empty()) status::SetLastEvent(names + " is in the prologue");
}

void OnHostWait(int seconds) {
    {
        Bridge& b = B();
        std::lock_guard<std::mutex> lk(b.mu);
        if (b.f.host_wait_s == seconds) return;
        b.f.host_wait_s = seconds;
        PublishLocked(b);
    }
    // The notice in the corner, refreshed every second while it waits (the toast shows the newest).
    if (seconds >= 0) status::SetLastEvent(HostWaitText(seconds));
}

void OnMemberJoined(const std::string& name, bool rejoined) {
    status::SetLastEvent(name + (rejoined ? " rejoined" : " joined"));
}

void OnMemberLeft(const std::string& name, bool slot_kept) {
    status::SetLastEvent(name + (slot_kept ? " lost (slot kept)" : " left"));
}

void OnTravel(const std::string& what) {
    {
        Bridge& b = B();
        std::lock_guard<std::mutex> lk(b.mu);
        b.travel_at = Clock::now();
        b.travel_load_seen = false;
        b.f.travelling = true;
        PublishLocked(b);
    }
    status::SetLastEvent(what);  // plain text (coop::TravelKindLabel)
}

void OnRing() {
    Bridge& b = B();
    std::lock_guard<std::mutex> lk(b.mu);
    b.bell_at = Clock::now();
    b.rung = true;
    b.f.bell_recent = true;
    PublishLocked(b);
}

void OnHostBusy(const std::string& text) {
    bool changed = false;
    {
        Bridge& b = B();
        std::lock_guard<std::mutex> lk(b.mu);
        changed = b.f.host_busy != text;
        b.f.host_busy = text;
        if (changed) PublishLocked(b);
    }
    if (changed && !text.empty()) status::SetLastEvent(text);
}

void OnLeft(bool host) {
    Update([](Facts& f) { f.left = true; });
    status::SetLastEvent(host ? "You ended the party" : "You left the party");
}

void OnRejoining() {
    Update([](Facts& f) {
        f.left = false;
        f.reject = RejectCode::None;
        f.reject_reason.clear();
        f.error.clear();
    });
}

namespace {

// BB_PARTY_STATUS_TEST=leave@60,rejoin@80,kick:Hunter1@40 - presses the Party tab's buttons this
// many s after the first tick (tests without keystrokes).
void RunScript(Bridge& b, bool host, Clock::time_point now) {
    if (!b.script_parsed) {
        b.script_parsed = true;
        b.first_tick = now;
        const char* v = std::getenv("BB_PARTY_STATUS_TEST");
        std::string list = v ? v : "";
        std::size_t at = 0;
        while (at < list.size()) {
            std::size_t end = list.find(',', at);
            if (end == std::string::npos) end = list.size();
            std::string item = list.substr(at, end - at);
            at = end + 1;
            // "host:" / "guest:" - only for that role.
            for (const char* role : {"host:", "guest:"}) {
                if (item.rfind(role, 0) == 0) {
                    if ((role[0] == 'h') != host) item.clear();
                    else item.erase(0, std::strlen(role));
                }
            }
            const std::size_t t = item.find('@');
            if (t == std::string::npos) continue;
            Bridge::Scripted s;
            s.at = std::atof(item.c_str() + t + 1);
            std::string cmd = item.substr(0, t);
            if (const std::size_t c = cmd.find(':'); c != std::string::npos) {
                s.name = cmd.substr(c + 1);
                cmd.resize(c);
            }
            if (cmd == "leave") s.type = status::CommandType::Leave;
            else if (cmd == "rejoin") s.type = status::CommandType::Rejoin;
            else if (cmd == "kick") s.type = status::CommandType::Kick;
            else continue;
            b.script.push_back(s);
            Log("status test: %s%s%s at %.0f s", status::CommandName(s.type), s.name.empty() ? "" : " ",
                s.name.c_str(), s.at);
        }
    }
    if (b.script.empty()) return;
    const double t = Since(b.first_tick, now);
    for (auto it = b.script.begin(); it != b.script.end();) {
        if (t < it->at) {
            ++it;
            continue;
        }
        Log("status test: pressing %s%s%s", status::CommandName(it->type), it->name.empty() ? "" : " ",
            it->name.c_str());
        if (it->type == status::CommandType::Leave) status::RequestLeave();
        else if (it->type == status::CommandType::Rejoin) status::RequestRejoin();
        else if (!status::KickMember(it->name)) Log("status test: kick %s refused by the board", it->name.c_str());
        it = b.script.erase(it);
    }
}

}  // namespace

TickOut Tick(const TickIn& in) {
    TickOut out;
    Bridge& b = B();
    const auto now = Clock::now();
    bool roster_due = false, log_due = false;
    {
        std::lock_guard<std::mutex> lk(b.mu);
        if (!b.actions_set) {
            b.actions = DefaultActions();
            b.actions_set = true;
        }
        // Only a few scalars per frame; the state is recomputed and published on a change.
        Facts& f = b.f;
        bool travelling = f.travelling, bell = f.bell_recent;
        if (travelling) {
            if (in.loading) b.travel_load_seen = true;
            const double t = Since(b.travel_at, now);
            if ((b.travel_load_seen && !in.loading && in.world_up) || t > kTravelTimeoutS ||
                (!b.travel_load_seen && t > kTravelNoLoadS))
                travelling = false;
        }
        if (bell && (!b.rung || Since(b.bell_at, now) > kBellWindowS)) bell = false;
        const int max_players = f.max_players ? f.max_players : in.max_players;
        const char* step = in.prologue && in.prologue_step ? in.prologue_step : "";
        if (!b.have_game || in.world_up != f.world_up || in.loading != f.loading ||
            in.session_role != f.session_role || in.cooperators != f.cooperators || travelling != f.travelling ||
            bell != f.bell_recent || max_players != f.max_players || in.prologue != f.prologue ||
            f.prologue_step != step) {
            b.have_game = true;
            f.world_up = in.world_up;
            f.loading = in.loading;
            f.session_role = in.session_role;
            f.cooperators = in.cooperators;
            f.travelling = travelling;
            f.bell_recent = bell;
            f.max_players = max_players;
            f.prologue = in.prologue;
            f.prologue_step = step;
            PublishLocked(b);
        }
        // The roster only while in a party (a rejected / stopped link keeps its last one).
        if (in.link && (f.link == LinkState::Connected || f.link == LinkState::Hosting) &&
            Since(b.last_roster, now) >= kRosterEveryS) {
            b.last_roster = now;
            roster_due = true;
        }
        if (LogOn(b) && Since(b.last_log, now) >= kLogEveryS) {
            b.last_log = now;
            log_due = true;
        }
    }
    if (roster_due) OnRoster(in.link->roster(), in.link->max_players());
    RunScript(b, in.host, now);
    // The tab's commands (main thread: the runtime calls below only start background work).
    status::Command c;
    while (status::PopCommand(&c)) HandleCommand(b, c, in, &out);
    if (b.leave_pending && (in.session_role == 0 || now - b.leave_at > std::chrono::seconds(3))) {
        b.leave_pending = false;
        Log("status: Leave: session %s; stopping the link", in.session_role == 0 ? "idle" : "still not idle after 3 s");
        if (b.actions.leave) b.actions.leave();
    }
    if (log_due) Log("%s", SnapshotLine(status::Snapshot()).c_str());
    return out;
}

void SetActionsForTest(Actions actions) {
    Bridge& b = B();
    std::lock_guard<std::mutex> lk(b.mu);
    b.actions = std::move(actions);
    b.actions_set = true;
}

void ResetForTest() {
    Bridge& b = B();
    std::lock_guard<std::mutex> lk(b.mu);
    b.f = Facts{};
    b.local_name.clear();
    b.published = Mapped{};
    b.have_published = false;
    b.have_game = false;
    b.travel_load_seen = false;
    b.rung = false;
    b.last_roster = b.last_log = Clock::time_point{};
    b.log_checked = false;
    b.actions = Actions{};
    b.actions_set = true;  // tests install their own (SetActionsForTest)
}

}  // namespace party::bridge
