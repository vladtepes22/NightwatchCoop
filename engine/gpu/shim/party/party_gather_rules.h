// SPDX-License-Identifier: GPL-3.0-or-later
// I5 "Gather the party" (docs/party/ERSC_PARITY.md I5), the pure rules: who may gather, who may
// come, how each guest comes, the wire and the player texts. Game side: party_gather.cpp.
//
//   host   System > Party > "Gather the party" (party_status RequestGather). Refused while the
//          host's world is not up (loading, dead), a boss fight or a cutscene is on, a party
//          travel / re-join runs, nobody is connected, or within kCooldownS of the last gather.
//          Else EVENT gather {"op":"ask"} to every guest with the host's map and spot.
//   guest  refused (and says why, also to the host) while dead / spectating, loading, in a menu
//          or a game dialog, in a cutscene or a boss fight, or travelling; else the game's own
//          YES / NO dialog ("The host gathers the party. Go to the host?"). YES: by RouteFor -
//          an in-world warp beside the host (the host's map, no load, the B2 spacing), the
//          party's one-load travel into the host's map and then beside the host (another map),
//          or the Rejoin (its own world: the bell, the summon brings it to the host).
//          EVENT gather {"op":"answer"} back to the host, who sums the round up on screen.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace coop::gather {

constexpr const char* kEventName = "gather";
constexpr double kCooldownS = 30.0;       // between two gathers of the host
constexpr double kAnswerTimeoutS = 25.0;  // a guest's question not answered by then: "no answer"
constexpr double kRoundSlackS = 8.0;      // the host waits this much longer for the answers
constexpr float kBesideM = 2.5f;          // a guest closer than this to the host already stands there

/// Why a gather (host) or a guest's coming (guest) is refused now.
enum class Refusal {
    None,
    Off,        // BB_PARTY_GATHER=0
    NotHost,    // only the host gathers
    NoGuests,   // nobody connected
    Loading,    // a loading screen, or no world
    Dead,       // dead (a guest in a boss-fight limbo spectates: Dead too, see `spectating`)
    Spectating, // dead in a boss fight, following a living member
    BossFight,  // a boss health bar is up
    Cutscene,
    Menu,       // a full-screen game menu
    Dialog,     // a game dialog / YES-NO box (another question)
    Travelling, // a party travel / re-join / alone load runs
    Cooldown,   // the host's last gather was less than kCooldownS ago
    NotStanding,// the host climbs a ladder or is in the air (a jump, a fall): no ground to put guests on
    Failed,     // YES, but the move could not start (logged)
    StayHome,   // the guest's "Stay in my world" is on (CL-adeb50)
    GuestsOff,  // CL-a3a9e6: the host has not allowed guests to gather
    OwnWorld,   // CL-a3a9e6: the requesting guest is not in the host's world
    OtherArea,  // CL-a3a9e6: the member stands in another map than the gathering guest (it is not moved there)
};

inline const char* RefusalWord(Refusal r) {
    switch (r) {
    case Refusal::None: return "none";
    case Refusal::Off: return "off";
    case Refusal::NotHost: return "not_host";
    case Refusal::NoGuests: return "no_guests";
    case Refusal::Loading: return "loading";
    case Refusal::Dead: return "dead";
    case Refusal::Spectating: return "spectating";
    case Refusal::BossFight: return "boss";
    case Refusal::Cutscene: return "cutscene";
    case Refusal::Menu: return "menu";
    case Refusal::Dialog: return "dialog";
    case Refusal::Travelling: return "travelling";
    case Refusal::Cooldown: return "cooldown";
    case Refusal::NotStanding: return "not_standing";
    case Refusal::Failed: return "failed";
    case Refusal::StayHome: return "stay_home";
    case Refusal::GuestsOff: return "guests_off";
    case Refusal::OwnWorld: return "own_world";
    case Refusal::OtherArea: return "other_area";
    }
    return "?";
}

inline Refusal RefusalFromWord(const std::string& w) {
    for (int i = 0; i <= int(Refusal::OtherArea); ++i) {
        if (w == RefusalWord(Refusal(i))) return Refusal(i);
    }
    return Refusal::Failed;
}

/// Short reason ("in a menu"), for the host's summary and the guest's notice.
inline const char* RefusalReason(Refusal r) {
    switch (r) {
    case Refusal::None: return "";
    case Refusal::Off: return "gathering is off";
    case Refusal::NotHost: return "only the host gathers";
    case Refusal::NoGuests: return "no guests connected";
    case Refusal::Loading: return "loading";
    case Refusal::Dead: return "dead";
    case Refusal::Spectating: return "spectating";
    case Refusal::BossFight: return "in a boss fight";
    case Refusal::Cutscene: return "in a cutscene";
    case Refusal::Menu: return "in a menu";
    case Refusal::Dialog: return "answering another question";
    case Refusal::Travelling: return "travelling";
    case Refusal::Cooldown: return "too soon after the last gather";
    case Refusal::NotStanding: return "on a ladder or in the air";
    case Refusal::Failed: return "could not move";
    case Refusal::StayHome: return "staying in own world";
    case Refusal::GuestsOff: return "guests may not gather";
    case Refusal::OwnWorld: return "in own world";
    case Refusal::OtherArea: return "in another area";
    }
    return "?";
}

/// Area and block equal (one loaded map; streamed neighbours of one area are other blocks).
constexpr bool MapsAlike(std::uint32_t a, std::uint32_t b) {
    return a != 0 && a != 0xffffffffu && b != 0 && b != 0xffffffffu && (a >> 16) == (b >> 16);
}

// ---- host ----
struct HostView {
    bool enabled = true;          // BB_PARTY_GATHER
    bool host = false;            // this game hosts a running party
    int guests_connected = 0;     // connected members other than the host
    bool world_up = false, loading = false;
    bool alive = true;
    bool boss_fight = false, cutscene = false;
    bool travelling = false;      // a B2 re-join / party travel runs
    bool standing = true;         // on the ground (HostStanding): the spot the guests are put beside
    double now = 0, last_gather_at = -1e9;
};

// A player's report (Old Yharnam, beside Djura's tower): the host gathered while it held still
// half way up a ladder; the guest was put 0.5 m beside it in the open shaft and dropped 3 m to the
// floor (from higher up it is a fall death). The spot a gather puts guests beside must be ground.
constexpr float kStandRiseMps = 3.5f;  // faster than this up or down: a jump or a fall, not stairs
/// The host stands on the ground: not on a ladder (`ladder_state` >= 0: the chr's ladder action
/// state; -1 none, below that unread) and not moving up or down faster than kStandRiseMps.
inline bool HostStanding(int ladder_state, float vertical_mps) {
    return ladder_state < 0 && std::fabs(vertical_mps) <= kStandRiseMps;
}

/// Why the host's gather is refused now (None: ask the guests). `wait_s` (Cooldown): seconds left.
inline Refusal HostRefusal(const HostView& v, int* wait_s = nullptr) {
    if (wait_s) *wait_s = 0;
    if (!v.enabled) return Refusal::Off;
    if (!v.host) return Refusal::NotHost;
    if (!v.world_up || v.loading) return Refusal::Loading;
    if (!v.alive) return Refusal::Dead;
    if (v.boss_fight) return Refusal::BossFight;
    if (v.cutscene) return Refusal::Cutscene;
    if (v.travelling) return Refusal::Travelling;
    if (!v.standing) return Refusal::NotStanding;
    if (v.guests_connected <= 0) return Refusal::NoGuests;
    const double left = kCooldownS - (v.now - v.last_gather_at);
    if (left > 0) {
        if (wait_s) *wait_s = int(std::ceil(left));
        return Refusal::Cooldown;
    }
    return Refusal::None;
}

/// The host's notice for a refused gather ("Gather the party: not during a boss fight").
inline std::string HostRefusalText(Refusal r, int wait_s) {
    switch (r) {
    case Refusal::Cooldown: return "Gather the party: wait " + std::to_string(wait_s) + " s";
    case Refusal::BossFight: return "Gather the party: not during a boss fight";
    case Refusal::Cutscene: return "Gather the party: not during a cutscene";
    case Refusal::Dead: return "Gather the party: not while you are dead";
    case Refusal::Loading: return "Gather the party: not while loading";
    case Refusal::Travelling: return "Gather the party: the party is travelling";
    case Refusal::NotStanding: return "Gather the party: not on a ladder or in the air";
    case Refusal::NoGuests: return "Gather the party: no guests connected";
    case Refusal::NotHost: return "Gather the party: only the host gathers";
    case Refusal::Off: return "Gather the party is off (System > Party rules: World)";
    case Refusal::GuestsOff: return "Gather the party: the host has not allowed guests to gather";
    case Refusal::OwnWorld: return "Gather the party: you are in your own world";
    case Refusal::OtherArea: return "Gather the party: the host is in another area";
    default: return std::string("Gather the party: ") + RefusalReason(r);
    }
}

// ---- guest ----
struct GuestView {
    bool world_up = false, loading = false;
    bool alive = true, spectating = false;
    bool boss_fight = false, cutscene = false;
    bool menu = false, dialog = false;
    bool travelling = false;
    bool stay_home = false;  // "Stay in my world" is on (CL-adeb50)
};

/// Why this guest does not come now (None: ask it). The order is what the player is told.
inline Refusal GuestRefusal(const GuestView& v) {
    if (v.stay_home) return Refusal::StayHome;
    if (!v.world_up || v.loading) return Refusal::Loading;
    if (v.spectating) return Refusal::Spectating;
    if (!v.alive) return Refusal::Dead;
    if (v.cutscene) return Refusal::Cutscene;
    if (v.boss_fight) return Refusal::BossFight;
    if (v.travelling) return Refusal::Travelling;
    if (v.menu) return Refusal::Menu;
    if (v.dialog) return Refusal::Dialog;
    return Refusal::None;
}

/// The guest's notice when it cannot come ("The host gathers the party: not now (in a menu)").
inline std::string GuestRefusalText(Refusal r, const std::string& by = std::string()) {
    return (by.empty() ? std::string("The host") : by) + " gathers the party: not now (" + RefusalReason(r) + ")";
}

// ---- a guest gathers (CL-a3a9e6) ----
// A guest with the host's permission (rule GuestGather) asks the host (EVENT gather {"op":"req"}); the host checks the
// rule and the cooldown (shared with its own gather), asks every other member (itself included) with the same
// question as its own gather ({"op":"ask"} + "by": the gathering guest's slot) and tells the guest ({"op":"reqreply"}).
// A member comes only by an in-world warp beside the guest's spot (the spot the guest asked from, ground the guest
// checked), never a map load and never a computed point: another map is refused (OtherArea). The members keep the
// YES / NO question and the same refusals as for the host's gather.
struct RequesterView {
    bool allowed = false;      // the host's rule GuestGather (synced)
    bool client = false;       // in the host's world (session role client)
    GuestView guest;           // the guest's own state (menu / dialog are ignored: the request comes from the menu)
    bool standing = true;      // on the ground (HostStanding)
    double now = 0, last_request_at = -1e9;
};

/// Why a guest's own press of Gather the party is refused now (None: send the request).
inline Refusal RequestRefusal(const RequesterView& v, int* wait_s = nullptr) {
    if (wait_s) *wait_s = 0;
    if (!v.allowed) return Refusal::GuestsOff;
    if (v.guest.stay_home) return Refusal::StayHome;
    if (!v.client) return Refusal::OwnWorld;
    GuestView g = v.guest;
    g.menu = false;
    g.dialog = false;
    if (const Refusal r = GuestRefusal(g); r != Refusal::None) return r;
    if (!v.standing) return Refusal::NotStanding;
    const double left = kCooldownS - (v.now - v.last_request_at);
    if (left > 0) {
        if (wait_s) *wait_s = int(std::ceil(left));
        return Refusal::Cooldown;
    }
    return Refusal::None;
}

/// Why the host turns a guest's request down (None: ask the members). `round_open`: its last round is still open.
inline Refusal RequestHostRefusal(bool host, bool allowed, bool round_open, double now, double last_gather_at,
                                  int* wait_s = nullptr) {
    if (wait_s) *wait_s = 0;
    if (!host) return Refusal::NotHost;
    if (!allowed) return Refusal::GuestsOff;
    const double left = kCooldownS - (now - last_gather_at);
    if (left > 0 || round_open) {
        if (wait_s) *wait_s = left > 0 ? int(std::ceil(left)) : 5;
        return Refusal::Cooldown;
    }
    return Refusal::None;
}

/// A member of a guest's gather (the host or another guest) that stands in another map than the gathering guest is not
/// moved there: a map load or a computed point is never used.
inline Refusal MemberAreaRefusal(std::uint32_t own_map, std::uint32_t asked_map) {
    return MapsAlike(own_map, asked_map) ? Refusal::None : Refusal::OtherArea;
}

// A player's report: after one NO the host's later gather questions never came back until a restart. The
// game's dialog result stayed unread (-1) although the box was gone, so the guest's question state
// stayed "active" and every later ask was refused as "answering another question".
constexpr double kBoxGoneS = 2.5;       // the box must have been up this long (it opens over a frame or two)
constexpr double kQuestionMaxS = 90.0;  // an unanswered question is closed by then (the YES-late limit is 60 s)
enum class Question { Open, Answered, Dismissed, Expired };
/// The state of a guest's open question. `result`: the dialog's result (-1 none yet), `box_up`: a game
/// dialog is up, `open_s`: seconds since the box opened, `ask_s`: since the ask came. A result is an
/// answer; no result with the box gone, or far too long, ends the question (a NO, so the next one is asked).
inline Question QuestionState(int result, bool box_up, double open_s, double ask_s) {
    if (result != -1) return Question::Answered;
    if (!box_up && open_s >= kBoxGoneS) return Question::Dismissed;
    if (ask_s >= kQuestionMaxS) return Question::Expired;
    return Question::Open;
}

/// How a guest that said YES comes.
enum class Route {
    Here,     // already beside the host (within kBesideM, same map): nothing to do
    InWorld,  // the host's map, its chr here: an in-world warp beside it (no load)
    Travel,   // another map (or the host's chr not here): one load into the host's map, then beside it
    Rejoin,   // not in the host's world (own world): the Rejoin (the bell; the summon brings it over)
};

inline const char* RouteWord(Route r) {
    switch (r) {
    case Route::Here: return "here";
    case Route::InWorld: return "inworld";
    case Route::Travel: return "travel";
    case Route::Rejoin: return "rejoin";
    }
    return "?";
}

struct Place {
    bool client = false;          // summoned into the host's world (session role client)
    std::uint32_t own_map = 0;    // 0xAABBCCDD; 0 / 0xffffffff unknown
    std::uint32_t host_map = 0;
    bool host_chr_here = false;   // the host's chr stands in this world, placed where it reports itself
    float distance = 1e9f;        // to that chr (m)
    bool host_dungeon = false;    // the host stands in a Chalice Dungeon (m29): a map load there has no dungeon
};

constexpr bool MapKnown(std::uint32_t m) { return m != 0 && m != 0xffffffffu; }
/// Area and block equal (one loaded map; streamed neighbours of one area are other blocks).
constexpr bool SameBlock(std::uint32_t a, std::uint32_t b) { return MapKnown(a) && MapKnown(b) && (a >> 16) == (b >> 16); }

// ---- where beside the host (guest, YES) ----
/// The question can stay up for a minute; the host may have climbed a ladder, jumped or dropped
/// since it asked. Its chr now more than kLevelM above or below the spot it asked from (ground, the
/// host checked: Ask::standing): the guest goes beside that spot, not beside the chr in the air.
constexpr float kLevelM = 1.5f;
enum class Spot { Live, Asked };
inline const char* SpotWord(Spot s) { return s == Spot::Live ? "the host's chr" : "the spot the host asked from"; }
inline Spot SpotFor(bool asked_standing, float live_y, float asked_y) {
    return asked_standing && std::fabs(live_y - asked_y) > kLevelM ? Spot::Asked : Spot::Live;
}

// ---- after the in-world warp (guest) ----
/// No ground where the guest was put (a spot in the air, ground not there): it falls. Watched for
/// kLandWatchS; kLandDropM below the spot (not on a ladder) is a fall: the guest goes back to where
/// it stood before the gather (no load) and comes by the travel instead (one loading screen: the
/// vanilla way, correct over seamless).
constexpr double kLandWatchS = 4.0;
constexpr float kLandDropM = 4.0f;
enum class Landing { Wait, Landed, Fell };
inline Landing LandingCheck(double since_s, float dest_y, float now_y, int ladder_state) {
    if (ladder_state >= 0) return Landing::Landed;  // it caught a ladder: standing on it
    if (dest_y - now_y >= kLandDropM) return Landing::Fell;
    return since_s >= kLandWatchS ? Landing::Landed : Landing::Wait;
}

/// A map load into m29 builds the map without the dungeon (the guest is sent back to its own world,
/// "loaded the host's Chalice Dungeon without the dungeon"): in a host's Chalice Dungeon a member is
/// moved in place when it is in the same loaded dungeon, else it comes by the Rejoin (its own world,
/// the bell's summon with the ChannelId); never by a plain map load.
inline Route RouteFor(const Place& p) {
    if (!p.client) return Route::Rejoin;
    if (SameBlock(p.own_map, p.host_map) && p.host_chr_here) {
        return p.distance <= kBesideM ? Route::Here : Route::InWorld;
    }
    return p.host_dungeon ? Route::Rejoin : Route::Travel;
}

// ---- where exactly beside the host (guest) ----
/// A spot beside the host is taken only at the host's height: the member then stands on the same
/// floor, not on a roof or under a ceiling of a stacked layout (player report: above the ceiling of a
/// Chalice Dungeon). The sides are tried in order (SpotForTry: own, mirrored, third), the last
/// resort is the host's own ground position (kHostSpot).
constexpr float kSpotLevelM = 1.5f;     // |spot - host| in height
constexpr float kSpotShoveM = 3.0f;     // a member found further than this from its spot was shoved (a wall, a roof)
constexpr int kSideTries = 3;           // sides tried; try kSideTries = the host's ground position
constexpr int kHostSpot = kSideTries;
constexpr double kGroundSettleS = 1.0;  // the game's safe-ground spot needs this long after a warp
constexpr double kGroundGiveUpS = 2.5;  // no safe ground under it by then: a bad landing
inline bool SpotAcceptable(float host_y, float spot_y) { return std::fabs(spot_y - host_y) <= kSpotLevelM; }
/// The next try after a bad landing (kHostSpot is the last; -1 none).
inline int NextTry(int attempt) { return attempt < kHostSpot ? attempt + 1 : -1; }

enum class Verdict { Wait, Landed, Fell, Bad };
inline const char* BadWhy(float dy, float shove_m, bool ground_known, bool ground) {
    if (dy > kSpotLevelM) return "above the host's floor";
    if (dy < -kSpotLevelM) return "below the host's floor";
    if (shove_m > kSpotShoveM) return "shoved away from the spot";
    if (ground_known && !ground) return "no ground under it";
    return "";
}
/// After the warp to a spot (host floor `ref_y`, the spot at `dest_y`): `now_y` the member's height,
/// `shove_m` its horizontal distance from the spot, `ground_known` the game's safe-ground spot could
/// be read, `ground` it lies within kSpotLevelM of the member and kSpotShoveM of its position.
/// Fell (>= kLandDropM below the spot) is LandingCheck's: back and travel. Bad: next side.
inline Verdict LandingVerdict(double since_s, float ref_y, float dest_y, float now_y, float shove_m, bool ground_known,
                              bool ground, int ladder_state) {
    if (ladder_state >= 0) return Verdict::Landed;
    if (dest_y - now_y >= kLandDropM) return Verdict::Fell;
    if (since_s < 0.3) return Verdict::Wait;
    const float dy = now_y - ref_y;
    if (dy > kSpotLevelM || shove_m > kSpotShoveM) return Verdict::Bad;  // on a roof: at once
    if (since_s < kGroundSettleS) return Verdict::Wait;
    if (std::fabs(dy) > kSpotLevelM) return Verdict::Bad;
    if (!ground_known) return since_s >= kLandWatchS ? Verdict::Landed : Verdict::Wait;
    if (ground) return Verdict::Landed;
    return since_s >= kGroundGiveUpS ? Verdict::Bad : Verdict::Wait;
}

// ---- wire ----
struct Ask {
    std::uint32_t round = 0;
    std::uint32_t map = 0;
    float pos[3] = {0, 0, 0};
    float yaw = 0;
    std::int64_t t = 0;  // the host clock (PartyLink::host_clock_ms) when sent: a replay after a reconnect is old
    bool standing = false;  // the host checked `pos` is ground (HostStanding); older hosts do not send it
    int by = 0;             // CL-a3a9e6: the party slot of the guest that gathers (0: the host gathers)
};
constexpr std::int64_t kAskMaxAgeMs = 15000;

enum class Reply { Yes, No, Refused, NoAnswer };
inline const char* ReplyWord(Reply r) {
    switch (r) {
    case Reply::Yes: return "yes";
    case Reply::No: return "no";
    case Reply::Refused: return "refused";
    case Reply::NoAnswer: return "noanswer";
    }
    return "?";
}

struct Answer {
    std::uint32_t round = 0;
    Reply reply = Reply::No;
    Refusal why = Refusal::None;  // Refused
    Route route = Route::Here;    // Yes
};

/// `op`: "ask" (to a member) or "req" (a guest's request to the host).
inline std::string EncodeAsk(const Ask& a, const char* op = "ask") {
    char b[280];
    std::snprintf(b, sizeof b,
                  "{\"op\":\"%s\",\"round\":%u,\"map\":%u,\"pos\":[%.3f,%.3f,%.3f],\"yaw\":%.4f,\"t\":%lld,\"stand\":%d,"
                  "\"by\":%d}",
                  op, a.round, a.map, double(a.pos[0]), double(a.pos[1]), double(a.pos[2]), double(a.yaw), (long long)a.t,
                  a.standing ? 1 : 0, a.by);
    return b;
}

inline bool DecodeAsk(const std::string& body, Ask* out, const char* op = "ask") {
    const char* s = body.c_str();
    const std::string needle = std::string("\"op\":\"") + op + "\"";
    if (!std::strstr(s, needle.c_str())) return false;
    const char* pr = std::strstr(s, "\"round\":");
    const char* pm = std::strstr(s, "\"map\":");
    const char* pp = std::strstr(s, "\"pos\":[");
    const char* py = std::strstr(s, "\"yaw\":");
    Ask a;
    if (!pr || !pm || !pp || !py || std::sscanf(pr, "\"round\":%u", &a.round) != 1 ||
        std::sscanf(pm, "\"map\":%u", &a.map) != 1 ||
        std::sscanf(pp, "\"pos\":[%f,%f,%f]", &a.pos[0], &a.pos[1], &a.pos[2]) != 3 ||
        std::sscanf(py, "\"yaw\":%f", &a.yaw) != 1) {
        return false;
    }
    for (float v : {a.pos[0], a.pos[1], a.pos[2], a.yaw}) {
        if (!std::isfinite(v) || std::fabs(v) > 1e5f) return false;
    }
    if (!MapKnown(a.map) || a.round == 0) return false;
    if (const char* pt = std::strstr(s, "\"t\":")) {
        long long t = 0;
        if (std::sscanf(pt, "\"t\":%lld", &t) == 1) a.t = t;
    }
    if (const char* ps = std::strstr(s, "\"stand\":")) {
        int st = 0;
        if (std::sscanf(ps, "\"stand\":%d", &st) == 1) a.standing = st == 1;
    }
    if (const char* pb = std::strstr(s, "\"by\":")) {
        int by = 0;
        if (std::sscanf(pb, "\"by\":%d", &by) == 1 && by >= 0 && by < 16) a.by = by;
    }
    *out = a;
    return true;
}

inline std::string EncodeAnswer(const Answer& a) {
    char b[128];
    std::snprintf(b, sizeof b, "{\"op\":\"answer\",\"round\":%u,\"reply\":\"%s\",\"why\":\"%s\",\"route\":\"%s\"}", a.round,
                  ReplyWord(a.reply), RefusalWord(a.why), RouteWord(a.route));
    return b;
}

/// The host's verdict on a guest's request (EVENT gather {"op":"reqreply"}).
struct RequestReply {
    std::uint32_t round = 0;
    bool ok = false;
    Refusal why = Refusal::None;
    int asked = 0;  // ok: how many members were asked
};

inline std::string EncodeReply(const RequestReply& r) {
    char b[128];
    std::snprintf(b, sizeof b, "{\"op\":\"reqreply\",\"round\":%u,\"ok\":%d,\"why\":\"%s\",\"asked\":%d}", r.round,
                  r.ok ? 1 : 0, RefusalWord(r.why), r.asked);
    return b;
}

inline std::string Word(const char* s, const char* key) {
    const char* p = std::strstr(s, key);
    if (!p) return {};
    p += std::strlen(key);
    const char* e = std::strchr(p, '"');
    return e && e - p < 32 ? std::string(p, e) : std::string();
}

inline bool DecodeReply(const std::string& body, RequestReply* out) {
    const char* s = body.c_str();
    if (!std::strstr(s, "\"op\":\"reqreply\"")) return false;
    const char* pr = std::strstr(s, "\"round\":");
    const char* po = std::strstr(s, "\"ok\":");
    RequestReply r;
    int ok = 0;
    if (!pr || !po || std::sscanf(pr, "\"round\":%u", &r.round) != 1 || std::sscanf(po, "\"ok\":%d", &ok) != 1) return false;
    r.ok = ok == 1;
    if (const char* pa = std::strstr(s, "\"asked\":")) std::sscanf(pa, "\"asked\":%d", &r.asked);
    const std::string why = Word(s, "\"why\":\"");
    r.why = why.empty() ? Refusal::None : RefusalFromWord(why);
    *out = r;
    return true;
}

inline bool DecodeAnswer(const std::string& body, Answer* out) {
    const char* s = body.c_str();
    if (!std::strstr(s, "\"op\":\"answer\"")) return false;
    const char* pr = std::strstr(s, "\"round\":");
    Answer a;
    if (!pr || std::sscanf(pr, "\"round\":%u", &a.round) != 1 || a.round == 0) return false;
    const std::string reply = Word(s, "\"reply\":\""), why = Word(s, "\"why\":\""), route = Word(s, "\"route\":\"");
    if (reply == "yes") a.reply = Reply::Yes;
    else if (reply == "no") a.reply = Reply::No;
    else if (reply == "refused") a.reply = Reply::Refused;
    else if (reply == "noanswer") a.reply = Reply::NoAnswer;
    else return false;
    a.why = why.empty() ? Refusal::None : RefusalFromWord(why);
    a.route = route == "inworld" ? Route::InWorld : route == "travel" ? Route::Travel : route == "rejoin" ? Route::Rejoin : Route::Here;
    *out = a;
    return true;
}

// ---- the host's summary of a round ----
struct MemberAnswer {
    std::string name;
    bool answered = false;
    Answer answer;
};

/// "Gather: Ann, Bob coming; Cid said no; Dee not now (in a menu)" (names as the roster has them).
inline std::string Summary(const std::vector<MemberAnswer>& m) {
    std::string coming, no, later;
    auto add = [](std::string& list, const std::string& item) { list += (list.empty() ? "" : ", ") + item; };
    for (const MemberAnswer& a : m) {
        if (!a.answered || a.answer.reply == Reply::NoAnswer) {
            add(later, a.name + " (no answer)");
        } else if (a.answer.reply == Reply::Yes) {
            add(coming, a.name);
        } else if (a.answer.reply == Reply::No) {
            add(no, a.name);
        } else {
            add(later, a.name + " (" + RefusalReason(a.answer.why) + ")");
        }
    }
    std::string out = "Gather:";
    if (!coming.empty()) out += " " + coming + " coming;";
    if (!no.empty()) out += " " + no + " said no;";
    if (!later.empty()) out += " not now: " + later + ";";
    if (out.back() == ';') out.pop_back();
    if (out == "Gather:") out += " nobody answered";
    return out;
}

/// The round is over: every guest asked has answered, or the time is up.
inline bool RoundOver(const std::vector<MemberAnswer>& m, double since_s) {
    if (since_s >= kAnswerTimeoutS + kRoundSlackS) return true;
    for (const MemberAnswer& a : m) {
        if (!a.answered) return false;
    }
    return true;
}

} // namespace coop::gather
