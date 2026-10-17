// SPDX-License-Identifier: GPL-3.0-or-later
// Party status bridge (gpu/shim/party/party_status_bridge.h): link / game facts -> board state,
// roster -> members, events, and the Party tab's commands -> actions.
// ninja -C out/gpu party-status-bridge-test && out/gpu/party-status-bridge-test
#include "party/party_status_bridge.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace party;
using bridge::Facts;
using bridge::MapState;
using status::Role;
using status::State;

static int g_checks = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(x)) {                                                                 \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #x);              \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static Facts up(Role role) {
    Facts f;
    f.role = role;
    f.have_link = true;
    f.link = role == Role::Host ? LinkState::Hosting : LinkState::Connected;
    f.members_connected = 2;
    f.max_players = 3;
    f.world_up = true;
    f.session_role = 0;
    f.cooperators = 0;
    return f;
}

static int test_map_state() {
    Facts f;
    CHECK(MapState(f).state == State::Off);
    f.role = Role::Guest;
    CHECK(MapState(f).state == State::Starting);
    f.error = "no party code: set BB_PARTY_CODE";
    CHECK(MapState(f).state == State::Error && MapState(f).detail == f.error);
    f.error.clear();
    f.have_link = true;
    f.link = LinkState::Connecting;
    f.host_address = "127.0.0.1:47600";
    CHECK(MapState(f).state == State::Connecting && MapState(f).detail == "127.0.0.1:47600");
    f.link = LinkState::Reconnecting;
    f.link_detail = "timeout; retrying in 2000 ms";
    CHECK(MapState(f).state == State::Reconnecting && MapState(f).detail == f.link_detail);
    // Host crash recovery: while the guest waits for a lost host, the line counts the seconds.
    f.host_wait_s = 12;
    CHECK(MapState(f).state == State::Reconnecting &&
          MapState(f).detail == "Waiting for the host to come back (12 s)...");
    f.link = LinkState::Connected;  // the restarted host's game answers; its world is not up yet
    CHECK(MapState(f).state == State::Reconnecting &&
          MapState(f).detail == "Waiting for the host to come back (12 s)...");
    f.link = LinkState::Reconnecting;
    f.host_wait_s = -1;
    CHECK(bridge::HostWaitText(-3) == "Waiting for the host to come back (0 s)...");
    CHECK(bridge::HostGoneNotice(180).rfind("Party: the host did not come back within 3 min\n", 0) == 0);
    CHECK(bridge::HostGoneNotice(90).find("within 90 s") != std::string::npos);
    CHECK(bridge::HostGoneNotice(180).find("Rejoin the host") != std::string::npos);
    f.link = LinkState::Rejected;
    f.reject = RejectCode::Kicked;
    f.reject_reason = "whatever";
    CHECK(MapState(f).state == State::Error && MapState(f).detail == "kicked by the host");
    f.reject = RejectCode::Mismatch;
    f.reject_reason = "eboot.bin differs";
    CHECK(MapState(f).detail == "host refused: eboot.bin differs");
    // Auth: the guest's own words (what it joined with) replace the host's generic reason.
    CHECK(party::bridge::AuthRejectText(true, false, false).rfind("Old party code", 0) == 0);
    CHECK(party::bridge::AuthRejectText(true, false, true).rfind("Old party code", 0) == 0);  // the saved password was ignored
    CHECK(party::bridge::AuthRejectText(true, true, false).find("needs a password") != std::string::npos);
    CHECK(party::bridge::AuthRejectText(true, true, true).rfind("Wrong party password", 0) == 0);
    CHECK(party::bridge::AuthRejectText(false, false, false) == "Wrong or missing party password");
    f.reject = RejectCode::Auth;
    f.reject_reason = party::bridge::AuthRejectText(true, true, false);
    CHECK(MapState(f).state == State::Error && MapState(f).detail == f.reject_reason);
    f.reject = RejectCode::Shutdown;
    f.reject_reason.clear();
    CHECK(MapState(f).state == State::Off && MapState(f).detail == "the host ended the party");
    f.link = LinkState::Stopped;
    CHECK(MapState(f).state == State::Off);
    f.left = true;
    CHECK(MapState(f).state == State::Off && MapState(f).detail == "you left the party");

    // Guest, link up: the game decides.
    f = up(Role::Guest);
    f.world_up = false;
    CHECK(MapState(f).state == State::WaitingForWorld && MapState(f).detail == "connected; title / menu");
    f.world_up = true;
    f.loading = true;
    CHECK(MapState(f).state == State::WaitingForWorld && MapState(f).detail == "connected; loading");
    f.loading = false;
    CHECK(MapState(f).state == State::WaitingForWorld);
    f.bell_recent = true;
    CHECK(MapState(f).state == State::RingingBell);
    f.bell_recent = false;
    f.session_role = 4;  // trying to join
    CHECK(MapState(f).state == State::RingingBell);
    f.session_role = 6;  // client
    CHECK(MapState(f).state == State::Joined && MapState(f).detail == "in the host's world");
    f.travelling = true;
    CHECK(MapState(f).state == State::Travelling);

    // Host.
    f = up(Role::Host);
    CHECK(MapState(f).state == State::Hosting && MapState(f).detail == "2/3");
    f.world_up = false;
    CHECK(MapState(f).state == State::Hosting && MapState(f).detail == "2/3, title / menu");
    f.world_up = true;
    f.session_role = 1;
    CHECK(MapState(f).state == State::RingingBell && MapState(f).detail == "2/3");
    f.session_role = 3;
    f.cooperators = 1;
    CHECK(MapState(f).state == State::Joined && MapState(f).detail == "2 in your world, 2/3 in the party");
    f.left = true;
    CHECK(MapState(f).state == State::Off && MapState(f).detail == "you ended the party");
    f = up(Role::Host);
    f.link = LinkState::Idle;
    CHECK(MapState(f).state == State::Starting);
    f.error = "cannot host: port taken";
    CHECK(MapState(f).state == State::Error);
    return 0;
}

static int test_members() {
    std::vector<RosterEntry> roster(3);
    roster[0] = {0, "Hunter0", MemberState::Home, true, 0x18010000u, 0};
    roster[1] = {1, "Hunter1", MemberState::InHostWorld, true, 0x18010000u, 34};
    roster[2] = {2, "Hunter2", MemberState::Loading, false, 0, 99};
    auto m = bridge::MapMembers(roster, "Hunter0", true);
    CHECK(m.size() == 3);
    CHECK(m[0].local && m[0].in_world && m[0].ping_ms == -1 && m[0].slot == 0 && m[0].area == "m24_01_00_00");
    CHECK(!m[1].local && m[1].in_world && m[1].connected && m[1].ping_ms == 34);
    CHECK(!m[2].connected && !m[2].in_world && m[2].ping_ms == -1 && m[2].area.empty());
    m = bridge::MapMembers(roster, "Hunter1", false, [](std::uint32_t id) { return id ? "Cathedral Ward" : ""; });
    CHECK(m[1].local && m[1].ping_ms == -1 && m[1].area == "Cathedral Ward" && !m[0].local);
    return 0;
}

// The producers through the board, and the commands through the actions.
static int test_board_and_commands() {
    status::ResetForTest(true);
    bridge::ResetForTest();
    int leaves = 0, rejoins = 0;
    std::vector<std::string> kicked;
    int rejoin_result = 0;
    bridge::SetActionsForTest({[&] { ++leaves; }, [&] {
                                   ++rejoins;
                                   return rejoin_result;
                               },
                               [&](const std::string& n) {
                                   kicked.push_back(n);
                                   return n == "Hunter1";
                               }});
    bridge::OnRuntimeStart(true, "Hunter0");
    status::Board b = status::Snapshot();
    CHECK(b.role == Role::Host && b.state == State::Starting);
    bridge::OnCode("BBP1-TEST");
    bridge::OnLinkState(LinkState::Hosting, "", RejectCode::None, "");
    std::vector<RosterEntry> roster(2);
    roster[0] = {0, "Hunter0", MemberState::Home, true, 0x18010000u, 0};
    roster[1] = {1, "Hunter1", MemberState::Home, true, 0x18010000u, 12};
    bridge::OnRoster(roster, 3);
    bridge::OnMemberJoined("Hunter1", false);
    bridge::TickIn in;
    in.host = true;
    in.world_up = true;
    in.session_role = 0;
    in.cooperators = 0;
    bridge::TickOut out = bridge::Tick(in);
    CHECK(!out.ring_now);
    b = status::Snapshot();
    CHECK(b.code == "BBP1-TEST" && b.state == State::Hosting && b.detail == "2/3");
    CHECK(b.members.size() == 2 && b.members[0].local && b.members[1].name == "Hunter1");
    CHECK(b.last_event == "Hunter1 joined");
    std::printf("  %s\n", bridge::SnapshotLine(b).c_str());

    // Unchanged facts: no new version.
    const auto v = status::Snapshot().version;
    bridge::Tick(in);
    CHECK(status::Snapshot().version == v);

    // Bell, then the guest is in.
    bridge::OnRing();
    bridge::Tick(in);
    CHECK(status::Snapshot().state == State::RingingBell);
    in.session_role = 3;
    in.cooperators = 1;
    bridge::Tick(in);
    CHECK(status::Snapshot().state == State::Joined);

    // Travel: until a loading screen came and went.
    bridge::OnTravel("Respawning with the party");  // plain text (coop::TravelKindLabel), shown as is
    CHECK(status::Snapshot().state == State::Travelling &&
          status::Snapshot().last_event == "Respawning with the party");
    in.loading = true;
    bridge::Tick(in);
    CHECK(status::Snapshot().state == State::Travelling);
    in.loading = false;
    bridge::Tick(in);
    CHECK(status::Snapshot().state == State::Joined);

    // Commands: Kick (host), Rejoin while up -> ring now, Leave.
    CHECK(status::KickMember("Hunter1"));
    out = bridge::Tick(in);
    CHECK(kicked.size() == 1 && kicked[0] == "Hunter1" && status::Snapshot().last_event == "Kicked Hunter1");
    status::RequestRejoin();
    out = bridge::Tick(in);
    CHECK(rejoins == 1 && out.ring_now);
    rejoin_result = 2;
    status::RequestRejoin();
    out = bridge::Tick(in);
    CHECK(rejoins == 2 && !out.ring_now);
    status::RequestLeave();
    bridge::Tick(in);
    CHECK(leaves == 1);
    bridge::OnLeft(true);
    bridge::OnLinkState(LinkState::Stopped, "", RejectCode::None, "");
    b = status::Snapshot();
    CHECK(b.state == State::Off && b.detail == "you ended the party" && b.last_event == "You ended the party");
    bridge::OnRejoining();
    bridge::OnLinkState(LinkState::Hosting, "", RejectCode::None, "");
    CHECK(status::Snapshot().state == State::Joined);  // the game facts still say 1 cooperator

    // Guest side: kicked, then reconnecting / connected events; Kick is host-only.
    status::ResetForTest(true);
    bridge::ResetForTest();
    kicked.clear();
    bridge::SetActionsForTest({[&] { ++leaves; }, [&] { return -1; }, [&](const std::string& n) {
                                   kicked.push_back(n);
                                   return true;
                               }});
    bridge::OnRuntimeStart(false, "Hunter1");
    bridge::OnHostAddress("127.0.0.1:47600");
    bridge::OnLinkState(LinkState::Connecting, "", RejectCode::None, "");
    CHECK(status::Snapshot().state == State::Connecting && status::Snapshot().detail == "127.0.0.1:47600");
    bridge::OnLinkState(LinkState::Connected, "", RejectCode::None, "");
    CHECK(status::Snapshot().last_event == "Joined the party");
    bridge::OnLinkState(LinkState::Reconnecting, "lost", RejectCode::None, "");
    CHECK(status::Snapshot().state == State::Reconnecting && status::Snapshot().last_event == "Connection to the host lost");
    bridge::OnHostWait(5);  // the host crashed: the guest waits for its restart
    CHECK(status::Snapshot().detail == "Waiting for the host to come back (5 s)..." &&
          status::Snapshot().last_event == "Waiting for the host to come back (5 s)...");
    const std::uint64_t seq = status::EventSeq();
    bridge::OnHostWait(5);  // the same second: no new notice
    CHECK(status::EventSeq() == seq);
    bridge::OnHostWait(-1);
    CHECK(status::Snapshot().detail == "lost");
    bridge::OnLinkState(LinkState::Connected, "", RejectCode::None, "");
    CHECK(status::Snapshot().last_event == "Rejoined the party");
    bridge::OnLinkState(LinkState::Rejected, "kicked by the host", RejectCode::Kicked, "kicked by the host");
    CHECK(status::Snapshot().state == State::Error && status::Snapshot().detail == "kicked by the host");
    CHECK(status::Snapshot().last_event == "kicked by the host");
    bridge::TickIn gin;
    gin.host = false;
    bridge::Tick(gin);
    CHECK(status::Snapshot().state == State::Error);
    return 0;
}

// C1: a player still in its prologue (party_start.h) shows that instead of "waiting for the bell".
static int test_prologue() {
    using party::LinkState;
    using party::status::Role;
    using party::status::State;
    Facts f;
    f.role = Role::Guest;
    f.have_link = true;
    f.link = LinkState::Connected;
    f.world_up = true;
    f.session_role = 0;
    f.prologue = true;
    f.prologue_step = "clinic";
    CHECK(MapState(f).state == State::WaitingForWorld && MapState(f).detail == "prologue (clinic), solo until ready");
    f.prologue = false;
    CHECK(MapState(f).detail == "connected; waiting for the host's bell");
    f.role = Role::Host;
    f.link = LinkState::Hosting;
    f.members_connected = 2;
    f.max_players = 3;
    f.prologue = true;
    f.prologue_step = "first-dream";
    CHECK(MapState(f).state == State::Hosting && MapState(f).detail == "2/3, prologue (first-dream), solo until ready");
    f.session_role = 6;  // already summoned: the session wins
    f.role = Role::Guest;
    f.link = LinkState::Connected;
    CHECK(MapState(f).state == State::Joined);
    // #19: whoever finished first waits for the others, and the page says for whom.
    f.session_role = 0;
    f.prologue = false;
    f.others_prologue = "Hunter1";
    CHECK(MapState(f).state == State::WaitingForWorld && MapState(f).detail == "Waiting: Hunter1 is in the prologue");
    f.role = Role::Host;
    f.link = LinkState::Hosting;
    f.others_prologue = "Hunter1 and Hunter2";
    CHECK(MapState(f).state == State::Hosting && MapState(f).detail == "2/3, Waiting: Hunter1 and Hunter2 are in the prologue");
    f.prologue = true;  // its own prologue comes first
    CHECK(MapState(f).detail == "2/3, prologue (first-dream), solo until ready");
    f.prologue = false;
    f.others_prologue.clear();
    f.session_role = 6;
    f.role = Role::Guest;
    f.link = LinkState::Connected;
    CHECK(MapState(f).state == State::Joined);
    party::RosterEntry e;
    e.name = "Hunter1";
    e.slot = 1;
    e.connected = true;
    e.state = party::MemberState::Prologue;
    const auto m = bridge::MapMembers({e}, "Hunter0", true, [](std::uint32_t) { return std::string("m24_01"); });
    CHECK(m.size() == 1 && m[0].area == "m24_01 (prologue)" && !m[0].in_world);
    return 0;
}

// A11: a join held by the host's boss fight / cutscene says why, on both sides; a summon wins.
static int test_host_busy() {
    using party::LinkState;
    using party::status::Role;
    using party::status::State;
    Facts f;
    f.role = Role::Guest;
    f.have_link = true;
    f.link = LinkState::Connected;
    f.world_up = true;
    f.session_role = 0;
    f.host_busy = "Waiting: the host is in a boss fight";
    CHECK(MapState(f).state == State::WaitingForWorld && MapState(f).detail == "Waiting: the host is in a boss fight");
    f.session_role = 6;  // summoned after the fight
    CHECK(MapState(f).state == State::Joined);
    f.role = Role::Host;
    f.link = LinkState::Hosting;
    f.members_connected = 2;
    f.max_players = 2;
    f.session_role = 0;
    f.host_busy = "Joiners wait: you are in a boss fight";
    CHECK(MapState(f).state == State::Hosting && MapState(f).detail == "2/2, Joiners wait: you are in a boss fight");
    f.host_busy.clear();
    CHECK(MapState(f).detail == "2/2");
    return 0;
}

// A refused guest: plays alone online, gets one message box with what to do; Rejoin's password.
static int test_refusal() {
    using bridge::RefusalNotice;
    using bridge::RefusalPlaysSolo;
    CHECK(!RefusalPlaysSolo(RejectCode::None) && !RefusalPlaysSolo(RejectCode::Shutdown));
    for (RejectCode c : {RejectCode::Auth, RejectCode::Full, RejectCode::Version, RejectCode::Mismatch,
                         RejectCode::Kicked, RejectCode::Name, RejectCode::Protocol}) {
        CHECK(RefusalPlaysSolo(c));
        const std::string t = RefusalNotice(c, "");
        CHECK(t.rfind("Party: ", 0) == 0 && t.find('\n') != std::string::npos);
    }
    CHECK(RefusalNotice(RejectCode::Shutdown, "x").empty() && RefusalNotice(RejectCode::None, "").empty());
    const std::string auth = RefusalNotice(RejectCode::Auth, bridge::AuthRejectText(true, true, true));
    CHECK(auth.find("Wrong party password") != std::string::npos && auth.find("Rejoin the host") != std::string::npos);
    CHECK(RefusalNotice(RejectCode::Full, "").find("full") != std::string::npos);
    CHECK(RefusalNotice(RejectCode::Mismatch, "eboot.bin differs").find("eboot.bin differs") != std::string::npos);
    // Rejoin: System > Party's password when it changed, else what the link joined with.
    CHECK(bridge::RejoinPassword("env-wrong", "", "", false) == "env-wrong");
    CHECK(bridge::RejoinPassword("env-wrong", "", "fixed", false) == "fixed");
    CHECK(bridge::RejoinPassword("old", "old", "old", false) == "old");
    CHECK(bridge::RejoinPassword("old", "old", "", false).empty());  // cleared in the menu
    CHECK(bridge::RejoinPassword("x", "", "y", true).empty());       // the code carries no password

    // The board: the box on a guest's refusal, cleared once in.
    status::ResetForTest(true);
    bridge::OnRuntimeStart(false, "Guest1");
    bridge::OnLinkState(LinkState::Connecting, "", RejectCode::None, "");
    bridge::OnLinkState(LinkState::Rejected, "", RejectCode::Auth, "Wrong or missing party password");
    status::Board b = status::Snapshot();
    CHECK(b.alert_seq == 1 && b.alert.find("Wrong or missing party password") != std::string::npos);
    bridge::OnLinkState(LinkState::Connected, "", RejectCode::None, "");
    b = status::Snapshot();
    CHECK(b.alert_seq == 2 && b.alert.empty());
    return 0;
}

int main() {
    if (test_map_state() || test_members() || test_board_and_commands() || test_prologue() || test_host_busy() || test_refusal()) return 1;
    std::printf("%d checks OK\n", g_checks);
    return 0;
}
