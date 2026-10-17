// SPDX-License-Identifier: GPL-3.0-or-later
// PartyLink: the party's TCP control channel. The host's game listens on the party port
// (BB_PARTY_PORT, default 9307); guests connect to it directly (no server anywhere).
//
// Wire: every frame is [u32 len LE][u8 type][payload], len = 1 + payload size.
//   guest -> HELLO     {magic "BBPL", u16 version, str name, sha256 eboot[32], mods hash[32], token[16],
//                       gameplay patches hash[32], str patch names, str mod names,  (newline-separated)
//                       str party rules,  (max players + 4-player rule set, party_fourp.h)
//                       str build id, sha256 params[32], party hooks hash[32], str hook names}
//                       (A9 build / patch-set fingerprint: docs/party/version_check.md; each
//                       trailing field is optional on the wire: absent = an older build)
//   host  -> CHALLENGE {nonce[24]}                         (or REJECT on version / hash / name; a
//                       Mismatch REJECT names what differs: identity_mismatch)
//   guest -> AUTH      {nonce[24], proof[32]}              proof = keyed BLAKE2b (party_crypto.h)
//   host  -> WELCOME   {slot, max players, resumed, host clock ms, observed ip[4] + port, token,
//                       roster} -- the first ENCRYPTED frame (proves the host knows the key too)
//         or REJECT    {u8 code, str reason}               plaintext, then the host closes
// From WELCOME on, every frame is [len][0xE0][mac 16][XChaCha20-Poly1305(type || payload)] with a
// per-direction key and an implicit per-direction counter nonce (party_crypto.h).
// Runtime: PING/PONG (1 Hz both ways, RTT), ROSTER, RPC_REQ/RPC_RESP, EVENT/EVENT_ACK (cursor;
// unacked events are replayed after a reconnect and de-duplicated by cursor), PARTY_CMD,
// PROGRESS, VITALS (party HUD: HP, map, position; ~4 Hz, see MemberVitals), BYE. No frame for lost_timeout (10 s) = connection lost; the host keeps a lost
// member's slot (and its event queue) for slot_keep (130 s); the guest reconnects with
// exponential backoff 1, 2, 4, 8 s (then every 8 s, each wait +-20 % so guests that lost the
// host together do not retry in lockstep) and gets the same slot back (resume token). A restarted
// host restores its member table (slot, name, token: kept_members / restore_members, from the
// party runtime's crash marker), so its guests get their slots back after a host crash too.
//
// Never blocks its caller: every public call takes the link's lock only for bookkeeping and
// non-blocking socket writes; the IO thread holds it only while processing (bounded: frames are
// flushed in batches, host names are resolved outside it). Waits: rpc_call (timeout_ms),
// wait_state (timeout_ms), stop (joins the IO and callback threads: the shutdown path only).
//
// Threads: one IO thread (WSAPoll) per PartyLink, plus one callback thread: every callback runs
// on the callback thread, one at a time, never while PartyLink's lock is held, so callbacks may
// call back into PartyLink (send_*, rpc_call_async, roster ...). Blocking rpc_call() from a
// callback works too (replies are matched on the IO thread) but stalls other callbacks meanwhile.
// Never call stop() / the destructor from a callback.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace party {

constexpr std::uint16_t kLinkProtocolVersion = 2;  // 2: HELLO carries the patch / mod sets
constexpr int kBroadcast = -1;  // send_* target: every member (host side)
// Players a party may hold, the host included (BB_PARTY_MAX, default 3, is clamped to
// 2..kMaxPartyPlayers): host + 4 guests (user, 2026-10-12: 5-player tests; 4 is the verified size,
// ERSC allows 6). Every party-side table (link slots, VITALS, HUD rows, the Party members page)
// follows this constant. 5 = every engine table full (party_fourp H6; four_players.md); 6 needs
// the engine's fixed tables relocated.
constexpr int kMaxPartyPlayers = 5;

// Peer input limits (bbport security pass). Every byte a peer sends is untrusted: a frame is
// refused (connection dropped) past these, and the host meters each member.
constexpr std::uint32_t kMaxFrame = 16u << 20;          // any frame after WELCOME
constexpr std::uint32_t kMaxHandshakeFrame = 64u << 10;  // HELLO / CHALLENGE / AUTH / REJECT
constexpr std::size_t kMaxPendingConns = 16;   // host: connections still in the handshake
constexpr std::size_t kMaxPendingPerIp = 4;    // ... from one address
constexpr std::size_t kMaxOutBuffer = 64u << 20;  // queued for a peer that does not read: dropped
constexpr std::size_t kReadBudget = 256u << 10;   // bytes read from one connection per IO pass
constexpr std::size_t kFramesPerPass = 256;       // frames handled per connection per IO pass
constexpr int kMaxCallbackBacklog = 64;  // a connection's frames waiting for the callback thread:
                                         // past this its socket is not read (backpressure)
constexpr std::size_t kMaxUnackedEvents = 16384;  // per stream; the oldest go past this
constexpr std::size_t kMaxEventName = 64;
constexpr std::size_t kMaxRosterEntries = 8;
// Host: what one member may send after WELCOME (frames / bytes per second, burst). A member past
// it has its frames dropped (an RPC gets an error reply); kFloodDisconnect drops in a row and
// the connection is closed (the slot is kept: a misbehaving client, not a kick).
constexpr double kMemberFrameRate = 200, kMemberFrameBurst = 2000;
constexpr double kMemberByteRate = 4u << 20, kMemberByteBurst = 48u << 20;
constexpr std::uint32_t kFloodDisconnect = 20000;

// Host: online password guessing (bbport security pass, F3). The party key is Argon2-derived, so
// an offline guess is slow; online, each wrong AUTH proof from an address counts. kAuthFailFree
// wrong proofs within kAuthFailWindowMs are free (a typo, an old code); the next one locks the
// address out for kAuthLockoutMs, doubling with every further lockout up to kAuthLockoutMaxMs
// (reset after kAuthForgetMs without a failure). A locked-out address gets REJECT Auth ("too many
// wrong passwords ...") at HELLO, before any proof is checked, the right password included. At
// most kAuthTrackedAddrs addresses are tracked (the stalest is forgotten first).
// Global budget (a guesser spread over many addresses): more than kAuthGlobalFree wrong proofs from
// all addresses together within kAuthFailWindowMs lock the HELLO of every address that has not
// passed AUTH before (kAuthTrustedAddrs remembered) for kAuthGlobalLockoutMs, doubling up to
// kAuthLockoutMaxMs (forgotten after kAuthForgetMs without a failure). Members who were in already
// (a crash, a re-join) still get in; strangers are told the host refuses new joins for a while.
constexpr int kAuthFailFree = 5;
constexpr int kAuthGlobalFree = 30;
constexpr std::int64_t kAuthGlobalLockoutMs = 60'000;
constexpr std::size_t kAuthTrustedAddrs = 64;
constexpr std::int64_t kAuthFailWindowMs = 60'000;
constexpr std::int64_t kAuthLockoutMs = 30'000;
constexpr std::int64_t kAuthLockoutMaxMs = 15 * 60'000;
constexpr std::int64_t kAuthForgetMs = 60 * 60'000;
constexpr std::size_t kAuthTrackedAddrs = 4096;

class AuthThrottle {
public:
    /// The address may not try now; *remaining_ms (optional): how long it still waits;
    /// *global (optional): true when only the global budget holds it (not its own failures).
    bool Locked(std::uint32_t ip, std::int64_t now_ms, std::int64_t* remaining_ms = nullptr,
                bool* global = nullptr) const;
    /// A wrong proof from `ip`; true when it started a lockout (of that address or the global one).
    bool Fail(std::uint32_t ip, std::int64_t now_ms);
    /// A good proof: the address's free failures start over (its lockout history stays) and it
    /// passes a global lockout from now on.
    void Success(std::uint32_t ip);
    std::size_t Tracked() const { return addrs_.size(); }
    std::uint64_t lockouts() const { return lockouts_; }
    std::uint64_t global_lockouts() const { return global_lockouts_; }
    bool GlobalLocked(std::int64_t now_ms) const { return now_ms < global_until_; }

private:
    struct Addr {
        int fails = 0;              // in the current window
        std::int64_t window = 0;    // window start
        int lockouts = 0;
        std::int64_t until = 0;     // locked out until
        std::int64_t last = 0;      // last failure
    };
    std::map<std::uint32_t, Addr> addrs_;
    std::uint64_t lockouts_ = 0;
    // the global budget
    int global_fails_ = 0, global_level_ = 0;
    std::int64_t global_window_ = 0, global_until_ = 0, global_last_ = 0;
    std::uint64_t global_lockouts_ = 0;
    std::vector<std::uint32_t> trusted_;  // addresses that passed AUTH (oldest first)
};
constexpr int kHostSlot = 0;

// Prologue: in its own world, still playing the campaign start (party_start.h); not summoned yet.
enum class MemberState : std::uint8_t { Title = 0, Home = 1, Joining = 2, InHostWorld = 3, Dead = 4, Loading = 5, Prologue = 6 };
const char* member_state_name(MemberState s);

struct RosterEntry {
    int slot = 0;
    std::string name;
    MemberState state = MemberState::Title;
    bool connected = false;  // false while the host keeps a lost member's slot
    std::uint32_t map_id = 0;
    std::uint32_t ping_ms = 0;  // as measured by the host
    std::string lost_why;       // host, on_member_left: why the connection went ("connection reset"...)
};

// Party HUD (party_hud.h): one member's health and whereabouts, as its own game reads them.
// Wire (VITALS, fire-and-forget, ~4 Hz): guest -> host its own entry; host -> every guest all
// fresh entries (the host's and every member's). Peers that do not know VITALS ignore it.
constexpr int kMaxVitals = kMaxPartyPlayers;  // party slots 0..4
constexpr std::uint32_t kMaxVitalsHp = 99999;  // HP / max HP above this: refused (clamped)
constexpr std::uint32_t kMaxVitalsStamina = 9999;
constexpr float kMaxVitalsCoord = 100000.0f;  // |x|, |y|, |z| past this: no position
constexpr int kVitalsSendMs = 250;            // at most this often per sender (4 Hz)
constexpr int kVitalsKeepaliveMs = 1000;      // unchanged vitals are re-sent this often
constexpr int kVitalsStaleMs = 3000;          // older entries are not relayed / count as unknown
enum : std::uint8_t {
    kVitalsValid = 1,   // hp / max_hp / stamina read from a live player
    kVitalsAlive = 2,   // hp > 0
    kVitalsHasPos = 4,  // pos / yaw valid
    kVitalsKnownFlags = 7,
};
struct MemberVitals {
    int slot = -1;
    std::uint8_t flags = 0;
    std::uint32_t hp = 0, max_hp = 0;
    std::uint16_t stamina = 0, max_stamina = 0;
    std::uint32_t map_id = 0;  // 0xAABBCCDD; 0 = unknown
    float pos[3] = {0, 0, 0};
    float yaw = 0;             // facing (radians; forward = (sin, 0, cos))
    std::uint32_t age_ms = 0;  // vitals(): how long ago it arrived (0 for our own)
    // Overhead display extras (party_balance.h; a trailer older peers ignore): 0 = unknown.
    std::uint16_t level = 0;   // Blood Echoes level (PlayerGameData +0x90)
    std::uint16_t deaths = 0;  // deaths since the game started
    std::uint8_t rot = 0;      // death debuff stacks (0..5)
};
// Bounds a peer's entry: unknown flags cleared, slot outside 0..kMaxVitals-1 -> false (dropped), HP /
// stamina clamped (hp <= max_hp), a non-finite or absurd position drops kVitalsHasPos, the yaw
// wrapped to [-pi, pi]. Alive needs Valid and hp > 0.
bool sanitize_vitals(MemberVitals& v);

enum class RejectCode : std::uint8_t {
    None = 0,
    Version = 1,   // different PartyLink protocol
    Mismatch = 2,  // different game version / gameplay patches / gameplay mods (the reason says which)
    Auth = 3,      // wrong password or party code
    Full = 4,
    Name = 5,      // invalid or duplicate name
    Protocol = 6,
    Shutdown = 7,  // host ended the party (BYE)
    Kicked = 8,
};

enum class LinkState : std::uint8_t {
    Idle,          // not started
    Hosting,       // host: listening
    Connecting,    // guest: TCP connect / handshake in progress
    Connected,     // guest: WELCOME received
    Reconnecting,  // guest: lost, waiting for the next attempt
    Rejected,      // guest: REJECT or BYE received; no more attempts (reject_code / reject_reason)
    Stopped,
};
const char* link_state_name(LinkState s);

struct LinkConfig {
    std::string name = "Hunter";  // <= 16 of [A-Za-z0-9_-]
    // The version check: players must match in all three hashes. The names only explain a
    // mismatch (the host lists what each side has); party_runtime fills them from
    // out/party_patch_hash.txt (scripts/patches.py) and out/party_mods.txt (scripts/mods.py).
    std::array<std::uint8_t, 32> eboot_sha256{};
    std::array<std::uint8_t, 32> patches_hash{};  // gameplay patches (zeros: none / not compared)
    std::array<std::uint8_t, 32> mods_hash{};     // gameplay mods (zeros: none)
    std::vector<std::string> patch_names, mod_names;
    // The party's game rules: "max N players, 4p:..." (party_runtime from BB_PARTY_MAX and
    // party_fourp.h FourpRulesTag). Part of the version check: must equal the host's.
    std::string rules;
    // A9 build / patch-set fingerprint (party_runtime): the bbport build ("0.5 (git describe)",
    // bbport_build_id.cmake), the game's param set (sha256 of dvdroot_ps4/param/gameparam/
    // gameparam.parambnd.dcx; zeros: not read) and the party's installed game hooks (hook_set_hash
    // over coop_hooks' install names without diagnostics; the names explain a mismatch).
    std::string build_id;
    std::array<std::uint8_t, 32> params_sha256{};
    std::array<std::uint8_t, 32> hooks_hash{};
    std::vector<std::string> hook_names;
    bool check_build = true;            // host: compare build_id (BB_PARTY_CHECK_BUILD=0: not)
    std::string password;               // BB_PARTY_PASSWORD (optional)
    std::array<std::uint8_t, 8> secret{};  // the party code's secret (zeros for plain host:port)
    std::uint16_t port = 9307;          // host: listen port (0 = ephemeral, see bound_port())
    std::string bind_addr = "0.0.0.0";  // host
    int max_players = 3;                // 2..kMaxPartyPlayers (5) including the host (BB_PARTY_MAX)
    // Timing (tests shrink these).
    int ping_interval_ms = 1000;
    int lost_timeout_ms = 10000;
    int slot_keep_ms = 130000;  // F5: a silent member's slot outlives a 2 min outage (BB_PARTY_SLOT_KEEP_MS)
    int backoff_initial_ms = 1000;
    int backoff_max_ms = 2000;     // a crashed host is back within ~2 s of its restart (rejoin timing, docs/party/join_timing.md)
    int backoff_jitter_pct = 20;   // each wait randomized +-20 %
    int connect_timeout_ms = 5000;
    int roster_refresh_ms = 5000;  // host re-broadcasts the roster (fresh pings) this often
    // The UDP path (party_rudp.h, docs/party/network_robustness.md 4): a host also accepts the link
    // over a reliable UDP stream on its port (hole punching: no port forward needed when both NATs
    // keep a mapping per endpoint); a guest whose TCP connect gets no answer within
    // tcp_fallback_ms (or is refused) tries UDP. Without an external transport
    // (set_udp_transport) the link uses its own UDP socket on the same port number.
    bool udp_link = true;
    int tcp_fallback_ms = 1500;
    // Guest: the DHT rendezvous (party_rdv.h, opt-in) announces us, so a host without an open port
    // can find us; only changes the failure hint (host_no_answer_hint).
    bool auto_punch = false;
    // Host: lock out an address after repeated wrong passwords (AuthThrottle). Only the fuzzer
    // turns it off (its random handshakes come from 127.0.0.1 like its real members).
    bool auth_throttle = true;
};

// What differs between the host's and a guest's game (bbport build, eboot.bin, game params,
// gameplay patches, gameplay mods, party hooks, party rules), one clause each with what to do,
// with the names only one side has; empty when they match. The host sends it as the Mismatch
// REJECT reason (the guest's party UI shows it).
std::string identity_mismatch(const LinkConfig& host, const LinkConfig& guest);
// The Auth REJECT reason the host sends (the guest's party UI shows it): with a password the
// usual cause is a wrong one; without, a party code from another hosting session (or a guest
// that sent a password the host does not use).
std::string auth_reject_reason(bool host_has_password);
// Guest, the host does not answer (no TCP / UDP answer): within kHostBackWaitS of the host ending
// the party or its link dropping (`since_host_gone_s` >= 0) the host is most likely restarting,
// so the text says to wait for it, not the NAT advice (playtest 1: "behind a NAT: it needs your
// guest code" while the host was only relaunching). `since_host_gone_s` < 0: never connected.
constexpr double kHostBackWaitS = 60.0;
// `auto_punch`: the guest's DHT rendezvous is on (else: send the guest code or turn it on on both PCs).
std::string host_no_answer_hint(double since_host_gone_s, bool auto_punch = false);
// A9: is an installed hook's name a diagnostic (log / probe / trace / test), not a gameplay patch?
bool diagnostic_hook_name(const std::string& name);
// A9: the party hook set's hash: BLAKE2b-256 over the sorted, de-duplicated names (diagnostics
// left out by the caller); zeros for an empty set.
std::array<std::uint8_t, 32> hook_set_hash(std::vector<std::string> names);
// Reads an identity file (out/party_patch_hash.txt, out/party_mods.txt): "hash <64 hex>" and
// "<item_key> <name>" lines; '#' lines are comments. False when it has no valid hash line.
bool parse_identity_file(const std::string& text, const std::string& item_key, std::array<std::uint8_t, 32>* hash,
                         std::vector<std::string>* items);

// BB_PARTY_PORT, BB_PARTY_MAX (clamped 2..kMaxPartyPlayers), BB_PARTY_PASSWORD over `cfg`.
void apply_link_env(LinkConfig& cfg);
bool valid_member_name(const std::string& name);

// Host: a member's slot as kept across a host restart.
struct KeptMember {
    int slot = 0;
    std::string name;
    std::array<std::uint8_t, 16> token{};
};

struct LinkCallbacks {
    // Host: a member completed the handshake (rejoined = it got its kept slot back).
    std::function<void(const RosterEntry& member, bool rejoined)> on_member_joined;
    // Host: slot_kept = connection lost, slot held for slot_keep_ms; false = slot released (BYE,
    // expiry, kick).
    std::function<void(const RosterEntry& member, bool slot_kept)> on_member_left;
    // Host: a guest's RPC_REQ; the returned JSON is the reply.
    std::function<std::string(int slot, const std::string& kind, const std::string& json)> on_rpc;
    // Both: an EVENT (host side: from guest `from_slot`; guest side: from_slot = 0).
    std::function<void(int from_slot, std::uint64_t cursor, const std::string& name, const std::string& json)> on_event;
    std::function<void(int from_slot, const std::string& cmd, const std::string& json)> on_party_cmd;
    std::function<void(int from_slot, const std::vector<std::uint8_t>& blob)> on_progress;
    std::function<void(const std::vector<RosterEntry>& roster)> on_roster;
    std::function<void(LinkState state, const std::string& detail)> on_state;
    std::function<void(const std::string& line)> on_log;
    // Party logging (party_log.h): an EVENT this side sends (send_event / post_event), called on the
    // caller's thread before the link's lock is taken. Optional.
    std::function<void(int to_slot, const std::string& name, const std::string& json)> on_event_sent;
};

class PartyLink {
public:
    PartyLink(LinkConfig cfg, LinkCallbacks cb);
    ~PartyLink();  // stop()
    PartyLink(const PartyLink&) = delete;
    PartyLink& operator=(const PartyLink&) = delete;

    // Host: derives the key, listens, starts the threads. False + error when the port is taken.
    bool start_host(std::string* error = nullptr);
    // Guest: derives the key and starts connecting in the background (retries with backoff until
    // WELCOME or REJECT). `host` is an IPv4 literal or hostname.
    bool start_guest(const std::string& host, std::uint16_t port, std::string* error = nullptr);
    // Graceful: BYE to everyone (host: members get RejectCode::Shutdown), then closes.
    // graceful = false drops the sockets without BYE (tests: simulates a crash).
    void stop(bool graceful = true);

    bool is_host() const;
    LinkState state() const;
    bool wait_state(LinkState s, int timeout_ms) const;  // true once state() == s
    bool wait_connected(int timeout_ms) const { return wait_state(LinkState::Connected, timeout_ms); }
    int local_slot() const;  // host 0; guest its WELCOME slot (-1 before)
    std::uint16_t bound_port() const;
    int max_players() const;
    std::vector<RosterEntry> roster() const;
    std::int64_t host_clock_ms() const;  // host: its own; guest: estimated from PONGs
    std::uint32_t rtt_ms() const;        // guest: last RTT to the host
    std::uint64_t udp_resends() const;   // RUDP segments resent on this link's UDP connections (loss)
    std::string observed_address() const;  // guest: "ip:port" the host saw us at
    // Host: the IPv4 (network byte order) member `slot` is connected from; 0 when not connected.
    std::uint32_t member_ip(int slot) const;
    RejectCode reject_code() const;
    std::string reject_reason() const;

    // Guest -> host RPC. Blocking: false on timeout / disconnect / no handler (reply then holds
    // the reason). Async: `done` runs on the callback thread exactly once; returns the id (0 when
    // it failed immediately; `done` still runs).
    bool rpc_call(const std::string& kind, const std::string& json, std::string* reply, int timeout_ms);
    std::uint32_t rpc_call_async(const std::string& kind, const std::string& json,
                                 std::function<void(bool ok, const std::string& reply)> done, int timeout_ms);

    // Reliable ordered events. Host: to `slot` or kBroadcast (queued for a lost member and
    // replayed when it resumes); guest: slot ignored, to the host. Returns the cursor (the last
    // one for a broadcast; 0 when nobody is addressed).
    std::uint64_t send_event(int slot, const std::string& name, const std::string& json);
    // send_event for the game's main thread: never waits for the IO thread's lock. When the lock
    // is busy (or earlier posts still wait) the event is queued and the IO thread sends it at the
    // start of its next pass (<= ~10 ms), in post order. Same delivery as send_event.
    void post_event(int slot, const std::string& name, const std::string& json);
    // Fire-and-forget (dropped while disconnected). Host: slot or kBroadcast; guest: to the host.
    bool send_party_cmd(int slot, const std::string& cmd, const std::string& json);
    bool send_progress(int slot, const std::vector<std::uint8_t>& blob);
    // This player's roster entry (host: broadcast to all; guest: reported to the host).
    void set_local_state(MemberState state, std::uint32_t map_id);
    // This player's vitals (party HUD), from the game's main thread: never waits for the IO
    // thread (a short copy under the snapshot lock). The IO thread sends them at most every
    // kVitalsSendMs when they changed, else every kVitalsKeepaliveMs. The slot is filled in.
    void set_local_vitals(const MemberVitals& v);
    // Everyone's latest vitals (ours included, others' up to kVitalsStaleMs old), into a caller's
    // array (no allocation); returns how many were written (<= max, <= kMaxVitals).
    int vitals(MemberVitals* out, int max) const;
    // Host: every member's slot, name and resume token (for the crash marker).
    std::vector<KeptMember> kept_members() const;
    // Host, before start_host: members of the previous (crashed) run, as lost members whose slot
    // is kept for slot_keep_ms from start_host. They come back by token or name; their WELCOME
    // says "not resumed" (the event streams start afresh: this process is new).
    void restore_members(const std::vector<KeptMember>& members);
    // Guest: the last WELCOME resumed our previous session (false: the host restarted or released
    // our slot - whatever the host knew about us is gone).
    bool last_welcome_resumed() const;

    // Host: drop a member and free its slot (BYE Kicked, reason default "kicked by the host").
    // The name is refused (REJECT Kicked) for the rest of this link's life: no auto-rejoin.
    bool kick(int slot, const std::string& reason);
    // Host: kick() by member name; false when no remote member has it.
    bool kick_name(const std::string& name, const std::string& reason = {});
    bool is_banned(const std::string& name) const;
    // Guest: while Reconnecting, try again now (backoff reset). False in any other state.
    bool reconnect_now();

    // The IO thread's longest single hold of the link's lock (microseconds, wall clock) since
    // the last reset: what a caller that needs the lock could have waited. `cycles`: the
    // longest hold in the IO thread's own CPU cycles (Windows; 0 elsewhere), i.e. without the
    // time it was preempted holding it.
    std::int64_t debug_lock_hold_max_us(bool reset = false, std::uint64_t* cycles = nullptr);
    // Tests: stop all IO (no reads, writes or pings) for `ms`, simulating a frozen network.
    void debug_freeze(int ms);
    // ---- UDP path (party_rudp.h) ----
    // An external datagram transport (the game's party UDP port, shared with the game's traffic):
    // `send` puts a datagram on the wire (addr / port network order; any thread, called from the
    // link's IO thread); datagrams for the link arrive through on_udp_datagram (any thread). Call
    // before start_host / start_guest.
    using UdpSend = std::function<bool(std::uint32_t addr, std::uint16_t port, const std::uint8_t* d, std::size_t n)>;
    void set_udp_transport(UdpSend send);
    void on_udp_datagram(std::uint32_t addr, std::uint16_t port, const std::uint8_t* d, std::size_t n);
    // Host: open our NAT mapping towards a guest's public endpoint (its guest code) for 60 s.
    void punch_guest(std::uint32_t addr, std::uint16_t port);
    // The link's own UDP socket's port (host order; 0 without one) and whether the guest's
    // connection to the host runs over UDP.
    std::uint16_t udp_local_port() const;
    bool connected_over_udp() const;
    // Tests: abort the current connection(s) without BYE (guest then reconnects).
    void debug_drop_connections();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace party
