// SPDX-License-Identifier: GPL-3.0-or-later
// Connection warnings (gpu/shim/party/party_connwarn.h): firewall rule classification, thresholds,
// once-per-cause, the seam. The real Windows Firewall is never queried (the stub below stands in).
// ninja -C out/gpu party-connwarn-test && out/gpu/party-connwarn-test
#include "party/party_connwarn.h"
#include "party/party_status.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace party::connwarn;

namespace party::connwarn {
int g_real_calls = 0;
FwReport QueryFirewallWindows(const std::string&, int) {
    ++g_real_calls;
    return {};
}
}  // namespace party::connwarn

static const char* kExe = "C:\\Games\\bbport\\bbport.exe";

static FwRule Allow(int proto, const std::string& ports, const std::string& app = "", int prof = 0x7fffffff) {
    FwRule r;
    r.protocol = proto;
    r.local_ports = ports;
    r.app = app;
    r.profiles = prof;
    return r;
}

static void test_ports_and_paths() {
    assert(PortListHas("", 9307) && PortListHas("*", 9307));
    assert(PortListHas("9307", 9307) && !PortListHas("9306", 9307));
    assert(PortListHas("80, 9300-9310", 9307) && !PortListHas("80,443", 9307));
    assert(!PortListHas("RPC", 9307) && !PortListHas("9400-9300", 9307));
    assert(SamePath("C:\\Games\\X.exe", "c:/games/x.EXE") && !SamePath("C:\\a.exe", "C:\\b.exe"));
}

static void test_classify() {
    const std::vector<FwProfile> pub = {{4, true, true}};
    // No rules, default block: missing.
    assert(Classify({}, pub, kExe, 9307).state == FwState::Missing);
    // The game's two Windows-prompt rules (TCP + UDP, any port, this program): allowed.
    std::vector<FwRule> r = {Allow(6, "*", kExe), Allow(17, "*", kExe)};
    assert(Classify(r, pub, kExe, 9307).state == FwState::Allowed);
    // Only TCP allowed: UDP missing.
    assert(Classify({Allow(6, "*", kExe)}, pub, kExe, 9307).state == FwState::Missing);
    // A rule for another program does not count; a port rule without a program does.
    assert(Classify({Allow(256, "*", "C:\\other.exe")}, pub, kExe, 9307).state == FwState::Missing);
    assert(Classify({Allow(6, "9307"), Allow(17, "9307")}, pub, kExe, 9307).state == FwState::Allowed);
    // Any-protocol rule (256) covers both.
    assert(Classify({Allow(256, "*", kExe)}, pub, kExe, 9307).state == FwState::Allowed);
    // Allowed on Private only while the active profile is Public: missing.
    assert(Classify({Allow(256, "*", kExe, 2)}, pub, kExe, 9307).state == FwState::Missing);
    // A block rule wins over an allow.
    FwRule b = Allow(17, "*", kExe);
    b.allow = false;
    assert(Classify({Allow(256, "*", kExe), b}, pub, kExe, 9307).state == FwState::Blocked);
    // Disabled and outbound rules never count.
    FwRule d = Allow(256, "*", kExe);
    d.enabled = false;
    FwRule o = Allow(256, "*", kExe);
    o.inbound = false;
    assert(Classify({d, o}, pub, kExe, 9307).state == FwState::Missing);
    // The profile allows inbound by default: no rule needed.
    assert(Classify({}, {{4, true, false}}, kExe, 9307).state == FwState::Allowed);
    // Firewall off on the active profile: Off (another product may manage it); nothing read: Unknown.
    assert(Classify({}, {{4, false, true}}, kExe, 9307).state == FwState::Off);
    assert(Classify({}, {}, kExe, 9307).state == FwState::Unknown);
    // Two active profiles: one missing is enough.
    assert(Classify({Allow(256, "*", kExe, 2)}, {{2, true, true}, {4, true, true}}, kExe, 9307).state == FwState::Missing);
}

static void test_firewall_notice() {
    Tracker t;
    FwReport ok;
    ok.state = FwState::Allowed;
    assert(t.FirewallVerdict(ok, 9307, 0).empty());
    FwReport un;
    assert(t.FirewallVerdict(un, 9307, 0).empty());
    FwReport off;
    off.state = FwState::Off;
    assert(t.FirewallVerdict(off, 9307, 0).empty());  // another product: the log only
    FwReport miss;
    miss.state = FwState::Missing;
    const std::string s = t.FirewallVerdict(miss, 9307, 10);
    assert(s.find("Windows Firewall may block your party") == 0 && s.find("9307") != std::string::npos);
    assert(t.FirewallVerdict(miss, 9307, 11).empty());  // once per session
    assert(t.FirewallVerdict(miss, 9307, 10 + 1799).empty());
    assert(!t.FirewallVerdict(miss, 9307, 10 + 1800).empty());  // still true after the long time
    FwReport blk;
    blk.state = FwState::Blocked;
    Tracker t2;
    assert(t2.FirewallVerdict(blk, 9400, 0).find("blocks your party") != std::string::npos);
}

static void test_no_guests() {
    Tracker t;
    assert(t.HostTick(1000, 9307).empty());  // not hosting yet
    t.HostStarted(false, false, 100);
    assert(t.HostTick(100 + 179, 9307).empty());
    const std::string s = t.HostTick(100 + 180, 9307);
    assert(s.find("Nobody has reached your party") == 0 && s.find("UPnP") != std::string::npos);
    assert(t.HostTick(100 + 200, 9307).empty());          // once
    assert(!t.HostTick(100 + 180 + 1800, 9307).empty());  // again after a long time, still true
    // The port is open: never.
    Tracker open;
    open.HostStarted(true, false, 0);
    assert(open.HostTick(5000, 9307).empty());
    // A VPN address in use: never.
    Tracker vpn;
    vpn.HostStarted(false, true, 0);
    assert(vpn.HostTick(5000, 9307).empty());
    // A guest reached the link: never (also later).
    Tracker seen;
    seen.HostStarted(false, false, 0);
    seen.GuestOnLink();
    assert(seen.HostTick(5000, 9307).empty());
}

static void test_guest_udp() {
    Tracker t;
    assert(t.GuestUdpUnheard("Ann", 9307, 0).empty());  // the first failure: not yet
    const std::string s = t.GuestUdpUnheard("Ann", 9307, 5);
    assert(s.find("Ann is in the party") == 0 && s.find("BB_PARTY_LOCAL_IP") != std::string::npos);
    assert(t.GuestUdpUnheard("Ann", 9307, 10).empty() && t.GuestUdpUnheard("Ann", 9307, 15).empty());  // once
    // Another guest has its own count and notice.
    assert(t.GuestUdpUnheard("Bob", 9307, 20).empty());
    assert(t.GuestUdpUnheard("Bob", 9307, 21).find("Bob is in the party") == 0);
    // A heard/joined guest in between resets the count: failures must be in a row.
    Tracker u;
    assert(u.GuestUdpUnheard("Cy", 9307, 0).empty());
    u.GuestUdpFine("Cy");
    assert(u.GuestUdpUnheard("Cy", 9307, 1).empty());
    assert(!u.GuestUdpUnheard("Cy", 9307, 2).empty());
    // After the long time, two more failures show it again.
    assert(u.GuestUdpUnheard("Cy", 9307, 3 + 1800).empty());
    assert(!u.GuestUdpUnheard("Cy", 9307, 4 + 1800).empty());
}

static void test_guest_side() {
    Tracker t;
    assert(t.HostLost("203.0.113.5:9307", 0).empty());
    assert(t.HostLost("203.0.113.5:9307", 11).empty());
    const std::string s = t.HostLost("203.0.113.5:9307", 22);
    assert(s.find("Cannot reach the host at 203.0.113.5:9307") == 0 && s.find("same VPN network") != std::string::npos);
    for (int i = 0; i < 6; ++i) assert(t.HostLost("203.0.113.5:9307", 30 + i).empty());  // once
    // Having connected once: a later outage is not this warning.
    Tracker c;
    c.HostLost("a", 0);
    c.HostLost("a", 1);
    c.HostConnected();
    for (int i = 0; i < 5; ++i) assert(c.HostLost("a", 10 + i).empty());
    // Own summon.
    Tracker o;
    assert(o.OwnSummonFailed(9307, 0).empty());
    assert(o.OwnSummonFailed(9307, 5).find("The host cannot hear your game") == 0);
    assert(o.OwnSummonFailed(9307, 6).empty() && o.OwnSummonFailed(9307, 7).empty());
    Tracker p;
    assert(p.OwnSummonFailed(9307, 0).empty());
    p.OwnSummonJoined();
    assert(p.OwnSummonFailed(9307, 1).empty());
}

static bool g_on = true;
static int g_logged = 0;
static std::string g_last_log;
static void Logger(const char* l) {
    ++g_logged;
    g_last_log = l;
}

static void test_glue_and_seam() {
    ResetForTest();
    party::status::ResetForTest(true);
    SetLogger(&Logger);
    SetEnabledProvider(+[] { return g_on; });
    int calls = 0;
    SetFirewallQuery([&](const std::string&, int port) {
        ++calls;
        FwReport r;
        r.state = FwState::Missing;
        r.detail = "stub port " + std::to_string(port);
        return r;
    });
    HostCheckFirewall(9307);
    for (int i = 0; i < 300 && party::status::AlertSeq() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(calls == 1 && g_real_calls == 0);
    assert(party::status::AlertSeq() == 1);
    assert(party::status::Snapshot().alert.find("Windows Firewall may block your party") == 0);
    assert(g_logged >= 2 && g_last_log.find("connection warning: Windows Firewall may block") == 0);
    // Switched off: no query, no notice, no log of a notice.
    g_on = false;
    const int logged = g_logged;
    HostCheckFirewall(9307);
    HostGuestUdpUnheard("Ann", 9307);
    HostGuestUdpUnheard("Ann", 9307);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(calls == 1 && party::status::AlertSeq() == 1 && g_logged == logged);
    // On again: the guest notice goes through (two failures in a row).
    g_on = true;
    HostGuestUdpUnheard("Ann", 9307);
    HostGuestUdpUnheard("Ann", 9307);
    assert(party::status::AlertSeq() == 2 && party::status::Snapshot().alert.find("Ann is in the party") == 0);
    ResetForTest();
}

int main() {
    test_ports_and_paths();
    test_classify();
    test_firewall_notice();
    test_no_guests();
    test_guest_udp();
    test_guest_side();
    test_glue_and_seam();
    std::puts("party-connwarn-test: ok");
    return 0;
}
