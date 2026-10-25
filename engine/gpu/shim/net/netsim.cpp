// SPDX-License-Identifier: GPL-3.0-or-later
// Network simulator (netsim.h): a delay line in front of sendto.
#include "netsim.h"

#include <cctype>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <atomic>
#include <set>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace bbnet::netsim {

namespace {

struct Preset {
    const char* name;
    const char* spec;
};
constexpr Preset kPresets[] = {
    {"lan", "lat=1,jitter=0.5"},
    {"wifi", "lat=5,jitter=5,loss=0.5"},
    {"dsl", "lat=80,jitter=20,loss=2,dup=0.5,reorder=1"},
    {"mobile", "lat=60,jitter=25,loss=2,dup=0.2,reorder=1"},
    {"bad", "lat=200,jitter=80,loss=8"},
};

bool heap_later(const Packet& a, const Packet& b) {
    if (a.due != b.due) return a.due > b.due;
    return a.seq > b.seq;
}

}  // namespace

bool parse(const char* spec, Config* out, std::string* error) {
    *out = Config{};
    if (!spec) return true;
    std::string s(spec);
    if (s.empty() || s == "0" || s == "off") return true;
    // A preset name, alone or first: "dsl" or "dsl,outage=5000,every=60000".
    {
        const std::size_t comma = s.find(',');
        const std::string head = s.substr(0, comma);
        for (const Preset& p : kPresets) {
            if (head == p.name) s = std::string(p.spec) + (comma == std::string::npos ? "" : s.substr(comma));
        }
    }
    std::size_t at = 0;
    while (at < s.size()) {
        std::size_t end = s.find(',', at);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(at, end - at);
        at = end + 1;
        if (item.empty()) continue;
        const std::size_t eq = item.find('=');
        if (eq == std::string::npos) {
            if (error) *error = "netsim: expected key=value, got '" + item + "'";
            return false;
        }
        const std::string key = item.substr(0, eq), value = item.substr(eq + 1);
        if (key == "nat" && !value.empty() && !std::isdigit(static_cast<unsigned char>(value[0]))) {
            if (value == "port" || value == "portrestricted") out->nat_kind = NatKind::PortRestricted;
            else if (value == "full" || value == "fullcone") out->nat_kind = NatKind::FullCone;
            else if (value == "addr" || value == "restricted") out->nat_kind = NatKind::AddrRestricted;
            else if (value == "sym" || value == "symmetric") out->nat_kind = NatKind::Symmetric;
            else {
                if (error) *error = "netsim: nat is 0, 1, port, full, addr or sym, not '" + value + "'";
                return false;
            }
            out->nat = true;
            continue;
        }
        if (key == "block") {  // block=15822+15823: destination ports (host order) cut off
            std::size_t p = 0;
            while (p < value.size()) {
                std::size_t q = value.find('+', p);
                if (q == std::string::npos) q = value.size();
                const long port = std::strtol(value.substr(p, q - p).c_str(), nullptr, 10);
                if (port <= 0 || port > 65535) {
                    if (error) *error = "netsim: bad port in block: '" + value + "'";
                    return false;
                }
                out->block_ports.push_back(static_cast<std::uint16_t>(port));
                p = q + 1;
            }
            continue;
        }
        char* rest = nullptr;
        const double v = std::strtod(value.c_str(), &rest);
        if (value.empty() || (rest && *rest) || v < 0) {
            if (error) *error = "netsim: bad value for " + key + ": '" + value + "'";
            return false;
        }
        if (key == "lat") out->lat_ms = v;
        else if (key == "jitter" || key == "jit") out->jitter_ms = v;
        else if (key == "at") out->outage_at_ms = v;
        else if (key == "any") out->any_addr = v != 0;
        else if (key == "nat") out->nat = v != 0;
        else if (key == "loss") out->loss_pct = std::min(v, 100.0);
        else if (key == "dup") out->dup_pct = std::min(v, 100.0);
        else if (key == "reorder") out->reorder_pct = std::min(v, 100.0);
        else if (key == "seed") out->seed = static_cast<std::uint64_t>(v);
        else if (key == "outage") out->outage_ms = v;
        else if (key == "every") out->outage_every_ms = v;
        else if (key == "block_at") out->block_at_ms = v;
        else if (key == "block_for") out->block_for_ms = v;
        else {
            if (error) *error = "netsim: unknown key '" + key + "'";
            return false;
        }
    }
    out->enabled = true;
    return true;
}

std::string describe(const Config& c) {
    if (!c.enabled) return "off";
    char buf[200];
    std::snprintf(buf, sizeof(buf), "lat=%g,jitter=%g,loss=%g,dup=%g,reorder=%g,seed=%llu", c.lat_ms, c.jitter_ms,
                  c.loss_pct, c.dup_pct, c.reorder_pct, static_cast<unsigned long long>(c.seed));
    std::string s = buf;
    if (c.outage_ms > 0 && c.outage_every_ms > 0) {
        std::snprintf(buf, sizeof(buf), ",outage=%g,every=%g", c.outage_ms, c.outage_every_ms);
        s += buf;
    }
    if (c.outage_ms > 0 && c.outage_at_ms > 0) {
        std::snprintf(buf, sizeof(buf), ",outage=%g,at=%g", c.outage_ms, c.outage_at_ms);
        s += buf;
    }
    if (!c.block_ports.empty()) {
        s += ",block=";
        for (std::size_t i = 0; i < c.block_ports.size(); ++i) s += (i ? "+" : "") + std::to_string(c.block_ports[i]);
        std::snprintf(buf, sizeof(buf), ",block_at=%g,block_for=%g", c.block_at_ms, c.block_for_ms);
        s += buf;
    }
    if (c.any_addr) s += ",any=1";
    if (c.nat) s += c.nat_kind == NatKind::PortRestricted ? std::string(",nat=1") : std::string(",nat=") + nat_kind_name(c.nat_kind);
    return s;
}

bool outage_at(const Config& c, Clock::time_point epoch, Clock::time_point first, Clock::time_point now,
               Clock::time_point* end) {
    using ms = std::chrono::duration<double, std::milli>;
    auto to_dur = [](double v) { return std::chrono::duration_cast<Clock::duration>(ms(v)); };
    if (c.outage_ms <= 0) return false;
    if (c.outage_at_ms > 0) {
        const Clock::time_point from = epoch + to_dur(c.outage_at_ms), until = from + to_dur(c.outage_ms);
        if (now >= from && now < until) {
            if (end) *end = until;
            return true;
        }
    }
    if (c.outage_every_ms > 0) {
        const double since = ms(now - first).count();
        if (since >= c.outage_every_ms) {
            const double into = std::fmod(since, c.outage_every_ms);
            if (into < c.outage_ms) {
                if (end) *end = now + to_dur(c.outage_ms - into);
                return true;
            }
        }
    }
    return false;
}

namespace {
// Test aid BB_NET_SIM_BLOCK_FILE=<path>: the file's text "1" cuts the block= ports now, "0" (or no
// file) lets them through; it replaces block_at / block_for. One file per instance (or shared).
std::atomic<int> g_block_file{-1};  // -1: no file in use
void block_file_watch(std::string path) {
    g_block_file = 0;
    std::thread([path] {
        int last = 0;
        for (;;) {
            int v = 0;
            if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
                char buf[16] = {};
                const std::size_t n = std::fread(buf, 1, sizeof buf - 1, f);
                std::fclose(f);
                v = n && buf[0] == '1' ? 1 : 0;
            }
            if (v != last) {
                std::printf("[coop] Net: block file %s: the block= ports are %s\n", path.c_str(), v ? "cut" : "open");
                last = v;
                g_block_file = v;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }).detach();
}
}  // namespace

bool blocked_at(const Config& c, Clock::time_point epoch, Clock::time_point now, std::uint16_t port_nbo) {
    if (c.block_ports.empty()) return false;
    if (g_block_file.load() >= 0) {
        if (!g_block_file.load()) return false;
        const std::uint16_t hp = static_cast<std::uint16_t>((port_nbo >> 8) | (port_nbo << 8));
        return std::find(c.block_ports.begin(), c.block_ports.end(), hp) != c.block_ports.end();
    }
    const std::uint16_t port = static_cast<std::uint16_t>((port_nbo >> 8) | (port_nbo << 8));
    if (std::find(c.block_ports.begin(), c.block_ports.end(), port) == c.block_ports.end()) return false;
    using ms = std::chrono::duration<double, std::milli>;
    const double since = ms(now - epoch).count();
    if (since < c.block_at_ms) return false;
    return c.block_for_ms <= 0 || since < c.block_at_ms + c.block_for_ms;
}

bool applies_to(const Config& c, std::uint32_t addr_net_order) {
    if (!c.enabled) return false;
    const auto* b = reinterpret_cast<const std::uint8_t*>(&addr_net_order);
    return c.any_addr || b[0] == 127;
}

StreamDelay::StreamDelay(const Config& c, Clock::time_point epoch, std::uint64_t salt)
    : cfg_(c), epoch_(epoch), rng_(c.seed * 0x9e3779b97f4a7c15ull + salt) {}

Clock::time_point StreamDelay::schedule(Clock::time_point now) {
    using ms = std::chrono::duration<double, std::milli>;
    auto uniform = [this] { return std::uniform_real_distribution<double>(0.0, 1.0)(rng_); };
    double d = cfg_.lat_ms;
    if (cfg_.jitter_ms > 0) d += (uniform() * 2.0 - 1.0) * cfg_.jitter_ms;
    if (d < 0) d = 0;
    if (cfg_.loss_pct > 0 && uniform() * 100.0 < cfg_.loss_pct) {
        // TCP: a lost segment is sent again after the retransmission timeout (at least 200 ms).
        d += std::max(200.0, 3.0 * cfg_.lat_ms);
        ++retransmits_;
    }
    Clock::time_point due = now + std::chrono::duration_cast<Clock::duration>(ms(d));
    Clock::time_point end;
    if (outage_at(cfg_, epoch_, epoch_, now, &end) || blackout_at(now, &end)) {
        const Clock::time_point after = end + std::chrono::duration_cast<Clock::duration>(ms(cfg_.lat_ms));
        if (after > due) due = after;
        ++outage_holds_;
    }
    if (due < last_) due = last_;
    last_ = due;
    return due;
}

Queue::Queue(const Config& c) : cfg_(c), rng_(c.seed) {}

double Queue::uniform() {
    return std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
}

Clock::duration Queue::delay() {
    double ms = cfg_.lat_ms;
    if (cfg_.jitter_ms > 0) ms += (uniform() * 2.0 - 1.0) * cfg_.jitter_ms;
    if (ms < 0) ms = 0;
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms));
}

void Queue::blackout(Clock::time_point from, Clock::time_point until) {
    black_from_ = from;
    black_until_ = until;
}

bool Queue::in_outage(Clock::time_point now) {
    if (!started_) {
        started_ = true;
        first_ = now;
    }
    if (now >= black_from_ && now < black_until_) return true;
    return outage_at(cfg_, has_epoch_ ? epoch_ : first_, first_, now, nullptr);
}

int Queue::submit(Clock::time_point now, std::uintptr_t fd, std::uint32_t addr, std::uint16_t port, const void* data,
                  std::size_t len) {
    ++stats_.submitted;
    if (has_epoch_ && blocked_at(cfg_, epoch_, now, port)) {
        ++stats_.dropped;
        return 0;
    }
    if (in_outage(now)) {
        ++stats_.dropped;
        ++stats_.blacked_out;
        return 0;
    }
    if (cfg_.loss_pct > 0 && uniform() * 100.0 < cfg_.loss_pct) {
        ++stats_.dropped;
        return 0;
    }
    const int copies = (cfg_.dup_pct > 0 && uniform() * 100.0 < cfg_.dup_pct) ? 2 : 1;
    if (copies == 2) ++stats_.duplicated;
    const std::uint64_t dest = addr | (static_cast<std::uint64_t>(port) << 32);
    for (int i = 0; i < copies; ++i) {
        Packet p;
        p.due = now + delay();
        p.seq = seq_++;
        p.fd = fd;
        p.addr = addr;
        p.port = port;
        p.data.assign(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + len);
        Clock::time_point& last = last_due_[dest];
        if (cfg_.reorder_pct > 0 && uniform() * 100.0 < cfg_.reorder_pct) {
            // Held back past the datagrams that follow it.
            const double extra = std::max(5.0, cfg_.jitter_ms * 2.0 + cfg_.lat_ms * 0.5 + 5.0);
            p.due += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(extra));
            ++stats_.reordered;
        } else if (p.due < last) {
            p.due = last;  // in order: never ahead of the destination's previous datagram
        } else {
            last = p.due;
        }
        heap_.push_back(std::move(p));
        std::push_heap(heap_.begin(), heap_.end(), heap_later);
    }
    return copies;
}

void Queue::take_due(Clock::time_point now, std::vector<Packet>& out) {
    while (!heap_.empty() && heap_.front().due <= now) {
        std::pop_heap(heap_.begin(), heap_.end(), heap_later);
        out.push_back(std::move(heap_.back()));
        heap_.pop_back();
        ++stats_.delivered;
    }
}

bool Queue::next_due(Clock::time_point* when) const {
    if (heap_.empty()) return false;
    *when = heap_.front().due;
    return true;
}

// --- Waiter ---------------------------------------------------------------------

#if defined(_WIN32)
Waiter::Waiter() {
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);  // before Windows 10 1803
}
Waiter::~Waiter() {
    if (timer_) CloseHandle(timer_);
    if (event_) CloseHandle(event_);
}
void Waiter::wait_until(Clock::time_point t) {
    const auto now = Clock::now();
    if (t <= now) return;
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t - now).count();
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(us) * 10;  // relative, 100 ns units
    HANDLE hs[2] = {event_, timer_};
    if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        CancelWaitableTimer(timer_);
    } else {
        WaitForSingleObject(event_, static_cast<DWORD>((us + 999) / 1000));
    }
}
void Waiter::notify() { SetEvent(event_); }
#else
Waiter::Waiter() = default;
Waiter::~Waiter() = default;
void Waiter::wait_until(Clock::time_point t) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_until(lk, t, [&] { return flag_; });
    flag_ = false;
}
void Waiter::notify() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        flag_ = true;
    }
    cv_.notify_one();
}
#endif

// --- The process-wide simulator ----------------------------------------------

namespace {

struct Sim {
    std::mutex mu;
    Waiter waiter;
    Queue* queue = nullptr;  // never freed: the sender thread lives as long as the process
    bool thread_started = false;
    Config cfg;
    Clock::time_point epoch{};
};
Sim& sim() {
    static Sim* s = new Sim;
    return *s;
}

void sender() {
    Sim& s = sim();
    std::vector<Packet> due;
    std::unique_lock<std::mutex> lk(s.mu);
    for (;;) {
        Clock::time_point when;
        const Clock::time_point now = Clock::now();
        if (!s.queue->next_due(&when)) when = now + std::chrono::seconds(1);
        if (when > now) {
            lk.unlock();
            s.waiter.wait_until(when);  // a submit wakes it (the event stays set if it came first)
            lk.lock();
            continue;
        }
        due.clear();
        s.queue->take_due(now, due);
        lk.unlock();
        for (const Packet& p : due) {
            sockaddr_in sa{};
            sa.sin_family = AF_INET;
            sa.sin_addr.s_addr = p.addr;
            sa.sin_port = p.port;
#if defined(_WIN32)
            ::sendto(static_cast<SOCKET>(p.fd), reinterpret_cast<const char*>(p.data.data()),
                     static_cast<int>(p.data.size()), 0, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
#else
            ::sendto(static_cast<int>(p.fd), p.data.data(), p.data.size(), 0, reinterpret_cast<const sockaddr*>(&sa),
                     sizeof(sa));
#endif
        }
        lk.lock();
    }
}

}  // namespace

namespace {
std::atomic<std::int64_t> g_black_from{0}, g_black_until{0};  // steady_clock ticks
}  // namespace

std::mutex g_nat_mu;
std::set<std::uint64_t> g_nat_out;

void nat_note_out(std::uint32_t addr, std::uint16_t port) {
    std::lock_guard<std::mutex> lk(g_nat_mu);
    g_nat_out.insert(static_cast<std::uint64_t>(addr) << 16 | port);
}

const char* nat_kind_name(NatKind k) {
    switch (k) {
    case NatKind::PortRestricted: return "port";
    case NatKind::FullCone: return "full";
    case NatKind::AddrRestricted: return "addr";
    case NatKind::Symmetric: return "sym";
    }
    return "?";
}

bool nat_allow_in_kind(NatKind kind, std::uint32_t addr, std::uint16_t port) {
    if (kind == NatKind::FullCone) return true;
    std::lock_guard<std::mutex> lk(g_nat_mu);
    if (kind == NatKind::AddrRestricted) {
        const std::uint64_t lo = static_cast<std::uint64_t>(addr) << 16;
        auto it = g_nat_out.lower_bound(lo);
        return it != g_nat_out.end() && (*it >> 16) == addr;
    }
    return g_nat_out.count(static_cast<std::uint64_t>(addr) << 16 | port) != 0;
}

bool nat_allow_in(std::uint32_t addr, std::uint16_t port) { return nat_allow_in_kind(config().nat_kind, addr, port); }

std::uint16_t nat_advertised_port(std::uint16_t port_nbo, int server) {
    const Config c = config();
    if (!c.enabled || !c.nat || c.nat_kind != NatKind::Symmetric) return port_nbo;
    // Another external port per destination: 1000 + 1000 * server above the local one.
    const std::uint16_t host = static_cast<std::uint16_t>((port_nbo >> 8) | (port_nbo << 8));
    const std::uint16_t moved = static_cast<std::uint16_t>(host + 1000 + 1000 * server);
    return static_cast<std::uint16_t>((moved >> 8) | (moved << 8));
}

bool blackout_at(Clock::time_point now, Clock::time_point* end) {
    const std::int64_t t = now.time_since_epoch().count();
    const std::int64_t from = g_black_from.load(), until = g_black_until.load();
    if (until == 0 || t < from || t >= until) return false;
    if (end) *end = Clock::time_point(Clock::duration(until));
    return true;
}

void outage_now(double ms_len) {
    const Clock::time_point now = Clock::now();
    const Clock::time_point until =
        now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms_len));
    g_black_from = now.time_since_epoch().count();
    g_black_until = until.time_since_epoch().count();
    Sim& s = sim();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.queue) s.queue->blackout(now, until);
    std::printf("[coop] Net: simulated outage for %.0f ms%s\n", ms_len, s.queue ? "" : " (simulator off: no effect)");
}

// Test aid BB_NET_SIM_OUTAGE_FILE=<path>: an outage_now(MS) whenever that file's text ("MS", or
// "MS tag" to fire again) changes - one file shared by every instance of a local run gives a
// symmetric total outage on all of them at the same moment (a real outage silences both ends; the
// director's host_outage / guest_outage steps run on one instance's clock each).
void outage_file_watch(std::string path) {
    std::thread([path] {
        std::string last;
        bool first = true;
        for (;;) {
            std::string text;
            if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
                char buf[64] = {};
                const std::size_t n = std::fread(buf, 1, sizeof buf - 1, f);
                std::fclose(f);
                text.assign(buf, n);
            }
            // A file already there at the start is old: only a change fires.
            if (!first && !text.empty() && text != last) {
                const double ms = std::atof(text.c_str());
                if (ms > 0 && ms <= 600000) {
                    std::printf("[coop] Net: outage file %s: %s\n", path.c_str(), text.c_str());
                    outage_now(ms);
                }
            }
            last = text;
            first = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }).detach();
}

void configure(const Config& c) {
    {
        Sim& s = sim();
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.queue) return;  // once per process
        if (!c.enabled) return;
        s.queue = new Queue(c);
        s.cfg = c;
        s.epoch = Clock::now();
        s.queue->set_epoch(s.epoch);
        std::printf("[coop] Net: network simulator on (%s)%s\n", describe(c).c_str(),
                    c.any_addr ? "" : " (loopback peers only)");
    }
    if (const char* f = std::getenv("BB_NET_SIM_OUTAGE_FILE"); f && f[0]) outage_file_watch(f);
    if (const char* f = std::getenv("BB_NET_SIM_BLOCK_FILE"); f && f[0] && !c.block_ports.empty()) block_file_watch(f);
}

void reconfigure_for_tests(const Config& c) {
    Sim& s = sim();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!c.enabled) return;
    s.queue = new Queue(c);  // the old one is leaked on purpose: the sender thread may hold it
    s.cfg = c;
    s.epoch = Clock::now();
    s.queue->set_epoch(s.epoch);
    std::printf("[coop] Net: network simulator reconfigured (%s)\n", describe(c).c_str());
}

Config config() {
    Sim& s = sim();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.queue ? s.cfg : Config{};
}

Clock::time_point epoch() {
    Sim& s = sim();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.epoch;
}

bool enabled() {
    Sim& s = sim();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.queue != nullptr;
}

Stats stats() {
    Sim& s = sim();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.queue ? s.queue->stats() : Stats{};
}

int sendto(std::uintptr_t fd, const void* data, std::size_t len, std::uint32_t addr, std::uint16_t port) {
    Sim& s = sim();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.queue) return -1;
        s.queue->submit(Clock::now(), fd, addr, port, data, len);
        if (!s.thread_started) {
            s.thread_started = true;
            std::thread(sender).detach();
        }
    }
    s.waiter.notify();
    return static_cast<int>(len);
}

}  // namespace bbnet::netsim
