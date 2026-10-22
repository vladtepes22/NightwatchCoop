// SPDX-License-Identifier: GPL-3.0-or-later
// Party HUD board: composition (pure) and the game -> overlay hand-over (party_hud.h).
#include "party_hud.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>

namespace party::hud {

namespace {

std::mutex g_mu;
Board g_board;  // g_mu
std::atomic<bool> g_shown{false};
std::atomic<bool> g_cutscene{false};  // a remo plays: the panel and tags stay hidden
std::atomic<bool> g_game_dialog{false};  // the game's generic dialog box (MenuMan +0x78)
std::atomic<bool> g_game_menu{false};  // the game's full-screen menu is up (party_hud_game's tick)

void CopyName(char (&dst)[kNameBytes], const std::string& src) {
    const std::size_t n = std::min<std::size_t>(src.size(), kNameBytes - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = 0;
}

bool KnownMap(std::uint32_t m) {
    return m != 0 && m != 0xffffffffu;
}

// In a world at all (not loading, not at the title).
bool InAWorld(MemberState s) {
    return s == MemberState::Home || s == MemberState::Joining || s == MemberState::InHostWorld ||
           s == MemberState::Prologue || s == MemberState::Dead;
}

}  // namespace

bool StaleChrDue(bool far, int max_hp, double far_for_s) {
    return far && max_hp > 0 && far_for_s >= kStaleChrHoldS;
}

bool SameWorld(int local_slot, MemberState local_state, int other_slot, MemberState other_state) {
    if (local_slot < 0 || other_slot < 0 || local_slot == other_slot) return false;
    if (!InAWorld(local_state) || !InAWorld(other_state)) return false;
    if (local_slot == kHostSlot)  // the host: its summoned guests
        return other_state == MemberState::InHostWorld && local_state != MemberState::Prologue;
    if (local_state != MemberState::InHostWorld) return false;  // a guest at home is alone
    // A summoned guest: the host (whose world it is) and the other summoned guests.
    return other_slot == kHostSlot || other_state == MemberState::InHostWorld;
}

void Compose(const ComposeIn& in, Board* out) {
    Board b;
    b.active = true;
    b.me_have_pos = in.me_have_pos;
    std::memcpy(b.me_pos, in.me_pos, sizeof b.me_pos);
    b.me_yaw = in.me_yaw;
    b.me_map = in.local_map;
    b.me_chr = in.me_have_pos ? in.me_chr : 0;
    // Companions in slot order.
    for (int slot = 0; slot < kMaxVitals; ++slot) {
        if (slot == in.local_slot) continue;
        const RosterEntry* r = nullptr;
        for (int i = 0; i < in.roster_n; ++i)
            if (in.roster[i].slot == slot) r = &in.roster[i];
        if (!r || b.count >= kMaxCompanions) continue;
        Companion& c = b.c[b.count++];
        CopyName(c.name, r->name);
        c.slot = slot;
        c.state = r->state;
        c.connected = r->connected;
        c.map_id = r->map_id;
        c.ping_ms = r->ping_ms;
        if (slot == kHostSlot && in.local_slot != kHostSlot) {
            // A guest's view of the host: the host measured this guest's link (its own entry).
            for (int i = 0; i < in.roster_n; ++i)
                if (in.roster[i].slot == in.local_slot) c.ping_ms = in.roster[i].ping_ms;
        }
        c.same_world = r->connected && SameWorld(in.local_slot, in.local_state, slot, r->state);
        for (int i = 0; i < in.vitals_n; ++i) {
            const MemberVitals& v = in.vitals[i];
            if (v.slot != slot) continue;
            c.age_ms = v.age_ms;
            c.level = v.level;
            c.deaths = v.deaths;
            c.rot = v.rot;
            if (v.flags & kVitalsValid) {
                c.have_hp = true;
                c.hp = static_cast<int>(v.hp);
                c.max_hp = static_cast<int>(v.max_hp);
                c.alive = (v.flags & kVitalsAlive) != 0;
                c.have_stamina = v.max_stamina > 0;
                c.stamina = v.stamina;
                c.max_stamina = v.max_stamina;
            }
            if (KnownMap(v.map_id)) c.map_id = v.map_id;
            if (v.flags & kVitalsHasPos) {
                c.have_pos = true;
                std::memcpy(c.pos, v.pos, sizeof c.pos);
            }
            c.spectating = (v.flags & kVitalsSpectating) != 0;
        }
        // Its PlayerIns here: the game's own copy, current every frame. Not for a spectator: its
        // copy is hidden here and its chr is carried along with the member it follows (a chr that
        // matches by name is that carried copy: "0.2 m"); its VITALS give the body's spot.
        if (c.same_world && !c.spectating)
            for (int i = 0; i < in.chrs_n; ++i) {
                const LocalChr& l = in.chrs[i];
                if (std::strncmp(l.name, c.name, kNameBytes) != 0) continue;
                c.local_read = true;
                c.chr = l.chr;
                c.age_ms = 0;
                c.local_dead = l.have_hp && l.hp <= 0;
                if (l.have_hp && !c.have_hp) {  // VITALS first: the remote data module lags
                    c.have_hp = true;
                    c.hp = l.hp;
                    c.max_hp = l.max_hp;
                    c.alive = l.hp > 0;
                    c.have_stamina = l.max_stamina > 0;
                    c.stamina = l.stamina;
                    c.max_stamina = l.max_stamina;
                }
                if (l.have_pos) {
                    c.have_pos = true;
                    std::memcpy(c.pos, l.pos, sizeof c.pos);
                }
                c.map_id = in.local_map;  // it stands in our world, on our map
            }
        if (!c.connected) c.have_pos = false;  // a lost member's last position means nothing
        // Distance / direction only within one world: two players in their own worlds on the same
        // map (e.g. both resting at the same lamp) are not "0.0 m" apart (playtest: "Hunter0 0.0 m"
        // while apart); the panel shows where it is instead (StateText).
        c.same_map = c.same_world && c.have_pos && in.me_have_pos && KnownMap(c.map_id) && c.map_id == in.local_map;
    }
    *out = b;
}

void Publish(const Board& b) {
    std::lock_guard<std::mutex> lk(g_mu);
    const std::uint64_t seq = g_board.seq + 1;
    g_board = b;
    g_board.seq = seq;
    g_board.published_ms = NowMs();
    g_shown.store(b.active && b.count > 0, std::memory_order_relaxed);
}

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool TagVisible(const Companion& c, std::int64_t board_age_ms, const char** why) {
    const char* w = nullptr;
    if (!c.connected) w = "not connected";
    else if (!c.same_world) w = "not in this world";
    else if (!c.local_read) w = "no chr here";  // only the live chr's position, never a network report
    else if (!c.have_pos || !c.same_map) w = "not on this map";
    else if (!c.alive || (c.have_hp && c.hp <= 0) || c.local_dead) w = "dead";
    else if (board_age_ms < 0 || board_age_ms > kTagFreshMs) w = "stale";
    if (why) *why = w ? w : "";
    return w == nullptr;
}

float PlateScale(float depth) {
    if (!std::isfinite(depth) || depth <= 0.0f) return kPlateMaxScale;
    return std::clamp(kPlateFarDepth / depth, 1.0f, kPlateMaxScale);
}

PlateLayout LayoutPlate(float ax, float ay, float w, float h, float depth, const PlateParams& p) {
    const float k = PlateScale(depth) * h;
    auto px = [&](float f, float lo) { return std::max(lo, std::round(f * k)); };
    PlateLayout l;
    l.font = p.font_h * k;
    const float bar_w = p.bar_w * w * PlateScale(depth);
    l.bar_x0 = std::round(ax - bar_w * 0.5f);
    l.bar_x1 = std::round(ax + bar_w * 0.5f);
    l.bar_y0 = std::round(ay);
    l.bar_y1 = l.bar_y0 + px(p.bar_h, 1.0f);
    l.cap_w = px(p.cap_w, 1.0f);
    l.cap_y0 = l.bar_y0 - std::round(p.cap_rise * k);
    l.cap_y1 = l.cap_y0 + px(p.cap_h, 1.0f);
    const float dx = std::round(p.cap_dx * k);
    l.cap_l = l.bar_x0 + dx;
    l.cap_r = l.bar_x1 - dx - l.cap_w;
    l.name_x = l.bar_x0 + std::round(p.name_inset * k);
    l.name_baseline = l.bar_y0 - std::round(p.baseline_gap * k);
    l.outline = p.outline > 0.0f ? px(p.outline, 1.0f) : 0.0f;
    return l;
}

namespace {
PlateParams g_tune;  // overlay thread only
}

int ParsePlateTune(const char* text, PlateParams* p) {
    struct Key { const char* name; float* v; int n; };
    const Key keys[] = {
        {"font_h", &p->font_h, 1}, {"bar_w", &p->bar_w, 1}, {"bar_h", &p->bar_h, 1}, {"cap_w", &p->cap_w, 1},
        {"cap_h", &p->cap_h, 1}, {"cap_rise", &p->cap_rise, 1}, {"cap_dx", &p->cap_dx, 1},
        {"name_inset", &p->name_inset, 1}, {"baseline_gap", &p->baseline_gap, 1}, {"outline", &p->outline, 1},
        {"rim", &p->rim, 1}, {"glow", &p->glow, 1}, {"name_rgb", p->name_rgb, 3}, {"bar_top", p->bar_top, 3},
        {"bar_bottom", p->bar_bottom, 3}, {"cap_rgb", p->cap_rgb, 3},
    };
    int applied = 0;
    while (text && *text) {
        const char* end = std::strchr(text, '\n');
        const std::size_t len = end ? std::size_t(end - text) : std::strlen(text);
        char line[160] = {};
        std::memcpy(line, text, std::min(len, sizeof line - 1));
        text = end ? end + 1 : nullptr;
        char* eq = std::strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        char* name = line;
        while (*name == ' ' || *name == '\t') ++name;
        for (char* e = eq - 1; e >= name && (*e == ' ' || *e == '\t'); --e) *e = 0;
        for (const Key& k : keys) {
            if (std::strcmp(name, k.name) != 0) continue;
            float v[3] = {};
            const int got = k.n == 1 ? std::sscanf(eq + 1, "%f", &v[0]) : std::sscanf(eq + 1, "%f,%f,%f", &v[0], &v[1], &v[2]);
            if (got != k.n) break;
            for (int i = 0; i < k.n; ++i) k.v[i] = k.n == 3 ? v[i] / 255.0f : v[i];
            ++applied;
        }
    }
    return applied;
}

const PlateParams& CurrentPlateParams() { return g_tune; }

void PlateTunePoll() {
    static const char* path = std::getenv("BB_PARTY_HUD_TUNE_FILE");
    static std::int64_t last = 0;
    if (!path || NowMs() - last < 500) return;
    last = NowMs();
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    const std::size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = 0;
    static std::string prev;
    if (prev == buf) return;
    prev = buf;
    PlateParams p;
    const int keys = ParsePlateTune(buf, &p);
    g_tune = p;
    std::printf("Party HUD: tune file read (%d keys)\n", keys);
}

float TagTextTop(float anchor_y, float text_h, bool with_bar, float bar_h, float gap) {
    return anchor_y - (with_bar ? bar_h + gap : 0.0f) - text_h;
}

void Read(Board* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    *out = g_board;
}

bool Shown() {
    return g_shown.load(std::memory_order_relaxed) && !g_cutscene.load(std::memory_order_relaxed);
}

void SetCutscene(bool playing) {
    g_cutscene.store(playing, std::memory_order_relaxed);
}

bool FullscreenMenuOpen(const int fields[kFullscreenMenuFields], signed char force) {
    bool any = false;
    for (int i = 0; i < kFullscreenMenuFields; ++i) any = any || fields[i] != 0;
    if (force == -1) return any;
    if (force == 1) return true;
    return force != 0 && any;
}

bool GameMenuUp(bool flow_menu_open, const int fields[kFullscreenMenuFields], signed char force) {
    return flow_menu_open || FullscreenMenuOpen(fields, force);
}

bool FlowMenuKind::Update(bool flow_open, bool dialog, double t) {
    if (!flow_open) {
        state_ = State::Closed;
        return false;
    }
    if (state_ == State::Closed || (state_ == State::Box && !dialog)) {
        state_ = State::Pending;
        since_ = t;
    }
    if (state_ == State::Pending) {
        if (dialog) state_ = State::Box;
        else if (t - since_ >= kBoxGraceS) state_ = State::Full;
    }
    return state_ == State::Full;
}

bool AreaBannerWatch::Update(bool in_world, std::uint64_t player, std::uint32_t map, double t) {
    if (!in_world || !player) {
        player_ = 0;  // the next PlayerIns is an arrival
        return false;
    }
    const bool known = map != 0 && map != 0xffffffffu;
    if (player != player_ || (known && map_ != 0xffffffffu && map != map_)) since_ = t;
    player_ = player;
    if (known) map_ = map;
    return t - since_ < kShowS;
}

float PerfHudBottom(float normal, float height, bool banner) {
    if (!banner || height <= 0.0f) return normal;
    const float top = kAreaBannerTop * height;
    return normal < top ? normal : top;
}

bool Overlaps(const HudRect& a, const HudRect& b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

HudRect HudStage(float w, float h, float frame_aspect) {
    HudRect r;
    if (w <= 0.0f || h <= 0.0f) return r;
    if (!(frame_aspect > 0.0f)) frame_aspect = 16.0f / 9.0f;
    // The game frame letterboxed in the display, then 16:9 inside the frame (never wider than it).
    float fw = w, fh = w / frame_aspect;
    if (fh > h) {
        fh = h;
        fw = h * frame_aspect;
    }
    float sw = fw, sh = fw * 9.0f / 16.0f;
    if (sh > fh) {
        sh = fh;
        sw = fh * 16.0f / 9.0f;
    }
    r.x0 = (w - sw) * 0.5f;
    r.y0 = (h - sh) * 0.5f;
    r.x1 = r.x0 + sw;
    r.y1 = r.y0 + sh;
    return r;
}

HudRect BossBarArea(const HudRect& stage, int rows) {
    if (rows < 1) rows = 1;
    const float sw = stage.x1 - stage.x0, sh = stage.y1 - stage.y0;
    float top = kBossBarTop - float(rows - 1) * kBossBarRowPitch;
    if (top < 0.0f) top = 0.0f;
    return HudRect{stage.x0 + kBossBarLeft * sw, stage.y0 + top * sh, stage.x0 + kBossBarRight * sw,
                   stage.y0 + kBossBarBottom * sh};
}

float BottomClearOf(float x0, float x1, float bottom, float box_h, const HudRect& avoid, float gap) {
    const HudRect box{x0, bottom - box_h, x1, bottom};
    if (!Overlaps(box, avoid)) return bottom;
    return avoid.y0 - gap;
}

std::vector<std::string> WrapNoticeLines(const std::string& text, float max_w,
                                         const std::function<float(const std::string&)>& measure) {
    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at <= text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string para = text.substr(at, end - at);
        at = end + 1;
        // Greedy at spaces: the line grows by whole words while it fits.
        std::string line;
        std::size_t w = 0;
        bool any = false;
        while (w < para.size()) {
            while (w < para.size() && para[w] == ' ') ++w;
            if (w >= para.size()) break;
            std::size_t e = para.find(' ', w);
            if (e == std::string::npos) e = para.size();
            const std::string word = para.substr(w, e - w);
            w = e;
            if (line.empty()) {
                line = word;
            } else if (measure(line + " " + word) <= max_w) {
                line += " " + word;
            } else {
                lines.push_back(line);
                line = word;
            }
            any = true;
        }
        if (any || para.empty()) lines.push_back(line);
        if (end == text.size()) break;
    }
    return lines;
}

float CenteredLineX(float left, float box_w, float line_w) {
    return line_w >= box_w ? left : left + (box_w - line_w) * 0.5f;
}

void SetGameMenu(bool open) {
    g_game_menu.store(open, std::memory_order_relaxed);
}

bool GameMenu() {
    return g_game_menu.load(std::memory_order_relaxed);
}

void SetGameDialog(bool open) {
    g_game_dialog.store(open, std::memory_order_relaxed);
}

bool GameDialog() {
    return g_game_dialog.load(std::memory_order_relaxed);
}

float Distance(const float a[3], const float b[3]) {
    const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float Bearing(const float from[3], const float to[3], float fx, float fz, float rx, float rz) {
    const float dx = to[0] - from[0], dz = to[2] - from[2];
    const float ahead = dx * fx + dz * fz, right = dx * rx + dz * rz;
    if (ahead == 0.0f && right == 0.0f) return 0.0f;
    return std::atan2(right, ahead);
}

bool Project(const float view[12], const float proj[4], const float p[3], float* u, float* v, float* depth) {
    const float x = view[0] * p[0] + view[1] * p[1] + view[2] * p[2] + view[3];
    const float y = view[4] * p[0] + view[5] * p[1] + view[6] * p[2] + view[7];
    const float z = view[8] * p[0] + view[9] * p[1] + view[10] * p[2] + view[11];
    if (!(z > 0.3f) || !std::isfinite(x) || !std::isfinite(y)) return false;
    *u = (x * proj[0] / z) * 0.5f + 0.5f;
    *v = (y * proj[1] / z) * 0.5f + 0.5f;
    *depth = z;
    return std::isfinite(*u) && std::isfinite(*v);
}

ScreenPoint WorldToScreen(const float view[12], const float proj[4], const float p[3], float w, float h,
                          float margin) {
    ScreenPoint sp;
    float u = 0, v = 0, depth = 0;
    if (!Project(view, proj, p, &u, &v, &depth)) return sp;
    sp.in_front = true;
    sp.x = u * w;
    sp.y = v * h;
    sp.depth = depth;
    sp.visible = u >= margin && u <= 1.0f - margin && v >= margin && v <= 1.0f - margin;
    return sp;
}

ScreenBox BodyBox(const float view[12], const float proj[4], const float feet[3], float w, float h, float half_width) {
    ScreenBox box;
    const float head[3] = {feet[0], feet[1] + kBodyHeightM, feet[2]};
    const ScreenPoint f = WorldToScreen(view, proj, feet, w, h), t = WorldToScreen(view, proj, head, w, h);
    if (!f.in_front || !t.in_front) return box;
    const float half = half_width * std::fabs(t.y - f.y);
    box.valid = true;
    box.x0 = std::min(f.x, t.x) - half;
    box.x1 = std::max(f.x, t.x) + half;
    box.y0 = std::min(f.y, t.y);
    box.y1 = std::max(f.y, t.y);
    box.depth = 0.5f * (f.depth + t.depth);
    return box;
}

float CoveredFraction(const ScreenBox& a, const ScreenBox& b) {
    const float area = (a.x1 - a.x0) * (a.y1 - a.y0);
    if (!a.valid || !b.valid || !(area > 0.0f)) return 0.0f;
    const float ix = std::min(a.x1, b.x1) - std::max(a.x0, b.x0), iy = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
    if (ix <= 0.0f || iy <= 0.0f) return 0.0f;
    return std::clamp(ix * iy / area, 0.0f, 1.0f);
}

float OwnBodyTagAlpha(const ScreenBox& member, const ScreenBox& me) {
    if (!member.valid || !me.valid || member.depth < me.depth + kOwnCoverDepthM) return 1.0f;
    const float cover = CoveredFraction(member, me);
    return std::clamp((kCoverHidden - cover) / (kCoverHidden - kCoverFadeFrom), 0.0f, 1.0f);
}

void MemberColour(int slot, float rgb[3]) {
    // Host gold, then pale blue, green, violet: readable on Yharnam's greys, distinct from the
    // game's own blue phantom label and red HP.
    static const float kColours[4][3] = {
        {0.96f, 0.80f, 0.42f}, {0.55f, 0.78f, 1.00f}, {0.55f, 0.92f, 0.55f}, {0.86f, 0.62f, 1.00f}};
    static const float kCream[3] = {0.96f, 0.88f, 0.70f};
    const float* c = slot >= 0 && slot < 4 ? kColours[slot] : kCream;
    for (int i = 0; i < 3; ++i) rgb[i] = c[i];
}

void ResetForTest() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_board = Board{};
    g_shown = false;
}

}  // namespace party::hud
