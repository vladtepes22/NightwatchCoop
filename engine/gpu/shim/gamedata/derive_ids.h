// SPDX-License-Identifier: GPL-3.0-or-later
// Map, lamp, entity, cutscene and NPC tables derived from the maps (MSB), the event scripts and the params.
#pragma once

#include "derive.h"
#include "msb.h"

#include <set>

namespace coop::gamedata {

/// A map studio file named mAA_BB_CC_DD.msb(.dcx) under map/mapstudio.
struct MsbFile {
    int a = 0, b = 0, c = 0, d = 0;
    std::string path;
    std::uint64_t size = 0;
};

/// What the tables take from one MSB (decoded once, in parallel).
struct MsbInfo {
    std::vector<std::int32_t> entities;                 // int32 words in the map's own id range (b < 10)
    std::vector<std::pair<std::int32_t, std::int32_t>> events;  // (event entity id, region index)   (c == d == 0, b < 10)
    std::vector<bbdata::MsbPoint> points;               // POINT_PARAM_ST entries                   (same)
    std::vector<std::int32_t> npcs;                     // NPC character part entity ids            (same)
};

/// Decodes one MSB for the tables. Throws FormatError.
MsbInfo ReadMsb(const MsbFile& f);

struct IdsInput {
    std::set<std::int64_t> maps;             // packed map ids (folders and MSBs)
    std::set<std::int64_t> remo_files;       // ids of remo/sAA_BB_NNNN.remobnd(.dcx)
    const std::vector<MsbFile>* msbs = nullptr;
    const std::vector<MsbInfo>* infos = nullptr;  // one per msbs entry
    const ParamSet* params = nullptr;
    const std::vector<FileEvents>* events = nullptr;
};

/// maps returnPoints entities remos returnSpots warpSpots npcMirror.
Sections DeriveIds(const IdsInput& in);

/// The Beckoning-Bell helpers' fog walk rows (section npcFog). `call_rows`: per map script stem, the
/// InitializeEvent calls of event 0 (as int32 arguments) whose event id ends 4450 / 4460.
struct FogCalls {
    std::string stem;
    std::vector<std::vector<std::int32_t>> calls;
};
/// The stems whose event 0 holds such calls, with the calls (stems in the order given).
std::vector<FogCalls> CollectFogCalls(const std::string& stem, const bbdata::Emevd& evd);
/// `msb_dir`: the mapstudio folder. Reads the arena MSBs it needs with `threads` threads.
Sections DeriveNpcFog(const std::vector<FogCalls>& calls, const std::string& msb_dir, unsigned threads);

}  // namespace coop::gamedata
