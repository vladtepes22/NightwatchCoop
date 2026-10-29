// SPDX-License-Identifier: GPL-3.0-or-later
// Party network library (gpu/shim/net): STUN encode/decode and the host's answer, vport framing,
// the network simulator's delay line, loopback UDP through the guest-facing socket functions,
// the host relay, NP signed-in answers, import routing and the guest-callback dispatcher.
// Build and run: ninja -C out/gpu party-net-test && out/gpu/party-net-test.exe
#include "bbnet_internal.h"
#include "net_stun.h"
#include "netsim.h"
#include "nr_loadgap.h"
#include "nr_outage.h"
#include "nr_rxgap.h"
#include "party_discovery.h"
#include "party_udp.h"
#include "party_util.h"
#include "json.h"
#include "gpu/bbnet.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// --- Runtime stand-ins (src/probe.c, runtime_thread.c, runtime.c) ---
static std::atomic<int> g_fs_restores{0}, g_attached{0};
extern "C" {
BBNET_ABI void restore_guest_fs(void) { g_fs_restores.fetch_add(1); }
void runtime_thread_attach_host(const char*) { g_attached.fetch_add(1); }
const char* runtime_symbol(const char* nid) {
    if (!std::strcmp(nid, "TEST1#A#B")) return "sceNetSocket";
    if (!std::strcmp(nid, "TEST2#A#B")) return "sceNpScoreCreateRequest";
    return nullptr;
}
}

static int g_failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

static void set_env(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

static void* fn(const char* name) {
    void* f = reinterpret_cast<void*>(bbnet_resolve(name));
    if (!f) std::fprintf(stderr, "no party function %s\n", name);
    return f;
}

struct SceAddr {
    std::uint8_t len, family;
    std::uint16_t port;
    std::uint32_t addr;
    std::uint16_t vport;
    std::uint8_t zero[6];
};
static SceAddr sce_addr(const char* ip, std::uint16_t port, std::uint16_t vport) {
    SceAddr a{};
    a.len = 16;
    a.family = 2;
    a.port = htons(port);
    inet_pton(AF_INET, ip, &a.addr);
    a.vport = htons(vport);
    return a;
}

using SocketFn = int(BBNET_ABI*)(const char*, int, int, int);
using BindFn = int(BBNET_ABI*)(int, const void*, int);
using SendtoFn = int(BBNET_ABI*)(int, const void*, std::uint64_t, int, const void*, int);
using RecvfromFn = int(BBNET_ABI*)(int, void*, std::uint64_t, int, void*, unsigned*);
using CloseFn = int(BBNET_ABI*)(int);
using SetsockoptFn = int(BBNET_ABI*)(int, int, int, const void*, int);
using GetsocknameFn = int(BBNET_ABI*)(int, void*, unsigned*);
using EpollCreateFn = int(BBNET_ABI*)(const char*, int);
using EpollControlFn = int(BBNET_ABI*)(int, int, int, void*);
using EpollWaitFn = int(BBNET_ABI*)(int, void*, int, int);
using ErrnoFn = int*(BBNET_ABI*)();
using InitFn = int(BBNET_ABI*)();

static void test_stun() {
    using namespace net::stun;
    std::uint8_t req[kMaxRequest], txid[kTxid];
    const std::uint8_t token[kTokenLen] = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::size_t n = build_binding_request(req, txid, true, token);
    CHECK(n == kHeader + 16);
    CHECK(req[0] == 0x00 && req[1] == 0x01);
    CHECK(req[4] == 0x21 && req[5] == 0x12 && req[6] == 0xA4 && req[7] == 0x42);  // the cookie leads the id
    std::uint8_t got_txid[kTxid], got_token[kTokenLen];
    bool hello = false;
    CHECK(parse_binding_request(req, n, got_txid, &hello, got_token));
    CHECK(hello && std::memcmp(got_token, token, kTokenLen) == 0 && std::memcmp(got_txid, txid, kTxid) == 0);
    // A plain request: no HELLO.
    const std::size_t n2 = build_binding_request(req, txid);
    CHECK(n2 == kHeader);
    CHECK(parse_binding_request(req, n2, got_txid, &hello, got_token) && !hello);
    CHECK(!parse_binding_request(req, 10, got_txid, &hello, got_token));

    // The server's answer, with the relay attribute, read back by the client parser.
    std::uint32_t addr = 0;
    inet_pton(AF_INET, "203.0.113.7", &addr);
    Relay relay;
    relay.present = true;
    std::memcpy(relay.token, token, kTokenLen);
    relay.vport = 50001;
    relay.observed_addr = addr;
    relay.observed_port = 41000;
    std::uint8_t resp[kMaxResponse];
    const std::size_t rn = build_binding_response(resp, txid, addr, 41000, &relay);
    CHECK(rn == kMaxResponse);
    std::uint32_t got_addr = 0;
    std::uint16_t got_port = 0;
    Relay got_relay;
    CHECK(parse_binding_response(resp, rn, txid, &got_addr, &got_port, &got_relay));
    CHECK(got_addr == addr && got_port == 41000);
    CHECK(got_relay.present && got_relay.vport == 50001 && got_relay.observed_addr == addr &&
          got_relay.observed_port == 41000 && std::memcmp(got_relay.token, token, kTokenLen) == 0);
    // XOR-MAPPED-ADDRESS alone: hide MAPPED-ADDRESS behind an unknown type.
    resp[kHeader] = 0x80;
    resp[kHeader + 1] = 0x22;
    got_addr = 0;
    got_port = 0;
    CHECK(parse_binding_response(resp, rn, txid, &got_addr, &got_port, nullptr));
    CHECK(got_addr == addr && got_port == 41000);
    // Another transaction's answer is not ours.
    std::uint8_t other[kTxid];
    std::memcpy(other, txid, kTxid);
    other[kTxid - 1] ^= 1;
    CHECK(!parse_binding_response(resp, rn, other, &got_addr, &got_port, nullptr));
    std::printf("stun: ok\n");
}

// PeerPaths (party_udp.h): direct while probes are answered, the host relay otherwise.
static void test_peer_paths() {
    using bbnet::udp::PeerPaths;
    using C = std::chrono::steady_clock;
    const auto t0 = C::now();
    auto at = [&](int ms) { return t0 + std::chrono::milliseconds(ms); };
    const std::uint32_t a = htonl(0x0a000002), b = htonl(0x0a000003);
    const std::uint16_t pa = htons(9307), pb = htons(9308);
    PeerPaths paths(PeerPaths::Config{1000, 3000, 5000, false});
    paths.add(a, pa, 50010, at(0));
    paths.add(b, pb, 0, at(0));  // no relay port: always direct
    std::vector<std::pair<std::uint32_t, std::uint16_t>> probes;
    std::vector<std::string> notes;
    paths.tick(at(0), &probes, &notes);
    CHECK(probes.size() == 2);  // both probed at once
    probes.clear();
    paths.tick(at(500), &probes, &notes);
    CHECK(probes.empty());      // once a second
    std::uint16_t rp = 0;
    paths.tick(at(2900), &probes, &notes);
    CHECK(!paths.relayed(a, pa, &rp));  // still trying directly
    paths.tick(at(3100), &probes, &notes);
    CHECK(paths.relayed(a, pa, &rp) && rp == 50010);  // no answer for 3 s: relay
    CHECK(!paths.relayed(b, pb, &rp));                 // nowhere to fall back to
    CHECK(notes.size() == 1);
    std::uint32_t da = 0;
    std::uint16_t dp = 0;
    CHECK(paths.direct_of_relay(50010, &da, &dp) && da == a && dp == pa);
    // An answer: direct again at the next tick.
    paths.on_ack(a, pa, at(4000));
    paths.tick(at(4100), &probes, &notes);
    CHECK(!paths.relayed(a, pa, &rp));
    // Answered before, then silent: relay after 5 s without an ack.
    paths.tick(at(8900), &probes, &notes);
    CHECK(!paths.relayed(a, pa, &rp));
    paths.tick(at(9100), &probes, &notes);
    CHECK(paths.relayed(a, pa, &rp));
    CHECK(paths.peers().size() == 2 && notes.size() == 3);
    // An unknown source and an unknown relay port: nothing.
    CHECK(!paths.relayed(htonl(0x0a000009), pa, &rp) && !paths.direct_of_relay(50099, &da, &dp));
    // BB_PARTY_FORCE_RELAY: relayed from the start, whatever the probes say.
    PeerPaths forced(PeerPaths::Config{1000, 3000, 5000, true});
    forced.add(a, pa, 50010, at(0));
    forced.on_ack(a, pa, at(10));
    forced.tick(at(20), &probes, &notes);
    CHECK(forced.relayed(a, pa, &rp));
    std::printf("peer paths: ok\n");
}

// 0.7.6 (guestmesh): the guest-to-guest path chooser with hysteresis and game traffic gaps.
static void test_peer_paths_gaps() {
    using bbnet::udp::PeerPaths;
    using C = std::chrono::steady_clock;
    const auto t0 = C::now();
    auto at = [&](int ms) { return t0 + std::chrono::milliseconds(ms); };
    const std::uint32_t a = htonl(0x0a000002), b = htonl(0x0a000003);
    const std::uint16_t pa = htons(9307), pb = htons(9308);
    PeerPaths::Config cfg{500, 2500, 2500, false};
    cfg.hold_ms = 10000;
    cfg.stable_ms = 4000;
    cfg.gaps_to_relay = 2;
    cfg.gap_window_ms = 60000;
    PeerPaths paths(cfg);
    paths.add(a, pa, 50010, at(0));
    paths.add(b, pb, 0, at(0));  // no relay port: nothing to fall back to
    std::vector<std::pair<std::uint32_t, std::uint16_t>> probes;
    std::vector<std::string> notes;
    std::uint16_t rp = 0;
    int t = 0;
    auto run = [&](int until, bool acks) {
        for (; t <= until; t += 100) {
            if (acks && t % 500 == 0) {
                paths.on_ack(a, pa, at(t));
                paths.on_ack(b, pb, at(t));
            }
            paths.tick(at(t), &probes, &notes);
        }
    };
    run(3000, true);
    CHECK(!paths.relayed(a, pa, &rp));
    // One gap is nothing; the second within the window moves the pair to the relay (probes pass).
    std::string note;
    CHECK(!paths.note_gap(a, pa, at(3000), &note));
    CHECK(!paths.relayed(a, pa, &rp));
    CHECK(paths.note_gap(a, pa, at(9000), &note) && !note.empty());
    CHECK(paths.relayed(a, pa, &rp) && rp == 50010);
    CHECK(!paths.note_gap(b, pb, at(9000), &note) && !paths.relayed(b, pb, &rp));  // no relay port
    // Held: probes keep being answered, the pair stays on the relay for hold_ms.
    t = 9100;
    run(18000, true);
    CHECK(paths.relayed(a, pa, &rp));
    run(19500, true);  // held 10 s, stable for far more than 4 s: direct again
    CHECK(!paths.relayed(a, pa, &rp));
    // An old gap leaves the window: a gap 70 s later is a first gap again.
    CHECK(!paths.note_gap(a, pa, at(90000), &note));
    CHECK(!paths.relayed(a, pa, &rp));
    // A flapping direct path: relayed for loss, back only after 4 s of unbroken answers + the hold.
    PeerPaths flap(cfg);
    flap.add(a, pa, 50010, at(0));
    t = 0;
    for (; t <= 4000; t += 100) {
        if (t <= 1000 && t % 500 == 0) flap.on_ack(a, pa, at(t));
        flap.tick(at(t), &probes, &notes);
    }
    CHECK(flap.relayed(a, pa, &rp));  // 2.5 s without an answer
    int back_at = -1;
    for (; t <= 40000 && back_at < 0; t += 100) {
        // answers in bursts of 3 s with a 1 s break: never 4 s unbroken
        const bool answer = (t / 500) % 8 < 6;
        if (answer && t % 500 == 0) flap.on_ack(a, pa, at(t));
        flap.tick(at(t), &probes, &notes);
        if (!flap.relayed(a, pa, &rp)) back_at = t;
    }
    CHECK(back_at < 0);  // flapping: stays on the relay
    int stable_from = t;
    for (; t <= stable_from + 12000 && back_at < 0; t += 100) {
        if (t % 500 == 0) flap.on_ack(a, pa, at(t));
        flap.tick(at(t), &probes, &notes);
        if (!flap.relayed(a, pa, &rp)) back_at = t;
    }
    CHECK(back_at >= stable_from + 3500 && back_at <= stable_from + 6000);  // ~4 s of answers
    // Gaps that go on over the relay: the relay did not help, direct again and no gap relay for 5 min.
    PeerPaths g2(cfg);
    g2.add(a, pa, 50010, at(0));
    for (t = 0; t <= 1000; t += 100) {
        if (t % 500 == 0) g2.on_ack(a, pa, at(t));
        g2.tick(at(t), &probes, &notes);
    }
    g2.note_gap(a, pa, at(1000), &note);
    CHECK(g2.note_gap(a, pa, at(2000), &note) && g2.relayed(a, pa, &rp));
    CHECK(!g2.note_gap(a, pa, at(5000), &note) && !g2.relayed(a, pa, &rp));
    CHECK(!g2.note_gap(a, pa, at(6000), &note) && !g2.note_gap(a, pa, at(7000), &note) && !g2.relayed(a, pa, &rp));
    // Off by default (gaps_to_relay 0).
    PeerPaths off(PeerPaths::Config{500, 2500, 2500, false});
    off.add(a, pa, 50010, at(0));
    CHECK(!off.note_gap(a, pa, at(1), &note) && !off.note_gap(a, pa, at(2), &note) && !off.relayed(a, pa, &rp));
    std::printf("peer paths gaps: ok\n");
}

// netsim per-peer block (BB_NET_SIM block=PORT+PORT,block_at,block_for): only datagrams to those
// ports, only in the window.
static void test_netsim_block() {
    using namespace bbnet::netsim;
    Config c;
    std::string err;
    CHECK(parse("lat=0,block=15822+15823,block_at=1000,block_for=2000", &c, &err));
    CHECK(c.block_ports.size() == 2 && c.block_ports[0] == 15822 && c.block_at_ms == 1000 && c.block_for_ms == 2000);
    CHECK(!parse("block=0", &c, &err) && !parse("block=70000", &c, &err));
    CHECK(parse("lat=0,block=15822+15823,block_at=1000,block_for=2000", &c, &err));
    const auto t0 = Clock::now();
    auto at = [&](int ms) { return t0 + std::chrono::milliseconds(ms); };
    CHECK(!blocked_at(c, t0, at(500), htons(15822)));
    CHECK(blocked_at(c, t0, at(1500), htons(15822)) && blocked_at(c, t0, at(1500), htons(15823)));
    CHECK(!blocked_at(c, t0, at(1500), htons(9307)));   // the host's port stays open
    CHECK(!blocked_at(c, t0, at(3500), htons(15822)));  // the window is over
    CHECK(parse("lat=0,block=15822", &c, &err) && blocked_at(c, t0, at(999999), htons(15822)));  // for good
    Queue q(c);
    q.set_epoch(t0);
    const std::uint32_t v = 1;
    CHECK(q.submit(at(10), 1, 0x0100007f, htons(15822), &v, 4) == 0);
    CHECK(q.submit(at(10), 1, 0x0100007f, htons(9307), &v, 4) == 1);
    std::printf("netsim block: ok\n");
}

// EndpointAliases / punch_choice (party_udp.h): a peer heard on its LAN address keeps its public
// address in the game; the local punch candidate wins when heard.
static void test_aliases() {
    using bbnet::udp::EndpointAliases;
    using bbnet::party::ip_parse;
    EndpointAliases al;
    const std::uint32_t pub = ip_parse("89.27.12.144"), lan = ip_parse("192.168.1.112");
    const std::uint16_t p = htons(9307), lp = htons(9307);
    std::uint32_t a = pub;
    std::uint16_t q = p;
    CHECK(!al.outgoing(&a, &q) && a == pub);
    al.set(pub, p, lan, lp);
    CHECK(al.outgoing(&a, &q) && a == lan && q == lp);
    CHECK(al.incoming(&a, &q) && a == pub && q == p);
    std::uint32_t other = ip_parse("192.168.1.50");
    std::uint16_t op = lp;
    CHECK(!al.incoming(&other, &op) && other == ip_parse("192.168.1.50"));
    // Re-pointing replaces the old alias both ways.
    const std::uint32_t lan2 = ip_parse("10.0.0.5");
    al.set(pub, p, lan2, lp);
    a = lan;
    q = lp;
    CHECK(!al.incoming(&a, &q));
    a = pub;
    q = p;
    CHECK(al.outgoing(&a, &q) && a == lan2);
    al.clear(pub, p);
    a = pub;
    CHECK(!al.outgoing(&a, &q) && al.size() == 0);
    CHECK(bbnet::udp::punch_choice(true, true, true) == 1);
    CHECK(bbnet::udp::punch_choice(false, true, true) == 1);
    CHECK(bbnet::udp::punch_choice(true, false, true) == 0);
    CHECK(bbnet::udp::punch_choice(false, false, false) == 0);
    // Heard on a VPN/LAN address inside our subnet: that path stays, whatever "local" address
    // the peer also reports (2026-10-25: guest home address picked over the Radmin one).
    CHECK(bbnet::udp::punch_choice(true, true, true, true) == 0);
    CHECK(bbnet::udp::punch_choice(false, true, true, true) == 1);
    {
        using bbnet::udp::OwnIf;
        const std::vector<OwnIf> own = {{ip_parse("26.121.14.68"), ip_parse("255.0.0.0")},
                                        {ip_parse("192.168.50.4"), ip_parse("255.255.255.0")}};
        CHECK(bbnet::udp::on_own_subnet(ip_parse("26.76.171.64"), own));
        CHECK(!bbnet::udp::on_own_subnet(ip_parse("26.121.14.68"), own));  // ourselves
        CHECK(!bbnet::udp::on_own_subnet(ip_parse("192.168.2.21"), own));
        CHECK(!bbnet::udp::on_own_subnet(ip_parse("66.231.158.8"), own));
        // A peer that reports our own VPN address as its local one: never usable.
        CHECK(!bbnet::udp::local_candidate_usable(ip_parse("26.121.14.68"), htons(9307), htons(9307), own, 0, 0));
        // Its home address (not on our networks, different public IP): not usable either.
        CHECK(!bbnet::udp::local_candidate_usable(ip_parse("192.168.2.21"), htons(9307), htons(9307), own,
                                                  ip_parse("66.231.158.8"), ip_parse("203.0.113.9")));
    }
    using bbnet::party::ip_is_private;
    CHECK(ip_is_private(ip_parse("192.168.1.112")) && ip_is_private(ip_parse("10.1.2.3")) &&
          ip_is_private(ip_parse("172.20.0.1")) && ip_is_private(ip_parse("100.100.1.1")));
    CHECK(!ip_is_private(ip_parse("89.27.12.144")) && !ip_is_private(ip_parse("172.32.0.1")) &&
          !ip_is_private(ip_parse("127.0.0.1")) && !ip_is_private(ip_parse("100.128.0.1")));
    std::printf("endpoint aliases: ok\n");
}

// Two homes with the same router-assigned LAN address (Radmin / Hamachi players, 2026-10-23): the
// peer's local candidate is our own endpoint and must never be probed or counted as heard.
static void test_same_lan_address() {
    using bbnet::party::ip_parse;
    using bbnet::udp::OwnIf;
    using bbnet::udp::is_own_endpoint;
    using bbnet::udp::local_candidate_usable;
    const std::uint32_t mask = ip_parse("255.255.255.0");
    const std::vector<OwnIf> own = {{ip_parse("192.168.1.50"), mask}, {ip_parse("127.0.0.1"), ip_parse("255.0.0.0")}};
    const std::uint16_t port = htons(9307);
    const std::uint32_t pub_a = ip_parse("203.0.113.5"), pub_b = ip_parse("198.51.100.7");
    // Same local IP and port on both sides, different public IPs: dropped.
    CHECK(!local_candidate_usable(ip_parse("192.168.1.50"), port, port, own, pub_a, pub_b));
    CHECK(!local_candidate_usable(ip_parse("192.168.1.50"), port, port, own, 0, 0));
    CHECK(!local_candidate_usable(ip_parse("192.168.1.50"), port, port, own, pub_a, pub_a));
    // Same public IP, different local IPs (two PCs in one home): the local address stays.
    CHECK(local_candidate_usable(ip_parse("192.168.1.20"), port, port, own, pub_a, pub_a));
    CHECK(local_candidate_usable(ip_parse("10.0.5.9"), port, port, own, pub_a, pub_a));
    // Different public IP but inside our subnet: kept.
    CHECK(local_candidate_usable(ip_parse("192.168.1.20"), port, port, own, pub_a, pub_b));
    // Another subnet and another public IP: not our network.
    CHECK(!local_candidate_usable(ip_parse("192.168.7.20"), port, port, own, pub_a, pub_b));
    // Same machine, another port (two instances): kept. No adapter list: kept.
    CHECK(local_candidate_usable(ip_parse("127.0.0.1"), htons(9308), port, own, 0, 0));
    CHECK(local_candidate_usable(ip_parse("192.168.1.50"), htons(9308), port, own, pub_a, pub_a));
    CHECK(local_candidate_usable(ip_parse("192.168.9.9"), port, port, {}, 0, 0));
    // A datagram from our own endpoint is ours; from a peer with another port or address, not.
    CHECK(is_own_endpoint(ip_parse("192.168.1.50"), port, port, own));
    CHECK(!is_own_endpoint(ip_parse("192.168.1.50"), htons(9308), port, own));
    CHECK(!is_own_endpoint(ip_parse("192.168.1.14"), port, port, own));
    CHECK(!is_own_endpoint(ip_parse("203.0.113.9"), port, port, own));
    std::printf("same LAN address: ok\n");
}

static void test_netsim() {
    using namespace bbnet::netsim;
    Config c;
    std::string err;
    CHECK(parse("lat=80,jitter=20,loss=2,dup=0.5,reorder=1,seed=7", &c, &err));
    CHECK(c.enabled && c.lat_ms == 80 && c.jitter_ms == 20 && c.loss_pct == 2 && c.dup_pct == 0.5 &&
          c.reorder_pct == 1 && c.seed == 7);
    CHECK(!parse("lat=abc", &c, &err));
    CHECK(!parse("speed=3", &c, &err));
    CHECK(parse("off", &c, &err) && !c.enabled);
    CHECK(parse("dsl", &c, &err) && c.enabled && c.lat_ms == 80 && c.jitter_ms == 20 && c.loss_pct == 2 &&
          c.dup_pct == 0.5 && c.reorder_pct == 1);
    CHECK(parse("bad", &c, &err) && c.lat_ms == 200 && c.jitter_ms == 80 && c.loss_pct == 8 && c.dup_pct == 0);
    CHECK(parse("dsl,seed=3,outage=5000,every=60000", &c, &err) && c.lat_ms == 80 && c.seed == 3 &&
          c.outage_ms == 5000 && c.outage_every_ms == 60000);
    {
        // Burst outages: nothing gets through for 5 s once a minute, from the first minute on.
        CHECK(parse("lat=10,outage=5000,every=60000", &c, &err));
        Queue q(c);
        const auto t0 = Clock::now();
        int through = 0, lost = 0;
        for (int ms = 0; ms < 130000; ms += 100) {
            const std::uint32_t v = static_cast<std::uint32_t>(ms);
            const int n = q.submit(t0 + std::chrono::milliseconds(ms), 1, 0x0100007f, htons(9), &v, sizeof(v));
            const bool out = (ms >= 60000 && ms < 65000) || (ms >= 120000 && ms < 125000);
            if (n == 0) ++lost;
            else ++through;
            CHECK((n == 0) == out);
        }
        CHECK(lost == 100 && q.stats().blacked_out == 100);
        // On demand (tests): blackout(from, until).
        Queue q2(Config{true, 1, 0, 0, 0, 0, 0, 0, 1});
        q2.blackout(t0 + std::chrono::seconds(1), t0 + std::chrono::seconds(2));
        const std::uint32_t v = 1;
        CHECK(q2.submit(t0, 1, 0x0100007f, htons(9), &v, 4) == 1);
        CHECK(q2.submit(t0 + std::chrono::milliseconds(1500), 1, 0x0100007f, htons(9), &v, 4) == 0);
        CHECK(q2.submit(t0 + std::chrono::milliseconds(2000), 1, 0x0100007f, htons(9), &v, 4) == 1);
        (void)through;
    }

    // The statistics over many datagrams.
    CHECK(parse("lat=50,jitter=10,loss=10,dup=5,reorder=5,seed=7", &c, &err));
    Queue q(c);
    const auto t0 = Clock::now();
    const int kN = 20000;
    for (int i = 0; i < kN; ++i) {
        std::uint32_t v = static_cast<std::uint32_t>(i);
        q.submit(t0 + std::chrono::milliseconds(i), 1, 0x0100007f, htons(9), &v, sizeof(v));
    }
    std::vector<Packet> out;
    q.take_due(t0 + std::chrono::milliseconds(kN - 1), out);
    CHECK(!out.empty());
    q.take_due(t0 + std::chrono::hours(1), out);
    CHECK(q.size() == 0);
    const Stats& s = q.stats();
    const double loss = 100.0 * s.dropped / kN, dup = 100.0 * s.duplicated / (kN - s.dropped);
    CHECK(loss > 8.5 && loss < 11.5);
    CHECK(dup > 4.0 && dup < 6.0);
    CHECK(out.size() == static_cast<std::size_t>(kN - s.dropped + s.duplicated));
    // Delivered earliest first; every datagram at least lat - jitter late; out of order only
    // where reordered (each reordered datagram can be overtaken by a handful).
    int inversions = 0;
    std::uint32_t last = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::uint32_t v;
        std::memcpy(&v, out[i].data.data(), 4);
        const auto sent = t0 + std::chrono::milliseconds(v);
        CHECK(out[i].due - sent >= std::chrono::milliseconds(40) - std::chrono::microseconds(1));
        if (i && out[i].due < out[i - 1].due) CHECK(false);
        if (i && v < last) ++inversions;
        last = v;
    }
    CHECK(inversions > 0);
    CHECK(inversions < static_cast<int>(s.reordered) * 2 + 10);

    // Without reorder, jitter alone keeps a destination's order.
    Config c2;
    CHECK(parse("lat=30,jitter=25,seed=3", &c2, &err));
    Queue q2(c2);
    for (int i = 0; i < 2000; ++i) {
        std::uint32_t v = static_cast<std::uint32_t>(i);
        q2.submit(t0 + std::chrono::microseconds(i * 300), 1, 0x0100007f, htons(9), &v, sizeof(v));
    }
    std::vector<Packet> out2;
    q2.take_due(t0 + std::chrono::hours(1), out2);
    CHECK(out2.size() == 2000);
    bool ordered = true;
    for (std::size_t i = 0; i < out2.size(); ++i) {
        std::uint32_t v;
        std::memcpy(&v, out2[i].data.data(), 4);
        if (v != i) ordered = false;
    }
    CHECK(ordered);

    {
        // jit alias, one-shot outage `at` (from the epoch), loopback-only unless any=1.
        Config c2;
        CHECK(parse("lat=60,jit=15,loss=1,outage=5000,at=30000", &c2, &err) && c2.jitter_ms == 15 &&
              c2.outage_at_ms == 30000 && c2.outage_ms == 5000 && !c2.any_addr);
        CHECK(describe(c2).find("at=30000") != std::string::npos);
        const auto e = Clock::now();
        Clock::time_point end;
        CHECK(!outage_at(c2, e, e, e + std::chrono::seconds(29), &end));
        CHECK(outage_at(c2, e, e, e + std::chrono::seconds(31), &end) && end == e + std::chrono::seconds(35));
        CHECK(!outage_at(c2, e, e, e + std::chrono::seconds(36), &end));
        const std::uint32_t lo = 0x0100007f, lan = 0x0a01a8c0;  // 127.0.0.1, 192.168.1.10 (network order)
        CHECK(applies_to(c2, lo) && !applies_to(c2, lan));
        CHECK(parse("lat=60,any=1", &c2, &err) && applies_to(c2, lan));
        Config off;
        CHECK(!applies_to(off, lo));
        // Queue: the one-shot outage drops everything inside it, nothing outside.
        CHECK(parse("lat=10,outage=1000,at=2000", &c2, &err));
        Queue q(c2);
        q.set_epoch(e);
        const std::uint8_t d[4] = {1, 2, 3, 4};
        CHECK(q.submit(e + std::chrono::milliseconds(1500), 1, lo, 1, d, 4) == 1);
        CHECK(q.submit(e + std::chrono::milliseconds(2500), 1, lo, 1, d, 4) == 0);
        CHECK(q.submit(e + std::chrono::milliseconds(3100), 1, lo, 1, d, 4) == 1);
        CHECK(q.stats().blacked_out == 1);
        // StreamDelay: in order, at least the latency, retransmission delays, outage hold.
        CHECK(parse("lat=50,jit=20,loss=10,seed=5,outage=1000,at=2000", &c2, &err));
        StreamDelay sd(c2, e, 1);
        Clock::time_point prev{};
        bool ordered = true, late_enough = true;
        for (int i = 0; i < 1000; ++i) {
            const Clock::time_point now = e + std::chrono::milliseconds(i);
            const Clock::time_point due = sd.schedule(now);
            ordered = ordered && due >= prev;
            late_enough = late_enough && due >= now + std::chrono::milliseconds(30);
            prev = due;
        }
        CHECK(ordered && late_enough);
        CHECK(sd.retransmits() > 50 && sd.retransmits() < 160);  // ~10 % of 1000
        const Clock::time_point held = sd.schedule(e + std::chrono::milliseconds(2200));
        CHECK(held >= e + std::chrono::milliseconds(3000 + 50) && sd.outage_holds() == 1);
        // outage_now: a process-wide window (director steps host_outage / guest_outage).
        CHECK(!blackout_at(Clock::now(), nullptr));
        outage_now(80);
        CHECK(blackout_at(Clock::now(), nullptr));
        {
            StreamDelay s3(c2, Clock::now() - std::chrono::hours(1), 2);
            const Clock::time_point t = Clock::now();
            CHECK(s3.schedule(t) >= t + std::chrono::milliseconds(70) && s3.outage_holds() == 1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(!blackout_at(Clock::now(), nullptr));
        std::printf("netsim: stream delay ok (%llu retransmits of 1000)\n",
                    static_cast<unsigned long long>(sd.retransmits()));
    }
    std::printf("netsim: ok (loss %.2f%%, dup %.2f%%, reordered %llu, inversions %d)\n", loss, dup,
                static_cast<unsigned long long>(s.reordered), inversions);
}

// A raw host socket on loopback, for looking at the wire.
static int raw_udp(std::uint16_t* port) {
    const auto fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    ::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    socklen_t sl = sizeof(sa);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &sl);
    *port = ntohs(sa.sin_port);
    return static_cast<int>(fd);
}
static int raw_recv(int fd, std::uint8_t* buf, int len, int timeout_ms) {
#if defined(_WIN32)
    WSAPOLLFD p{static_cast<SOCKET>(fd), POLLIN, 0};
    if (WSAPoll(&p, 1, timeout_ms) <= 0) return -1;
#else
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0) return -1;
#endif
    return static_cast<int>(::recv(fd, reinterpret_cast<char*>(buf), len, 0));
}
static void raw_send(int fd, const void* buf, int len, std::uint16_t port) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    sa.sin_port = htons(port);
    ::sendto(fd, static_cast<const char*>(buf), len, 0, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
}

static void test_sockets(std::uint16_t party_port) {
    auto init = reinterpret_cast<InitFn>(fn("sceNetInit"));
    auto sock = reinterpret_cast<SocketFn>(fn("sceNetSocket"));
    auto bind = reinterpret_cast<BindFn>(fn("sceNetBind"));
    auto sendto = reinterpret_cast<SendtoFn>(fn("sceNetSendto"));
    auto recvfrom = reinterpret_cast<RecvfromFn>(fn("sceNetRecvfrom"));
    auto close = reinterpret_cast<CloseFn>(fn("sceNetSocketClose"));
    auto setsockopt = reinterpret_cast<SetsockoptFn>(fn("sceNetSetsockopt"));
    auto getsockname = reinterpret_cast<GetsocknameFn>(fn("sceNetGetsockname"));
    auto ep_create = reinterpret_cast<EpollCreateFn>(fn("sceNetEpollCreate"));
    auto ep_ctl = reinterpret_cast<EpollControlFn>(fn("sceNetEpollControl"));
    auto ep_wait = reinterpret_cast<EpollWaitFn>(fn("sceNetEpollWait"));
    auto errno_loc = reinterpret_cast<ErrnoFn>(fn("sceNetErrnoLoc"));
    if (!init || !sock || !bind || !sendto || !recvfrom || !close || !setsockopt || !getsockname || !ep_create ||
        !ep_ctl || !ep_wait || !errno_loc) {
        CHECK(false);
        return;
    }
    CHECK(init() == 0);

    // --- Plain UDP over loopback ---
    const int a = sock("a", 2, 2, 0), b = sock("b", 2, 2, 0);
    CHECK(a > 0 && b > 0);
    SceAddr any = sce_addr("127.0.0.1", 0, 0);
    CHECK(bind(a, &any, 16) == 0);
    CHECK(bind(b, &any, 16) == 0);
    SceAddr bname{};
    unsigned blen = 16;
    CHECK(getsockname(b, &bname, &blen) == 0 && bname.port != 0);
    const int one = 1;
    CHECK(setsockopt(b, 0xffff, 0x1200, &one, 4) == 0);  // non-blocking
    char buf[256];
    SceAddr from{};
    unsigned fromlen = 16;
    CHECK(recvfrom(b, buf, sizeof(buf), 0, &from, &fromlen) == static_cast<int>(0x80410123));  // EAGAIN
    CHECK(*errno_loc() == 35);
    const int ep = ep_create("test", 0);
    struct {
        std::uint32_t events, pad;
        std::uint64_t ident, data;
    } ev{1, 0, 0, 0x1234}, evs[4];
    CHECK(ep_ctl(ep, 1, b, &ev) == 0);
    CHECK(ep_wait(ep, evs, 4, 20000) == 0);  // nothing yet: 20 ms timeout
    CHECK(sendto(a, "hello", 5, 0, &bname, 16) == 5);
    CHECK(ep_wait(ep, evs, 4, 2000000) == 1 && evs[0].data == 0x1234 && (evs[0].events & 1));
    std::memset(buf, 0, sizeof(buf));
    CHECK(recvfrom(b, buf, sizeof(buf), 0, &from, &fromlen) == 5 && std::memcmp(buf, "hello", 5) == 0);
    CHECK(from.addr == htonl(0x7f000001));
    // Blocking receive, woken by a sender thread.
    const int zero = 0;
    CHECK(setsockopt(b, 0xffff, 0x1200, &zero, 4) == 0);
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        sendto(a, "later", 5, 0, &bname, 16);
    });
    CHECK(recvfrom(b, buf, sizeof(buf), 0, &from, &fromlen) == 5 && std::memcmp(buf, "later", 5) == 0);
    t.join();
    CHECK(close(a) == 0 && close(b) == 0);
    std::printf("udp loopback: ok\n");

    // --- P2P sockets on the party port: vport framing ---
    const int p40 = sock("p2p40", 2, 6, 0), p30 = sock("p2p30", 2, 6, 0);
    SceAddr game40 = sce_addr("0.0.0.0", 3658, 40), game30 = sce_addr("0.0.0.0", 3658, 30);
    CHECK(bind(p40, &game40, 16) == 0);
    CHECK(bind(p30, &game30, 16) == 0);
    CHECK(bbnet::p2p_bound_port() == party_port);
    SceAddr name40{};
    unsigned n40 = 16;
    CHECK(getsockname(p40, &name40, &n40) == 0 && ntohs(name40.port) == party_port && ntohs(name40.vport) == 40);
    // 40 -> ourselves, vport 30: through the wire and the reader.
    SceAddr to30 = sce_addr("127.0.0.1", party_port, 30);
    CHECK(sendto(p40, "abc", 3, 0, &to30, 16) == 3);
    std::memset(buf, 0, sizeof(buf));
    from = SceAddr{};
    CHECK(recvfrom(p30, buf, sizeof(buf), 0, &from, &fromlen) == 3 && std::memcmp(buf, "abc", 3) == 0);
    CHECK(ntohs(from.vport) == 40 && ntohs(from.port) == party_port);
    // The wire: a P2P datagram to a raw socket carries [ff][c3][src][dst].
    std::uint16_t raw_port = 0;
    const int raw = raw_udp(&raw_port);
    SceAddr to_raw = sce_addr("127.0.0.1", raw_port, 30);
    CHECK(sendto(p40, "xyz", 3, 0, &to_raw, 16) == 3);
    std::uint8_t wire[64];
    CHECK(raw_recv(raw, wire, sizeof(wire), 2000) == 7);
    CHECK(wire[0] == 0xff && wire[1] == 0xc3 && wire[2] == 40 && wire[3] == 30 && std::memcmp(wire + 4, "xyz", 3) == 0);
    // Two-byte vports from a peer (flag 0x40 clear) reach vport 30 too.
    const std::uint8_t wide[] = {0xff, 0x83, 0x01, 0x2c, 0x00, 0x1e, 'w', 'd'};
    raw_send(raw, wide, sizeof(wide), party_port);
    CHECK(recvfrom(p30, buf, sizeof(buf), 0, &from, &fromlen) == 2 && std::memcmp(buf, "wd", 2) == 0);
    CHECK(ntohs(from.vport) == 300 && ntohs(from.port) == raw_port);
    // A hole-punch probe never reaches a game socket; the datagram after it does.
    const std::uint8_t probe[8] = {0xfe, 'b', 'b', 'h', 'p', 0, 0, 0};
    raw_send(raw, probe, sizeof(probe), party_port);
    const std::uint8_t after[] = {0xff, 0xc3, 40, 30, 'o', 'k'};
    raw_send(raw, after, sizeof(after), party_port);
    CHECK(recvfrom(p30, buf, sizeof(buf), 0, &from, &fromlen) == 2 && std::memcmp(buf, "ok", 2) == 0);
    // ... and is answered with a probe ack (the direct path works both ways: PeerPaths).
    CHECK(raw_recv(raw, wire, sizeof(wire), 2000) == 8 && bbnet::udp::is_probe_ack(wire, 8));
    // The host answers a STUN Binding Request on the party port.
    std::uint8_t req[net::stun::kMaxRequest], txid[net::stun::kTxid];
    const std::size_t rq = net::stun::build_binding_request(req, txid);
    raw_send(raw, req, static_cast<int>(rq), party_port);
    const int rn = raw_recv(raw, wire, sizeof(wire), 2000);
    std::uint32_t mapped = 0;
    std::uint16_t mapped_port = 0;
    CHECK(rn > 0 && net::stun::parse_binding_response(wire, static_cast<std::size_t>(rn), txid, &mapped, &mapped_port));
    CHECK(mapped == htonl(0x7f000001) && mapped_port == raw_port);
    std::printf("p2p vport framing + stun server: ok\n");

    // --- The session layer's STUN query (to ourselves) with the relay HELLO ---
    std::uint32_t self_addr = 0;
    std::uint16_t self_port = 0;
    bbnet::RelayInfo relay;
    CHECK(bbnet::p2p_stun("127.0.0.1", party_port, 2000, &self_addr, &self_port, true, &relay));
    CHECK(self_addr == htonl(0x7f000001) && self_port == party_port);
    CHECK(relay.present && relay.vport >= 50001);
    std::uint32_t server = 0;
    std::uint16_t relay_vport = 0;
    CHECK(bbnet::p2p_relay(&server, &relay_vport) && relay_vport == relay.vport);
    // A port on the host's address that is not a known relay port goes as addressed (a player
    // sharing the host's address); route_to_peer remembers the relay ports it routes to.
    {
        std::uint8_t hdr[bbnet::udp::kRelayHeader];
        std::uint32_t sa = 0;
        std::uint16_t sp = 0;
        CHECK(!bbnet::p2p_relay_client().frame_for(server, htons(relay.vport), hdr, &sa, &sp));
        const std::uint16_t peer = relay.vport == 51234 ? 51235 : 51234;
        const bbnet::udp::PeerRoute r = bbnet::udp::route_to_peer(bbnet::p2p_relay_client(), server, 40000, peer, true);
        CHECK(r.relayed && r.addr == server && r.port == peer);
        CHECK(bbnet::p2p_relay_client().frame_for(server, htons(peer), hdr, &sa, &sp));
        // Ourselves, the host itself (its party port) and anything without force: direct.
        CHECK(!bbnet::udp::route_to_peer(bbnet::p2p_relay_client(), server, 40000, relay.vport, true).relayed);
        CHECK(!bbnet::udp::route_to_peer(bbnet::p2p_relay_client(), server, party_port, peer, true).relayed);
        CHECK(!bbnet::udp::route_to_peer(bbnet::p2p_relay_client(), server, 40000, peer, false).relayed);
        bbnet::p2p_relay_client().add_relay_port(relay.vport);  // the round trip below: to ourselves
    }
    // Relay round trip: a datagram for our own relay port goes framed to the host (ourselves),
    // is forwarded back as [fb]['r'][port] and reaches vport 30 from 127.0.0.1:<relay port>.
    SceAddr via_relay = sce_addr("127.0.0.1", relay.vport, 30);
    CHECK(sendto(p40, "rly", 3, 0, &via_relay, 16) == 3);
    CHECK(recvfrom(p30, buf, sizeof(buf), 0, &from, &fromlen) == 3 && std::memcmp(buf, "rly", 3) == 0);
    CHECK(ntohs(from.port) == relay.vport && ntohs(from.vport) == 40);
    std::printf("relay: ok (port %u)\n", relay.vport);
    std::printf("status: %s\n", bbnet_status_line());

    CHECK(close(p40) == 0 && close(p30) == 0);
#if defined(_WIN32)
    closesocket(static_cast<SOCKET>(raw));
#else
    ::close(raw);
#endif
}

static void test_np() {
    using GetStateFn = int(BBNET_ABI*)(int, int*);
    using OnlineIdFn = int(BBNET_ABI*)(int, void*);
    using AuthCodeFn = int(BBNET_ABI*)(int, const void*, char*, int*);
    using CheckPlusFn = int(BBNET_ABI*)(int, const void*, std::uint8_t*);
    using CreateReqFn = int(BBNET_ABI*)(int, const char*, const char*, int, const void*, std::int64_t*);
    using ReadFn = int(BBNET_ABI*)(std::int64_t, void*, std::uint64_t);
    using RegStateFn = int(BBNET_ABI*)(void*, void*);
    using CheckCbFn = int(BBNET_ABI*)();
    int state = 0;
    CHECK(reinterpret_cast<GetStateFn>(fn("sceNpGetState"))(1, &state) == 0 && state == 2);
    char id[20];
    CHECK(reinterpret_cast<OnlineIdFn>(fn("sceNpGetOnlineId"))(1, id) == 0 && std::strcmp(id, "Test_Hunter-1") == 0);
    std::uint8_t npid[36];
    CHECK(reinterpret_cast<OnlineIdFn>(fn("sceNpGetNpId"))(1, npid) == 0 &&
          std::strcmp(reinterpret_cast<char*>(npid), "Test_Hunter-1") == 0);
    char code[128];
    int issuer = 0;
    CHECK(reinterpret_cast<AuthCodeFn>(fn("sceNpAuthGetAuthorizationCode"))(1, nullptr, code, &issuer) == 0 &&
          std::strcmp(code, "DUMMY") == 0 && issuer == 10);
    std::uint8_t plus = 0;
    CHECK(reinterpret_cast<CheckPlusFn>(fn("sceNpCheckPlus"))(1, nullptr, &plus) == 0 && plus == 1);
    std::int64_t req = 0;
    CHECK(reinterpret_cast<CreateReqFn>(fn("sceNpWebApiCreateRequest"))(1, "userProfile", "/v1/users/me/friendList", 0,
                                                                         nullptr, &req) == 0);
    char body[128] = {};
    const int got = reinterpret_cast<ReadFn>(fn("sceNpWebApiReadData"))(req, body, sizeof(body) - 1);
    CHECK(got > 0);
    json::Value v;
    std::string err;
    CHECK(json::parse(body, v, err) && v.find("totalResults") && v.find("totalResults")->number == 0 &&
          v.find("friendList") && v.find("friendList")->array.empty());
    // The state callback fires once, on the caller's thread.
    static std::atomic<int> fired{0};
    struct Cb {
        static BBNET_ABI void state(int user, int st, void* np, void* ud) {
            if (user == 1 && st == 2 && np && ud == reinterpret_cast<void*>(0x55)) fired.fetch_add(1);
        }
    };
    CHECK(reinterpret_cast<RegStateFn>(fn("sceNpRegisterStateCallback"))(reinterpret_cast<void*>(&Cb::state),
                                                                         reinterpret_cast<void*>(0x55)) == 0);
    auto check_cb = reinterpret_cast<CheckCbFn>(fn("sceNpCheckCallback"));
    CHECK(check_cb() == 0 && check_cb() == 0 && fired.load() == 1);
    std::printf("np: ok\n");
}

static void test_routing() {
    CHECK(bbnet_resolve("sceNetSocket") != 0);
    CHECK(bbnet_resolve("TEST1#A#B") == bbnet_resolve("sceNetSocket"));  // scoped NID via runtime_symbol
    CHECK(bbnet_resolve("TEST2#A#B") == 0);                              // NpScore stays on the stubs
    CHECK(bbnet_resolve("sceNpScoreCreateRequest") == 0);
    // ...except the comment filter: its SIGNED_OUT made the game drop offline in the world.
    CHECK(bbnet_resolve("sceNpScoreSanitizeComment") != 0);
    CHECK(bbnet_resolve("sceNpScoreCensorComment") != 0);
    {
        using Sanitize = int(BBNET_ABI*)(int, const char*, char*, void*);
        using Censor = int(BBNET_ABI*)(int, const char*, void*);
        char out[256];
        std::memset(out, 'x', sizeof out);
        CHECK(reinterpret_cast<Sanitize>(bbnet_resolve("sceNpScoreSanitizeComment"))(1, "visy", out, nullptr) == 0);
        CHECK(std::strcmp(out, "visy") == 0);
        CHECK(reinterpret_cast<Censor>(bbnet_resolve("sceNpScoreCensorComment"))(1, "visy", nullptr) == 0);
    }
    CHECK(bbnet_resolve("sceNpTrophyCreateContext") == 0);
    CHECK(bbnet_resolve("sceNpCommerceDialogOpen") == 0);
    CHECK(bbnet_resolve("sceNpProfileDialogOpen") == 0);
    CHECK(bbnet_resolve("sceNpMatching2Initialize") != 0);  // A3: np_matching2.cpp
    CHECK(bbnet_resolve("sceNpSignalingInitialize") != 0);  // A3: np_signaling.cpp
    CHECK(bbnet_resolve("sceHttpInit") != 0);               // A3: http_hle.cpp
    CHECK(bbnet_resolve("sceSslInit") != 0);
    CHECK(bbnet_resolve("sceNpCmpNpId") == 0);              // the stub's NOT_MATCH contract is kept
    CHECK(bbnet_resolve("sceNpGetState") != 0);
    CHECK(bbnet_resolve("sceNetCtlGetInfo") != 0);
    CHECK(bbnet_resolve("sceKernelUsleep") == 0);
    CHECK(bbnet_resolve("UNKNOWN#A#B") == 0);
    std::printf("routing: ok\n");
}

static std::atomic<std::uint64_t> g_cb_sum{0};
static BBNET_ABI std::uint64_t guest_cb(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                                        std::uint64_t e, std::uint64_t f) {
    g_cb_sum.fetch_add(a + b + c + d + e + f);
    return 0;
}
static void test_dispatcher() {
    const int before = g_fs_restores.load();
    for (int i = 0; i < 10; ++i) bbnet_post_guest_call(reinterpret_cast<uintptr_t>(&guest_cb), 1, 2, 3, 4, 5, 6);
    bbnet::drain_guest_calls();
    CHECK(g_cb_sum.load() == 210);
    CHECK(g_fs_restores.load() - before == 10);
    CHECK(g_attached.load() == 1);
    std::printf("dispatcher: ok\n");
}

static void test_port_remap(std::uint16_t party_port) {
    // With the P2P sockets closed the party port is free again: a plain UDP bind of the game's
    // 3658 lands on it.
    auto sock = reinterpret_cast<SocketFn>(fn("sceNetSocket"));
    auto bind = reinterpret_cast<BindFn>(fn("sceNetBind"));
    auto getsockname = reinterpret_cast<GetsocknameFn>(fn("sceNetGetsockname"));
    auto close = reinterpret_cast<CloseFn>(fn("sceNetSocketClose"));
    if (bbnet::p2p_bound_port()) {
        std::printf("port remap: skipped (party port held by the session layer)\n");
        return;
    }
    const int s = sock("udp3658", 2, 2, 0);
    SceAddr game = sce_addr("0.0.0.0", 3658, 0);
    CHECK(bind(s, &game, 16) == 0);
    SceAddr name{};
    unsigned len = 16;
    CHECK(getsockname(s, &name, &len) == 0 && ntohs(name.port) == party_port);
    CHECK(close(s) == 0);
    std::printf("port remap: ok\n");
}

// The LAN address choice: BB_PARTY_LOCAL_IP / BB_PARTY_LOOPBACK win over every adapter (the
// loopback one included); real adapters with the default route beat Hyper-V / WSL / VPN ones.
static std::uint32_t ipv4(const char* text) {
    std::uint32_t a = 0;
    inet_pton(AF_INET, text, &a);
    return a;
}
// Host over a VPN (0.7.5): which adapter is "the VPN", for auto / off / a name; same-LAN-address homes.
static void test_discovery() {
    using namespace bbnet::disc;
    using bbnet::LanAdapter;
    Announce a;
    a.id = 0xdeadbeef; a.port = 9307; a.level = 120; a.players = 2; a.max_players = 4; a.password = true;
    a.build = "0.7.5-abc"; a.name = "Hunter_One"; a.area = "the Cathedral Ward";
    const std::vector<std::uint8_t> pkt = encode(a);
    CHECK(pkt.size() <= kMaxPacket && pkt[0] == 0xfa && pkt[2] == 1);
    Announce b;
    CHECK(parse(pkt.data(), pkt.size(), &b));
    CHECK(b.id == a.id && b.port == 9307 && b.level == 120 && b.players == 2 && b.max_players == 4 && b.password);
    CHECK(b.build == a.build && b.name == a.name && b.area == a.area);
    // strictness: every truncation, a wrong magic / version, flags, trailing bytes, control characters
    for (std::size_t n = 0; n < pkt.size(); ++n) CHECK(!parse(pkt.data(), n, &b));
    auto mutated = [&](std::size_t i, std::uint8_t v) { auto m = pkt; m[i] = v; return parse(m.data(), m.size(), &b); };
    CHECK(!mutated(0, 0) && !mutated(1, 'B') && !mutated(2, 2) && !mutated(3, 2));
    { auto m = pkt; m[9] = 0; m[8] = 0; CHECK(!parse(m.data(), m.size(), &b)); }
    { auto m = pkt; m[12] = 9; CHECK(!parse(m.data(), m.size(), &b)); }  // players > max
    { auto m = pkt; m.push_back(0); CHECK(!parse(m.data(), m.size(), &b)); }
    { auto m = pkt; m[15 + a.build.size()] = 0x07; CHECK(!parse(m.data(), m.size(), &b)); }  // control char in name
    { auto m = pkt; m[14] = 200; CHECK(!parse(m.data(), m.size(), &b)); }  // length overruns
    std::vector<std::uint8_t> big(kMaxPacket + 1, 'x');
    CHECK(!parse(big.data(), big.size(), &b) && !parse(nullptr, 20, &b));
    // junk never crashes or parses
    unsigned seed = 12345;
    for (int i = 0; i < 20000; ++i) {
        std::vector<std::uint8_t> j(seed % 90);
        for (auto& c : j) { seed = seed * 1664525u + 1013904223u; c = static_cast<std::uint8_t>(seed >> 24); }
        seed = seed * 1664525u + 1013904223u;
        if (j.size() > 3 && (seed & 1)) { j[0] = 0xfa; j[1] = 'A'; j[2] = 1; j[3] &= 1; }
        Announce x;
        if (parse(j.data(), j.size(), &x)) CHECK(x.port && x.players <= x.max_players && !x.name.empty());
    }
    // long / odd strings are cut and cleaned on the way out, never over the wire limit
    Announce l = a;
    l.name = std::string(40, 'n') + "Ã©"; l.area = std::string(100, 'a'); l.build = std::string(100, 'b');
    const auto lp = encode(l);
    CHECK(lp.size() <= kMaxPacket && parse(lp.data(), lp.size(), &b) && b.name.size() == 16 && b.area.size() == 32 &&
          b.build.size() == kMaxBuild);
    // our real build id (nightwatch-coop-<version>-<16 hex>) goes out whole
    Announce nw = a;
    nw.build = "nightwatch-coop-0.2.4-beta.1-0123456789abcdef";
    const auto np = encode(nw);
    CHECK(parse(np.data(), np.size(), &b) && b.build == nw.build);
    // the adapter filter: LAN and VPN only, never the Internet
    auto ad = [](const char* name, const char* ip, const char* mask) {
        LanAdapter x; x.name = name; x.ip = ipv4(ip); x.mask = ipv4(mask); x.physical = true; return x;
    };
    const LanAdapter lan = ad("Ethernet / Realtek PCIe", "192.168.1.20", "255.255.255.0");
    const LanAdapter lan10 = ad("Wi-Fi / Intel", "10.1.2.3", "255.255.0.0");
    const LanAdapter lan172 = ad("Ethernet 2 / Intel", "172.20.0.5", "255.255.0.0");
    const LanAdapter pub = ad("Ethernet 3 / Intel", "203.0.113.9", "255.255.255.0");
    const LanAdapter pub172 = ad("Ethernet 4 / Intel", "172.32.0.5", "255.255.0.0");  // outside 172.16/12
    const LanAdapter radmin = ad("Radmin VPN / Famatech Radmin VPN Ethernet Adapter", "26.41.7.9", "255.0.0.0");
    const LanAdapter hamachi = ad("Hamachi / LogMeIn Hamachi Virtual Ethernet Adapter", "25.4.5.6", "255.0.0.0");
    const LanAdapter zt = ad("ZeroTier One / ZeroTier Virtual Port", "10.147.17.5", "255.255.0.0");
    const LanAdapter tail = ad("Tailscale / Tailscale Tunnel", "100.101.102.103", "255.192.0.0");
    const LanAdapter nord = ad("NordLynx / NordLynx Tunnel", "10.5.0.2", "255.255.255.0");
    const LanAdapter hyperv = ad("vEthernet (Default Switch) / Hyper-V Virtual Ethernet Adapter", "172.28.80.1", "255.255.240.0");
    const LanAdapter docker = ad("vEthernet (WSL) / Hyper-V Virtual Ethernet Adapter", "192.168.96.1", "255.255.240.0");
    LanAdapter loop = ad("Loopback Pseudo-Interface 1", "127.0.0.1", "255.0.0.0"); loop.loopback = true;
    const LanAdapter apipa = ad("Ethernet 5 / Intel", "169.254.3.4", "255.255.0.0");
    LanAdapter tun = lan; tun.tunnel = true;
    CHECK(adapter_discovery_ok(lan) && adapter_discovery_ok(lan10) && adapter_discovery_ok(lan172));
    CHECK(adapter_discovery_ok(radmin) && adapter_discovery_ok(hamachi) && adapter_discovery_ok(zt));
    CHECK(!adapter_discovery_ok(pub) && !adapter_discovery_ok(pub172));
    CHECK(!adapter_discovery_ok(tail) && !adapter_discovery_ok(nord));
    CHECK(!adapter_discovery_ok(hyperv) && !adapter_discovery_ok(docker));
    CHECK(!adapter_discovery_ok(loop) && !adapter_discovery_ok(apipa) && !adapter_discovery_ok(tun));
    // a public address on a Radmin-named adapter is still not used
    CHECK(!adapter_discovery_ok(ad("Radmin VPN", "8.8.4.4", "255.255.255.0")));
    const auto t = targets_for({lan, pub, radmin, tail, nord, hyperv, lan, loop});
    CHECK(t.size() == 2);
    CHECK(t[0].source == ipv4("192.168.1.20") && t[0].broadcast == ipv4("192.168.1.255"));
    CHECK(t[1].source == ipv4("26.41.7.9") && t[1].broadcast == ipv4("26.255.255.255"));
    LanAdapter nomask = lan; nomask.mask = 0;
    CHECK(targets_for({nomask}).empty());
    std::printf("party discovery: ok\n");
}

static void test_vpn_selection() {
    using bbnet::LanAdapter;
    using bbnet::VpnKind;
    auto adapter = [](const char* name, const char* ip, bool gw, bool def, bool phys) {
        LanAdapter a;
        a.name = name;
        a.ip = ipv4(ip);
        a.gateway = gw;
        a.default_route = def;
        a.physical = phys;
        return a;
    };
    const LanAdapter lan = adapter("Ethernet / Realtek PCIe 2.5GbE Family Controller", "192.168.1.20", true, true, true);
    const LanAdapter radmin = adapter("Radmin VPN / Famatech Radmin VPN Ethernet Adapter", "26.41.7.9", false, false, true);
    const LanAdapter hamachi = adapter("Hamachi / LogMeIn Hamachi Virtual Ethernet Adapter", "25.4.5.6", false, false, true);
    const LanAdapter tail = adapter("Tailscale / Tailscale Tunnel", "100.101.102.103", false, false, false);
    const LanAdapter zt = adapter("ZeroTier One [8056c2e21c000001] / ZeroTier Virtual Port", "10.147.17.5", false, false, true);
    const LanAdapter nord = adapter("NordLynx / NordLynx Tunnel", "10.5.0.2", true, true, false);
    const LanAdapter lan2 = adapter("Ethernet 2 / Intel(R) Ethernet I219-V", "192.168.1.20", true, false, true);  // same address as lan
    CHECK(bbnet::vpn_kind(radmin) == VpnKind::Radmin && bbnet::vpn_kind(hamachi) == VpnKind::Hamachi);
    CHECK(bbnet::vpn_kind(tail) == VpnKind::Tailscale && bbnet::vpn_kind(zt) == VpnKind::ZeroTier);
    CHECK(bbnet::vpn_kind(nord) == VpnKind::Other && bbnet::vpn_kind(lan) == VpnKind::None);
    CHECK(bbnet::vpn_label(radmin) == "Radmin VPN" && bbnet::vpn_label(nord) == "NordLynx");
    // an unnamed virtual adapter is recognised by its range; a real adapter is not
    CHECK(bbnet::vpn_kind(adapter("Ethernet 5 / Virtual Adapter", "26.1.2.3", false, false, true)) == VpnKind::Radmin);
    CHECK(bbnet::vpn_kind(adapter("Ethernet 5 / Virtual Adapter", "100.64.0.9", false, false, true)) == VpnKind::Tailscale);
    CHECK(bbnet::vpn_kind(adapter("Ethernet 5 / Virtual Adapter", "100.128.0.9", false, false, true)) == VpnKind::None);
    CHECK(bbnet::vpn_kind(adapter("Ethernet / Realtek", "26.1.2.3", true, true, true)) == VpnKind::None);
    CHECK(bbnet::vpn_pref_normalize("") == "auto" && bbnet::vpn_pref_normalize(" AUTO ") == "auto");
    CHECK(bbnet::vpn_pref_normalize("Off") == "off" && bbnet::vpn_pref_normalize(" Radmin VPN ") == "Radmin VPN");

    const std::vector<LanAdapter> list{lan, radmin, nord};
    CHECK(bbnet::select_vpn_adapter(list, "auto") == 1);   // Radmin; the commercial VPN is never taken unasked
    CHECK(bbnet::select_vpn_adapter(list, "") == 1);
    CHECK(bbnet::select_vpn_adapter(list, "off") == -1);
    CHECK(bbnet::select_vpn_adapter({lan, nord}, "auto") == -1);
    CHECK(bbnet::select_vpn_adapter({lan}, "auto") == -1);
    CHECK(bbnet::select_vpn_adapter(list, "radmin") == 1);  // a name, any case, a part of it
    CHECK(bbnet::select_vpn_adapter(list, "NORDLYNX") == 2);
    CHECK(bbnet::select_vpn_adapter(list, "zerotier") == -1);  // not up: the usual address
    CHECK(bbnet::select_vpn_adapter({lan, hamachi, tail, radmin}, "auto") == 3);  // Radmin, Hamachi, Tailscale, ZeroTier
    CHECK(bbnet::select_vpn_adapter({lan, zt, tail}, "auto") == 2);
    // an adapter without an address (169.254 / 0) is never the VPN
    CHECK(bbnet::select_vpn_adapter({adapter("Radmin VPN", "169.254.1.1", false, false, true)}, "auto") == -1);
    // two homes with the same router-assigned address (0.7.5 samelanip): the VPN address tells them apart
    const int pick = bbnet::select_vpn_adapter({lan2, radmin}, "auto");
    CHECK(pick == 1 && ipv4("192.168.1.20") == lan2.ip && radmin.ip != lan2.ip);
    // The address choice itself: auto takes the VPN over the LAN with the default route, off does not.
    // (Not on the real adapters of this PC: the scores only.)
    CHECK(bbnet::vpn_adapter_score(radmin, "auto") > bbnet::lan_adapter_score(lan));
    CHECK(bbnet::vpn_adapter_score(radmin, "off") < 0);
    std::printf("vpn selection: ok\n");
}

static void test_vpn_online() {
    using bbnet::IfCounters;
    using bbnet::VpnState;
    const std::string off_log =
        "2026.10.23 19:58:20.265\tINFO\tService starting...\n"
        "2026.10.23 19:58:26.453\tINFO\tConnected to server (17)\n"
        "2026.10.23 19:59:19.382\tINFO\tConnected to 154680149/'friend' via TcpRelay/outgoing\n"
        "2026.10.23 21:37:57.218\tINFO\tSwitched Off\n"
        "2026.10.23 21:37:57.219\tINFO\tDisconnected from server(code:4)\n"
        "2026.10.25 13:46:46.742\tINFO\tVirtual network adapter opened\n";
    CHECK(bbnet::vpn_state_from_radmin_log(off_log) == VpnState::Offline);
    // a friend line alone ("Connected to 123/'x'") is not the server
    CHECK(bbnet::vpn_state_from_radmin_log("x\tINFO\tConnected to 154680149/'friend' via TcpRelay/outgoing\n") == VpnState::Unknown);
    CHECK(bbnet::vpn_state_from_radmin_log(off_log + "t\tINFO\tConnected to server (3)\n") == VpnState::Online);
    CHECK(bbnet::vpn_state_from_radmin_log(off_log + "t\tINFO\tService starting...\n") == VpnState::Offline);
    CHECK(bbnet::vpn_state_from_radmin_log("") == VpnState::Unknown);
    CHECK(bbnet::vpn_state_from_tailscale_json("{\"Version\":\"1\",\"BackendState\": \"Running\",\"Self\":{}}") == VpnState::Online);
    CHECK(bbnet::vpn_state_from_tailscale_json("{\"BackendState\":\"Stopped\"}") == VpnState::Offline);
    CHECK(bbnet::vpn_state_from_tailscale_json("{\"BackendState\":\"NeedsLogin\"}") == VpnState::Offline);
    CHECK(bbnet::vpn_state_from_tailscale_json("{\"BackendState\":\"Starting\"}") == VpnState::Unknown);
    CHECK(bbnet::vpn_state_from_tailscale_json("failed to connect to local tailscaled") == VpnState::Unknown);
    CHECK(bbnet::vpn_state_from_zerotier_info("200 info 8056c2e21c 1.12.2 ONLINE\n") == VpnState::Online);
    CHECK(bbnet::vpn_state_from_zerotier_info("200 info 8056c2e21c 1.12.2 OFFLINE\n") == VpnState::Offline);
    CHECK(bbnet::vpn_state_from_zerotier_info("zerotier-cli: missing authentication token") == VpnState::Unknown);
    IfCounters a, b;
    a.valid = b.valid = true;
    a.in_octets = b.in_octets = 1380;
    a.in_ucast = b.in_ucast = 23;
    std::string why;
    // measured on an offline Radmin: nothing in, the rest of the stack only sends
    CHECK(!bbnet::vpn_counters_rising(a, b));
    CHECK(!bbnet::vpn_decide_online(VpnState::Unknown, a, b, true, &why) && !why.empty());
    b.in_nucast = 2;
    CHECK(bbnet::vpn_counters_rising(a, b) && bbnet::vpn_decide_online(VpnState::Unknown, a, b, true));
    b.in_nucast = 0;
    b.in_octets = 1500;
    CHECK(bbnet::vpn_decide_online(VpnState::Unknown, a, b, true));
    b.in_octets = 1380;
    // the vendor wins over the counters either way
    CHECK(bbnet::vpn_decide_online(VpnState::Online, a, b, true));
    b.in_octets = 9999;
    CHECK(!bbnet::vpn_decide_online(VpnState::Offline, a, b, true));
    // no evidence: trust the adapter; a named adapter is trusted on quiet counters
    CHECK(bbnet::vpn_decide_online(VpnState::Unknown, IfCounters{}, IfCounters{}, true));
    b.in_octets = 1380;
    CHECK(bbnet::vpn_decide_online(VpnState::Unknown, a, b, false));
    // an offline VPN adapter is never chosen: the real adapter wins, and alone it gives none
    bbnet::LanAdapter lan, radmin;
    lan.name = "Ethernet / Realtek";
    lan.ip = ipv4("192.168.1.20");
    lan.gateway = lan.default_route = lan.physical = true;
    radmin.name = "Radmin VPN / Famatech Radmin VPN Ethernet Adapter";
    radmin.ip = ipv4("26.41.7.9");
    CHECK(bbnet::select_vpn_adapter({lan, radmin}, "auto") == 1);
    radmin.vpn_offline = true;
    CHECK(bbnet::select_vpn_adapter({lan, radmin}, "auto") == -1);
    CHECK(bbnet::select_vpn_adapter({lan, radmin}, "radmin") == -1);
    CHECK(bbnet::lan_adapter_score(radmin) < 0 && bbnet::lan_adapter_score(lan) >= 0);
    std::printf("vpn online: ok\n");
}

static void test_local_address() {
    using bbnet::LanAdapter;
    auto adapter = [](const char* name, const char* ip, bool gw, bool def, bool phys) {
        LanAdapter a;
        a.name = name;
        a.ip = ipv4(ip);
        a.gateway = gw;
        a.default_route = def;
        a.physical = phys;
        return a;
    };
    const LanAdapter lan = adapter("Ethernet / Realtek PCIe 2.5GbE Family Controller", "192.168.1.20", true, true, true);
    const LanAdapter wifi = adapter("Wi-Fi / Intel(R) Wi-Fi 6E AX211", "192.168.1.21", true, false, true);
    const LanAdapter second = adapter("Ethernet 2 / Intel(R) Ethernet I219-V", "10.0.0.5", false, false, true);
    const LanAdapter hyperv =
        adapter("vEthernet (Default Switch) / Hyper-V Virtual Ethernet Adapter", "172.23.16.1", false, false, true);
    const LanAdapter wsl = adapter("vEthernet (WSL (Hyper-V firewall)) / Hyper-V Virtual Ethernet Adapter #2",
                                   "172.23.32.1", true, false, true);
    const LanAdapter vbox =
        adapter("Ethernet 3 / VirtualBox Host-Only Ethernet Adapter", "192.168.56.1", false, false, true);
    const LanAdapter vpn = adapter("OpenVPN TAP / TAP-Windows Adapter V9", "10.8.0.6", true, true, true);
    CHECK(!bbnet::adapter_is_virtual(lan.name) && !bbnet::adapter_is_virtual(wifi.name));
    CHECK(bbnet::adapter_is_virtual(hyperv.name) && bbnet::adapter_is_virtual(wsl.name));
    CHECK(bbnet::adapter_is_virtual(vbox.name) && bbnet::adapter_is_virtual(vpn.name));
    CHECK(bbnet::adapter_is_virtual("Tailscale / Tailscale Tunnel"));
    CHECK(bbnet::lan_adapter_score(lan) > bbnet::lan_adapter_score(wifi));
    CHECK(bbnet::lan_adapter_score(wifi) > bbnet::lan_adapter_score(second));
    for (const LanAdapter* v : {&hyperv, &wsl, &vbox, &vpn}) {
        CHECK(bbnet::lan_adapter_score(*v) >= 0);
        CHECK(bbnet::lan_adapter_score(second) > bbnet::lan_adapter_score(*v));  // even a VPN with the default route
    }
    LanAdapter lo = adapter("Loopback Pseudo-Interface 1", "127.0.0.1", false, false, false);
    lo.loopback = true;
    CHECK(bbnet::lan_adapter_score(lo) < 0);
    CHECK(bbnet::lan_adapter_score(adapter("Ethernet", "169.254.3.4", false, false, true)) < 0);
    LanAdapter teredo = adapter("Teredo", "10.1.1.1", false, false, false);
    teredo.tunnel = true;
    CHECK(bbnet::lan_adapter_score(teredo) < 0);

    // The environment overrides, read at every query (party_runtime sets them late).
    std::string how;
    set_env("BB_PARTY_LOOPBACK", "");
    set_env("BB_MP_LOCAL_TEST", "");
    set_env("BB_PARTY_LOCAL_IP", "127.0.0.1");
    CHECK(bbnet::forced_local_ipv4() == ipv4("127.0.0.1"));
    CHECK(bbnet::query_local_ipv4(&how) == ipv4("127.0.0.1"));
    CHECK(how.find("BB_PARTY_LOCAL_IP") != std::string::npos);
    std::printf("local address (forced): %s\n", how.c_str());
    set_env("BB_PARTY_LOCAL_IP", "10.200.201.202");
    CHECK(bbnet::query_local_ipv4(&how) == ipv4("10.200.201.202"));
    CHECK(how.find("no adapter") != std::string::npos);
    set_env("BB_PARTY_LOCAL_IP", "");
    set_env("BB_PARTY_LOOPBACK", "1");
    std::string source;
    CHECK(bbnet::forced_local_ipv4(&source) == ipv4("127.0.0.1") && source == "BB_PARTY_LOOPBACK=1");
    CHECK(bbnet::query_local_ipv4(&how) == ipv4("127.0.0.1"));
    set_env("BB_PARTY_LOOPBACK", "0");
    set_env("BB_PARTY_LOCAL_IP", "not-an-ip");
    CHECK(bbnet::forced_local_ipv4() == 0);
    set_env("BB_PARTY_LOCAL_IP", "");
    CHECK(bbnet::forced_local_ipv4() == 0);
    const std::uint32_t chosen = bbnet::query_local_ipv4(&how);
    CHECK((ntohl(chosen) >> 24) != 127);  // never the loopback adapter unless forced
    std::printf("local address (auto): %s\n", how.c_str());
    std::printf("local address: ok\n");
}

// F5: the session transport's records (shapes from a live capture, 2026-10-14) and the outage
// keepalive / resume replay built from them.
static void test_nr_outage() {
    using namespace bbnet::nr;
    using std::chrono::milliseconds;
    // A data record (seq 0xb5d1, type 3) and a bare ack (ack 0xb5ce, type 0x30) in one datagram,
    // then an unreliable record.
    const std::vector<std::uint8_t> d = {0x01, 0x00, 0x08, 0xb5, 0xd1, 0x00, 0x00, 0x03, 0xff, 0xaa, 0xbb,
                                         0x01, 0x00, 0x06, 0x00, 0x00, 0xb5, 0xce, 0x30, 0xff,
                                         0x00, 0x00, 0x02, 0x11, 0x22};
    int n = 0, data = 0;
    CHECK(for_each_record(d.data(), d.size(), [&](const Record& r) { ++n; data += is_data(r); }));
    CHECK(n == 3 && data == 1);
    const std::uint8_t bad[] = {0x01, 0x00, 0x40, 0x00};
    CHECK(!for_each_record(bad, sizeof bad, [](const Record&) {}));
    // Keepalive: the data record alone; a piggy-backed ack (0x33) loses its ack flag and number.
    std::vector<std::uint8_t> ka = keepalive_from(d.data(), d.size());
    CHECK(ka == (std::vector<std::uint8_t>{0x01, 0x00, 0x08, 0xb5, 0xd1, 0x00, 0x00, 0x03, 0xff, 0xaa, 0xbb}));
    const std::uint8_t pig[] = {0x01, 0x00, 0x06, 0x12, 0x34, 0xb5, 0xce, 0x33, 0xf0};
    ka = keepalive_from(pig, sizeof pig);
    CHECK(ka.size() == 9 && ka[5] == 0 && ka[6] == 0 && ka[7] == 0x03 && ka[8] == 0xf0);
    const std::uint8_t only_ack[] = {0x01, 0x00, 0x06, 0x00, 0x00, 0xb5, 0xce, 0x30, 0xff};
    CHECK(keepalive_from(only_ack, sizeof only_ack).empty());

    // Dup-ack reports held back after a resume: the type-2 record goes, the others stay.
    const std::uint8_t mix[] = {0x01, 0x00, 0x0a, 0x00, 0x00, 0x7f, 0xf2, 0x02, 0xfe, 0x00, 0x00, 0x00, 0x27,
                                0x01, 0x00, 0x06, 0x00, 0x00, 0x7f, 0xf2, 0x30, 0xfd, 0x00, 0x00, 0x01, 0x05};
    std::vector<std::uint8_t> kept;
    CHECK(strip_dup_acks(mix, sizeof mix, &kept) == 1);
    CHECK(kept == (std::vector<std::uint8_t>{0x01, 0x00, 0x06, 0x00, 0x00, 0x7f, 0xf2, 0x30, 0xfd, 0x00, 0x00, 0x01, 0x05}));
    CHECK(strip_dup_acks(mix, 13, &kept) == 1 && kept.empty());
    CHECK(strip_dup_acks(bad, sizeof bad, &kept) == 0 && kept.size() == sizeof bad);

    // An oversized datagram leaves in MTU-sized parts at record boundaries.
    std::vector<std::uint8_t> big;
    for (int i = 0; i < 50; ++i) {
        const std::uint8_t r[] = {0x01, 0x00, 0x27};
        big.insert(big.end(), r, r + 3);
        big.insert(big.end(), 0x27, static_cast<std::uint8_t>(i));
    }
    std::vector<std::vector<std::uint8_t>> parts;
    CHECK(big.size() == 2100 && split_records(big.data(), big.size(), kGameMtu, &parts) && parts.size() == 2);
    std::size_t sum = 0;
    for (const auto& pt : parts) {
        CHECK(pt.size() <= kGameMtu && pt[0] == 0x01 && pt[2] == 0x27);
        sum += pt.size();
    }
    CHECK(sum == big.size());
    CHECK(!split_records(bad, sizeof bad, kGameMtu, &parts));

    // Resume replay: the peer heard until t=10 s, silent to t=30 s.
    PeerReplay rp;
    const Clock::time_point t0 = Clock::now();
    auto at = [&](int ms) { return t0 + milliseconds(ms); };
    auto rec = [](std::uint16_t seq) {
        return std::vector<std::uint8_t>{0x01, 0x00, 0x06, static_cast<std::uint8_t>(seq >> 8),
                                         static_cast<std::uint8_t>(seq), 0x00, 0x00, 0x03, 0xff};
    };
    std::uint16_t seq = 100;
    for (int ms = 0; ms < 30000; ms += 100) {
        const Clock::time_point heard = at(std::min(ms, 10000));
        auto r = rec(seq++);
        rp.on_send(at(ms), heard, r.data(), r.size());
        if (ms == 20000) {  // a retransmission of an outage segment: sent once in the replay
            auto again = rec(static_cast<std::uint16_t>(seq - 50));
            rp.on_send(at(ms), heard, again.data(), again.size());
        }
    }
    // Kept: from 3 s before the silence (7 s) to its end (30 s), 10/s, plus the retransmission.
    CHECK(rp.kept() >= 228 && rp.kept() <= 233);
    std::size_t records = 0;
    CHECK(rp.on_heard(at(30000), at(29500), &records).empty());  // no silence: nothing
    auto out = rp.on_heard(at(30050), at(10000), &records);
    CHECK(records >= 227 && records <= 232 && !out.empty() && rp.kept() == 0);
    std::uint16_t prev = 0;
    bool ordered = true;
    std::size_t total = 0;
    for (const auto& g : out) {
        CHECK(g.size() <= 1100);
        for_each_record(g.data(), g.size(), [&](const Record& r) {
            if (prev && seq_of(r) != static_cast<std::uint16_t>(prev + 1)) ordered = false;
            prev = seq_of(r);
            ++total;
        });
    }
    CHECK(ordered && total == records && prev == static_cast<std::uint16_t>(seq - 1));
}

// 0.7.7 (nr_loadgap.h): receive gaps inside loading screens are no outage.
static void test_nr_loadgap() {
    using namespace bbnet::nr;
    using std::chrono::milliseconds;
    const LoadGapGate::Clock::time_point t0 = LoadGapGate::Clock::now();
    auto at = [&](int ms) { return t0 + milliseconds(ms); };
    const std::uint64_t peer = 7, other = 9;
    {  // CL-9142b8: a member that left is expected silence, however long; back in the roster it counts again
        LoadGapGate g;
        g.set_gone(peer, true);
        CHECK(!g.gap_counts(peer, at(0), at(60000), 4.0));
        CHECK(g.gap_counts(other, at(0), at(60000), 4.0));  // a staying member's real outage still counts
        CHECK(g.gone(peer) && g.load_touched(peer, at(0)));
        g.set_gone(peer, false);
        CHECK(g.gap_counts(peer, at(0), at(5000), 4.0));
    }
    {  // no load: the whole gap counts, 4 s is the threshold
        LoadGapGate g;
        CHECK(g.gap_counts(peer, at(0), at(4000), 4.0));
        CHECK(!g.gap_counts(peer, at(0), at(3900), 4.0));
    }
    {  // the peer's 15 s load covers the gap: nothing counts, also right at its end
        LoadGapGate g;
        g.set_loading(peer, true, at(1000));
        CHECK(!g.gap_counts(peer, at(0), at(10000), 4.0));  // still loading
        g.set_loading(peer, false, at(16000));
        CHECK(!g.gap_counts(peer, at(0), at(16200), 4.0));  // heard 0.2 s after the load ended
        CHECK(g.load_touched(peer, at(0)));
    }
    {  // CL-d329c8: a hitch right after the load (heard 4.6 s after its end) is inside the grace: no outage
        LoadGapGate g;
        g.set_loading(0, true, at(1000));
        g.set_loading(0, false, at(16000));  // our world-up
        CHECK(!g.gap_counts(peer, at(0), at(20600), 4.0));
        CHECK(!g.gap_counts(peer, at(0), at(23900), 4.0));
        // still silent after the grace: counted from the grace's end
        CHECK(g.gap_counts(peer, at(0), at(28000), 4.0));
        CHECK(!g.gap_counts(peer, at(0), at(27900), 4.0));
        g.set_loading(peer, true, at(30000));
        g.set_loading(peer, false, at(40000));  // the peer's load end gets the same grace
        CHECK(!g.gap_counts(peer, at(29000), at(45000), 4.0));
        CHECK(g.gap_counts(peer, at(29000), at(52000), 4.0));
        CHECK(g.effective_silent_s(peer, at(29000), at(53000)) > 4.9 && g.effective_silent_s(peer, at(29000), at(53000)) < 5.1);
    }
    {  // a steady member's real outage, no load anywhere: counted at once
        LoadGapGate g;
        CHECK(g.gap_counts(peer, at(0), at(4100), 4.0));
    }
    {  // our own load excuses every link; another peer's load excuses only its own link
        LoadGapGate g;
        g.set_loading(0, true, at(500));
        g.set_loading(0, false, at(9000));
        CHECK(!g.gap_counts(peer, at(0), at(9500), 4.0) && !g.gap_counts(other, at(0), at(9500), 4.0));
        g.set_loading(other, true, at(10000));
        g.set_loading(other, false, at(20000));
        CHECK(g.gap_counts(peer, at(9500), at(30000), 4.0));    // a real outage on the other link (past our grace)
        CHECK(!g.gap_counts(other, at(9500), at(20000), 4.0));
    }
    {  // the session host's load excuses every link (guest-guest too)
        LoadGapGate g;
        g.set_loading(LoadGapGate::kHost, true, at(500));
        CHECK(!g.gap_counts(peer, at(0), at(9000), 4.0) && !g.gap_counts(other, at(0), at(9000), 4.0));
        g.set_loading(LoadGapGate::kHost, false, at(9000));
        CHECK(!g.gap_counts(peer, at(0), at(14000), 4.0));  // inside the grace
        CHECK(g.gap_counts(peer, at(0), at(21000), 4.0));  // 12 s past the end: real
    }
    {  // a load that ended before the gap began does not excuse it
        LoadGapGate g;
        g.set_loading(peer, true, at(0));
        g.set_loading(peer, false, at(3000));
        CHECK(g.gap_counts(peer, at(4000), at(9000), 4.0));
        CHECK(!g.load_touched(peer, at(4000)));
    }
}

// 0.7.6 diagnostics (nr_rxgap.h): a quiet peer with a reader that ran on time vs. a stalled reader.
static void test_nr_rxgap() {
    using namespace bbnet::nr;
    using std::chrono::milliseconds;
    const RxGapMonitor::Clock::time_point t0 = RxGapMonitor::Clock::now();
    auto at = [&](int ms) { return t0 + milliseconds(ms); };
    // Two peers; the reader polls every 200 ms. Peer A goes quiet for 4 s while B keeps talking.
    {
        RxGapMonitor m;
        RxGapMonitor::Gap g;
        int t = 0;
        CHECK(!m.heard(1, at(t), &g) && !m.heard(2, at(t), &g));
        for (; t < 4000; t += 200) {
            m.poll_begin(at(t));
            m.poll_end(at(t + 200), true, 200);  // on time
            if (t % 600 == 0) CHECK(!m.heard(2, at(t + 200), &g));
        }
        CHECK(m.heard(1, at(4200), &g));
        CHECK(g.key == 1 && g.silent_s > 4.1 && g.silent_s < 4.3);
        CHECK(g.worst_stall_ms == 0);  // the reader was fine: the peer was quiet
        CHECK(g.others >= 6);          // the other source arrived meanwhile
        CHECK(g.silent_peers == 0 && g.peers == 2);
    }
    // Both peers go quiet while the reader's wait overruns by 3.8 s: a stalled reader.
    {
        RxGapMonitor m;
        RxGapMonitor::Gap g;
        CHECK(!m.heard(2, at(0), &g) && !m.heard(1, at(0), &g));
        m.poll_begin(at(100));
        m.poll_end(at(4100), true, 200);  // a 200 ms wait that took 4 s
        CHECK(m.heard(1, at(4110), &g));
        CHECK(g.worst_stall_ms > 3700 && g.worst_stall_ms < 3900 && std::string(g.stall_kind) == "poll");
        CHECK(g.others == 0 && g.silent_peers == 1);  // the other one is silent too
    }
    // Long work between two waits is a stall of its own kind.
    {
        RxGapMonitor m;
        RxGapMonitor::Gap g;
        CHECK(!m.heard(7, at(0), &g));
        m.poll_begin(at(0));
        m.poll_end(at(10), false, 200);
        m.poll_begin(at(2600));  // 2.59 s of work after the wait returned
        m.poll_end(at(2610), false, 200);
        CHECK(m.heard(7, at(2620), &g) && g.worst_stall_ms > 2500 && std::string(g.stall_kind) == "work");
    }
    // At most 20 gap lines a minute; the rest are counted. The summary reports the minute.
    {
        RxGapMonitor m;
        RxGapMonitor::Gap g;
        int lines = 0, t = 0;
        CHECK(!m.heard(3, at(t), &g));
        for (int i = 0; i < 25; ++i) {
            t += 2100;
            if (m.heard(3, at(t), &g)) ++lines;
        }
        CHECK(lines == 20);
        RxGapMonitor::Summary s;
        CHECK(!m.summary(at(t), &s));
        CHECK(m.summary(at(t + 60000), &s) || m.summary(at(t + 120000), &s));
        CHECK(s.gaps == 25 && s.suppressed == 5 && s.datagrams == 26);
        CHECK(s.by_peer.count(3) && s.by_peer[3].longest_s > 2.0);
    }
    std::printf("nr_rxgap: ok\n");
}

int main() {
    // Loopback-only sockets: no Windows Firewall prompt for a test program (party_sock.h).
    if (!std::getenv("BB_PARTY_LOOPBACK")) {
#if defined(_WIN32)
        _putenv("BB_PARTY_LOOPBACK=1");
#else
        setenv("BB_PARTY_LOOPBACK", "1", 1);
#endif
    }
    const std::uint16_t party_port = static_cast<std::uint16_t>(39000 + (std::rand() % 500));
    char port_text[16];
    std::snprintf(port_text, sizeof(port_text), "%u", party_port);
    set_env("BB_PARTY", "host");
    set_env("BB_PARTY_PORT", port_text);
    set_env("BB_PARTY_NAME", "Test_Hunter-1");
    setvbuf(stdout, nullptr, _IONBF, 0);

    test_local_address();

    test_vpn_selection();
    test_vpn_online();
    test_discovery();
    set_env("BB_PARTY_LOOPBACK", "1");  // back to loopback-only sockets for the tests below
    test_stun();
    test_netsim();
    test_nr_outage();
    test_nr_loadgap();
    test_nr_rxgap();
    test_peer_paths();
    test_peer_paths_gaps();
    test_netsim_block();
    test_aliases();
    test_same_lan_address();
    test_routing();
    test_np();
    test_dispatcher();
    test_port_remap(party_port);
    test_sockets(party_port);
    // F2 (0.7.1, IPv6 off by default): the party port opens without an IPv6 socket unless asked.
    CHECK(bbnet::p2p_open());
    CHECK(!bbnet::p2p_ipv6_open());
    bbnet::p2p_set_ipv6(true);  // turned on: [::1] under BB_PARTY_LOOPBACK
    CHECK(bbnet::p2p_ipv6_open());
    bbnet::p2p_set_ipv6(false);
    // json round trip (the host service's bodies).
    json::Value o = json::Value::make_object();
    o.set("totalResults", 0);
    o.set("name", "a\"b");
    json::Value back;
    std::string err;
    CHECK(json::parse(json::dump(o, 0), back, err) && back.find("name")->string == "a\"b");

    if (g_failures) {
        std::fprintf(stderr, "party-net-test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("party-net-test: all passed\n");
    return 0;
}
