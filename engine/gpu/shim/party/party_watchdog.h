// SPDX-License-Identifier: GPL-3.0-or-later
// Host session watchdog: a host whose summon never completes ends its session and rings again.
//
// The case it is for: a guest's game dies while it joins (killed at session role client(6),
// before the host's cooperator count moved). The host's game then keeps its room (role host(3))
// with 0 cooperators and the half-loaded phantom; the restarted guest is invited and the P2P
// handshake runs, but the host's game never sends it the world data (the 267-byte message) and
// the pair sits there for minutes.
//
// The decision (pure, unit tested by party-watchdog-test): while the host is in its world (no
// loading screen), session role host(3), 0 cooperators:
//   - no connected party member at home or on its way in (roster Home / Joining / Loading /
//     InHostWorld) for `gone_s` (the guest is gone: crashed, lost, at the title)    -> stop
//   - a member is on its way in but nothing in the roster moved (state or map of any member)
//     for `stall_s` (the summon is stuck)                                           -> stop
//   - members only wait at home (not invited) and nothing moved for `wait_s`       -> stop
//     (an invite normally reaches a waiting member within ~30 s; 2026-10-10 runs saw 26 s)
//   - `hard_s` in total, whatever the roster does                                   -> stop
// Leaving role 3, a cooperator arriving or a loading screen resets it. After a stop the next
// stop comes `retry_s` later at the earliest (the leave takes a moment; role 7 resets anyway).
//
// The stop (party_director.cpp, main thread): 0x13CDDA0, the game's own "leave the session and
// drop the presentation members" helper - BeginLeaveSession 0x1ED07A0 (role -> leaving(7),
// matching stop task with reason 0xFF000023 via 0xC8ED70, leave step at +0x280) then
// 0x15BED10(network flow +0x16F8), which disables every phantom still in the presentation
// table. The idiom of OnLeaveMenu_Yes / OnMatchingCheck / OnReviveMagic; see docs/party/travel.md.
//
// Env: BB_PARTY_HOST_WATCHDOG=0 (off), BB_PARTY_HOST_STALL_S (60), BB_PARTY_HOST_GONE_S (25),
//      BB_PARTY_HOST_WAIT_S (90), BB_PARTY_HOST_HARD_S (240); test: BB_PARTY_HOST_TEST_STOP_S.
#pragma once

#include "party_link.h"

#include <string>
#include <vector>

namespace coop {

struct HostWatchdogConfig {
    bool on = true;
    double stall_s = 60.0;
    double gone_s = 25.0;
    double wait_s = 90.0;
    double hard_s = 240.0;
    double retry_s = 15.0;
    /// Test only (BB_PARTY_HOST_TEST_STOP_S): stop once, when a member has been on its way in for
    /// this long - a stop with the half-loaded phantom present, as in the stuck case.
    double test_stop_s = 0;
};

/// Reads BB_PARTY_HOST_WATCHDOG / _STALL_S / _GONE_S / _WAIT_S / _HARD_S.
HostWatchdogConfig HostWatchdogConfigFromEnv();

struct HostWatchdogInput {
    double now_s = 0;         ///< a monotonic clock in seconds
    bool in_world = false;    ///< world up, no loading screen
    int session_role = -1;    ///< SprjSessionManager +0x124
    int cooperators = -1;     ///< -1: unknown (then nothing changes)
    /// Members in another area (split travel, docs/party/split_travel.md): present, not stuck.
    int members_apart = 0;
    /// The host is mid-travel: its own load, or the B2 re-join after it (playtest 1 H1:3694: a join
    /// counted as stuck through a host travel). The clocks restart once it is over.
    bool host_travel = false;
    std::vector<party::RosterEntry> roster;
};

class HostWatchdog {
public:
    explicit HostWatchdog(HostWatchdogConfig c = {}) : cfg_(c) {}
    /// True when the session should be stopped now; `why()` says why.
    bool Update(const HostWatchdogInput& in);
    const std::string& why() const { return why_; }
    bool armed() const { return armed_; }
    /// Seconds since the stall started (0 when not armed).
    double stalled_for(double now_s) const { return armed_ ? now_s - since_ : 0.0; }
    const HostWatchdogConfig& config() const { return cfg_; }

private:
    HostWatchdogConfig cfg_;
    bool armed_ = false;
    double since_ = 0, last_change_ = 0, last_present_ = 0, last_stop_ = -1e9;
    double incoming_since_ = -1;
    bool test_done_ = false;
    std::string sig_;
    std::string why_;
};

/// A guest is on its way into the host's world (roster state Joining / Loading / InHostWorld).
bool MemberIncoming(const party::RosterEntry& e);

} // namespace coop
