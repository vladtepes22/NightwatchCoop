// SPDX-License-Identifier: GPL-3.0-or-later
// The party network library's C interface (gpu/bbnet.h): settings from the environment, import
// routing, the status line and the guest-callback dispatcher thread.
#include "bbnet_internal.h"
#include "netsim.h"
#include "../../bbnet.h"
#include "../../party_env.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace bbnet {

namespace {

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? v : nullptr;
}

bool valid_party_name(const char* s) {
    const std::size_t n = std::strlen(s);
    if (n == 0 || n > 16) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (!std::isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

// BB_USER_NAME as an online id: the allowed characters, at most 16.
std::string online_id_from_user_name(const char* s) {
    std::string out;
    for (const char* p = s; p && *p && out.size() < 16; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (std::isalnum(c) || c == '_' || c == '-') out += static_cast<char>(c);
        else if (c == ' ') out += '_';
    }
    return out.empty() ? "Hunter" : out;
}

std::mutex g_settings_mu;
Settings* g_settings = nullptr;

Settings* load_settings() {
    auto* s = new Settings;
    if (const char* p = bb_party_value()) {
        s->party = true;
        s->party_value = p;
        std::string lower(p);
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        s->host = lower == "host";
    }
    if (const char* h = env("BB_PARTY_HOST")) s->host = h[0] == '1';
    if (const char* port = env("BB_PARTY_PORT")) {
        const long v = std::strtol(port, nullptr, 10);
        if (v > 0 && v < 65536) s->party_port = static_cast<std::uint16_t>(v);
        else std::printf("Net: BB_PARTY_PORT '%s' is not a port; using %u\n", port, s->party_port);
    }
    const char* name = env("BB_PARTY_NAME");
    if (name && valid_party_name(name)) {
        s->online_id = name;
    } else {
        if (name) std::printf("Net: BB_PARTY_NAME '%s' is not 1-16 of [A-Za-z0-9_-]; using the user name\n", name);
        s->online_id = online_id_from_user_name(env("BB_USER_NAME") ? env("BB_USER_NAME") : "Hunter");
    }
    if (const char* cb = env("BB_NP_STATE_CB")) s->np_state_callback = cb[0] != '0';
    if (s->party) {
        netsim::Config sim;
        std::string error;
        if (!netsim::parse(env("BB_NET_SIM"), &sim, &error)) {
            std::printf("Net: BB_NET_SIM ignored: %s\n", error.c_str());
        } else {
            netsim::configure(sim);
        }
        std::printf("Net: party networking on (%s, as %s, port %u)\n", s->host ? "host" : "guest", s->online_id.c_str(),
                    s->party_port);
    }
    return s;
}

}  // namespace

const Settings& settings() {
    std::lock_guard<std::mutex> lk(g_settings_mu);
    if (!g_settings) g_settings = load_settings();
    return *g_settings;
}

void set_dirs(const char* app0, const char* user_dir) {
    settings();
    std::lock_guard<std::mutex> lk(g_settings_mu);
    if (app0) g_settings->app0 = app0;
    if (user_dir) g_settings->user_dir = user_dir;
}

namespace {
std::atomic<std::uintptr_t> g_image_base{0};
std::atomic<std::uint64_t> g_image_size{0};
}  // namespace

std::uintptr_t guest_image_base() { return g_image_base.load(std::memory_order_acquire); }
std::uint64_t guest_image_size() { return g_image_size.load(std::memory_order_acquire); }
void* guest_image_at(std::uint64_t offset, std::size_t len) {
    const std::uintptr_t base = guest_image_base();
    const std::uint64_t size = guest_image_size();
    if (!base || offset > size || len > size - offset) return nullptr;
    return reinterpret_cast<void*>(base + offset);
}

namespace {
// True when [addr, addr+n) is committed memory with one of `access`'s protections.
bool memory_allows(std::uintptr_t addr, std::size_t n, bool write) {
    if (addr < 0x10000) return false;
#if defined(_WIN32)
    const DWORD ok = write ? (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)
                           : (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                              PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
    std::uintptr_t at = addr;
    const std::uintptr_t end = addr + n;
    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || !(mbi.Protect & ok) || (mbi.Protect & PAGE_GUARD)) return false;
        at = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
#else
    (void)n;
    (void)write;
#endif
    return true;
}
}  // namespace

bool guest_read(std::uintptr_t addr, void* out, std::size_t n) {
    if (!out || !memory_allows(addr, n, false)) return false;
    std::memcpy(out, reinterpret_cast<const void*>(addr), n);
    return true;
}

bool guest_write(std::uintptr_t addr, const void* in, std::size_t n) {
    if (!in || !memory_allows(addr, n, true)) return false;
    std::memcpy(reinterpret_cast<void*>(addr), in, n);
    return true;
}

bool party_trace() {
    static const bool on = [] {
        const char* e = std::getenv("BB_PARTY_TRACE");
        return e && e[0] == '1';
    }();
    return on;
}

void log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("Net: %s\n", buf);
    std::fflush(stdout);
}

// --- Guest-callback dispatcher ----------------------------------------------------------------

namespace {

struct GuestCall {
    std::uintptr_t fn;
    std::uint64_t a[6];
};
struct Dispatcher {
    std::mutex mu;
    std::condition_variable cv, done_cv;
    std::deque<GuestCall> q;
    std::uint64_t posted = 0, done = 0;
    bool started = false;
};
Dispatcher& dispatcher() {
    static Dispatcher* d = new Dispatcher;  // lives as long as its thread: the process
    return *d;
}

using GuestFn = std::uint64_t(BBNET_ABI*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                          std::uint64_t);

void dispatcher_main() {
    runtime_thread_attach_host("bb:net-cb");
    Dispatcher& d = dispatcher();
    std::unique_lock<std::mutex> lk(d.mu);
    for (;;) {
        d.cv.wait(lk, [&] { return !d.q.empty(); });
        const GuestCall c = d.q.front();
        d.q.pop_front();
        lk.unlock();
        restore_guest_fs();  // host code on this thread (the C library, Winsock) may have moved FS
        reinterpret_cast<GuestFn>(c.fn)(c.a[0], c.a[1], c.a[2], c.a[3], c.a[4], c.a[5]);
        lk.lock();
        ++d.done;
        d.done_cv.notify_all();
    }
}

}  // namespace

void post_guest_call(std::uintptr_t fn, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                     std::uint64_t a4, std::uint64_t a5) {
    if (!fn) return;
    Dispatcher& d = dispatcher();
    {
        std::lock_guard<std::mutex> lk(d.mu);
        d.q.push_back(GuestCall{fn, {a0, a1, a2, a3, a4, a5}});
        ++d.posted;
        if (!d.started) {
            d.started = true;
            std::thread(dispatcher_main).detach();
        }
    }
    d.cv.notify_one();
}

void drain_guest_calls() {
    Dispatcher& d = dispatcher();
    std::unique_lock<std::mutex> lk(d.mu);
    const std::uint64_t target = d.posted;
    d.done_cv.wait(lk, [&] { return d.done >= target; });
}

// --- The party runtime hooks ------------------------------------------------------------------

namespace {
RuntimeHooks& runtime_hooks() {
    static RuntimeHooks* h = new RuntimeHooks;  // set during static initialization, used at exit
    return *h;
}
}  // namespace

void set_runtime_hooks(const RuntimeHooks& hooks) { runtime_hooks() = hooks; }

void runtime_start_once() {
    static std::atomic<bool> started{false};
    if (!settings().party || started.exchange(true)) return;
    if (runtime_hooks().start) {
        runtime_hooks().start();
    } else {
        log("no party runtime linked: party link, codes and host service wiring are off");
    }
}

namespace {

// The imports routed to this library under BB_PARTY: libSceNet, NetCtl, Http, Ssl and Np,
// except the Np parts that stay offline whatever the mode.
bool routed(const char* name) {
    // Score: only the comment filter (np_manager.cpp); ranking and game data stay offline.
    static const char* const kIncluded[] = {"sceNpScoreCensorComment", "sceNpScoreSanitizeComment"};
    for (const char* x : kIncluded) {
        if (std::strcmp(name, x) == 0) return true;
    }
    static const char* const kExcluded[] = {"sceNpTrophy", "sceNpCommerce", "sceNpProfileDialog", "sceNpScore"};
    for (const char* x : kExcluded) {
        if (std::strncmp(name, x, std::strlen(x)) == 0) return false;
    }
    static const char* const kPrefixes[] = {"sceNet", "sceHttp", "sceSsl", "sceNp"};
    for (const char* p : kPrefixes) {
        if (std::strncmp(name, p, std::strlen(p)) == 0) return true;
    }
    return false;
}

void* find(const Export* table, const char* name) {
    for (const Export* e = table; e->name; ++e) {
        if (std::strcmp(e->name, name) == 0) return e->fn;
    }
    return nullptr;
}

}  // namespace

}  // namespace bbnet

extern "C" {

void bbnet_configure(const char* app0, const char* user_dir) {
    bbnet::set_dirs(app0, user_dir);
}

void bbnet_set_image(void* image, uint64_t size) {
    bbnet::g_image_size.store(image ? size : 0, std::memory_order_release);
    bbnet::g_image_base.store(reinterpret_cast<std::uintptr_t>(image), std::memory_order_release);
}

void bbnet_start(void) { bbnet::runtime_start_once(); }

int bbnet_party_enabled(void) { return bbnet::settings().party ? 1 : 0; }

uintptr_t bbnet_resolve(const char* scoped_nid_or_name) {
    if (!scoped_nid_or_name || !bbnet::settings().party) return 0;
    const char* name = scoped_nid_or_name;
    if (std::strchr(name, '#')) {
        name = runtime_symbol(scoped_nid_or_name);
        if (!name) return 0;
    }
    if (!bbnet::routed(name)) return 0;
    void* fn = nullptr;
    for (const bbnet::Export* table : {bbnet::kNetExports, bbnet::kNpExports, bbnet::kMatching2Exports,
                                       bbnet::kSignalingExports, bbnet::kHttpExports}) {
        if ((fn = bbnet::find(table, name))) break;
    }
    if (fn) bbnet::log("%s -> party library", name);
    return reinterpret_cast<uintptr_t>(fn);
}

const char* bbnet_status_line(void) {
    static std::mutex mu;
    static std::string line;
    const bbnet::Settings& s = bbnet::settings();
    std::lock_guard<std::mutex> lk(mu);
    if (!s.party) {
        line = "party off";
        return line.c_str();
    }
    const bbnet::netsim::Stats st = bbnet::netsim::stats();
    char buf[512];
    std::snprintf(buf, sizeof(buf), "party %s as %s; %s; netsim %s", s.host ? "host" : "guest", s.online_id.c_str(),
                  bbnet::p2p_status().c_str(),
                  bbnet::netsim::enabled()
                      ? ("on (" + std::to_string(st.submitted) + " sent, " + std::to_string(st.dropped) + " lost)").c_str()
                      : "off");
    line = buf;
    if (bbnet::runtime_hooks().status) line += "; " + bbnet::runtime_hooks().status();
    return line.c_str();
}

void bbnet_post_guest_call(uintptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    bbnet::post_guest_call(fn, a0, a1, a2, a3, a4, a5);
}

}  // extern "C"
