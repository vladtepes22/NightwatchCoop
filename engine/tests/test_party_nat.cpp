// SPDX-License-Identifier: GPL-3.0-or-later
// NAT traversal (F2): the NAT model (party_nat: classification from two STUN answers, the path
// prediction, the advice text) checked against simulated NATs of every type with the real
// reliable-UDP streams; the network simulator's NAT kinds; the DHT rendezvous (party_rdv:
// bencode, KRPC, an iterative lookup through a simulated DHT, the socket driver against fake DHT
// nodes on loopback) and the whole automatic path: a guest behind the simulated NAT, no guest
// code, connects because the host finds its endpoint in the (fake) DHT and punches it.
// F2 0.7.1: the party port's route table (net/party_route.h: member relay, IPv6) over simulated NATs
// for every host x member x guest triple, its security rules, IPv6-only pairs through stateful
// firewalls, the path chooser (party_netpath.h) and PeerPaths' IPv6 step.
// Build and run: ninja -C out/gpu party-nat-test && out/gpu/party-nat-test.exe
#include "net/netsim.h"
#include "net/party_route.h"
#include "net/party_udp.h"
#include "party/party_code.h"
#include "party/party_crypto.h"
#include "party/party_link.h"
#include "party/party_nat.h"
#include "party/party_netpath.h"
#include "party/party_rdv.h"
#include "party/party_rudp.h"
#include "party/party_sock.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace party;
using rudp::Endpoint;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static bool wait_for(const std::function<bool()>& pred, int timeout_ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

static std::uint32_t ip(int a, int b, int c, int d) {
    const std::uint8_t v[4] = {static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(c),
                               static_cast<std::uint8_t>(d)};
    std::uint32_t r;
    std::memcpy(&r, v, 4);
    return r;
}
static std::uint16_t nbo(std::uint16_t p) { return htons(p); }
static const std::uint8_t* udp_probe() { return bbnet::udp::kProbe; }

// ---- the router's outside address ----

// The router's outside address against what the internet sees (beta 8 report: UPnP "mapped" behind
// CGNAT was taken for an open port). Documentation addresses (RFC 5737) and a CGNAT example.
static void test_outside() {
    using namespace nat;
    CHECK(classify_outside("100.64.10.1", "198.51.100.20") == Outside::ProviderNat);
    CHECK(classify_outside("100.127.255.1", "") == Outside::ProviderNat);
    CHECK(classify_outside("100.128.0.1", "100.128.0.1") == Outside::Public);  // just past 100.64.0.0/10
    CHECK(classify_outside("192.168.0.10", "203.0.113.4") == Outside::PrivateNat);
    CHECK(classify_outside("10.1.2.3", "") == Outside::PrivateNat);
    CHECK(classify_outside("172.20.0.1", "") == Outside::PrivateNat);
    CHECK(classify_outside("172.32.0.1", "172.32.0.1") == Outside::Public);
    CHECK(classify_outside("203.0.113.4", "198.51.100.7") == Outside::Mismatch);
    CHECK(classify_outside("203.0.113.4", "203.0.113.4") == Outside::Public);
    CHECK(classify_outside("203.0.113.4", "") == Outside::Public);  // no STUN answer: UPnP is believed
    CHECK(classify_outside("", "203.0.113.4") == Outside::Unknown);
    CHECK(classify_outside("0.0.0.0", "") == Outside::Unknown);
    CHECK(classify_outside("not an ip", "") == Outside::Unknown);
    CHECK(classify_outside("1.2.3.4.5", "") == Outside::Unknown);
    const std::string cg = outside_advice(Outside::ProviderNat, "100.64.10.1", "198.51.100.20", 9307);
    CHECK(cg.find("CGNAT") != std::string::npos && cg.find("198.51.100.20") != std::string::npos &&
          cg.find("VPN") != std::string::npos && cg.find(":9307") != std::string::npos);
    CHECK(outside_advice(Outside::Public, "203.0.113.4", "203.0.113.4", 9307).empty());
    std::printf("  %s\n", cg.c_str());
}

// ---- the model against simulated NATs ----

static void test_classify() {
    using namespace nat;
    const std::uint32_t lan = ip(192, 168, 1, 20);
    const Endpoint s1{ip(192, 0, 2, 3), nbo(3478)}, s2{ip(192, 0, 2, 4), nbo(19302)};
    for (Type t : {Type::Open, Type::FullCone, Type::Restricted, Type::PortRestricted, Type::Symmetric}) {
        Box b(t, t == Type::Open ? lan : ip(203, 0, 113, 4));
        const Endpoint a = b.stun(9307, s1), c = b.stun(9307, s2);
        StunSample sa{true, a.addr, ntohs(a.port)}, sb{true, c.addr, ntohs(c.port)};
        const Type got = classify({lan}, 9307, sa, sb);
        const Type want = t == Type::Open ? Type::Open : t == Type::Symmetric ? Type::Symmetric : Type::Cone;
        CHECK(got == want);
        std::printf("  %-22s -> classified %s\n", type_name(t), type_name(got));
    }
    CHECK(classify({lan}, 9307, StunSample{}, StunSample{}) == Type::Unknown);
    CHECK(classify({lan}, 9307, StunSample{true, ip(203, 0, 113, 4), 9307}, StunSample{}) == Type::Cone);
    CHECK(parse_type("sym") == Type::Symmetric && parse_type("full") == Type::FullCone &&
          parse_type("addr") == Type::Restricted && parse_type("port") == Type::PortRestricted &&
          parse_type("bogus") == Type::Unknown);
    // Advice names the remedy.
    const Rendezvous on = Rendezvous::On, off = Rendezvous::Off, na = Rendezvous::NotApplicable;
    CHECK(advice(Type::Symmetric, false, false, on, 9307).find("VPN") != std::string::npos);
    CHECK(advice(Type::Symmetric, true, false, on, 9307).find("Forward UDP+TCP 9307") != std::string::npos);
    CHECK(advice(Type::Cone, true, false, on, 9307).find("automatically") != std::string::npos);
    CHECK(advice(Type::Cone, true, true, on, 9307).find("open") != std::string::npos);
    // The DHT rendezvous is opt-in: off, the host's and the guest's lines say to turn it on on both PCs.
    CHECK(advice(Type::Cone, true, false, off, 9307).find("DHT option on both PCs") != std::string::npos);
    CHECK(advice(Type::Cone, true, false, off, 9307).find("automatically") == std::string::npos);
    CHECK(advice(Type::Restricted, false, false, off, 9307).find(kRendezvousOffHint) != std::string::npos);
    CHECK(advice(Type::Restricted, false, false, na, 9307).find("DHT") == std::string::npos);
    CHECK(advice(Type::Restricted, false, false, on, 9307).find("automatically") != std::string::npos);
}

// One guest connection attempt through two simulated NATs, with the real rudp streams: the host
// (party port 9307 behind `ht`) and the guest (port 5000 behind `gt`). The host's party code
// carries its STUN-mapped endpoint; `known`: the host punches the guest's STUN endpoint (its
// guest code / the rendezvous). Returns whether the stream opened.
static bool nat_attempt(nat::Type ht, nat::Type gt, bool known) {
    using namespace nat;
    const Endpoint s1{ip(192, 0, 2, 3), nbo(3478)};
    Box hb(ht, ht == Type::Open ? ip(203, 0, 113, 1) : ip(203, 0, 113, 1));
    Box gb(gt, ip(198, 51, 100, 2));
    const Endpoint host_pub = hb.stun(9307, s1);   // the party code
    const Endpoint guest_pub = gb.stun(5000, s1);  // the guest code / the rendezvous announce
    rudp::Mux host(11), guest(13);
    host.accepting = true;
    const auto t0 = rudp::Clock::now();
    if (known) host.punch(guest_pub, t0, 30000, 200);
    auto gs = guest.connect(host_pub, t0, 6000);
    std::shared_ptr<rudp::Stream> hs;
    struct Flight {
        int at;
        bool to_host;
        Endpoint from, to;
        std::vector<std::uint8_t> data;
    };
    std::vector<Flight> wire;
    for (int ms = 0; ms < 8000; ms += 5) {
        const auto now = t0 + std::chrono::milliseconds(ms);
        std::vector<rudp::Datagram> out;
        host.poll(now, out);
        for (auto& d : out) wire.push_back({ms + 20, d.to.addr == gb.public_addr(), hb.out(9307, d.to), d.to, d.data});
        out.clear();
        guest.poll(now, out);
        for (auto& d : out) wire.push_back({ms + 20, d.to.addr == hb.public_addr(), gb.out(5000, d.to), d.to, d.data});
        for (auto it = wire.begin(); it != wire.end();) {
            if (it->at > ms) {
                ++it;
                continue;
            }
            // to_host here means "addressed to the other side's public address".
            const bool from_host = it->from.addr == hb.public_addr();
            if (it->to_host) {
                Box& dst = from_host ? gb : hb;
                if (dst.in(it->to.port, it->from) != 0)
                    (from_host ? guest : host).on_datagram(it->from, it->data.data(), it->data.size(), now);
            }
            it = wire.erase(it);
        }
        if (!hs) hs = host.accept();
        if (gs->state() == rudp::Stream::State::Open && hs) return true;
        if (gs->state() == rudp::Stream::State::Failed) return false;
    }
    return false;
}

static void test_matrix() {
    using namespace nat;
    const Type types[] = {Type::Open, Type::FullCone, Type::Restricted, Type::PortRestricted, Type::Symmetric};
    int agree = 0, total = 0, punched_ok = 0, need_code = 0;
    std::printf("  host \\ guest          open  full  addr  port  sym   (o: connects with the endpoint known, "
                "x: never, d: direct)\n");
    for (Type h : types) {
        std::printf("  %-20s", type_name(h));
        for (Type g : types) {
            const bool with = nat_attempt(h, g, true), without = nat_attempt(h, g, false);
            const Side hs{h, false}, gs{g, false};
            const Path pw = predict(hs, gs, true), pn = predict(hs, gs, false);
            const bool ok = with == (pw != Path::None) && without == (pn != Path::None);
            CHECK(ok);
            if (!ok)
                std::fprintf(stderr, "  mismatch host %s guest %s: with %d (%s), without %d (%s)\n", type_name(h),
                             type_name(g), with, path_name(pw), without, path_name(pn));
            agree += ok;
            ++total;
            if (with && !without) ++need_code;
            if (with) ++punched_ok;
            std::printf("  %s   ", without ? "d" : with ? "o" : "x");
        }
        std::printf("\n");
    }
    std::printf("  model agrees with the simulation in %d/%d pairs; %d pairs connect, %d of them only once the "
                "host knows the guest's endpoint\n",
                agree, total, punched_ok, need_code);
    // An open port (UPnP / forward) always gives TCP.
    CHECK(predict({Type::Symmetric, true}, {Type::Symmetric, false}, false) == Path::Tcp);
}

static void test_netsim_kinds() {
    using namespace bbnet::netsim;
    Config c;
    std::string err;
    CHECK(parse("lat=0,nat=sym", &c, &err) && c.nat && c.nat_kind == NatKind::Symmetric);
    CHECK(describe(c).find("nat=sym") != std::string::npos);
    CHECK(parse("lat=0,nat=full", &c, &err) && c.nat_kind == NatKind::FullCone);
    CHECK(parse("lat=0,nat=addr", &c, &err) && c.nat_kind == NatKind::AddrRestricted);
    CHECK(parse("lat=0,nat=1", &c, &err) && c.nat && c.nat_kind == NatKind::PortRestricted);
    CHECK(!parse("lat=0,nat=weird", &c, &err));
    const std::uint32_t a = ip(127, 0, 0, 9), b = ip(127, 0, 0, 10);
    nat_note_out(a, nbo(1111));
    CHECK(nat_allow_in_kind(NatKind::PortRestricted, a, nbo(1111)));
    CHECK(!nat_allow_in_kind(NatKind::PortRestricted, a, nbo(2222)));
    CHECK(nat_allow_in_kind(NatKind::AddrRestricted, a, nbo(2222)));
    CHECK(!nat_allow_in_kind(NatKind::AddrRestricted, b, nbo(1111)));
    CHECK(nat_allow_in_kind(NatKind::FullCone, b, nbo(3333)));
    CHECK(!nat_allow_in_kind(NatKind::Symmetric, a, nbo(2222)));
    Config s;
    CHECK(parse("lat=0,nat=sym", &s, &err));
    reconfigure_for_tests(s);
    CHECK(nat_advertised_port(nbo(9307), 0) == nbo(10307) && nat_advertised_port(nbo(9307), 1) == nbo(11307));
    Config p;
    CHECK(parse("lat=0,nat=1", &p, &err));
    reconfigure_for_tests(p);
    CHECK(nat_advertised_port(nbo(9307), 1) == nbo(9307));
}

// ---- the DHT ----

// A DHT node enough for get_peers / announce_peer (BEP 5).
struct FakeNode {
    rdv::Id id{};
    Endpoint ep;
    std::vector<std::pair<rdv::Id, Endpoint>> known;
    std::map<rdv::Id, std::vector<Endpoint>> store;
    int announces = 0;
    std::string token_for(const Endpoint& from) const {
        return "tk" + std::string(reinterpret_cast<const char*>(&from.addr), 4);
    }
    std::string handle(const Endpoint& from, const std::uint8_t* d, std::size_t n) {
        rdv::BNode m;
        if (!rdv::bdecode(d, n, &m)) return {};
        const rdv::BNode* t = m.get("t");
        const rdv::BNode* q = m.get("q");
        const rdv::BNode* a = m.get("a");
        if (!t || !q || !a) return {};
        rdv::BNode r;
        r.kind = rdv::BNode::Dict;
        r.dict.emplace_back("id", rdv::BNode::str(std::string(reinterpret_cast<const char*>(id.data()), 20)));
        if (q->s == "get_peers") {
            const rdv::BNode* ih = a->get("info_hash");
            if (!ih || ih->s.size() != 20) return {};
            rdv::Id target;
            std::memcpy(target.data(), ih->s.data(), 20);
            r.dict.emplace_back("token", rdv::BNode::str(token_for(from)));
            auto k = known;
            std::sort(k.begin(), k.end(), [&](auto& x, auto& y) { return rdv::closer(target, x.first, y.first); });
            std::string nodes;
            for (std::size_t i = 0; i < k.size() && i < 8; ++i) {
                nodes.append(reinterpret_cast<const char*>(k[i].first.data()), 20);
                nodes.append(reinterpret_cast<const char*>(&k[i].second.addr), 4);
                nodes.append(reinterpret_cast<const char*>(&k[i].second.port), 2);
            }
            r.dict.emplace_back("nodes", rdv::BNode::str(nodes));
            if (auto it = store.find(target); it != store.end()) {
                rdv::BNode vals;
                vals.kind = rdv::BNode::List;
                for (const Endpoint& e : it->second)
                    vals.list.push_back(rdv::BNode::str(std::string(reinterpret_cast<const char*>(&e.addr), 4) +
                                                        std::string(reinterpret_cast<const char*>(&e.port), 2)));
                r.dict.emplace_back("values", vals);
            }
        } else if (q->s == "announce_peer") {
            const rdv::BNode* ih = a->get("info_hash");
            const rdv::BNode* tok = a->get("token");
            const rdv::BNode* port = a->get("port");
            if (!ih || ih->s.size() != 20 || !tok || tok->s != token_for(from) || !port) {
                rdv::BNode e;
                e.kind = rdv::BNode::Dict;
                e.dict.emplace_back("t", *t);
                e.dict.emplace_back("y", rdv::BNode::str("e"));
                return rdv::bencode(e);
            }
            rdv::Id target;
            std::memcpy(target.data(), ih->s.data(), 20);
            Endpoint e{from.addr, htons(static_cast<std::uint16_t>(port->i))};
            auto& v = store[target];
            if (std::find(v.begin(), v.end(), e) == v.end()) v.push_back(e);
            ++announces;
        }
        rdv::BNode msg;
        msg.kind = rdv::BNode::Dict;
        msg.dict.emplace_back("t", *t);
        msg.dict.emplace_back("y", rdv::BNode::str("r"));
        msg.dict.emplace_back("r", r);
        return rdv::bencode(msg);
    }
};

static void test_bencode() {
    using namespace rdv;
    const char* s = "d1:ad2:id20:abcdefghij01234567899:info_hash20:mnopqrstuvwxyz123456e1:q9:get_peers1:t2:aa1:y1:qe";
    BNode m;
    CHECK(bdecode(reinterpret_cast<const std::uint8_t*>(s), std::strlen(s), &m));
    CHECK(m.get("q") && m.get("q")->s == "get_peers" && m.get("a")->get("id")->s.size() == 20);
    CHECK(bencode(m) == s);  // canonical (sorted keys) round trip
    for (const char* bad : {"d1:a", "i12", "l", "5:abc", "d1:ai1ee1", "i-e", "di1ei1ee", "d1:ai1e1:b"}) {
        BNode x;
        CHECK(!bdecode(reinterpret_cast<const std::uint8_t*>(bad), std::strlen(bad), &x));
    }
    // A deep nest is refused, not a stack overflow.
    std::string deep(200, 'l');
    deep += std::string(200, 'e');
    BNode x;
    CHECK(!bdecode(reinterpret_cast<const std::uint8_t*>(deep.data()), deep.size(), &x));
    Id self{}, target{};
    self[0] = 1;
    target[0] = 2;
    const std::string gp = make_get_peers("xy", self, target);
    CHECK(gp.find("9:get_peers") != std::string::npos && gp.find("2:roi1e") != std::string::npos);
    const std::string an = make_announce("xz", self, target, 5555, "tok");
    CHECK(an.find("4:porti5555e") != std::string::npos && an.find("12:implied_porti0e") != std::string::npos);
    // The hash depends on the key only.
    std::uint8_t k1[32] = {1}, k2[32] = {2};
    CHECK(info_hash(k1, 32) == info_hash(k1, 32) && !(info_hash(k1, 32) == info_hash(k2, 32)));
    CHECK(!usable_node({ip(192, 168, 0, 1), nbo(6881)}, false) && !usable_node({ip(127, 0, 0, 1), nbo(1)}, false) &&
          usable_node({ip(127, 0, 0, 1), nbo(1)}, true) && usable_node({ip(203, 0, 113, 10), nbo(6881)}, false) &&
          !usable_node({ip(100, 64, 1, 1), nbo(6881)}, false));
}

// 300 simulated DHT nodes, each knowing 12 others: the guest's announce lands on the nodes
// closest to the hash and the host's lookup (a fresh node id, the same bootstrap) finds it.
static void test_lookup_sim() {
    using namespace rdv;
    std::mt19937 rng(42);
    std::vector<FakeNode> net(300);
    for (std::size_t i = 0; i < net.size(); ++i) {
        for (auto& b : net[i].id) b = static_cast<std::uint8_t>(rng());
        net[i].ep = {ip(192, 0, 2, static_cast<int>(i % 250 + 1)), nbo(static_cast<std::uint16_t>(6881 + i / 250))};
    }
    // Kademlia-like routing tables: up to 3 nodes per shared-prefix length (bucket).
    auto prefix = [](const rdv::Id& a, const rdv::Id& b) {
        for (int i = 0; i < 160; ++i)
            if (((a[i / 8] ^ b[i / 8]) >> (7 - i % 8)) & 1) return i;
        return 160;
    };
    for (std::size_t i = 0; i < net.size(); ++i) {
        std::vector<std::size_t> order(net.size());
        for (std::size_t j = 0; j < order.size(); ++j) order[j] = j;
        std::shuffle(order.begin(), order.end(), rng);
        std::map<int, int> per_bucket;
        for (std::size_t j : order)
            if (j != i && per_bucket[prefix(net[i].id, net[j].id)]++ < 3) net[i].known.emplace_back(net[j].id, net[j].ep);
    }
    std::uint8_t key[32];
    for (auto& b : key) b = static_cast<std::uint8_t>(rng());
    const Id target = info_hash(key, 32);
    auto run = [&](Lookup& lk, const Endpoint& me, double loss) {
        auto t = Clock::now();
        std::uniform_real_distribution<double> u(0, 1);
        for (int step = 0; step < 4000 && !lk.done(); ++step, t += std::chrono::milliseconds(10)) {
            std::vector<rudp::Datagram> out;
            lk.poll(t, out);
            for (auto& d : out) {
                if (u(rng) < loss) continue;
                for (FakeNode& n : net)
                    if (n.ep == d.to) {
                        const std::string r = n.handle(me, d.data.data(), d.data.size());
                        if (!r.empty() && u(rng) >= loss)
                            lk.on_datagram(n.ep, reinterpret_cast<const std::uint8_t*>(r.data()), r.size(), t);
                    }
            }
        }
    };
    Id gid{}, hid{};
    for (auto& b : gid) b = static_cast<std::uint8_t>(rng());
    for (auto& b : hid) b = static_cast<std::uint8_t>(rng());
    Lookup::Options go;
    go.announce = true;
    go.announce_port = 47961;
    const Endpoint guest_pub{ip(198, 51, 100, 2), nbo(5000)};
    Lookup g(target, gid, go, Clock::now());
    g.seed(net[0].ep);
    run(g, guest_pub, 0.05);
    CHECK(g.done() && g.announced() >= 4);
    // The announce reached the true closest nodes.
    auto sorted = net;
    std::sort(sorted.begin(), sorted.end(), [&](auto& a, auto& b) { return closer(target, a.id, b.id); });
    int top_hit = 0;
    for (int i = 0; i < 8; ++i) top_hit += sorted[i].announces > 0;
    Lookup h(target, hid, Lookup::Options{}, Clock::now());
    h.seed(net[1].ep);
    run(h, {ip(203, 0, 113, 1), nbo(9307)}, 0.05);
    const Endpoint want{guest_pub.addr, nbo(47961)};
    CHECK(h.done() && std::find(h.peers().begin(), h.peers().end(), want) != h.peers().end());
    std::printf("  simulated DHT (300 nodes, 5%% loss): guest asked %d nodes, stored at %d (%d of the 8 closest); "
                "host asked %d, found %zu endpoint(s)\n",
                g.queried(), g.announced(), top_hit, h.queried(), h.peers().size());
    CHECK(top_hit >= 5);
    // A second lookup seeded with the first one's closest nodes needs one hop.
    Lookup h2(target, hid, Lookup::Options{}, Clock::now());
    for (const Endpoint& e : h.closest()) h2.seed(e);
    run(h2, {ip(203, 0, 113, 1), nbo(9307)}, 0);
    CHECK(h2.done() && !h2.peers().empty() && h2.queried() <= 12);
    std::printf("  re-poll seeded with the closest nodes: %d queries\n", h2.queried());
}

// Fake DHT nodes on loopback sockets, served by one thread.
struct LoopDht {
    std::vector<FakeNode> nodes;
    std::vector<sock::Socket> socks;
    std::atomic<bool> stop{false};
    std::thread th;
    explicit LoopDht(int n) {
        sock::startup();
        std::mt19937 rng(9);
        nodes.resize(n);
        for (FakeNode& f : nodes) {
            for (auto& b : f.id) b = static_cast<std::uint8_t>(rng());
            sock::Socket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
            sock::socklen l = sizeof a;
            getsockname(s, reinterpret_cast<sockaddr*>(&a), &l);
            sock::set_nonblocking(s);
            f.ep = {a.sin_addr.s_addr, a.sin_port};
            socks.push_back(s);
        }
        for (FakeNode& f : nodes)
            for (FakeNode& o : nodes)
                if (&f != &o) f.known.emplace_back(o.id, o.ep);
        th = std::thread([this] {
            while (!stop) {
                for (std::size_t i = 0; i < socks.size(); ++i) {
                    for (;;) {
                        std::uint8_t buf[2048];
                        sockaddr_in from{};
                        sock::socklen fl = sizeof from;
                        const long r = ::recvfrom(socks[i], reinterpret_cast<char*>(buf), sizeof buf, 0,
                                                  reinterpret_cast<sockaddr*>(&from), &fl);
                        if (r <= 0) break;
                        const std::string out =
                            nodes[i].handle({from.sin_addr.s_addr, from.sin_port}, buf, static_cast<std::size_t>(r));
                        if (!out.empty())
                            ::sendto(socks[i], out.data(), static_cast<int>(out.size()), 0,
                                     reinterpret_cast<sockaddr*>(&from), sizeof from);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }
    ~LoopDht() {
        stop = true;
        th.join();
        for (auto s : socks) sock::close(s);
    }
    std::string bootstrap() const { return "127.0.0.1:" + std::to_string(ntohs(nodes[0].ep.port)); }
};

static void test_rendezvous_sockets() {
    LoopDht dht(5);
    std::uint8_t key[32] = {7, 7, 7};
    const rdv::Id target = rdv::info_hash(key, 32);
    std::mutex mu;
    std::vector<Endpoint> found;
    std::vector<std::string> logs;
    auto log = [&](const std::string& s) {
        std::lock_guard<std::mutex> lk(mu);
        logs.push_back(s);
    };
    rdv::Rendezvous host(target, {dht.bootstrap()}, true, log), guest(target, {dht.bootstrap()}, true, log);
    CHECK(host.start_host([&](const Endpoint& e) {
        std::lock_guard<std::mutex> lk(mu);
        if (std::find(found.begin(), found.end(), e) == found.end()) found.push_back(e);
    }, 200));
    std::atomic<bool> want{true};
    CHECK(guest.start_guest(4321, [&] { return want.load(); }, 300));
    const Endpoint expect{htonl(INADDR_LOOPBACK), htons(4321)};
    CHECK(wait_for([&] {
        std::lock_guard<std::mutex> lk(mu);
        return std::find(found.begin(), found.end(), expect) != found.end();
    }, 10000));
    want = false;
    host.stop();
    guest.stop();
    CHECK(guest.announces() > 0 && host.lookups() > 0);
    std::printf("  loopback DHT (5 nodes): host found the guest's 127.0.0.1:4321 after %d lookup(s); %d announce answer(s)\n",
                host.lookups(), guest.announces());
}

// The whole automatic path: the host behind the simulated NAT (netsim nat=1: no TCP listener,
// unsolicited UDP dropped), a guest with no guest code. The guest's link fails over to UDP and
// announces its endpoint; the host's rendezvous finds it and punches; the link comes up.
static LinkConfig cfg_for(const char* name) {
    LinkConfig c;
    c.name = name;
    c.password = "hunter2";
    c.bind_addr = "127.0.0.1";
    c.port = 0;
    c.secret = {1, 2, 3, 4, 5, 6, 7, 8};
    c.lost_timeout_ms = 15000;
    c.connect_timeout_ms = 15000;
    c.tcp_fallback_ms = 500;
    return c;
}

static void test_auto_punch_link() {
    bbnet::netsim::Config sc;
    std::string err;
    CHECK(bbnet::netsim::parse("lat=0,nat=1", &sc, &err));
    bbnet::netsim::reconfigure_for_tests(sc);
    LoopDht dht(4);
    LinkCallbacks cb;
    if (std::getenv("PARTY_TEST_VERBOSE"))
        cb.on_log = [](const std::string& l) {
            std::printf("    [%lld] %s\n",
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::steady_clock::now().time_since_epoch())
                                                   .count() %
                                               100000),
                        l.c_str());
        };
    PartyLink host(cfg_for("Host"), cb);
    CHECK(host.start_host(&err));
    PartyLink g(cfg_for("Carol"), cb);
    CHECK(g.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(wait_for([&] { return g.udp_local_port() != 0; }, 5000));
    const crypto::Key k = crypto::derive_party_key("hunter2", cfg_for("x").secret.data());
    const rdv::Id target = rdv::info_hash(k.data(), k.size());
    std::atomic<int> punched{0};
    rdv::Rendezvous hr(target, {dht.bootstrap()}, true, nullptr), gr(target, {dht.bootstrap()}, true, nullptr);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(hr.start_host([&](const Endpoint& e) {
        host.punch_guest(e.addr, e.port);
        ++punched;
    }, 300));
    CHECK(gr.start_guest(g.udp_local_port(), [&] { return g.state() != LinkState::Connected; }, 1000));
    CHECK(g.wait_connected(20000));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(g.connected_over_udp() && punched > 0);
    std::printf("  guest without a code behind the simulated NAT: connected over UDP %lld ms after the "
                "rendezvous started (host punched %d time(s))\n",
                static_cast<long long>(ms), punched.load());
    hr.stop();
    gr.stop();
    g.stop();
    host.stop();
}

// ---- F2 0.7.1: the route table (member relay, IPv6) and the path chooser ----

using bbnet::udp::Ep6;
using bbnet::udp::Ip6;
using bbnet::udp::Router;
using bbnet::udp::Wire;

static Ip6 ip6(const char* text) {
    Ip6 a;
    const bool ok = bbnet::udp::parse_ip6(text, &a);
    CHECK(ok);
    return a;
}

static void test_route_basics() {
    using namespace bbnet::udp;
    // IPv6 text both ways.
    for (const char* t : {"2001:db8::1", "::1", "2a02:8108:1c0:5e00:1d2:3e4:5f6:789", "fe80::1", "2001:db8:0:1::"}) {
        Ip6 a;
        CHECK(parse_ip6(t, &a));
        CHECK(ip6_text(a) == t);
    }
    Ip6 a;
    CHECK(parse_ip6("[2001:DB8::A]", &a) && ip6_text(a) == "2001:db8::a");
    CHECK(parse_ip6("fe80::1%12", &a) && ip6_text(a) == "fe80::1");
    for (const char* bad : {"", ":", "1:2:3", "2001:db8::1::2", "12345::", "1:2:3:4:5:6:7:8:9", "g::1", ":1::"}) CHECK(!parse_ip6(bad, &a));
    CHECK(ip6_global(ip6("2001:db8::1")) && !ip6_global(ip6("fe80::1")) && !ip6_global(ip6("fd7a::1")) &&
          ip6_loopback(ip6("::1")));
    // Synthetic keys: 240/5, keyed per process (stable within it), the port kept.
    std::uint32_t k1 = 0, k2 = 0;
    std::uint16_t p1 = 0, p2 = 0;
    {
        Router kr;
        CHECK(kr.key_of(Ep6{ip6("2001:db8::7"), nbo(9307)}, &k1, &p1));
        CHECK(kr.key_of(Ep6{ip6("2001:db8::7"), nbo(9307)}, &k2, &p2));
        CHECK(k1 == k2 && p1 == nbo(9307) && is_synthetic(k1) && !is_synthetic(ip(203, 0, 113, 1)));
        CHECK(reinterpret_cast<const std::uint8_t*>(&k1)[0] >= 240 && reinterpret_cast<const std::uint8_t*>(&k1)[0] <= 247);
        // Another process (another hash key) gives another key: nobody can aim at a peer's key.
        Router other;
        std::uint32_t k3 = 0;
        std::uint16_t p3 = 0;
        CHECK(other.key_of(Ep6{ip6("2001:db8::7"), nbo(9307)}, &k3, &p3) && k3 != k1);
    }
    // A synthetic key without an IPv6 route is never put on the IPv4 wire.
    Router r;
    std::vector<Wire> out;
    const std::uint8_t d[4] = {1, 2, 3, 4};
    r.outgoing(k1, p1, d, 4, rudp::Clock::now(), &out);
    CHECK(out.empty());
    r.set_ipv6(true);
    CHECK(r.key_of(Ep6{ip6("2001:db8::7"), nbo(9307)}, &k1, &p1));
    Wire in;
    in.v6 = true;
    in.addr6 = ip6("2001:db8::7");
    in.port = nbo(9307);
    in.data.assign(d, d + 4);
    std::vector<std::uint8_t> got;
    std::uint32_t sa = 0;
    std::uint16_t sp = 0;
    bool v6 = false;
    CHECK(r.incoming(in, rudp::Clock::now(), &got, &sa, &sp, &out, &v6) && sa == k1 && sp == p1 && v6 && got.size() == 4);
    r.outgoing(k1, p1, d, 4, rudp::Clock::now(), &out);  // the reply goes back over IPv6
    CHECK(out.size() == 1 && out[0].v6 && out[0].addr6 == in.addr6 && out[0].port == nbo(9307));
    // A bound key wins over the synthetic one, both ways.
    const std::uint32_t host = ip(203, 0, 113, 9);
    r.bind_v6(host, nbo(9307), Ep6{ip6("2001:db8::7"), nbo(9307)}, true);
    CHECK(r.incoming(in, rudp::Clock::now(), &got, &sa, &sp, &out) && sa == host && sp == nbo(9307));
    out.clear();
    r.route_v6(host, nbo(9307), false);  // a candidate only: IPv4 out, IPv6 still recognised in
    r.outgoing(host, nbo(9307), d, 4, rudp::Clock::now(), &out);
    CHECK(out.size() == 1 && !out[0].v6 && out[0].addr == host);
    r.set_ipv6(false);  // IPv6 off: nothing from the IPv6 socket gets in
    CHECK(!r.incoming(in, rudp::Clock::now(), &got, &sa, &sp, &out));
    for (auto& n : r.take_notes()) (void)n;

    // The party code's IPv6 tail and the BBG6 guest code.
    std::array<std::uint8_t, 8> secret{1, 2, 3, 4, 5, 6, 0, 0};
    std::array<std::uint8_t, 16> h6{};
    std::memcpy(h6.data(), ip6("2001:db8:1:2::abcd").b.data(), 16);
    const std::string c6 = make_party_code({203, 0, 113, 9}, 9307, secret, 0, &h6);
    const std::string c4 = make_party_code({203, 0, 113, 9}, 9307, secret, 0);
    CHECK(c6.rfind(c4 + "+6-", 0) == 0);
    PartyCode pc;
    std::string err;
    CHECK(decode_party_code(c6, &pc, &err) && pc.has_ipv6 && pc.ipv6 == h6 && pc.host == "203.0.113.9" &&
          pc.secret == secret);
    CHECK(decode_party_code(c4, &pc, &err) && !pc.has_ipv6);
    std::string typo = c6;
    typo[typo.size() - 3] = typo[typo.size() - 3] == 'A' ? 'B' : 'A';
    CHECK(!decode_party_code(typo, &pc, &err) && err.find("IPv6") != std::string::npos);
    CHECK(!decode_party_code("203.0.113.9:9307+6-AAAA", &pc, &err));
    std::printf("  party code with IPv6: %s\n", c6.c_str());
    std::uint8_t g6[16], back[16];
    std::memcpy(g6, ip6("2001:db8::42").b.data(), 16);
    const std::string gc6 = rudp::encode_guest_code6(g6, nbo(9307));
    std::uint16_t gp = 0;
    CHECK(rudp::decode_guest_code6(gc6, back, &gp) && std::memcmp(back, g6, 16) == 0 && gp == nbo(9307));
    CHECK(rudp::decode_guest_code6(" " + gc6 + " ", back, &gp));
    std::string gbad = gc6;
    gbad[6] = gbad[6] == '0' ? '1' : '0';
    CHECK(!rudp::decode_guest_code6(gbad, back, &gp) && !rudp::decode_guest_code6(rudp::encode_guest_code(1, 2), back, &gp));
    std::printf("  guest code with IPv6: %s\n", gc6.c_str());
    // 0.7.1 defaults (member relay on, IPv6 off): a host's code is exactly the 0.7.0 one. The
    // documented 0.7.0 examples decode and encode back unchanged, with no "+6" tail.
    for (const char* ex : {PARTY_CODE_EXAMPLE, PARTY_LAN_CODE_EXAMPLE}) {
        PartyCode pc0;
        CHECK(decode_party_code(ex, &pc0, &err) && !pc0.has_ipv6);
        const std::string again = make_party_code(pc0.ipv4, pc0.port, pc0.secret, pc0.flags);
        if (!pc0.lan_partial) CHECK(again == ex);
        CHECK(again.find('+') == std::string::npos);
    }
    // The tail only with IPv6 on, its socket, an address and no open IPv4 port.
    using netpath::code_carries_ipv6;
    for (int m = 0; m < 16; ++m) {
        const bool on = m & 1, sock = m & 2, addr = m & 4, open = m & 8;
        CHECK(code_carries_ipv6(on, sock, addr, open) == (on && sock && addr && !open));
    }
    CHECK(!code_carries_ipv6(false, true, true, false));  // IPv6 off (the default): never
    CHECK(!code_carries_ipv6(true, true, true, true));    // UPnP / forward / public IP: never
}

static void test_chooser() {
    using namespace netpath;
    Chooser::Config cfg;
    cfg.relay_after_ms = 20000;
    cfg.stick_failures = 2;
    Chooser c(cfg);
    const auto t0 = Clock::now();
    auto at = [&](int ms) { return t0 + std::chrono::milliseconds(ms); };
    c.start(t0);
    CHECK(c.current().route == Route::Direct);  // no IPv6: direct only
    c.set_ipv6(true);
    CHECK(c.current().route == Route::Ipv6);  // not tried yet: IPv6 first (TCP, then UDP over IPv6)
    CHECK(c.add_relay(ip(198, 51, 100, 7), nbo(9307), "guest code") && !c.add_relay(ip(198, 51, 100, 7), nbo(9307), "x"));
    CHECK(c.failed(at(7000)).route == Route::Direct);
    CHECK(c.failed(at(14000)).route == Route::Ipv6);  // relays wait for 20 s
    CHECK(c.failed(at(21000)).route == Route::Direct);
    const Step r = c.failed(at(28000));
    CHECK(r.route == Route::Relay && r.relay == 0);
    CHECK(c.describe(r).find("198.51.100.7:9307") != std::string::npos);
    CHECK(c.failed(at(35000)).route == Route::Ipv6);  // round again: direct steps keep their turns
    CHECK(c.failed(at(42000)).route == Route::Direct);
    CHECK(c.failed(at(49000)).route == Route::Relay);
    c.connected();  // the relay worked: reconnects stay on it
    c.lost();
    CHECK(c.failed(at(60000)).route == Route::Relay);
    CHECK(c.failed(at(65000)).route == Route::Ipv6);  // two failed reconnects: the rounds from the top
    CHECK(c.relays()[0].failures == 2);
    // Without IPv6 or relays it stays direct.
    Chooser d;
    d.start(t0);
    CHECK(d.failed(at(30000)).route == Route::Direct && d.failed(at(60000)).route == Route::Direct);
}

// Three (or two) players behind simulated NATs, each with the real route table and reliable-UDP
// streams, on one simulated wire (15 ms one way). IPv6: an address and a stateful firewall per player.
namespace sim3 {

struct Party {
    std::string name;
    nat::Box box;
    Router router;
    rudp::Mux mux;
    std::shared_ptr<rudp::Stream> stream;  // guest: its link; host: the accepted one
    rudp::Endpoint accepted_from;
    std::uint64_t game_in = 0;  // non-link datagrams that reached the layers above
    rudp::Endpoint last_game_src;  // where they came from (a game answers its peers there)
    Ip6 a6;
    bool has6 = false, fw_open6 = false;
    std::set<std::pair<Ip6, std::uint16_t>> sent6;  // the stateful IPv6 firewall's flows
    Party(std::string n, nat::Type t, std::uint32_t pub, std::uint32_t seed)
        : name(std::move(n)), box(t, pub, 40000), mux(seed) {}
};

struct Net {
    struct Flight {
        int at;
        bool v6;
        rudp::Endpoint from, to;  // IPv4 (public, after the sender's NAT)
        Ip6 from6, to6;
        std::uint16_t from6_port = 0, to6_port = 0;
        std::vector<std::uint8_t> data;
    };
    std::vector<Party*> parties;
    std::vector<Flight> wire;
    int now = 0;
    rudp::Clock::time_point t0 = rudp::Clock::now();
    rudp::Clock::time_point t() const { return t0 + std::chrono::milliseconds(now); }

    void put(Party& p, const std::vector<Wire>& out) {
        for (const Wire& w : out) {
            Flight f;
            f.at = now + 15;
            f.v6 = w.v6;
            f.data = w.data;
            if (!w.v6) {
                f.to = {w.addr, w.port};
                f.from = p.box.out(9307, f.to);
            } else {
                if (!p.has6) continue;
                f.from6 = p.a6;
                f.from6_port = nbo(9307);
                f.to6 = w.addr6;
                f.to6_port = w.port;
                p.sent6.insert({w.addr6, w.port});
            }
            wire.push_back(std::move(f));
        }
    }
    void send(Party& p, std::uint32_t addr, std::uint16_t port, const std::uint8_t* d, std::size_t n) {
        std::vector<Wire> out;
        p.router.outgoing(addr, port, d, n, t(), &out);
        put(p, out);
    }
    void deliver(Party& p, const Wire& in) {
        std::vector<std::uint8_t> d;
        std::uint32_t sa = 0;
        std::uint16_t sp = 0;
        std::vector<Wire> rep;
        const bool up = p.router.incoming(in, t(), &d, &sa, &sp, &rep);
        put(p, rep);
        if (!up) return;
        if (rudp::is_rudp(d.data(), d.size())) p.mux.on_datagram({sa, sp}, d.data(), d.size(), t());
        else {
            ++p.game_in;
            p.last_game_src = {sa, sp};
        }
    }
    void step() {
        for (std::size_t i = 0; i < wire.size();) {
            if (wire[i].at > now) {
                ++i;
                continue;
            }
            Flight f = std::move(wire[i]);
            wire.erase(wire.begin() + static_cast<std::ptrdiff_t>(i));
            for (Party* p : parties) {
                Wire in;
                if (!f.v6) {
                    if (f.to.addr != p->box.public_addr()) continue;
                    if (!p->box.in(f.to.port, f.from)) break;  // its NAT drops it
                    in.addr = f.from.addr;
                    in.port = f.from.port;
                } else {
                    if (!p->has6 || !(f.to6 == p->a6)) continue;
                    if (!p->fw_open6 && !p->sent6.count({f.from6, f.from6_port})) break;  // its firewall drops it
                    in.v6 = true;
                    in.addr6 = f.from6;
                    in.port = f.from6_port;
                }
                in.data = std::move(f.data);
                deliver(*p, in);
                break;
            }
        }
        for (Party* p : parties) {
            std::vector<Wire> out;
            p->router.poll(t(), &out);
            put(*p, out);
            std::vector<rudp::Datagram> dg;
            p->mux.poll(t(), dg);
            for (const rudp::Datagram& d : dg) send(*p, d.to.addr, d.to.port, d.data.data(), d.data.size());
            if (p->mux.accepting && !p->stream) {
                if (auto s = p->mux.accept()) {
                    p->stream = s;
                    p->accepted_from = s->peer();
                }
            }
        }
        now += 5;
    }
};

crypto::Key key_of_pw(const char* pw) {
    std::uint8_t secret[8] = {9, 8, 7, 6, 5, 4, 0, 0};
    return crypto::relay_key(crypto::derive_party_key(pw, secret));
}
Router::Pake pake(const crypto::Key& k) {
    Router::Pake p;
    p.generator = [k](const std::uint8_t sid[16], std::uint8_t g[32]) { crypto::cpace_generator(k, sid, g); };
    p.x25519 = [](const std::uint8_t sc[32], const std::uint8_t pt[32], std::uint8_t out[32]) {
        return crypto::x25519(sc, pt, out);
    };
    p.hash = [](const std::uint8_t* d, std::size_t n, std::uint8_t out[32]) {
        const crypto::Key h = crypto::blake2b256(d, n);
        std::memcpy(out, h.data(), 32);
    };
    return p;
}
Router::KeyedMac kmac() {
    return [](const std::uint8_t key[32], const std::uint8_t* d, std::size_t n, std::uint8_t out[16]) {
        crypto::Key k;
        std::memcpy(k.data(), key, 32);
        crypto::mac16(k, d, n, out);
    };
}

struct RelayResult {
    bool link = false, data = false;
    rudp::Endpoint host_saw;
    // game traffic between the relayed guest and the member (the real 2-PC run 20261021_1929):
    bool game_to_lan = false;     // the guest's game datagram to the member's LAN address arrived
    bool game_to_public = false;  // ... to the endpoint it relays for us at arrived
    rudp::Endpoint member_heard_at;  // where the guest heard the member's answer from
};

// The guest reaches the host only through the member: the host and the member already play
// (they exchange game datagrams; the host knows the member's endpoint), the member relays. With
// `punch`, the member opens its NAT towards the guest (the host had its guest code / DHT entry:
// relay_punch). `member_key` != the guest's: a party member with another key.
RelayResult relay_run(nat::Type ht, nat::Type mt, nat::Type gt, bool punch, const crypto::Key& gkey,
                      const crypto::Key& mkey) {
    const rudp::Endpoint s1{ip(192, 0, 2, 3), nbo(3478)};
    Party H("host", ht, ip(203, 0, 113, 1), 21), M("member", mt, ip(192, 0, 2, 50), 23), G("guest", gt, ip(198, 51, 100, 2), 29);
    Net net;
    net.parties = {&H, &M, &G};
    const rudp::Endpoint hp = H.box.stun(9307, s1), mp = M.box.stun(9307, s1), gp = G.box.stun(9307, s1);
    H.mux.accepting = true;
    // The member's key reaches the host over its (encrypted) party link: relay_key.
    const Router::MemberKey mk = Router::new_member_key();
    for (Party* p : {&H, &M, &G}) p->router.set_keyed_mac(kmac());
    H.router.accept_relayed(true);
    H.router.set_member_key(mk, M.box.public_addr(), "Member");
    const std::uint8_t game[8] = {0xff, 0xc3, 40, 40, 1, 2, 3, 4};
    bool m_heard_host = false;
    RelayResult res;
    for (; net.now < 12000;) {
        // The host and the member play together (game datagrams both ways, the host punching the
        // member as for its guest code); the member opens its NAT towards the guest.
        if (net.now % 100 == 0) {
            // The host's game addresses the member where its STUN answer saw it (its room record).
            const rudp::Endpoint to_m = H.last_game_src.addr ? H.last_game_src : mp;
            net.send(H, to_m.addr, to_m.port, game, sizeof game);
            net.send(M, hp.addr, hp.port, game, sizeof game);
            if (punch) net.send(M, gp.addr, gp.port, udp_probe(), 8);
        }
        if (!m_heard_host && M.game_in > 0) {
            m_heard_host = true;  // its link is up: it serves
            M.router.serve(true, hp.addr, hp.port, pake(mkey), "Member", {}, mk);
        }
        if (net.now == 1500) {
            G.router.relay_use(mp.addr, mp.port, hp.addr, hp.port, pake(gkey), net.t());
            G.stream = G.mux.connect(hp, net.t(), 8000);
        }
        net.step();
        if (G.stream && G.stream->state() == rudp::Stream::State::Open && H.stream && !res.link) {
            res.link = true;
            res.host_saw = H.accepted_from;
            std::vector<std::uint8_t> blob(40000);
            for (std::size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<std::uint8_t>(i * 7);
            G.stream->write(blob.data(), blob.size());
            H.stream->write(blob.data(), blob.size());
        }
        if (res.link && G.stream->readable() == 40000 && H.stream->readable() == 40000 && !res.data) {
            res.data = true;
            // The game's own traffic with the member: the host lists the member at its LAN address
            // (its link came over the LAN), which the relayed guest cannot reach; the endpoint the
            // member relays at is reachable, and the member's answers come from there.
            const std::uint8_t g2m[8] = {0xff, 0xc3, 40, 40, 9, 9, 9, 9};
            const std::uint64_t m_before = M.game_in;
            net.send(G, ip(192, 168, 56, 1), nbo(9307), g2m, sizeof g2m);  // the LAN record (the bug)
            for (int i = 0; i < 20; ++i) net.step();
            res.game_to_lan = M.game_in > m_before;
            const std::uint64_t m_mid = M.game_in;
            net.send(G, mp.addr, mp.port, g2m, sizeof g2m);  // set_peer_endpoint's record (the fix)
            for (int i = 0; i < 20; ++i) net.step();
            res.game_to_public = M.game_in > m_mid;
            const rudp::Endpoint g_seen = M.last_game_src;  // the guest as the member hears it
            G.last_game_src = {};
            net.send(M, g_seen.addr, g_seen.port, g2m, sizeof g2m);
            for (int i = 0; i < 20; ++i) net.step();
            res.member_heard_at = G.last_game_src;
            break;
        }
    }
    return res;
}

}  // namespace sim3

static void test_member_relay_matrix() {
    using namespace nat;
    const Type types[] = {Type::Open, Type::FullCone, Type::Restricted, Type::PortRestricted, Type::Symmetric};
    const crypto::Key k = sim3::key_of_pw("hunter");
    int agree = 0, total = 0, only_relay = 0, only_relay_nocode = 0;
    std::printf("  host / member -> guest (o: through the member relay only, r: relay works, d: direct works too, x: neither)\n");
    for (bool punch : {true, false}) {
        std::printf("  %s\n", punch ? "the host has the guest's endpoint (guest code / DHT): the member punches it too"
                                    : "no guest code, no DHT: nobody knows the guest's endpoint");
        for (Type h : types) {
            for (Type m : types) {
                std::printf("    %-16s %-16s", type_name(h), type_name(m));
                for (Type g : types) {
                    // The model: the member is in the party (the host reached it, punching its code), and
                    // the guest reaches the member as it would a host that punches it (or one open to all).
                    const bool m_in = nat_attempt(h, m, true);
                    const bool g_to_m = nat_attempt(m, g, punch);
                    const bool want = m_in && g_to_m;
                    const sim3::RelayResult r = sim3::relay_run(h, m, g, punch, k, k);
                    const bool ok = r.link && r.data;
                    CHECK(ok == want);
                    if (ok != want)
                        std::fprintf(stderr, "  mismatch host %s member %s guest %s punch %d: relay %d, model %d\n",
                                     type_name(h), type_name(m), type_name(g), punch, ok, want);
                    agree += ok == want;
                    ++total;
                    const bool direct = nat_attempt(h, g, punch);
                    if (ok && !direct) (punch ? only_relay : only_relay_nocode)++;
                    std::printf(" %s", ok ? (direct ? "d" : "o") : "x");
                }
                std::printf("\n");
            }
        }
    }
    std::printf("  relay agrees with the model in %d/%d triples; only the member relay connects in %d (with the "
                "guest's endpoint known) + %d (without) triples\n",
                agree, total, only_relay, only_relay_nocode);
    CHECK(only_relay > 0 && only_relay_nocode > 0);
    // The relayed guest is at its endpoint as the member sees it (its own address: its password checks
    // and lockout count against it at the host, not against the member).
    const sim3::RelayResult r = sim3::relay_run(Type::PortRestricted, Type::FullCone, Type::Symmetric, true, k, k);
    CHECK(r.link && r.host_saw.addr == ip(198, 51, 100, 2));
    // The real-network case (20261021_1929): the party link through the relay works, but game
    // traffic to the member's LAN address (as the host lists it) never arrives; to the endpoint the
    // member relays at it does, and the member answers from exactly there (the address the guest's
    // records now carry: RemoteGuest::set_peer_endpoint, party-host-test).
    CHECK(r.data && !r.game_to_lan && r.game_to_public);
    CHECK(r.member_heard_at.addr == ip(192, 0, 2, 50));
}

// Delivers the wires addressed to `to_addr` (any port) into `to`, as coming from from_addr:from_port.
static void sec_pump(std::vector<Wire>& out, std::uint32_t from_addr, std::uint16_t from_port, Router& to,
                     std::uint32_t to_addr, rudp::Clock::time_point t, std::vector<Wire>* replies,
                     std::vector<std::vector<std::uint8_t>>* delivered = nullptr, std::uint32_t* src = nullptr,
                     std::uint16_t* srcp = nullptr) {
    for (Wire& w : out) {
        if (w.v6 || w.addr != to_addr) continue;
        Wire in;
        in.addr = from_addr;
        in.port = from_port;
        in.data = w.data;
        std::vector<std::uint8_t> d;
        std::uint32_t sa = 0;
        std::uint16_t sp = 0;
        if (to.incoming(in, t, &d, &sa, &sp, replies)) {
            if (delivered) delivered->push_back(d);
            if (src) *src = sa;
            if (srcp) *srcp = sp;
        }
    }
    out.clear();
}

// A guest's handshake with a member over a direct wire; returns the guest's state.
static Router::ClientState sec_handshake(Router& g, std::uint32_t ga, Router& m, std::uint32_t ma, std::uint16_t P,
                                         std::uint32_t ha, const Router::Pake& pake, rudp::Clock::time_point t,
                                         int* code = nullptr) {
    g.set_keyed_mac(sim3::kmac());
    m.set_keyed_mac(sim3::kmac());
    g.relay_use(ma, P, ha, P, pake, t);
    std::vector<Wire> go, mo;
    for (int k = 0; k < 3; ++k) {
        g.poll(t, &go);
        sec_pump(go, ga, P, m, ma, t, &mo);
        sec_pump(mo, ma, P, g, ga, t, &go);
    }
    return g.relay_state(nullptr, code);
}

static void test_member_relay_security() {
    using namespace nat;
    const crypto::Key good = sim3::key_of_pw("hunter"), other = sim3::key_of_pw("not the password");
    // Another party key: the member refuses (Q5), nothing reaches the host.
    {
        const sim3::RelayResult r = sim3::relay_run(Type::PortRestricted, Type::Open, Type::PortRestricted, true, other, good);
        CHECK(!r.link);
    }
    const std::uint32_t M = ip(192, 0, 2, 50), G = ip(198, 51, 100, 2), H = ip(203, 0, 113, 1), X = ip(198, 51, 100, 66);
    const std::uint32_t M2 = ip(192, 0, 2, 51);
    const std::uint16_t P = nbo(9307);
    const auto t0 = rudp::Clock::now();
    auto at = [&](int ms) { return t0 + std::chrono::milliseconds(ms); };
    AuthThrottle throttle;
    std::int64_t clock_ms = 0;
    Router::Throttle th;
    th.locked = [&](std::uint32_t a, std::int64_t* rem) { return throttle.Locked(a, clock_ms, rem); };
    th.fail = [&](std::uint32_t a) { throttle.Fail(a, clock_ms); };
    th.success = [&](std::uint32_t a) { throttle.Success(a); };
    const Router::MemberKey mk = Router::new_member_key(), mk2 = Router::new_member_key();
    Router m, g, h;
    for (Router* r : {&m, &g, &h}) r->set_keyed_mac(sim3::kmac());

    // Not serving: a refusal, smaller than the hello (no amplification); Q2 too.
    CHECK(sec_handshake(g, G, m, M, P, H, sim3::pake(good), t0) == Router::ClientState::Refused);
    m.serve(true, H, P, sim3::pake(good), "Hunter1", th, mk);
    {
        std::vector<Wire> go, mo;
        g.relay_use(M, P, H, P, sim3::pake(good), t0);
        g.poll(t0, &go);
        CHECK(go.size() == 1 && go[0].data.size() == Router::kQ1Size);
        sec_pump(go, G, P, m, M, t0, &mo);
        CHECK(mo.size() == 1 && mo[0].data.size() <= Router::kQ1Size);
    }
    // N1: the member's answer to a stranger's hello is its PAKE point only (51 bytes): nothing in
    // it can be checked against a password guess (no MAC, no proof).
    {
        std::vector<Wire> go, mo;
        g.relay_use(M, P, H, P, sim3::pake(other), t0);  // a stranger with a guessed password
        g.poll(t0, &go);
        sec_pump(go, G, P, m, M, t0, &mo);
        CHECK(mo.size() == 1 && mo[0].data.size() == 67 && mo[0].data[2] == 2);
        // The same hello from a guest with the right password gets the same kind of answer.
        Router gr;
        gr.set_keyed_mac(sim3::kmac());
        gr.relay_use(M, P, H, P, sim3::pake(good), t0);
        std::vector<Wire> go2, mo2;
        gr.poll(t0, &go2);
        sec_pump(go2, G, nbo(9400), m, M, t0, &mo2);
        CHECK(mo2.size() == 1 && mo2[0].data.size() == 67);
    }
    // An impostor member (another password): the guest's confirmation fails there (one online guess
    // for the impostor, no offline test), and no welcome the impostor can make is accepted.
    {
        Router imp, g2;
        imp.set_keyed_mac(sim3::kmac());
        g2.set_keyed_mac(sim3::kmac());
        imp.serve(true, H, P, sim3::pake(other), "Impostor", {}, Router::new_member_key());
        int code = 0;
        CHECK(sec_handshake(g2, G, imp, M, P, H, sim3::pake(good), t0, &code) == Router::ClientState::Refused &&
              code == Router::kWrongKey);
        // A hand-made welcome with any tag: ignored.
        Router g3;
        g3.set_keyed_mac(sim3::kmac());
        g3.relay_use(M, P, H, P, sim3::pake(good), t0);
        std::vector<Wire> go, io;
        g3.poll(t0, &go);
        sec_pump(go, G, P, imp, M, t0, &io);
        sec_pump(io, M, P, g3, G, t0, &go);  // Q2 -> the guest's Q3
        CHECK(go.size() == 1 && go[0].data[2] == 3 && go[0].data.size() == 115);
        std::vector<Wire> fake{Wire{}};
        fake[0].addr = G;
        fake[0].data = {0xfc, 'Q', 4};
        fake[0].data.insert(fake[0].data.end(), go[0].data.begin() + 3, go[0].data.begin() + 19);  // cn
        fake[0].data.insert(fake[0].data.end(), {0, 0, 0, 7, 3, 'I', 'm', 'p'});
        fake[0].data.resize(fake[0].data.size() + 16, 0x33);
        std::vector<Wire> none;
        sec_pump(fake, M, P, g3, G, t0, &none);
        CHECK(g3.relay_state() == Router::ClientState::Refused);  // and backed off (R3)
    }
    // A low-order point as the guest's Ya: the member's X25519 gives zero, the confirmation fails.
    {
        std::vector<Wire> q{Wire{}};
        q[0].addr = M;
        q[0].data = {0xfc, 'Q', 1};
        q[0].data.resize(19, 9);
        q[0].data.resize(19 + 32, 0);  // Ya = 0 (low order)
        q[0].data.resize(Router::kQ1Size, 0);
        std::vector<Wire> ans;
        sec_pump(q, X, nbo(9555), m, M, t0, &ans);
        CHECK(ans.size() == 1);
        std::vector<Wire> q3{Wire{}};
        q3[0].addr = M;
        q3[0].data = {0xfc, 'Q', 3};
        q3[0].data.resize(19, 9);
        q3[0].data.resize(51, 0);
        q3[0].data.insert(q3[0].data.end(), ans[0].data.begin() + 19, ans[0].data.begin() + 67);  // Yb, cookie
        q3[0].data.resize(115, 0);
        std::vector<Wire> r3;
        sec_pump(q3, X, nbo(9555), m, M, t0, &r3);
        CHECK(r3.size() == 1 && r3[0].data[2] == 5 && r3[0].data[19] == Router::kWrongKey);
        clock_ms += 61000;  // that wrong proof does not count against the tests below
    }
    // The PAKE transcript: a full handshake on the wire, then its Q3 replayed by an observer from
    // another address: the member's point is bound to the guest's endpoint, so nothing comes back.
    {
        Router gt, mt;
        gt.set_keyed_mac(sim3::kmac());
        mt.set_keyed_mac(sim3::kmac());
        mt.serve(true, H, P, sim3::pake(good), "T", {}, Router::new_member_key());
        gt.relay_use(M, P, H, P, sim3::pake(good), t0);
        std::vector<Wire> q1, q2, q3, q4;
        gt.poll(t0, &q1);
        CHECK(q1.size() == 1 && q1[0].data[2] == 1);
        const Wire w1 = q1[0];
        sec_pump(q1, G, P, mt, M, t0, &q2);
        CHECK(q2.size() == 1 && q2[0].data[2] == 2);
        sec_pump(q2, M, P, gt, G, t0, &q3);
        CHECK(q3.size() == 1 && q3[0].data[2] == 3);
        const Wire w3 = q3[0];
        sec_pump(q3, G, P, mt, M, t0, &q4);
        CHECK(q4.size() == 1 && q4[0].data[2] == 4);
        std::vector<Wire> none;
        sec_pump(q4, M, P, gt, G, t0, &none);
        CHECK(gt.relay_state() == Router::ClientState::Ready);
        // Q1 / Q3 carry the guest's point, Q2 the member's, Q3 / Q4 a 16-byte confirmation each.
        CHECK(w1.data.size() == Router::kQ1Size && w3.data.size() == 115);
        std::vector<Wire> rp{w3}, ans;
        sec_pump(rp, X, P, mt, M, t0, &ans);
        CHECK(ans.empty());
        rp = {w3};
        sec_pump(rp, G, nbo(9999), mt, M, t0, &ans);  // the guest's address, another port
        CHECK(ans.empty());
    }
    // The real handshake.
    std::string name;
    CHECK(sec_handshake(g, G, m, M, P, H, sim3::pake(good), t0) == Router::ClientState::Ready);
    g.relay_state(&name);
    CHECK(name == "Hunter1" && m.served().size() == 1);

    // H1: the guest's datagram for the host leaves the member as a MACed V frame.
    const std::uint8_t dgram[5] = {0xfa, 'L', 1, 2, 3};
    std::vector<Wire> gout, mout, hout;
    g.outgoing(H, P, dgram, 5, t0, &gout);
    CHECK(gout.size() == 1 && gout[0].addr == M && gout[0].data[1] == 'T');
    sec_pump(gout, G, P, m, M, t0, &mout);
    CHECK(mout.size() == 1 && mout[0].addr == H && mout[0].data[1] == 'V');
    const Wire v1 = mout[0];
    std::vector<std::vector<std::uint8_t>> got;
    // The host without the member's key (no relay_key over its link yet): dropped.
    h.accept_relayed(true);
    std::vector<Wire> tmp{v1};
    sec_pump(tmp, M, P, h, H, t0, &hout, &got);
    CHECK(got.empty() && h.host_routes().empty());
    h.set_member_key(mk, M, "Hunter1");
    // Spoofed from the member's IP on another port, with a forged MAC: dropped.
    {
        Wire f = v1;
        f.data[30] ^= 1;  // a MAC byte
        std::vector<Wire> t{f};
        sec_pump(t, M, nbo(1234), h, H, t0, &hout, &got);
        Wire f2 = v1;
        f2.data[f2.data.size() - 1] ^= 1;  // the payload
        t = {f2};
        sec_pump(t, M, P, h, H, t0, &hout, &got);
        CHECK(got.empty());
    }
    // From another address with the right MAC (a stranger replaying a captured frame): dropped.
    tmp = {v1};
    sec_pump(tmp, X, P, h, H, t0, &hout, &got);
    CHECK(got.empty());
    // The real frame: delivered from the guest's endpoint.
    std::uint32_t sa = 0;
    std::uint16_t sp = 0;
    tmp = {v1};
    sec_pump(tmp, M, P, h, H, t0, &hout, &got, &sa, &sp);
    CHECK(got.size() == 1 && got[0].size() == 5 && sa == G && sp == P && h.relayed_guest(G, P));
    // Replayed: dropped.
    tmp = {v1};
    sec_pump(tmp, M, P, h, H, t0, &hout, &got);
    CHECK(got.size() == 1);
    // The same member's next frame from another port of its IP (spoofed, right key unknown to the
    // spoofer: this one is genuine but the endpoint is bound): dropped while the bound one is live.
    g.outgoing(H, P, dgram, 5, t0, &gout);
    sec_pump(gout, G, P, m, M, t0, &mout);
    tmp = mout;
    mout.clear();
    sec_pump(tmp, M, nbo(4444), h, H, at(100), &hout, &got);
    CHECK(got.size() == 1);

    // The host's answer: a MACed v frame to the member's bound endpoint, then t to the guest.
    h.outgoing(G, P, dgram, 5, at(200), &hout);
    CHECK(hout.size() == 1 && hout[0].addr == M && hout[0].port == P && hout[0].data[1] == 'v');
    const Wire vh = hout[0];
    hout.clear();
    {
        Wire f = vh;
        f.data[25] ^= 1;  // forged
        std::vector<Wire> t{f};
        sec_pump(t, H, P, m, M, at(200), &mout);
        t = {vh};
        sec_pump(t, X, P, m, M, at(200), &mout);  // from someone else
        CHECK(mout.empty());
    }
    tmp = {vh};
    sec_pump(tmp, H, P, m, M, at(200), &mout);
    CHECK(mout.size() == 1 && mout[0].addr == G && mout[0].data[1] == 't');
    tmp = {vh};
    std::vector<Wire> again;
    sec_pump(tmp, H, P, m, M, at(200), &again);  // replayed
    CHECK(again.empty());
    got.clear();
    sec_pump(mout, M, P, g, G, at(200), &gout, &got);
    CHECK(got.size() == 1 && got[0].size() == 5);

    // A T frame from a stranger (no session): nothing goes to the host.
    {
        std::vector<Wire> t{Wire{}};
        t[0].addr = M;
        t[0].data = {0xfc, 'T', 0, 0, 0, 1, 0xfa, 'L', 1};
        sec_pump(t, X, P, m, M, at(300), &mout);
        CHECK(mout.empty());
    }

    // Builds a genuine V frame of member key `key` for guest ga:gp (as a member's router would).
    std::uint64_t ctr = 1000;
    auto vframe = [&](const Router::MemberKey& key, std::uint32_t ga, std::uint16_t gp, bool start) {
        std::vector<std::uint8_t> f{0xfc, 'V', static_cast<std::uint8_t>(start ? 1 : 0)};
        for (int i = 3; i >= 0; --i) f.push_back(static_cast<std::uint8_t>(key.id >> (8 * i)));
        ++ctr;
        for (int i = 7; i >= 0; --i) f.push_back(static_cast<std::uint8_t>(ctr >> (8 * i)));
        f.insert(f.end(), reinterpret_cast<const std::uint8_t*>(&ga), reinterpret_cast<const std::uint8_t*>(&ga) + 4);
        f.insert(f.end(), reinterpret_cast<const std::uint8_t*>(&gp), reinterpret_cast<const std::uint8_t*>(&gp) + 2);
        const std::size_t mac_at = f.size();
        f.resize(f.size() + 16);
        f.insert(f.end(), dgram, dgram + 5);
        std::vector<std::uint8_t> msg(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(mac_at));
        msg.insert(msg.end(), f.begin() + static_cast<std::ptrdiff_t>(mac_at + 16), f.end());
        sim3::kmac()(key.key.data(), msg.data(), msg.size(), f.data() + mac_at);
        Wire w;
        w.addr = H;
        w.port = P;
        w.data = f;
        return w;
    };
    auto host_takes = [&](Router& host, const Wire& w, std::uint32_t from, rudp::Clock::time_point t) {
        std::vector<Wire> o{w}, r;
        std::vector<std::vector<std::uint8_t>> dl;
        sec_pump(o, from, P, host, H, t, &r, &dl);
        return !dl.empty();
    };
    // L1: guest keys a member may not hand us: loopback, unspecified, multicast, broadcast, our own.
    {
        Router hh;
        hh.set_keyed_mac(sim3::kmac());
        hh.accept_relayed(true);
        hh.set_member_key(mk, M, "Hunter1");
        hh.set_own_addrs({H});
        CHECK(!host_takes(hh, vframe(mk, ip(127, 0, 0, 1), P, true), M, t0));
        CHECK(!host_takes(hh, vframe(mk, ip(0, 0, 0, 0), P, true), M, t0));
        CHECK(!host_takes(hh, vframe(mk, ip(224, 0, 0, 5), P, true), M, t0));
        CHECK(!host_takes(hh, vframe(mk, ip(255, 255, 255, 255), P, true), M, t0));
        CHECK(!host_takes(hh, vframe(mk, H, nbo(5000), true), M, t0));
        CHECK(host_takes(hh, vframe(mk, ip(198, 51, 100, 7), P, true), M, t0));
        // a member on loopback (a local test) may relay a loopback guest
        Router hl;
        hl.set_keyed_mac(sim3::kmac());
        hl.accept_relayed(true);
        hl.set_member_key(mk2, ip(127, 0, 0, 1), "Local");
        CHECK(host_takes(hl, vframe(mk2, ip(127, 0, 0, 1), nbo(48641), true), ip(127, 0, 0, 1), t0));
    }
    // H1: a route moves to another member only once quiet and on that member's new session.
    {
        Router hh;
        hh.set_keyed_mac(sim3::kmac());
        hh.accept_relayed(true);
        hh.set_member_key(mk, M, "Hunter1");
        hh.set_member_key(mk2, M2, "Hunter3");
        const std::uint32_t gx = ip(198, 51, 100, 9);
        CHECK(host_takes(hh, vframe(mk, gx, P, true), M, t0));
        CHECK(!host_takes(hh, vframe(mk2, gx, P, false), M2, at(1000)));   // a first frame of another member
        CHECK(!host_takes(hh, vframe(mk2, gx, P, true), M2, at(2000)));    // the route is not quiet yet
        CHECK(!host_takes(hh, vframe(mk2, gx, P, false), M2, at(12000)));  // quiet, but no new session
        CHECK(host_takes(hh, vframe(mk2, gx, P, true), M2, at(13000)));    // quiet and a new session: moved
        std::uint32_t ma = 0;
        CHECK(hh.relayed_guest(gx, P, &ma) && ma == M2);
        // At most kMaxRoutesPerMember guests per member.
        int took = 0;
        for (int i = 0; i < 6; ++i) took += host_takes(hh, vframe(mk, ip(198, 51, 100, 20 + i), P, true), M, at(14000));
        CHECK(took == static_cast<int>(Router::kMaxRoutesPerMember));
        // A member's earlier key goes with a new one (its routes too).
        hh.set_member_key(Router::new_member_key(), M, "Hunter1");
        CHECK(!host_takes(hh, vframe(mk, ip(198, 51, 100, 40), P, true), M, at(15000)));
        hh.drop_member("Hunter3");
        CHECK(!hh.relayed_guest(gx, P));
    }
    // A member cannot pose as a guest the host hears directly (a new route); a running route keeps
    // working when the guest is heard directly too.
    {
        Router hh;
        hh.set_keyed_mac(sim3::kmac());
        hh.accept_relayed(true);
        hh.set_member_key(mk, M, "Hunter1");
        Wire direct;
        direct.addr = G;
        direct.port = P;
        direct.data = {0xff, 0xc3, 40, 40};
        std::vector<std::uint8_t> d;
        std::uint32_t a2;
        std::uint16_t p2;
        std::vector<Wire> rr;
        CHECK(hh.incoming(direct, t0, &d, &a2, &p2, &rr));
        CHECK(!host_takes(hh, vframe(mk, G, P, true), M, at(2000)));
        CHECK(host_takes(hh, vframe(mk, G, P, true), M, at(12000)));  // quiet for 10 s: allowed
        CHECK(hh.incoming(direct, at(13000), &d, &a2, &p2, &rr));
        CHECK(host_takes(hh, vframe(mk, G, P, false), M, at(14000)));
    }
    // L3: a replayed Q3 (an older handshake) never replaces a live session; a new one only after quiet.
    {
        Router mm, gg;
        mm.set_keyed_mac(sim3::kmac());
        gg.set_keyed_mac(sim3::kmac());
        mm.set_keyed_mac(sim3::kmac());
        mm.serve(true, H, P, sim3::pake(good), "M", {}, mk);
        // Capture the guest's first Q3.
        gg.relay_use(M, P, H, P, sim3::pake(good), t0);
        std::vector<Wire> go, mo;
        gg.poll(t0, &go);
        sec_pump(go, G, P, mm, M, t0, &mo);
        sec_pump(mo, M, P, gg, G, t0, &go);
        CHECK(go.size() == 1 && go[0].data[2] == 3);
        const Wire q3old = go[0];
        sec_pump(go, G, P, mm, M, t0, &mo);
        sec_pump(mo, M, P, gg, G, t0, &go);
        CHECK(gg.relay_state() == Router::ClientState::Ready);
        // A second handshake 1 s later (live session): not replaced.
        Router g3;
        CHECK(sec_handshake(g3, G, mm, M, P, H, sim3::pake(good), at(1000)) != Router::ClientState::Ready);
        // A fresh handshake once quiet (a restarted guest): a new session.
        Router g4;
        CHECK(sec_handshake(g4, G, mm, M, P, H, sim3::pake(good), at(20000)) == Router::ClientState::Ready);
        // Replaying the first handshake's Q3 later (that session is gone): no new session, no welcome.
        std::vector<Wire> t{q3old};
        std::vector<Wire> rep;
        sec_pump(t, G, P, mm, M, at(26000), &rep);
        CHECK(rep.empty() && mm.served().size() == 1);
        // ... and g4's session still carries traffic.
        std::vector<Wire> o4, m4;
        g4.outgoing(H, P, dgram, 5, at(26000), &o4);
        sec_pump(o4, G, P, mm, M, at(26000), &m4);
        CHECK(m4.size() == 1 && m4[0].data[1] == 'V');
    }
    // Wrong keys: refused, counted per address, then a lockout (the F3 rule) that holds the right key too.
    {
        int refusals = 0, lockouts = 0;
        for (int i = 0; i < 7; ++i) {
            Router bad;
            int c = 0;
            if (sec_handshake(bad, X, m, M, P, H, sim3::pake(sim3::key_of_pw("guess")), t0, &c) ==
                Router::ClientState::Refused) {
                if (c == Router::kWrongKey) ++refusals;
                if (c == Router::kLockedOut) ++lockouts;
            }
        }
        // Each guess is one online try at the member, counted: 5 free, then the lockout.
        CHECK(refusals == 6 && lockouts == 1);
        clock_ms += 31000 * 4;  // let that lockout and its doubling pass
        throttle = AuthThrottle{};
        // A guesser that ignores the member's proof (a hand-made Q3) is still counted and locked out.
        int wrong = 0, locked = 0;
        std::uint8_t base[32] = {9}, sc[32], ya[32];
        for (int i = 0; i < 32; ++i) sc[i] = static_cast<std::uint8_t>(i * 7 + 1);
        crypto::x25519(sc, base, ya);
        for (int i = 0; i < 7; ++i) {
            std::array<std::uint8_t, 16> cn{};
            cn[0] = static_cast<std::uint8_t>(i + 1);
            std::vector<Wire> h1{Wire{}};
            h1[0].addr = M;
            h1[0].data = {0xfc, 'Q', 1};
            h1[0].data.insert(h1[0].data.end(), cn.begin(), cn.end());
            h1[0].data.insert(h1[0].data.end(), ya, ya + 32);
            h1[0].data.resize(Router::kQ1Size);
            std::vector<Wire> q2;
            sec_pump(h1, X, P, m, M, at((i + 1) * 3000), &q2);
            CHECK(q2.size() == 1);
            std::vector<Wire> q3{Wire{}};
            q3[0].addr = M;
            q3[0].data = {0xfc, 'Q', 3};
            q3[0].data.insert(q3[0].data.end(), cn.begin(), cn.end());
            q3[0].data.insert(q3[0].data.end(), ya, ya + 32);
            q3[0].data.insert(q3[0].data.end(), q2[0].data.begin() + 19, q2[0].data.begin() + 67);
            q3[0].data.resize(115, 0x5a);  // a guessed confirmation
            std::vector<Wire> ans;
            sec_pump(q3, X, P, m, M, at((i + 1) * 3000), &ans);
            if (ans.size() == 1 && ans[0].data[2] == 5) {
                if (ans[0].data[19] == Router::kWrongKey) ++wrong;
                if (ans[0].data[19] == Router::kLockedOut) ++locked;
            }
        }
        CHECK(wrong == 6 && locked == 1);
        Router right;
        int c = 0;
        CHECK(sec_handshake(right, X, m, M, P, H, sim3::pake(good), at(30000), &c) == Router::ClientState::Refused &&
              c == Router::kLockedOut);
        clock_ms += 31000;  // the lockout ends
        Router right2;
        CHECK(sec_handshake(right2, X, m, M, P, H, sim3::pake(good), at(40000)) == Router::ClientState::Ready);
    }
    // R1: a Q3 flood with forged cookies costs no curve operation; a Q1 flood from one /56 does not
    // keep a real guest out.
    {
        Router mm;
        mm.set_keyed_mac(sim3::kmac());
        mm.set_ipv6(true);
        mm.serve(true, H, P, sim3::pake(good), "M", {}, mk);
        const std::uint64_t ops0 = mm.curve_ops();
        for (int i = 0; i < 2000; ++i) {
            std::vector<Wire> q{Wire{}};
            q[0].addr = M;
            q[0].data = {0xfc, 'Q', 3};
            q[0].data.resize(115, static_cast<std::uint8_t>(i));
            std::vector<Wire> ans;
            sec_pump(q, X, P, mm, M, t0, &ans);
        }
        CHECK(mm.curve_ops() == ops0);
        // 2000 hellos from 2000 addresses of one /56, over IPv6: at most its budget is served.
        int answered = 0;
        for (int i = 0; i < 2000; ++i) {
            char t[64];
            std::snprintf(t, sizeof t, "2001:db8:44:4400::%x", i + 1);
            Wire in;
            in.v6 = true;
            in.addr6 = ip6(t);
            in.port = P;
            in.data = {0xfc, 'Q', 1};
            in.data.resize(Router::kQ1Size, 1);
            std::vector<std::uint8_t> d;
            std::uint32_t a2;
            std::uint16_t p2;
            std::vector<Wire> o;
            mm.incoming(in, t0, &d, &a2, &p2, &o);
            answered += static_cast<int>(o.size());
        }
        CHECK(answered <= static_cast<int>(2 * Router::kHandshakeRatePerGroup) && mm.curve_ops() - ops0 <= 2u * 10);
        Router gg;
        CHECK(sec_handshake(gg, G, mm, M, P, H, sim3::pake(good), t0) == Router::ClientState::Ready);
    }
    // R2: T and t frames are sealed: a forged or replayed one is dropped both ways.
    {
        Router mm, gg, hh;
        for (Router* r : {&mm, &gg, &hh}) r->set_keyed_mac(sim3::kmac());
        mm.serve(true, H, P, sim3::pake(good), "M", {}, mk);
        hh.accept_relayed(true);
        hh.set_member_key(mk, M, "M");
        CHECK(sec_handshake(gg, G, mm, M, P, H, sim3::pake(good), t0) == Router::ClientState::Ready);
        std::vector<Wire> go, mo, ho;
        gg.outgoing(H, P, dgram, 5, t0, &go);
        CHECK(go.size() == 1 && go[0].data.size() == 30 + 5);
        const Wire tf = go[0];
        Wire bad = tf;
        bad.data.back() ^= 1;  // the payload altered on the way
        std::vector<Wire> t{bad};
        sec_pump(t, G, P, mm, M, t0, &mo);
        CHECK(mo.empty());
        t = {tf};
        sec_pump(t, G, P, mm, M, t0, &mo);
        CHECK(mo.size() == 1 && mo[0].data[1] == 'V');
        sec_pump(mo, M, P, hh, H, t0, &ho);
        t = {tf};
        sec_pump(t, G, P, mm, M, t0, &mo);  // replayed
        CHECK(mo.empty());
        // t: the host's answer through the member, then forged and replayed copies.
        hh.outgoing(G, P, dgram, 5, t0, &ho);
        sec_pump(ho, H, P, mm, M, t0, &mo);
        CHECK(mo.size() == 1 && mo[0].data[1] == 't');
        const Wire lt = mo[0];
        mo.clear();
        Wire bt = lt;
        bt.data[12] ^= 1;  // the counter altered
        std::vector<std::vector<std::uint8_t>> got;
        std::vector<Wire> x{bt}, r;
        sec_pump(x, M, P, gg, G, t0, &r, &got);
        CHECK(got.empty());
        x = {lt};
        sec_pump(x, M, P, gg, G, t0, &r, &got);
        CHECK(got.size() == 1);
        x = {lt};
        sec_pump(x, M, P, gg, G, t0, &r, &got);  // replayed
        CHECK(got.size() == 1);
    }
    // R3 / L1: a refusal (Q5, unauthenticated) only ends the handshake and holds that member back for
    // at most 30 s, without growing; a failed member confirmation (Q4) grows the wait toward an hour.
    {
        Router imp, gg;
        imp.set_keyed_mac(sim3::kmac());
        imp.serve(true, H, P, sim3::pake(other), "Impostor", {}, Router::new_member_key());
        int c = 0;
        CHECK(sec_handshake(gg, G, imp, M, P, H, sim3::pake(good), t0, &c) == Router::ClientState::Refused &&
              c == Router::kWrongKey);
        CHECK(sec_handshake(gg, G, imp, M, P, H, sim3::pake(good), at(5000), &c) == Router::ClientState::Refused &&
              c == Router::kBackoff);
        CHECK(sec_handshake(gg, G, imp, M, P, H, sim3::pake(good), at(31000), &c) == Router::ClientState::Refused &&
              c == Router::kWrongKey);
        // refused again: still 30 s, not 60 (anyone can forge a Q5)
        CHECK(sec_handshake(gg, G, imp, M, P, H, sim3::pake(good), at(62000), &c) == Router::ClientState::Refused &&
              c == Router::kWrongKey);
        // A failed confirmation in Q4 (authenticated failure): 30 s, then 60 s.
        Router g3;
        g3.set_keyed_mac(sim3::kmac());
        auto fake_q4 = [&](rudp::Clock::time_point t) {
            g3.relay_use(M, P, H, P, sim3::pake(good), t);
            if (g3.relay_state() != Router::ClientState::Handshake) return;
            std::vector<Wire> go, io;
            g3.poll(t, &go);
            sec_pump(go, G, nbo(7000), imp, M, t, &io);
            sec_pump(io, M, P, g3, G, t, &go);  // Q2 -> our Q3 (its cn)
            if (go.size() != 1) return;
            std::vector<Wire> fake{Wire{}};
            fake[0].addr = G;
            fake[0].data = {0xfc, 'Q', 4};
            fake[0].data.insert(fake[0].data.end(), go[0].data.begin() + 3, go[0].data.begin() + 19);
            fake[0].data.insert(fake[0].data.end(), {0, 0, 0, 7, 3, 'I', 'm', 'p'});
            fake[0].data.resize(fake[0].data.size() + 16, 0x33);
            std::vector<Wire> none;
            sec_pump(fake, M, P, g3, G, t, &none);
        };
        fake_q4(at(100000));
        CHECK(g3.relay_state(nullptr, &c) == Router::ClientState::Refused && c == Router::kWrongKey);
        g3.relay_use(M, P, H, P, sim3::pake(good), at(120000));
        CHECK(g3.relay_state(nullptr, &c) == Router::ClientState::Refused && c == Router::kBackoff);
        fake_q4(at(131000));  // allowed again after 30 s; fails again
        g3.relay_use(M, P, H, P, sim3::pake(good), at(180000));
        CHECK(g3.relay_state(nullptr, &c) == Router::ClientState::Refused && c == Router::kBackoff);  // 60 s now
        g3.relay_use(M, P, H, P, sim3::pake(good), at(192000));
        CHECK(g3.relay_state() == Router::ClientState::Handshake);
    }
    // L2: a flood of hellos from hundreds of /56s spends the general budget; a source whose network
    // completed a handshake recently still gets an answer (the reserved part).
    {
        Router mm, gg;
        mm.set_keyed_mac(sim3::kmac());
        mm.serve(true, H, P, sim3::pake(good), "M", {}, mk);
        CHECK(sec_handshake(gg, G, mm, M, P, H, sim3::pake(good), at(500000)) == Router::ClientState::Ready);
        int answered = 0;
        for (int i = 0; i < 1000; ++i) {
            std::vector<Wire> q{Wire{}};
            q[0].addr = M;
            q[0].data = {0xfc, 'Q', 1};
            q[0].data.resize(Router::kQ1Size, 1);
            std::vector<Wire> ans;
            sec_pump(q, ip(10, i / 250, i % 250, 1), P, mm, M, at(501000), &ans);
            answered += static_cast<int>(ans.size());
        }
        CHECK(answered <= static_cast<int>(2 * (Router::kHandshakeRateGlobal - Router::kHandshakeRateReserved)));
        std::vector<Wire> q{Wire{}};
        q[0].addr = M;
        q[0].data = {0xfc, 'Q', 1};
        q[0].data.resize(Router::kQ1Size, 2);
        std::vector<Wire> ans;
        sec_pump(q, ip(203, 0, 113, 77), P, mm, M, at(501000), &ans);
        CHECK(ans.empty());  // a stranger: nothing left for it
        q = {Wire{}};
        q[0].addr = M;
        q[0].data = {0xfc, 'Q', 1};
        q[0].data.resize(Router::kQ1Size, 3);
        sec_pump(q, G, nbo(9999), mm, M, at(501000), &ans);  // the earlier guest's network
        CHECK(ans.size() == 1);
    }
    // L3: the relay key is the same for a member with the party code and one with a plain address.
    {
        const std::uint8_t secret[8] = {9, 8, 7, 6, 5, 4, 0, 0}, zero[8] = {};
        CHECK(crypto::relay_key_for("pw", secret) == crypto::relay_key_for("pw", zero));
        CHECK(crypto::relay_key_for("pw", secret) != crypto::relay_key_for("other", secret));
        CHECK(crypto::relay_key_for("", secret) != crypto::relay_key_for("", zero));  // no password: the secret
    }
    // At most kMaxSessions guests per member.
    {
        Router mm;
        mm.set_keyed_mac(sim3::kmac());
        mm.serve(true, H, P, sim3::pake(good), "M", {}, mk);
        int ready = 0, full = 0;
        for (int i = 0; i < 6; ++i) {
            Router gg;
            int c = 0;
            const auto st = sec_handshake(gg, ip(198, 51, 100, 100 + i), mm, M, P, H, sim3::pake(good), t0, &c);
            ready += st == Router::ClientState::Ready;
            full += st == Router::ClientState::Refused && c == Router::kFull;
        }
        CHECK(ready == static_cast<int>(Router::kMaxSessions) && full == 2);
    }
    // A member that is not serving any more forwards nothing.
    m.serve(false, 0, 0, {}, {}, {}, {});
    g.outgoing(H, P, dgram, 5, at(400), &gout);
    sec_pump(gout, G, P, m, M, at(400), &mout);
    CHECK(mout.empty());
}

// H2 / M1 / M3: IPv6 keys are not stealable, /64s count as one, and the host's nonce check.
static void test_ipv6_keys() {
    // H2: two endpoints whose hash collides: the newcomer gets another key, the first keeps its own.
    {
        Router r;
        r.set_ipv6(true);
        r.set_key_hash_for_tests([](const Ip6&) { return 5u; });
        std::uint32_t a1 = 0, a2 = 0;
        std::uint16_t p1 = 0, p2 = 0;
        const Ep6 e1{ip6("2001:db8:1::1"), nbo(9307)}, e2{ip6("2001:db8:2::2"), nbo(9307)};
        CHECK(r.key_of(e1, &a1, &p1) && r.key_of(e2, &a2, &p2));
        CHECK(a1 != a2 && p1 == p2);
        Ep6 back;
        CHECK(r.v6_of(a1, p1, &back) && back == e1);  // not evicted
        CHECK(r.v6_of(a2, p2, &back) && back == e2);
        // A bound key is never taken by a synthetic one, and a bound endpoint is not rebound.
        const std::uint32_t host = ip(203, 0, 113, 9);
        CHECK(r.bind_v6(host, nbo(9307), e1, true));
        CHECK(!r.bind_v6(ip(203, 0, 113, 10), nbo(9307), e1, true));
        CHECK(r.v6_of(host, nbo(9307), &back) && back == e1);
        std::uint32_t a3 = 0;
        std::uint16_t p3 = 0;
        CHECK(r.key_of(e1, &a3, &p3) && a3 == host);
    }
    // M1 / N4: lockouts count a whole /56 as one address, looked up by the full key (address and
    // port: two keys may share an address).
    {
        Router r;
        r.set_ipv6(true);
        r.set_key_hash_for_tests([](const Ip6&) { return 77u; });  // every key on one address, other ports
        std::uint32_t a1 = 0, a2 = 0, a3 = 0;
        std::uint16_t p1 = 0, p2 = 0, p3 = 0;
        CHECK(r.key_of(Ep6{ip6("2001:db8:aa:bb::1"), nbo(9307)}, &a1, &p1));
        CHECK(r.key_of(Ep6{ip6("2001:db8:aa:bc:ffff::7"), nbo(9307)}, &a2, &p2));  // same /56, another /64
        CHECK(r.key_of(Ep6{ip6("2001:db8:ab:cc::1"), nbo(9308)}, &a3, &p3));       // another /56
        CHECK(r.throttle_key(a1, p1) == r.throttle_key(a2, p2));
        CHECK(r.throttle_key(a1, p1) != r.throttle_key(a3, p3));
        CHECK(r.throttle_key(ip(198, 51, 100, 1), nbo(9307)) == ip(198, 51, 100, 1));
        CHECK(!bbnet::udp::is_synthetic(r.throttle_key(a1, p1)));  // a group key is never a peer's key
        // Rotating addresses in one /56: locked out by its group, the next address waits too.
        AuthThrottle at;
        for (int i = 0; i < 6; ++i) at.Fail(r.throttle_key(a1, p1), 0);
        CHECK(at.Locked(r.throttle_key(a2, p2), 1));
        CHECK(!at.Locked(r.throttle_key(a3, p3), 1));
    }
    // N2: a /56 floods the table; a party member's (trusted) key and a bound one are never evicted,
    // and a newcomer from elsewhere still gets a key.
    {
        Router r;
        r.set_ipv6(true);
        std::uint32_t ta = 0, ba = 0;
        std::uint16_t tp = 0, bp = 0;
        const Ep6 member{ip6("2001:db8:77:7701::5"), nbo(9307)};  // in the attacker's /48
        CHECK(r.key_of(member, &ta, &tp));
        r.mark_trusted(ta, tp);
        CHECK(r.bind_v6(ip(203, 0, 113, 50), nbo(9307), Ep6{ip6("2001:db8:77:7702::9"), nbo(9307)}, true));
        int got = 0;
        for (int i = 0; i < 2000; ++i) {  // 2000 sources over 250 /64s of 2001:db8:77:7700::/56
            char t[64];
            std::snprintf(t, sizeof t, "2001:db8:77:77%02x::%x", i % 250, i / 250 + 16);
            std::uint32_t a = 0;
            std::uint16_t p = 0;
            got += r.key_of(Ep6{ip6(t), nbo(9307)}, &a, &p);
        }
        CHECK(got == 2000);  // each stranger evicts an older stranger of its prefix
        std::uint32_t a = 0;
        std::uint16_t p = 0;
        CHECK(r.known_key(member, &a, &p) && a == ta && p == tp);           // the member kept its key
        CHECK(r.v6_of(ip(203, 0, 113, 50), nbo(9307), nullptr, nullptr));  // the bound one too
        int in48 = 0;
        for (int i = 0; i < 2000; ++i) {
            char t[64];
            std::snprintf(t, sizeof t, "2001:db8:77:77%02x::%x", i % 250, i / 250 + 16);
            in48 += r.known_key(Ep6{ip6(t), nbo(9307)}, &a, &p);
        }
        CHECK(in48 + 1 <= static_cast<int>(Router::kMaxV6Per48));  // the /48 holds at most its cap
        CHECK(r.key_of(Ep6{ip6("2001:db8:99::1"), nbo(9307)}, &a, &p));  // another network: served
        // The trusted member's traffic still gets in.
        Wire in;
        in.v6 = true;
        in.addr6 = member.addr;
        in.port = member.port;
        in.data = {1, 2, 3};
        std::vector<std::uint8_t> d;
        std::uint32_t sa;
        std::uint16_t sp;
        std::vector<Wire> o;
        CHECK(r.incoming(in, rudp::Clock::now(), &d, &sa, &sp, &o) && sa == ta);
    }
    // N3: a V frame naming a key of the host's own IPv6 table (a peer it reaches directly) is refused.
    {
        Router h;
        h.set_ipv6(true);
        h.set_keyed_mac(sim3::kmac());
        h.accept_relayed(true);
        const Router::MemberKey mk = Router::new_member_key();
        const std::uint32_t M = ip(192, 0, 2, 50);
        h.set_member_key(mk, M, "Hunter1");
        std::uint32_t ka = 0;
        std::uint16_t kp = 0;
        CHECK(h.key_of(Ep6{ip6("2001:db8:5::5"), nbo(9307)}, &ka, &kp));  // a direct IPv6 peer
        auto vf = [&](std::uint32_t ga, std::uint16_t gp, std::uint64_t ctr) {
            std::vector<std::uint8_t> f{0xfc, 'V', 1};
            for (int i = 3; i >= 0; --i) f.push_back(static_cast<std::uint8_t>(mk.id >> (8 * i)));
            for (int i = 7; i >= 0; --i) f.push_back(static_cast<std::uint8_t>(ctr >> (8 * i)));
            f.insert(f.end(), reinterpret_cast<const std::uint8_t*>(&ga), reinterpret_cast<const std::uint8_t*>(&ga) + 4);
            f.insert(f.end(), reinterpret_cast<const std::uint8_t*>(&gp), reinterpret_cast<const std::uint8_t*>(&gp) + 2);
            const std::size_t at = f.size();
            f.resize(f.size() + 16);
            f.push_back(0x42);
            std::vector<std::uint8_t> msg(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(at));
            msg.push_back(0x42);
            sim3::kmac()(mk.key.data(), msg.data(), msg.size(), f.data() + at);
            Wire w;
            w.addr = M;
            w.port = nbo(9307);
            w.data = f;
            return w;
        };
        std::vector<std::uint8_t> d;
        std::uint32_t sa;
        std::uint16_t sp;
        std::vector<Wire> o;
        CHECK(!h.incoming(vf(ka, kp, 1), rudp::Clock::now(), &d, &sa, &sp, &o));
        CHECK(h.incoming(vf(ip(198, 51, 100, 3), nbo(9307), 2), rudp::Clock::now(), &d, &sa, &sp, &o));
    }
    // N5: a key id another member registered cannot be taken over.
    {
        Router h;
        h.set_keyed_mac(sim3::kmac());
        h.accept_relayed(true);
        const Router::MemberKey mk = Router::new_member_key();
        h.set_member_key(mk, ip(192, 0, 2, 50), "Hunter1");
        Router::MemberKey stolen = mk;
        stolen.key[0] ^= 0xff;
        h.set_member_key(stolen, ip(192, 0, 2, 66), "Hunter3");
        bool refused = false;
        for (const std::string& n : h.take_notes()) refused |= n.find("refused") != std::string::npos;
        CHECK(refused);
        // Hunter1's frames still pass under its own key (and not Hunter3's).
        std::vector<std::uint8_t> f{0xfc, 'V', 1};
        for (int i = 3; i >= 0; --i) f.push_back(static_cast<std::uint8_t>(mk.id >> (8 * i)));
        for (int i = 7; i >= 0; --i) f.push_back(static_cast<std::uint8_t>(i == 0 ? 5 : 0));
        const std::uint32_t ga = ip(198, 51, 100, 4);
        const std::uint16_t gp = nbo(9307);
        f.insert(f.end(), reinterpret_cast<const std::uint8_t*>(&ga), reinterpret_cast<const std::uint8_t*>(&ga) + 4);
        f.insert(f.end(), reinterpret_cast<const std::uint8_t*>(&gp), reinterpret_cast<const std::uint8_t*>(&gp) + 2);
        const std::size_t at = f.size();
        f.resize(f.size() + 16);
        f.push_back(0x42);
        std::vector<std::uint8_t> msg(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(at));
        msg.push_back(0x42);
        sim3::kmac()(mk.key.data(), msg.data(), msg.size(), f.data() + at);
        Wire w;
        w.addr = ip(192, 0, 2, 50);
        w.port = nbo(9307);
        w.data = f;
        std::vector<std::uint8_t> d;
        std::uint32_t sa;
        std::uint16_t sp;
        std::vector<Wire> o;
        CHECK(h.incoming(w, rudp::Clock::now(), &d, &sa, &sp, &o) && sa == ga);
    }
    // N6 / R4: nonce frames need the party's nonce key: a stranger's flood (any number of networks)
    // takes no slot; the host's nonce arrives.
    {
        std::array<std::uint8_t, 32> party_nk{}, stranger_nk{};
        party_nk[0] = 1;
        stranger_nk[0] = 2;
        Router r, hostr, stranger;
        for (Router* x : {&r, &hostr, &stranger}) x->set_keyed_mac(sim3::kmac());
        r.set_nonce_key(party_nk);
        hostr.set_nonce_key(party_nk);
        stranger.set_nonce_key(stranger_nk);
        r.set_ipv6(true);
        std::array<std::uint8_t, 16> host_nonce{};
        host_nonce[0] = 0x99;
        std::vector<std::uint8_t> d;
        std::uint32_t sa;
        std::uint16_t sp;
        std::vector<Wire> o;
        for (int i = 0; i < 500; ++i) {
            std::array<std::uint8_t, 16> junk{};
            junk[1] = static_cast<std::uint8_t>(i);
            char t[64];
            std::snprintf(t, sizeof t, "2001:db8:%x:66::%x", 0x100 + i, i + 1);  // 500 networks
            Wire w = stranger.nonce_wire(Ep6{ip6("2001:db8::5"), nbo(9307)}, junk);
            w.addr6 = ip6(t);
            r.incoming(w, rudp::Clock::now(), &d, &sa, &sp, &o);
            if (i == 250) {
                Wire hw = hostr.nonce_wire(Ep6{ip6("2001:db8::5"), nbo(9307)}, host_nonce);
                hw.addr6 = ip6("2001:db8:1::1");  // the host
                r.incoming(hw, rudp::Clock::now(), &d, &sa, &sp, &o);
            }
        }
        const auto got = r.take_nonces();
        CHECK(got.size() == 1 && got[0] == host_nonce);
    }
    // M3: the host's nonce reaches the member over IPv6 only.
    {
        Router r;
        r.set_ipv6(true);
        r.set_keyed_mac(sim3::kmac());
        std::array<std::uint8_t, 32> nk{};
        nk[5] = 5;
        r.set_nonce_key(nk);
        std::array<std::uint8_t, 16> nonce{};
        nonce[3] = 7;
        Wire w = r.nonce_wire(Ep6{ip6("2001:db8::5"), nbo(9307)}, nonce);
        CHECK(w.v6 && w.data.size() == 34);
        Wire in = w;
        in.addr6 = ip6("2001:db8::1");  // from the host
        std::vector<std::uint8_t> d;
        std::uint32_t sa;
        std::uint16_t sp;
        std::vector<Wire> o;
        CHECK(!r.incoming(in, rudp::Clock::now(), &d, &sa, &sp, &o));
        auto got = r.take_nonces();
        CHECK(got.size() == 1 && got[0] == nonce);
        Wire v4;
        v4.addr = ip(203, 0, 113, 1);
        v4.port = nbo(9307);
        v4.data = w.data;
        CHECK(!r.incoming(v4, rudp::Clock::now(), &d, &sa, &sp, &o));
        CHECK(r.take_nonces().empty());  // over IPv4 it proves nothing
    }
}

// IPv6 when IPv4 cannot connect at all (the host behind a symmetric NAT / CGNAT): the guest's
// link to the host's IPv6 address, through stateful IPv6 firewalls. It opens when the host's
// firewall lets it in, or the host first sent to the guest's IPv6 endpoint (its BBG6 code).
static void test_ipv6_pairs() {
    using namespace nat;
    int agree = 0, total = 0;
    std::printf("  host firewall / guest firewall / host has the guest's BBG6 code -> link over IPv6\n");
    for (bool hopen : {true, false})
        for (bool gopen : {true, false})
            for (bool code6 : {true, false}) {
                sim3::Party H("host", Type::Symmetric, ip(203, 0, 113, 1), 31), G("guest", Type::Symmetric, ip(198, 51, 100, 2), 37);
                H.has6 = G.has6 = true;
                H.a6 = ip6("2001:db8:1::1");
                G.a6 = ip6("2001:db8:2::2");
                H.fw_open6 = hopen;
                G.fw_open6 = gopen;
                H.router.set_ipv6(true);
                G.router.set_ipv6(true);
                H.mux.accepting = true;
                sim3::Net net;
                net.parties = {&H, &G};
                const rudp::Endpoint hp = H.box.stun(9307, {ip(192, 0, 2, 3), nbo(3478)});
                // The guest: the host's code address is reached at its IPv6 address (the code's tail).
                G.router.bind_v6(hp.addr, hp.port, Ep6{H.a6, nbo(9307)}, true);
                G.stream = G.mux.connect(hp, net.t(), 6000);
                bool ok = false;
                for (; net.now < 8000 && !ok; ) {
                    if (code6 && net.now % 250 == 0) {  // the host punches the guest's BBG6 endpoint
                        Wire w;
                        w.v6 = true;
                        w.addr6 = G.a6;
                        w.port = nbo(9307);
                        w.data.assign(udp_probe(), udp_probe() + 8);
                        net.put(H, {w});
                    }
                    net.step();
                    ok = G.stream->state() == rudp::Stream::State::Open && H.stream;
                }
                const bool want = hopen || code6;
                CHECK(ok == want);
                agree += ok == want;
                ++total;
                std::printf("    %-6s %-6s %-3s -> %s\n", hopen ? "open" : "closed", gopen ? "open" : "closed",
                            code6 ? "yes" : "no", ok ? "IPv6" : "no");
                if (ok) {
                    // The host sees the guest at its synthetic key, the same one every PC derives.
                    std::uint32_t k = 0;
                    std::uint16_t p = 0;
                    CHECK(H.router.known_key(Ep6{G.a6, nbo(9307)}, &k, &p));
                    CHECK(bbnet::udp::is_synthetic(k) && H.accepted_from.addr == k && H.accepted_from.port == p);
                    // IPv4 between these two cannot work at all (both symmetric).
                    CHECK(!nat_attempt(Type::Symmetric, Type::Symmetric, true));
                }
            }
    std::printf("  IPv6 pairs agree with the firewall model in %d/%d\n", agree, total);
}

// Guest to guest: IPv4 probes unanswered, IPv6 answered -> over IPv6 before the host relay; back to
// IPv4 as soon as it answers.
static void test_peerpaths_v6() {
    using bbnet::udp::PeerPaths;
    PeerPaths paths(PeerPaths::Config{500, 2500, 2500, false});
    const auto t0 = std::chrono::steady_clock::now();
    auto at = [&](int ms) { return t0 + std::chrono::milliseconds(ms); };
    const std::uint32_t a = ip(198, 51, 100, 9);
    const std::uint16_t p = nbo(9307);
    paths.add(a, p, 50123, at(0));
    paths.set_v6(a, p, true, at(0));
    std::vector<std::pair<std::uint32_t, std::uint16_t>> probes, probes6;
    std::vector<std::string> notes;
    for (int ms = 0; ms <= 3000; ms += 100) {
        paths.tick(at(ms), &probes, &notes, &probes6);
        if (ms % 500 == 0) paths.on_ack6(a, p, at(ms));  // only IPv6 answers
    }
    CHECK(!probes6.empty());
    auto v = paths.peers();
    CHECK(v.size() == 1 && v[0].via6 && !v[0].relayed);  // IPv6, not the relay
    std::uint16_t rp = 0;
    CHECK(!paths.relayed(a, p, &rp));
    paths.on_ack(a, p, at(3100));  // IPv4 answers again
    paths.tick(at(3150), &probes, &notes, &probes6);
    v = paths.peers();
    CHECK(!v[0].via6 && !v[0].relayed);
    // Neither answers: the relay.
    for (int ms = 3200; ms <= 9000; ms += 100) paths.tick(at(ms), &probes, &notes, &probes6);
    v = paths.peers();
    CHECK(v[0].relayed && !v[0].via6);
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
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("outside address (CGNAT, double NAT)\n");
    test_outside();
    std::printf("classify\n");
    test_classify();
    std::printf("NAT matrix (model vs simulated NATs, real rudp streams)\n");
    test_matrix();
    std::printf("netsim NAT kinds\n");
    test_netsim_kinds();
    std::printf("bencode / KRPC\n");
    test_bencode();
    std::printf("DHT lookup (simulated)\n");
    test_lookup_sim();
    std::printf("rendezvous over loopback sockets\n");
    test_rendezvous_sockets();
    std::printf("automatic hole punch, no guest code\n");
    test_auto_punch_link();
    std::printf("route table basics (IPv6 keys, the party code's IPv6 tail, BBG6)\n");
    test_route_basics();
    std::printf("path chooser (IPv6, direct, member relays)\n");
    test_chooser();
    std::printf("member relay: host x member x guest NAT triples (model vs simulation, real rudp streams)\n");
    test_member_relay_matrix();
    std::printf("member relay: security\n");
    test_member_relay_security();
    std::printf("IPv6 keys: no stealing, /64 groups, the host's nonce check\n");
    test_ipv6_keys();
    std::printf("IPv6 pairs (no IPv4 path at all)\n");
    test_ipv6_pairs();
    std::printf("guest to guest: IPv6 before the host relay\n");
    test_peerpaths_v6();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
