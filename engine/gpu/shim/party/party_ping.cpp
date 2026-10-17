// SPDX-License-Identifier: GPL-3.0-or-later
// Pings (I2), the pure part: EVENT map_ping JSON, the relay rate, the marker board, the aim
// pick from the depth probe and the screen-edge clamp (party_ping.h). No game dependency
// (tests/test_party_ping.cpp, tests/party_fuzz.cpp). The game side: party_ping_game.cpp.
#include "party_ping.h"

#include "net/json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace party::ping {

namespace {

const json::Value* Find(const json::Value& v, const char* key) {
    return v.type == json::Value::Type::Object ? v.find(key) : nullptr;
}

bool Number(const json::Value& v, const char* key, double lo, double hi, bool integer, double* out) {
    const json::Value* x = Find(v, key);
    if (!x) return true;  // optional
    if (x->type != json::Value::Type::Number || !std::isfinite(x->number) || x->number < lo || x->number > hi) {
        return false;
    }
    if (integer && x->number != std::floor(x->number)) return false;
    *out = x->number;
    return true;
}

bool ValidName(const std::string& s) {
    if (s.empty() || s.size() > 16) return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    }
    return true;
}

float Dist(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::mutex g_mu;
Marker g_board[kMaxSlots];

}  // namespace

std::string ToJson(const PingEvent& e) {
    json::Value v = json::Value::make_object();
    v.set("id", e.id);
    v.set("map", e.map);
    json::Value p = json::Value::make_array();
    for (float f : e.pos) p.push(json::Value(double(std::round(f * 100.0f) / 100.0f)));
    v.set("pos", p);
    if (e.hit) v.set("hit", 1);
    if (e.clear) v.set("clear", 1);
    v.set("t", static_cast<long long>(e.t));
    if (e.slot >= 0) v.set("slot", e.slot);
    if (!e.who.empty()) v.set("who", e.who);
    return json::dump(v, 0);
}

bool FromJson(const std::string& text, PingEvent* out, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = std::string("map_ping: ") + why;
        return false;
    };
    json::Value v;
    std::string err;
    if (text.size() > 1024) return fail("too long");
    if (!json::parse(text, v, err)) return fail("not JSON");
    if (v.type != json::Value::Type::Object) return fail("not an object");
    PingEvent e;
    double id = 0, map = 0, hit = 0, clear = 0, t = 0, slot = -1;
    if (!Number(v, "id", 0, 4294967295.0, true, &id) || !Number(v, "map", 0, 4294967295.0, true, &map) ||
        !Number(v, "hit", 0, 1, true, &hit) || !Number(v, "clear", 0, 1, true, &clear) ||
        !Number(v, "t", 0, 9.0e15, true, &t) || !Number(v, "slot", -1, kMaxSlots - 1, true, &slot)) {
        return fail("bad number");
    }
    e.id = static_cast<std::uint32_t>(id);
    e.map = static_cast<std::uint32_t>(map);
    e.hit = hit != 0;
    e.clear = clear != 0;
    e.t = static_cast<std::int64_t>(t);
    e.slot = static_cast<int>(slot);
    if (!e.clear && (e.map == 0 || e.map == 0xffffffffu)) return fail("no map");
    const json::Value* p = Find(v, "pos");
    if (!p) {
        if (!e.clear) return fail("no position");
    } else {
        if (p->type != json::Value::Type::Array || p->array.size() != 3) return fail("bad position");
        for (int i = 0; i < 3; ++i) {
            const json::Value& c = p->array[std::size_t(i)];
            if (c.type != json::Value::Type::Number || !std::isfinite(c.number) || std::fabs(c.number) > 1e5) {
                return fail("bad position");
            }
            e.pos[i] = float(c.number);
        }
    }
    if (const json::Value* w = Find(v, "who")) {
        if (w->type != json::Value::Type::String || !ValidName(w->string)) return fail("bad name");
        e.who = w->string;
    }
    *out = e;
    return true;
}

bool RelayRate::Take(int slot, double now_s) {
    if (slot < 0 || slot >= kMaxSlots || !std::isfinite(now_s)) return false;
    const double rate = kRelayBurst / kRelayWindowS;
    if (last_[slot] != 0.0 && now_s > last_[slot]) {
        tokens_[slot] = std::min<double>(kRelayBurst, tokens_[slot] + (now_s - last_[slot]) * rate);
    }
    last_[slot] = now_s;
    if (tokens_[slot] < 1.0) return false;
    tokens_[slot] -= 1.0;
    return true;
}

StainMark g_stain;  // under g_mu

void SetStain(const StainMark& m) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_stain = m;
}

bool GetStain(StainMark* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    *out = g_stain;
    return g_stain.live;
}

void Place(const Marker& m) {
    if (m.slot < 0 || m.slot >= kMaxSlots) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_board[m.slot] = m;
    g_board[m.slot].live = true;
    g_board[m.slot].name[kNameBytes - 1] = 0;
}

void Clear(int slot) {
    if (slot < 0 || slot >= kMaxSlots) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_board[slot].live = false;
}

void ClearAll() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (Marker& m : g_board) m.live = false;
}

int Read(Marker* out, std::int64_t now_ms) {
    std::lock_guard<std::mutex> lk(g_mu);
    int n = 0;
    for (Marker& m : g_board) {
        if (m.live && m.until_ms <= now_ms) m.live = false;
        if (m.live) out[n++] = m;
    }
    return n;
}

float Alpha(std::int64_t left_ms) {
    if (left_ms <= 0) return 0.0f;
    if (left_ms >= kFadeMs) return 1.0f;
    return float(left_ms) / float(kFadeMs);
}

std::int64_t LifeLeft(std::int64_t t, std::int64_t now) {
    if (t <= 0 || now <= 0) return kLifeMs;  // no clock on one side: a full life
    const std::int64_t age = now - t;
    // A sender's clock estimate may run a little ahead of the host's (RTT / 2): up to 2 s is fine.
    if (age < -2000 || age >= kLifeMs) return 0;
    return std::min(kLifeMs, kLifeMs - std::max<std::int64_t>(age, 0));
}

bool PickAim(const float cam[3], const float player[3], const Sample* s, int n, int centre, float out[3]) {
    if (!s || n <= 0 || centre < 0 || centre >= n) return false;
    const float chest[3] = {player[0], player[1] + 1.0f, player[2]};
    const float player_depth = Dist(cam, chest);
    for (int i = centre; i >= 0; --i) {
        const Sample& x = s[i];
        if (!x.hit || !std::isfinite(x.p[0]) || !std::isfinite(x.p[1]) || !std::isfinite(x.p[2])) continue;
        const float from_player = Dist(x.p, chest);
        if (Dist(cam, x.p) < player_depth - 0.3f) continue;  // between the camera and the player
        if (from_player < 1.6f) continue;                     // the player itself (or at its feet)
        if (from_player > kMaxRange) return false;            // the far distance / sky dome
        std::memcpy(out, x.p, 3 * sizeof(float));
        return true;
    }
    return false;
}

void FallbackAim(const float fwd[3], const float player[3], float out[3]) {
    float f[3] = {fwd[0], fwd[1], fwd[2]};
    const float l = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (!(l > 1e-4f) || !std::isfinite(l)) {
        f[0] = 0, f[1] = 0, f[2] = 1;
    } else {
        for (float& c : f) c /= l;
    }
    for (int i = 0; i < 3; ++i) out[i] = player[i] + (i == 1 ? 1.0f : 0.0f) + f[i] * kFallbackDistance;
}

EdgePoint ToScreenOrEdge(const float view[12], const float proj[4], const float p[3], float w, float h, float inset) {
    EdgePoint e;
    const float x = view[0] * p[0] + view[1] * p[1] + view[2] * p[2] + view[3];
    const float y = view[4] * p[0] + view[5] * p[1] + view[6] * p[2] + view[7];
    const float z = view[8] * p[0] + view[9] * p[1] + view[10] * p[2] + view[11];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !(w > 2 * inset) || !(h > 2 * inset)) {
        e.x = w * 0.5f, e.y = h * 0.5f;
        return e;
    }
    e.depth = z;
    const float cx = w * 0.5f, cy = h * 0.5f;
    if (z > 0.3f) {
        const float sx = cx + (x * proj[0] / z) * cx, sy = cy + (y * proj[1] / z) * cy;
        if (sx >= inset && sx <= w - inset && sy >= inset && sy <= h - inset) {
            e.on_screen = true;
            e.x = sx, e.y = sy;
            e.angle = std::atan2(sy - cy, sx - cx);
            return e;
        }
    }
    // Off screen: the direction on the screen plane (behind the camera too: x, y keep their sides).
    float dx = x * proj[0] * cx, dy = y * proj[1] * cy;
    if (z > 0.3f) dx /= z, dy /= z;
    if (std::fabs(dx) < 1e-6f && std::fabs(dy) < 1e-6f) dy = 1.0f;  // straight behind: the bottom
    const float hx = cx - inset, hy = cy - inset;
    const float k = std::min(std::fabs(dx) > 1e-9f ? hx / std::fabs(dx) : 1e30f,
                             std::fabs(dy) > 1e-9f ? hy / std::fabs(dy) : 1e30f);
    e.x = cx + dx * k;
    e.y = cy + dy * k;
    e.angle = std::atan2(dy, dx);
    return e;
}

void FormatDistance(float metres, char* out, std::size_t n) {
    if (!out || n == 0) return;
    if (!std::isfinite(metres) || metres < 0) metres = 0;
    std::snprintf(out, n, metres < 10.0f ? "%.1f m" : "%.0f m", double(metres));
}

void ResetForTest() {
    ClearAll();
}

}  // namespace party::ping
