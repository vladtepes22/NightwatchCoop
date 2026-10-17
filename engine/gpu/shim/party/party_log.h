// SPDX-License-Identifier: GPL-3.0-or-later
// Party logging levels, areas, correlation tags and user marks (docs/party/logging.md).
//
// Env (the launcher's Advanced / Debug tab sets them):
//   BB_PARTY_LOG=normal (default) | verbose | trace   (also 0 / 1 / 2)
//   BB_PARTY_LOG_AREAS=all (default) | comma list of net,summon,travel,death,items,flags,dialog
//   BB_PARTY_LOG_SNAPSHOT_S=10   verbose: one state snapshot per this many seconds (0 = none)
//
// Normal: the party's usual lines, nothing added (no per-frame work, no tags).
// Verbose: every "Party..." line gets a correlation tag
//   {run 3fa2c1 G1 t+123.456 pc 98765.432 19:49:49.123}
//   run = this game process's random log id, G1 / H0 = role + link slot, t+ = seconds since start
//   (monotonic), pc = the party clock (the host's link clock, the same on every peer within ~RTT/2),
//   then local wall time; the area events (party_log_game.cpp) and the periodic snapshots.
// Trace: verbose plus event bodies and per-change detail (rate-limited).
// User marks (F9 / the overlay's Party tab) are written at every level.
#pragma once

#include <cstdint>
#include <string>

namespace coop::plog {

enum class Level : int { Normal = 0, Verbose = 1, Trace = 2 };

enum Area : unsigned {
    kNet = 1u << 0,
    kSummon = 1u << 1,
    kTravel = 1u << 2,
    kDeath = 1u << 3,
    kItems = 1u << 4,
    kFlags = 1u << 5,
    kDialog = 1u << 6,
    kAllAreas = (1u << 7) - 1,
};

struct Config {
    Level level = Level::Normal;
    unsigned areas = kAllAreas;
    int snapshot_s = 10;
};

/// Parses the env values (nullptr / "" = defaults). Unknown level words = Normal; unknown area
/// names are ignored ("all" or an empty list = every area). Pure (tests).
Config ParseConfig(const char* level, const char* areas, const char* snapshot_s);
const char* LevelName(Level l);
/// "net,summon,..." ("all" for every area, "none" for no area).
std::string AreasText(unsigned areas);
/// The area an event name of the party link belongs to (kNet when none fits). Pure.
unsigned AreaOfEvent(const std::string& name);

/// The process configuration (read from the env once).
const Config& Get();
/// Tests: replaces the process configuration.
void SetConfigForTest(const Config& c);
inline bool Verbose() { return Get().level >= Level::Verbose; }
inline bool TraceOn() { return Get().level >= Level::Trace; }
/// Level reached and one of `areas` selected.
inline bool On(unsigned areas, Level min = Level::Verbose) {
    const Config& c = Get();
    return c.level >= min && (c.areas & areas) != 0;
}

/// This process's log id (6 hex digits, random per run).
const std::string& RunId();
/// Monotonic milliseconds since this module first ran.
std::int64_t MonoMs();
/// The local peer: role 'H' / 'G' / '-' and link slot (-1 unknown). Any thread.
void SetPeer(char role, int slot);
/// The party clock: host link clock = MonoMs() + offset. `known` false = no link (pc shown as "-").
void SetPartyClockOffset(bool known, std::int64_t offset_ms);
/// The party clock now in ms (-1 when unknown).
std::int64_t PartyClockMs();
/// The correlation tag ("{run .. G1 t+.. pc .. hh:mm:ss.mmm} ") at Verbose and above, else "".
std::string Tag();
/// Inserts Tag() into a "Party...: text" line after its first ": " (other lines unchanged).
/// Returns the line as is at Normal. Pure apart from Tag() (tests pass `tag`).
std::string TagLine(const std::string& line, const std::string& tag);
/// Registers TagLine as party_async_io's line tagger (once; Verbose and above only).
void InstallTagger();

/// Writes "Party log: <area>: text" when On(area, min).
void Event(unsigned area, Level min, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

/// At most `burst` hits per `window_ms`; later hits are counted and reported by the next allowed
/// one (`*suppressed`). Not thread-safe: one per call site / thread.
class RateLimit {
public:
    RateLimit(int burst, int window_ms) : burst_(burst), window_ms_(window_ms) {}
    bool Allow(std::int64_t now_ms, int* suppressed = nullptr);

private:
    int burst_, window_ms_;
    std::int64_t window_start_ = -1;
    int used_ = 0, dropped_ = 0;
};

// ---- User marks ----
/// The mark line: "=== USER MARK n === <who>, <origin>; local hh:mm:ss.mmm, party clock X s".
std::string MarkLine(int n, const std::string& who, const std::string& origin, std::int64_t pc_ms,
                     const std::string& wall);
/// A user mark by this player (any thread): writes the line, asks the peers to mark theirs
/// (MarkRelay) and requests a snapshot. Returns its number.
int UserMark(const char* origin);
/// A mark from a peer (link event): written with the origin's number and name.
void PeerMark(int n, const std::string& who, int from_slot, std::int64_t pc_ms);
/// The local player's name for mark lines (party name).
void SetLocalName(const std::string& name);
/// Called by UserMark with the new mark's number (party_log_game.cpp sends it to the peers).
using MarkRelay = void (*)(int n);
void SetMarkRelay(MarkRelay fn);
/// A snapshot was asked for (a mark); the game tick takes it (one-shot).
bool TakeSnapshotRequest();

/// The local wall clock "hh:mm:ss.mmm".
std::string WallClock();

}  // namespace coop::plog
