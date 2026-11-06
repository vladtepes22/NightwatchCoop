// SPDX-License-Identifier: GPL-3.0-or-later
// The event scripts of the player's own game expanded the way the game runs them: a parameterised event is
// instantiated once per InitializeEvent 2000[0] call with that call's own arguments, so the flag and entity
// ids are the concrete ones. For each instance: the instructions that write or register event flags, the
// markers (boss defeat, item lot award, cutscene, map move ...), the flag reads, and the calls it makes.
#pragma once

#include "emevd.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bbdata {

enum class FlagOp : std::uint8_t {
    Set, Toggle, Random, Batch, ValueInc, ValueClear, ValueOp, ValueItemCount, ValueItemDirect, AiSos, NpcSummon,
    HawkInfo, HawkDrop, Ladder, WanderInit, WanderReg, Bonfire, Lamp, ObjDamage, ObjEventDel, NpcHumanity
};
enum class StateKind : std::uint8_t { Int, Reg, Rand, Value };

struct Op {
    FlagOp op = FlagOp::Set;
    StateKind kind = StateKind::Int;  // Int: `state` is the written state (0 off, 1 on, 2 toggle)
    std::int64_t first = 0, last = 0, state = 0;
    bool StateIs(std::int64_t v) const { return kind == StateKind::Int && state == v; }
};

enum class MarkerKind : std::uint8_t {
    BossDefeat, MidbossDefeat, ItemLot, ItemLotClient, MapMove, Trophy, NgPlus, TitleExit, WarpReturn,
    SaveRequest, NetworkSync, BossRoomEnter, Cutscene
};
struct Marker {
    MarkerKind kind = MarkerKind::BossDefeat;
    std::uint8_t sub = 0;  // Cutscene: the 2002[k] instruction number
    std::vector<std::int64_t> v;
};

struct EventCall {
    std::int32_t slot = 0;
    std::uint32_t target = 0;
    std::string params;  // the call's argument bytes after slot + target
};

/// A condition instruction the party derivations read arguments of, in instruction order: 3[0] (flag state: v[3] = the
/// flag), 3[2] (player in a region: v[3] = the region's entity), 3[3] (player near an entity: the entity and the metres
/// as float bits), 3[4] (player holds an item: v[1] = item type, v[2] = its id, v[3] = 1 for holds), 3[24] (action
/// button: v[2] = the object's entity), 4[0] (character dead: v[1] = the entity) and 2004[8] (SpEffect on a
/// character: v[0] = the entity, v[1] = the effect id).
struct Cond {
    std::uint32_t bank = 0, id = 0;
    std::vector<std::int64_t> v;
};

struct EventRec {
    std::int64_t event = 0;
    std::int32_t slot = 0;
    std::uint32_t rest = 0;
    std::string name;
    std::vector<Op> ops;
    std::vector<Marker> markers;
    std::vector<std::vector<std::int64_t>> reads;  // flag ids of the condition instructions
    std::vector<EventCall> calls;
    std::vector<Cond> conds;
    bool Has(MarkerKind k) const {
        for (const Marker& m : markers) {
            if (m.kind == k) return true;
        }
        return false;
    }
};

/// A call into an event that another file defines (a map script starting a common.emevd event).
struct ExternalCall {
    std::int32_t slot = 0;
    std::string params;
};
using ExternalCalls = std::unordered_map<std::int64_t, std::vector<ExternalCall>>;

/// Instances of every event of one file. `names`: the file's EMELD names. `external`: concrete calls into
/// this file's events from other files.
std::vector<EventRec> ExpandFile(const Emevd& ev, const std::unordered_map<std::int64_t, std::string>& names,
                                 const Emedf& emedf, const ExternalCalls& external);

}  // namespace bbdata
