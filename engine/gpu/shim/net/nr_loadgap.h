#pragma once
// 0.7.7: receive gaps that sit in a loading screen (ours or the peer's) are not outages.
//
// A loading screen stops the game's network traffic for several seconds (the long party travel:
// 10 to 20 s). The 4 s silence detector then fired on the first datagram after the load: the
// "connection unstable" notice, and for guest-guest links PeerPaths::note_gap moved the pair to the
// host relay after two such loads. A gap counts only for the part of it that lies outside loads: a
// link that stays silent after the load ended is a real outage again, from the load's end on.
//
// Pure bookkeeping (no sockets, no logging, no locks - the owner locks) so tests drive it with a
// fake clock. Scope key 0 is this game's own load, 1 the session host's, any other key a peer (the net layer's
// source_key).
#include <chrono>
#include <cstdint>
#include <map>
#include <set>

namespace bbnet::nr {

class LoadGapGate {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::uint64_t kOwn = 0;   // this game's own load
    static constexpr std::uint64_t kHost = 1;  // the session host's load, seen from a guest: the whole
                                               // session (guest-guest links too) goes quiet in it

    void set_loading(std::uint64_t scope, bool loading, Clock::time_point now) {
        Scope& s = scopes_[scope];
        if (s.loading && !loading) s.last_end = now;
        if (!s.loading && loading) s.last_start = now;
        s.loading = loading;
    }
    // CL-9142b8: a member that left the party (Party > Leave: its own home load, the roster drops it) is
    // expected silence on every link to it, until the roster has it again (a rejoin).
    void set_gone(std::uint64_t peer, bool gone) {
        if (gone) gone_.insert(peer); else gone_.erase(peer);
    }
    bool gone(std::uint64_t peer) const { return gone_.count(peer) != 0; }
    bool loading(std::uint64_t scope) const {
        auto it = scopes_.find(scope);
        return it != scopes_.end() && it->second.loading;
    }

    // The silence of the link to `peer` that counts: it was last heard at `prev` and is heard at
    // `now`. 0 while a load of either side is up; after a load that ended inside the gap, the time
    // since that load ended; with no load in the gap, the whole gap.
    double effective_silent_s(std::uint64_t peer, Clock::time_point prev, Clock::time_point now) const {
        if (gone_.count(peer)) return 0;
        Clock::time_point from = prev;
        for (std::uint64_t k : {kOwn, kHost, peer}) {
            auto it = scopes_.find(k);
            if (it == scopes_.end()) continue;
            const Scope& s = it->second;
            if (s.loading) return 0;
            if (s.last_end.time_since_epoch().count() && s.last_end > from) from = s.last_end;
        }
        if (now <= from) return 0;
        return std::chrono::duration<double>(now - from).count();
    }
    // The rule in one line: does this gap count as an outage (>= `silent_s` outside loads)?
    bool gap_counts(std::uint64_t peer, Clock::time_point prev, Clock::time_point now, double silent_s) const {
        return effective_silent_s(peer, prev, now) >= silent_s;
    }
    // Whether a load of either side touched the gap at all (for log wording).
    bool load_touched(std::uint64_t peer, Clock::time_point prev) const {
        if (gone_.count(peer)) return true;
        for (std::uint64_t k : {kOwn, kHost, peer}) {
            auto it = scopes_.find(k);
            if (it == scopes_.end()) continue;
            const Scope& s = it->second;
            if (s.loading || (s.last_end.time_since_epoch().count() && s.last_end > prev)) return true;
        }
        return false;
    }

private:
    struct Scope {
        bool loading = false;
        Clock::time_point last_start{}, last_end{};
    };
    std::map<std::uint64_t, Scope> scopes_;
    std::set<std::uint64_t> gone_;
};

}  // namespace bbnet::nr
