// Boot watchdog rules (gpu/shim/bbport_boot_watch.h). ninja -C out/gpu boot-watch-test
#include "bbport_boot_watch.h"

#include <cassert>
#include <cstdio>

int main() {
    // BB_BOOT_WATCHDOG: seconds; off unless a positive number.
    assert(bb_boot_watch_limit_ms(nullptr) == 0);
    assert(bb_boot_watch_limit_ms("") == 0);
    assert(bb_boot_watch_limit_ms("0") == 0);
    assert(bb_boot_watch_limit_ms("off") == 0);
    assert(bb_boot_watch_limit_ms("12x") == 0);
    assert(bb_boot_watch_limit_ms("-5") == 0);
    assert(bb_boot_watch_limit_ms("20") == 20000);
    assert(bb_boot_watch_limit_ms("9999") == 600000);

    // Off: never fires.
    BbBootWatch off{};
    for (uint64_t t = 0; t < 100000; t += 1000) assert(bb_boot_watch_step(&off, t, 0, 0, 0, 0) == BB_BOOT_WATCH_NONE);

    // Frames and ticks moving: nothing.
    BbBootWatch w{};
    w.limit_ms = 5000;
    uint64_t flips = 0, ticks = 0, t = 1000;
    for (int i = 0; i < 20; ++i, t += 1000) assert(bb_boot_watch_step(&w, t, ++flips, ++ticks, 0, 0) == 0);

    // The 0.7.1 gray screen: the main loop stops (no flips, no ticks). One snapshot after the
    // limit, not one a second.
    const uint64_t stop = t - 1000;
    int fired = 0;
    for (; t <= stop + 12000; t += 1000) {
        const int why = bb_boot_watch_step(&w, t, flips, ticks, 0, 0);
        if (why) {
            ++fired;
            assert(why == BB_BOOT_WATCH_FLIPS);
            assert(t - stop >= 5000);
        }
    }
    assert(fired == 1);
    // Moving again re-arms: the next stall gets its own snapshot.
    assert(bb_boot_watch_step(&w, t, ++flips, ++ticks, 0, 0) == 0);
    t += 1000;
    fired = 0;
    for (int i = 0; i < 8; ++i, t += 1000) fired += bb_boot_watch_step(&w, t, flips, ticks, 0, 0) != 0;
    assert(fired == 1);

    // Frames go on (the presenter / a loading card) but the main loop's tick stopped.
    BbBootWatch m{};
    m.limit_ms = 3000;
    flips = 0;
    ticks = 7;
    t = 0;
    int why = 0;
    for (int i = 0; i < 6 && !why; ++i, t += 1000) why = bb_boot_watch_step(&m, t, ++flips, ticks, 0, 0);
    assert(why == BB_BOOT_WATCH_TICKS);
    // No director (single-player): ticks stay 0 and are not a stall.
    BbBootWatch solo{};
    solo.limit_ms = 3000;
    for (t = 0, flips = 0; t < 20000; t += 1000) assert(bb_boot_watch_step(&solo, t, ++flips, 0, 0, 0) == 0);

    // The director's request (auto-continue stuck while frames and ticks go on): at once, once.
    BbBootWatch r{};
    r.limit_ms = 20000;
    assert(bb_boot_watch_step(&r, 0, 1, 1, 0, 0) == 0);
    assert(bb_boot_watch_step(&r, 1000, 2, 2, 0, 1) == BB_BOOT_WATCH_REQUEST);
    assert(bb_boot_watch_step(&r, 2000, 3, 3, 0, 0) == 0);

    // The first world load ends the watch for good.
    BbBootWatch d{};
    d.limit_ms = 2000;
    assert(bb_boot_watch_step(&d, 0, 1, 1, 0, 0) == 0);
    assert(bb_boot_watch_step(&d, 1000, 2, 2, 1, 0) == 0);
    for (t = 2000; t < 30000; t += 1000) assert(bb_boot_watch_step(&d, t, 2, 2, 0, 1) == 0);

    assert(bb_boot_watch_reason(BB_BOOT_WATCH_TICKS)[0]);
    std::puts("boot-watch-test: ok");
    return 0;
}
