// SPDX-License-Identifier: GPL-3.0-or-later
// C5 spectate: which living party member a dead player in a boss-fight limbo follows, and how
// the carried spot follows it (smoothing, diagnostics counters).
// Pure logic (unit test: tests/test_party_spectate.cpp); the game side is SpectateTick in
// party_b2.cpp.
#pragma once

#include <cmath>

/// living: the slots of the living party members in task order (n of them). current: the slot
/// followed so far (-1: none). advance: the spectator pressed Cross (switch to the next member).
/// Returns the slot to follow, or -1 when nobody is alive. The followed member stays while it
/// lives; when it dies (or leaves) or on a switch, the next living slot after it in task order
/// (wrapping); with one living member a switch keeps it.
inline int SpectateTarget(const int* living, int n, int current, bool advance) {
    if (!living || n <= 0) {
        return -1;
    }
    int at = -1;
    for (int i = 0; i < n; ++i) {
        if (living[i] == current) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        // Not followed yet or the followed member is gone: the next slot after it, else the first.
        for (int i = 0; i < n; ++i) {
            if (current >= 0 && living[i] > current) {
                return living[i];
            }
        }
        return living[0];
    }
    return advance ? living[(at + 1) % n] : current;
}

/// The spot the dead chr is carried to (the spectator's camera orbits it): it follows the followed
/// member's spot with a short exponential lag (`tau` s, frame-rate independent), so the camera
/// glides instead of hopping, and jumps only when the member is more than `snap` m away (a new
/// member, a warp). Step returns how far the spot moved this tick (m); *snapped when it jumped.
struct SpectateFollow {
    float at[3] = {0, 0, 0};
    bool have = false;

    float Step(const float want[3], float dt, float tau, float snap, bool* snapped) {
        const float dx = want[0] - at[0], dy = want[1] - at[1], dz = want[2] - at[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!have || !(d <= snap) || tau <= 0.0f) {
            at[0] = want[0], at[1] = want[1], at[2] = want[2];
            if (snapped) *snapped = true;
            have = true;
            return d;
        }
        if (snapped) *snapped = false;
        if (dt <= 0.0f) {
            return 0.0f;
        }
        const float k = 1.0f - std::exp(-dt / tau);
        at[0] += dx * k, at[1] += dy * k, at[2] += dz * k;
        return d * k;
    }
};

/// BB_PARTY_SPECTATE_LOG diagnostics: per window (a summary line), camera resets (CamReset calls),
/// the largest carried-spot step in one tick, and the render camera's largest move / turn between
/// two ticks (what a player sees as a snap).
struct SpectateCamStats {
    int resets = 0, ticks = 0, snaps = 0;
    float max_step = 0.0f, max_cam_move = 0.0f, max_cam_turn = 0.0f;  // m, m, radians
    float min_cam_dist = 1e9f;  // render camera to the followed member (m): ~0 = inside it / a wall
    void Reset() { *this = SpectateCamStats{}; }
    void Step(float step, bool snapped) {
        ++ticks;
        snaps += snapped ? 1 : 0;
        if (!snapped && step > max_step) max_step = step;
    }
    void Camera(float move, float turn, float dist) {
        if (move > max_cam_move) max_cam_move = move;
        if (turn > max_cam_turn) max_cam_turn = turn;
        if (dist < min_cam_dist) min_cam_dist = dist;
    }
    /// Resets per minute over a window of `secs`.
    float ResetsPerMinute(double secs) const { return secs > 0 ? float(resets * 60.0 / secs) : 0.0f; }
};
