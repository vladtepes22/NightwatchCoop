// SPDX-License-Identifier: GPL-3.0-or-later
// Stall probe rules (gpu/shim/bbport_stall_probe_rules.h). ninja -C out/gpu stall-probe-test
#include "bbport_stall_probe_rules.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace BbStallProbe;

static void TestThreshold() {
    assert(ThresholdMsFromEnv(nullptr) == 0);
    assert(ThresholdMsFromEnv("") == 0);
    assert(ThresholdMsFromEnv("0") == 0);
    assert(ThresholdMsFromEnv("off") == 0);
    assert(ThresholdMsFromEnv("1") == 250);
    assert(ThresholdMsFromEnv("on") == 250);
    assert(ThresholdMsFromEnv("400") == 400);
    assert(ThresholdMsFromEnv("5") == 20);
    assert(ThresholdMsFromEnv("999999") == 60000);
    assert(ThresholdMsFromEnv("abc") == 0);
    assert(ThresholdMsFromEnv("-3") == 0);
}

static void TestSplit() {
    // 700 ms of main work, then 100 ms waiting in the frame end.
    Split s = SplitFrame(1'000'000'000ull, 1'700'000'000ull, 1'800'000'000ull);
    assert(s.main_ms > 699.9 && s.main_ms < 700.1);
    assert(s.end_ms > 99.9 && s.end_ms < 100.1);
    assert(s.total_ms > 799.9 && s.total_ms < 800.1);
    // The first frame: no previous exit, only the frame end counts.
    s = SplitFrame(0, 5'000'000ull, 9'000'000ull);
    assert(s.main_ms == 0 && s.end_ms > 3.99 && s.end_ms < 4.01);
    // A clock that went backwards is clamped, never negative.
    s = SplitFrame(10, 5, 3);
    assert(s.main_ms == 0 && s.end_ms == 0 && s.total_ms == 0);
}

static void TestTable() {
    SampleTable t;
    for (int i = 0; i < 6; ++i) t.Add(-1, 0x1234, 0);
    for (int i = 0; i < 3; ++i) t.Add(0, 0xee401, 0x114171);
    t.Add(-1, 0x99, 0);
    assert(t.Total() == 10 && t.Size() == 3 && t.Other() == 0);
    SampleTable::Row rows[4];
    assert(t.Top(rows, 4) == 3);
    assert(rows[0].a == 0x1234 && rows[0].count == 6);
    assert(rows[1].module == 0 && rows[1].caller == 0x114171 && rows[1].count == 3);
    const char* mods[] = {"ucrtbase.dll"};
    const std::string f = FormatTop(t, mods, 1, 2);
    assert(f == "+0x1234 60%; host:ucrtbase.dll+0xee401 (from +0x114171) 30%");
    // Full table: new places count as "other".
    SampleTable u;
    for (int i = 0; i < SampleTable::kSlots + 5; ++i) u.Add(-1, u64(i), 0);
    assert(u.Size() == SampleTable::kSlots && u.Other() == 5 && u.Total() == unsigned(SampleTable::kSlots + 5));
    u.Clear();
    assert(u.Total() == 0 && u.Size() == 0 && FormatTop(u, mods, 1, 3).empty());
    // A system-DLL row with the first return address into our exe.
    SampleTable w;
    w.Add(0, 0x1628a4, 0x109ed6c, 0x7b3712 + 1);
    w.Add(0, 0x1628a4, 0x109ed6c, 0x7b3712 + 1);
    w.Add(0, 0x1628a4, 0x109ed6c, 0);  // same place, no exe frame: its own row
    assert(w.Size() == 2);
    assert(FormatTop(w, mods, 1, 1) == "host:ucrtbase.dll+0x1628a4 via exe+0x7b3712 (from +0x109ed6c) 67%");
    // A module index without a name prints "?".
    SampleTable v;
    v.Add(7, 0x10, 0);
    assert(FormatTop(v, mods, 1, 1) == "host:?+0x10 100%");
}

static void TestRateLimit() {
    RateLimit r(2, 1000);
    unsigned d = 99;
    assert(r.Allow(10, &d) && d == 0);
    assert(r.Allow(20, &d) && d == 0);
    assert(!r.Allow(30, &d));
    assert(!r.Allow(40, &d));
    assert(r.Allow(1500, &d) && d == 2);  // a new window reports the two dropped lines
    assert(r.Allow(1600, &d) && d == 0);
}

int main() {
    TestThreshold();
    TestSplit();
    TestTable();
    TestRateLimit();
    std::printf("stall probe rules: ok\n");
    return 0;
}
