// SPDX-License-Identifier: GPL-3.0-or-later
// Shared declarations of the party network library (gpu/shim/net): the settings read from the
// environment, the logging helper, the guest-callable export tables, the P2P port services the
// session layer uses (STUN, relay, hole punching) and the guest-callback dispatcher.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#define BBNET_ABI __attribute__((sysv_abi))

// Runtime entry points (src/probe.c, src/runtime_thread.c, src/runtime.c).
extern "C" {
BBNET_ABI void restore_guest_fs(void);
void runtime_thread_attach_host(const char* name);
const char* runtime_symbol(const char* nid);
}

namespace bbnet::udp {
class RelayClient;
class Router;
struct Ip6;
struct Ep6;
struct Wire;
}

namespace bbnet {

// Every guest-callable function restores the guest FS base on its way out (as the runtime's
// mutex and semaphore functions do): Windows does not keep FS across a wait, and a blocking
// call (epoll, recv, a contended lock) would return to guest code with the TEB's FS.
struct GuestReturn {
    GuestReturn() = default;
    GuestReturn(const GuestReturn&) = delete;
    GuestReturn& operator=(const GuestReturn&) = delete;
    ~GuestReturn() { restore_guest_fs(); }
};
#define BBNET_GUEST_RETURN() ::bbnet::GuestReturn bbnet_guest_return_

struct Settings {
    bool party = false;          // BB_PARTY set (non-empty)
    bool host = false;           // BB_PARTY=host (or BB_PARTY_HOST=1): answers relay HELLOs
    std::string party_value;     // BB_PARTY as given
    std::uint16_t party_port = 9307;  // BB_PARTY_PORT: the UDP port the game's P2P port 3658 maps to
    std::string online_id;       // BB_PARTY_NAME (validated) else BB_USER_NAME, at most 16 characters
    bool np_state_callback = true;  // BB_NP_STATE_CB=0: sceNpCheckCallback fires nothing
    std::string app0, user_dir;  // from bbnet_configure
};
const Settings& settings();
void set_dirs(const char* app0, const char* user_dir);

void log(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));

struct Export {
    const char* name;
    void* fn;
};
// Guest-callable functions by symbol name; nullptr-terminated.
extern const Export kNetExports[];
extern const Export kNpExports[];
extern const Export kMatching2Exports[];  // np_matching2.cpp
extern const Export kSignalingExports[];  // np_signaling.cpp
extern const Export kHttpExports[];       // http_hle.cpp (sceHttp*, sceSsl*)

// --- The guest image (bbnet_set_image, from bbgpu_patch_image) ---
// The loaded eboot's base (0x800000000 in this runtime) and size; 0 until the loader set them.
std::uintptr_t guest_image_base();
std::uint64_t guest_image_size();
// The address of `offset` (raw ELF VA, docs/PARTY_COOP_PLAN.md "our offset") inside the image,
// or nullptr when no image is known or [offset, offset+len) is outside it.
void* guest_image_at(std::uint64_t offset, std::size_t len);
// Copies n bytes of game memory at `addr` (image or heap) into `out`; false when the range
// is not committed readable memory (a pointer the game has not set up yet).
bool guest_read(std::uintptr_t addr, void* out, std::size_t n);
// The game process's fault-recovering reader (coop::SafeRead), installed by the party director at
// start-up. Without it guest_read checks the range with VirtualQuery first, which costs
// milliseconds per call on Windows once the address space is large (GitHub issue #3).
void set_guest_reader(bool (*reader)(std::uint64_t addr, void* out, std::size_t n));
// Writes n bytes at `addr` when the range is committed writable memory.
bool guest_write(std::uintptr_t addr, const void* in, std::size_t n);
// BB_PARTY_TRACE=1: log every party request and answer.
bool party_trace();

// --- The P2P port (net_socket.cpp) ---

// The UDP port the party traffic uses, once bound (0 before).
std::uint16_t p2p_bound_port();
// Opens the party UDP port now (the game opens it at its first P2P bind); false on failure.
bool p2p_open();
// The party link's UDP path (party_rudp.h) shares the party port: a datagram out of it (addr /
// port network order), and the handler that gets the link's datagrams (magic fa 'L') from the
// port's reader thread.
bool p2p_send_raw(std::uint32_t addr, std::uint16_t port, const std::uint8_t* d, std::size_t n);
void p2p_set_link_handler(std::function<void(std::uint32_t, std::uint16_t, const std::uint8_t*, std::size_t)> fn);
// Sends a STUN Binding Request from the party port to host:port and waits for the answer;
// with `want_relay` the request carries the relay HELLO and a relay in the answer turns
// relay framing on for that server's other ports.
struct RelayInfo {
    bool present = false;
    std::uint16_t vport = 0;
    std::uint32_t observed_addr = 0;  // network order
    std::uint16_t observed_port = 0;
};
bool p2p_stun(const char* host, std::uint16_t port, int timeout_ms, std::uint32_t* mapped_addr,
              std::uint16_t* mapped_port, bool want_relay = false, RelayInfo* relay = nullptr);
bool p2p_relay(std::uint32_t* server, std::uint16_t* vport);
// The guest-side relay state (party_udp.h), for routing a peer through the host relay.
const udp::RelayClient& p2p_relay_client();
// A guest learned another guest's addresses: its direct address (network order, host-order
// port) and its relay port on the host (0 = none). Its game traffic then goes direct while
// the peer answers probes, through the relay otherwise (party_udp.h PeerPaths).
void p2p_add_peer(const char* label, std::uint32_t addr, std::uint16_t port_host, std::uint16_t relay_port);
// "addr:port direct|relay|probing, ..." for the status line.
std::string p2p_paths_status();
// Host side: the relay port the host's relay gave the client seen at addr (network order) :
// port_host (host order), from its STUN HELLO; false when that address never asked for one.
bool p2p_relay_vport_for(std::uint32_t addr, std::uint16_t port_host, std::uint16_t* vport);
// Host side: which source addresses (network order) the party port answers STUN for and gives
// relay ports to (the party runtime: its PartyLink members). Unset: everyone.
void p2p_set_relay_admit(std::function<bool(std::uint32_t addr)> admit);
void p2p_punch(const char* label, std::uint32_t addr, std::uint16_t port, std::uint32_t local_addr,
               std::uint16_t local_port);
// F2 (0.7.1, party_route.h): the party port's route table (IPv6, the member relay), which every
// datagram of the port passes; the party runtime configures it.
udp::Router& p2p_router();
// IPv6 on the party port: a second socket on its number (now, or when the port opens) and IPv6
// routes in the table. Off: IPv4 only, as before.
void p2p_set_ipv6(bool on);
bool p2p_ipv6_open();
// Another guest's IPv6 endpoint (the host's net6 table) for its IPv4 key: PeerPaths probes it
// too and its traffic goes over IPv6 when IPv4 does not answer.
void p2p_peer_v6(std::uint32_t addr, std::uint16_t port_host, const udp::Ep6& e6);
// One datagram straight onto the wire (party_route.h Wire: the host's IPv6 nonce check).
bool p2p_send_wire(const udp::Wire& w);
// Opens our IPv6 firewall towards a guest (its BBG6 code): probes from the party port for 10 s.
void p2p_punch6(const char* label, const udp::Ep6& e6);
// This PC's global IPv6 address (the shortest-lived preferred one: a temporary privacy address
// where there is one; BB_PARTY_IPV6_ADDR
// forces one, ::1 included, for loopback tests). False when there is none.
bool local_ipv6(udp::Ip6* out, std::string* how = nullptr);
// How long ago the party port last heard addr (network order) : port (host order), anything (a
// probe's answer too), ms; -1: never (or no party port).
std::int64_t p2p_ms_since_heard(std::uint32_t addr, std::uint16_t port_host);
// One line about the party port (counters), for the status line.
std::string p2p_status();
// This machine's LAN IPv4 address (network order), BB_PARTY_LOCAL_IP overriding; 0 when none.
// Chosen once (the first use) and logged.
std::uint32_t local_ipv4();

// The LAN address choice (net_socket.cpp query_local_net), exposed for the tests.
// The address forced by the environment (network order): BB_PARTY_LOCAL_IP, else 127.0.0.1 under
// BB_PARTY_LOOPBACK=1 / BB_MP_LOCAL_TEST=1; 0 when nothing is forced. `source` names the variable.
std::uint32_t forced_local_ipv4(std::string* source = nullptr);
// A network adapter as the choice sees it.
struct LanAdapter {
    std::string name;         // friendly name + " / " + description
    std::uint32_t ip = 0;     // network order
    bool loopback = false;    // the software loopback adapter (or 127/8)
    bool tunnel = false;      // IF_TYPE_TUNNEL (Teredo, 6to4 ...)
    bool gateway = false;     // has an IPv4 default gateway
    bool default_route = false;  // the interface the system routes the Internet through
    bool physical = false;    // Ethernet or Wi-Fi interface type
    std::uint32_t metric = 0;    // IPv4 interface metric (lower first), a tie breaker
    std::uint32_t mask = 0;      // network order; 0 unknown (party discovery's directed broadcast)
};
// Hyper-V / WSL / Docker switches, VirtualBox / VMware host-only adapters, VPN and overlay
// adapters (TAP, Wintun, WireGuard, Tailscale, ZeroTier, Hamachi, Radmin ...): by name.
bool adapter_is_virtual(const std::string& name);
// < 0: never chosen (loopback, tunnel, 169.254/16, no address). Otherwise higher wins: a
// non-virtual adapter always beats a virtual one; then default route, gateway, physical type.
int lan_adapter_score(const LanAdapter& a);
// --- Host over a VPN (0.7.5, party_vpn / BB_PARTY_VPN) ---
// The overlay VPNs players use to play together: by adapter name, else (a virtual adapter only) by
// the address range: Radmin 26/8, Hamachi 25/8, Tailscale 100.64/10.
enum class VpnKind { None, Radmin, Hamachi, ZeroTier, Tailscale, Other };
VpnKind vpn_kind(const LanAdapter& a);
// "Radmin VPN", "Hamachi", "ZeroTier", "Tailscale"; Other: the adapter's friendly name.
std::string vpn_label(const LanAdapter& a);
// The preference: "auto" (default, also empty), "off", or an adapter name (case-insensitive part of
// the adapter's name). Anything else is read as a name.
std::string vpn_pref_normalize(const std::string& pref);
// Score of an adapter as the VPN to host over: < 0 not chosen, else 1000 + a rank (higher wins).
// auto: a usable Radmin / Hamachi / ZeroTier / Tailscale adapter (a commercial VPN that carries all
// the PC's traffic is never taken unasked); a name: any usable adapter whose name contains it.
int vpn_adapter_score(const LanAdapter& a, const std::string& pref);
// Index of the adapter to host over, -1 for none (off, or nothing matches). Reusable by LAN/VPN
// discovery: the list is whatever the platform enumerated (up adapters).
int select_vpn_adapter(const std::vector<LanAdapter>& adapters, const std::string& pref);
// The current preference: BB_PARTY_VPN (bbport.ini party_vpn puts it there at start); read when the
// local address is first needed (the game start / hosting start).
std::string vpn_preference();
// This PC's chosen VPN, for the log and the launcher: "name|ip" of the adapter local_ipv4() sits on, or "".
std::string vpn_in_use();

// Queries the adapters now (not cached) and returns the address local_ipv4() would pick;
// `how` describes the choice (the log line).
std::uint32_t query_local_ipv4(std::string* how = nullptr);

// --- NP (np_manager.cpp) ---
void fill_npid(void* out, const char* online);

// --- The party runtime (shim/party/party_runtime.cpp) ---
// It registers itself at static initialization (the network library alone, as in the unit tests,
// runs without one). start: once, lazily, on the library's first use under BB_PARTY (bbnet_resolve /
// bbnet_configure); status: appended to bbnet_status_line.
struct RuntimeHooks {
    void (*start)() = nullptr;
    void (*shutdown)() = nullptr;
    std::string (*status)() = nullptr;
};
void set_runtime_hooks(const RuntimeHooks& hooks);
// Starts the registered runtime once (no-op without BB_PARTY or without a runtime).
void runtime_start_once();

// --- Guest callbacks (bbnet_glue.cpp) ---
// Calls the guest's System V function `fn` with up to six integer arguments on the dispatcher
// thread (attached to the runtime, guest FS restored before each call), in posting order.
void post_guest_call(std::uintptr_t fn, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0,
                     std::uint64_t a3 = 0, std::uint64_t a4 = 0, std::uint64_t a5 = 0);
// Waits until every call posted so far has returned (tests, shutdown).
void drain_guest_calls();

// --- The game's FROM server sign-in as the HTTP layer saw it (http_hle.cpp) ---
// The party director waits for it on the title before it confirms Continue: pressing on while
// the online chain (ss.info, login, sync_chara_id, notices) still runs leaves the game offline.
struct OnlineProgress {
    int ss_info = 0, login = 0, chara_id = 0, notice = 0;  // answers with HTTP 200 so far
    int failures = 0;                                      // requests without an HTTP 200
    double since_last = -1;                                // s since the latest FROM request (-1: none)
};
OnlineProgress online_progress();
// Host: how many signs members have put up on the party's board so far (from_api.cpp). The
// director lets the host's game poll the board at once when it changes (fast join).
std::uint64_t sign_board_changes();
// Host: how many summon_messenger/request calls succeeded so far (from_api.cpp).
std::uint64_t summon_requests();
// Logs the game's online state (FrpgNetMan, the FROM client, the network flow) on every change
// (online_watch.cpp); started by the first sceHttpInit under BB_PARTY.
void online_watch_start();

// Diagnostic packet trace (party_fourp's probe sets it; null = off): every P2P datagram the game
// sends (send = true, on the game's thread) or that is queued for it, with its vports and payload.
using PacketTraceFn = void (*)(bool send, unsigned src_vport, unsigned dst_vport, const unsigned char* data,
                               std::size_t len);
void set_packet_trace(PacketTraceFn fn);

// C6 crash rejoin: called on the game's own thread at the start of every receive on a P2P game
// socket (where 0), every epoll wait (where 1, sock = the epoll id) and every P2P send (where 2);
// null = off. The party layer drops a crashed member's game connection from there.
using P2pRecvHookFn = void (*)(int sock, int where);
void set_p2p_recv_hook(P2pRecvHookFn fn);
// When the P2P game socket `sock` last heard each source, as the game sees it (key: addr |
// port << 32, both network order; value: steady-clock ms). Empty for other sockets.
std::map<std::uint64_t, std::int64_t> p2p_heard(int sock);
// C6: forget the F5 replay / keepalive state of a crashed peer's source (key as p2p_heard) on
// every game socket; returns the number of entries removed.
std::size_t p2p_forget_source(std::uint64_t key);
// ms since a datagram last reached any of the game's P2P sockets (-1: none yet). F5: a guest whose
// link came back after an outage tells a surviving game session (host traffic again) from a dead one.
std::int64_t p2p_game_rx_age_ms();
// F5: while on, every game P2P socket hears each source silent for 2.5..120 s replay its last
// reliable data record every 2 s (nr_outage.h: it restarts the transport's 25 s idle deadline; the
// last small datagram for a source that never sent one) - a network outage the party link knows
// to be temporary.
void p2p_outage_keepalive(bool on);
// 0.7.6: called (off the reader thread's locks) when the game hears a peer again after a silence of
// at least 4 s in which it had sent reliable data: the peer's address (network order), port (network
// order) and the silence in seconds. The party shows "connection to X unstable".
void p2p_set_gap_hook(std::function<void(std::uint32_t, std::uint16_t, double)> fn);

}  // namespace bbnet
