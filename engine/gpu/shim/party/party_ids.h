// SPDX-License-Identifier: GPL-3.0-or-later
// Game id allowlists for peer validation (bbport security pass): which map, lamp / warp, entity
// and cutscene ids exist in Bloodborne 1.09. A host's travel / story / phantom events and a
// member's sign carry such ids into the other players' native code (0x13CDF30, 0x132E010,
// 0x132E050, the remo player, the summon list); an id outside these tables would make that code
// look up a param row, map or file that is not there, so it never gets there.
//
// The tables are read at run time from the player's own game (party_gamedata.h; scripts/partydata/ids_tool.py
// says exactly what each one holds). With no tables loaded every id is unknown: the peer's request is refused.
#pragma once

#include "party_gamedata.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::ids {

inline const std::vector<std::uint32_t>& Maps() { return gamedata::Get().maps; }
/// ReturnPointParam row ids whose map exists (every lamp / headstone / last-lamp record id).
inline const std::vector<std::uint32_t>& ReturnPoints() { return gamedata::Get().return_points; }
inline const std::vector<std::uint32_t>& Entities() { return gamedata::Get().entities; }
inline const std::vector<std::uint32_t>& Remos() { return gamedata::Get().remos; }
/// Return point rows that have a spot, and their x, y, z, yaw (4 floats per id), index-aligned.
inline const std::vector<std::uint32_t>& ReturnSpotIds() { return gamedata::Get().return_spot_ids; }
inline const std::vector<float>& ReturnSpots() { return gamedata::Get().return_spots; }

inline bool InSorted(const std::vector<std::uint32_t>& table, std::uint32_t v) {
    return std::binary_search(table.begin(), table.end(), v);
}

/// area << 24 | block << 16 | region << 8 | index of an existing map (folder or MSB).
inline bool MapKnown(std::uint32_t packed_map) {
    return InSorted(Maps(), packed_map);
}
/// The map of an entity id AABnnnn (area AA, block B): AA << 24 | B << 16.
constexpr std::uint32_t EntityMap(std::uint32_t entity) {
    return (entity / 100000) << 24 | (entity / 10000 % 10) << 16;
}
/// A ReturnPointParam row with an existing map: an id the lamp warp 0x13CDF30 (and the last-lamp
/// / respawn records +0x1528 / +0x153C) can hold.
inline bool ReturnPointKnown(std::uint32_t id) {
    return InSorted(ReturnPoints(), id);
}
/// An entity id of its own map's MSB (or an EMEVD / param warp point): a warp point / bonfire id
/// 0x132E010 / 0x132E050 can be asked to place the player at.
inline bool EntityKnown(std::uint32_t id) {
    return InSorted(Entities(), id);
}
/// A warp point in map `packed_map` (same area and block).
inline bool WarpPointKnown(std::uint32_t packed_map, std::uint32_t point) {
    return EntityKnown(point) && EntityMap(point) == (packed_map & 0xffff0000u);
}
/// The table's spot as it is, the m29 template MSB's for a Chalice Dungeon point (the world warper's
/// opt-in tour of the template map; nothing in a party).
inline bool TemplateReturnSpot(std::uint32_t id, float out[4]) {
    const auto& ids = ReturnSpotIds();
    const auto b = ids.begin();
    const auto e = ids.end();
    const auto it = std::lower_bound(b, e, id);
    if (it == e || *it != id) {
        return false;
    }
    const std::size_t i = static_cast<std::size_t>(it - b) * 4;
    for (int k = 0; k < 4; ++k) {
        out[k] = ReturnSpots()[i + static_cast<std::size_t>(k)];
    }
    return true;
}
/// Where the lamp / headstone warp of ReturnPointParam row `id` puts the player in its map: x, y,
/// z and the yaw (radians) of the MSB region its revive event names. False: no spot (a Chalice
/// Dungeon point, whose layout is generated, or no revive event).
inline bool ReturnSpot(std::uint32_t id, float out[4]) {
    // chalicelamp: the table holds the m29 template MSB's spots for 2902950.., but a dungeon's layout is
    // generated and its lamps stand elsewhere (a guest's report: the template's spot of 2902954 was
    // 180 m from that lamp, in the air above the dungeon). No spot for them, as documented above.
    if (id / 100000 == 29) {
        return false;
    }
    return TemplateReturnSpot(id, out);
}
/// Where a stage warp's warp point `id` (EMEVD 2003[14] / Lua WarpNextStage, GSM +0x10) puts the
/// player: x, y, z, yaw (radians) of the MSB region with that entity id. False: not in a block MSB.
inline bool WarpSpot(std::uint32_t id, float out[4]) {
    const auto& ids = gamedata::Get().warp_spot_ids;
    const auto b = ids.begin();
    const auto e = ids.end();
    const auto it = std::lower_bound(b, e, id);
    if (it == e || *it != id) {
        return false;
    }
    const std::size_t i = static_cast<std::size_t>(it - b) * 4;
    for (int k = 0; k < 4; ++k) {
        out[k] = gamedata::Get().warp_spots[i + static_cast<std::size_t>(k)];
    }
    return true;
}
/// A cutscene id with a remo/sAA_BB_NNNN.remobnd file (or one an EMEVD 2002 plays).
inline bool RemoKnown(std::uint32_t remo_id) {
    return InSorted(Remos(), remo_id);
}

} // namespace coop::ids
