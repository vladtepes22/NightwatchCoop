// SPDX-License-Identifier: GPL-3.0-or-later
// PartyTransport: the seam between the game-facing NP/HTTP layer (np_matching2.cpp,
// np_signaling.cpp, http_hle.cpp, np_session.cpp) and the party host service. It replaces
// bbhost's np_post() HTTP calls to the private server (/mp/matching2/*, /np/signaling/*,
// /np/events/*): the same JSON bodies, carried in-process on the host (LocalHost) and over
// PartyLink RPC on guests (RemoteGuest, A4).
//
// Every call is one JSON object in, one JSON object out. The kinds and their shapes (field
// names are bbhost's / The Hunter's Dream server's, so traces compare 1:1):
//
//  "context_start"     {OnlineId, SignalingAddr, SignalingPort, MappedAddr, MappedPort, RelayPort?}
//                   -> {ResKind:0, OnlineId}
//  "create_room"       {OnlineId, LocalAddr, LocalPort, PublicAddr, PublicPort, MappedAddr, MappedPort,
//                       MaxMembers, HostArea?, HostLevel?, HostPos?[3], MemberTag?}
//                   -> {ResKind:0, RoomId, SessionId, MemberId, MaxMembers, OwnerMemberId}
//  "join_room"         {<endpoint as create_room>, RoomId}
//                   -> {ResKind:0, RoomId, SessionId, MemberId, MaxMembers, OwnerMemberId,
//                       Members:[MemberRecord...]}   (every member but the joiner)
//  "leave_room"        {SessionId, MemberId}                         -> {ResKind:0}
//  "heartbeat"         {SessionId, MemberId}                         -> {ResKind:0, InRoom:0|1}
//  "kick_member"       {SessionId, MemberId, KickerMemberId, OptData(base64, <=16 bytes)}
//                   -> {ResKind:0}
//  "signaling_resolve" {OnlineId}  -> {ResKind:0, OnlineId, Addr, Port, LocalAddr, LocalPort,
//                                      MappedAddr, MappedPort, RelayPort}
//  "signaling_update"  {OnlineId, MappedAddr, MappedPort, SessionId, MemberId} -> {ResKind:0}
//  "http"              {Method:"GET"|"POST"|"PUT"|"HEAD", Url, Headers:["Name: value"...],
//                       Body(base64)}  -> {ResKind:0, Status, ContentType, Body(base64)}
//                       (the guest game's FROM API traffic, answered by the host's FromApi)
// A refusal is {ResKind:<nonzero>, Error:"text"}; a call that could not be made at all
// returns false with `error` (the link is down, a timeout).
//
// MemberRecord = {MemberId, OnlineId, Addr, Port, LocalAddr, LocalPort, MappedAddr, MappedPort,
//                 RelayPort}. Addresses are dotted IPv4 text, ports numbers (host order).
//
// Events (the host service's per-member queue; on a guest pushed over PartyLink EVENT):
//  {EventId, Name:"room_member_joined", RoomId, <MemberRecord fields>}
//  {EventId, Name:"room_member_left",   RoomId, MemberId, OnlineId, Reason:"leave_room"|"kicked"|"timeout"}
//  {EventId, Name:"room_closed",        RoomId, Reason:"host_left"|"host_timeout"|"server"}
//  {EventId, Name:"room_member_kicked", RoomId, MemberId, OptData(base64)}
//  {EventId, Name:"peer_deactivated",   OnlineId}
//  {EventId, Name:"guest_invite",       RoomId, HostOnlineId, HostAddr, HostPort, HostLocalAddr,
//                                       HostLocalPort, HostMappedAddr, HostMappedPort, MemberTag,
//                                       HostArea, HostLevel, HostPos:[x,y,z]}
// EventId grows by one per recipient; a receiver acknowledges the highest id it handled
// (cursor semantics of bbhost's /np/events/poll + /np/events/ack).
#pragma once

#include "json.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bbnet::party {

class PartyHostService;
class FromApi;
struct Caller;

// The call kinds, as their wire names.
namespace call {
constexpr const char* kContextStart = "context_start";
constexpr const char* kCreateRoom = "create_room";
constexpr const char* kJoinRoom = "join_room";
constexpr const char* kLeaveRoom = "leave_room";
constexpr const char* kHeartbeat = "heartbeat";
constexpr const char* kKickMember = "kick_member";
constexpr const char* kSignalingResolve = "signaling_resolve";
constexpr const char* kSignalingUpdate = "signaling_update";
constexpr const char* kHttp = "http";
}  // namespace call

using EventHandler = std::function<void(const json::Value& event)>;

class PartyTransport {
public:
    virtual ~PartyTransport() = default;
    virtual const char* name() const = 0;
    // One call (see the table above). Blocking; never called on a game thread by the NP layer
    // (it runs on the session scheduler) - http_hle's nonblocking requests call it from a worker.
    virtual bool rpc(const char* kind, const json::Value& request, json::Value& reply, std::string& error,
                     int timeout_ms = 4000) = 0;
    // Starts delivering our events to `handler` (one thread, in EventId order, each once);
    // replaces an earlier handler. The handler must not call guest code.
    virtual void start_events(const std::string& online_id, EventHandler handler) = 0;
    virtual void stop_events() = 0;
    // True when the host is this machine (loopback addresses in its records are real).
    virtual bool host_is_local() const = 0;

    // Typed helpers over rpc(); each returns false with `error` (ResKind != 0 included).
    bool ok_call(const char* kind, const json::Value& request, json::Value& reply, std::string& error,
                 int timeout_ms = 4000);
};

// The host: calls PartyHostService directly as `online_id` (this game's own account).
class LocalHost final : public PartyTransport {
public:
    // `api` answers the "http" call (nullptr: refused).
    explicit LocalHost(PartyHostService& service, FromApi* api = nullptr);
    ~LocalHost() override;
    const char* name() const override { return "local-host"; }
    bool rpc(const char* kind, const json::Value& request, json::Value& reply, std::string& error,
             int timeout_ms) override;
    void start_events(const std::string& online_id, EventHandler handler) override;
    void stop_events() override;
    bool host_is_local() const override { return true; }

private:
    struct Pump;
    PartyHostService& service_;
    FromApi* api_;
    std::unique_ptr<Pump> pump_;
};

// A guest: every call is a PartyLink RPC to the host, every event a PartyLink EVENT.
//
// TODO(A4/A5): wire to PartyLink. The integration is mechanical:
//   rpc(kind, req, reply, err, timeout) -> link.rpc_call(kind, json::dump(req, 0), timeout)
//       and json::parse the RPC_RESP payload into `reply`; false + error when the link is
//       down or the call timed out.
//   On the host, PartyLink's RPC_REQ handler calls
//       PartyHostService::handle(Caller{peer online id, peer TCP address}, kind, req, reply)
//   and answers RPC_RESP with json::dump(reply, 0).
//   Events: the host runs one pump per connected guest -
//       service.wait_events(guest, cursor, 1000) -> send EVENT(json) for each; on the guest's
//       ACK(EventId) call service.ack_events(guest, EventId). Here, on_link_event(json) feeds
//       the handler (dropping EventIds already seen) and the link ACKs it.
//   Address fix-up: records whose OnlineId is the host's and whose Addr is empty or not
//       reachable from here get the address this guest's PartyLink connected to (set_host_endpoint).
class RemoteGuest final : public PartyTransport {
public:
    using RpcFn = std::function<bool(const char* kind, const std::string& request_json, std::string& reply_json,
                                     std::string& error, int timeout_ms)>;
    RemoteGuest();
    const char* name() const override { return "remote-guest"; }
    // The PartyLink call; until it is set every rpc fails with "no party link".
    void set_rpc(RpcFn fn);
    // The host's address and party UDP port as this guest reaches them (PartyLink's peer).
    void set_host_endpoint(std::uint32_t addr_nbo, std::uint16_t port, bool loopback);
    // The host's online id: its signaling_resolve answer gets the same address fix-up.
    void set_host_online_id(const std::string& id);
    // An EVENT frame's payload from PartyLink; returns the EventId to ACK (0 when unparsable).
    std::uint64_t on_link_event(const std::string& event_json);
    // The host restarted (PartyLink WELCOME not resumed): its EventIds start again at 1.
    void reset_event_cursor();

    bool rpc(const char* kind, const json::Value& request, json::Value& reply, std::string& error,
             int timeout_ms) override;
    void start_events(const std::string& online_id, EventHandler handler) override;
    void stop_events() override;
    bool host_is_local() const override;
    // Solo (a guest the host refused: wrong password, old code, full, version): the game's calls
    // go to this process's own host service and FromApi, like a host with no guests, so the game
    // plays online alone in its own world instead of failing its server checks ("Failed to
    // acquire Bloodborne server information"). Off again once a new link is up (Rejoin). Returns
    // whether the mode changed.
    bool set_solo(bool on);
    bool solo() const;

private:
    void fix_host_addresses(json::Value& v) const;
    struct State;
    std::shared_ptr<State> st_;
};

// The host's entry point for a call that reached it - in-process or a guest's PartyLink
// RPC_REQ (A4: host_handle(Caller{peer name, peer address}, kind, request, reply)): "http"
// goes to FromApi::instance(), the rest to PartyHostService::instance().
void host_handle(const Caller& caller, const std::string& kind, const json::Value& request, json::Value& reply);

// The process's transport: LocalHost over the process's PartyHostService on the host
// (BB_PARTY=host), a RemoteGuest otherwise. set_transport replaces it (tests, A4).
PartyTransport& transport();
void set_transport(std::shared_ptr<PartyTransport> t);
// The RemoteGuest when this process is a guest (nullptr on the host), for A4 to wire.
RemoteGuest* remote_guest();

}  // namespace bbnet::party
