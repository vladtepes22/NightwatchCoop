// SPDX-License-Identifier: GPL-3.0-or-later
// Party status board (gpu/shim/party/party_status.h): snapshot, events, command queue, threads.
// ninja -C out/gpu party-status-test && out/gpu/party-status-test
#include "party/party_status.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace party::status;

static void test_snapshot() {
    ResetForTest(true);
    Board b = Snapshot();
    assert(b.enabled && b.role == Role::Off && b.state == State::Off && b.members.empty());
    assert(b.version == 0 && b.event_seq == 0);

    SetRole(Role::Host);
    SetCode("BBP1-ABCD-EFGH");
    SetState(State::Hosting, "1/3");
    SetMembers({{"Hunter0", true, true, 0, 0, "Hunter's Dream", true},
                {"Hunter1", true, false, 34, 1, "Cathedral Ward", false}});
    b = Snapshot();
    assert(b.role == Role::Host && b.code == "BBP1-ABCD-EFGH");
    assert(b.state == State::Hosting && b.detail == "1/3");
    assert(b.members.size() == 2 && b.members[1].name == "Hunter1" && b.members[1].ping_ms == 34);
    assert(b.members[0].local && b.members[1].area == "Cathedral Ward");
    const auto v = b.version;
    SetState(State::Hosting, "1/3");  // unchanged: no new version
    SetRole(Role::Host);
    assert(Snapshot().version == v);
    SetState(State::Joined, "2/3");
    assert(Snapshot().version == v + 1);

    assert(EventSeq() == 0);
    SetLastEvent("Hunter1 joined");
    b = Snapshot();
    assert(EventSeq() == 1 && b.event_seq == 1 && b.last_event == "Hunter1 joined" && b.event_ms > 0);
    SetLastEvent("Hunter1 left");
    assert(EventSeq() == 2 && Snapshot().last_event == "Hunter1 left");

    assert(std::strcmp(StateName(State::WaitingForWorld), "waiting_for_world") == 0);
    assert(std::strcmp(StateName(State::RingingBell), "ringing_bell") == 0);
    assert(std::strcmp(RoleName(Role::Guest), "guest") == 0);

    ResetForTest(false);
    assert(!Snapshot().enabled && !Enabled() && Snapshot().code.empty() && EventSeq() == 0);
}

static void test_commands() {
    ResetForTest(true);
    Command c;
    assert(!PopCommand(&c));

    RequestLeave();
    RequestLeave();  // coalesced
    RequestRejoin();
    assert(PopCommand(&c) && c.type == CommandType::Leave);
    assert(PopCommand(&c) && c.type == CommandType::Rejoin);
    assert(!PopCommand(&c));

    // Kick: host only, remote members of the roster only.
    SetRole(Role::Guest);
    SetMembers({{"Hunter0", true, true, 0, 0, "", false}, {"Hunter1", true, true, 5, 1, "", true}});
    assert(!KickMember("Hunter0"));
    SetRole(Role::Host);
    SetMembers({{"Hunter0", true, true, 0, 0, "", true}, {"Hunter1", true, true, 5, 1, "", false},
                {"Hunter2", false, false, -1, 2, "", false}});
    assert(!KickMember(""));
    assert(!KickMember("Hunter0"));  // self
    assert(!KickMember("Nobody"));
    assert(KickMember("Hunter2"));
    assert(KickMember("Hunter2"));  // coalesced
    assert(KickMember("Hunter1"));
    assert(PopCommand(&c) && c.type == CommandType::Kick && c.name == "Hunter2");
    assert(PopCommand(&c) && c.type == CommandType::Kick && c.name == "Hunter1");
    assert(!PopCommand(nullptr));

    // Bounded: the oldest are dropped.
    for (int i = 0; i < 40; ++i) {
        const std::string name = "M" + std::to_string(i);
        std::vector<Member> ms = {{"H", true, true, 0, 0, "", true}, {name, true, false, 1, 1, "", false}};
        SetMembers(ms);
        assert(KickMember(name));
    }
    int n = 0;
    std::string first;
    while (PopCommand(&c)) {
        if (n++ == 0) first = c.name;
    }
    assert(n == 16 && first == "M24");

    // No code: nothing to copy (the clipboard is not touched).
    SetCode("");
    assert(!RequestCopyCode());
}

static void test_threads() {
    ResetForTest(true);
    std::atomic<bool> stop{false};
    std::thread producer([&] {
        for (int i = 0; i < 20000; ++i) {
            SetState(i % 2 ? State::Joined : State::Travelling, "d" + std::to_string(i));
            SetMembers({{"A", true, true, i % 100, 0, "area", true}, {"B", i % 3 != 0, false, 7, 1, "", false}});
            if (i % 100 == 0) SetLastEvent("event " + std::to_string(i));
        }
        stop = true;
    });
    std::thread ui([&] {
        std::uint64_t last_version = 0;
        while (!stop) {
            const Board b = Snapshot();
            assert(b.version >= last_version);
            last_version = b.version;
            assert(b.members.empty() || b.members.size() == 2);
            RequestRejoin();
        }
    });
    std::thread director([&] {
        Command c;
        while (!stop) {
            while (PopCommand(&c)) assert(c.type == CommandType::Rejoin);
        }
    });
    producer.join();
    ui.join();
    director.join();
    assert(EventSeq() == 200);
}

int main() {
    test_snapshot();
    test_commands();
    test_threads();
    std::printf("party-status-test: ok\n");
    return 0;
}
