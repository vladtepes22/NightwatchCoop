// SPDX-License-Identifier: GPL-3.0-or-later
// Two flag lists the progress sync (party_progress.cpp) needs besides the category table, derived from the map
// scripts of the player's own game:
//
//  * stateIds: flags whose state the game moves back and forth, so the sync copies the host's current state (both 0
//    and 1) instead of only ever turning them on: every flag a map script writes with SetEventFlag to 0 (or toggles)
//    and the category table files under shortcut_door / world_state (elevators, gates, bridges: the scripts clear the
//    flag of the position the door left). Common events are not read: they are the per-player systems (the DLC
//    clean-up that clears flags of a player without the DLC).
//  * runtimeIds: flags the scripts count or randomise (value operations, RandomlySetEventFlagInRange), in the same two
//    categories. A partial sync would corrupt them: a counter that only the host's copy of the script advanced, a
//    random pick that each copy makes for itself. Never synced.
#include "derive.h"

#include <set>

namespace coop::gamedata {

Sections DeriveProgressIds(const FlagCategories& cats, const std::vector<FileEvents>& events) {
    using bbdata::FlagOp;
    constexpr std::int64_t kMaxSpan = 64;  // a longer range is a batch over a whole group, not a state flag
    std::set<std::int64_t> state, runtime;
    for (const FileEvents& fe : events) {
        for (const bbdata::EventRec& r : fe.recs) {
            for (const bbdata::Op& o : r.ops) {
                if (o.last < o.first || o.last - o.first > kMaxSpan) continue;
                const bool clears = fe.stem != "common" && ((o.op == FlagOp::Set && o.StateIs(0)) || o.op == FlagOp::Toggle || (o.op == FlagOp::Set && o.StateIs(2)));
                const bool counts = o.op == FlagOp::Random || o.op == FlagOp::ValueInc || o.op == FlagOp::ValueClear || o.op == FlagOp::ValueOp ||
                                    o.op == FlagOp::ValueItemCount || o.op == FlagOp::ValueItemDirect;
                if (!clears && !counts) continue;
                for (std::int64_t f = o.first; f <= o.last; ++f) {
                    const Cat c = cats.Lookup(f);
                    if (c != Cat::shortcut_door && c != Cat::world_state) continue;
                    if (clears) state.insert(f);
                    if (counts) runtime.insert(f);
                }
            }
        }
    }
    Sections s;
    auto& a = s["stateIds"];
    for (const std::int64_t f : state) a.push_back({std::to_string(f)});
    auto& b = s["runtimeIds"];
    for (const std::int64_t f : runtime) b.push_back({std::to_string(f)});
    return s;
}

}  // namespace coop::gamedata
