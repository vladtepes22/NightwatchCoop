// SPDX-License-Identifier: GPL-3.0-or-later
// PartyHostService: what The Hunter's Dream server did for bbhost's Matching2 layer, run inside
// the party host's game process (docs/PARTY_COOP_PLAN.md A3). Semantics reimplemented from the
// server contract visible in droogie/bbhost src/net/session.cpp, src/hle/np_matching2.cpp and
// tools/nettest/simclient.py @8f2746c (no server code was available or copied):
//
//  - contexts: each party member's signaling endpoint from its context_start (local address,
//    STUN-mapped address as the host's relay saw it, relay port);
//  - rooms: MaxMembers (the request's, else BB_PARTY_MAX, default 3) with member ids from 1
//    (the creator, the owner), a SessionId per room, joins capped at BB_PARTY_MAX members;
//  - per-member event queues with cursors (EventId), long-poll wait_events + ack_events;
//  - heartbeats: a member silent for 15 s leaves the room ("timeout"; the owner's timeout
//    closes it) - unless the roster says it is loading (set_member_loading / the loading
//    query hook), which holds the timeout until it is back;
//  - signaling resolve: a member's reachable address, preferring what it registered at
//    context_start (a mapped address that differs from its local one, else the local one),
//    with the host relay's port for that address when it has one.
// Thread-safe: every entry point locks; events wake waiters.
#pragma once

#include "json.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bbnet::party {

struct Caller {
    std::string online_id;         // who is calling (PartyLink's authenticated name; ours on the host)
    std::uint32_t link_addr = 0;   // the caller's address as the host sees its PartyLink (network order), 0 = in-process
};

class PartyHostService {
public:
    PartyHostService();
    ~PartyHostService();
    PartyHostService(const PartyHostService&) = delete;
    PartyHostService& operator=(const PartyHostService&) = delete;

    // One PartyTransport call (party_transport.h). Always fills `reply` (ResKind 0 or an error).
    void handle(const Caller& caller, const std::string& kind, const json::Value& request, json::Value& reply);

    // --- events ---
    // Queues an event (an object with Name) for `online_id`; returns its EventId.
    std::uint64_t push_event(const std::string& online_id, json::Value event);
    // Events after `cursor` for `online_id`, waiting up to wait_ms for the first (0 = no wait).
    std::vector<json::Value> wait_events(const std::string& online_id, std::uint64_t cursor, int wait_ms,
                                         std::size_t max = 16);
    // Drops the events up to `cursor` (they were handled).
    void ack_events(const std::string& online_id, std::uint64_t cursor);
    // Wakes every waiter (shutdown, tests).
    void wake_all();

    // --- the roster hooks (A4/A5) ---
    // A member whose game is loading (map load, death, warp): its heartbeat timeout is held.
    void set_member_loading(const std::string& online_id, bool loading);
    // Optional query consulted as well (the roster's PROGRESS state); called under no lock of ours.
    void set_loading_query(std::function<bool(const std::string& online_id)> query);
    // The member's link went away: its context is dropped, peers hear peer_deactivated; its room
    // membership then times out as usual (or is held while the roster says loading).
    void context_gone(const std::string& online_id);
    // A member's game died (its party link ended with a TCP reset): its room memberships end now
    // (MEMBER_LEFT + signaling DEAD to the other games), as at its restart, so no game keeps its
    // frozen character until the session's own timeout.
    void member_crashed(const std::string& online_id);
    // Members whose game registered a new context while the host still knew their old one (a
    // crash restart), since the last call; the host's director clears the host game's stale peer
    // reservation for them (fast join, docs/party/join_timing.md).
    std::vector<std::string> take_restarted();
    // The host's own reachable endpoint for guests (A4: public address, UPnP, LAN), used for the
    // host's records when its context_start carried no mapped address.
    void set_host_endpoint(const std::string& online_id, const std::string& addr, int port);

    // --- time (tests) ---
    void set_clock(std::function<std::int64_t()> now_ms);
    void set_heartbeat_timeout_ms(int ms);
    // F5: our game dropped this member during a network outage (its session's own silence
    // timeout): its room membership ends now ("timeout"), not at the heartbeat timeout after the
    // network is back, so our game is done with that session before the member is summoned again.
    bool member_timed_out(const std::string& online_id);
    // Applies timeouts now (the service's own reaper thread calls this every second).
    void tick();
    // Starts the reaper thread (idempotent); tests drive tick() themselves instead.
    void start_reaper();

    // --- queries (FromApi, status, tests) ---
    struct MemberView {
        std::uint16_t member_id = 0;
        std::string online_id;
        bool loading = false;
    };
    struct RoomView {
        std::uint64_t room_id = 0;
        std::string session_id;
        std::uint16_t owner_id = 0;
        int max_members = 0;
        std::vector<MemberView> members;
        json::Value extra;  // HostArea, HostLevel, HostPos, MemberTag from create_room
    };
    // The room `online_id` owns (true) or is in.
    bool room_of(const std::string& online_id, RoomView* out, bool owned_only = false) const;
    bool room(std::uint64_t room_id, RoomView* out) const;
    // A member record for `online_id` as signaling_resolve answers it (false: no context).
    bool resolve(const std::string& online_id, json::Value* record) const;
    std::string status_line() const;

    // The process's service (the host's; created on first use).
    static PartyHostService& instance();

private:
    struct Impl;
    std::shared_ptr<Impl> d_;  // shared with the reaper thread
};

// BB_PARTY_MAX (2..8, default 3): members in a party room, the host included.
int party_max_members();

}  // namespace bbnet::party
