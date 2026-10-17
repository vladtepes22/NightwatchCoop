// SPDX-License-Identifier: GPL-3.0-or-later
// The party runtime (party_runtime.h): wires PartyLink (A4) to the host service / transport (A3)
// and the network library (A1/A2) inside the game process.
#include "party_runtime.h"
#include "party_async_io.h"
#include "party_chalice.h"

#include "party_addr.h"
#include "party_code.h"
#include "party_crypto.h"
#include "party_b2.h"
#include "party_balance.h"
#include "party_director.h"
#include "party_flow.h"
#include "party_story.h"
#include "party_status_bridge.h"
#include "party_items.h"
#include "party_guestsvc.h"
#include "party_phantom.h"
#include "party_fourp.h"
#include "party_progress.h"
#include "party_split.h"
#include "party_desync.h"
#include "party_travel.h"
#include "party_worldact.h"
#include "party_npctalk.h"
#include "party_ping.h"
#include "party_droproll.h"
#include "party_log_game.h"
#include "coop_hooks.h"
#include "game_state.h"
#include "upnp_win.h"
#include "party_rudp.h"
#include "party_lan.h"
#include "party_traversal.h"

#include "../net/bbnet_internal.h"
#include "../net/np_hle.h"
#include "../net/np_session.h"
#include "../net/from_api.h"
#include "../net/party_host_service.h"
#include "../net/party_transport.h"
#include "../net/party_util.h"
#include "../../bbnet.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace party {

// The build's identity, generated at build time (gpu/cmake/bbport_build_id.cmake).
extern "C" const char* bbport_build_id(void);

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

void plog(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void plog(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party: ") + coop::PartyLogStamp() + buf + "\n"); // never blocks the caller (party_async_io.h)
}

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? v : nullptr;
}

bool env_on(const char* name) {
    const char* v = env(name);
    return v && v[0] != '0';
}

bool loopback_mode() { return env_on("BB_PARTY_LOOPBACK") || env_on("BB_MP_LOCAL_TEST"); }
void nr_fast_expire(const std::string& name);  // below (C6)

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---- SHA-256 (FIPS 180-4), for the eboot hash in HELLO -------------------------------------

class Sha256 {
public:
    Sha256() { reset(); }
    void reset() {
        static const std::uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::memcpy(h_, init, sizeof h_);
        len_ = 0;
        fill_ = 0;
    }
    void update(const void* data, std::size_t n) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        len_ += n;
        if (fill_) {
            const std::size_t take = std::min<std::size_t>(64 - fill_, n);
            std::memcpy(buf_ + fill_, p, take);
            fill_ += take;
            p += take;
            n -= take;
            if (fill_ == 64) {
                block(buf_);
                fill_ = 0;
            }
        }
        while (n >= 64) {
            block(p);
            p += 64;
            n -= 64;
        }
        if (n) {
            std::memcpy(buf_, p, n);
            fill_ = n;
        }
    }
    std::array<std::uint8_t, 32> finish() {
        const std::uint64_t bits = len_ * 8;
        const std::uint8_t pad = 0x80;
        update(&pad, 1);
        const std::uint8_t zero = 0;
        while (fill_ != 56) update(&zero, 1);
        std::uint8_t lenb[8];
        for (int i = 0; i < 8; ++i) lenb[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        update(lenb, 8);
        std::array<std::uint8_t, 32> out{};
        for (int i = 0; i < 8; ++i) {
            out[4 * i] = static_cast<std::uint8_t>(h_[i] >> 24);
            out[4 * i + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
            out[4 * i + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
            out[4 * i + 3] = static_cast<std::uint8_t>(h_[i]);
        }
        return out;
    }

private:
    static std::uint32_t ror(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const std::uint8_t* p) {
        static const std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
                   (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }
    std::uint32_t h_[8];
    std::uint64_t len_;
    std::uint8_t buf_[64];
    std::size_t fill_;
};

std::string hex_prefix(const std::array<std::uint8_t, 32>& h, int bytes = 6) {
    std::string s;
    char b[3];
    for (int i = 0; i < bytes; ++i) {
        std::snprintf(b, sizeof b, "%02x", h[i]);
        s += b;
    }
    return s;
}

bool sha256_file(const fs::path& path, std::array<std::uint8_t, 32>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    Sha256 sha;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = f.gcount();
        if (got > 0) sha.update(buf.data(), static_cast<std::size_t>(got));
    }
    *out = sha.finish();
    return true;
}

bool read_file(const fs::path& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

std::string data_dir() { return env("BB_DATA_DIR") ? env("BB_DATA_DIR") : "."; }

std::string user_dir() {
    const std::string& configured = bbnet::settings().user_dir;
    if (!configured.empty()) return configured;
    if (const char* u = env("BB_GPU_USER_DIR")) return u;
    if (const char* u = env("BB_USER_DIR")) return u;
    return data_dir() + "/user";
}

// The game's eboot.bin: app0 (bbnet_configure) or BB_GAME_DIR. Zeros (with a warning) when it
// cannot be read: then only players who also could not read theirs match.
std::array<std::uint8_t, 32> eboot_hash() {
    std::array<std::uint8_t, 32> h{};
    std::string dir = bbnet::settings().app0;
    if (dir.empty() && env("BB_GAME_DIR")) dir = env("BB_GAME_DIR");
    if (dir.empty()) {
        plog("warning: no game directory (BB_GAME_DIR) to hash eboot.bin from");
        return h;
    }
    const auto t0 = Clock::now();
    const fs::path p = fs::path(dir) / "eboot.bin";
    if (!sha256_file(p, &h)) {
        plog("warning: cannot read %s for the game version check", p.string().c_str());
        return h;
    }
    plog("eboot.bin sha256 %s... (%.0f ms)", hex_prefix(h).c_str(),
         std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    return h;
}

// A9: the game's param set, dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx under the game
// directory (mods replacing it count under the gameplay mods). Zeros (logged) when unreadable.
std::array<std::uint8_t, 32> params_hash() {
    std::array<std::uint8_t, 32> h{};
    std::string dir = bbnet::settings().app0;
    if (dir.empty() && env("BB_GAME_DIR")) dir = env("BB_GAME_DIR");
    if (dir.empty()) return h;
    const fs::path p = fs::path(dir) / "dvdroot_ps4" / "param" / "gameparam" / "gameparam.parambnd.dcx";
    if (!sha256_file(p, &h)) {
        plog("warning: cannot read %s for the version check (params not compared)", p.string().c_str());
        return std::array<std::uint8_t, 32>{};
    }
    return h;
}

// A9: this game's full version-check identity in <user dir>/party_identity.txt (what HELLO
// carries), for tools/party/simguest.py --identity: a simulated member that joins this host
// (or this guest's host) with the same identity. Never holds the password or the party secret.
void write_identity_file(const LinkConfig& cfg) {
    const auto hex = [](const std::array<std::uint8_t, 32>& h) {
        static const char* d = "0123456789abcdef";
        std::string s;
        for (std::uint8_t b : h) s += d[b >> 4], s += d[b & 15];
        return s;
    };
    std::string t = "# bbport party identity (A9 version check; party_runtime.cpp)\n";
    t += "build " + cfg.build_id + "\n";
    t += "rules " + cfg.rules + "\n";
    t += "eboot " + hex(cfg.eboot_sha256) + "\n";
    t += "params " + hex(cfg.params_sha256) + "\n";
    t += "patches " + hex(cfg.patches_hash) + "\n";
    t += "mods " + hex(cfg.mods_hash) + "\n";
    t += "hooks " + hex(cfg.hooks_hash) + "\n";
    for (const std::string& n : cfg.patch_names) t += "patch " + n + "\n";
    for (const std::string& n : cfg.mod_names) t += "mod " + n + "\n";
    for (const std::string& n : cfg.hook_names) t += "hook " + n + "\n";
    std::error_code ec;
    fs::create_directories(user_dir(), ec);
    std::ofstream f(fs::path(user_dir()) / "party_identity.txt", std::ios::binary | std::ios::trunc);
    if (f) f << t;
}

// A9: the party's installed game hooks (coop_hooks), diagnostics (logs, probes, traces, test
// fixtures) left out: every peer must run the same gameplay patch set.
void hook_identity(LinkConfig& cfg) {
    std::vector<std::string> names;
    for (const std::string& n : coop::InstalledHooks())
        if (!diagnostic_hook_name(n)) names.push_back(n);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    cfg.hooks_hash = hook_set_hash(names);
    cfg.hook_names = std::move(names);
}

// The gameplay patches (BB_PARTY_HASH_PATCHES=0: not compared): out/party_patch_hash.txt, which
// scripts/patches.py writes next to patches.bin with the hash and names of the patches that are
// not cosmetic (graphics, FPS, resolution ... do not count). Without it (an older patches.py) the
// whole patches.bin is hashed, as before. Fills cfg.patches_hash / patch_names.
void patch_identity(LinkConfig& cfg, std::string* what) {
    if (env("BB_PARTY_HASH_PATCHES") && !env_on("BB_PARTY_HASH_PATCHES")) {
        *what += "patches not compared (BB_PARTY_HASH_PATCHES=0)";
        return;
    }
    const fs::path out = fs::path(data_dir()) / "out";
    std::string data;
    if (read_file(out / "party_patch_hash.txt", &data) &&
        parse_identity_file(data, "patch", &cfg.patches_hash, &cfg.patch_names)) {
        *what += std::to_string(cfg.patch_names.size()) + " gameplay patches";
        return;
    }
    if (read_file(out / "patches.bin", &data)) {
        const std::string blob = std::string("bbparty-patches-bin\0", 20) + data;
        cfg.patches_hash = crypto::blake2b256(blob.data(), blob.size());
        cfg.patch_names = {"patches.bin (all patches: no party_patch_hash.txt)"};
        *what += "patches.bin " + std::to_string(data.size()) + " B (no party_patch_hash.txt)";
        return;
    }
    *what += "no patches";
}

// The gameplay mods: out/party_mods.txt from scripts/mods.py (the enabled mods' winning files that
// are not cosmetic - textures, shaders, sound, fonts ... do not count; ReShade-type files outside
// dvdroot_ps4 never do). No file (mods.py did not run or failed: no mods applied) or mods off:
// none. Fills cfg.mods_hash / mod_names.
void mod_identity(LinkConfig& cfg, std::string* what) {
    if (env("BB_MODS_ENABLED") && !env_on("BB_MODS_ENABLED")) {
        *what += ", mods off";
        return;
    }
    std::string data;
    const fs::path f = fs::path(data_dir()) / "out" / "party_mods.txt";
    if (read_file(f, &data) && parse_identity_file(data, "mod", &cfg.mods_hash, &cfg.mod_names)) {
        *what += cfg.mod_names.empty() ? ", no gameplay mods" : ", " + std::to_string(cfg.mod_names.size()) + " gameplay mods";
        return;
    }
    *what += ", no party_mods.txt (no mods applied)";
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : ", ") + x;
    return s.empty() ? "none" : s;
}

// ---- state --------------------------------------------------------------------------------

struct Pump {
    std::string name;
    int slot = 0;
    std::atomic<bool> stop{false};
};

struct Runtime {
    std::mutex mu;
    std::condition_variable cv;
    RuntimeStatus st;
    bool stopping = false;
    bool shut = false;
    PartyLink* link = nullptr;  // never deleted (the director may hold it)
    std::unique_ptr<UpnpMapper> upnp;
    std::map<std::string, std::shared_ptr<Pump>> pumps;  // host: per guest online id
    bool rpc_wired = false;
    // A fresh link after Leave (rejoin_main): the host dropped our NP context and relay
    // registration with the old member, so they are registered again once connected.
    bool fresh_link = false;
    bool have_local_state = false;
    MemberState local_state = MemberState::Title;
    std::uint32_t local_map = 0;
    std::string roster_sig;
    Clock::time_point last_roster_log{};
    std::string marker;  // <user>/party_state.json while this run is up
    // Leave / Rejoin (party_status_bridge.h): the config the link was made with.
    LinkConfig cfg;
    bool have_cfg = false;
    bool left = false;
    bool busy = false;  // a Leave / Rejoin is under way
    std::string marker_head;  // its pid / role / name / started members
    bool marker_secret = false;
    std::array<std::uint8_t, 8> secret{};  // host: the party code's secret (kept across a crash)
    // guest
    std::uint32_t host_ip_nbo = 0;
    // Guest: what it joined with, for a readable Auth refusal (bridge::AuthRejectText).
    bool code_coded = false, code_needs_password = false, sent_password = false;
    bool code_ignores_password = false;  // the code carries no password (its key ignores one)
    std::string flow_password_at_start;   // System > Party's password when the link started
    std::uint16_t host_port = 0;
    bool was_connected = false;
    std::uint64_t loss_gen = 0;  // bumps on every link state change (host-lost grace check)
    Clock::time_point lost_at{};
    bool outage = false;  // guest: the link went silent ("no traffic"): the outage hold runs
    std::uint64_t loss_epoch = 0;  // guest: +1 per loss of a connected link (its host-lost timer's)
};

std::mutex g_marker_mu;  // serializes marker writes (startup thread, link callback thread)

Runtime& R() {
    static Runtime* r = new Runtime;  // lives as long as the process (exit paths use it)
    return *r;
}

void set_error(const std::string& e) {
    {
        std::lock_guard<std::mutex> lk(R().mu);
        R().st.last_error = e;
    }
    bridge::OnRuntimeError(e);
}

std::string roster_text(const std::vector<RosterEntry>& roster, bool with_ping) {
    std::string s;
    for (const RosterEntry& e : roster) {
        if (!s.empty()) s += ", ";
        char b[160];
        if (with_ping) {
            std::snprintf(b, sizeof b, "%s(slot %d, %s%s, map %u, %u ms)", e.name.c_str(), e.slot,
                          member_state_name(e.state), e.connected ? "" : ", lost", e.map_id, e.ping_ms);
        } else {
            std::snprintf(b, sizeof b, "%s(slot %d, %s%s, map %u)", e.name.c_str(), e.slot, member_state_name(e.state),
                          e.connected ? "" : ", lost", e.map_id);
        }
        s += b;
    }
    return s.empty() ? "(empty)" : s;
}

void write_text_file(const fs::path& path, const std::string& text);

std::string hex_of(const std::uint8_t* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

bool unhex(const std::string& t, std::uint8_t* out, std::size_t n) {
    if (t.size() != 2 * n) return false;
    auto v = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    for (std::size_t i = 0; i < n; ++i) {
        const int hi = v(t[2 * i]), lo = v(t[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    return true;
}

// The crash marker <user>/party_state.json: pid, role, name, start time and, on the host, the
// party secret and the member table (slot, name, resume token), so a restarted host keeps its
// party code and gives every member its slot back. Rewritten on every member change.
void write_marker() {
    Runtime& r = R();
    std::lock_guard<std::mutex> mk(g_marker_mu);
    std::string path, head;
    bool with_secret;
    std::array<std::uint8_t, 8> secret;
    PartyLink* link;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        if (r.shut || r.left || r.marker.empty()) return;  // left: no crash rejoin
        path = r.marker;
        head = r.marker_head;
        with_secret = r.marker_secret;
        secret = r.secret;
        link = r.link;
    }
    std::string body = "{" + head;
    if (with_secret) body += ",\"secret\":\"" + hex_of(secret.data(), secret.size()) + "\"";
    if (link && link->is_host()) {
        body += ",\"members\":[";
        bool first = true;
        for (const KeptMember& k : link->kept_members()) {
            body += std::string(first ? "" : ",") + "{\"slot\":" + std::to_string(k.slot) + ",\"name\":\"" + k.name +
                    "\",\"token\":\"" + hex_of(k.token.data(), k.token.size()) + "\"}";
            first = false;
        }
        body += "]";
    }
    body += "}\n";
    write_text_file(path, body);
}

// ---- host: event pumps --------------------------------------------------------------------

void pump_main(std::shared_ptr<Pump> p, PartyLink* link) {
    bbnet::party::PartyHostService& svc = bbnet::party::PartyHostService::instance();
    std::uint64_t cursor = 0;
    while (!p->stop.load()) {
        std::vector<json::Value> evs = svc.wait_events(p->name, cursor, 1000);
        if (p->stop.load()) break;
        std::uint64_t last = cursor;
        for (const json::Value& ev : evs) {
            const auto id = static_cast<std::uint64_t>(bbnet::party::int_of(ev, "EventId", 0));
            if (id && id <= cursor) continue;
            // PartyLink owns delivery from here (queued and replayed for a lost member).
            link->send_event(p->slot, bbnet::party::str_of(ev, "Name"), json::dump(ev, 0));
            if (id > last) last = id;
        }
        if (last > cursor) {
            cursor = last;
            svc.ack_events(p->name, cursor);
        }
    }
}

void pump_start(const RosterEntry& m) {
    Runtime& r = R();
    std::lock_guard<std::mutex> lk(r.mu);
    auto it = r.pumps.find(m.name);
    if (it != r.pumps.end()) {
        if (it->second->slot == m.slot && !it->second->stop.load()) return;  // resumed: keep its cursor
        it->second->stop = true;
    }
    auto p = std::make_shared<Pump>();
    p->name = m.name;
    p->slot = m.slot;
    r.pumps[m.name] = p;
    PartyLink* link = r.link;
    std::thread(pump_main, p, link).detach();
}

void pump_stop(const std::string& name) {
    Runtime& r = R();
    {
        std::lock_guard<std::mutex> lk(r.mu);
        auto it = r.pumps.find(name);
        if (it == r.pumps.end()) return;
        it->second->stop = true;
        r.pumps.erase(it);
    }
    bbnet::party::PartyHostService::instance().wake_all();
}

// ---- callbacks ----------------------------------------------------------------------------

// Guest: the host is gone - its game crashed or quit without BYE (the link dropped and did not
// come back within the grace), or it restarted and does not know us (WELCOME not resumed). The
// game's room with it ends the way a host leaving ends it (np party_host_lost: ROOM_DESTROYED),
// so the game goes home on its own; the link keeps reconnecting and the director rings again.
int host_lost_grace_ms() {
    const char* v = env("BB_PARTY_HOST_LOST_MS");
    return v ? std::max(0, std::atoi(v)) : 2000;
}

// "no traffic for 10 s": the link went silent (a network outage, a frozen game), not a reset / BYE.
bool silent_loss(const std::string& why) {
    return why.rfind("no traffic", 0) == 0;
}

// F5: how long a guest keeps the host's world while its link is silent (a network outage, not a
// reset / BYE / refusal): the game session outlives the outage (live: 60, 90 and 120 s kept the
// session with no loading screen), and the host keeps the slot 130 s. The link calls the host lost
// 10 s into the silence, so the guest waits for outages up to ~135 s. BB_PARTY_OUTAGE_HOLD_MS (0 =
// the old 2 s grace; 0.6.2 before parlast: 45 s).
int outage_hold_ms() {
    const char* v = env("BB_PARTY_OUTAGE_HOLD_MS");
    return v ? std::max(0, std::atoi(v)) : 125000;
}

void host_lost(const std::string& why);

// F5: the game's session would end after 25 s of silence (its own timeout, live: both games at
// 25.0 s into a 30 s outage); while the link says the peer is only temporarily lost, the game hears
// that peer's last keepalive again (bbnet::p2p_outage_keepalive). BB_PARTY_OUTAGE_KEEPALIVE=0: off.
bool outage_keepalive_on() {
    const char* v = env("BB_PARTY_OUTAGE_KEEPALIVE");
    return !(v && v[0] == '0');
}

// F5, guest: during the outage hold. If our game ends the host's session anyway (its own silence
// timeout), it would stand on in the host's world without a session until the game's room-gone
// path sends it home ~15 s later (live): home at once instead (one loading screen, while the
// network is still down), and the bell / direct summon brings it back once the link is.
void outage_watch(std::uint64_t epoch) {
    std::thread([epoch] {
        const bool was_in = bbnet::np::party_guest_in_room();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            {
                std::lock_guard<std::mutex> lk(R().mu);
                if (R().loss_epoch != epoch || R().st.state == LinkState::Connected) return;
            }
            if (was_in && !bbnet::np::party_guest_in_room()) {
                bbnet::p2p_outage_keepalive(false);
                plog("outage: our game ended the host's session itself (its silence timeout): home now; the bell / "
                     "direct summon brings us back once the network is back");
                coop::RequestGuestSendHome("the game ended the session during the outage");
                return;
            }
        }
    }).detach();
}

// F5, guest: the link is back after an outage. Our game is either still in the host's session
// (the host's game traffic flows again within a second: it survived), out of it (it ended the
// session itself: the bell / direct summon brings us back), or half in (our game still holds the
// room but no host traffic comes: the host's game dropped us) - that one goes home cleanly now
// (correct over seamless) instead of a phantom in a world that does not know it.
void outage_resume_check() {
    std::thread([] {
        const auto t0 = Clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(R().mu);
                if (R().st.state != LinkState::Connected) return;  // lost again: the hold logic runs anew
            }
            const double s = std::chrono::duration<double>(Clock::now() - t0).count();
            if (!bbnet::np::party_guest_in_room()) {
                plog("outage over: our game is not in the host's session (it ended it itself during the outage); "
                     "the bell / direct summon brings us back");
                return;
            }
            const std::int64_t age = bbnet::p2p_game_rx_age_ms();
            if (age >= 0 && age < 1500) {
                plog("outage over: the game session survived (the host's game traffic is back %.1f s after the link)", s);
                return;
            }
            if (s >= 12.0) {
                plog("outage over: still in the host's room but no game traffic from it (last %.1f s ago): the "
                     "host's game dropped us",
                     double(age) / 1000.0);
                host_lost("the game session did not survive the outage");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }).detach();
}

// Host crash recovery (0.6.2): a guest whose host is gone (its game crashed: the link dropped and did
// not come back within the grace or the outage hold, or it came back as a new session) waits for it
// instead of giving up: it is home in its own world (one loading screen), the link keeps trying
// every ~2 s, the status line and the corner notice say "Waiting for the host to come back
// (N s)...", and once the host's crash restart (run.bat, BB_PARTY_RESTARTED) has its world up again
// the direct summon brings the guest back with one more load. The wait ends when the guest is in the
// host's room again or the host's own roster entry says its world is up (a boss fight or cutscene
// on the host's side then holds the summon as usual, A11). After BB_PARTY_HOST_WAIT_S (180 s; 0 = no
// limit) without the host it stops waiting: a notice, and the party is left (System > Party >
// Rejoin the host joins again).
int host_wait_cap_s() {
    const char* v = env("BB_PARTY_HOST_WAIT_S");
    return v && v[0] ? std::max(0, std::atoi(v)) : 180;
}

std::atomic<bool> g_host_waiting{false};

// The host is back: its link is up and its world is (its roster entry), or we are in its room.
bool host_world_back(PartyLink* link) {
    if (bbnet::np::party_guest_in_room()) return true;
    if (!link || link->state() != LinkState::Connected) return false;
    for (const RosterEntry& e : link->roster())
        if (e.slot == kHostSlot && e.connected &&
            (e.state == MemberState::Home || e.state == MemberState::InHostWorld || e.state == MemberState::Prologue))
            return true;
    return false;
}

void host_wait() {
    if (g_host_waiting.exchange(true)) return;  // one wait at a time (a second loss joins it)
    std::thread([] {
        const int cap = host_wait_cap_s();
        Clock::time_point lost_at;
        {
            std::lock_guard<std::mutex> lk(R().mu);
            lost_at = R().lost_at;
        }
        // Counted from the loss, unless that was long ago (a Rejoin after Leave also meets a host
        // that does not know our session): then from now.
        if (lost_at == Clock::time_point{} || Clock::now() - lost_at > std::chrono::seconds(120)) lost_at = Clock::now();
        plog("host gone: waiting for it to come back (its crash restart)%s; the link keeps trying",
             cap ? (" up to " + std::to_string(cap) + " s").c_str() : "");
        int shown = -1;
        bool link_back = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            LinkState state;
            bool left;
            PartyLink* link;
            {
                std::lock_guard<std::mutex> lk(R().mu);
                state = R().st.state;
                left = R().left;
                link = R().link;
            }
            const double s = std::chrono::duration<double>(Clock::now() - lost_at).count();
            if (left || state == LinkState::Rejected || state == LinkState::Stopped) {  // Leave, refused, ended
                plog("host wait: over after %.1f s (%s)", s, left ? "left the party" : "the link stopped");
                break;
            }
            if (state == LinkState::Connected && !link_back) {
                link_back = true;
                plog("host wait: the host's game answers again %.1f s after it was lost; waiting for its world", s);
            }
            if (host_world_back(link)) {
                plog("host wait: the host is back %.1f s after it was lost; its summon brings us to its world", s);
                status::SetLastEvent("The host is back: joining it");
                break;
            }
            if (cap && s >= cap) {
                plog("host wait: the host did not come back within %d s: leaving the party (System > Party > "
                     "Rejoin the host joins again)",
                     cap);
                bridge::OnHostWait(-1);
                status::SetAlert(bridge::HostGoneNotice(cap));
                status::SetLastEvent("The host did not come back");
                status::RequestLeave();
                g_host_waiting = false;
                return;
            }
            if (static_cast<int>(s) != shown) {
                shown = static_cast<int>(s);
                bridge::OnHostWait(shown);
                if (shown % 30 == 0) plog("host wait: %d s", shown);
            }
        }
        bridge::OnHostWait(-1);
        g_host_waiting = false;
    }).detach();
}

void host_lost(const std::string& why) {
    bbnet::p2p_outage_keepalive(false);
    // Only a guest in the host's room goes home. Regression run 0.6.1 (live): the link dropped while
    // the guest was still at its title; the request stayed pending and sent the guest home 10 s
    // later, the moment its (new) title join reached the host's world.
    if (bbnet::np::party_guest_in_room()) {
        coop::RequestGuestSendHome(why);  // home at once, not the game's room-gone notice 10 s later
    }
    if (bbnet::np::party_host_lost(why)) plog("host lost (%s): the game leaves the host's world", why.c_str());
    else plog("host lost (%s): not in the host's world", why.c_str());
}

void on_state(LinkState s, const std::string& detail) {
    Runtime& r = R();
    PartyLink* link;
    bool host;
    std::uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.st.state = s;
        link = r.link;
        host = r.st.role == RuntimeRole::Host;
        gen = ++r.loss_gen;
    }
    if (!link) return;
    std::string reason = link->reject_reason();
    if (!host && s == LinkState::Rejected && link->reject_code() == RejectCode::Auth) {
        std::lock_guard<std::mutex> lk(r.mu);
        reason = bridge::AuthRejectText(r.code_coded, r.code_needs_password, r.sent_password);
    }
    if (s == LinkState::Rejected && !host) plog("refused by the host: %s", reason.c_str());
    // The host ended the party (BYE, Shutdown): no refusal notice, but the game must not keep
    // asking a host that is gone either: live parx s0a, 17 s after the host's Leave the guest's
    // game dropped offline ("WanderingGhostGet failed 3 times": "Failed to connect to Bloodborne
    // server. Returning to title menu"). It plays online alone too, until a Rejoin.
    if (s == LinkState::Rejected && !host &&
        (bridge::RefusalPlaysSolo(link->reject_code()) || link->reject_code() == RejectCode::Shutdown)) {
        // The game must not fail its server checks ("Failed to acquire Bloodborne server
        // information" covered our message): it plays online alone in its own world, answered
        // by this process's own host service, until a Rejoin gets a link up.
        if (bbnet::party::RemoteGuest* rg = bbnet::party::remote_guest(); rg && rg->set_solo(true)) {
            plog("refused: playing online alone in this world (System > Party > Rejoin the host retries)");
            bbnet::session::host_session_reset();  // a context the game already started: our own service now
        }
    }
    bridge::OnLinkState(s, detail, link->reject_code(), reason);
    if (!host && s == LinkState::Rejected && link->reject_code() == RejectCode::Auth) {
        // A1 live test aid: BB_PARTY_TEST_REJOIN_PASSWORD=<password>: 20 s after a wrong-password
        // refusal, the player "types" this password in System > Party and chooses Rejoin the host.
        static std::atomic<bool> used{false};
        const char* pw = std::getenv("BB_PARTY_TEST_REJOIN_PASSWORD");
        if (pw && pw[0] && !used.exchange(true)) {
            const std::string fixed = pw;
            std::thread([fixed] {
                std::this_thread::sleep_for(std::chrono::seconds(20));
                {
                    std::lock_guard<std::mutex> lk(R().mu);
                    R().cfg.password = fixed;
                }
                plog("test: the right password set (as typed in System > Party); Rejoin the host -> %d", runtime_rejoin());
            }).detach();
        }
    }
    if (!host && (s == LinkState::Reconnecting || s == LinkState::Rejected)) {
        bool first = false;
        std::uint64_t epoch = 0;
        {
            std::lock_guard<std::mutex> lk(r.mu);
            if (r.was_connected) {
                first = true;
                r.was_connected = false;
                r.lost_at = Clock::now();
                epoch = ++r.loss_epoch;
            }
        }
        if (first) {
            // F5: a silent link (an outage) keeps the host's world for the outage hold: the link
            // resumes by itself (the host keeps our slot 130 s) and the game session may have
            // survived (outage_resume_check). A reset, BYE or refusal: home after the short grace.
            // An error (reset, send failed) after the host's game traffic had already stopped for 5 s
            // is the host giving up on a silent link while the network is still out (a real crash
            // resets the link while game traffic still flows): an outage too. Live (loopback sim):
            // the host's link timed out first and its close reached us as "send failed".
            const std::int64_t game_quiet = bbnet::p2p_game_rx_age_ms();
            const bool silent = s == LinkState::Reconnecting && (silent_loss(detail) || game_quiet >= 5000) &&
                                outage_hold_ms() > 0;
            const int grace = s == LinkState::Rejected ? 0 : silent ? outage_hold_ms() : host_lost_grace_ms();
            if (silent) {
                {
                    std::lock_guard<std::mutex> lk(r.mu);
                    r.outage = true;
                }
                plog("link silent (%s; host game traffic %.1f s ago): we stay in the host's world up to %d s for the "
                     "network to come back",
                     detail.c_str(), double(game_quiet) / 1000.0, grace / 1000);
                if (outage_keepalive_on()) bbnet::p2p_outage_keepalive(true);
                outage_watch(epoch);
            }
            std::thread([gen, grace, detail, epoch] {
                std::this_thread::sleep_for(std::chrono::milliseconds(grace));
                {
                    std::lock_guard<std::mutex> lk(R().mu);
                    if (R().loss_gen != gen && R().st.state == LinkState::Connected) return;  // back already
                    if (R().loss_epoch != epoch) return;  // back and lost again since: that loss's own timer
                }
                host_lost("party link down: " + detail);
                bool lost;
                {
                    std::lock_guard<std::mutex> lk(R().mu);
                    const LinkState st = R().st.state;
                    lost = !R().left && st != LinkState::Rejected && st != LinkState::Stopped;
                }
                if (lost) host_wait();  // a refusal / the host ending the party: no wait
            }).detach();
        }
    }
    switch (s) {
    case LinkState::Connected: {
        const int slot = link->local_slot();
        {
            std::lock_guard<std::mutex> lk(r.mu);
            r.st.local_slot = slot;
            r.st.last_error.clear();
        }
        plog("connected to the host as slot %d (we are seen at %s)%s%s", slot, link->observed_address().c_str(),
             detail.empty() ? "" : ": ", detail.c_str());
        const bool resumed = link->last_welcome_resumed();
        bool again, fresh, outage;
        Clock::time_point lost_at;
        {
            std::lock_guard<std::mutex> lk(r.mu);
            again = r.rpc_wired;
            fresh = r.fresh_link;
            r.fresh_link = false;
            lost_at = r.lost_at;
            r.was_connected = true;
            outage = r.outage;
            r.outage = false;
        }
        if (again) {
            plog("back in the party after %.1f s%s", std::chrono::duration<double>(Clock::now() - lost_at).count(),
                 resumed ? " (session resumed)" : " (the host restarted: a new session)");
            if (!resumed) {
                if (bbnet::party::RemoteGuest* rg = bbnet::party::remote_guest()) rg->reset_event_cursor();
                // It no longer knows our NP context or relay registration either.
                bbnet::session::host_session_reset();
                host_lost("the host does not know our session any more");
                host_wait();  // its world is not up yet: the guest waits for it at home
            } else if (outage) {
                bbnet::p2p_outage_keepalive(false);
                outage_resume_check();
            }
        }
        bbnet::party::RemoteGuest* rg = bbnet::party::remote_guest();
        // Refused before, now in: the host's service answers again (registered below: fresh).
        if (rg && rg->set_solo(false)) fresh = true;
        bool wire = false;
        {
            std::lock_guard<std::mutex> lk(r.mu);
            wire = !r.rpc_wired;
            r.rpc_wired = true;
        }
        if (rg && wire) {
            rg->set_rpc([link](const char* kind, const std::string& req, std::string& reply, std::string& error,
                               int timeout_ms) {
                std::string out;
                if (!link->rpc_call(kind, req, &out, timeout_ms)) {
                    error = out.empty() ? std::string("party link: ") + kind + " failed" : out;
                    return false;
                }
                reply = std::move(out);
                return true;
            });
            const bool lb = bbnet::party::ip_is_loopback(r.host_ip_nbo);
            rg->set_host_endpoint(r.host_ip_nbo, r.host_port, lb);
            bbnet::session::set_stun_server(bbnet::party::ip_text(r.host_ip_nbo), r.host_port);
            plog("transport: RPC and events over the party link; STUN at %s:%u",
                 bbnet::party::ip_text(r.host_ip_nbo).c_str(), r.host_port);
        } else if (!rg) {
            plog("warning: this process has no RemoteGuest transport (BB_PARTY_HOST set on a guest?)");
        }
        if (fresh) {
            // Join party after Leave (live: the host's game could not resolve us, 'not registered',
            // and its summons never arrived): our NP context and relay registration again
            // (after the RPC is wired to the new link).
            plog("rejoined after Leave: registering our session with the host again");
            bbnet::session::host_session_reset();
        }
        break;
    }
    case LinkState::Connecting: break;  // PartyLink logs it (on_log)
    case LinkState::Reconnecting: plog("connection lost; reconnecting%s%s", detail.empty() ? "" : ": ", detail.c_str()); break;
    case LinkState::Rejected: {
        const RejectCode code = link->reject_code();
        if (code == RejectCode::Mismatch) {
            // The host names what differs (identity_mismatch): eboot.bin, gameplay patches, mods.
            set_error("host refused: " + link->reject_reason());
            plog("the host refused this game: %s", link->reject_reason().c_str());
            plog("make what is listed the same as the host's: the bbport build, the game dump, the launcher's "
                 "Patches and Mods tabs, the party options; BB_PARTY_HASH_PATCHES=0 / BB_PARTY_CHECK_BUILD=0 on "
                 "the host skip the patch / build comparison");
            break;
        }
        const std::string why = std::string("rejected (") + std::to_string(static_cast<int>(code)) +
                                "): " + link->reject_reason();
        set_error(why);
        plog("%s", why.c_str());
        break;
    }
    default: break;  // hosting / stopped: PartyLink logs it (on_log)
    }
}

// Split travel (playtest 1 bug 20): a member asks "Travel with the party?" (EVENT split_choosing).
void on_split_choosing(int slot, const std::string& body) {
    std::string who;
    if (PartyLink* link = R().link)
        for (const RosterEntry& e : link->roster())
            if (e.slot == slot) who = e.name;
    coop::split::OnChoosingEvent(slot, who, body);
}

void on_roster(const std::vector<RosterEntry>& roster) {
    Runtime& r = R();
    const std::string sig = roster_text(roster, false);
    bool log_it = false;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.st.roster = roster;
        const auto now = Clock::now();
        if (sig != r.roster_sig || now - r.last_roster_log >= std::chrono::seconds(15)) {
            r.roster_sig = sig;
            r.last_roster_log = now;
            log_it = true;
        }
    }
    if (log_it) plog("roster: %s", roster_text(roster, true).c_str());
    // Guest: the host's name, so its signaling_resolve answer gets the link's (LAN) address too.
    if (bbnet::party::RemoteGuest* rg = bbnet::party::remote_guest()) {
        for (const RosterEntry& e : roster)
            if (e.slot == kHostSlot) rg->set_host_online_id(e.name);
    }
    PartyLink* link = runtime_link();
    // C6 on the OTHER guests (confid 2026-10-16, repeat_party c6 --players 3: 2 of 3 crash rejoins
    // failed): a member whose game crashed comes back at its title from the same address and port.
    // The host releases its game connection to the dead game (NrDropSilentPeers from on_member_left),
    // but every other guest's game still held one, and its F5 outage replay sent the dead session's
    // reliable records into the restarted game ("outage resume: ... heard again after 7.9 s: 18
    // reliable record(s) sent again"). The restarted game's title join then got no world data
    // ("title join: FAILED (no snapshot within 12 s)") and ~38 s later the host's game dropped the
    // other guest as a stale member (KickoutRoomMember 0xFF000020). A guest now does what the host
    // does as soon as the roster shows that member back at its title after it was in the world.
    if (link && !link->is_host() && !(env("BB_PARTY_NR_DROP") && env("BB_PARTY_NR_DROP")[0] == '0')) {
        static std::mutex mu;
        static std::map<std::string, MemberState> last;
        static std::set<std::string> lost;  // members whose link dropped while in the world (a crash)
        std::vector<std::string> crashed;
        {
            std::lock_guard<std::mutex> lk(mu);
            for (const RosterEntry& e : roster) {
                if (e.slot == kHostSlot || e.slot == link->local_slot()) continue;
                const auto it = last.find(e.name);
                const bool was_in = it != last.end() && (it->second == MemberState::InHostWorld ||
                                                         it->second == MemberState::Joining ||
                                                         it->second == MemberState::Loading ||
                                                         it->second == MemberState::Dead);
                // smokefix (live smoke, 2026-10-16): a guest's own Leave + Rejoin also shows it at
                // its title for a moment; releasing the connection to its LIVE game then made the
                // host's game drop this guest 15 s later (KickoutRoomMember 0xFF000020). Only a
                // member whose link was lost (its game died) is released.
                if (!e.connected && was_in) lost.insert(e.name);
                if (e.state == MemberState::Title && lost.count(e.name)) {
                    crashed.push_back(e.name);
                    lost.erase(e.name);
                } else if (e.connected && e.state == MemberState::InHostWorld) {
                    lost.erase(e.name);  // back in the world without a restart (an outage resumed)
                }
                last[e.name] = e.state;
            }
        }
        for (const std::string& name : crashed) {
            plog("C6 (guest): %s is back at its title after being in the world (a restart): releasing our "
                 "game's connection to its old game", name.c_str());
            coop::b2::NrDropSilentPeers(name, 0);
        }
    }
    bridge::OnRoster(roster, link ? link->max_players() : 0);
}

// F5, host: why each member's link went last (on_member_left), for its return.
std::mutex g_lost_mu;
std::map<std::string, std::string> g_lost_why;  // g_lost_mu

// F5, host: the game hears silent members' last keepalive while any member's link is lost by
// silence with its slot kept (an outage), so its session's 25 s timeout does not drop them.
void host_outage_keepalive_update() {
    bool any = false;
    {
        std::lock_guard<std::mutex> lk(g_lost_mu);
        for (const auto& [name, why] : g_lost_why) any = any || silent_loss(why);
    }
    bbnet::p2p_outage_keepalive(any && outage_keepalive_on());
}

LinkCallbacks make_callbacks(bool host) {
    LinkCallbacks cb;
    cb.on_state = on_state;
    cb.on_roster = on_roster;
    cb.on_log = [](const std::string& line) { plog("link: %s", line.c_str()); };
    if (host) {
        cb.on_member_joined = [](const RosterEntry& m, bool rejoined) {
            plog("%s %s (slot %d)", m.name.c_str(), rejoined ? "is back" : "joined", m.slot);
            std::string why;
            {
                std::lock_guard<std::mutex> lk(g_lost_mu);
                auto it = g_lost_why.find(m.name);
                if (it != g_lost_why.end()) {
                    why = it->second;
                    g_lost_why.erase(it);
                }
            }
            host_outage_keepalive_update();
            // F5: back after a network outage while its sign hold runs: if its game left our session
            // meanwhile, our game's connection to it is released now, and the hold ends either way,
            // so its first direct summon is not refused (party_b2 NrDropAfterOutage).
            if (rejoined && silent_loss(why) && bbnet::party::FromApi::instance().hold_left_ms() > 0 &&
                !(env("BB_PARTY_NR_DROP") && env("BB_PARTY_NR_DROP")[0] == '0'))
                coop::b2::NrDropAfterOutage(m.name);
            bridge::OnMemberJoined(m.name, rejoined);
            pump_start(m);
            write_marker();
            coop::RequestHostFullItems(m.slot);  // C3: everything the host has, for its own save
            coop::StoryMemberJoined();           // C4 catch-up: the host's time of day (Blood Moon)
        };
        cb.on_member_left = [](const RosterEntry& m, bool slot_kept) {
            bridge::OnMemberLeft(m.name, slot_kept);
            {
                std::lock_guard<std::mutex> lk(g_lost_mu);
                if (slot_kept) g_lost_why[m.name] = m.lost_why;
                else g_lost_why.erase(m.name);
            }
            host_outage_keepalive_update();
            // Its signs go too (a crashed game cannot answer a summon; with 2-3 guests the host's
            // game would otherwise pick a dead sign while the others wait). The restart puts up a new one.
            bbnet::party::FromApi::instance().drop_signs(m.name);
            // Fast join: the host's game keeps its transport connection to a member that was in (or
            // joining) its world until the transport times out; a restarted game summoned before
            // that never gets the summon data (docs/party/join_timing.md).
            // A guest's own Leave (BYE, slot released) while its going-home load runs: its game left
            // our room itself (the room's member-left came first), so no old connection is held.
            // Live parx s0a (3 players, 3rd leave/rejoin): that leave took 4.3 s, the BYE arrived
            // during the load, and the 26 s hold refused the rejoin's direct summon (25 s bell
            // path instead of 2 s). A kick (still in our world) or a lost link keeps the hold.
            const bool own_leave_home = !slot_kept && m.lost_why.empty() && m.state == MemberState::Loading &&
                                        bbnet::np::ms_since_member_left() < 10000;
            if (own_leave_home) {
                plog("%s left with its own Leave during its going-home load (left our room %lld ms ago): no sign hold",
                     m.name.c_str(), static_cast<long long>(bbnet::np::ms_since_member_left()));
            }
            if (!own_leave_home && (m.state == MemberState::InHostWorld || m.state == MemberState::Joining ||
                m.state == MemberState::Loading)) {
                // A crash (RST): the connection is expired early (nr_fast_expire), so a short hold.
                const bool crashed = m.lost_why.find("reset") != std::string::npos && env_on("BB_PARTY_NR_FAST_EXPIRE");
                bbnet::party::FromApi::instance().hold_signs(m.name, 26000);
                // Experimental (BB_PARTY_CRASH_LEAVE=1, off): on a crash (TCP reset) the member's
                // room membership ends at once so no game keeps its frozen character (~6-8 s
                // otherwise). Live 2026-10-12: the host's game then froze ~20 s later (2 runs).
                if (m.lost_why.find("reset") != std::string::npos && env_on("BB_PARTY_CRASH_LEAVE"))
                    std::thread([name = m.name] {  // not on the link's callback thread (lock order)
                        bbnet::party::PartyHostService::instance().member_crashed(name);
                    }).detach();
                if (crashed) nr_fast_expire(m.name);  // shortens that hold once the timeout is lowered
                // A crash: the host's game releases its connection to the dead game now (C6).
                if (m.lost_why.find("reset") != std::string::npos && !(env("BB_PARTY_NR_DROP") && env("BB_PARTY_NR_DROP")[0] == '0'))
                    coop::b2::NrDropSilentPeers(m.name);
            }
            if (slot_kept) {
                plog("%s (slot %d) lost; slot kept", m.name.c_str(), m.slot);
                return;
            }
            plog("%s (slot %d) left", m.name.c_str(), m.slot);
            write_marker();
            pump_stop(m.name);
            bbnet::party::PartyHostService::instance().context_gone(m.name);
        };
        cb.on_event = [](int slot, std::uint64_t, const std::string& name, const std::string& body) {
            if (name == coop::split::kChoosingEventName) {  // a guest asks "Travel with the party?"
                on_split_choosing(slot, body);
                return;
            }
            if (name == coop::b2::kEventName && body.find("\"dsum_") != std::string::npos) {
                if (PartyLink* link = R().link)
                    for (const RosterEntry& e : link->roster())
                        if (e.slot == slot) coop::b2::DirectSummonName(slot, e.name);
            }
            if (name == coop::b2::kEventName) coop::b2::OnLinkEvent(slot, body);  // B2: a guest arrived
            if (name == coop::chalice::kFormEventName) {  // a guest's ritual dungeon (party_chalice.h)
                std::string member;
                if (PartyLink* link = R().link) {
                    for (const RosterEntry& e : link->roster())
                        if (e.slot == slot) member = e.name;
                }
                coop::chalice::OnGuestForm(slot, member, body);
                return;
            }
            if (name == coop::chalice::kGoodsEventName) {  // a guest's chalices (party_chalice.h)
                std::vector<std::uint32_t> ids;
                std::string err;
                if (coop::chalice::GoodsFromJsonText(body, &ids, &err)) coop::chalice::RequestGuestGoods(ids);
                else plog("%s event from slot %d: %s", name.c_str(), slot, err.c_str());
                return;
            }
            if (name == coop::desync::kCensusAskEvent) {  // a guest's enemy census differs from the digest
                std::string err;
                if (!coop::desync::HostQueueCensusAsk(slot, body, &err)) plog("census_ask from slot %d refused: %s", slot, err.c_str());
                return;
            }
            if (name == coop::desync::kResyncEvent) {  // F5: a guest's state differs from the host's digest
                std::string err;
                if (!coop::desync::HostQueueResync(slot, body, &err)) plog("sync_resync from slot %d refused: %s", slot, err.c_str());
                return;
            }
            if (name == coop::kLampLitEventName) {  // a guest lit a lamp of the host's world
                std::string err;
                if (!coop::GuestServicesOnLampLit(slot, body, &err)) plog("lamp_lit from slot %d refused: %s", slot, err.c_str());
                return;
            }
            if (name == coop::kStoryUpEventName) {  // D10: a guest's cutscene, played for everyone
                std::string err;
                if (!coop::HostStoryFromGuest(slot, body, &err)) plog("story_up from slot %d refused: %s", slot, err.c_str());
                return;
            }
            if (name == coop::kGiftUpEventName) {  // D5: an NPC's gift to a guest; the host takes it too
                std::string err;
                if (!coop::OnHostGiftUp(slot, body, &err)) plog("gift_up from slot %d refused: %s", slot, err.c_str());
                return;
            }
            if (name == coop::npctalk::kEventName) {  // a guest's NPC conversation (D8, party_npctalk.h)
                coop::npctalk::OnEvent(slot, body);
                return;
            }
            if (name == party::ping::kEventName) {  // a guest's ping (I2, party_ping.h): relayed by the tick
                coop::ping::OnEvent(slot, body);
                return;
            }
            if (name == coop::worldact::kEventName) {  // a guest's lever / door / NPC talk (D3, D8)
                std::string err;
                if (!coop::worldact::OnHostEvent(slot, body, &err)) plog("world_action from slot %d refused: %s", slot, err.c_str());
                return;
            }
            if (name == coop::kLampRequestEventName) {  // a guest used a lamp / headstone
                std::string err;
                if (!coop::GuestServicesOnLampRequest(slot, body, &err)) plog("lamp request from slot %d refused: %s", slot, err.c_str());
            }
        };
        cb.on_rpc = [](int slot, const std::string& kind, const std::string& body) -> std::string {
            PartyLink* link = R().link;
            bbnet::party::Caller caller;
            if (link) {
                for (const RosterEntry& e : link->roster())
                    if (e.slot == slot) caller.online_id = e.name;
                caller.link_addr = link->member_ip(slot);
            }
            json::Value req, reply;
            std::string err;
            if (caller.online_id.empty()) {  // no authenticated roster name: it cannot act as anyone
                reply = json::Value::make_object();
                reply.set("ResKind", 7);
                reply.set("Error", "caller not authenticated");
                return json::dump(reply, 0);
            }
            if (!json::parse(body, req, err)) {
                reply = json::Value::make_object();
                reply.set("ResKind", 1);
                reply.set("Error", "bad request: " + err);
                return json::dump(reply, 0);
            }
            if (bbnet::party_trace()) plog("rpc from %s: %s %s", caller.online_id.c_str(), kind.c_str(), body.c_str());
            bbnet::party::host_handle(caller, kind, req, reply);
            return json::dump(reply, 0);
        };
    } else {
        // A6: the host's Leave closes its game's room before its link stops (BYE ~0.4 s later):
        // the room's end sends us home at once, not the game's "Session lost" 15-19 s later.
        bbnet::np::set_host_closed_room_hook(&host_lost);
        cb.on_event = [](int slot, std::uint64_t, const std::string& name, const std::string& body) {
            if (name == coop::split::kChoosingEventName) {  // the host asks "Travel with the party?"
                on_split_choosing(slot, body);
                return;
            }
            if (name == coop::kHostSessionEndEvent) {  // F5: the host's game ended the session itself
                if (bbnet::np::party_guest_in_room()) host_lost("the host's game ended the session");
                else plog("host_session_end: not in the host's world");
                return;
            }
            if (name == coop::kTravelEventName) {  // B1: follow the host's warp
                coop::TravelIntent t;
                std::string err;
                if (coop::TravelFromJsonText(body, &t, &err) && coop::SanitizePeerTravel(&t, &err)) {
                    bridge::OnTravel(coop::TravelKindLabel(t.kind));
                    if (!coop::b2::OnHostTravel(t)) coop::RequestGuestTravel(t);  // B2 keeps the room
                } else {
                    plog("travel event: %s", err.c_str());
                }
                return;
            }
            if (name == coop::b2::kEventName) {  // B2: a guest arrived in the host world
                if (body.find("\"dsum_") != std::string::npos)
                    if (PartyLink* link = R().link)
                        for (const RosterEntry& e : link->roster())
                            if (e.slot == slot) coop::b2::DirectSummonName(slot, e.name);
                coop::b2::OnLinkEvent(slot, body);
                return;
            }
            if (name == coop::balance::kEventName) {  // the host's party rules (party_balance.h)
                std::string err;
                if (!coop::balance::OnHostRules(body, &err)) plog("balance event refused: %s", err.c_str());
                return;
            }
            if (name == coop::kStoryNoticeEventName) {  // D10: the host's story notice
                coop::StoryNotice n;
                std::string err;
                if (coop::StoryNoticeFromJsonText(body, &n, &err)) coop::RequestGuestStoryNotice(n);
                else plog("story notice refused: %s", err.c_str());
                return;
            }
            if (name == coop::kStoryEventName) {  // C4: the host's cutscene / ending
                coop::StoryIntent s;
                std::string err;
                if (coop::StoryFromJsonText(body, &s, &err) && coop::SanitizePeerStory(&s, &err)) coop::RequestGuestStory(s);
                else plog("story event refused: %s", err.c_str());
                return;
            }
            if (name == coop::desync::kCensusListEvent) {  // the host's census entries asked for
                std::string err;
                if (!coop::desync::GuestOnCensusList(body, &err)) plog("census_list event: %s", err.c_str());
                return;
            }
            if (name == coop::progress::kEventFlags || name == coop::progress::kEventFlagSnapshot ||
                name == coop::progress::kEventWorldGroups || name == coop::desync::kDigestEvent) {
                std::string err;  // C2: the host's flags, applied by the main-thread tick
                if (!coop::progress::OnLinkEvent(name, body, &err)) plog("%s event: %s", name.c_str(), err.c_str());
                return;
            }
            if (name == coop::kItemsEventName || name == coop::kItemsFullEventName) {  // C3: the host's items
                std::vector<coop::ItemGrant> items;
                std::string err;
                if (coop::ItemsFromJsonText(body, &items, &err)) {
                    std::size_t rejected = 0;
                    items = coop::FilterPeerItems(items, &rejected);  // only what the tables allow
                    if (rejected) plog("%s event: %zu row(s) refused (not a valid host grant)", name.c_str(), rejected);
                    coop::RequestGuestItems(items);
                } else {
                    plog("%s event: %s", name.c_str(), err.c_str());
                }
                return;
            }
            if (name == coop::chalice::kGoodsEventName) {  // the host's chalices (party_chalice.h)
                std::vector<std::uint32_t> ids;
                std::string err;
                if (coop::chalice::GoodsFromJsonText(body, &ids, &err)) coop::chalice::RequestGuestGoods(ids);
                else plog("%s event: %s", name.c_str(), err.c_str());
                return;
            }
            if (name == coop::droproll::kEventName) {  // D4b: the host's shared enemy drop roll seed
                std::string err;
                if (!coop::droproll::OnEvent(body, &err)) plog("drop_seed event refused: %s", err.c_str());
                return;
            }
            if (name == coop::npctalk::kEventName) {  // a member's NPC conversation (D8, party_npctalk.h)
                coop::npctalk::OnEvent(slot, body);
                return;
            }
            if (name == party::ping::kEventName) {  // a member's ping (I2, party_ping.h)
                coop::ping::OnEvent(slot, body);
                return;
            }
            if (name == coop::kPhantomEventName) {  // host lamp / rested / boss Insight
                coop::PhantomOnEvent(body);
                return;
            }
            bbnet::party::RemoteGuest* rg = bbnet::party::remote_guest();
            const std::uint64_t id = rg ? rg->on_link_event(body) : 0;
            if (bbnet::party_trace()) plog("event %s id %llu", name.c_str(), static_cast<unsigned long long>(id));
        };
    }
    // Party logging (party_log.h): every event either way (BB_PARTY_LOG=verbose / trace) and the
    // log's own events (run ids, user marks), which no other handler sees.
    cb.on_event = [inner = std::move(cb.on_event)](int slot, std::uint64_t cursor, const std::string& name,
                                                   const std::string& body) {
        if (coop::plog::OnEventReceived(slot, name, body)) return;
        if (inner) inner(slot, cursor, name, body);
    };
    cb.on_event_sent = [](int slot, const std::string& name, const std::string& body) {
        coop::plog::OnEventSent(slot, name, body);
    };
    return cb;
}

// ---- crash recovery -----------------------------------------------------------------------

fs::path marker_path() { return fs::path(user_dir()) / "party_state.json"; }

struct RestartInfo {
    bool restarted = false;
    std::string why;
    std::string marker_body;  // the previous run's marker (host: secret, members)
};

const RestartInfo& restart_info() {
    static RestartInfo info;
    static std::once_flag once;
    std::call_once(once, [] {
        if (env("BB_PARTY_RESTARTED") && env_on("BB_PARTY_RESTARTED")) {
            info.restarted = true;
            info.why = "BB_PARTY_RESTARTED=1";
        }
        std::error_code ec;
        const fs::path m = marker_path();
        if (fs::exists(m, ec)) {
            const auto age = fs::file_time_type::clock::now() - fs::last_write_time(m, ec);
            if (!ec && age < std::chrono::minutes(30)) {
                std::string body;
                read_file(m, &body);
                info.marker_body = body;
                while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
                info.restarted = true;
                info.why += std::string(info.why.empty() ? "" : "; ") + "unclean exit marker " + m.string() + " " + body;
            }
        }
    });
    return info;
}

// ---- startup ------------------------------------------------------------------------------

bool stopping() {
    std::lock_guard<std::mutex> lk(R().mu);
    return R().stopping;
}

void write_text_file(const fs::path& path, const std::string& text) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            plog("warning: cannot write %s", path.string().c_str());
            return;
        }
        f << text;
    }
    fs::rename(tmp, path, ec);  // replaces on Windows too (MoveFileEx REPLACE_EXISTING)
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
    }
}

// A restarted host's party secret and members: the crash marker's, or (an older marker) the
// secret of the code it wrote to <user>/party_code.txt. The party code stays the same.
bool previous_party(std::array<std::uint8_t, 8>* secret, std::vector<KeptMember>* members, std::string* from) {
    const RestartInfo& ri = restart_info();
    json::Value v;
    std::string err;
    if (!ri.marker_body.empty() && json::parse(ri.marker_body, v, err) && v.type == json::Value::Type::Object &&
        unhex(bbnet::party::str_of(v, "secret"), secret->data(), secret->size())) {
        *from = "the crash marker";
        if (const json::Value* ms = v.find("members"); ms && ms->type == json::Value::Type::Array) {
            for (const json::Value& m : ms->array) {
                KeptMember k;
                k.slot = static_cast<int>(bbnet::party::int_of(m, "slot", 0));
                k.name = bbnet::party::str_of(m, "name");
                if (!unhex(bbnet::party::str_of(m, "token"), k.token.data(), k.token.size())) k.token = {};
                if (k.slot > 0 && !k.name.empty()) members->push_back(std::move(k));
            }
        }
        return true;
    }
    std::string body;
    if (read_file(fs::path(user_dir()) / "party_secret.txt", &body)) {
        while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back()))) body.pop_back();
        if (unhex(body, secret->data(), secret->size())) {
            *from = "party_secret.txt";
            return true;
        }
    }
    if (read_file(fs::path(user_dir()) / "party_code.txt", &body)) {
        std::istringstream in(body);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            const auto eq = line.find('=');
            const std::string c = eq == std::string::npos ? line : line.substr(eq + 1);
            PartyCode code;
            std::string e;
            if (!c.empty() && decode_party_code(c, &code, &e)) {
                *secret = code.secret;
                *from = "party_code.txt";
                return true;
            }
        }
    }
    return false;
}

void add_guest_code_text(const std::string& text);
void punch_guest_codes_tick();
void attach_udp_path(PartyLink* link);

// ---- LAN discovery (party_lan.h): the host answers same-network probes for its party ----
// BB_PARTY_LAN_DISCOVERY=0 turns both sides off.
struct LanResponder {
    std::mutex mu;
    bool on = false;
    std::vector<crypto::Key> keys;  // the party key; with a password also the password-only key
    std::array<std::uint8_t, 4> ip{};
    std::uint16_t port = 0;
    lan::ReplyLimiter limiter;
    std::uint64_t answered = 0;
};
LanResponder& lan_responder() {
    static LanResponder* l = new LanResponder;
    return *l;
}
bool lan_discovery_enabled() {
    const char* v = env("BB_PARTY_LAN_DISCOVERY");
    if (!v) return true;
    const std::string t = lower(v);
    return !(t == "0" || t == "off" || t == "false" || t == "no");
}
void lan_responder_start(const LinkConfig& cfg, bool lb) {
    if (!lan_discovery_enabled()) {
        plog("LAN discovery: off (BB_PARTY_LAN_DISCOVERY=0)");
        return;
    }
    std::vector<crypto::Key> keys{crypto::derive_party_key(cfg.password, cfg.secret.data())};
    if (!cfg.password.empty()) {
        const std::array<std::uint8_t, 8> no_secret{};
        keys.push_back(crypto::derive_party_key(cfg.password, no_secret.data()));
    }
    std::array<std::uint8_t, 4> ip{127, 0, 0, 1};
    if (!lb && !best_local_ipv4(&ip)) ip = {0, 0, 0, 0};
    LanResponder& l = lan_responder();
    std::lock_guard<std::mutex> lk(l.mu);
    l.keys = std::move(keys);
    l.ip = ip;
    l.port = cfg.port;
    l.on = true;
}
// The party UDP port's reader thread: a probe for our party gets our LAN address and port.
void lan_on_datagram(std::uint32_t a, std::uint16_t p, const std::uint8_t* d, std::size_t n) {
    LanResponder& l = lan_responder();
    std::vector<std::uint8_t> reply;
    std::uint64_t count = 0;
    {
        std::lock_guard<std::mutex> lk(l.mu);
        if (!l.on) return;
        reply = lan::answer_probe(l.keys, d, n, l.ip, l.port, &l.limiter, Clock::now());
        if (reply.empty()) return;
        count = ++l.answered;
    }
    bbnet::p2p_send_raw(a, p, reply.data(), reply.size());
    if (count <= 5 || count % 100 == 0) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(&a);
        plog("LAN discovery: answered a probe from %u.%u.%u.%u:%u (%llu so far)", b[0], b[1], b[2], b[3],
             static_cast<unsigned>(static_cast<std::uint16_t>((p >> 8) | (p << 8))),
             static_cast<unsigned long long>(count));
    }
}

// The host's secret as the BBP3 code carries it (party_code.h): 4 bytes while a password is set
// (the password keeps strangers out), 6 without (the secret alone does). A stored secret of the
// other size is shortened / lengthened, a missing one made: the party key changes with it, so
// friends need the new code once.
void fit_secret_to_code(std::array<std::uint8_t, 8>* secret, bool password) {
    auto& s = *secret;
    const bool empty = std::all_of(s.begin(), s.end(), [](std::uint8_t b) { return b == 0; });
    if (empty) {
        crypto::random_bytes(s.data(), password ? kSecretBytesWithPassword : kSecretBytes);
        return;
    }
    if (s[6] || s[7] || (password && (s[4] || s[5]))) {
        // An 8-byte secret from before BBP2, or a 6-byte one now that a password is set.
        for (std::size_t i = password ? kSecretBytesWithPassword : kSecretBytes; i < s.size(); ++i) s[i] = 0;
        plog("party secret shortened to %u bits for the shorter party code: send your friends the new code",
             static_cast<unsigned>(8 * (password ? kSecretBytesWithPassword : kSecretBytes)));
    } else if (!password && !s[4] && !s[5]) {
        // A 4-byte secret from a party with a password: without one the code is all that keeps
        // strangers out, so it gets its 48 bits back.
        do {
            crypto::random_bytes(s.data() + 4, 2);
        } while (!s[4] && !s[5]);
        plog("party secret lengthened to 48 bits (no password): send your friends the new code");
    }
}

void start_host(LinkConfig cfg) {
    Runtime& r = R();
    const bool lb = loopback_mode();
    std::vector<KeptMember> kept;
    std::string from;
    const bool restarted = restart_info().restarted;
    const std::string menu_secret = flow::RuntimeHostSecret();
    if (restarted && previous_party(&cfg.secret, &kept, &from)) {
        plog("restarted host: keeping the party code (secret from %s) and %zu member slot(s)", from.c_str(), kept.size());
    } else if (!menu_secret.empty() && unhex(menu_secret, cfg.secret.data(), cfg.secret.size())) {
        plog("party game from the menu: the party code of the last session (secret from party.json)");
    } else {
        cfg.secret = {};
    }
    fit_secret_to_code(&cfg.secret, !cfg.password.empty());
    flow::StoreHostSecret(hex_of(cfg.secret.data(), cfg.secret.size()));
    // <user>/party_secret.txt: the restart's second source after the crash marker.
    write_text_file(fs::path(user_dir()) / "party_secret.txt", hex_of(cfg.secret.data(), cfg.secret.size()) + "\n");
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.secret = cfg.secret;
        r.marker_secret = true;
    }
    if (lb) cfg.bind_addr = "127.0.0.1";  // local test: nothing on the network (no firewall prompt)
    const std::string name = cfg.name;
    const std::uint16_t port = cfg.port;

    // The game's party UDP port first: STUN below goes out from it (so the mapping is that
    // port's) and the host's STUN responder / relay is up before any guest asks. The TCP link
    // shares the number, which is fine (different protocol).
    if (!bbnet::p2p_open()) plog("warning: cannot open the party UDP port %u (another program?)", port);

    auto* link = new PartyLink(cfg, make_callbacks(true));
    attach_udp_path(link);
    if (const char* gc = env("BB_PARTY_GUEST_CODES")) add_guest_code_text(gc);
    std::thread([] {  // guest codes: punch towards each new one (env, file, menu)
        while (!stopping()) {
            punch_guest_codes_tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }).detach();
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.link = link;
        r.cfg = cfg;
        r.have_cfg = true;
    }
    if (!kept.empty()) link->restore_members(kept);
    // The party port answers STUN and hands out relay ports only to PartyLink members' addresses
    // (it is on the internet; STUN is a reflector and the relay forwards to members).
    // BB_PARTY_RELAY_OPEN=1: anyone (a guest whose UDP leaves from another address than its TCP).
    if (!env_on("BB_PARTY_RELAY_OPEN")) {
        bbnet::p2p_set_relay_admit([](std::uint32_t addr) {
            if ((addr & 0xff) == 127) return true;
            PartyLink* l = runtime_link();
            if (!l) return false;
            for (const RosterEntry& e : l->roster())
                if (e.slot != kHostSlot && l->member_ip(e.slot) == addr) return true;
            return false;
        });
    }
    std::string err;
    // A restarted host's port can be held a little longer by the dead process's connections:
    // retry for up to 60 s (the guests keep reconnecting meanwhile).
    const auto bind_deadline = Clock::now() + std::chrono::seconds(restarted ? 60 : 0);
    bool up = link->start_host(&err);
    while (!up && Clock::now() < bind_deadline && !stopping()) {
        plog("cannot host on TCP port %u yet (%s); retrying", port, err.c_str());
        std::this_thread::sleep_for(std::chrono::seconds(1));
        up = link->start_host(&err);
    }
    if (!up) {
        set_error("cannot host: " + err);
        plog("cannot host on TCP port %u: %s", port, err.c_str());
        return;
    }
    coop::PartyDirector::Get().SetLink(link);
    write_marker();
    lan_responder_start(cfg, lb);

    bbnet::party::PartyHostService& svc = bbnet::party::PartyHostService::instance();
    svc.set_loading_query([](const std::string& id) {
        PartyLink* l = R().link;
        if (!l) return false;
        for (const RosterEntry& e : l->roster()) {
            if (e.name != id) continue;
            if (e.state == MemberState::Loading || !e.connected) return true;
            // F5: its link has gone silent (no VITALS for kVitalsStaleMs: an outage before the link's
            // 10 s lost timeout): the room's 15 s heartbeat timeout must not end its membership
            // first (our game would drop it while the link still keeps its slot).
            MemberVitals v[kMaxVitals];
            const int n = l->vitals(v, kMaxVitals);
            for (int i = 0; i < n; ++i)
                if (v[i].slot == e.slot) return false;
            return true;
        }
        return false;
    });
    bbnet::party::transport();  // LocalHost over the service

    std::string upnp_ip, upnp_msg;
    bool upnp_ok = false;
    if (lb) {
        upnp_msg = "off (loopback test)";
    } else if (!UpnpMapper::enabled_by_env()) {
        upnp_msg = "off (BB_PARTY_UPNP=0)";
    } else {
        auto m = std::make_unique<UpnpMapper>();
        m->start(port);
        const UpnpResult u = m->wait(3000);
        upnp_ip = u.external_ip;
        upnp_ok = u.ok;
        upnp_msg = u.done ? u.message : "still trying: " + u.message;
        std::lock_guard<std::mutex> lk(r.mu);
        r.upnp = std::move(m);
    }
    plog("UPnP: %s", upnp_msg.c_str());

    // The public address: loopback test / BB_PARTY_PUBLIC_ADDR / STUN from the game's port / UPnP.
    std::array<std::uint8_t, 4> pub{};
    std::uint16_t pub_port = port;
    std::string source;
    // F2: the NAT type (a second STUN server) for the advice line and the traversal chain.
    nat::Type host_nat = nat::Type::Unknown;
    const auto stun_fn = [](const std::string& h, std::uint16_t p, std::uint32_t* a, std::uint16_t* mp) {
        return bbnet::p2p_stun(h.c_str(), p, 2000, a, mp);
    };
    if (lb) {
        pub = {127, 0, 0, 1};
        source = "loopback";
        host_nat = traversal::detect(stun_fn, {}, 0, port, true);
    } else if (const char* ov = env("BB_PARTY_PUBLIC_ADDR"); ov && resolve_ipv4(ov, port, &pub, &pub_port)) {
        source = "override";
    } else {
        const char* stun_env = env("BB_PARTY_STUN");
        const std::string server = stun_env ? stun_env : "stun.l.google.com:19302";
        if (server != "off" && server != "0") {
            std::array<std::uint8_t, 4> sip{};
            std::uint16_t sport = 19302;
            std::string host_part = server;
            if (const auto c = server.rfind(':'); c != std::string::npos) {
                host_part = server.substr(0, c);
                sport = static_cast<std::uint16_t>(std::strtoul(server.c_str() + c + 1, nullptr, 10));
            }
            std::uint32_t ma = 0;
            std::uint16_t mp = 0;
            bool ok = false;
            for (int i = 0; i < 3 && !ok && !stopping(); ++i)
                ok = bbnet::p2p_stun(host_part.c_str(), sport, 2000, &ma, &mp);
            (void)sip;
            if (ok) {
                std::memcpy(pub.data(), &ma, 4);
                pub_port = mp;
                source = "stun " + server;
                if (mp != port) plog("note: the NAT maps UDP %u to %u: guests may need the host relay", port, mp);
                host_nat = traversal::detect(stun_fn, {true, ma, mp}, bbnet::local_ipv4(), port, false);
            } else {
                plog("STUN %s: no answer", server.c_str());
            }
        }
        if (source.empty() && !upnp_ip.empty() && parse_ipv4(upnp_ip, &pub)) source = "upnp";
    }

    std::uint8_t flags = 0;
    if (!cfg.password.empty()) flags |= kCodeFlagPasswordRequired;
    std::string internet, lan;
    if (!source.empty()) internet = make_party_code(pub, port, cfg.secret, flags);
    if (lb) {
        lan = make_party_code({127, 0, 0, 1}, port, cfg.secret, static_cast<std::uint8_t>(flags | kCodeFlagLan));
    } else if (!make_lan_code(port, cfg.secret, flags, &lan)) {
        lan.clear();
    }
    const std::string pub_text = source.empty() ? std::string() : format_ipv4(pub) + ":" + std::to_string(pub_port);
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.st.internet_code = internet;
        r.st.lan_code = lan;
        r.st.public_address = source.empty() ? "unknown" : pub_text + " (" + source + ")";
        r.st.upnp = upnp_msg;
        r.st.local_slot = 0;
    }
    bridge::OnCode(!internet.empty() ? internet : lan);
    bridge::OnLanCode(!internet.empty() ? lan : std::string());  // the second code, when there are two
    plog("hosting on port %u as %s; code (Internet): %s; code (LAN): %s", port, name.c_str(),
         internet.empty() ? "(no public address: set BB_PARTY_PUBLIC_ADDR)" : internet.c_str(),
         lan.empty() ? "(no LAN address)" : lan.c_str());
    if (!source.empty()) plog("public address %s (%s)", pub_text.c_str(), source.c_str());

    // The host's own endpoint for guests' records and our MappedAddr.
    const std::string ep_addr = source.empty() ? bbnet::party::ip_text(bbnet::local_ipv4()) : format_ipv4(pub);
    if (!ep_addr.empty()) {
        svc.set_host_endpoint(name, ep_addr, pub_port);
        bbnet::session::set_mapped_override(ep_addr, pub_port);
    }

    {
        // F2 traversal chain: TCP to an open port -> UDP straight in (full cone) -> hole punch
        // (the guest's endpoint from the DHT rendezvous or a guest code) -> relay / VPN advice.
        const bool port_open = upnp_ok || source == "override" || host_nat == nat::Type::Open;
        // The DHT rendezvous is opt-in (launcher Party tab / System > Party rules; both PCs).
        const bool rdv_on = !port_open && !internet.empty() && traversal::rendezvous_enabled(lb);
        const nat::Rendezvous rdv = internet.empty()                     ? nat::Rendezvous::NotApplicable
                                    : traversal::rendezvous_enabled(lb) ? nat::Rendezvous::On
                                                                        : nat::Rendezvous::Off;
        const std::string adv = nat::advice(host_nat, true, port_open, rdv, port);
        plog("NAT: %s (%s); %s", nat::type_name(host_nat), port_open ? "port open" : "no open port", adv.c_str());
        {
            std::lock_guard<std::mutex> lk(r.mu);
            r.st.network = adv;
        }
        bridge::OnNetwork(adv);
        if (rdv_on) {
            const crypto::Key key = crypto::derive_party_key(cfg.password, cfg.secret.data());
            traversal::start_host(
                key.data(), lb,
                [](std::uint32_t a, std::uint16_t p) {
                    if (PartyLink* l = runtime_link()) l->punch_guest(a, p);
                },
                [](const std::string& m) { plog("%s", m.c_str()); });
        }
    }
    std::string text = "internet=" + internet + "\nlan=" + lan + "\n";
    write_text_file(fs::path(user_dir()) / "party_code.txt", text);
    if (const char* cf = env("BB_PARTY_CODE_FILE")) write_text_file(cf, text);
}


// ---- The party link's UDP path (party_rudp.h): hole punching, guest codes ----
// The link's datagrams share the game's party UDP port (one NAT mapping for both).
void attach_udp_path(PartyLink* link) {
    link->set_udp_transport([](std::uint32_t a, std::uint16_t p, const std::uint8_t* d, std::size_t n) {
        return bbnet::p2p_send_raw(a, p, d, n);
    });
    bbnet::p2p_set_link_handler([](std::uint32_t a, std::uint16_t p, const std::uint8_t* d, std::size_t n) {
        if (lan::is_lan(d, n)) return lan_on_datagram(a, p, d, n);
        if (PartyLink* l = runtime_link()) l->on_udp_datagram(a, p, d, n);
    });
}

// Guest: its own public UDP endpoint as a guest code (BBG1-...), for a host behind a NAT without a
// port forward: the host enters it (or BB_PARTY_GUEST_CODES) and punches towards it.
// Returns the mapped port (host order; 0: none) and the NAT type in *type.
std::uint16_t publish_guest_code(nat::Type* type) {
    std::uint32_t ma = 0;
    std::uint16_t mp = 0;
    std::string source;
    *type = nat::Type::Unknown;
    const auto stun_fn = [](const std::string& h, std::uint16_t p, std::uint32_t* a, std::uint16_t* m) {
        return bbnet::p2p_stun(h.c_str(), p, 2000, a, m);
    };
    if (loopback_mode()) {
        ma = 0x0100007fu;  // 127.0.0.1, network order
        // A simulated symmetric NAT (BB_NET_SIM nat=sym) reports another port, as STUN would.
        const std::uint16_t sp = traversal::simulated_mapped_port(bbnet::settings().party_port);
        mp = static_cast<std::uint16_t>((sp >> 8) | (sp << 8));
        source = "loopback";
        *type = traversal::detect(stun_fn, {}, 0, bbnet::settings().party_port, true);
    } else {
        const char* stun_env = env("BB_PARTY_STUN");
        const std::string server = stun_env ? stun_env : "stun.l.google.com:19302";
        if (server == "off" || server == "0") return 0;
        std::string host_part = server;
        std::uint16_t sport = 19302;
        if (const auto c = server.rfind(':'); c != std::string::npos) {
            host_part = server.substr(0, c);
            sport = static_cast<std::uint16_t>(std::strtoul(server.c_str() + c + 1, nullptr, 10));
        }
        bool ok = false;
        for (int i = 0; i < 3 && !ok && !stopping(); ++i) ok = bbnet::p2p_stun(host_part.c_str(), sport, 2000, &ma, &mp);
        if (!ok) {
            plog("guest code: STUN %s did not answer (no guest code; a host behind a NAT then needs a port forward)",
                 server.c_str());
            return 0;
        }
        source = "stun " + server;
        *type = traversal::detect(stun_fn, {true, ma, static_cast<std::uint16_t>((mp >> 8) | (mp << 8))},
                                  bbnet::local_ipv4(), bbnet::settings().party_port, false);
    }
    const std::string code = rudp::encode_guest_code(ma, mp);
    {
        std::lock_guard<std::mutex> lk(R().mu);
        R().st.guest_code = code;
    }
    bridge::OnGuestCode(code);
    write_text_file(fs::path(user_dir()) / "party_guest_code.txt", code + "\n");
    if (const char* f = env("BB_PARTY_GUEST_CODE_FILE")) write_text_file(f, code + "\n");
    const auto* ip = reinterpret_cast<const std::uint8_t*>(&ma);
    plog("guest code %s (UDP %u.%u.%u.%u:%u, %s): give it to the host if it cannot open a port", code.c_str(), ip[0], ip[1],
         ip[2], ip[3], static_cast<unsigned>(static_cast<std::uint16_t>((mp >> 8) | (mp << 8))), source.c_str());
    return static_cast<std::uint16_t>((mp >> 8) | (mp << 8));
}

// Guest (F2): the NAT advice, the DHT announce while the link is not up (a host without an open
// port then finds and punches us: no guest code to pass on), and the path the link took.
void guest_traversal(const LinkConfig& cfg, const PartyCode& code, bool lan_code) {
    const bool lb = loopback_mode();
    nat::Type t = nat::Type::Unknown;
    const std::uint16_t mport = publish_guest_code(&t);
    const bool applies = mport != 0 && !code.plain && !lan_code;
    const bool rdv_on = applies && traversal::rendezvous_enabled(lb);
    const nat::Rendezvous rdv = !applies ? nat::Rendezvous::NotApplicable
                                : rdv_on ? nat::Rendezvous::On
                                         : nat::Rendezvous::Off;  // opt-in, off: the guest code or turn it on
    const std::string adv = nat::advice(t, false, false, rdv, bbnet::settings().party_port);
    plog("NAT: %s; %s", nat::type_name(t), adv.c_str());
    {
        std::lock_guard<std::mutex> lk(R().mu);
        R().st.network = adv;
    }
    bridge::OnNetwork(adv);
    if (rdv_on) {
        const crypto::Key key = crypto::derive_party_key(cfg.password, code.secret.data());
        auto since = std::make_shared<Clock::time_point>(Clock::now());
        traversal::start_guest(
            key.data(), lb, mport,
            [since] {
                PartyLink* l = runtime_link();
                if (stopping() || !l) return false;
                if (l->state() == LinkState::Connected) {
                    *since = Clock::now();
                    return false;
                }
                return Clock::now() - *since > std::chrono::seconds(3);  // TCP had its chance
            },
            [](const std::string& m) { plog("%s", m.c_str()); });
    }
    // The path, once per connection (logged and on the Party page).
    std::thread([adv] {
        bool was = false;
        while (!stopping()) {
            PartyLink* l = runtime_link();
            const bool up = l && l->state() == LinkState::Connected;
            if (up && !was) {
                const bool udp = l->connected_over_udp();
                const std::string path =
                    udp ? "UDP (the host's NAT let us in or punched towards us)" : "TCP (the host's port is open)";
                plog("network path: party link over %s", path.c_str());
                bridge::OnNetwork("Connected over " + path + ". " + adv);
            }
            was = up;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }).detach();
}

// Host: guest codes from BB_PARTY_GUEST_CODES (comma list), BB_PARTY_GUEST_CODE_FILE (polled) and
// the menu (AddGuestCode): each opens our NAT mapping towards that guest for 60 s.
std::mutex g_codes_mu;
std::set<std::string> g_codes_seen;
std::vector<std::string> g_codes_new;

void add_guest_code_text(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_codes_mu);
    std::string cur;
    auto flush = [&] {
        while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\r' || cur.back() == '\n')) cur.pop_back();
        while (!cur.empty() && cur.front() == ' ') cur.erase(cur.begin());
        if (!cur.empty() && g_codes_seen.insert(cur).second) g_codes_new.push_back(cur);
        cur.clear();
    };
    for (char c : text) {
        if (c == ',' || c == '\n') flush();
        else cur.push_back(c);
    }
    flush();
}

void punch_guest_codes_tick() {
    static auto last_file = Clock::time_point{};
    if (const char* f = env("BB_PARTY_GUEST_CODE_FILE"); f && Clock::now() - last_file > std::chrono::seconds(1)) {
        last_file = Clock::now();
        std::string body;
        if (read_file(f, &body)) add_guest_code_text(body);
    }
    std::vector<std::string> codes;
    {
        std::lock_guard<std::mutex> lk(g_codes_mu);
        codes.swap(g_codes_new);
    }
    PartyLink* l = runtime_link();
    for (const std::string& c : codes) {
        std::uint32_t a = 0;
        std::uint16_t p = 0;
        if (!rudp::decode_guest_code(c, &a, &p)) {
            plog("guest code '%s' is not valid (BBG1-XXXX-XXXX-XXXX)", c.c_str());
            continue;
        }
        if (l) l->punch_guest(a, p);
        // The game's own UDP traffic to that guest too: one probe out of the party port opens the
        // same mapping (the link's punches already share it).
        std::lock_guard<std::mutex> lk(g_codes_mu);
        g_codes_seen.insert(c);
    }
}

// ---- C6: a crashed guest's stale game connection, expired early ----
// The host's game keeps its NexusRevolution transport connection to a member until that
// connection's idle timeout (25 s; config built at 0xcdb250: 0x4d0, 3000, 5000, 6000, 25000,
// 20000, 200 ms) and would send a restarted guest's summon over it (docs/party/join_timing.md).
// When the guest's link ends with "connection reset" (its OS answered for a dead process: a crash,
// not a network hiccup), the host lowers the live copies of those two timeouts for a few seconds,
// so the dead connection expires within ~4 s instead of 25, then restores them.
// Experimental: BB_PARTY_NR_FAST_EXPIRE=1 on (off by default).
std::mutex g_nr_mu;
std::atomic<int> g_nr_epoch{0};

std::vector<std::uint32_t*> nr_config_copies(std::uint8_t* lo, std::uint8_t* hi) {
    static const std::uint32_t pat[7] = {0x4d0, 3000, 5000, 6000, 25000, 20000, 200};
    std::vector<std::uint32_t*> hits;
#ifdef _WIN32
    struct WsInfo {
        void* va;
        std::uint64_t attr;
    };
    using Qws = BOOL(WINAPI*)(HANDLE, PVOID, DWORD);
    static const auto qws = reinterpret_cast<Qws>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32QueryWorkingSetEx"));
    MEMORY_BASIC_INFORMATION mbi;
    std::uint8_t* at = lo;
    while (at < hi && VirtualQuery(at, &mbi, sizeof mbi) == sizeof mbi) {
        auto* base = static_cast<std::uint8_t*>(mbi.BaseAddress);
        const std::size_t size = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && qws) {
            // Only pages that are resident: reading a never-touched committed page would make
            // Windows back it with RAM (the guest's direct memory is GiBs of those).
            constexpr std::size_t kPage = 4096, kBatch = 4096;
            std::vector<WsInfo> info(kBatch);
            for (std::size_t off = 0; off < size; off += kBatch * kPage) {
                const std::size_t pages = std::min(kBatch, (size - off) / kPage);
                for (std::size_t k = 0; k < pages; ++k) {
                    info[k].va = base + off + k * kPage;
                    info[k].attr = 0;
                }
                if (!qws(GetCurrentProcess(), info.data(), static_cast<DWORD>(pages * sizeof(WsInfo)))) continue;
                for (std::size_t k = 0; k < pages; ++k) {
                    if (!(info[k].attr & 1)) continue;  // Valid
                    const auto* w = reinterpret_cast<const std::uint32_t*>(base + off + k * kPage);
                    for (std::size_t i = 0; i + 7 <= kPage / 4; ++i) {
                        if (w[i] == 0x4d0 && w[i + 4] == 25000 && std::memcmp(w + i, pat, sizeof pat) == 0)
                            hits.push_back(const_cast<std::uint32_t*>(w + i));
                    }
                }
            }
        }
        at = base + size;
        if (reinterpret_cast<std::uintptr_t>(at) >= (1ull << 47)) break;
    }
#endif
    return hits;
}

// Experimental, off by default (BB_PARTY_NR_FAST_EXPIRE=1): the copies were found in 2 of 3 live
// runs (none in the third) and an early expiry was never shown (docs/party/join_timing.md).
bool nr_disabled() { return !env_on("BB_PARTY_NR_FAST_EXPIRE"); }

bool nr_copy_ok(const std::uint32_t* h) {
    std::uint32_t v[7] = {};
    if (!coop::SafeRead(reinterpret_cast<std::uint64_t>(h), v, sizeof v)) return false;
    // The live transport keeps the run from 5000 on (+0x84..+0x94 of the transport; the words
    // before it are in use), so 0x4d0 / 3000 are not required.
    return v[2] == 5000 && v[3] == 6000 && v[6] == 200;
}

std::vector<std::uint32_t*> g_nr_hits;  // g_nr_mu

// RE aid (BB_PARTY_NR_WATCH=1, with BB_PARTY_NR_FAST_EXPIRE=1): hardware data breakpoints on the
// first config copy's two idle timeouts (+0x10, +0x14 from the 0x4d0 word) in every thread; each
// distinct reading instruction is logged once a second (guest offset, count).
std::atomic<std::uint64_t> g_nrw_rip[64];
std::atomic<std::uint32_t> g_nrw_cnt[64];
std::atomic<std::uint64_t> g_nrw_lo{0}, g_nrw_hi{0};

LONG CALLBACK nr_watch_veh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!(c->Dr6 & 3)) return EXCEPTION_CONTINUE_SEARCH;
    const std::uint64_t rip = c->Rip;
    for (int i = 0; i < 64; ++i) {
        std::uint64_t cur = g_nrw_rip[i].load();
        if (cur == rip) {
            g_nrw_cnt[i].fetch_add(1);
            break;
        }
        if (cur == 0 && g_nrw_rip[i].compare_exchange_strong(cur, rip)) {
            g_nrw_cnt[i].fetch_add(1);
            break;
        }
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void nr_watch_arm(std::uint32_t* h) {
    static bool once = false;
    if (once || !env_on("BB_PARTY_NR_WATCH")) return;
    once = true;
    AddVectoredExceptionHandler(1, nr_watch_veh);
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    int armed = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != static_cast<DWORD>(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &c)) {
                c.Dr0 = reinterpret_cast<DWORD64>(h + 4);
                c.Dr1 = reinterpret_cast<DWORD64>(h + 5);
                // L0, L1; RW = 11 (read/write), LEN = 11 (4 bytes) for both.
                c.Dr7 = (c.Dr7 & ~0xff0000full) | 1 | 4 | (0xfull << 16) | (0xfull << 20);
                if (SetThreadContext(th, &c)) ++armed;
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    const std::uint64_t base = reinterpret_cast<std::uint64_t>(coop::Image());
    plog("nr watch: data breakpoints on %p (+0x10 / +0x14) in %d threads", static_cast<void*>(h), armed);
    std::thread([base] {
        std::uint32_t last[64] = {};
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            for (int i = 0; i < 64; ++i) {
                const std::uint64_t rip = g_nrw_rip[i].load();
                if (!rip) break;
                const std::uint32_t n = g_nrw_cnt[i].load();
                if (n != last[i] && (last[i] == 0 || n / 100 != last[i] / 100)) {
                    plog("nr watch: read at %#llx (guest +%#llx) x%u", static_cast<unsigned long long>(rip),
                         static_cast<unsigned long long>(rip >= base ? rip - base : 0), n);
                    last[i] = n;
                }
            }
        }
    }).detach();
}


// Finds the copies ahead of time (the scan of the resident heap takes seconds): when the host's
// game gets a cooperator (its transport holds them), and again at a crash if they moved.
void nr_scan_locked(const char* why) {
    const auto t0 = Clock::now();
    // The game's heap layout repeats from run to run: the last place they were found
    // (user/party_nr_hint.txt) +-512 MiB first, the whole process only when that misses.
    const fs::path hint_file = fs::path(user_dir()) / "party_nr_hint.txt";
    std::string body;
    const char* how = "whole process";
    g_nr_hits.clear();
    for (const coop::b2::NrTransport& t : coop::b2::NrTransports()) {
        if (t.config && nr_copy_ok(t.config)) g_nr_hits.push_back(t.config);
        // Live (2026-10-12 search): the transport keeps 5000 6000 25000 20000 200 at +0x84..+0x94
        // and again at +0x254c..+0x255c (two connection-config blocks); h = start - 8.
        for (const std::uint64_t off : {0x7cull, 0x2544ull}) {
            auto* c = reinterpret_cast<std::uint32_t*>(t.transport + off);
            if (t.transport && nr_copy_ok(c)) g_nr_hits.push_back(c);
        }
        if (t.transport) {
            std::uint32_t w[16] = {};
            MEMORY_BASIC_INFORMATION mi{};
            plog("transport %#llx timing values:%s", static_cast<unsigned long long>(t.transport),
                 coop::b2::NrFindTimeouts(t.transport, 0x8000).c_str());
            {
                // The transport's connection array (FUN_027163B0: transport +0xd0, count +0x6c,
                // 0xc0 bytes each): timing values in each element and one pointer level down.
                std::uint64_t arr = 0;
                std::uint32_t cnt = 0;
                coop::SafeRead(t.transport + 0xd0, &arr, 8);
                coop::SafeRead(t.transport + 0x6c, &cnt, 4);
                plog("transport %#llx connections: array %#llx count %u", static_cast<unsigned long long>(t.transport),
                     static_cast<unsigned long long>(arr), cnt);
                for (std::uint32_t k = 0; arr && k < cnt && k < 8; ++k) {
                    const std::uint64_t el = arr + k * 0xc0ull;
                    std::string sub;
                    std::uint64_t q[24] = {};
                    coop::SafeRead(el, q, sizeof q);
                    for (int w = 0; w < 24; ++w) {
                        if (q[w] >> 32 != 0x11 && q[w] >> 32 != 0x10) continue;
                        const std::string f = coop::b2::NrFindTimeouts(q[w], 0x800);
                        if (!f.empty()) {
                            char h[48];
                            std::snprintf(h, sizeof h, " [+%#x -> %#llx]", w * 8, static_cast<unsigned long long>(q[w]));
                            sub += h + f;
                        }
                    }
                    plog("  conn %u at %#llx:%s |%s", k, static_cast<unsigned long long>(el),
                         coop::b2::NrFindTimeouts(el, 0xc0).c_str(), sub.c_str());
                }
            }
            plog("factory %#llx timing values:%s", static_cast<unsigned long long>(t.factory),
                 coop::b2::NrFindTimeouts(t.factory, 0x4000).c_str());
            plog("owner %#llx timing values:%s", static_cast<unsigned long long>(t.owner),
                 coop::b2::NrFindTimeouts(t.owner, 0xc0).c_str());
            if (coop::SafeRead(t.transport + 0x140, w, sizeof w)) {
                plog("transport %#llx +0x140: %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x",
                     static_cast<unsigned long long>(t.transport), w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8],
                     w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
            }
        }
        if (!g_nr_hits.empty()) break;  // the newest transport only
    }
    if (!g_nr_hits.empty()) {
        plog("game transport config: %zu cop%s from the transport-create hook", g_nr_hits.size(),
             g_nr_hits.size() == 1 ? "y" : "ies");
        nr_watch_arm(g_nr_hits[0]);
        return;
    }
    if (read_file(hint_file, &body)) {
        const std::uintptr_t h = static_cast<std::uintptr_t>(std::strtoull(body.c_str(), nullptr, 16));
        if (h > (1ull << 30)) {
            g_nr_hits = nr_config_copies(reinterpret_cast<std::uint8_t*>(h - (512ull << 20)),
                                         reinterpret_cast<std::uint8_t*>(h + (512ull << 20)));
            how = "near the last place";
            if (g_nr_hits.empty())
                plog("game transport config: none within 512 MiB of %llx (%.0f ms); the whole process next",
                     static_cast<unsigned long long>(h),
                     std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
        } else {
            plog("game transport config: hint '%s' not usable", body.c_str());
        }
    } else {
        plog("game transport config: no hint file %s", hint_file.string().c_str());
    }
    if (g_nr_hits.empty()) {
        how = "whole process";
        g_nr_hits = nr_config_copies(nullptr, reinterpret_cast<std::uint8_t*>(1ull << 47));
    }
    if (!g_nr_hits.empty()) {
        char hx[32];
        std::snprintf(hx, sizeof hx, "%llx\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_nr_hits[0])));
        write_text_file(hint_file, hx);
    }
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    plog("game transport config: %zu copies (%s, scan %.0f ms, %s)", g_nr_hits.size(), why, ms, how);
    for (std::uint32_t* h : g_nr_hits) {
        MEMORY_BASIC_INFORMATION mi{};
        VirtualQuery(h, &mi, sizeof mi);
        plog("  at %p, region %p + %#llx (type %#lx)", static_cast<void*>(h), mi.AllocationBase,
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(h) -
                                             reinterpret_cast<std::uintptr_t>(mi.AllocationBase)),
             static_cast<unsigned long>(mi.Type));
    }
}
}  // namespace

void runtime_nr_prescan() {
    if (nr_disabled()) return;
    std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        std::unique_lock<std::mutex> lk(g_nr_mu, std::try_to_lock);
        if (!lk.owns_lock()) return;
        bool ok = !g_nr_hits.empty();
        for (const std::uint32_t* h : g_nr_hits) ok = ok && nr_copy_ok(h);
        if (!ok) nr_scan_locked("a cooperator joined");
        if (!g_nr_hits.empty()) nr_watch_arm(g_nr_hits[0]);
    }).detach();
}

namespace {
void nr_fast_expire(const std::string& name) {
    if (nr_disabled()) return;
    const int epoch = ++g_nr_epoch;
    std::thread([name, epoch] {
        std::lock_guard<std::mutex> lk(g_nr_mu);
        std::vector<std::uint32_t*> hits;
        for (std::uint32_t* h : g_nr_hits)
            if (nr_copy_ok(h) && (h[4] == 25000 || h[4] == 4000)) hits.push_back(h);
        if (hits.empty()) {
            nr_scan_locked("at the crash");
            hits = g_nr_hits;
        }
        for (std::uint32_t* h : hits) {
            h[4] = 4000;
            h[5] = 3000;
        }
        plog("%s crashed (connection reset): game transport idle timeout 25 -> 4 s for 8 s (%zu config copies)",
             name.c_str(), hits.size());
        // The dead connection now expires within ~4 s: the sign hold (26 s at the drop) ends 5 s from now.
        if (!hits.empty()) bbnet::party::FromApi::instance().hold_signs(name, 5000);
        std::this_thread::sleep_for(std::chrono::seconds(8));
        for (std::uint32_t* h : hits) {
            if (nr_copy_ok(h) && h[4] == 4000) {
                h[4] = 25000;
                h[5] = 20000;
            }
        }
        plog("game transport idle timeout restored (25 s)%s", g_nr_epoch.load() != epoch ? " (another crash since)" : "");
    }).detach();
}

// The code a guest uses: BB_PARTY_CODE, BB_PARTY=<code>, or BB_PARTY_CODE_FILE (waits <= 60 s).
bool guest_code(PartyCode* code, std::string* error, std::string* used) {
    std::string text;
    if (const char* c = env("BB_PARTY_CODE")) {
        text = c;
    } else if (const std::string saved = flow::RuntimeCode(); !saved.empty()) {
        text = saved;  // the party flow's host address (party.json)
    } else if (const char* p = env("BB_PARTY"); p && lower(p) != "join" && lower(p) != "guest" && lower(p) != "1") {
        text = p;
    }
    if (!text.empty()) {
        *used = text;
        return decode_party_code(text, code, error);
    }
    const char* cf = env("BB_PARTY_CODE_FILE");
    if (!cf) {
        *error = "no party code: set BB_PARTY_CODE (or BB_PARTY_CODE_FILE)";
        return false;
    }
    const bool prefer_lan = env_on("BB_PARTY_PREFER_LAN");
    plog("waiting for the party code in %s", cf);
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    std::string last_err = "the file did not appear";
    while (Clock::now() < deadline && !stopping()) {
        std::string body;
        if (read_file(cf, &body)) {
            std::string internet, lan, bare;
            std::istringstream in(body);
            std::string line;
            while (std::getline(in, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.rfind("internet=", 0) == 0) internet = line.substr(9);
                else if (line.rfind("lan=", 0) == 0) lan = line.substr(4);
                else if (!line.empty() && bare.empty()) bare = line;
            }
            for (const std::string& c : prefer_lan ? std::vector<std::string>{lan, internet, bare}
                                                   : std::vector<std::string>{internet, lan, bare}) {
                if (!c.empty() && decode_party_code(c, code, &last_err)) {
                    *used = c;
                    return true;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    *error = std::string("no usable party code in ") + cf + ": " + last_err;
    return false;
}

// Guest with an Internet code (or host:port with a password): a host on our own network is often
// unreachable at its public address (no hairpin NAT on the router). When that address does not
// answer a TCP connect within 1.5 s, or our own public address (STUN) is the code's, ask the LAN
// (party_lan.h, ~2 s): a host of this party answers with its LAN address, and we join that.
// A BBP2 LAN code (lan_partial) takes part too: its first three octets are only our own guess.
// `forced`: ask the LAN straight away for this reason (a LAN code and no local IPv4 to guess with).
bool lan_fallback(const PartyCode& code, const LinkConfig& cfg, std::array<std::uint8_t, 4>* ip, std::uint16_t* port,
                  const char* forced = nullptr) {
    if ((code.flags & kCodeFlagLan) && !code.lan_partial) return false;  // a full LAN address
    if (code.plain && cfg.password.empty()) return false;  // no secret and no password: nothing to prove
    if (!lan_discovery_enabled()) return false;
    const bool lb = loopback_mode();
    std::uint32_t nbo = 0;
    std::memcpy(&nbo, ip->data(), 4);
    std::string why;
    if (forced) {
        why = forced;
    } else if (!lan::tcp_reachable(nbo, *port, 1500)) {
        why = "the host's address " + format_ipv4(*ip) + ":" + std::to_string(*port) + " did not answer in 1.5 s";
    } else if (!lb && !(code.flags & kCodeFlagLan)) {
        const char* stun_env = env("BB_PARTY_STUN");
        const std::string server = stun_env ? stun_env : "stun.l.google.com:19302";
        if (server != "off" && server != "0") {
            std::string host_part = server;
            std::uint16_t sport = 19302;
            if (const auto c = server.rfind(':'); c != std::string::npos) {
                host_part = server.substr(0, c);
                sport = static_cast<std::uint16_t>(std::strtoul(server.c_str() + c + 1, nullptr, 10));
            }
            std::uint32_t ma = 0;
            std::uint16_t mp = 0;
            if (bbnet::p2p_stun(host_part.c_str(), sport, 1000, &ma, &mp) && ma == nbo)
                why = "our public address is the host's (same network)";
        }
    }
    if (why.empty()) return false;
    const std::array<std::uint8_t, 8> no_secret{};
    const crypto::Key key = crypto::derive_party_key(cfg.password, code.plain ? no_secret.data() : code.secret.data());
    const std::vector<std::uint32_t> targets = lb ? std::vector<std::uint32_t>{0x0100007fu} : lan::broadcast_targets();
    plog("LAN fallback: %s; looking for the host on this network (port %u)", why.c_str(), *port);
    lan::Found f;
    std::string note;
    if (lan::discover(key, targets, *port, 2000, lb, &f, &note)) {
        std::memcpy(ip->data(), &f.addr_nbo, 4);
        *port = f.port;
        plog("LAN fallback: found host at %s:%u (it reports %s; %s)", format_ipv4(*ip).c_str(), *port,
             format_ipv4(f.said).c_str(), note.c_str());
        return true;
    }
    plog("LAN fallback: no host of this party answered on this network (%s); trying %s:%u", note.c_str(),
         format_ipv4(*ip).c_str(), *port);
    return false;
}

void start_guest(LinkConfig cfg) {
    Runtime& r = R();
    PartyCode code;
    std::string err, code_text;
    if (!guest_code(&code, &err, &code_text)) {
        set_error(err);
        plog("%s", err.c_str());
        return;
    }
    const char* lan_forced = nullptr;
    if (code.lan_partial) {
        // A BBP2 LAN code holds the host's last octet: the rest is our own /24.
        std::array<std::uint8_t, 4> local{127, 0, 0, 1};
        if (loopback_mode() || best_local_ipv4(&local)) {
            fill_lan_prefix(&code, local);
            plog("LAN code: host %s (first three octets from our address %s)", code.host.c_str(),
                 format_ipv4(local).c_str());
        } else {
            code.host = format_ipv4(code.ipv4);
            lan_forced = "a LAN code and no local IPv4 address to complete it";
        }
    }
    std::array<std::uint8_t, 4> ip{};
    std::uint16_t port = code.port;
    if (!parse_ipv4(code.host, &ip) && !resolve_ipv4(code.host, code.port, &ip, &port)) {
        set_error("cannot resolve the host " + code.host);
        plog("cannot resolve the host %s", code.host.c_str());
        return;
    }
    bridge::OnCode(code_text);
    cfg.secret = code.secret;
    if ((code.flags & kCodeFlagPasswordRequired) && cfg.password.empty())
        plog("warning: the host requires a password (BB_PARTY_PASSWORD)");
    if (code_ignores_password(code) && !cfg.password.empty()) {
        // A launcher / party.json password left over from another party would make the key differ
        // from the host's (argon2(password || secret)): "wrong password or party code".
        plog("code needs no password; saved password ignored");
        cfg.password.clear();
    }
    if (!bbnet::p2p_open()) plog("warning: cannot open the party UDP port %u (another program?)", bbnet::settings().party_port);
    if (!lan_fallback(code, cfg, &ip, &port, lan_forced) && lan_forced) {
        set_error("cannot find the host of this LAN code (no local IPv4 address, no LAN answer)");
        plog("cannot find the host of this LAN code (no local IPv4 address, no LAN answer)");
        return;
    }
    std::uint32_t nbo = 0;
    std::memcpy(&nbo, ip.data(), 4);
    // The failure hint: with the DHT rendezvous off (opt-in), only a guest code or the option on both PCs helps.
    cfg.auto_punch = !code.plain && !(code.flags & kCodeFlagLan) && traversal::rendezvous_enabled(loopback_mode());
    auto* link = new PartyLink(cfg, make_callbacks(false));
    attach_udp_path(link);
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.link = link;
        r.host_ip_nbo = nbo;
        r.code_coded = !code.plain;
        r.code_needs_password = (code.flags & kCodeFlagPasswordRequired) != 0;
        r.sent_password = !cfg.password.empty();
        r.code_ignores_password = code_ignores_password(code);
        r.flow_password_at_start = flow::RuntimePassword();
        r.host_port = port;
        r.st.host_address = format_ipv4(ip) + ":" + std::to_string(port);
        r.cfg = cfg;
        r.have_cfg = true;
    }
    bridge::OnHostAddress(format_ipv4(ip) + ":" + std::to_string(port));
    plog("joining %s as %s%s", r.st.host_address.c_str(), cfg.name.c_str(),
         (code.flags & kCodeFlagLan) ? " (LAN code)" : "");
    bbnet::party::remote_guest();  // the RemoteGuest transport exists before the game asks
    if (!link->start_guest(format_ipv4(ip), port, &err)) {
        set_error("cannot join: " + err);
        plog("cannot join: %s", err.c_str());
        return;
    }
    coop::PartyDirector::Get().SetLink(link);
    guest_traversal(cfg, code, (code.flags & kCodeFlagLan) != 0);
}

void startup_main() {
    Runtime& r = R();
    const bbnet::Settings& s = bbnet::settings();
    const bool host = s.host;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.st.role = host ? RuntimeRole::Host : RuntimeRole::Guest;
        r.st.name = s.online_id;
        r.st.port = s.party_port;
    }
    bridge::OnRuntimeStart(host, s.online_id);
    {
        const RestartInfo& ri = restart_info();  // before the marker below is rewritten
        {
            std::lock_guard<std::mutex> lk(r.mu);
            r.st.restarted = ri.restarted;
        }
        if (ri.restarted) plog("restarted after an unclean exit (%s): rejoining", ri.why.c_str());
        const fs::path m = marker_path();
        char body[256];
        std::snprintf(body, sizeof body, "\"pid\":%lu,\"role\":\"%s\",\"name\":\"%s\",\"started\":%lld",
                      static_cast<unsigned long>(
#if defined(_WIN32)
                          GetCurrentProcessId()
#else
                          0
#endif
                              ),
                      host ? "host" : "guest", s.online_id.c_str(),
                      static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(
                                                 std::chrono::system_clock::now().time_since_epoch())
                                                 .count()));
        {
            std::lock_guard<std::mutex> lk(r.mu);
            r.marker = m.string();
            r.marker_head = body;
        }
        // restart_info() above kept the old marker's text (the host's previous secret and
        // members) before this first write.
        write_marker();
    }
    if (loopback_mode()) {
#if defined(_WIN32)
        if (!env("BB_PARTY_LOCAL_IP")) _putenv_s("BB_PARTY_LOCAL_IP", "127.0.0.1");
#endif
    }
    LinkConfig cfg;
    apply_link_env(cfg);
    if (cfg.password.empty()) cfg.password = flow::RuntimePassword();  // the party flow's (party.json)
    cfg.port = s.party_port;  // the network library's (validated) value
    cfg.name = s.online_id;
    if (!valid_member_name(cfg.name)) {
        set_error("invalid party name " + cfg.name);
        plog("invalid party name '%s'", cfg.name.c_str());
        return;
    }
    cfg.eboot_sha256 = eboot_hash();
    std::string what;
    patch_identity(cfg, &what);
    mod_identity(cfg, &what);
    // Max players and the 4-player rule set (party_fourp.h): the same for every player.
    cfg.rules = "max " + std::to_string(cfg.max_players) + " players, " + coop::fourp::FourpRulesTag();
    // A9 build / patch-set fingerprint (party_link.h identity_mismatch).
    cfg.build_id = bbport_build_id();
    cfg.check_build = !(env("BB_PARTY_CHECK_BUILD") && !env_on("BB_PARTY_CHECK_BUILD"));
    cfg.params_sha256 = params_hash();
    hook_identity(cfg);
    plog("%s %s, port %u, up to %d players; version check: eboot %s..., patches %s..., mods %s... (%s)%s",
         host ? "host" : "guest", cfg.name.c_str(), cfg.port, cfg.max_players, hex_prefix(cfg.eboot_sha256).c_str(),
         hex_prefix(cfg.patches_hash).c_str(), hex_prefix(cfg.mods_hash).c_str(), what.c_str(),
         loopback_mode() ? "; loopback test" : "");
    plog("gameplay patches: %s", joined(cfg.patch_names).c_str());
    plog("gameplay mods: %s", joined(cfg.mod_names).c_str());
    plog("party rules: %s", cfg.rules.c_str());
    write_identity_file(cfg);
    plog("build %s%s, params %s..., party patches %s... (%zu hooks)", cfg.build_id.c_str(),
         cfg.check_build ? "" : " (not compared: BB_PARTY_CHECK_BUILD=0)", hex_prefix(cfg.params_sha256).c_str(),
         hex_prefix(cfg.hooks_hash).c_str(), cfg.hook_names.size());
    {
        std::lock_guard<std::mutex> lk(r.mu);
        if (r.stopping) return;
    }
    if (host) start_host(std::move(cfg));
    else start_guest(std::move(cfg));
    bool have;
    MemberState ms;
    std::uint32_t map;
    PartyLink* link;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        have = r.have_local_state;
        ms = r.local_state;
        map = r.local_map;
        link = r.link;
    }
    if (have && link) link->set_local_state(ms, map);
}

void do_shutdown() {
    Runtime& r = R();
    PartyLink* link;
    std::unique_ptr<UpnpMapper> upnp;
    std::vector<std::shared_ptr<Pump>> pumps;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.stopping = true;
        link = r.link;
        upnp = std::move(r.upnp);
        for (auto& [k, p] : r.pumps) pumps.push_back(p);
        r.pumps.clear();
    }
    for (auto& p : pumps) p->stop = true;
    if (!pumps.empty()) bbnet::party::PartyHostService::instance().wake_all();
    if (link) {
        coop::PartyDirector::Get().SetLink(nullptr);
        link->stop(true);  // BYE; the object stays (never deleted)
    }
    traversal::stop();
    if (upnp) upnp->stop(2000);
}

// ---- Leave / Rejoin (the overlay's Party tab, party_status_bridge.h) -----------------------

// Stops the link with BYE (host: the party ends for everyone; guest: no more reconnects) and
// forgets the crash marker (a crash after Leave must not rejoin). Background thread.
void leave_main() {
    Runtime& r = R();
    PartyLink* link;
    bool host;
    std::vector<std::shared_ptr<Pump>> pumps;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        link = r.link;
        host = r.st.role == RuntimeRole::Host;
        r.left = true;
        for (auto& [k, p] : r.pumps) pumps.push_back(p);
        r.pumps.clear();
        if (!r.marker.empty()) {
            std::error_code ec;
            fs::remove(r.marker, ec);
        }
    }
    for (auto& p : pumps) p->stop = true;
    if (!pumps.empty()) bbnet::party::PartyHostService::instance().wake_all();
    coop::PartyDirector::Get().SetLink(nullptr);
    if (link) link->stop(true);
    plog("%s", host ? "Leave: the party is over (BYE sent)" : "Leave: left the party (BYE sent; no reconnects)");
    if (!host) {
        // The guest plays on alone: its game's server calls go to this process's own service
        // (as after a refusal), not to a host it left (3 failed calls drop the game offline).
        if (bbnet::party::RemoteGuest* rg = bbnet::party::remote_guest(); rg && rg->set_solo(true)) {
            plog("Leave: playing online alone in this world (System > Party > Rejoin the host retries)");
            bbnet::session::host_session_reset();
        }
    }
    bridge::OnLeft(host);
    std::lock_guard<std::mutex> lk(r.mu);
    r.busy = false;
}

// A fresh link with the same config (host: same port and secret, so the codes stay valid).
void rejoin_main() {
    Runtime& r = R();
    LinkConfig cfg;
    bool host;
    std::string host_text;
    std::uint16_t port;
    PartyLink* old;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        cfg = r.cfg;
        host = r.st.role == RuntimeRole::Host;
        if (!host) {
            // A password set in System > Party since the start (a refused guest fixing it) is
            // the one this Rejoin sends.
            cfg.password = bridge::RejoinPassword(cfg.password, r.flow_password_at_start, flow::RuntimePassword(),
                                                  r.code_ignores_password);
            r.cfg.password = cfg.password;
            r.sent_password = !cfg.password.empty();
        }
        host_text = bbnet::party::ip_text(r.host_ip_nbo);
        port = r.host_port;
        old = r.link;
    }
    coop::PartyDirector::Get().SetLink(nullptr);
    if (old) old->stop(true);  // a Rejected / Stopped link: only joins its threads
    bridge::OnRejoining();
    auto* link = new PartyLink(cfg, make_callbacks(host));
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.link = link;
        r.left = false;
        r.rpc_wired = false;  // the RemoteGuest RPC goes over the new link
        r.fresh_link = !host;
        r.st.last_error.clear();
    }
    write_marker();  // back in a party: a crash rejoins again
    std::string err;
    const bool ok = host ? link->start_host(&err) : link->start_guest(host_text, port, &err);
    if (!ok) {
        set_error(std::string(host ? "cannot host: " : "cannot join: ") + err);
        plog("Rejoin: %s", err.c_str());
    } else {
        plog("Rejoin: %s", host ? "hosting again" : ("connecting to " + host_text + ":" + std::to_string(port)).c_str());
        coop::PartyDirector::Get().SetLink(link);
        bool have;
        MemberState ms;
        std::uint32_t map;
        {
            std::lock_guard<std::mutex> lk(r.mu);
            have = r.have_local_state;
            ms = r.local_state;
            map = r.local_map;
        }
        if (have) link->set_local_state(ms, map);
    }
    std::lock_guard<std::mutex> lk(r.mu);
    r.busy = false;
}

// Registered with the network library (bbnet_internal.h) at static initialization.
struct Registrar {
    Registrar() {
        bbnet::RuntimeHooks h;
        h.start = &runtime_start;
        h.shutdown = &runtime_shutdown;
        h.status = &party_status_line;
        bbnet::set_runtime_hooks(h);
    }
} g_registrar;

}  // namespace

const char* runtime_role_name(RuntimeRole r) {
    switch (r) {
    case RuntimeRole::Host: return "host";
    case RuntimeRole::Guest: return "guest";
    default: return "none";
    }
}

void runtime_start() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!bbnet::settings().party) return;
        std::thread([] {
            startup_main();
            Runtime& r = R();
            std::lock_guard<std::mutex> lk(r.mu);
            r.st.started = true;
            r.cv.notify_all();
        }).detach();
    });
}

void runtime_leave() {
    Runtime& r = R();
    {
        std::lock_guard<std::mutex> lk(r.mu);
        if (r.busy || r.left || r.stopping || !r.link) return;
        r.busy = true;
    }
    std::thread(leave_main).detach();
}

int runtime_rejoin() {
    Runtime& r = R();
    {
        std::lock_guard<std::mutex> lk(r.mu);
        if (r.busy || r.stopping || !r.have_cfg) return -1;
        if (r.link && !r.left) {
            const LinkState s = r.link->state();
            if (s == LinkState::Connected || s == LinkState::Hosting) return 0;
            if (s == LinkState::Connecting) return 1;
            if (s == LinkState::Reconnecting) return r.link->reconnect_now() ? 1 : -1;
        }
        r.busy = true;
    }
    std::thread(rejoin_main).detach();
    return 2;
}

void runtime_shutdown() {
    Runtime& r = R();
    {
        std::lock_guard<std::mutex> lk(r.mu);
        if (r.shut) return;
        r.shut = true;
        if (!r.marker.empty()) {  // a clean exit: the next start is not a crash recovery
            std::error_code ec;
            fs::remove(r.marker, ec);
        }
        if (!r.link && !r.upnp) {
            r.stopping = true;
            return;
        }
    }
    plog("shutting down (BYE%s)", r.upnp ? ", UPnP unmap" : "");
    // Bounded: the exit path must not hang on a stuck router or socket.
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread([done] {
        do_shutdown();
        done->store(true);
    }).detach();
    const auto deadline = Clock::now() + std::chrono::milliseconds(2500);
    while (!done->load() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    plog("shutdown %s", done->load() ? "done" : "timed out");
}

RuntimeStatus runtime_status() {
    Runtime& r = R();
    PartyLink* link;
    RuntimeStatus st;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        st = r.st;
        link = r.link;
    }
    if (link) {
        st.state = link->state();
        st.local_slot = link->local_slot();
        st.roster = link->roster();
    }
    return st;
}

std::string party_status_line() {
    const RuntimeStatus st = runtime_status();
    if (st.role == RuntimeRole::None) return "party off";
    std::string s = std::string("party ") + runtime_role_name(st.role) + " " + st.name + ": " + link_state_name(st.state);
    if (st.role == RuntimeRole::Host) {
        s += " :" + std::to_string(st.port);
    } else if (!st.host_address.empty()) {
        s += " " + st.host_address;
        if (st.local_slot > 0) s += " slot " + std::to_string(st.local_slot);
    }
    int connected = 0;
    std::string others;
    for (const RosterEntry& e : st.roster) {
        if (e.connected) ++connected;
        if (e.name == st.name) continue;
        if (!others.empty()) others += ", ";
        others += e.name + " " + member_state_name(e.state) + (e.connected ? "" : " lost") + " " +
                  std::to_string(e.ping_ms) + " ms";
    }
    if (!st.roster.empty()) s += ", " + std::to_string(connected) + " in party";
    if (!others.empty()) s += " (" + others + ")";
    if (!st.last_error.empty()) s += "; " + st.last_error;
    return s;
}

bool runtime_restarted() { return bbnet::settings().party && restart_info().restarted; }

PartyLink* RuntimeApi::link() const { return runtime_link(); }
RuntimeStatus RuntimeApi::status() const { return runtime_status(); }
std::string RuntimeApi::status_line() const { return party_status_line(); }
void RuntimeApi::set_local_state(MemberState state, std::uint32_t map_id) const { party::set_local_state(state, map_id); }

const RuntimeApi& runtime() {
    static const RuntimeApi api;
    return api;
}

int runtime_add_guest_code(const std::string& text) {
    int valid = 0;
    std::string cur;
    auto one = [&] {
        std::uint32_t a = 0;
        std::uint16_t p = 0;
        if (!cur.empty() && rudp::decode_guest_code(cur, &a, &p)) ++valid;
        cur.clear();
    };
    for (char c : text) {
        if (c == ',' || c == '\n' || c == '\r' || c == ' ') one();
        else cur.push_back(c);
    }
    one();
    if (valid) add_guest_code_text(text);
    return valid;
}

PartyLink* runtime_link() {
    std::lock_guard<std::mutex> lk(R().mu);
    return R().link;
}

void set_local_state(MemberState state, std::uint32_t map_id) {
    Runtime& r = R();
    PartyLink* link;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.have_local_state = true;
        r.local_state = state;
        r.local_map = map_id;
        link = r.link;
    }
    if (link) link->set_local_state(state, map_id);
}

void runtime_host_cooperators(int cooperators) {
    // F5: our game dropped a cooperator while members' links are silent (an outage): those
    // memberships end now (PartyHostService::member_timed_out), so our game finishes with that
    // session (it leaves it ~5 s after the member left, live) while the network is still down and
    // the returning member's first direct summon gets a fresh room.
    static int prev = -1;
    if (cooperators < 0) return;
    const int before = prev;
    prev = cooperators;
    if (before < 0 || cooperators >= before) return;
    {
        std::lock_guard<std::mutex> lk(R().mu);
        if (R().st.role != RuntimeRole::Host) return;
    }
    std::vector<std::string> silent;
    {
        std::lock_guard<std::mutex> lk(g_lost_mu);
        for (const auto& [name, why] : g_lost_why)
            if (silent_loss(why)) silent.push_back(name);
    }
    int left = before - cooperators;
    for (const std::string& name : silent) {
        if (left-- <= 0) break;
        if (bbnet::party::PartyHostService::instance().member_timed_out(name))
            plog("%s: our game dropped it during the outage (cooperators %d -> %d): its room membership ended",
                 name.c_str(), before, cooperators);
    }
}

}  // namespace party

extern "C" void bbnet_shutdown(void) { party::runtime_shutdown(); }
