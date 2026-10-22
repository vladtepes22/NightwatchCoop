// SPDX-License-Identifier: GPL-3.0-or-later
// B / D parity (streamsync, 0.7.1): the party's kill record. Only a rest or a death load resets the
// world, for everyone; until then an enemy killed in the party stays dead on every game.
//
// Two holes it closes (hostcheck run respawnE):
//  - A neighbouring map's enemy (a Cathedral Ward 2400xxx seen from the Tomb of Oedon, m24_01)
//    comes back alive when its map streams out and in again, with no load. That is the game's own
//    behaviour, in single player too (streamsync solo run: killed 2400125 at 28 m, walked 130 m
//    away and back: "loaded, HP 282" again, the echoes paid again), but only the game that streamed
//    it shows it alive: the others keep it dead. In a party it is put to death again at once, with
//    no echoes and no drop (it is the same enemy, already paid for).
//  - The game's own enemy sync never carries a kill of a neighbouring map's enemy, and the census
//    repair (party_progress_bridge, every 15 s) only sees it while both games have it loaded: a
//    host who walked away 3 s after the kill left it alive on the guests for good. Every game now
//    sends the deaths it sees to the host, which keeps the record and relays it to every member;
//    a member that has the enemy loaded and alive puts it to death 3 s later (the game's own sync
//    first), with the kill's echoes and the shared drop roll when it had it loaded at the kill,
//    silently when it streams it in only later (it was not there).
//
// Pure logic (unit test: tests/test_party_enemy_census.cpp); the game side is in
// party_progress_bridge.cpp (the host's rule: System > Party rules: World, party_kill_record, BB_PARTY_KILL_RECORD). Regular named enemies only: no boss
// (their defeat has its own path), no enemy without a map entity id (no id the same on every game).
#pragma once

#include "party_enemy_census.h"

#include <cstdint>
#include <map>
#include <vector>

namespace coop::killrec {

constexpr double kRemoteGraceS = 3.0;  // a remote kill: the game's own enemy sync gets this long first
constexpr double kLoadHoldS = 10.0;    // after this game's own load: no action (a party load's reset first)

/// One loaded enemy of this game's census.
struct Obs {
    std::uint32_t entity = 0;  ///< map entity id (chr +0x1F0)
    bool alive = false;
    std::uint64_t chr = 0;     ///< the chr instance
    std::uint32_t key = 0;     ///< census key (chr +0x3B8): another chr instance has another key
};

/// Put `chr` to death; `silent`: no echoes, no drop (an enemy already killed and paid for).
struct Action {
    std::uint32_t entity = 0;
    std::uint64_t chr = 0;
    bool silent = false;
    bool respawn = false;  ///< it came back when its map streamed in again (else: a party member's kill)
};

inline bool Recordable(std::uint32_t entity) {
    return entity != 0 && entity != 0xFFFFFFFFu && !EnemyIsBossEntity(entity);
}

class KillRecord {
public:
    /// A new world (the host's load: a rest, a death, a travel): everything forgotten.
    void Reset(std::uint64_t epoch) {
        epoch_ = epoch;
        hold_until_ = 0;
        m_.clear();
        alive_.clear();
    }
    /// This game's own load while the host's world stays (a guest's): every chr is a new one now.
    /// Nothing is put to death for kLoadHoldS: a party load (a rest, a wipe) resets the record
    /// with the host's next epoch, which may come a little after this game's load.
    void LocalLoad(double now) {
        hold_until_ = now + kLoadHoldS;
        alive_.clear();
        for (auto& [id, e] : m_) {
            e.gone = true;
            e.present_at_remote = false;
        }
    }
    std::uint64_t epoch() const { return epoch_; }
    std::vector<std::uint32_t> Entities() const {
        std::vector<std::uint32_t> out;
        for (const auto& [id, e] : m_) out.push_back(id);
        return out;
    }
    std::size_t size() const { return m_.size(); }
    bool Dead(std::uint32_t entity) const {
        const auto it = m_.find(entity);
        return it != m_.end();
    }
    /// This game has seen it dead (killed here, or put to death here): paid for here.
    bool PaidHere(std::uint32_t entity) const {
        const auto it = m_.find(entity);
        return it != m_.end() && it->second.seen_dead;
    }

    /// A party member's kill (from the host). True when it is new here.
    bool AddRemote(std::uint32_t entity, double now) {
        if (!Recordable(entity) || m_.count(entity)) return false;
        E& e = m_[entity];
        e.remote_at = now;
        e.present_at_remote = alive_.count(entity) != 0;  // loaded here, alive, at the kill
        return true;
    }

    /// This game's loaded enemies now. Returns what to put to death; `died` gets the deaths seen
    /// here that are new to the record (for the party).
    std::vector<Action> Update(const std::vector<Obs>& list, double now, std::vector<std::uint32_t>* died) {
        std::vector<Action> out;
        // Several chrs of one entity id (rare): left alone.
        std::map<std::uint32_t, int> count;
        for (const Obs& o : list)
            if (Recordable(o.entity)) ++count[o.entity];
        std::map<std::uint32_t, const Obs*> here;
        for (const Obs& o : list)
            if (Recordable(o.entity) && count[o.entity] == 1) here[o.entity] = &o;
        for (auto& [id, e] : m_)
            if (!here.count(id)) e.gone = true;  // streamed out (or never loaded here)
        std::map<std::uint32_t, std::uint64_t> alive_now;
        for (const auto& [id, o] : here) {
            auto it = m_.find(id);
            if (!o->alive) {
                if (it == m_.end()) {
                    // Seen alive here before: a death here (dead at first sight: dead before this
                    // game looked, e.g. a load's dead chr: not a kill of this party's).
                    if (alive_.count(id)) {
                        E& e = m_[id];
                        e.seen_dead = true;
                        e.chr = o->chr;
                        e.key = o->key;
                        if (died) died->push_back(id);
                    }
                } else {
                    E& e = it->second;
                    e.seen_dead = true;
                    e.gone = false;
                    e.chr = o->chr;
                    e.key = o->key;
                    e.acted_chr = 0;
                }
                continue;
            }
            alive_now[id] = o->chr;
            if (it == m_.end()) continue;
            E& e = it->second;
            if (e.acted_chr == o->chr) continue;  // put to death already, still dying
            if (now < hold_until_) continue;      // just after this game's own load
            if (e.seen_dead) {
                if (o->chr == e.chr && o->key == e.key) {
                    // The same chr alive again, never unloaded or not: the game revived it (a
                    // scripted revive). Not the party's to undo: forgotten.
                    m_.erase(it);
                    continue;
                }
                if (!e.gone) continue;  // a new chr without the map streaming out: wait for a census
                out.push_back({id, o->chr, true, true});
                e.acted_chr = o->chr;
                continue;
            }
            // A party member's kill, not seen dead here yet.
            if (now - e.remote_at < kRemoteGraceS) continue;
            out.push_back({id, o->chr, !e.present_at_remote, false});
            e.acted_chr = o->chr;
        }
        alive_.swap(alive_now);
        return out;
    }

private:
    struct E {
        bool seen_dead = false;          // dead on this game (killed or put to death here)
        bool gone = false;               // not loaded here since it was seen dead
        bool present_at_remote = false;  // loaded and alive here when the remote kill came
        double remote_at = 0;
        std::uint64_t chr = 0;  // the dead chr instance
        std::uint32_t key = 0;
        std::uint64_t acted_chr = 0;  // put to death by the party: not again
    };
    std::uint64_t epoch_ = 0;
    double hold_until_ = 0;
    std::map<std::uint32_t, E> m_;
    std::map<std::uint32_t, std::uint64_t> alive_;  // seen alive in the last update
};

}  // namespace coop::killrec
