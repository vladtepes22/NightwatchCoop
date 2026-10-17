// SPDX-License-Identifier: GPL-3.0-or-later
// Host session watchdog decision (see party_watchdog.h).
#include "party_watchdog.h"

#include <cstdio>
#include <cstdlib>

namespace coop {

namespace {

constexpr int kRoleHost = 3;

double EnvSeconds(const char* name, double def) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) {
        return def;
    }
    const double d = std::atof(v);
    return d >= 1.0 ? d : def;
}

} // namespace

HostWatchdogConfig HostWatchdogConfigFromEnv() {
    HostWatchdogConfig c;
    if (const char* v = std::getenv("BB_PARTY_HOST_WATCHDOG"); v && v[0] == '0') {
        c.on = false;
    }
    c.stall_s = EnvSeconds("BB_PARTY_HOST_STALL_S", c.stall_s);
    c.gone_s = EnvSeconds("BB_PARTY_HOST_GONE_S", c.gone_s);
    c.wait_s = EnvSeconds("BB_PARTY_HOST_WAIT_S", c.wait_s);
    c.hard_s = EnvSeconds("BB_PARTY_HOST_HARD_S", c.hard_s);
    c.test_stop_s = EnvSeconds("BB_PARTY_HOST_TEST_STOP_S", 0);
    return c;
}

bool MemberIncoming(const party::RosterEntry& e) {
    return e.slot != party::kHostSlot && e.connected &&
           (e.state == party::MemberState::Joining || e.state == party::MemberState::Loading ||
            e.state == party::MemberState::InHostWorld);
}

bool HostWatchdog::Update(const HostWatchdogInput& in) {
    why_.clear();
    if (!cfg_.on) {
        return false;
    }
    if (!in.in_world || in.host_travel) {
        // A loading screen or a host travel (the cooperator count reads -1 while loading, which
        // used to keep the clocks running through the load: playtest 1 H1:3694, a stop right
        // after the host's travel).
        armed_ = false;
        incoming_since_ = -1;
        return false;
    }
    if (in.cooperators < 0) {
        return false; // unknown this frame: keep the clocks as they are
    }
    if (in.session_role != kRoleHost || in.cooperators > 0 || in.members_apart > 0) {
        armed_ = false;
        incoming_since_ = -1;
        return false;
    }
    std::string sig;
    bool incoming = false, waiting = false;
    for (const party::RosterEntry& e : in.roster) {
        if (e.slot == party::kHostSlot) {
            continue;
        }
        char b[64];
        std::snprintf(b, sizeof b, "%d:%d:%d:%u;", e.slot, e.connected ? 1 : 0, int(e.state), e.map_id);
        sig += b;
        incoming = incoming || MemberIncoming(e);
        waiting = waiting || (e.connected && e.state == party::MemberState::Home);
    }
    if (!armed_) {
        armed_ = true;
        since_ = last_change_ = last_present_ = in.now_s;
        sig_ = sig;
    }
    if (sig != sig_) {
        sig_ = sig;
        last_change_ = in.now_s;
    }
    if (incoming || waiting) {
        last_present_ = in.now_s;
    }
    if (!incoming) {
        incoming_since_ = -1;
    } else if (incoming_since_ < 0) {
        incoming_since_ = in.now_s;
    }
    if (in.now_s - last_stop_ < cfg_.retry_s) {
        return false;
    }
    char b[200];
    if (cfg_.test_stop_s > 0 && !test_done_ && incoming && in.now_s - incoming_since_ >= cfg_.test_stop_s) {
        test_done_ = true;
        std::snprintf(b, sizeof b, "test stop: a member on its way in for %.0f s (BB_PARTY_HOST_TEST_STOP_S)",
                      in.now_s - incoming_since_);
    } else if (!incoming && !waiting && in.now_s - last_present_ >= cfg_.gone_s) {
        std::snprintf(b, sizeof b, "host with 0 cooperators and no member home or on its way in for %.0f s (guest gone)",
                      in.now_s - last_present_);
    } else if (incoming && in.now_s - last_change_ >= cfg_.stall_s) {
        std::snprintf(b, sizeof b, "host with 0 cooperators; the joining member made no progress for %.0f s (stuck summon)",
                      in.now_s - last_change_);
    } else if (!incoming && waiting && in.now_s - last_change_ >= cfg_.wait_s) {
        std::snprintf(b, sizeof b, "host with 0 cooperators; the waiting member was not invited for %.0f s",
                      in.now_s - last_change_);
    } else if (in.now_s - since_ >= cfg_.hard_s) {
        std::snprintf(b, sizeof b, "host with 0 cooperators for %.0f s", in.now_s - since_);
    } else {
        return false;
    }
    why_ = b;
    last_stop_ = in.now_s;
    return true;
}

} // namespace coop
