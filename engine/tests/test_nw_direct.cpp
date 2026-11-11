// SPDX-License-Identifier: GPL-3.0-or-later
// Opt-in Nightwatch direct path rules (gpu/shim/net/party_nw_direct_rules.h): margin, loss in the probe window,
// sticky fallback, opt-in on both sides, probe wire format, candidate filter, safe moment.
// Build and run: cmake --build out/gpu --target nw-direct-test && out/gpu/nw-direct-test.exe
#include "net/party_nw_direct_rules.h"

#include <cassert>
#include <cstdio>

using namespace bbnet::nwd;

static PathStats Path(int sent, int answered, double rtt) {
    PathStats p;
    p.sent = sent;
    p.answered = answered;
    for (int i = 0; i < answered; ++i) p.rtt_ms.push_back(rtt);
    return p;
}

int main() {
    double t = 0, d = 0;
    // Margin: 92 vs 18 ms switches; 92 vs 75 (17 ms, 18 %) does not; 92 vs 72 (20 ms) does; 40 vs 27 (13 ms, 32 %) does.
    assert(judge(Path(20, 20, 92), Path(20, 20, 18), &t, &d) == Verdict::Direct && t == 92 && d == 18);
    assert(judge(Path(20, 20, 92), Path(20, 20, 75), &t, &d) == Verdict::SlowerTunnel);
    assert(judge(Path(20, 20, 92), Path(20, 20, 72), &t, &d) == Verdict::Direct);
    assert(judge(Path(20, 20, 40), Path(20, 20, 27), &t, &d) == Verdict::Direct);
    assert(judge(Path(20, 20, 40), Path(20, 20, 30), &t, &d) == Verdict::SlowerTunnel);
    assert(judge(Path(20, 20, 10), Path(20, 20, 10), &t, &d) == Verdict::SlowerTunnel);
    // Direct slower than the tunnel: stays.
    assert(judge(Path(20, 20, 30), Path(20, 20, 90), &t, &d) == Verdict::SlowerTunnel);
    // Loss in the probe window: one unanswered probe is enough, however fast the rest.
    assert(judge(Path(20, 20, 92), Path(20, 19, 5), &t, &d) == Verdict::Loss);
    assert(judge(Path(20, 20, 92), Path(20, 0, 0), &t, &d) == Verdict::Loss);
    // A window cut short (fewer than kProbes sent) decides nothing.
    assert(judge(Path(20, 20, 92), Path(12, 12, 5), &t, &d) == Verdict::NoProbes);
    assert(judge(Path(20, 0, 0), Path(20, 20, 5), &t, &d) == Verdict::NoProbes);
    // The median, not the mean: one slow outlier among 20 answers does not matter.
    {
        PathStats dir = Path(20, 20, 18);
        dir.rtt_ms[3] = 400;
        assert(judge(Path(20, 20, 92), dir, &t, &d) == Verdict::Direct && d == 18);
    }
    // Log lines.
    assert(log_line(Verdict::Direct, 92, 18, 0, 20) == "direct path: tunnel 92 ms, direct 18 ms -> direct");
    assert(log_line(Verdict::Loss, 92, 18, 3, 20) == "direct path: tunnel 92 ms, direct 18 ms -> tunnel (loss 3/20)");
    assert(log_line(Verdict::PeerDeclined, 92, 18, 0, 20) ==
           "direct path: tunnel 92 ms, direct 18 ms -> tunnel (peer declined)");
    assert(log_line(Verdict::NoCandidate, 0, 0, 0, 0) == "direct path: -> tunnel (no public address)");
    assert(not_opted_line() == "direct path: -> tunnel (not opted in)");

    // Opt-in on both sides.
    assert(both_opted(true, true) && !both_opted(true, false) && !both_opted(false, true) && !both_opted(false, false));
    {
        Pair p;
        p.flags(true, false);
        assert(p.final_tunnel());
        p.flags(true, true);  // final: a later flag changes nothing
        assert(p.final_tunnel());
    }
    {
        Pair p;
        p.flags(false, true);
        assert(p.final_tunnel());
    }
    // Full run: opted in, probed, both Direct, switch only at a safe moment.
    {
        Pair p;
        assert(p.phase() == Phase::Idle);
        p.flags(true, true);
        assert(p.phase() == Phase::Wait);
        p.start_probing();
        assert(p.phase() == Phase::Probing);
        assert(p.decide(Verdict::Direct, true));
        assert(p.phase() == Phase::Decided);
        assert(!p.switch_now(false) && !p.direct());  // mid-fight: waits
        assert(p.switch_now(true) && p.direct());
        assert(!p.switch_now(true));  // once
    }
    // The peer's own verdict was not Direct (its probe lost a packet): both stay on the tunnel.
    {
        Pair p;
        p.flags(true, true);
        p.start_probing();
        assert(!p.decide(Verdict::Direct, false));
        assert(p.final_tunnel());
    }
    {
        Pair p;
        p.flags(true, true);
        p.start_probing();
        assert(!p.decide(Verdict::Loss, true));
        assert(p.final_tunnel());
        p.start_probing();  // sticky: nothing restarts it
        assert(p.final_tunnel());
    }
    // Sticky fallback: the direct path silent for kLostMs -> tunnel for good; a heard ack resets the timer.
    {
        Pair p;
        p.flags(true, true);
        p.start_probing();
        p.decide(Verdict::Direct, true);
        p.switch_now(true);
        const std::int64_t since = 10000;
        assert(!p.lost(since + 1500, since));
        p.heard(since + 1900);
        assert(!p.lost(since + 3000, since));  // 1.1 s after the last ack
        assert(!p.lost(since + 3899, since));
        assert(p.lost(since + 3900, since) && p.final_tunnel());
        assert(!p.lost(since + 9000, since));
        p.flags(true, true);  // never back to direct
        p.start_probing();
        assert(p.final_tunnel() && !p.direct());
    }
    // Never silent since the switch (no ack at all): lost kLostMs after it.
    {
        Pair p;
        p.flags(true, true);
        p.start_probing();
        p.decide(Verdict::Direct, true);
        p.switch_now(true);
        assert(!p.lost(5000 + kLostMs - 1, 5000));
        assert(p.lost(5000 + kLostMs, 5000));
    }
    // Probe wire format.
    {
        std::uint8_t b[kProbeLen];
        assert(encode_probe(b, sizeof(b), 1, 1, 0xdeadbeef));
        std::uint8_t ty = 9, tg = 9;
        std::uint32_t sq = 0;
        assert(decode_probe(b, sizeof(b), &ty, &tg, &sq) && ty == 1 && tg == 1 && sq == 0xdeadbeef);
        assert(!decode_probe(b, sizeof(b) - 1, &ty, &tg, &sq));
        b[4] = 'x';
        assert(!decode_probe(b, sizeof(b), &ty, &tg, &sq));
        assert(!encode_probe(b, 4, 0, 0, 0));
    }
    // Candidate filter.
    {
        auto ip = [](int a, int b, int c, int d2) { return (std::uint32_t(a) << 24) | (b << 16) | (c << 8) | d2; };
        assert(candidate_ok(ip(203, 0, 113, 5), 9307, false));
        assert(!candidate_ok(ip(10, 8, 0, 5), 9307, false));
        assert(!candidate_ok(ip(192, 168, 1, 5), 9307, false));
        assert(!candidate_ok(ip(172, 20, 1, 5), 9307, false));
        assert(!candidate_ok(ip(100, 64, 1, 5), 9307, false));
        assert(!candidate_ok(ip(169, 254, 1, 5), 9307, false));
        assert(!candidate_ok(ip(127, 0, 0, 2), 9307, false) && candidate_ok(ip(127, 0, 0, 2), 9307, true));
        assert(!candidate_ok(ip(203, 0, 113, 5), 80, false) && !candidate_ok(0, 9307, true));
        assert(!candidate_ok(ip(240, 0, 0, 1), 9307, false));
    }
    // Safe moments: a loading screen on either side, or nobody playing in the host's world.
    assert(safe_pair(1, true, 1, false));                  // host home, guest home
    assert(safe_pair(1, true, 2, false) && safe_pair(0, false, 1, true));  // guest joining / title
    assert(!safe_pair(1, true, 3, false) && !safe_pair(3, false, 1, true));  // a guest playing in the host's world
    assert(!safe_pair(3, true, 4, false) && !safe_pair(4, false, 3, true));  // dead in the world counts as playing
    assert(safe_pair(3, true, 2, false));                  // the host in its own world is not play with this pair
    assert(safe_pair(5, true, 3, false) && safe_pair(3, false, 5, true));  // a load screen on either side
    assert(!safe_pair(3, false, 3, false));                // two guests playing
    assert(safe_pair(3, false, 5, false));
    assert(safe_pair(6, false, 1, true));                  // prologue is the guest's own world
    std::printf("nw-direct-test: ok\n");
    return 0;
}
