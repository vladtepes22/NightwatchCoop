// SPDX-License-Identifier: GPL-3.0-or-later
// MSB (map studio files) of the player's own game: only the parts the party tables use. Bounds checked.
#pragma once

#include "bytes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bbdata {

struct MsbList {
    std::string name;  // "EVENT_PARAM_ST", "POINT_PARAM_ST", "PARTS_PARAM_ST" ...
    std::vector<std::int64_t> entries;  // entry offsets from the file start
};

/// The entry lists of an MSB (MSBB: MODEL, EVENT, POINT, ROUTE, LAYER, PARTS). Throws FormatError.
std::vector<MsbList> ParseMsbLists(Span data);
/// The entries of the named list (the last list of that name), empty when absent.
const std::vector<std::int64_t>* FindMsbList(const std::vector<MsbList>& lists, const std::string& name);

struct MsbPoint {
    float x = 0, y = 0, z = 0, yaw_deg = 0;
    bool has_entity = false;
    std::int32_t entity = 0;
};
/// POINT_PARAM_ST entries: position at +0x14, rotation (degrees) at +0x20, the entity data offset at +0x48.
/// `strict_entity`: an entity offset that is not inside the file throws (the NPC fog reads it unchecked);
/// otherwise such a point simply has no entity.
std::vector<MsbPoint> ReadMsbPoints(Span data, const std::vector<std::int64_t>& entries, bool strict_entity);

/// PARTS_PARAM_ST entries: the part type at +0x14 (1 object, 2 character ...), the position at +0x28, the entity
/// data offset at +0xB0. An entry too short for that, or an entity offset outside the file, has no entity.
struct MsbPart {
    std::int32_t type = 0;
    float x = 0, y = 0, z = 0;
    bool has_entity = false;
    std::int32_t entity = 0;
};
std::vector<MsbPart> ReadMsbParts(Span data, const std::vector<std::int64_t>& entries);
/// The names (UTF-8) of the entries of a list (the offset at +0 of each entry, relative to it; "" when absent).
std::vector<std::string> ReadMsbNames(Span data, const std::vector<std::int64_t>& entries);

/// Every 4-byte aligned int32 of the file in [lo, lo + 10000), inserted into `out` (a superset of the map's
/// point / part entity ids). Throws unless the file starts with "MSB ".
void ScanMsbEntities(Span data, std::int64_t lo, std::vector<std::int32_t>* out);

}  // namespace bbdata
