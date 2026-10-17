// SPDX-License-Identifier: GPL-3.0-or-later
// Party status board (plan phase A7): a small thread-safe snapshot between the party code
// (director / runtime: producers) and the UI (overlay Party tab, toasts: consumer). Neither side
// includes the other's headers.
//
//   producers (any thread):  SetRole, SetCode, SetState(state, detail), SetMembers, SetLastEvent
//   UI (any thread):         Snapshot() copy, EventSeq() to notice a new event cheaply,
//                            RequestLeave / RequestRejoin / KickMember (queued),
//                            RequestCopyCode (done at once on the calling thread, Win32 clipboard)
//   director (main thread):  PopCommand() once a frame, until it returns false
//
// No game, no link, no ImGui: unit-tested by tests/test_party_status.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace party::status {

enum class Role : std::uint8_t { Off, Host, Guest };

enum class State : std::uint8_t {
    Off,              // party mode not running
    Starting,         // runtime coming up (UPnP, public address, code)
    Hosting,          // host: listening, the world is open to the party
    Connecting,       // guest: link handshake in progress
    WaitingForWorld,  // link up, this player's world not loaded yet (title, loading)
    RingingBell,      // a bell is up (host: Beckoning, guest: Small Resonant)
    Joined,           // guest: in the host's world / host: members are in
    Travelling,       // the party moves (lamp, area load); rejoin follows
    Reconnecting,     // link lost, retrying with backoff
    Error,            // rejected or failed: see detail
};

const char* RoleName(Role role);     // "off" / "host" / "guest"
const char* StateName(State state);  // "off", "starting", ... (stable, for logs)

struct Member {
    std::string name;
    bool connected = false;  // false while the host keeps a lost member's slot
    bool in_world = false;   // in the host's world (summoned)
    int ping_ms = -1;        // -1: unknown
    int slot = -1;           // 0 = host
    std::string area;        // map / area text ("m24_01 Cathedral Ward"), may be empty
    bool local = false;      // this player
};

struct Board {
    bool enabled = false;  // BB_PARTY set for this run
    Role role = Role::Off;
    State state = State::Off;
    std::string detail;    // with the state ("2/3, Hunter1 waiting", the reject reason ...)
    std::string code;      // the party code to share (host) / joined (guest)
    std::string lan_code;  // host: the LAN code (same network), when `code` is the Internet one
    std::string guest_code;  // guest: BBG1-... (its public UDP endpoint, for a host behind a NAT)
    std::string network;     // F2: NAT advice / the link's path ("Connected over UDP ...")
    std::vector<Member> members;
    std::string last_event;
    std::uint64_t event_seq = 0;  // +1 per SetLastEvent
    std::int64_t event_ms = 0;    // steady clock (ms) of the last event
    std::string alert;            // an important notice for a message box (the host refused us); empty: none
    std::uint64_t alert_seq = 0;  // +1 per SetAlert
    std::uint64_t version = 0;    // +1 per change of anything above
};

// --- producer side ---
void SetRole(Role role);
void SetCode(const std::string& code);
void SetGuestCode(const std::string& code);
/// F2: the network line (NAT type advice / the path the party link took).
void SetNetwork(const std::string& text);
void SetLanCode(const std::string& code);
void SetState(State state, const std::string& detail = {});
void SetMembers(const std::vector<Member>& members);
/// A one-line event for the log line and the toast ("Hunter1 joined", "Travelling to ...").
void SetLastEvent(const std::string& text);
/// An important notice shown as a message box in the middle of the screen for a while (lines
/// split by '\n'; the first is its title). Empty clears it.
void SetAlert(const std::string& text);

// --- consumer side ---
Board Snapshot();
/// Cheap (atomic): changes whenever SetLastEvent is called.
std::uint64_t EventSeq();
/// Cheap (atomic): changes whenever SetAlert is called.
std::uint64_t AlertSeq();
/// I1: friendly fire in force on this player (party_pvp.h; the host's rule on a guest), for the
/// menus. Cheap (atomic); set by the party tick.
void SetFriendlyFire(bool on);
bool FriendlyFire();
/// Statsaudit finding 5: shared lamps in force (the host's rule on a guest), for the menus.
void SetShareLamps(bool on);
bool ShareLamps();
/// BB_PARTY turns party mode on for this run (party_env.h: unset, empty, 0, off, false, no = off) (read once, or forced by tests via ResetForTest).
bool Enabled();

// --- commands: UI -> party code ---
enum class CommandType : std::uint8_t { Leave, Rejoin, Kick, AddGuestCode };
const char* CommandName(CommandType type);

struct Command {
    CommandType type = CommandType::Leave;
    std::string name;  // Kick: the member's name; AddGuestCode: the code text
};

void RequestLeave();
void RequestRejoin();
/// Host only: false (nothing queued) when this player is not the host, the name is empty or
/// not a remote member of the current roster.
bool KickMember(const std::string& name);
/// Copies the current party code to the Windows clipboard (UI thread, no queue). False when
/// there is no code or the clipboard is busy.
bool RequestCopyCode();
/// Host: copies the LAN code (same network) to the clipboard. False when there is none.
bool RequestCopyLanCode();
/// Guest: copies its guest code (BBG1-...) to the clipboard.
bool RequestCopyGuestCode();
/// Host: queues the guest code(s) on the clipboard (the host then punches towards each). False
/// when the clipboard holds no BBG1 text. `got` (optional): the text.
bool RequestAddGuestCodeFromClipboard(std::string* got = nullptr);
bool RequestAddGuestCode(const std::string& text);
/// The director, once a frame: true and *out filled while commands are queued (FIFO; at most 16
/// kept, a repeated Leave / Rejoin / Kick of the same name is coalesced).
bool PopCommand(Command* out);

/// Tests: clears everything; `enabled` replaces the BB_PARTY check.
void ResetForTest(bool enabled);

}  // namespace party::status
