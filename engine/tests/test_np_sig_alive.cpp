// SPDX-License-Identifier: GPL-3.0-or-later
// np_sig_alive.h: a dead signaling connection is not announced up; link-silence rules.
// ninja -C out/gpu np-sig-alive-test && out/gpu/np-sig-alive-test.exe
#include "net/np_sig_alive.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

using namespace bbnet::np::sigalive;

int main() {
    CHECK(Deliver(0x01, true, false));
    CHECK(Deliver(0x0c, true, false));
    CHECK(!Deliver(0x01, true, true));    // died between announce and delivery
    CHECK(!Deliver(0x0c, true, true));
    CHECK(!Deliver(0x0c, false, false));  // connection deleted
    CHECK(Deliver(0x00, true, true));     // DEAD always
    CHECK(Deliver(0x00, false, true));

    CHECK(!LinkDead(-1, false));                   // never heard
    CHECK(!LinkDead(1000, false));
    CHECK(!LinkDead(25000, false));                // the transport's own idle timeout is not ours
    CHECK(!LinkDead(60000, false));
    CHECK(LinkDead(60001, false));
    CHECK(!LinkDead(130000, true));                // an outage that resumes is never dead
    CHECK(!LinkDead(180000, true));
    CHECK(LinkDead(180001, true));
    CHECK(LinkDead(8001, false, 8000));            // test override
    CHECK(!LinkDead(20000, true, 8000));           // x3 in an outage
    CHECK(LinkDead(24001, true, 8000));

    const std::uint32_t lo = 0x0100007f;
    std::map<std::uint64_t, std::int64_t> ages = {{lo | (std::uint64_t(0x29bb) << 32), 500},
                                                  {lo | (std::uint64_t(0x2abb) << 32), 90000}};
    CHECK(SourceAge(ages, lo, 0x29bb) == 500);
    CHECK(SourceAge(ages, lo, 0x2abb) == 90000);
    CHECK(SourceAge(ages, lo, 0x2bbb) == -1);       // two candidates, none exact: unknown
    CHECK(SourceAge(ages, 0x0200007f, 0x29bb) == -1);
    ages.erase(lo | (std::uint64_t(0x29bb) << 32));
    CHECK(SourceAge(ages, lo, 0x2bbb) == 90000);     // one candidate for that address (NAT port)
    CHECK(SourceAge({}, lo, 1) == -1);

    std::printf(g_failures ? "FAILED %d\n" : "np-sig-alive: all checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
