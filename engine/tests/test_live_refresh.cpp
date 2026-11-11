// visy 0.8.1 livefps081: the rule that re-creates the swapchain once when Live FPS stays low after a load
// (gpu/shim/bbport_live_refresh_logic.h; his tests/test_frame_rate.cpp block). ninja -C out/gpu live-refresh-test
#undef NDEBUG
#include "bbport_live_refresh_logic.h"
#include <cassert>
#include <cstdio>

int main() {
    {
        using namespace BbLiveRefresh;
        State st;
        auto stay = [&](double fps, int secs, int& fires) {
            for (int i = 0; i < secs; ++i) fires += Second(st, fps, true, 0) ? 1 : 0;
        };
        int fires = 0;
        stay(95, 20, fires);                 // healthy first stay, no baseline yet
        assert(fires == 0);
        Second(st, 0, false, 0);             // a load
        stay(66, 30, fires);                 // stuck at 66 after the load: fires once after settle + sustain
        assert(fires == 1);
        int f2 = 0;
        Second(st, 0, false, 0);
        stay(66, 30, f2);                    // the stuck rate is the next baseline: nothing more
        assert(f2 == 0);
        // a recovered stay, then a load into a slower scene that is steady: not a drop of a stay but a baseline of 66
        State a;
        int fa = 0;
        for (int i = 0; i < 20; ++i) fa += Second(a, 95, true, 0);
        Second(a, 0, false, 0);
        for (int i = 0; i < 40; ++i) fa += Second(a, i % 2 ? 90 : 100, true, 0); // jitter, no drop
        assert(fa == 0);
        // short blips do not count
        State b;
        int fb = 0;
        for (int i = 0; i < 20; ++i) fb += Second(b, 95, true, 0);
        Second(b, 0, false, 0);
        for (int i = 0; i < 60; ++i) fb += Second(b, i % 5 == 0 ? 40 : 95, true, 0);
        assert(fb == 0);
        // a low baseline (a cap or a slow scene) never counts; a stay shorter than the window sets none
        State c;
        int fc = 0;
        for (int i = 0; i < 20; ++i) fc += Second(c, 30, true, 0);
        Second(c, 0, false, 0);
        for (int i = 0; i < 30; ++i) fc += Second(c, 10, true, 0);
        assert(fc == 0);
        State d;
        int fd = 0;
        for (int i = 0; i < 8; ++i) fd += Second(d, 95, true, 0); // 4 judged seconds < Window
        Second(d, 0, false, 0);
        for (int i = 0; i < 30; ++i) fd += Second(d, 50, true, 0);
        assert(fd == 0);
        // a changed frame limit drops the baseline
        State e;
        int fe = 0;
        for (int i = 0; i < 20; ++i) fe += Second(e, 95, true, 0);
        Second(e, 0, false, 0);
        for (int i = 0; i < 30; ++i) fe += Second(e, 60, true, 1);
        assert(fe == 0);
        // test aid: N in-world seconds after each load, once per stay
        State g;
        int fg = 0;
        for (int i = 0; i < 10; ++i) fg += Second(g, 95, true, 0, 5);
        assert(fg == 1);
        Second(g, 0, false, 0, 5);
        for (int i = 0; i < 10; ++i) fg += Second(g, 95, true, 0, 5);
        assert(fg == 2);
    }
    std::puts("live-refresh-test: ok");
    return 0;
}
