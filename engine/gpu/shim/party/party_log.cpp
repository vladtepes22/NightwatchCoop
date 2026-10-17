// SPDX-License-Identifier: GPL-3.0-or-later
// Party logging levels, tags and user marks (party_log.h, docs/party/logging.md). No game code:
// the game side (snapshots, area events, the mark relay) is party_log_game.cpp.
#include "party_log.h"

#include "party_async_io.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <random>

namespace coop::plog {
namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point g_start = Clock::now();

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

struct AreaName {
    const char* name;
    unsigned bit;
};
constexpr AreaName kAreaNames[] = {{"net", kNet},     {"summon", kSummon}, {"travel", kTravel}, {"death", kDeath},
                                   {"items", kItems}, {"flags", kFlags},   {"dialog", kDialog}};

std::mutex g_cfg_mu;
std::atomic<Config*> g_cfg{nullptr};  // never freed (read from any thread at exit)

std::atomic<char> g_role{'-'};
std::atomic<int> g_slot{-1};
std::atomic<bool> g_clock_known{false};
std::atomic<std::int64_t> g_clock_offset{0};
std::atomic<int> g_marks{0};
std::atomic<bool> g_snapshot_req{false};
std::atomic<MarkRelay> g_relay{nullptr};
std::mutex g_name_mu;
std::string g_name;

}  // namespace

Config ParseConfig(const char* level, const char* areas, const char* snapshot_s) {
    Config c;
    const std::string l = Lower(Trim(level ? level : ""));
    if (l == "verbose" || l == "1" || l == "v") c.level = Level::Verbose;
    else if (l == "trace" || l == "2" || l == "t" || l == "debug") c.level = Level::Trace;
    else c.level = Level::Normal;
    const std::string a = Lower(Trim(areas ? areas : ""));
    if (!a.empty() && a != "all") {
        unsigned bits = 0;
        bool any_word = false;
        std::size_t pos = 0;
        while (pos <= a.size()) {
            std::size_t end = a.find_first_of(",; +", pos);
            if (end == std::string::npos) end = a.size();
            const std::string word = Trim(a.substr(pos, end - pos));
            if (!word.empty()) {
                any_word = true;
                if (word == "all") bits |= kAllAreas;
                for (const AreaName& n : kAreaNames)
                    if (word == n.name) bits |= n.bit;
            }
            pos = end + 1;
        }
        // Only unknown words: every area (a typo should not silence the log); "none": no area.
        c.areas = bits ? bits : (any_word && a == "none") ? 0u : static_cast<unsigned>(kAllAreas);
    }
    if (snapshot_s && *snapshot_s) {
        char* end = nullptr;
        const long v = std::strtol(snapshot_s, &end, 10);
        if (end != snapshot_s && v >= 0) c.snapshot_s = v > 3600 ? 3600 : static_cast<int>(v);
    }
    return c;
}

const char* LevelName(Level l) {
    switch (l) {
    case Level::Verbose: return "verbose";
    case Level::Trace: return "trace";
    default: return "normal";
    }
}

std::string AreasText(unsigned areas) {
    if ((areas & kAllAreas) == kAllAreas) return "all";
    std::string out;
    for (const AreaName& n : kAreaNames) {
        if (!(areas & n.bit)) continue;
        if (!out.empty()) out += ",";
        out += n.name;
    }
    return out.empty() ? "none" : out;
}

unsigned AreaOfEvent(const std::string& name) {
    if (name == "travel" || name == "lamp_request" || name == "lamp_lit") return kTravel;
    if (name == "b2") return kSummon;
    if (name == "phantom") return kDeath;
    if (name == "items" || name == "items_full" || name == "gift_up" || name == "drop_seed" || name == "chalice_goods")
        return kItems;
    if (name == "flags" || name == "flag_snapshot" || name == "world_groups" || name == "story" ||
        name == "story_up" || name == "story_notice" || name == "world_action")
        return kFlags;
    return kNet;
}

const Config& Get() {
    if (Config* c = g_cfg.load(std::memory_order_acquire)) return *c;
    std::lock_guard<std::mutex> lk(g_cfg_mu);
    if (!g_cfg.load()) {
        g_cfg.store(new Config(ParseConfig(std::getenv("BB_PARTY_LOG"), std::getenv("BB_PARTY_LOG_AREAS"),
                                           std::getenv("BB_PARTY_LOG_SNAPSHOT_S"))),
                    std::memory_order_release);
    }
    return *g_cfg.load();
}

void SetConfigForTest(const Config& c) {
    std::lock_guard<std::mutex> lk(g_cfg_mu);
    if (Config* cur = g_cfg.load()) *cur = c;
    else g_cfg.store(new Config(c));
}

const std::string& RunId() {
    static const std::string id = [] {
        std::random_device rd;
        std::uint64_t seed = (static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^
                             static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        std::mt19937_64 rng(seed);
        char b[16];
        std::snprintf(b, sizeof b, "%06x", static_cast<unsigned>(rng() & 0xffffffu));
        return std::string(b);
    }();
    return id;
}

std::int64_t MonoMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_start).count();
}

void SetPeer(char role, int slot) {
    g_role.store(role, std::memory_order_relaxed);
    g_slot.store(slot, std::memory_order_relaxed);
}

void SetPartyClockOffset(bool known, std::int64_t offset_ms) {
    g_clock_offset.store(offset_ms, std::memory_order_relaxed);
    g_clock_known.store(known, std::memory_order_relaxed);
}

std::int64_t PartyClockMs() {
    if (!g_clock_known.load(std::memory_order_relaxed)) return -1;
    return MonoMs() + g_clock_offset.load(std::memory_order_relaxed);
}

std::string WallClock() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t secs = std::chrono::system_clock::to_time_t(now);
    const int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif
    char b[32];
    std::snprintf(b, sizeof b, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    return b;
}

std::string Tag() {
    if (!Verbose()) return {};
    const std::int64_t t = MonoMs(), pc = PartyClockMs();
    const int slot = g_slot.load(std::memory_order_relaxed);
    char who[8];
    if (slot >= 0) std::snprintf(who, sizeof who, "%c%d", g_role.load(std::memory_order_relaxed), slot);
    else std::snprintf(who, sizeof who, "%c-", g_role.load(std::memory_order_relaxed));
    char pcs[32];
    if (pc >= 0) std::snprintf(pcs, sizeof pcs, "%lld.%03lld", static_cast<long long>(pc / 1000), static_cast<long long>(pc % 1000));
    else std::snprintf(pcs, sizeof pcs, "-");
    char b[128];
    std::snprintf(b, sizeof b, "{run %s %s t+%lld.%03lld pc %s %s} ", RunId().c_str(), who,
                  static_cast<long long>(t / 1000), static_cast<long long>(t % 1000), pcs, WallClock().c_str());
    return b;
}

std::string TagLine(const std::string& line, const std::string& tag) {
    if (tag.empty() || line.compare(0, 5, "Party") != 0) return line;
    const std::size_t colon = line.find(": ");
    if (colon == std::string::npos || colon > 40) return line;
    std::string out;
    out.reserve(line.size() + tag.size());
    out.append(line, 0, colon + 2);
    out += tag;
    out.append(line, colon + 2, std::string::npos);
    return out;
}

namespace {
std::string Tagger(const std::string& line) { return TagLine(line, Tag()); }
}  // namespace

void InstallTagger() {
    if (!Verbose()) return;
    asyncio::SetLineTagger(&Tagger);
    const Config& c = Get();
    asyncio::Print("Party log: level " + std::string(LevelName(c.level)) + ", areas " + AreasText(c.areas) +
                   ", snapshot every " + std::to_string(c.snapshot_s) + " s; run id " + RunId() +
                   " (tag: {run <id> <H|G><slot> t+<s since start> pc <party clock s> <local time>})\n");
}

void Event(unsigned area, Level min, const char* fmt, ...) {
    if (!On(area, min)) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    const char* name = "net";
    for (const AreaName& n : kAreaNames)
        if (area & n.bit) {
            name = n.name;
            break;
        }
    asyncio::Print(std::string("Party log: ") + name + ": " + buf + "\n");
}

bool RateLimit::Allow(std::int64_t now_ms, int* suppressed) {
    if (window_start_ < 0 || now_ms - window_start_ >= window_ms_) {
        window_start_ = now_ms;
        used_ = 0;
    }
    if (used_ >= burst_) {
        ++dropped_;
        return false;
    }
    ++used_;
    if (suppressed) *suppressed = dropped_;
    dropped_ = 0;
    return true;
}

std::string MarkLine(int n, const std::string& who, const std::string& origin, std::int64_t pc_ms,
                     const std::string& wall) {
    char pcs[32];
    if (pc_ms >= 0) std::snprintf(pcs, sizeof pcs, "%lld.%03lld s", static_cast<long long>(pc_ms / 1000),
                                  static_cast<long long>(pc_ms % 1000));
    else std::snprintf(pcs, sizeof pcs, "unknown (no party link)");
    return "=== USER MARK " + std::to_string(n) + " === " + (who.empty() ? std::string("this player") : who) + ", " +
           origin + "; local " + wall + ", party clock " + pcs;
}

void SetLocalName(const std::string& name) {
    std::lock_guard<std::mutex> lk(g_name_mu);
    g_name = name;
}

void SetMarkRelay(MarkRelay fn) { g_relay.store(fn); }

int UserMark(const char* origin) {
    const int n = ++g_marks;
    std::string who;
    {
        std::lock_guard<std::mutex> lk(g_name_mu);
        who = g_name;
    }
    const int slot = g_slot.load(std::memory_order_relaxed);
    if (slot >= 0) who += (who.empty() ? "" : " ") + std::string("(slot ") + std::to_string(slot) + ")";
    // Unconditional (every level), without a tagger prefix so it starts the line.
    asyncio::Print(MarkLine(n, who, origin ? origin : "mark", PartyClockMs(), WallClock()) + " (run " + RunId() +
                   ")\n");
    g_snapshot_req = true;
    if (MarkRelay fn = g_relay.load()) fn(n);
    return n;
}

void PeerMark(int n, const std::string& who, int from_slot, std::int64_t pc_ms) {
    asyncio::Print(MarkLine(n, who + " (slot " + std::to_string(from_slot) + ")", "from the party link", pc_ms,
                            WallClock()) +
                   " (received; run " + RunId() + ")\n");
    g_snapshot_req = true;
}

bool TakeSnapshotRequest() { return g_snapshot_req.exchange(false); }

}  // namespace coop::plog
