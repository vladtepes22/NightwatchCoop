// SPDX-License-Identifier: GPL-3.0-or-later
// Party transport soak test: a host and three guests in one process over loopback, under the
// network simulator's profiles (lan, dsl, bad) and burst outages, with a verdict line per
// scenario. No game: the party's own transport only.
//
//   control plane  the real PartyLink (TCP; handshake, RPC, reliable EVENT/ACK with replay,
//                  PING, roster) between the host and each guest, every guest's connection
//                  through its own TcpProxy that applies the profile (latency, jitter, loss as
//                  TCP retransmission delay, outages) in both directions;
//                  the host's PartyHostService event queues pumped into PartyLink as
//                  party_runtime.cpp does, delivered on the guests through RemoteGuest.
//   game plane     one UDP socket per player with the party port's framing and relay
//                  (party_udp.h: vport header, STUN HELLO, [fb]['R'|'r'] relay frames, the
//                  host's RelayServer), every datagram through a netsim::Queue (the simulator's
//                  delay line: latency, jitter, loss, dup, reorder, outages); each guest sends
//                  every other player a stream directly and, to the other guests, a second
//                  stream through the host relay (BB_PARTY_FORCE_RELAY's routing, route_to_peer).
//   main thread    a thread standing in for the game's main thread calls what the director
//                  calls every frame (state, roster, set_local_state, send_event, send_party_cmd)
//                  and records how long each call took: none may block.
//
// Scenarios: lan, dsl, bad, outage-5s, outage-30s, drop-rejoin, host-restart (each prints
// "VERDICT <name>: PASS|FAIL ..." and its measurements). Logs go to party_soak.log.
// Build and run: cmake --build out/gpu --target party-soak && out/gpu/party-soak.exe [scenario...]
#include "json.h"
#include "net_stun.h"
#include "netsim.h"
#include "party_host_service.h"
#include "party_transport.h"
#include "party_udp.h"
#include "party_util.h"

#include "party/party_link.h"
#include "party/party_sock.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#endif

// --- Runtime stand-ins (src/probe.c, runtime_thread.c, runtime.c) ---
extern "C" {
void restore_guest_fs(void) {}
void runtime_thread_attach_host(const char*) {}
const char* runtime_symbol(const char*) { return nullptr; }
}

using Clock = std::chrono::steady_clock;
using bbnet::netsim::Config;
using party::LinkState;
using party::PartyLink;
namespace udp = bbnet::udp;

namespace {

// ---------------------------------------------------------------- utilities

const Clock::time_point g_t0 = Clock::now();
double now_us() { return std::chrono::duration<double, std::micro>(Clock::now() - g_t0).count(); }
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

std::mutex g_log_mu;
FILE* g_log = nullptr;
void logf(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_log_mu);
    if (g_log) {
        std::fprintf(g_log, "%9.3f %s\n", now_us() / 1e6, buf);
        std::fflush(g_log);
    }
}
// Stdout and the log.
void say(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void say(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::printf("%s\n", buf);
    std::fflush(stdout);
    logf("%s", buf);
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

bool wait_until(const std::function<bool()>& pred, int timeout_ms, int step_ms = 10) {
    const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < end) {
        if (pred()) return true;
        sleep_ms(step_ms);
    }
    return pred();
}

struct Samples {
    std::mutex mu;
    std::vector<double> v;
    void add(double x) {
        std::lock_guard<std::mutex> lk(mu);
        v.push_back(x);
    }
    std::size_t count() {
        std::lock_guard<std::mutex> lk(mu);
        return v.size();
    }
    double pct(double p) {
        std::lock_guard<std::mutex> lk(mu);
        if (v.empty()) return 0;
        std::vector<double> c = v;
        std::sort(c.begin(), c.end());
        const std::size_t i = std::min(c.size() - 1, static_cast<std::size_t>(p / 100.0 * (c.size() - 1) + 0.5));
        return c[i];
    }
    double max() {
        std::lock_guard<std::mutex> lk(mu);
        return v.empty() ? 0 : *std::max_element(v.begin(), v.end());
    }
    bool p99_ok(double limit) { return pct(99) < limit; }
    void clear() {
        std::lock_guard<std::mutex> lk(mu);
        v.clear();
    }
};

std::uint32_t loopback() {
    in_addr a{};
    inet_pton(AF_INET, "127.0.0.1", &a);
    std::uint32_t v;
    std::memcpy(&v, &a, 4);
    return v;
}
std::uint16_t bswap16(std::uint16_t v) { return static_cast<std::uint16_t>((v << 8) | (v >> 8)); }

// ---------------------------------------------------------------- network conditions

// One player's network: its profile and its outages (both directions, TCP and UDP).
struct NetCond {
    std::mutex mu;
    Config cfg;
    Clock::time_point out_from{}, out_until{};
    std::mt19937_64 rng{1};
    void set(const Config& c) {
        std::lock_guard<std::mutex> lk(mu);
        cfg = c;
        rng.seed(c.seed);
    }
    void outage(Clock::time_point from, Clock::time_point until) {
        std::lock_guard<std::mutex> lk(mu);
        out_from = from;
        out_until = until;
    }
    bool in_outage(Clock::time_point t) {
        std::lock_guard<std::mutex> lk(mu);
        return t >= out_from && t < out_until;
    }
    // One TCP chunk's one-way delay: latency +- jitter; a lost segment costs a retransmission
    // (fast retransmit after duplicate ACKs: about one round trip more).
    Clock::duration tcp_delay() {
        std::lock_guard<std::mutex> lk(mu);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        double ms = cfg.lat_ms + (u(rng) * 2.0 - 1.0) * cfg.jitter_ms;
        if (ms < 0) ms = 0;
        if (cfg.loss_pct > 0 && u(rng) * 100.0 < cfg.loss_pct) ms += 2.0 * cfg.lat_ms + cfg.jitter_ms + 10.0;
        return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms));
    }
};

Config profile(const char* spec) {
    Config c;
    std::string err;
    if (!bbnet::netsim::parse(spec, &c, &err)) {
        std::fprintf(stderr, "bad profile %s: %s\n", spec, err.c_str());
        std::exit(2);
    }
    return c;
}

// ---------------------------------------------------------------- TCP proxy

// Forwards connections from its port to the host's PartyLink port through a delay line per
// direction (in order, as TCP delivers): latency/jitter/loss from the guest's NetCond, nothing
// at all during an outage (held, delivered after it - TCP retransmits). An upstream that
// refuses (host down) closes the guest's connection with a reset.
class TcpProxy {
public:
    TcpProxy(std::uint16_t upstream, NetCond* cond, std::string name)
        : upstream_(upstream), cond_(cond), name_(std::move(name)) {
        ls_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = loopback();
        a.sin_port = 0;
        ::bind(ls_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::listen(ls_, 16);
        party::sock::set_nonblocking(ls_);
        sockaddr_in b{};
        party::sock::socklen bl = sizeof b;
        getsockname(ls_, reinterpret_cast<sockaddr*>(&b), &bl);
        port_ = ntohs(b.sin_port);
        th_ = std::thread([this] { loop(); });
    }
    ~TcpProxy() {
        stop_ = true;
        if (th_.joinable()) th_.join();
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& p : pairs_) close_pair(*p, true);
        party::sock::close(ls_);
    }
    std::uint16_t port() const { return port_; }
    void set_upstream(std::uint16_t p) { upstream_ = p; }
    // Resets every connection now (a NAT dropping its state, a cable pulled and replugged).
    void reset_all() {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& p : pairs_) close_pair(*p, true);
    }
    std::uint64_t held_max() const { return held_max_; }

private:
    struct Chunk {
        Clock::time_point due;
        std::vector<std::uint8_t> data;
        bool eof = false;
    };
    struct Dir {
        std::deque<Chunk> q;
        std::size_t off = 0;
        std::size_t bytes = 0;
        Clock::time_point last_due{};
        bool eof_in = false, eof_out = false;
    };
    struct Pair {
        party::sock::Socket d = party::sock::kInvalid, u = party::sock::kInvalid;
        bool connecting = true;
        bool dead = false;
        Dir to_u, to_d;
    };

    static void abort_socket(party::sock::Socket& s) {
        if (s == party::sock::kInvalid) return;
        linger l{1, 0};  // RST
        setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof l);
        party::sock::close(s);
        s = party::sock::kInvalid;
    }
    void close_pair(Pair& p, bool reset) {
        if (reset) {
            abort_socket(p.d);
            abort_socket(p.u);
        } else {
            if (p.d != party::sock::kInvalid) party::sock::close(p.d);
            if (p.u != party::sock::kInvalid) party::sock::close(p.u);
            p.d = p.u = party::sock::kInvalid;
        }
        p.dead = true;
    }
    void push(Dir& dir, const std::uint8_t* d, std::size_t n, bool eof) {
        Chunk c;
        c.due = Clock::now() + cond_->tcp_delay();
        if (c.due < dir.last_due) c.due = dir.last_due;
        dir.last_due = c.due;
        c.data.assign(d, d + n);
        c.eof = eof;
        dir.bytes += n;
        dir.q.push_back(std::move(c));
        held_max_ = std::max<std::uint64_t>(held_max_, dir.bytes);
    }
    // Reads what `from` has into `dir` (a little back-pressure: at most 4 MB in flight).
    bool pump_in(party::sock::Socket from, Dir& dir) {
        std::uint8_t buf[16384];
        while (dir.bytes < (4u << 20) && !dir.eof_in) {
            long r = party::sock::recv_some(from, buf, sizeof buf);
            if (r > 0) {
                push(dir, buf, static_cast<std::size_t>(r), false);
                continue;
            }
            if (r == 0) {
                dir.eof_in = true;
                push(dir, nullptr, 0, true);
                break;
            }
            if (party::sock::would_block(party::sock::last_error())) break;
            return false;  // reset
        }
        return true;
    }
    bool pump_out(party::sock::Socket to, Dir& dir, Clock::time_point now) {
        while (!dir.q.empty() && dir.q.front().due <= now) {
            Chunk& c = dir.q.front();
            if (c.eof) {
                ::shutdown(to, 1 /* SD_SEND / SHUT_WR */);
                dir.eof_out = true;
                dir.q.pop_front();
                continue;
            }
            while (dir.off < c.data.size()) {
                long w = party::sock::send_some(to, c.data.data() + dir.off, c.data.size() - dir.off);
                if (w < 0) {
                    if (party::sock::would_block(party::sock::last_error())) return true;
                    return false;
                }
                dir.off += static_cast<std::size_t>(w);
            }
            dir.bytes -= c.data.size();
            dir.off = 0;
            dir.q.pop_front();
        }
        return true;
    }
    void loop() {
        std::vector<party::sock::PollFd> fds;
        std::vector<std::pair<Pair*, int>> who;
        while (!stop_) {
            fds.clear();
            who.clear();
            {
                std::lock_guard<std::mutex> lk(mu_);
                party::sock::PollFd l{};
                l.fd = ls_;
                l.events = POLLIN;
                fds.push_back(l);
                who.emplace_back(nullptr, 0);
                for (auto& p : pairs_) {
                    if (p->dead) continue;
                    party::sock::PollFd f{};
                    f.fd = p->d;
                    f.events = POLLIN;
                    fds.push_back(f);
                    who.emplace_back(p.get(), 0);
                    party::sock::PollFd g{};
                    g.fd = p->u;
                    g.events = static_cast<short>(p->connecting ? POLLOUT : POLLIN);
                    fds.push_back(g);
                    who.emplace_back(p.get(), 1);
                }
            }
            party::sock::poll(fds.data(), static_cast<unsigned long>(fds.size()), 2);
            std::lock_guard<std::mutex> lk(mu_);
            const auto now = Clock::now();
            const bool out = cond_->in_outage(now);
            for (std::size_t i = 0; i < fds.size(); ++i) {
                const short re = fds[i].revents;
                Pair* p = who[i].first;
                if (!p) {
                    if (re & POLLIN) accept_all();
                    continue;
                }
                if (p->dead || !re) continue;
                if (who[i].second == 1 && p->connecting) {
                    int err = 0;
                    party::sock::socklen el = sizeof err;
                    getsockopt(p->u, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el);
                    if (err || ((re & (POLLERR | POLLHUP)) && !(re & POLLOUT))) {
                        close_pair(*p, true);  // the host is down: the guest sees a reset
                    } else if (re & POLLOUT) {
                        p->connecting = false;
                    }
                    continue;
                }
                if (out) continue;  // nothing moves during an outage (TCP keeps it for later)
                if (re & (POLLIN | POLLHUP | POLLERR)) {
                    const bool ok = who[i].second == 0 ? pump_in(p->d, p->to_u) : pump_in(p->u, p->to_d);
                    if (!ok) close_pair(*p, true);
                }
            }
            if (!out) {
                for (auto& p : pairs_) {
                    if (p->dead) continue;
                    if (!p->connecting && !pump_out(p->u, p->to_u, now)) close_pair(*p, true);
                    if (!p->dead && !pump_out(p->d, p->to_d, now)) close_pair(*p, true);
                    if (!p->dead && p->to_u.eof_out && p->to_d.eof_out) close_pair(*p, false);
                }
            }
            pairs_.erase(std::remove_if(pairs_.begin(), pairs_.end(), [](const std::unique_ptr<Pair>& p) { return p->dead; }),
                         pairs_.end());
        }
    }
    void accept_all() {
        for (;;) {
            party::sock::Socket s = ::accept(ls_, nullptr, nullptr);
            if (s == party::sock::kInvalid) return;
            if (cond_->in_outage(Clock::now())) {
                // A connect during an outage fails (its SYNs are lost; PartyLink would give up
                // after connect_timeout): reset at once, the backoff schedule is what counts.
                abort_socket(s);
                continue;
            }
            party::sock::set_nonblocking(s);
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
            auto p = std::make_unique<Pair>();
            p->d = s;
            p->u = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            party::sock::set_nonblocking(p->u);
            setsockopt(p->u, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = loopback();
            a.sin_port = htons(upstream_.load());
            int rc = ::connect(p->u, reinterpret_cast<sockaddr*>(&a), sizeof a);
            if (rc != 0 && !party::sock::would_block(party::sock::last_error())) {
                close_pair(*p, true);
                continue;
            }
            p->connecting = rc != 0;
            pairs_.push_back(std::move(p));
        }
    }

    std::atomic<std::uint16_t> upstream_;
    NetCond* cond_;
    std::string name_;
    party::sock::Socket ls_ = party::sock::kInvalid;
    std::uint16_t port_ = 0;
    std::mutex mu_;
    std::vector<std::unique_ptr<Pair>> pairs_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> held_max_{0};
    std::thread th_;
};

// ---------------------------------------------------------------- UDP (game plane)

constexpr std::uint32_t kSoakMagic = 0x4b414f53;  // "SOAK"
constexpr std::uint16_t kVportGame = 40;
constexpr int kMaxPlayers = 4;
enum Flow { kDirect = 0, kRelay = 1 };

struct FlowRx {
    std::uint64_t received = 0, dups = 0, wrong_source = 0;
    std::uint32_t max_seq = 0;
    std::vector<bool> seen;
};

// One player's party UDP port: vport framing, the host's STUN responder and relay or a
// guest's relay client, every datagram out through a netsim::Queue.
class UdpNode {
public:
    UdpNode(int idx, bool host, NetCond* cond, std::uint16_t want_port = 0) : idx_(idx), host_(host), cond_(cond) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#if defined(_WIN32)
        BOOL excl = TRUE;
        setsockopt(fd_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
        BOOL no_reset = FALSE;
        DWORD ret = 0;
        WSAIoctl(fd_, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &no_reset, sizeof no_reset, nullptr, 0, &ret, nullptr, nullptr);
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = loopback();
        a.sin_port = htons(want_port);
        bind_ok_ = ::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
        sockaddr_in b{};
        party::sock::socklen bl = sizeof b;
        getsockname(fd_, reinterpret_cast<sockaddr*>(&b), &bl);
        port_ = ntohs(b.sin_port);
        server_.set_own_port(port_);
        {
            std::lock_guard<std::mutex> lk(cond_->mu);
            q_ = std::make_unique<bbnet::netsim::Queue>(cond_->cfg);
        }
        reader_ = std::thread([this] { read_loop(); });
        sender_ = std::thread([this] { send_loop(); });
    }
    ~UdpNode() {
        stop_ = true;
        waiter_.notify();
        if (reader_.joinable()) reader_.join();
        if (sender_.joinable()) sender_.join();
        party::sock::close(fd_);
    }
    bool bound() const { return bind_ok_; }
    std::uint16_t port() const { return port_; }
    udp::RelayClient& relay_client() { return rc_; }
    udp::RelayServer& relay_server() { return server_; }

    // Guest: a STUN Binding Request with the relay HELLO to the host; true once answered.
    bool stun(std::uint16_t host_port, int timeout_ms) {
        std::uint8_t req[net::stun::kMaxRequest];
        std::uint8_t token[net::stun::kTokenLen] = {};
        const bool have = rc_.token_for(loopback(), htons(host_port), token);
        std::size_t n;
        {
            std::lock_guard<std::mutex> lk(stun_mu_);
            n = net::stun::build_binding_request(req, txid_, true, have ? token : nullptr);
            stun_done_ = false;
            stun_pending_ = true;
            stun_host_port_ = host_port;
        }
        const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (Clock::now() < end) {
            send_raw(req, n, loopback(), htons(host_port));
            std::unique_lock<std::mutex> lk(stun_mu_);
            if (stun_cv_.wait_for(lk, std::chrono::milliseconds(500), [&] { return stun_done_; })) return true;
        }
        std::lock_guard<std::mutex> lk(stun_mu_);
        stun_pending_ = false;
        return false;
    }

    // A game datagram for player `to` at addr:port (host order port) on its vport.
    void send_game(int to, Flow flow, std::uint32_t addr, std::uint16_t port_host) {
        std::uint8_t pkt[udp::kRelayHeader + 6 + 64];
        std::uint8_t* body = pkt + udp::kRelayHeader;
        std::size_t hdr = udp::p2p_write_header(body, kVportGame, kVportGame);
        std::uint32_t seq;
        {
            std::lock_guard<std::mutex> lk(tx_mu_);
            seq = ++tx_seq_[to][flow];
            ++sent_[to][flow];
        }
        std::uint8_t* pl = body + hdr;
        const std::uint32_t magic = kSoakMagic;
        const double t = now_us();
        std::memcpy(pl, &magic, 4);
        pl[4] = static_cast<std::uint8_t>(idx_);
        pl[5] = static_cast<std::uint8_t>(flow);
        std::memcpy(pl + 6, &seq, 4);
        std::memcpy(pl + 10, &t, 8);
        std::memset(pl + 18, 0x5a, 64 - 18);  // ~ a game sync packet
        const std::size_t len = hdr + 64;
        std::uint32_t sa = 0;
        std::uint16_t sp = 0;
        // net_socket.cpp dgram_send: a guest whose direct path does not answer goes through its
        // relay port on the host.
        std::uint32_t via_addr = addr;
        std::uint16_t via_port = htons(port_host);
        std::uint16_t rp = 0;
        if (!host_ && paths_.relayed(addr, htons(port_host), &rp)) {
            std::uint32_t server = 0;
            if (rc_.get(&server, nullptr)) {
                via_addr = server;
                via_port = htons(rp);
            }
        }
        if (rc_.frame_for(via_addr, via_port, pkt, &sa, &sp)) {
            send_raw(pkt, udp::kRelayHeader + len, sa, sp);
            relay_bytes_out_ += udp::kRelayHeader + len;
        } else {
            send_raw(body, len, addr, htons(port_host));
            direct_bytes_out_ += len;
        }
    }

    // Received-side statistics.
    struct RxView {
        std::uint64_t received = 0, dups = 0, wrong_source = 0;
    };
    RxView rx(int from, Flow flow) {
        std::lock_guard<std::mutex> lk(rx_mu_);
        const FlowRx& f = rx_[from][flow];
        return {f.received, f.dups, f.wrong_source};
    }
    std::uint64_t sent(int to, Flow flow) {
        std::lock_guard<std::mutex> lk(tx_mu_);
        return sent_[to][flow];
    }
    Samples& lat(Flow flow) { return lat_[flow]; }
    // Who sends from where (the identity the receiving game would see): player `from`'s direct
    // port and relay port, for the wrong-source check.
    void set_identity(int from, std::uint16_t direct_port, std::uint16_t relay_port) {
        std::lock_guard<std::mutex> lk(rx_mu_);
        ident_[from] = {direct_port, relay_port};
    }
    // From now on, remember when the next datagram of each flow arrives (first_rx).
    void arm_first_rx() {
        std::lock_guard<std::mutex> lk(rx_mu_);
        for (auto& row : first_rx_)
            for (auto& t : row) t = Clock::time_point{};
        armed_ = true;
    }
    Clock::time_point first_rx(int from, Flow flow) {
        std::lock_guard<std::mutex> lk(rx_mu_);
        return first_rx_[from][flow];
    }
    std::uint64_t relay_bytes_out() const { return relay_bytes_out_; }
    std::uint64_t direct_bytes_out() const { return direct_bytes_out_; }
    bbnet::netsim::Stats sim_stats() {
        std::lock_guard<std::mutex> lk(qmu_);
        return q_->stats();
    }

private:
    void send_raw(const void* d, std::size_t n, std::uint32_t addr, std::uint16_t port_nbo) {
        {
            std::lock_guard<std::mutex> lk(qmu_);
            const auto now = Clock::now();
            if (cond_->in_outage(now)) return;  // our network is down
            if (blocked(ntohs(port_nbo))) return;  // no direct path to that player (NAT)
            q_->submit(now, 0, addr, port_nbo, d, n);
        }
        waiter_.notify();
    }
    void send_loop() {
        std::vector<bbnet::netsim::Packet> due;
        while (!stop_) {
            Clock::time_point when;
            due.clear();
            {
                std::lock_guard<std::mutex> lk(qmu_);
                const auto now = Clock::now();
                if (!q_->next_due(&when)) when = now + std::chrono::milliseconds(50);
                if (when <= now) q_->take_due(now, due);
            }
            if (due.empty()) {
                waiter_.wait_until(when);  // netsim's precise wait (1 ms deadlines on Windows too)
                continue;
            }
            for (const auto& p : due) {
                const double late = std::chrono::duration<double, std::milli>(Clock::now() - p.due).count();
                if (late > 20 && late_logged_ < 20) {
                    ++late_logged_;
                    logf("p%d sender %.1f ms late (queue %zu)", idx_, late, due.size());
                }
                sockaddr_in sa{};
                sa.sin_family = AF_INET;
                sa.sin_addr.s_addr = p.addr;
                sa.sin_port = p.port;
                ::sendto(fd_, reinterpret_cast<const char*>(p.data.data()), static_cast<int>(p.data.size()), 0,
                         reinterpret_cast<const sockaddr*>(&sa), sizeof sa);
            }
        }
    }
    void read_loop() {
        std::uint8_t buf[udp::kMaxDatagram + udp::kRelayHeader];
        auto last_tick = Clock::now();
        std::vector<std::pair<std::uint32_t, std::uint16_t>> probes;
        std::vector<std::string> notes;
        while (!stop_) {
            if (!host_ && Clock::now() - last_tick > std::chrono::milliseconds(100)) {
                // net_socket.cpp p2p_reader: probe the other guests, switch paths on the answers.
                last_tick = Clock::now();
                probes.clear();
                notes.clear();
                paths_.tick(last_tick, &probes, &notes);
                for (const auto& [a, pt] : probes) send_raw(udp::kProbe, sizeof udp::kProbe, a, pt);
                for (const auto& line : notes) logf("p%d %s", idx_, line.c_str());
            }
            party::sock::PollFd p{};
            p.fd = fd_;
            p.events = POLLIN;
            if (party::sock::poll(&p, 1, 20) <= 0) continue;
            sockaddr_in sa{};
            party::sock::socklen sl = sizeof sa;
            const int n = ::recvfrom(fd_, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&sa), &sl);
            if (n <= 0) continue;
            if (cond_->in_outage(Clock::now())) continue;  // our network is down
            if (blocked(ntohs(sa.sin_port))) continue;      // no direct path from that player
            std::size_t len = static_cast<std::size_t>(n);
            std::uint32_t from_addr = sa.sin_addr.s_addr;
            std::uint16_t from_port = sa.sin_port;
            if (host_) {
                if (udp::is_relay_request(buf, len)) {
                    std::uint8_t* out = nullptr;
                    std::size_t out_len = 0;
                    std::uint32_t to_addr = 0;
                    std::uint16_t to_port = 0;
                    std::string note;
                    if (server_.forward(buf, len, from_addr, from_port, &out, &out_len, &to_addr, &to_port, &note))
                        send_raw(out, out_len, to_addr, to_port);
                    if (!note.empty()) logf("host %s", note.c_str());
                    continue;
                }
                if (len >= 20 && buf[0] == 0x00 && buf[1] == 0x01) {
                    std::uint8_t out[net::stun::kMaxResponse];
                    std::string note;
                    const std::size_t rl = server_.answer_stun(buf, len, from_addr, from_port, true, out, &note);
                    if (rl) send_raw(out, rl, from_addr, from_port);
                    if (!note.empty()) logf("host %s", note.c_str());
                    continue;
                }
            } else {
                if (len >= 20 && buf[0] == 0x01 && buf[1] == 0x01) {
                    std::lock_guard<std::mutex> lk(stun_mu_);
                    std::uint32_t a = 0;
                    std::uint16_t pt = 0;
                    net::stun::Relay relay;
                    if (stun_pending_ && net::stun::parse_binding_response(buf, len, txid_, &a, &pt, &relay)) {
                        if (rc_.on_answer(from_addr, from_port, relay)) {
                            std::uint16_t vp = 0;
                            rc_.get(nullptr, &vp);
                            logf("p%d relay %s, port %u", idx_, rc_.on() ? "on" : "off", vp);
                        }
                        stun_pending_ = false;
                        stun_done_ = true;
                        stun_cv_.notify_all();
                    }
                    continue;
                }
                std::uint16_t relayed_from = 0;
                if (rc_.unwrap(buf, len, from_addr, from_port, &relayed_from)) {
                    std::memmove(buf, buf + udp::kDeliveryHeader, len - udp::kDeliveryHeader);
                    len -= udp::kDeliveryHeader;
                    from_port = relayed_from;
                    std::uint32_t da = 0;
                    std::uint16_t dp = 0;
                    if (paths_.direct_of_relay(ntohs(relayed_from), &da, &dp)) {
                        from_addr = da;  // the game knows the peer by its direct address
                        from_port = dp;
                    }
                }
            }
            if (udp::is_probe(buf, len)) {
                if (udp::is_probe_ack(buf, len)) paths_.on_ack(from_addr, from_port, Clock::now());
                else send_raw(udp::kProbeAck, sizeof udp::kProbeAck, from_addr, from_port);
                continue;
            }
            std::uint16_t src = 0, dst = 0;
            const std::size_t hdr = udp::p2p_header(buf, len, &src, &dst);
            if (!hdr || len < hdr + 18) continue;
            const std::uint8_t* pl = buf + hdr;
            std::uint32_t magic, seq;
            double t;
            std::memcpy(&magic, pl, 4);
            if (magic != kSoakMagic) continue;
            const int from = pl[4];
            const int flow = pl[5];
            std::memcpy(&seq, pl + 6, 4);
            std::memcpy(&t, pl + 10, 8);
            if (from < 0 || from >= kMaxPlayers || flow < 0 || flow > 1) continue;
            const double lat_ms = (now_us() - t) / 1000.0;
            std::lock_guard<std::mutex> lk(rx_mu_);
            FlowRx& f = rx_[from][flow];
            if (f.seen.size() <= seq) f.seen.resize(seq + 1024, false);
            if (f.seen[seq]) {
                ++f.dups;
                continue;
            }
            f.seen[seq] = true;
            ++f.received;
            if (armed_ && first_rx_[from][flow] == Clock::time_point{}) first_rx_[from][flow] = Clock::now();
            lat_[flow].add(lat_ms);
            if (lat_ms > 30 && slow_logged_ < 20) {
                ++slow_logged_;
                logf("p%d slow datagram from p%d flow %d seq %u: %.1f ms", idx_, from, flow, seq, lat_ms);
            }
            // The source the game would see: the sender's direct port, or its relay port.
            const auto id = ident_.find(from);
            if (id != ident_.end()) {
                // Relay deliveries from a known peer are presented from its direct address.
                const std::uint16_t want = id->second.first;
                if (want && bswap16(from_port) != want) ++f.wrong_source;
            }
        }
    }

    int idx_;
    bool host_;
    NetCond* cond_;
    party::sock::Socket fd_ = party::sock::kInvalid;
    bool bind_ok_ = false;
    std::uint16_t port_ = 0;
    udp::RelayServer server_;
    udp::RelayClient rc_;
    udp::PeerPaths paths_;
    mutable std::mutex block_mu_;
    std::set<std::uint16_t> blocked_;
    bool blocked(std::uint16_t port_host) const {
        std::lock_guard<std::mutex> lk(block_mu_);
        return blocked_.count(port_host) != 0;
    }

public:
    // net_socket.cpp p2p_add_peer: another guest's direct address and relay port.
    void add_peer(std::uint32_t addr, std::uint16_t port_host, std::uint16_t relay_port) {
        if (host_ || rc_.is_server(addr, htons(port_host))) return;
        std::uint16_t own = 0;
        rc_.get(nullptr, &own);
        if (relay_port && relay_port == own) return;
        if (relay_port) rc_.add_relay_port(relay_port);
        paths_.add(addr, htons(port_host), relay_port, Clock::now());
    }
    // Simulates a NAT pair that cannot reach each other: nothing to or from that UDP port.
    void block(std::uint16_t port_host, bool on) {
        std::lock_guard<std::mutex> lk(block_mu_);
        if (on) blocked_.insert(port_host);
        else blocked_.erase(port_host);
    }
    std::vector<udp::PeerPaths::PeerView> peer_paths() const { return paths_.peers(); }
    // This node's path to UDP port `port_host` (on loopback): relayed?
    bool relayed_to(std::uint16_t port_host) const { return paths_.relayed(loopback(), htons(port_host), nullptr); }
    int switches_to(std::uint16_t port_host) const {
        for (const auto& v : paths_.peers())
            if (v.port == htons(port_host)) return v.switches;
        return 0;
    }

private:
    std::mutex qmu_;
    bbnet::netsim::Waiter waiter_;
    std::unique_ptr<bbnet::netsim::Queue> q_;
    std::mutex stun_mu_;
    std::condition_variable stun_cv_;
    std::uint8_t txid_[net::stun::kTxid] = {};
    bool stun_pending_ = false, stun_done_ = false;
    std::uint16_t stun_host_port_ = 0;
    std::mutex tx_mu_;
    std::uint32_t tx_seq_[kMaxPlayers][2] = {};
    std::uint64_t sent_[kMaxPlayers][2] = {};
    std::mutex rx_mu_;
    FlowRx rx_[kMaxPlayers][2];
    Clock::time_point first_rx_[kMaxPlayers][2] = {};
    bool armed_ = false;
    std::map<int, std::pair<std::uint16_t, std::uint16_t>> ident_;
    Samples lat_[2];
    int slow_logged_ = 0, late_logged_ = 0;
    std::atomic<std::uint64_t> relay_bytes_out_{0}, direct_bytes_out_{0};
    std::atomic<bool> stop_{false};
    std::thread reader_, sender_;
};

// ---------------------------------------------------------------- the party

constexpr const char* kNames[kMaxPlayers] = {"Host", "Alice", "Bob", "Carol"};

party::LinkConfig link_config(const char* name) {
    party::LinkConfig c;
    c.name = name;
    c.secret = {1, 2, 3, 4, 5, 6, 7, 8};
    c.password = "soak";
    c.max_players = 4;
    c.port = 0;
    c.bind_addr = "127.0.0.1";
    return c;  // the real timings: ping 1 s, lost 10 s, slot 60 s, backoff 1..30 s
}

// Reliable host->guest events, checked on arrival: per host incarnation (Epoch) the sequence
// must be contiguous (a resumed session gets its missed events replayed, exactly once).
struct EvCheck {
    std::mutex mu;
    int epoch = -1;
    std::uint64_t last = 0;
    bool any = false;
    bool expect_from_one = false;  // a later host incarnation: from its first event on
    std::uint64_t first_seq = 0;   // this guest instance's first event (a restarted guest joins mid-stream)
    std::uint64_t delivered = 0, dups = 0, gaps = 0, missing = 0, bad_start = 0;
    void on(int ep, std::uint64_t seq) {
        std::lock_guard<std::mutex> lk(mu);
        if (ep != epoch) {
            expect_from_one = epoch != -1;
            epoch = ep;
            last = 0;
            any = false;
        }
        if (any && seq <= last) {
            ++dups;
            return;
        }
        if (!any) {
            if (expect_from_one && seq != 1) {
                ++bad_start;
                missing += seq - 1;
            }
        } else if (seq != last + 1) {
            ++gaps;
            missing += seq - last - 1;
        }
        if (!first_seq) first_seq = seq;
        any = true;
        last = seq;
        ++delivered;
    }
};

struct GuestRun;  // one guest process incarnation

struct Totals {
    Samples ev_lat, rpc_rtt, main_call;
    std::atomic<std::uint64_t> rpc_ok{0}, rpc_fail{0}, rpc_bad{0};
    std::atomic<std::uint64_t> g2h_delivered{0}, g2h_dups{0}, g2h_gaps{0};
    std::atomic<int> state_changes{0};
    std::atomic<std::int64_t> hold_max_us{0};      // the links' IO threads: longest hold of their lock
    std::atomic<std::uint64_t> hold_max_cycles{0};  // ... in their own CPU cycles
    void clear() {
        hold_max_us = 0;
        hold_max_cycles = 0;
        ev_lat.clear();
        rpc_rtt.clear();
        main_call.clear();
        rpc_ok = rpc_fail = rpc_bad = 0;
        g2h_delivered = g2h_dups = g2h_gaps = 0;
        state_changes = 0;
    }
};
Totals g_tot;
double g_cycles_per_ms = 1e6;  // calibrated in main()

struct GuestRun {
    int idx = 0;
    int inst = 0;  // incarnation (a crashed guest restarts as a new one)
    std::unique_ptr<PartyLink> link;
    bbnet::party::RemoteGuest rg;
    EvCheck ev;
    std::mutex mu;
    std::vector<std::pair<Clock::time_point, LinkState>> states;
    std::atomic<std::uint64_t> g2h_seq{0};
    std::atomic<int> rejected{0};
    std::atomic<std::uint64_t> rpc_seq{0};

    Clock::time_point connected_after(Clock::time_point t) {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& [at, s] : states)
            if (at >= t && s == LinkState::Connected) return at;
        return {};
    }
    int count_state(LinkState st, Clock::time_point since) {
        std::lock_guard<std::mutex> lk(mu);
        int n = 0;
        for (const auto& [at, s] : states)
            if (at >= since && s == st) ++n;
        return n;
    }
};

struct HostRun {
    int epoch = 0;
    std::unique_ptr<PartyLink> link;
    std::unique_ptr<bbnet::party::PartyHostService> svc;
    struct Pump {
        std::string name;
        int slot = 0;
        std::atomic<bool> stop{false};
        std::thread th;
    };
    std::mutex pmu;
    std::map<std::string, std::unique_ptr<Pump>> pumps;
    std::mutex g2h_mu;
    std::map<std::string, std::pair<int, std::uint64_t>> g2h_last;  // name -> (guest inst, last seq)
    std::atomic<std::uint64_t> pushed[kMaxPlayers] = {};

    // party_runtime.cpp's pump: the service's queue for one member into PartyLink.
    void pump_start(const party::RosterEntry& m) {
        std::lock_guard<std::mutex> lk(pmu);
        auto it = pumps.find(m.name);
        if (it != pumps.end()) {
            if (it->second->slot == m.slot && !it->second->stop) return;  // resumed: keep its cursor
            it->second->stop = true;
            svc->wake_all();
            it->second->th.join();
            pumps.erase(it);
        }
        auto p = std::make_unique<Pump>();
        p->name = m.name;
        p->slot = m.slot;
        Pump* raw = p.get();
        raw->th = std::thread([this, raw] {
            std::uint64_t cursor = 0;
            while (!raw->stop) {
                std::vector<json::Value> evs = svc->wait_events(raw->name, cursor, 200);
                if (raw->stop) break;
                std::uint64_t last = cursor;
                for (const json::Value& ev : evs) {
                    const auto id = static_cast<std::uint64_t>(bbnet::party::int_of(ev, "EventId", 0));
                    if (id && id <= cursor) continue;
                    link->send_event(raw->slot, bbnet::party::str_of(ev, "Name"), json::dump(ev, 0));
                    if (id > last) last = id;
                }
                if (last > cursor) {
                    cursor = last;
                    svc->ack_events(raw->name, cursor);
                }
            }
        });
        pumps[m.name] = std::move(p);
    }
    void pump_stop(const std::string& name) {
        std::unique_ptr<Pump> p;
        {
            std::lock_guard<std::mutex> lk(pmu);
            auto it = pumps.find(name);
            if (it == pumps.end()) return;
            p = std::move(it->second);
            pumps.erase(it);
        }
        p->stop = true;
        svc->wake_all();
        p->th.join();
    }
    void stop_pumps() {
        std::vector<std::string> names;
        {
            std::lock_guard<std::mutex> lk(pmu);
            for (auto& [n, p] : pumps) names.push_back(n);
        }
        for (auto& n : names) pump_stop(n);
    }
};

struct Party {
    std::string scenario;
    NetCond cond[kMaxPlayers];  // [0] = the host's own network
    std::unique_ptr<HostRun> host;
    std::uint16_t host_port = 0;   // PartyLink TCP
    std::uint16_t host_udp = 0;    // the party UDP port
    std::unique_ptr<UdpNode> udp[kMaxPlayers];
    std::unique_ptr<TcpProxy> proxy[kMaxPlayers];
    std::unique_ptr<GuestRun> guest[kMaxPlayers];
    std::mutex gmu;  // guest[] swaps
    std::atomic<bool> traffic{false}, gen{true}, stop{false};
    std::thread traffic_th, events_th, main_th, stun_th;
    int next_epoch = 1;
    int next_inst = 1;

    // ---- host ----
    bool start_host(std::uint16_t want_tcp) {
        auto h = std::make_unique<HostRun>();
        h->epoch = next_epoch++;
        h->svc = std::make_unique<bbnet::party::PartyHostService>();
        HostRun* hr = h.get();
        party::LinkCallbacks cb;
        cb.on_log = [](const std::string& l) { logf("host %s", l.c_str()); };
        cb.on_member_joined = [hr](const party::RosterEntry& m, bool rejoined) {
            logf("host: %s %s slot %d", m.name.c_str(), rejoined ? "rejoined" : "joined", m.slot);
            hr->pump_start(m);
        };
        cb.on_member_left = [hr](const party::RosterEntry& m, bool kept) {
            logf("host: %s left (slot %s)", m.name.c_str(), kept ? "kept" : "released");
            if (!kept) hr->pump_stop(m.name);
        };
        cb.on_rpc = [](int, const std::string& kind, const std::string& body) -> std::string {
            if (kind != "soak_echo") return "{\"ResKind\":1}";
            return body;
        };
        cb.on_event = [hr](int slot, std::uint64_t, const std::string& name, const std::string& body) {
            if (name != "g_soak") return;
            json::Value v;
            std::string err;
            if (!json::parse(body, v, err)) return;
            const std::string who = bbnet::party::str_of(v, "Name");
            const int inst = static_cast<int>(bbnet::party::int_of(v, "Inst", 0));
            const auto seq = static_cast<std::uint64_t>(bbnet::party::int_of(v, "Seq", 0));
            std::lock_guard<std::mutex> lk(hr->g2h_mu);
            auto& [li, ls] = hr->g2h_last[who];
            (void)slot;
            if (li != inst) {
                li = inst;
                ls = 0;
            }
            if (ls && seq <= ls) {
                ++g_tot.g2h_dups;
                return;
            }
            if (ls && seq != ls + 1) ++g_tot.g2h_gaps;
            ls = seq;
            ++g_tot.g2h_delivered;
        };
        auto cfg = link_config(kNames[0]);
        cfg.port = want_tcp;
        h->link = std::make_unique<PartyLink>(cfg, cb);
        std::string err;
        const auto t0 = Clock::now();
        bool ok = false;
        // A restarted host binds its old port: retry while the OS still holds it.
        while (!(ok = h->link->start_host(&err)) && ms_since(t0) < 15000) {
            h->link = std::make_unique<PartyLink>(cfg, cb);
            sleep_ms(250);
        }
        if (!ok) {
            say("  host: cannot start: %s", err.c_str());
            return false;
        }
        if (ms_since(t0) > 300) say("  host: port %u bound after %.0f ms (%s)", want_tcp, ms_since(t0), err.c_str());
        host_port = h->link->bound_port();
        for (int i = 1; i < kMaxPlayers; ++i)
            if (proxy[i]) proxy[i]->set_upstream(host_port);
        std::lock_guard<std::mutex> lk(gmu);
        host = std::move(h);
        return true;
    }
    // Host process gone: no BYE, sockets dropped (and its UDP port with its relay state).
    void crash_host() {
        std::unique_ptr<HostRun> h;
        {
            std::lock_guard<std::mutex> lk(gmu);
            h = std::move(host);
        }
        h->link->stop(false);
        h->stop_pumps();
        udp[0].reset();
    }

    // ---- guests ----
    bool start_guest(int i) {
        auto g = std::make_unique<GuestRun>();
        g->idx = i;
        g->inst = next_inst++;
        GuestRun* gr = g.get();
        Party* self = this;
        gr->rg.start_events(kNames[i], [gr](const json::Value& ev) {
            if (bbnet::party::str_of(ev, "Name") != "soak") return;
            const double t = static_cast<double>(bbnet::party::int_of(ev, "T", 0));
            g_tot.ev_lat.add((now_us() - t) / 1000.0);
            gr->ev.on(static_cast<int>(bbnet::party::int_of(ev, "Epoch", 0)),
                      static_cast<std::uint64_t>(bbnet::party::int_of(ev, "Seq", 0)));
        });
        party::LinkCallbacks cb;
        cb.on_log = [i](const std::string& l) { logf("%s %s", kNames[i], l.c_str()); };
        cb.on_event = [gr](int, std::uint64_t, const std::string&, const std::string& body) { gr->rg.on_link_event(body); };
        cb.on_state = [gr, self, i](LinkState s, const std::string& d) {
            {
                std::lock_guard<std::mutex> lk(gr->mu);
                gr->states.emplace_back(Clock::now(), s);
            }
            ++g_tot.state_changes;
            if (s == LinkState::Rejected) ++gr->rejected;
            if (s == LinkState::Connected && gr->link && !gr->link->last_welcome_resumed()) {
                // party_runtime.cpp: a fresh host session - event ids start over, the relay
                // registration is the old host's (np_session host_session_reset re-STUNs).
                gr->rg.reset_event_cursor();
                self->restun(i);
            }
            logf("%s state %s %s", kNames[i], party::link_state_name(s), d.c_str());
        };
        g->link = std::make_unique<PartyLink>(link_config(kNames[i]), cb);
        std::string err;
        if (!g->link->start_guest("127.0.0.1", proxy[i]->port(), &err)) {
            say("  %s: cannot start: %s", kNames[i], err.c_str());
            return false;
        }
        std::lock_guard<std::mutex> lk(gmu);
        guest[i] = std::move(g);
        return true;
    }
    // The guest's process dies (no BYE).
    void crash_guest(int i) {
        std::unique_ptr<GuestRun> g;
        {
            std::lock_guard<std::mutex> lk(gmu);
            g = std::move(guest[i]);
        }
        if (g) g->link->stop(false);
    }
    GuestRun* G(int i) { return guest[i].get(); }

    std::atomic<bool> restun_due[kMaxPlayers] = {};
    void restun(int i) { restun_due[i] = true; }

    void set_profile(const Config& c) {
        for (auto& k : cond) k.set(c);
    }
    void outage_all(int ms) {
        const auto from = Clock::now();
        for (int i = 1; i < kMaxPlayers; ++i) cond[i].outage(from, from + std::chrono::milliseconds(ms));
    }

    // ---- threads ----
    void start_threads() {
        traffic = true;
        // The game plane: 30 Hz per destination and flow.
        traffic_th = std::thread([this] {
            while (!stop) {
                const auto t = Clock::now();
                if (traffic) {
                    for (int s = 0; s < kMaxPlayers; ++s) {
                        UdpNode* src = udp[s].get();
                        if (!src) continue;
                        for (int d = 0; d < kMaxPlayers; ++d) {
                            if (d == s || !udp[d]) continue;
                            const std::uint16_t dport = d == 0 ? host_udp : udp[d]->port();
                            src->send_game(d, kDirect, loopback(), dport);
                            // Guest to guest through the host relay (BB_PARTY_FORCE_RELAY routing).
                            if (s != 0 && d != 0) {
                                std::uint16_t peer_relay = 0;
                                udp[d]->relay_client().get(nullptr, &peer_relay);
                                const udp::PeerRoute r =
                                    udp::route_to_peer(src->relay_client(), loopback(), dport, peer_relay, true);
                                if (r.relayed) src->send_game(d, kRelay, r.addr, r.port);
                            }
                        }
                    }
                }
                std::this_thread::sleep_until(t + std::chrono::microseconds(33333));
            }
        });
        // Host-side events (20/s per member) and guest RPCs (5/s) and guest->host events (5/s).
        events_th = std::thread([this] {
            int tick = 0;
            while (!stop) {
                const auto t = Clock::now();
                {
                    std::lock_guard<std::mutex> lk(gmu);
                    if (host && gen) {
                        for (int i = 1; i < kMaxPlayers; ++i) {
                            json::Value ev = json::Value::make_object();
                            ev.set("Name", "soak");
                            ev.set("Epoch", host->epoch);
                            ev.set("Seq", static_cast<double>(++host->pushed[i]));
                            ev.set("T", now_us());
                            host->svc->push_event(kNames[i], std::move(ev));
                        }
                    }
                    if (tick % 4 == 0) {
                        for (int i = 1; i < kMaxPlayers; ++i) {
                            GuestRun* g = guest[i].get();
                            if (!g) continue;
                            if (g->link->state() == LinkState::Connected) {
                                json::Value rq = json::Value::make_object();
                                rq.set("Seq", static_cast<double>(++g->rpc_seq));
                                rq.set("T", now_us());
                                const double sent = now_us();
                                const std::string body = json::dump(rq, 0);
                                g->link->rpc_call_async(
                                    "soak_echo", body,
                                    [sent, body](bool ok, const std::string& reply) {
                                        if (!ok) {
                                            ++g_tot.rpc_fail;
                                            return;
                                        }
                                        if (reply != body) ++g_tot.rpc_bad;
                                        ++g_tot.rpc_ok;
                                        g_tot.rpc_rtt.add((now_us() - sent) / 1000.0);
                                    },
                                    4000);
                            }
                            json::Value e = json::Value::make_object();
                            e.set("Name", kNames[i]);
                            e.set("Inst", g->inst);
                            e.set("Seq", static_cast<double>(++g->g2h_seq));
                            g->link->send_event(0, "g_soak", json::dump(e, 0));
                        }
                    }
                }
                ++tick;
                std::this_thread::sleep_until(t + std::chrono::milliseconds(50));
            }
        });
        // The game's main thread: what PartyDirector calls every frame, timed.
        main_th = std::thread([this] {
            int frame = 0;
            while (!stop) {
                const auto t = Clock::now();
                std::vector<PartyLink*> links;
                {
                    std::lock_guard<std::mutex> lk(gmu);
                    if (host) links.push_back(host->link.get());
                    for (int i = 1; i < kMaxPlayers; ++i)
                        if (guest[i]) links.push_back(guest[i]->link.get());
                    for (PartyLink* l : links) {
                        const auto c0 = Clock::now();
                        double part[5] = {};
                        auto lap = [&](int k) { part[k] = ms_since(c0); };
                        (void)l->state();
                        lap(0);
                        (void)l->roster();
                        lap(1);
                        l->set_local_state(frame % 120 < 60 ? party::MemberState::InHostWorld : party::MemberState::Loading,
                                           0x0a000000u + static_cast<std::uint32_t>(frame / 120));
                        lap(2);
                        if (l->is_host() && frame % 60 == 0) l->send_event(party::kBroadcast, "travel", "{}");
                        lap(3);
                        if (frame % 30 == 0) l->send_party_cmd(party::kBroadcast, "ping", "{}");
                        lap(4);
                        const double total = ms_since(c0);
                        g_tot.main_call.add(total);
                        std::uint64_t cyc = 0;
                        const std::int64_t held = l->debug_lock_hold_max_us(true, &cyc);
                        if (held > 3000)
                            logf("%s link IO thread held its lock %.2f ms (%.2f ms of its own CPU)",
                                 l->is_host() ? "host" : "guest", held / 1000.0, cyc / g_cycles_per_ms);
                        std::int64_t prev = g_tot.hold_max_us.load();
                        while (held > prev && !g_tot.hold_max_us.compare_exchange_weak(prev, held)) {
                        }
                        std::uint64_t pc = g_tot.hold_max_cycles.load();
                        while (cyc > pc && !g_tot.hold_max_cycles.compare_exchange_weak(pc, cyc)) {
                        }
                        if (total > 10)
                            logf("main thread: %s link calls took %.1f ms (state %.1f roster %.1f set_local %.1f event %.1f cmd %.1f)",
                                 l->is_host() ? "host" : "guest", total, part[0], part[1], part[2], part[3], part[4]);
                    }
                }
                ++frame;
                std::this_thread::sleep_until(t + std::chrono::milliseconds(16));
            }
        });
        // np_session's keepalive: a STUN HELLO to the host every 15 s (and at once after a
        // fresh host session); the relay ports are what peers address each other by.
        stun_th = std::thread([this] {
            Clock::time_point last[kMaxPlayers] = {};
            while (!stop) {
                for (int i = 1; i < kMaxPlayers; ++i) {
                    if (!udp[i]) continue;
                    if (restun_due[i].exchange(false) || ms_since(last[i]) > 15000) {
                        last[i] = Clock::now();
                        udp[i]->stun(host_udp, 1500);
                    }
                }
                refresh_identities();
                sleep_ms(50);
            }
        });
    }
    void refresh_identities() {
        for (int d = 0; d < kMaxPlayers; ++d) {
            if (!udp[d]) continue;
            for (int s = 0; s < kMaxPlayers; ++s) {
                if (s == d || !udp[s]) continue;
                std::uint16_t rp = 0;
                udp[s]->relay_client().get(nullptr, &rp);
                udp[d]->set_identity(s, s == 0 ? host_udp : udp[s]->port(), rp);
                if (d != 0 && s != 0 && rp) udp[d]->add_peer(loopback(), udp[s]->port(), rp);
            }
        }
    }
    void stop_threads() {
        stop = true;
        for (std::thread* t : {&traffic_th, &events_th, &main_th, &stun_th})
            if (t->joinable()) t->join();
    }

    std::vector<std::pair<int, int>> blocked_pairs;  // guest pairs without a direct path from the start
    void block_pair(int a, int b, bool on) {
        udp[a]->block(udp[b]->port(), on);
        udp[b]->block(udp[a]->port(), on);
    }

    // ---- setup / teardown ----
    bool up(const Config& c) {
        set_profile(c);
        udp[0] = std::make_unique<UdpNode>(0, true, &cond[0]);
        host_udp = udp[0]->port();
        if (!start_host(0)) return false;
        for (int i = 1; i < kMaxPlayers; ++i) {
            udp[i] = std::make_unique<UdpNode>(i, false, &cond[i]);
            proxy[i] = std::make_unique<TcpProxy>(host_port, &cond[i], kNames[i]);
        }
        for (const auto& [a, b] : blocked_pairs) block_pair(a, b, true);
        const auto t0 = Clock::now();
        for (int i = 1; i < kMaxPlayers; ++i)
            if (!start_guest(i)) return false;
        bool all = wait_until(
            [&] {
                for (int i = 1; i < kMaxPlayers; ++i)
                    if (G(i)->link->state() != LinkState::Connected) return false;
                return true;
            },
            20000);
        say("  handshake: %s, 3 guests connected in %.0f ms", all ? "ok" : "FAILED", ms_since(t0));
        for (int i = 1; i < kMaxPlayers; ++i) udp[i]->stun(host_udp, 3000);
        refresh_identities();
        start_threads();
        return all;
    }
    void down() {
        stop_threads();
        for (int i = 1; i < kMaxPlayers; ++i) crash_guest(i);
        if (host) {
            host->link->stop(true);
            host->stop_pumps();
            host.reset();
        }
        for (auto& p : proxy) p.reset();
        for (auto& u : udp) u.reset();
    }
};

// ---------------------------------------------------------------- verdicts

struct Verdict {
    std::string name;
    std::vector<std::string> fails;
    std::vector<std::string> notes;
    void check(bool ok, const std::string& what) {
        if (!ok) fails.push_back(what);
    }
    void note(const char* fmt, ...) __attribute__((format(gnu_printf, 2, 3))) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        notes.push_back(buf);
        say("  %s", buf);
    }
};
std::vector<std::pair<std::string, bool>> g_results;

void finish(Verdict& v) {
    std::string f;
    for (const auto& s : v.fails) f += (f.empty() ? "" : "; ") + s;
    say("VERDICT %s: %s%s%s", v.name.c_str(), v.fails.empty() ? "PASS" : "FAIL", f.empty() ? "" : " - ", f.c_str());
    g_results.emplace_back(v.name, v.fails.empty());
}

// Waits until every live guest has the host's current epoch's events up to what was pushed.
bool drain_events(Party& p, int timeout_ms) {
    return wait_until(
        [&] {
            std::lock_guard<std::mutex> lk(p.gmu);
            if (!p.host) return false;
            for (int i = 1; i < kMaxPlayers; ++i) {
                GuestRun* g = p.guest[i].get();
                if (!g) continue;
                std::lock_guard<std::mutex> el(g->ev.mu);
                if (g->ev.epoch != p.host->epoch || g->ev.last < p.host->pushed[i].load()) return false;
            }
            return true;
        },
        timeout_ms, 50);
}

// The common checks and measurements of a run.
void report(Party& p, Verdict& v, bool expect_lossless_udp_direct) {
    (void)expect_lossless_udp_direct;
    p.traffic = false;
    p.gen = false;
    const auto t_drain = Clock::now();
    const bool drained = drain_events(p, 30000);
    v.note("drain: every guest had every event %.2f s after the last was queued", ms_since(t_drain) / 1000);
    v.check(drained, "events not all delivered");
    std::uint64_t delivered = 0, dups = 0, gaps = 0, missing = 0, bad_start = 0;
    for (int i = 1; i < kMaxPlayers; ++i) {
        GuestRun* g = p.G(i);
        if (!g) continue;
        std::lock_guard<std::mutex> lk(g->ev.mu);
        delivered += g->ev.delivered;
        dups += g->ev.dups;
        gaps += g->ev.gaps;
        missing += g->ev.missing;
        bad_start += g->ev.bad_start;
    }
    v.check(dups == 0, "duplicate events delivered");
    v.check(gaps == 0 && bad_start == 0, "event gaps (" + std::to_string(missing) + " missing)");
    v.note("events host->guests: %llu delivered, %llu dup, %llu gaps; latency p50 %.1f ms p99 %.1f ms max %.0f ms",
           static_cast<unsigned long long>(delivered), static_cast<unsigned long long>(dups),
           static_cast<unsigned long long>(gaps), g_tot.ev_lat.pct(50), g_tot.ev_lat.pct(99), g_tot.ev_lat.max());
    v.note("events guests->host: %llu delivered, %llu dup, %llu gaps",
           static_cast<unsigned long long>(g_tot.g2h_delivered.load()),
           static_cast<unsigned long long>(g_tot.g2h_dups.load()), static_cast<unsigned long long>(g_tot.g2h_gaps.load()));
    v.check(g_tot.g2h_dups == 0, "duplicate guest events");
    v.note("rpc: %llu ok, %llu failed, %llu wrong reply; rtt p50 %.1f ms p99 %.1f ms",
           static_cast<unsigned long long>(g_tot.rpc_ok.load()), static_cast<unsigned long long>(g_tot.rpc_fail.load()),
           static_cast<unsigned long long>(g_tot.rpc_bad.load()), g_tot.rpc_rtt.pct(50), g_tot.rpc_rtt.pct(99));
    v.check(g_tot.rpc_bad == 0, "rpc reply mismatch");
    v.check(g_tot.rpc_ok > 0, "no rpc answered");
    // Ping as the link measures it.
    std::vector<double> pings;
    for (int i = 1; i < kMaxPlayers; ++i)
        if (p.G(i)) pings.push_back(p.G(i)->link->rtt_ms());
    std::string ps;
    for (double x : pings) ps += " " + std::to_string(static_cast<int>(x));
    v.note("ping (guest rtt ms):%s", ps.c_str());
    // Game UDP.
    for (int f = 0; f < 2; ++f) {
        std::uint64_t sent = 0, got = 0, d = 0, wrong = 0;
        for (int s = 1; s < kMaxPlayers; ++s)
            for (int r = 0; r < kMaxPlayers; ++r) {
                if (r == s || !p.udp[s] || !p.udp[r]) continue;
                if (f == kRelay && r == 0) continue;
                sent += p.udp[s]->sent(r, static_cast<Flow>(f));
                const auto rx = p.udp[r]->rx(s, static_cast<Flow>(f));
                got += rx.received;
                d += rx.dups;
                wrong += rx.wrong_source;
            }
        double p50 = 0, p99 = 0;
        std::size_t n = 0;
        for (int r = 0; r < kMaxPlayers; ++r) {
            if (!p.udp[r]) continue;
            n += p.udp[r]->lat(static_cast<Flow>(f)).count();
        }
        // Latency over every receiver (merged).
        Samples all;
        for (int r = 0; r < kMaxPlayers; ++r) {
            if (!p.udp[r]) continue;
            Samples& s = p.udp[r]->lat(static_cast<Flow>(f));
            std::lock_guard<std::mutex> lk(s.mu);
            for (double x : s.v) all.v.push_back(x);
        }
        p50 = all.pct(50);
        p99 = all.pct(99);
        (void)n;
        const double ratio = sent ? 100.0 * got / sent : 0;
        v.note("udp %s: %llu sent, %.2f%% delivered, %llu dup, %llu wrong source; latency p50 %.1f ms p99 %.1f ms",
               f == kDirect ? "direct" : "relay ", static_cast<unsigned long long>(sent), ratio,
               static_cast<unsigned long long>(d), static_cast<unsigned long long>(wrong), p50, p99);
        v.check(wrong == 0, "udp datagrams from the wrong source address");
        v.check(sent > 0 && got > 0, std::string("no udp over ") + (f == kDirect ? "direct" : "relay"));
    }
    std::uint64_t rb = 0, db = 0;
    for (int s = 1; s < kMaxPlayers; ++s)
        if (p.udp[s]) {
            rb += p.udp[s]->relay_bytes_out();
            db += p.udp[s]->direct_bytes_out();
        }
    if (p.udp[0]) {
        const auto st = p.udp[0]->relay_server().stats();
        v.note("relay: %llu datagrams forwarded (%llu payload B), %llu clients, %llu re-registered, %llu to unknown ports",
               static_cast<unsigned long long>(st.forwarded), static_cast<unsigned long long>(st.forwarded_bytes),
               static_cast<unsigned long long>(p.udp[0]->relay_server().clients()),
               static_cast<unsigned long long>(st.reregistered), static_cast<unsigned long long>(st.unknown_dst));
    }
    const double hold_cpu = g_tot.hold_max_cycles.load() / g_cycles_per_ms;
    {
        // Guest-to-guest paths (PeerPaths): which are relayed now, how often they switched.
        std::string ps;
        for (int g = 1; g < kMaxPlayers; ++g) {
            if (!p.udp[g]) continue;
            for (int o = 1; o < kMaxPlayers; ++o) {
                if (o == g || !p.udp[o]) continue;
                char b[64];
                std::snprintf(b, sizeof b, " %c->%c %s/%d", kNames[g][0], kNames[o][0],
                              p.udp[g]->relayed_to(p.udp[o]->port()) ? "relay" : "direct",
                              p.udp[g]->switches_to(p.udp[o]->port()));
                ps += b;
            }
        }
        v.note("guest paths (now/switches):%s", ps.c_str());
    }
    v.note("main thread: %zu link calls, p99 %.3f ms, max %.2f ms (wall clock, scheduler included); link IO thread "
           "lock hold max %.2f ms wall, %.2f ms of its own CPU",
           g_tot.main_call.count(), g_tot.main_call.pct(99), g_tot.main_call.max(), g_tot.hold_max_us.load() / 1000.0,
           hold_cpu);
    // What the transport can make the main thread wait: the per-frame getters take no lock the
    // IO thread holds; a change (set_local_state, send_event) waits at most for one IO-thread
    // hold, whose own work is bounded (the CPU check). The wall-clock figures also count this
    // machine's scheduler (the test runs ~40 threads), so they only fail far above a frame.
    v.check(hold_cpu < 5.0, "the link IO thread worked over 5 ms under its lock");
    v.check(g_tot.main_call.p99_ok(1.0), "main-thread link calls p99 over 1 ms");
    v.check(g_tot.main_call.max() < 200.0, "a main-thread call took over 200 ms");
    (void)rb;
    (void)db;
}

// ---------------------------------------------------------------- scenarios

void run_profile(const char* name, const char* spec, int seconds) {
    Verdict v{name, {}, {}};
    say("== %s (%s, %d s)", name, spec, seconds);
    g_tot.clear();
    Party p;
    p.scenario = name;
    if (!p.up(profile(spec))) v.check(false, "handshake");
    sleep_ms(seconds * 1000);
    // No reconnects in a steady run.
    int lost = 0;
    for (int i = 1; i < kMaxPlayers; ++i)
        if (p.G(i)) lost += p.G(i)->count_state(LinkState::Reconnecting, Clock::time_point{});
    v.check(lost == 0, "link dropped " + std::to_string(lost) + " times");
    report(p, v, false);
    p.down();
    finish(v);
}

void run_outage(const char* name, int outage_ms) {
    Verdict v{name, {}, {}};
    say("== %s (dsl, all guests cut off for %d s)", name, outage_ms / 1000);
    g_tot.clear();
    Party p;
    if (!p.up(profile("dsl"))) v.check(false, "handshake");
    sleep_ms(5000);
    const auto t_out = Clock::now();
    p.outage_all(outage_ms);
    const auto t_back = t_out + std::chrono::milliseconds(outage_ms);
    sleep_ms(outage_ms - 200);
    p.udp[2]->arm_first_rx();  // just before the network is back (nothing gets through until then)
    // Back: every guest connected (resumed when it was dropped), events replayed.
    const bool back = wait_until(
        [&] {
            for (int i = 1; i < kMaxPlayers; ++i)
                if (p.G(i)->link->state() != LinkState::Connected) return false;
            return true;
        },
        60000);
    v.check(back, "a guest did not come back");
    int drops = 0, rejects = 0;
    std::string rt;
    for (int i = 1; i < kMaxPlayers; ++i) {
        GuestRun* g = p.G(i);
        drops += g->count_state(LinkState::Reconnecting, t_out);
        rejects += g->rejected;
        const auto c = g->connected_after(t_out);
        if (c != Clock::time_point{}) {
            char b[96];
            std::snprintf(b, sizeof b, " %s %.1f s;", kNames[i], std::chrono::duration<double>(c - t_back).count());
            rt += b;
            v.check(g->link->last_welcome_resumed(), std::string(kNames[i]) + " not resumed");
        }
    }
    v.check(rejects == 0, "rejected");
    if (outage_ms < 10000) {
        v.check(drops == 0, "the link dropped during a " + std::to_string(outage_ms / 1000) + " s outage");
        v.note("link: no drop (lost_timeout 10 s)");
    } else {
        v.check(drops >= 3, "the link did not notice the outage");
        v.note("reconnected after the outage ended:%s", rt.c_str());
    }
    // The roster at the host: everyone connected again in the same slot.
    const auto ro = p.host->link->roster();
    int conn = 0;
    for (const auto& e : ro) conn += e.connected;
    v.check(conn == kMaxPlayers, "host roster not full");
    // UDP flows recover.
    sleep_ms(3000);
    for (int f = 0; f < 2; ++f) {
        const auto first = p.udp[2]->first_rx(1, static_cast<Flow>(f));
        v.check(first != Clock::time_point{}, std::string("udp ") + (f ? "relay" : "direct") + " did not recover");
        if (first != Clock::time_point{})
            v.note("udp %s back %.2f s after the outage", f ? "relay" : "direct",
                   std::chrono::duration<double>(first - t_back).count());
    }
    report(p, v, false);
    p.down();
    finish(v);
}

void run_drop_rejoin() {
    Verdict v{"drop-rejoin", {}, {}};
    say("== drop-rejoin (dsl): Alice's connection reset, Bob's game crashes and restarts 20 s later");
    g_tot.clear();
    Party p;
    if (!p.up(profile("dsl"))) v.check(false, "handshake");
    sleep_ms(4000);
    const int alice_slot = p.G(1)->link->local_slot();
    const int bob_slot = p.G(2)->link->local_slot();
    // Alice: the NAT drops her connection (RST); she reconnects with her token at once.
    const auto t_reset = Clock::now();
    p.proxy[1]->reset_all();
    const bool a_back = wait_until(
        [&] { return p.G(1)->connected_after(t_reset) != Clock::time_point{}; }, 20000);
    v.check(a_back, "Alice did not reconnect");
    if (a_back) {
        v.note("Alice (connection reset): back in %.2f s, slot %d -> %d, resumed %s",
               std::chrono::duration<double>(p.G(1)->connected_after(t_reset) - t_reset).count(), alice_slot,
               p.G(1)->link->local_slot(), p.G(1)->link->last_welcome_resumed() ? "yes" : "no");
        v.check(p.G(1)->link->last_welcome_resumed() && p.G(1)->link->local_slot() == alice_slot, "Alice not resumed");
    }
    // Bob: his process dies; 20 s later a new one (no token, same name) joins.
    p.crash_guest(2);
    const auto t_crash = Clock::now();
    const bool lost_seen = wait_until(
        [&] {
            for (const auto& e : p.host->link->roster())
                if (e.name == kNames[2] && !e.connected) return true;
            return false;
        },
        15000);
    v.check(lost_seen, "the host did not see Bob lost");
    v.note("host saw Bob lost after %.1f s (slot kept)", ms_since(t_crash) / 1000);
    sleep_ms(std::max(0, 20000 - static_cast<int>(ms_since(t_crash))));
    const auto t_restart = Clock::now();
    p.start_guest(2);
    const bool b_back = wait_until([&] { return p.G(2)->link->state() == LinkState::Connected; }, 30000);
    v.check(b_back, "Bob did not rejoin");
    if (b_back) {
        v.note("Bob (crash, restart after 20 s): joined in %.2f s, slot %d -> %d (inside the 60 s slot window)",
               ms_since(t_restart) / 1000, bob_slot, p.G(2)->link->local_slot());
        v.check(p.G(2)->link->local_slot() == bob_slot, "Bob got another slot");
    }
    int conn = 0;
    for (const auto& e : p.host->link->roster()) conn += e.connected;
    v.check(conn == kMaxPlayers, "host roster not full");
    sleep_ms(4000);
    {
        std::lock_guard<std::mutex> lk(p.G(2)->ev.mu);
        v.note("Bob's new game got events from #%llu on (a name rejoin starts a fresh event stream: the crashed "
               "game's unacknowledged ones are dropped, by design)",
               static_cast<unsigned long long>(p.G(2)->ev.first_seq));
    }
    report(p, v, false);
    p.down();
    finish(v);
}

// Alice and Bob have no direct path (their NATs): PeerPaths falls back to the host relay; the
// path opens (back to direct), then closes again mid-session (back to the relay).
void run_nat_pair() {
    Verdict v{"nat-pair", {}, {}};
    say("== nat-pair (dsl): Alice <-> Bob have no direct path, then one opens, then it closes again");
    g_tot.clear();
    Party p;
    p.blocked_pairs = {{1, 2}};
    if (!p.up(profile("dsl"))) v.check(false, "handshake");
    const auto t0 = Clock::now();
    p.udp[1]->arm_first_rx();
    p.udp[2]->arm_first_rx();
    UdpNode& A = *p.udp[1];
    UdpNode& B = *p.udp[2];
    UdpNode& Cn = *p.udp[3];
    const bool through = wait_until(
        [&] {
            return B.first_rx(1, kDirect) != Clock::time_point{} && A.first_rx(2, kDirect) != Clock::time_point{};
        },
        15000);
    v.check(through, "Alice and Bob never reached each other");
    if (through) {
        v.note("blocked from the start: Alice->Bob through the relay after %.2f s, Bob->Alice after %.2f s",
               std::chrono::duration<double>(B.first_rx(1, kDirect) - t0).count(),
               std::chrono::duration<double>(A.first_rx(2, kDirect) - t0).count());
    }
    v.check(A.relayed_to(B.port()) && B.relayed_to(A.port()), "Alice <-> Bob not relayed");
    v.check(!A.relayed_to(Cn.port()) && !Cn.relayed_to(A.port()) && !B.relayed_to(Cn.port()),
            "a pair with a direct path was relayed");
    // Delivery over the relayed path, as the game sends it (to the direct address).
    const std::uint64_t s0 = A.sent(2, kDirect), r0 = B.rx(1, kDirect).received;
    sleep_ms(8000);
    const std::uint64_t s1 = A.sent(2, kDirect), r1 = B.rx(1, kDirect).received;
    const double ratio = s1 > s0 ? 100.0 * static_cast<double>(r1 - r0) / static_cast<double>(s1 - s0) : 0;
    v.note("Alice->Bob addressed directly, delivered through the relay: %.1f %% over 8 s", ratio);
    v.check(ratio > 85, "the relayed path lost too much");
    // The direct path opens: back to direct.
    const auto t_open = Clock::now();
    p.block_pair(1, 2, false);
    const bool direct = wait_until([&] { return !A.relayed_to(B.port()) && !B.relayed_to(A.port()); }, 10000);
    v.check(direct, "not back to direct after the path opened");
    if (direct) v.note("path opened: direct again after %.2f s", ms_since(t_open) / 1000);
    sleep_ms(4000);
    // ... and closes again mid-session: the relay takes over.
    const auto t_close = Clock::now();
    B.arm_first_rx();
    p.block_pair(1, 2, true);
    const bool relayed = wait_until([&] { return A.relayed_to(B.port()) && B.relayed_to(A.port()); }, 10000);
    const double t_relayed = ms_since(t_close) / 1000;
    v.check(relayed, "not relayed after the path closed");
    const bool again = wait_until([&] { return B.first_rx(1, kDirect) != Clock::time_point{}; }, 10000);
    v.check(again, "Alice->Bob did not resume through the relay");
    if (relayed && again)
        v.note("path closed: relayed after %.2f s, Alice->Bob datagrams flowing again after %.2f s",
               t_relayed,
               std::chrono::duration<double>(B.first_rx(1, kDirect) - t_close).count());
    sleep_ms(3000);
    v.check(A.switches_to(Cn.port()) == 0 && B.switches_to(Cn.port()) == 0 && Cn.switches_to(A.port()) == 0 &&
                Cn.switches_to(B.port()) == 0,
            "a pair with a working direct path switched");
    report(p, v, false);
    p.down();
    finish(v);
}

void run_host_restart(int down_ms) {
    Verdict v{"host-restart", {}, {}};
    say("== host-restart (dsl): the host's game crashes, is back %d s later on the same ports", down_ms / 1000);
    g_tot.clear();
    Party p;
    if (!p.up(profile("dsl"))) v.check(false, "handshake");
    sleep_ms(5000);
    const std::uint16_t tcp = p.host_port, udpp = p.host_udp;
    std::uint16_t relay_before[kMaxPlayers] = {};
    for (int i = 1; i < kMaxPlayers; ++i) p.udp[i]->relay_client().get(nullptr, &relay_before[i]);
    p.crash_host();
    const auto t_crash = Clock::now();
    sleep_ms(down_ms);
    // The new host process: same party code (secret), same ports, fresh state everywhere.
    p.udp[0] = std::make_unique<UdpNode>(0, true, &p.cond[0], udpp);
    v.check(p.udp[0]->bound(), "the party UDP port could not be bound again");
    p.udp[2]->arm_first_rx();  // direct Alice->Bob never stopped; relay did with the host
    const bool started = p.start_host(tcp);
    v.check(started, "the host could not start again");
    const auto t_up = Clock::now();
    const bool back = wait_until(
        [&] {
            for (int i = 1; i < kMaxPlayers; ++i)
                if (p.G(i)->link->state() != LinkState::Connected || p.G(i)->connected_after(t_up) == Clock::time_point{})
                    return false;
            return true;
        },
        90000);
    v.check(back, "guests did not reconnect");
    std::string rt;
    double worst = 0;
    for (int i = 1; i < kMaxPlayers; ++i) {
        GuestRun* g = p.G(i);
        v.check(g->rejected == 0, std::string(kNames[i]) + " rejected");
        const auto c = g->connected_after(t_up);
        if (c == Clock::time_point{}) continue;
        const double s = std::chrono::duration<double>(c - t_up).count();
        worst = std::max(worst, s);
        char b[96];
        std::snprintf(b, sizeof b, " %s %.1f s (attempts while down: %d);", kNames[i], s,
                      g->count_state(LinkState::Reconnecting, t_crash));
        rt += b;
    }
    v.note("reconnect after the host came back:%s worst %.1f s", rt.c_str(), worst);
    // Relay: the guests' old tokens are re-registered by their first frames; ports unchanged.
    sleep_ms(3000);
    for (int f = 0; f < 2; ++f) {
        const auto first = p.udp[2]->first_rx(1, static_cast<Flow>(f));
        v.check(first != Clock::time_point{}, std::string("udp ") + (f ? "relay" : "direct") + " did not recover");
        if (first != Clock::time_point{})
            v.note("udp %s Alice->Bob back %.2f s after the host", f ? "relay" : "direct",
                   std::chrono::duration<double>(first - t_up).count());
    }
    std::string ports;
    for (int i = 1; i < kMaxPlayers; ++i) {
        std::uint16_t now = 0;
        p.udp[i]->relay_client().get(nullptr, &now);
        ports += " " + std::to_string(relay_before[i]) + "->" + std::to_string(now);
        v.check(now == relay_before[i], std::string(kNames[i]) + "'s relay port changed");
    }
    v.note("relay ports kept across the restart:%s", ports.c_str());
    report(p, v, false);
    p.down();
    finish(v);
}

}  // namespace

int main(int argc, char** argv) {
    // Loopback-only sockets: no Windows Firewall prompt for a test program (party_sock.h).
    if (!std::getenv("BB_PARTY_LOOPBACK")) {
#if defined(_WIN32)
        _putenv("BB_PARTY_LOOPBACK=1");
#else
        setenv("BB_PARTY_LOOPBACK", "1", 1);
#endif
    }
    party::sock::startup();
#if defined(_WIN32)
    timeBeginPeriod(1);  // as the game runs (its frame pacing): 1 ms waits, not 15.6 ms
#endif
    g_log = std::fopen("party_soak.log", "w");
#if defined(_WIN32)
    _putenv_s("BB_PARTY_FORCE_RELAY", "1");
#else
    setenv("BB_PARTY_FORCE_RELAY", "1", 1);
#endif
    if (!udp::force_relay_from_env()) std::printf("BB_PARTY_FORCE_RELAY not parsed\n");
#if defined(_WIN32)
    {
        // CPU cycles per millisecond of this thread, for the lock-hold figures.
        ULONG64 c0 = 0, c1 = 0;
        QueryThreadCycleTime(GetCurrentThread(), &c0);
        const auto t = Clock::now();
        while (ms_since(t) < 50) {
        }
        QueryThreadCycleTime(GetCurrentThread(), &c1);
        g_cycles_per_ms = static_cast<double>(c1 - c0) / ms_since(t);
    }
#endif
    std::set<std::string> only;
    for (int i = 1; i < argc; ++i) only.insert(argv[i]);
    auto want = [&](const char* n) { return only.empty() || only.count(n); };
    const auto t0 = Clock::now();
    if (want("lan")) run_profile("lan", "lan", 20);
    if (want("dsl")) run_profile("dsl", "dsl", 25);
    if (want("bad")) run_profile("bad", "bad", 25);
    if (want("outage-5s")) run_outage("outage-5s", 5000);
    if (want("outage-30s")) run_outage("outage-30s", 30000);
    if (want("drop-rejoin")) run_drop_rejoin();
    if (want("host-restart")) run_host_restart(10000);
    if (want("nat-pair")) run_nat_pair();
    int failed = 0;
    std::printf("\n");
    for (const auto& [n, ok] : g_results) {
        std::printf("%-14s %s\n", n.c_str(), ok ? "PASS" : "FAIL");
        failed += !ok;
    }
    say("party-soak: %d/%zu scenarios passed in %.0f s", static_cast<int>(g_results.size()) - failed, g_results.size(),
        ms_since(t0) / 1000);
    if (g_log) std::fclose(g_log);
    return failed ? 1 : 0;
}
