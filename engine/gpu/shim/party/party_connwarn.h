// SPDX-License-Identifier: GPL-3.0-or-later
// Connection warnings (0.7.6): on-screen notices that tell a player in the game that something
// (the Windows Firewall, the router, a wrong address) keeps the party from working. Before, these
// cases were silent and players waited forever. Five causes, each shown at most once per session
// (and per guest where it names one), again only after kRepeatS when it is still true:
//   1 Firewall   host, at start: the Windows Firewall has no inbound allow rule for the game (or
//                the party port) on the active profile, or a block rule.
//   2 NoGuests   host: nobody reached the party link for kNoGuestS while the port is not open (no
//                UPnP / forward / public address) and no VPN address is in use.
//   3 GuestUdp   host: a guest is on the link but its game UDP is not heard (the "direct summon: ...
//                its UDP is not heard on the party port" failure) kGuestUdpN times in a row.
//   4 HostGone   guest: the link lost its host kHostLostN times without ever connecting.
//   5 OwnSummon  guest: its own summon is delivered but the game does not join, kOwnSummonN times.
// The decisions are pure (Tracker; time in seconds, any monotonic clock); the Windows Firewall
// query sits behind a seam (SetFirewallQuery) so tests never touch the real firewall. Shown through
// the party alert (party_status SetAlert: held under the game's full-screen menus by the overlay),
// logged with the same text. Never changes any firewall rule. Setting: bbport.ini
// party_conn_warnings (default on; System > My party: Look & notices, launcher My Party tab).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace party::connwarn {

// ---- the firewall rules (pure) ----
enum class FwState {
    Unknown,       // the query failed (no COM, no rights, Wine)
    Allowed,       // inbound UDP and TCP on the port are allowed on every active profile
    Blocked,       // a block rule matches
    Missing,       // no allow rule matches and the profile blocks inbound by default
    Off,           // the Windows Firewall is off on the active profiles (another product may manage it)
};
struct FwRule {
    bool enabled = true;
    bool inbound = true;
    bool allow = true;
    int protocol = 256;           // 6 TCP, 17 UDP, 256 any
    std::string app;              // full path; empty: any program
    std::string local_ports;      // "9307", "1000-2000", "80,443", "*" or empty: any
    int profiles = 0x7fffffff;    // bit 1 domain, 2 private, 4 public; all by default
};
struct FwProfile {
    int bit = 0;                  // 1, 2 or 4
    bool enabled = true;          // the firewall is on for this profile
    bool inbound_blocked = true;  // the default inbound action is "block"
};
struct FwReport {
    FwState state = FwState::Unknown;
    std::string detail;           // for the log
};
/// Whether `ports` ("*", "9307", "9000-9400", "80,9307") covers `port`.
bool PortListHas(const std::string& ports, int port);
/// Windows paths compare case-insensitively with / and \ alike.
bool SamePath(const std::string& a, const std::string& b);
/// The verdict for `exe` listening on UDP + TCP `port` over the active profiles. A matching block
/// rule wins over any allow (Windows' own order); a profile that allows inbound by default needs no rule.
FwReport Classify(const std::vector<FwRule>& rules, const std::vector<FwProfile>& active, const std::string& exe,
                  int port);

/// The seam: the real query asks the Windows Firewall (party_fwquery.cpp); tests install their own.
using FirewallQuery = std::function<FwReport(const std::string& exe, int port)>;
void SetFirewallQuery(FirewallQuery q);
FwReport QueryFirewall(const std::string& exe, int port);
/// The real query (party_fwquery.cpp; tests define a stub). Blocking: a few hundred ms. Own COM apartment.
FwReport QueryFirewallWindows(const std::string& exe, int port);

// ---- the texts (title on the first line, the fix after it) ----
std::string FirewallText(FwState s, int port);
std::string NoGuestsText(int port);
std::string GuestUdpText(const std::string& guest, int port);
std::string HostGoneText(const std::string& host_addr);
std::string OwnSummonText(int port);

// ---- the decisions (pure) ----
struct Thresholds {
    double no_guest_s = 180.0;     // the host waited this long with nobody on the link
    int guest_udp_n = 2;           // failures in a row per guest
    int host_lost_n = 3;           // losses without ever connecting
    int own_summon_n = 2;          // own failed summons in a row
    double repeat_s = 1800.0;      // the same cause again only after this long, and only if still true
};
class Tracker {
public:
    explicit Tracker(Thresholds t = {}) : t_(t) {}
    /// Host, at start: the firewall verdict. Text to show (empty: nothing).
    std::string FirewallVerdict(const FwReport& r, int port, double now);
    /// Host: hosting began; `port_open` the NAT result (UPnP / forward / public address worked),
    /// `vpn` a VPN address is in use (BB_PARTY_LOCAL_IP or an adapter pick).
    void HostStarted(bool port_open, bool vpn, double now);
    /// Host: a guest reached the party link (once is enough: the waiting warning is over).
    void GuestOnLink();
    /// Host, every few seconds.
    std::string HostTick(double now, int port);
    /// Host: a guest's failure report whose UDP is not heard / a guest that joined or is heard.
    std::string GuestUdpUnheard(const std::string& guest, int port, double now);
    void GuestUdpFine(const std::string& guest);
    /// Guest: the link lost its host (any reason) / connected.
    std::string HostLost(const std::string& host_addr, double now);
    void HostConnected();
    /// Guest: its summon was delivered and the game did not join / it joined.
    std::string OwnSummonFailed(int port, double now);
    void OwnSummonJoined();
    const Thresholds& thresholds() const { return t_; }

private:
    struct Shown {
        std::string key;
        double at = 0;
    };
    bool Due(const std::string& key, double now) const;
    void Mark(const std::string& key, double now);
    Thresholds t_;
    std::vector<Shown> shown_;
    bool hosting_ = false, port_open_ = true, vpn_ = false, guest_seen_ = false;
    double host_start_ = 0;
    struct Count {
        std::string who;
        int n = 0;
    };
    std::vector<Count> udp_fails_;
    int lost_n_ = 0, own_n_ = 0;
    bool ever_connected_ = false;
};

// ---- the process-wide glue (party_runtime / party_b2 call these from any thread) ----
/// The player's switch (bbport.ini party_conn_warnings); default on. Read live.
void SetEnabledProvider(bool (*enabled)());
/// The party log line writer (the runtime's plog): a raised notice is logged with the same text.
void SetLogger(void (*log)(const char* line));
/// Host start: checks the firewall for this game on a background thread and raises the notice.
void HostCheckFirewall(int port);
/// Host: hosting began with this NAT result / VPN use; HostTick (every few seconds) decides NoGuests.
void HostStarted(bool port_open, bool vpn);
void HostTick(int port);
void HostGuestOnLink();
void HostGuestUdpUnheard(const std::string& guest, int port);
void HostGuestUdpFine(const std::string& guest);
void GuestHostLost(const std::string& host_addr);
void GuestHostConnected();
void GuestOwnSummonFailed(int port);
void GuestOwnSummonJoined();
void ResetForTest();

}  // namespace party::connwarn
