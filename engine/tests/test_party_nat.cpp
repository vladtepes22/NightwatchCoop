// SPDX-License-Identifier: GPL-3.0-or-later
// NAT traversal (F2): the NAT model (party_nat: classification from two STUN answers, the path
// prediction, the advice text) checked against simulated NATs of every type with the real
// reliable-UDP streams; the network simulator's NAT kinds; the DHT rendezvous (party_rdv:
// bencode, KRPC, an iterative lookup through a simulated DHT, the socket driver against fake DHT
// nodes on loopback) and the whole automatic path: a guest behind the simulated NAT, no guest
// code, connects because the host finds its endpoint in the (fake) DHT and punches it.
// Build and run: ninja -C out/gpu party-nat-test && out/gpu/party-nat-test.exe
#include "net/netsim.h"
#include "party/party_crypto.h"
#include "party/party_link.h"
#include "party/party_nat.h"
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
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
