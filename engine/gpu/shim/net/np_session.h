// SPDX-License-Identifier: GPL-3.0-or-later
// derived from droogie/bbhost src/net/session.h @8f2746c
//
// The session layer behind sceNpMatching2 / sceNpSignaling (np_matching2.cpp,
// np_signaling.cpp). Three parts, none of which knows the SDK's structs:
//  - a scheduler: runs closures after a delay in (due, priority, order) order on one host
//    thread ("bb:np-session"); guest callbacks are never made there - closures post them to
//    the A1 guest-callback dispatcher (bbnet::post_guest_call), which keeps the order;
//  - the peer table (member id -> addresses and signaling state), shared by both libraries;
//  - the transport side: our endpoint (local address, the STUN-mapped address the party host
//    saw, the relay port), the keepalive that refreshes it every 15 s, and the PartyTransport
//    calls (party_transport.h) that replaced bbhost's HTTP to its private server.
// bbport: bbhost's poller is the transport's event stream; accounts, plugins and rulesets are
// gone; the summon invite (A5) and the host's area/level/position are hooks.
#pragma once

#include "json.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bbnet::party {
class PartyTransport;
}

namespace bbnet::session {

// Priority when several closures are due at once: the contract's order.
enum class Prio { Context = 0, Request = 1, Signaling = 2, RoomEvent = 3 };

// Runs fn on the session thread after delay_ms. Serial: no two run at once.
void dispatch_after(int delay_ms, Prio prio, std::function<void()> fn);
bool on_session_thread();
// Waits until every closure due by now + horizon_ms has run (tests).
void drain(int horizon_ms);

// The online id we play as (BB_PARTY_NAME / BB_USER_NAME).
std::string online_id();
// Our P2P endpoint: the LAN address (BB_PARTY_LOCAL_IP overrides) and the party UDP port.
std::string local_addr_text();
int p2p_port();

struct Peer {
    std::uint16_t member_id = 0;
    std::string online_id;
    std::uint32_t addr = 0;        // network byte order, the address we reach it at
    std::uint16_t port = 0;        // host byte order
    std::uint32_t local_addr = 0;  // network byte order
    std::uint16_t local_port = 0;
    unsigned conn_id = 0;          // NpSignaling connection id once activated (0 = none)
    int sig_status = 0;            // 0 inactive, 1 pending, 2 active
};

// The peer table. All calls lock internally.
void peers_clear();
void peers_upsert(const Peer& p);  // by member_id; keeps conn_id/sig_status when the caller left them 0
bool peers_get(std::uint16_t member_id, Peer* out);
bool peers_find_online(const std::string& online_id, Peer* out);
bool peers_find_conn(unsigned conn_id, Peer* out);
void peers_set_conn(std::uint16_t member_id, unsigned conn_id, int sig_status);
std::vector<Peer> peers_all();
void peers_erase(std::uint16_t member_id);
std::int64_t now_ms();
// Parses member records ({MemberId, OnlineId, Addr, Port, LocalAddr, LocalPort, MappedAddr,
// MappedPort}) into the table. Returns how many.
int peers_from_members(const json::Value& members);
// A peer known by address before it is a room member (provisional ids 0xff00..).
Peer peers_provisional(const std::string& online_id, std::uint32_t addr, std::uint16_t port);

// The transport calls (bbhost's server_*). Each returns false with `error` on failure.
party::PartyTransport& transport();
bool server_context_start(json::Value& reply, std::string& error);
// The party link reached a host that does not know this session (it restarted, or our slot
// was released): the relay registration is refreshed and, once the game started a context,
// context_start is sent again (on the session thread).
void host_session_reset();
bool server_create_room(int max_members, const json::Value& extra, json::Value& reply, std::string& error);
bool server_join_room(std::uint64_t room_id, json::Value& reply, std::string& error);
bool server_leave_room(const std::string& session_id, int member_id, json::Value& reply, std::string& error);
// 1 in the room, 0 not in it any more, -1 not asked (the link is down).
int server_heartbeat(const std::string& session_id, int member_id);
bool server_kick_member(const std::string& session_id, int member_id, int kicker_id, const std::uint8_t* opt,
                        std::size_t opt_len, std::string& error);
// addr in network byte order, port in host order.
bool server_signaling_resolve(const std::string& online_id, std::uint32_t* addr, std::uint16_t* port,
                              std::string& error);

// The event stream (the transport's), handled on the transport's thread; the handler must not
// call guest code (it schedules through dispatch_after). Starts the 15 s STUN keepalive too.
void events_start(std::function<void(const json::Value& event)> handler);
void events_stop();

// The room we are in, for the keepalive's address updates.
void room_set(const std::string& session_id, int member_id);
void room_clear();
// The last STUN round trip, microseconds (0 = none yet).
int stun_rtt_us();

// Where our STUN Binding Requests go (a guest: the party host's address and party port, A4);
// unset (the host) means no STUN - the host's mapped address is set_mapped_override's.
void set_stun_server(const std::string& host, std::uint16_t port);
// The host's public endpoint (A4: UPnP / public STUN / LAN), reported as our MappedAddr.
void set_mapped_override(const std::string& addr, int port);

// --- game data (the image bbnet_set_image recorded) ---
// The NpMatching2 signaling gate (our 0x546cc09): the game's callbacks queue events only
// when it is 1; on PSN the library set it. True when written (or already 1).
bool set_signaling_gate();
// The host's area, level, position for the invite (HostArea, HostLevel, HostPos, MemberTag),
// from FrpgNetMan (our 0x553b120: +0xa7c area, +0xaa8 level, +0xaac position) unless a hook
// is set (A5).
void host_extra(json::Value& extra);
void set_host_extra_hook(std::function<void(json::Value& extra)> hook);

// --- the summon invite (A5) ---
struct SummonInvite {
    std::uint64_t room_id = 0;
    std::uint32_t member_tag = 0;
    std::uint32_t host_area = 0;
    float host_pos[3] = {0, 0, 0};
    std::uint32_t host_level = 0;
    std::string host_online_id;
};
// Called on the session thread when a guest_invite arrives (bbhost's summon_invite_deliver,
// which is off by default there too: the host's game sends its own type-1 item over P2P).
// Without a hook the invite is logged only.
void set_invite_hook(std::function<void(const SummonInvite&)> hook);
void deliver_invite(const SummonInvite& inv);

}  // namespace bbnet::session
