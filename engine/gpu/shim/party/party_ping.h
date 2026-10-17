// SPDX-License-Identifier: GPL-3.0-or-later
// Pings (I2, docs/party/ERSC_PARITY.md): a party member marks the spot it is looking at and every
// member sees the marker through walls, with its distance in metres, in the pinging player's colour
// (party_hud.h MemberColour), for kLifeMs. One active ping per player (a new one replaces it).
// Optional: every party member on this map drawn through walls as a small marker with its distance
// ("Show party through walls"; off by default).
//
//   input    hold the touchpad (keyboard Tab) and press R3 (keyboard Q / C), or the V key. The
//            touchpad press is held back from the game until it is released (then the game gets
//            a normal touchpad click: the Gestures menu opens on release) or R3 comes (a ping: the
//            game sees neither). On pads without a touch surface (Xbox): hold Back and press R3.
//            src/runtime_pad.c, only in a party with pings on.
//   aim      the scene depth along the screen's centre column, read back from the GPU once per
//            ping (vk_camera_motion.cpp depth probe, bbport_camera.h Probe): from the screen centre
//            upwards, the first surface that is not the player itself (PickAim). Sky, no depth or
//            no answer within kProbeWaitMs: a point kFallbackDistance along the camera's view
//            from the player's chest (FallbackAim).
//   wire     EVENT "map_ping" (PingEvent JSON): a guest's to the host, the host's own and every
//            guest's relayed to every other member with the slot and name from the host's roster
//            (never trusted from a guest), at most kRelayBurst per kRelayWindowS per member.
//   draw     the overlay (bbport_overlay.cpp PartyPings): every live marker on this player's map
//            from a member in this player's world, projected with the game camera
//            (party_hud.h WorldToScreen), clamped to the screen edge with an arrow when off
//            screen; hidden under the game's menus, dialogs, cutscenes and loading screens.
//
// Settings: bbport.ini party_pings (default on), party_through_walls (default off); System >
// Pings, the overlay's Party tab, the launcher's Party tab. Env: BB_PARTY_PINGS=0/1 over the
// setting; BB_PARTY_PING_LOG=1 logs the aim samples. Test aid: BB_PAD_FILE "ping" line (tools/mp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace party::ping {

constexpr const char* kEventName = "map_ping";
constexpr std::int64_t kLifeMs = 10000;     // a ping shows this long
constexpr std::int64_t kFadeMs = 1500;      // ... fading out over its last kFadeMs
constexpr std::int64_t kCooldownMs = 750;   // between two pings of this player
constexpr int kRelayBurst = 4;              // host: a member's pings relayed per window
constexpr double kRelayWindowS = 6.0;
constexpr float kMaxRange = 250.0f;         // m from the pinging player
constexpr float kFallbackDistance = 15.0f;  // m along the view when the depth gives nothing
constexpr std::int64_t kProbeWaitMs = 300;  // the GPU depth answer, at most this late
constexpr int kMaxSlots = 8;
constexpr int kNameBytes = 17;

/// EVENT map_ping. slot / who: set by the host from its roster. t: the host clock (ms,
/// PartyLink::host_clock_ms) when sent: a ping replayed after a reconnect is dropped as old.
struct PingEvent {
    std::uint32_t id = 0;   // the sender's ping counter (logs)
    std::uint32_t map = 0;  // 0xAABBCCDD
    float pos[3] = {0, 0, 0};
    bool hit = false;       // on a surface (depth) or the fallback point
    bool clear = false;     // the sender took its ping down
    std::int64_t t = 0;
    int slot = -1;
    std::string who;
};
std::string ToJson(const PingEvent& e);
/// Parses and validates: finite position within 100 km, a known map (not 0 / 0xffffffff unless
/// clear), slot -1..kMaxSlots-1, name valid_member_name style (<= 16 of [A-Za-z0-9_-]).
bool FromJson(const std::string& text, PingEvent* out, std::string* error = nullptr);

/// Host: a member's pings per window (token bucket: kRelayBurst, refilled over kRelayWindowS).
class RelayRate {
public:
    bool Take(int slot, double now_s);
private:
    double tokens_[kMaxSlots] = {kRelayBurst, kRelayBurst, kRelayBurst, kRelayBurst,
                                 kRelayBurst, kRelayBurst, kRelayBurst, kRelayBurst};
    double last_[kMaxSlots] = {};
};

/// A ping as drawn: one per slot.
struct Marker {
    bool live = false;
    bool mine = false;          // this player's own ping
    int slot = -1;
    char name[kNameBytes] = {};
    std::uint32_t map = 0;
    float pos[3] = {0, 0, 0};
    bool hit = false;
    std::int64_t until_ms = 0;  // steady clock (party_hud NowMs)
    std::uint32_t id = 0;
};
/// The board (any thread): Place replaces the slot's ping; Read copies the live ones (expired
/// ones are dropped) into `out` (kMaxSlots entries), returns how many.
void Place(const Marker& m);
void Clear(int slot);
void ClearAll();
int Read(Marker* out, std::int64_t now_ms);
/// Opacity of a marker with `left_ms` to live: 1, fading to 0 over the last kFadeMs.
float Alpha(std::int64_t left_ms);
/// How long a ping sent at host time `t` still shows at host time `now` (0: expired / bogus).
std::int64_t LifeLeft(std::int64_t t, std::int64_t now);

// ---- aim (pure) ----
/// The depth probe's world points along the screen's centre column, top row first.
struct Sample {
    bool hit = false;
    float p[3] = {0, 0, 0};
};
/// The spot the player looks at: walking from the sample at the screen centre (`centre`) up the
/// column, the first surface that is past the player (farther from the camera than the player's
/// chest, more than 1.6 m from it) and within kMaxRange of it. False: nothing (sky, too far, only
/// the player itself).
bool PickAim(const float cam[3], const float player[3], const Sample* s, int n, int centre, float out[3]);
/// No surface: kFallbackDistance along the camera's forward axis from the player's chest.
void FallbackAim(const float fwd[3], const float player[3], float out[3]);

// ---- screen (pure) ----
/// A marker on the screen: inside the display less `inset` pixels, or clamped to that edge in the
/// marker's direction (`angle`: radians, screen space, 0 = right, +pi/2 = down) when it is off
/// screen or behind the camera. view / proj as party_hud.h Project.
struct EdgePoint {
    bool on_screen = false;
    float x = 0, y = 0, angle = 0;
    float depth = 0;  // metres along the view (<= 0 behind)
};
EdgePoint ToScreenOrEdge(const float view[12], const float proj[4], const float p[3], float w, float h, float inset);
/// "12 m" / "3.4 m" (one decimal under 10 m).
void FormatDistance(float metres, char* out, std::size_t n);

/// Statsaudit finding 1: this guest's own bloodstain in the host's world (a party-side stain the
/// overlay marks; paid when the guest walks onto it, party_b2.cpp GuestStainTick). One per game.
struct StainMark {
    bool live = false;
    std::uint32_t map = 0;  // 0xAABBCCDD
    float pos[3] = {0, 0, 0};
    int echoes = 0;
};
void SetStain(const StainMark& m);
bool GetStain(StainMark* out);  // false: none to draw

/// Tests: back to the start.
void ResetForTest();

}  // namespace party::ping

namespace party {
class PartyLink;
}
namespace coop {
struct GameSnapshot;
namespace ping {
/// Pings on (the setting, BB_PARTY_PINGS over it) and a party running: the combo is ours.
bool Enabled();
/// Input (any thread): the ping button was pressed.
void Request(const char* source);
/// Link thread: EVENT map_ping from `slot` (host: a guest's; guest: the host's / relayed).
void OnEvent(int slot, const std::string& body);
/// Main thread, once a frame (the director): input -> aim -> send; incoming events -> the board.
/// `in_world`: this game shows a world (not loading, the title, a menu screen).
void Tick(const GameSnapshot& s, party::PartyLink* link, bool in_world);
}  // namespace ping
}  // namespace coop
