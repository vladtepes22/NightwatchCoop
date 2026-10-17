// SPDX-License-Identifier: GPL-3.0-or-later
// Party HUD (Seamless-Co-op style): every member sees its companions' names and health at a
// glance, greyed while they are not in this player's world, with the distance and a direction
// arrow while they are on the same map (and, with the game camera, a name tag above them).
//
//   game (main thread, party_hud_game.cpp, ~10 Hz): reads this player's HP / stamina / map /
//     position and sends them as PartyLink VITALS (party_link.h MemberVitals, ~4 Hz); reads the
//     remote players the game has in this world (their PlayerIns: exact and current); composes
//     the board (Compose: pure) and publishes it.
//   overlay (present thread, bbport_overlay.cpp PartyHud): Read() into its own Board (a fixed
//     size copy under a mutex: no allocation), draws the panel and the tags.
//
// Data per companion: from its PlayerIns when the game has it here (same world), else from the
// VITALS its own game sent (through the host), else only the roster (state, map).
// Env: BB_PARTY_HUD_LOG=1 logs the reads every 5 s ("Party HUD: ..."); BB_PARTY_HUD_TAGS=0 turns
// the floating name tags off. Setting: bbport.ini party_hud (default on; menus: Party members
// page "Party HUD", overlay Party tab, launcher Party tab).
#pragma once

#include "party_link.h"

#include <cstdint>
#include <string>

namespace party::hud {

constexpr int kMaxCompanions = kMaxVitals - 1;
constexpr int kNameBytes = 17;  // 16 + NUL (valid_member_name)

struct Companion {
    char name[kNameBytes] = {};
    int slot = -1;
    MemberState state = MemberState::Title;
    bool connected = false;
    bool same_world = false;  // in this player's world (summoned / the host we are summoned to)
    bool local_read = false;  // HP / position read from its PlayerIns here (not from VITALS)
    bool have_hp = false;
    bool alive = true;
    int hp = 0, max_hp = 0;
    bool have_stamina = false;
    int stamina = 0, max_stamina = 0;
    std::uint32_t map_id = 0;  // its map (VITALS, else the roster); 0 unknown
    bool have_pos = false;
    bool same_map = false;     // in this world, both on one map with positions: distance / direction mean
                               // something (never across worlds: own worlds on one map are not "0.0 m")
    float pos[3] = {0, 0, 0};
    std::uint32_t age_ms = 0;  // VITALS age (0 for a local read)
    bool local_dead = false;   // its chr here reads HP 0 (a corpse left behind, a held death)
    int level = 0;             // VITALS extras (0 = unknown): the overhead display (E2)
    int deaths = 0;
    int rot = 0;
    std::uint32_t ping_ms = 0; // the link's round trip to this member (E2 "name and ping"; 0 unknown):
                               // the host's measure; a guest's view of the host shows its own link's
    std::uint64_t chr = 0;     // its PlayerIns here (local_read): the overlay re-reads its position
                               // every frame (coop::PartyHudChrPos), the board's is the 10 Hz tick's
};

struct Board {
    bool active = false;  // party mode with a link: the HUD may show
    int count = 0;        // companions (never this player)
    Companion c[kMaxCompanions];
    bool me_have_pos = false;
    float me_pos[3] = {0, 0, 0};
    float me_yaw = 0;     // facing (forward = (sin, 0, cos)): the direction fallback without a camera
    std::uint32_t me_map = 0;
    std::uint64_t seq = 0;  // +1 per Publish
    std::int64_t published_ms = 0;  // steady clock (ms) of the Publish: the game tick's freshness
};

/// A remote player's PlayerIns as read in this world, matched to a member by name.
struct LocalChr {
    char name[kNameBytes] = {};
    bool have_hp = false;
    int hp = 0, max_hp = 0;
    int stamina = 0, max_stamina = 0;
    bool have_pos = false;
    float pos[3] = {0, 0, 0};
    std::uint64_t chr = 0;  // the PlayerIns (guest address)
};

struct ComposeIn {
    int local_slot = -1;
    MemberState local_state = MemberState::Title;
    std::uint32_t local_map = 0;
    bool me_have_pos = false;
    float me_pos[3] = {0, 0, 0};
    float me_yaw = 0;
    const RosterEntry* roster = nullptr;
    int roster_n = 0;
    const MemberVitals* vitals = nullptr;
    int vitals_n = 0;
    const LocalChr* chrs = nullptr;
    int chrs_n = 0;
};

/// Whether a member is in this player's world: summoned guests share the host's world; a guest
/// at home (or loading, at the title, in its prologue) shares nobody's.
bool SameWorld(int local_slot, MemberState local_state, int other_slot, MemberState other_state);

/// Playtest 1 ("stale remote chr?" 32 times): a remote chr that no member's report is near
/// (`far`, > 8 m) is reported as stale only when it is built (`max_hp` > 0: a chr the game is
/// still creating sits at (0, 0, 0) with HP 0/0) and has stayed far for kStaleChrHoldS: a warp or
/// a re-creation moves the chr and the report within a second or two (every playtest line was one).
constexpr double kStaleChrHoldS = 5.0;
bool StaleChrDue(bool far, int max_hp, double far_for_s);
/// The companions (by slot, this player left out) from the roster, the VITALS and the local reads.
void Compose(const ComposeIn& in, Board* out);

/// Publish (game): the overlay's next Read sees it. Read: the latest (inactive before the first).
void Publish(const Board& b);
void Read(Board* out);
/// Cheap (atomic): something to show (active with at least one companion, no cutscene playing).
bool Shown();
/// A cutscene (remo) plays on this game: the panel and the name tags hide (party_story's tick).
void SetCutscene(bool playing);

/// The game's own full-screen menu (inventory, equipment, level up, shops, the system menu's
/// pages...) is up: the game's IsFullscreenMenu (FUN_0176bb70, the debug line
/// "IsFullscreenMenu:%d"): any of MenuMan (*0x5562878) +0x3c, +0x40, +0x80, +0x88, +0x98, +0x9c,
/// +0x120, +0x138, +0x13c non-zero, overridden by the debug byte *0x553d6b8 +8 (-1: none, 1: open,
/// 0: closed, anything else: as the fields say). Pure; `fields` in that order.
constexpr int kFullscreenMenuFields = 9;
constexpr unsigned kFullscreenMenuOffsets[kFullscreenMenuFields] = {0x3c, 0x40, 0x88, 0x80, 0x120,
                                                                    0x13c, 0x98, 0x138, 0x9c};
bool FullscreenMenuOpen(const int fields[kFullscreenMenuFields], signed char force);
/// The in-game menus (the top menu, inventory, equipment, stats, the lamp's warp menu...) are the
/// menu flow's (*0x5562870, used while the "MenuMan.NewSystem" byte 0x5562882 is set): its +0xa88
/// is the open menu (0 = none: its update then polls the buttons that open one, FUN_01baf8c0 ->
/// "MenuMan.IngameTop"; bbhost menu_pointer.cpp kFlowMenu). The game's own menu-bits function
/// 0x176b7e0 reports it as 0x800008 (live: 0 at the lamp before, 0x800008 once the warp menu was
/// up, with every IsFullscreenMenu field 0; the inventory left those 0 too). Pure: either counts.
bool GameMenuUp(bool flow_menu_open, const int fields[kFullscreenMenuFields], signed char force);
/// The generic YES/NO box (MenuMan +0x78, e.g. the lamp's "Return to the Hunter's Dream?") opens
/// through the menu flow too (live 2026-10-14: +0xa88 set, +0x78 1 about 35 ms later), and only the
/// name tags should step aside for it. A flow menu that a box joins within kBoxGraceS of opening is
/// that box (not full-screen; a new look once the box closes and the flow menu stays); one without a
/// box by then is a full-screen menu for as long as it stays open (a box inside the inventory keeps
/// it full-screen). Pure; `t` in seconds, any monotonic clock.
class FlowMenuKind {
public:
    static constexpr double kBoxGraceS = 0.25;
    /// True while the open flow menu counts as a full-screen menu.
    bool Update(bool flow_open, bool dialog, double t);
    bool box() const { return state_ == State::Box; }
private:
    enum class State { Closed, Pending, Box, Full };
    State state_ = State::Closed;
    double since_ = 0.0;
};
/// The game's area-name banner ("Cathedral Ward", bottom right, right after a loading screen and
/// when the player walks into another map block): the performance HUD in the bottom-right corner
/// lifts above it for kShowS (0.6.1 regression run: the HUD covered the banner). The banner has no
/// readable flag here (FrpgMenuDlgTE_MapName, built by 0x17788c0, is not kept in a known slot), so
/// the times it shows are its triggers: an arrival in the world (the local PlayerIns comes back
/// after a load, or a new one) and a change of the map block. Pure; `t` in seconds, any monotonic
/// clock; `map` 0 / 0xffffffff = unknown.
class AreaBannerWatch {
public:
    static constexpr double kShowS = 10.0;
    /// True while a banner may be up.
    bool Update(bool in_world, std::uint64_t player, std::uint32_t map, double t);
private:
    std::uint64_t player_ = 0;
    std::uint32_t map_ = 0xffffffffu;
    double since_ = -1e9;
};
/// While the banner may be up, the bottom edge of a bottom-right performance HUD at most this
/// fraction of the screen height from the top (the banner's text and rule span ~0.82 .. 0.91 of
/// the height, from ~0.67 of the width to the right edge, at every resolution: the game scales it).
constexpr float kAreaBannerTop = 0.80f;
/// The bottom edge (pixels from the top) of a bottom-right performance HUD: `normal` (the corner)
/// unless the banner may be up, then no lower than kAreaBannerTop * `height`.
float PerfHudBottom(float normal, float height, bool banner);

/// Set by party_hud_game's tick (game thread), read by the overlay: while the game's full-screen
/// menu is up the party panel and the name tags hide and the party notices / NPC captions are held
/// (not drawn, their time stopped) so they never draw crisply over the game's own menu.
void SetGameMenu(bool open);
bool GameMenu();
/// The game's generic dialog box (MenuMan +0x78, e.g. "Return to the Hunter's Dream? YES / NO",
/// which dims the screen) is up: only the name tags hide (the panel and the notices stay: the
/// party's travel question is such a box).
void SetGameDialog(bool open);
bool GameDialog();

// ---- geometry (overlay; pure) ----
/// The distance (3D, metres) from `a` to `b`.
float Distance(const float a[3], const float b[3]);
/// The direction to `to` from `from`, radians in [-pi, pi]: 0 straight ahead along the forward
/// axis (fx, fz), positive to the right (rx, rz).
float Bearing(const float from[3], const float to[3], float fx, float fz, float rx, float rz);
/// World -> screen with the game camera (view: world to view, 3x4 rows; proj: x scale, y scale
/// as the motion shader uses them, ndc +y down): false behind the camera / too close to it.
/// u, v in 0..1 across the scene (outside when off screen).
bool Project(const float view[12], const float proj[4], const float p[3], float* u, float* v, float* depth);

/// World point -> display pixels: the reusable world-space marker projection (E2 name tags; I2
/// pings and through-wall markers reuse it). `w` x `h`: the display size in pixels. `visible`: in
/// front of the camera and inside the display minus `margin` (a fraction of each side); x / y are
/// set whenever the point is in front (an off-screen marker can be clamped to the edge by the caller).
struct ScreenPoint {
    bool in_front = false, visible = false;
    float x = 0, y = 0, depth = 0;  // pixels, metres from the camera
};
ScreenPoint WorldToScreen(const float view[12], const float proj[4], const float p[3], float w, float h,
                          float margin = 0.02f);
/// A party member's colour by slot (0 host .. 3), RGB 0..1: the same everywhere a member is drawn
/// (tags now, I2 pings later). Slot outside 0..3: the game's cream.
void MemberColour(int slot, float rgb[3]);

/// Overhead tags (E2): a tag shows only for a member whose chr the game has in this world right
/// now (read this tick, not a VITALS report), alive, on our map, with the board fresh (the game's
/// tick published it within kTagFreshMs; a load or a stalled tick stops publishing). Otherwise
/// `why` names the reason ("stale", "dead", "not here", ...).
constexpr std::int64_t kTagFreshMs = 500;
bool TagVisible(const Companion& c, std::int64_t board_age_ms, const char** why);
/// The game's own phantom plate (FrpgMenuDlgFloatingPCGauge: role line, name, thin HP bar) is
/// turned off in party mode (party_hud_game.cpp: its per-entry visible flag, written by
/// FUN_0176cfa0 into MenuMan+0x900+i*0x90+0xc, is stored as 0) and ours, drawn in its style,
/// replaces it at every distance (the game hid its plate behind any occluder, so members vanished).
/// The game plate's HP bar sits this high above the chr's position (both screenshots at 7 and 16
/// m: 1.82 m); its name text stacks above the bar in pixels. Ours anchors the same way.
constexpr float kTagBarHeightM = 1.82f;
/// Our name plate, laid out as the game's phantom plate (FrpgMenuDlgFloatingPCGauge), tuned
/// pixel by pixel against it live 2026-10-12 (both drawn side by side, BB_PARTY_HUD_TUNE_FILE, one
/// run at 1, 11.5 and 21 m, an 833x469 window): the game's plate is a FIXED screen size - a 2 px
/// (0.0043 h) bar 0.0996 of the display width centred on the anchor, its top on the anchor (1.82 m
/// above the chr), 1 px light end caps ON the bar's ends from 2 px above it to 1 px below (5 px:
/// 0.0107 h), no dark rim; the name line (cap height 8 px: font 0.021 h) left-aligned 1 px in from
/// the left cap, its baseline 6 px (0.0118 h) above the bar, blue (49,155,199) with a dark
/// shadow / outline; the bar red (95,22,32) over burnt orange (102,36,24). No role line.
constexpr float kPlateFontH = 0.021f, kPlateBarW = 0.0996f, kPlateBarH = 0.0043f, kPlateCapH = 0.0107f,
                kPlateCapRise = 0.0043f, kPlateNameInset = 0.0021f, kPlateBaselineGap = 0.0118f,
                kPlateCapToFont = 0.77f;  // Libre Baskerville: cap height / font size
/// Scale of our plate over the game's measured size by camera depth (metres from the camera):
/// 1 from kPlateFarDepth on, growing as 1/depth nearer, at most kPlateMaxScale; kPlateMaxScale 1
/// keeps the game's fixed size (a larger value would grow the plate near the camera).
constexpr float kPlateFarDepth = 10.0f, kPlateMaxScale = 1.0f;  // fixed size, like the game's plate (user, 2026-10-12)
float PlateScale(float depth);
/// The plate's tunable parameters (sizes: fractions of the display height, bar_w of the width;
/// colours RGB 0..1). Defaults: the k* constants. Test aid BB_PARTY_HUD_TUNE_FILE=<path>: the
/// overlay re-reads key=value lines from that file every 0.5 s (PlateTunePoll; keys = the field
/// names, colours as r,g,b in 0..255), so a live run can tune ours against the game's plate.
struct PlateParams {
    float font_h = kPlateFontH, bar_w = kPlateBarW, bar_h = kPlateBarH;
    float cap_w = 0.0021f, cap_h = kPlateCapH, cap_rise = kPlateCapRise;  // cap top kPlateCapRise above the bar
    float cap_dx = 0.0f;                    // caps' inner offset from the bar ends (h)
    float name_inset = kPlateNameInset, baseline_gap = kPlateBaselineGap;
    float outline = 0.0021f, rim = 0.0f, glow = 0.45f;  // outline (h), bar rim px, cap glow alpha
    float name_rgb[3] = {0.19f, 0.61f, 0.78f};
    float bar_top[3] = {95 / 255.0f, 22 / 255.0f, 32 / 255.0f}, bar_bottom[3] = {102 / 255.0f, 36 / 255.0f, 24 / 255.0f};
    float cap_rgb[3] = {0.91f, 0.95f, 0.97f};
};
/// Applies "key=value" lines (unknown keys ignored, '#' comments) to `p`; returns keys applied.
int ParsePlateTune(const char* text, PlateParams* p);
/// The parameters in use (defaults, or the tune file's last read).
const PlateParams& CurrentPlateParams();
/// Overlay, once a frame: re-reads BB_PARTY_HUD_TUNE_FILE (when set) every 0.5 s.
void PlateTunePoll();
struct PlateLayout {
    float font = 0;                        // font size, px
    float bar_x0 = 0, bar_y0 = 0, bar_x1 = 0, bar_y1 = 0;  // the bar
    float cap_w = 0, cap_y0 = 0, cap_y1 = 0;  // end caps, cap_w wide, cap_y0..cap_y1
    float cap_l = 0, cap_r = 0;            // left x of the left / right cap
    float name_x = 0, name_baseline = 0;   // the name's left edge and baseline
    float outline = 0;                     // shadow / outline offset, px
};
/// The plate around the projected anchor (ax, ay) on a display w x h at camera depth `depth`.
PlateLayout LayoutPlate(float ax, float ay, float w, float h, float depth, const PlateParams& p = PlateParams{});
/// Tag layout in pixels from the projected anchor (the bar's bottom edge, y down): the bar
/// (bar_h tall, when drawn) ends at anchor_y, the name's bottom sits `gap` above the bar (or at
/// the anchor without a bar). Returns the name's top-left y for a name `text_h` tall.
float TagTextTop(float anchor_y, float text_h, bool with_bar, float bar_h, float gap);
/// Steady clock in ms (the board's timestamps).
std::int64_t NowMs();

/// Tests: back to the start.
void ResetForTest();

}  // namespace party::hud

namespace coop {
struct GameSnapshot;  // game_state.h
/// The game side (party_hud_game.cpp): the director, once a frame on the main thread (works
/// every 100 ms): this player's VITALS out, the remote PlayerIns read, the board published.
/// `link` null: the board goes inactive.
void PartyHudTick(const GameSnapshot& s, party::PartyLink* link, party::MemberState local_state);
/// Any thread (the overlay, once a frame): the current physics position of the PlayerIns `chr`
/// (the one the game draws, read now), when it is still a live chr within 10 m of `near` (the
/// board's position from the last tick); false otherwise (freed, disabled, moved far).
bool PartyHudChrPos(std::uint64_t chr, const float near[3], float out[3]);
/// Any thread: the party member's name the last HUD tick matched to the remote PlayerIns `chr`
/// (empty when none; C5 spectate notice).
std::string PartyHudChrName(std::uint64_t chr);
/// The overlay (present thread), once a frame, for the performance HUD: whether the game's
/// full-screen menu is up (the party tick's GameMenu() while it runs, else the same read here:
/// solo play has no party tick) and whether the area-name banner may be up (AreaBannerWatch).
/// Reads every 100 ms at most; false / false outside the world or before the image is known.
void PerfHudGameState(bool* game_menu, bool* area_banner);
}  // namespace coop
