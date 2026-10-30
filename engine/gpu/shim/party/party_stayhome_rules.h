// SPDX-License-Identifier: GPL-3.0-or-later
// "Stay in my world" (CL-adeb50): a guest pauses the automatic joining into the host's world and stays in
// its own (a Chalice ritual, shopping) without leaving the party. Pure logic, header only (unit test
// tests/test_party_stayhome.cpp, ninja -C out/gpu party-stayhome-test); the game side is party_stayhome.cpp
// and the call sites named below.
//
// One switch, party_stay (bbport.ini, env BB_PARTY_STAY, launcher My Party > Stay in my world, System >
// Party > Stay in my world on a guest). Per player, live, off by default (off = joining as before). What
// BB_PARTY_AUTO=0 does for the whole process (no automatic bells, a test aid) this does for one guest, live.
//
// While it is on, on the guest:
//   * the director rings no Small Resonant Bell and runs no gather to the host's lamp (a bell already
//     rung is withdrawn); turning it off rings at the next opportunity (the normal join path);
//   * a title join (the restart / Continue flow) is skipped: plain Continue into its own world;
//   * the host's "Gather the party" is refused with the reason "staying in own world" (the host is told).
// The host learns it from EVENT stay_home {"on":0|1} (the guest sends it on every change and every
// kReportEveryS s; a report older than kExpireS s no longer counts, so a lost "off" cannot keep a guest
// parked). On the host a staying guest
//   * is not rung for (the Beckoning Bell and the direct-summon hold ignore it, like an unreachable one);
//   * does not vote on a party travel (the vote never waits for it);
//   * shows "Stays home" in the Party members rows instead of "In own world".
// Party travel itself already leaves a guest in its own world alone (GuestFollowsHostTravel).
#pragma once

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace coop::stayhome {

constexpr const char* kEventName = "stay_home";
constexpr double kReportEveryS = 5.0;  // a guest repeats its state this often while the switch is on
constexpr double kExpireS = 15.0;      // the host forgets a "stay" report older than this

inline std::string Encode(bool on) {
    return on ? "{\"on\":1}" : "{\"on\":0}";
}

/// false: not a stay_home body.
inline bool Decode(const std::string& body, bool* on) {
    const char* p = std::strstr(body.c_str(), "\"on\":");
    int v = 0;
    if (!p || std::sscanf(p, "\"on\":%d", &v) != 1 || (v != 0 && v != 1)) return false;
    *on = v == 1;
    return true;
}

/// Whether a guest sends its state now: on every change, and while on every kReportEveryS s.
inline bool ShouldReport(bool on, bool last_sent_on, bool ever_sent, double since_last_s) {
    if (!ever_sent) return on;  // nothing to say before the first "on" (the host's default is "not staying")
    return on != last_sent_on || (on && since_last_s >= kReportEveryS);
}

/// The host's record of who stays.
class Book {
public:
    void Set(int slot, bool on, double now) {
        if (on) at_[slot] = now;
        else at_.erase(slot);
    }
    bool Stays(int slot, double now) const {
        const auto it = at_.find(slot);
        return it != at_.end() && now - it->second <= kExpireS;
    }
    void Forget(int slot) { at_.erase(slot); }
    void Clear() { at_.clear(); }
    int Count(double now) const {
        int n = 0;
        for (const auto& kv : at_) n += now - kv.second <= kExpireS ? 1 : 0;
        return n;
    }

private:
    std::map<int, double> at_;
};

}  // namespace coop::stayhome
