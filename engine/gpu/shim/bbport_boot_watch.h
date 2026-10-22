/* SPDX-License-Identifier: GPL-2.0-or-later
 * Boot watchdog (BB_BOOT_WATCHDOG=S, diagnostics, off by default; launcher: Advanced / Debug >
 * Diagnostics > Boot freeze dump). From the game's entry until the first world load, the probe's
 * once-a-second watchdog thread writes a hang snapshot (out\boot_hang_<time>.log, also copied into
 * the log) when the boot stops moving for S seconds:
 *   - no frame flipped for S s (the game's main thread blocked), or
 *   - no main-loop tick for S s (SprjFlipper::Update, party mode: the director's tick), or
 *   - the party director asked for it (title auto-continue made no press for S s while neither a
 *     load nor the FROM sign-in held it).
 * One snapshot per stall: progress on every counter re-arms it. Party mode ends the watch at the
 * first world load; single-player has no world signal, so it stays on frame stalls all session.
 * 0.7.1 full smoke: one host in 16 boots stood on a plain gray screen right after the title's
 * logo file open, with no log line saying where (tools: docs/party/DEBUG_SWITCHES.md).
 *
 * C header: the probe (C) and the GPU library (C++) both use it; the rules are header-only so the
 * unit test (tests/test_boot_watch.cpp) needs nothing else. */
#ifndef BBPORT_BOOT_WATCH_H
#define BBPORT_BOOT_WATCH_H

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BB_BOOT_WATCH_NONE = 0,
    BB_BOOT_WATCH_FLIPS = 1,    /* no frame flipped for the limit */
    BB_BOOT_WATCH_TICKS = 2,    /* the main loop (director tick) stopped */
    BB_BOOT_WATCH_REQUEST = 3,  /* the director asked (title auto-continue stuck) */
};

typedef struct {
    uint64_t limit_ms;   /* 0: off */
    uint64_t flips, ticks;
    uint64_t flips_at, ticks_at;
    int started, dumped, done;
} BbBootWatch;

/* BB_BOOT_WATCHDOG: seconds (1..600); unset, 0, "off" or junk: off. */
static inline uint64_t bb_boot_watch_limit_ms(const char *env) {
    if (!env || !*env) return 0;
    char *end = NULL;
    long s = strtol(env, &end, 10);
    if (end == env || *end || s <= 0) return 0;
    if (s > 600) s = 600;
    return (uint64_t)s * 1000u;
}

/* Once a second from the watchdog thread. Returns a BB_BOOT_WATCH_* reason when a snapshot is due
 * (at most once per stall), else BB_BOOT_WATCH_NONE. `ticks` 0 = no main-loop counter (no party). */
static inline int bb_boot_watch_step(BbBootWatch *w, uint64_t now_ms, uint64_t flips, uint64_t ticks, int boot_done,
                                     int requested) {
    if (!w->limit_ms || w->done) return BB_BOOT_WATCH_NONE;
    if (boot_done) {
        w->done = 1;
        return BB_BOOT_WATCH_NONE;
    }
    if (!w->started) {
        w->started = 1;
        w->flips = flips;
        w->ticks = ticks;
        w->flips_at = w->ticks_at = now_ms;
    }
    if (flips != w->flips) {
        w->flips = flips;
        w->flips_at = now_ms;
    }
    if (ticks != w->ticks) {
        w->ticks = ticks;
        w->ticks_at = now_ms;
    }
    int why = BB_BOOT_WATCH_NONE;
    if (now_ms - w->flips_at >= w->limit_ms) {
        why = BB_BOOT_WATCH_FLIPS;
    } else if (ticks && now_ms - w->ticks_at >= w->limit_ms) {
        why = BB_BOOT_WATCH_TICKS;
    } else if (requested) {
        why = BB_BOOT_WATCH_REQUEST;
    }
    if (why == BB_BOOT_WATCH_NONE) {
        w->dumped = 0; /* moving again: the next stall gets its own snapshot */
        return BB_BOOT_WATCH_NONE;
    }
    if (w->dumped) return BB_BOOT_WATCH_NONE;
    w->dumped = 1;
    return why;
}

static inline const char *bb_boot_watch_reason(int why) {
    switch (why) {
    case BB_BOOT_WATCH_FLIPS: return "no frame flipped";
    case BB_BOOT_WATCH_TICKS: return "the game's main loop stopped (no SprjFlipper::Update)";
    case BB_BOOT_WATCH_REQUEST: return "the title's auto-continue is stuck";
    default: return "";
    }
}

/* The GPU library's side (bbgpu.cpp): the director's main-loop ticks, the first world load, and
 * a pending request (with its reason text, taken once). */
uint64_t bbgpu_boot_ticks(void);
int bbgpu_boot_done(void);
int bbgpu_boot_take_request(char *why, unsigned size);

#ifdef __cplusplus
}

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace BbBootWatchState {
inline std::atomic<std::uint64_t> ticks{0};
inline std::atomic<bool> done{false};
inline std::atomic<bool> requested{false};
inline std::mutex why_mu;
inline std::string why;

/// The director's tick (game main thread, every frame).
inline void Tick() { ticks.fetch_add(1, std::memory_order_relaxed); }
/// The first world load: the boot is over.
inline void Done() { done.store(true, std::memory_order_relaxed); }
/// Asks the watchdog thread for a snapshot (it writes it within a second, once per stall).
inline void Request(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lk(why_mu);
        why = reason;
    }
    requested.store(true, std::memory_order_release);
}
/// BB_BOOT_WATCHDOG in seconds (0: off).
inline double LimitSeconds() {
    return double(bb_boot_watch_limit_ms(std::getenv("BB_BOOT_WATCHDOG"))) / 1000.0;
}
} // namespace BbBootWatchState
#endif

#endif
