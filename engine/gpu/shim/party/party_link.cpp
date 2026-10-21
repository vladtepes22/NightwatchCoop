// SPDX-License-Identifier: GPL-3.0-or-later
#include "party_link.h"

#include "party_crypto.h"
#include "party_sock.h"
#include "party_rudp.h"
#include "../net/netsim.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <thread>

namespace party {

namespace {

using Clock = std::chrono::steady_clock;
using crypto::Key;

// Guest: when the host last ended the party (BYE) or its open link dropped; process-wide, as a
// rejoin starts a fresh link (host_no_answer_hint).
std::atomic<long long> g_host_gone_ms{-1};
void note_host_gone() {
    g_host_gone_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
double since_host_gone_s() {
    const long long at = g_host_gone_ms.load();
    if (at < 0) return -1;
    const long long now =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    return double(now - at) / 1000.0;
}
using crypto::Nonce;

enum MsgType : std::uint8_t {
    kHello = 1,
    kChallenge = 2,
    kAuth = 3,
    kWelcome = 4,
    kReject = 5,
    kPing = 10,
    kPong = 11,
    kRoster = 12,
    kRpcReq = 13,
    kRpcResp = 14,
    kEvent = 15,
    kEventAck = 16,
    kPartyCmd = 17,
    kProgress = 18,
    kBye = 19,
    kVitals = 20,  // party HUD (MemberVitals); unknown to older peers, which ignore it
    kEncrypted = 0xE0,
};

constexpr std::uint8_t kMagic[4] = {'B', 'B', 'P', 'L'};

// The calling thread's CPU cycles (0 where unavailable): a lock hold's own work, without the
// time the thread was preempted.
std::uint64_t thread_cycles() {
#if defined(_WIN32)
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return c;
#else
    return 0;
#endif
}

// A peer's clock value in ms (a host's uptime): beyond this it is nonsense, and arithmetic on it
// could overflow.
constexpr std::int64_t kMaxClockMs = 1ll << 50;

std::int64_t ms_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
}

struct Writer {
    std::vector<std::uint8_t> b;
    Writer& u8(std::uint8_t v) {
        b.push_back(v);
        return *this;
    }
    Writer& u16(std::uint16_t v) { return u8(static_cast<std::uint8_t>(v)).u8(static_cast<std::uint8_t>(v >> 8)); }
    Writer& u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i)));
        return *this;
    }
    Writer& u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i)));
        return *this;
    }
    Writer& bytes(const void* p, std::size_t n) {
        auto* c = static_cast<const std::uint8_t*>(p);
        b.insert(b.end(), c, c + n);
        return *this;
    }
    Writer& str16(const std::string& s) {
        std::size_t n = std::min<std::size_t>(s.size(), 0xFFFF);
        u16(static_cast<std::uint16_t>(n));
        return bytes(s.data(), n);
    }
    Writer& str32(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        return bytes(s.data(), s.size());
    }
};

struct Reader {
    const std::uint8_t* p;
    std::size_t n;
    bool ok = true;
    Reader(const std::uint8_t* d, std::size_t len) : p(d), n(len) {}
    bool need(std::size_t k) {
        if (!ok || n < k) ok = false;
        return ok;
    }
    std::uint8_t u8() {
        if (!need(1)) return 0;
        std::uint8_t v = *p;
        ++p;
        --n;
        return v;
    }
    std::uint16_t u16() {
        std::uint16_t lo = u8();
        return static_cast<std::uint16_t>(lo | (u8() << 8));
    }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(u8()) << (8 * i);
        return v;
    }
    std::uint64_t u64() {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(u8()) << (8 * i);
        return v;
    }
    void bytes(void* out, std::size_t k) {
        if (!need(k)) {
            std::memset(out, 0, k);
            return;
        }
        std::memcpy(out, p, k);
        p += k;
        n -= k;
    }
    std::string str(std::size_t len) {
        if (!need(len)) return {};
        std::string s(reinterpret_cast<const char*>(p), len);
        p += len;
        n -= len;
        return s;
    }
    std::string str16() { return str(u16()); }
    std::string str32() { return str(u32()); }
};

// HELLO's name lists: '\n'-separated (names never hold one), at most ~16 KiB.
std::string join_names(const std::vector<std::string>& names) {
    std::string s;
    for (const std::string& n : names) {
        if (s.size() + n.size() + 1 > 16384) break;
        if (!s.empty()) s += '\n';
        s += n;
    }
    return s;
}
std::vector<std::string> split_names(const std::string& s) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t end = s.find('\n', pos);
        if (end == std::string::npos) end = s.size();
        if (end > pos) out.push_back(s.substr(pos, end - pos));
        pos = end + 1;
    }
    return out;
}

std::string sanitize_text(const std::string& s, std::size_t max);

void write_roster(Writer& w, const std::vector<RosterEntry>& r) {
    w.u8(static_cast<std::uint8_t>(r.size()));
    for (const auto& e : r) {
        w.u8(static_cast<std::uint8_t>(e.slot)).str16(e.name).u8(static_cast<std::uint8_t>(e.state));
        w.u8(e.connected ? 1 : 0).u32(e.map_id).u32(e.ping_ms);
    }
}

std::vector<RosterEntry> read_roster(Reader& r) {
    std::vector<RosterEntry> out;
    unsigned n = r.u8();
    for (unsigned i = 0; i < n && r.ok; ++i) {
        RosterEntry e;
        e.slot = r.u8();
        e.name = sanitize_text(r.str16(), 16);
        std::uint8_t st = r.u8();
        e.state = st <= 6 ? static_cast<MemberState>(st) : MemberState::Title;
        e.connected = r.u8() != 0;
        e.map_id = r.u32();
        e.ping_ms = r.u32();
        // At most a party's worth of entries, each a real slot.
        if (e.slot <= 7 && out.size() < kMaxRosterEntries) out.push_back(std::move(e));
    }
    return out;
}

// VITALS: u8 count, then per entry u8 slot, u8 flags, u32 hp, u32 max hp, u16 stamina,
// u16 max stamina, u32 map, f32 x, y, z, yaw (IEEE bits, LE). 36 bytes an entry.
std::uint32_t float_bits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
float bits_float(std::uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

void write_vitals(Writer& w, const MemberVitals* v, int n) {
    n = std::clamp(n, 0, kMaxVitals);
    w.u8(static_cast<std::uint8_t>(n));
    for (int i = 0; i < n; ++i) {
        const MemberVitals& e = v[i];
        w.u8(static_cast<std::uint8_t>(e.slot)).u8(e.flags).u32(e.hp).u32(e.max_hp).u16(e.stamina).u16(e.max_stamina);
        w.u32(e.map_id).u32(float_bits(e.pos[0])).u32(float_bits(e.pos[1])).u32(float_bits(e.pos[2]));
        w.u32(float_bits(e.yaw));
    }
    // Trailer (overhead display): per entry, in the same order, u16 level, u16 deaths, u8 rot.
    // Older readers stop after the entries; a frame without it reads as all zeros.
    w.u8(static_cast<std::uint8_t>(n));
    for (int i = 0; i < n; ++i) w.u16(v[i].level).u16(v[i].deaths).u8(v[i].rot);
}

// The trailer of write_vitals for `n` entries just read (the first `n` of `e`, in frame order;
// absent from older peers: then nothing changes and the reader stays ok).
void read_vitals_extras(Reader& r, MemberVitals* e, int n) {
    if (r.n < 1) return;
    Reader t = r;
    const unsigned k = t.u8();
    for (unsigned i = 0; i < k && t.ok; ++i) {
        const std::uint16_t level = t.u16(), deaths = t.u16();
        const std::uint8_t rot = t.u8();
        if (!t.ok) break;
        if (static_cast<int>(i) < n) {
            e[i].level = std::min<std::uint16_t>(level, 999);
            e[i].deaths = deaths;
            e[i].rot = std::min<std::uint8_t>(rot, 5);
        }
    }
}

// One entry as sent (not yet sanitized); false when the frame is short.
bool read_vitals_entry(Reader& r, MemberVitals& e) {
    e.slot = r.u8();
    e.flags = r.u8();
    e.hp = r.u32();
    e.max_hp = r.u32();
    e.stamina = r.u16();
    e.max_stamina = r.u16();
    e.map_id = r.u32();
    for (float& c : e.pos) c = bits_float(r.u32());
    e.yaw = bits_float(r.u32());
    return r.ok;
}

// At most `max` entries (each sanitized; bad slots dropped); returns how many went to `out`
// (0 for a short frame).
int read_vitals(Reader& r, MemberVitals* out, int max) {
    const unsigned n = r.u8();
    MemberVitals all[kMaxVitals];
    unsigned read = 0;
    for (unsigned i = 0; i < n && i < static_cast<unsigned>(kMaxVitals); ++i, ++read) {
        if (!read_vitals_entry(r, all[i])) return 0;
    }
    if (!r.ok) return 0;
    if (n <= static_cast<unsigned>(kMaxVitals)) read_vitals_extras(r, all, static_cast<int>(read));
    int got = 0;
    for (unsigned i = 0; i < read; ++i) {
        MemberVitals e = all[i];
        if (sanitize_vitals(e) && got < max) out[got++] = e;
    }
    return got;
}

using Token = std::array<std::uint8_t, 16>;

bool is_zero(const Token& t) {
    for (auto b : t)
        if (b) return false;
    return true;
}

// Printable ASCII only, at most `max` bytes: a peer's text (REJECT / BYE reasons, roster names)
// goes to logs and the overlay.
std::string sanitize_text(const std::string& s, std::size_t max) {
    std::string out;
    for (char c : s) {
        if (out.size() >= max) break;
        const unsigned char u = static_cast<unsigned char>(c);
        out += u >= 0x20 && u < 0x7f ? c : '?';
    }
    return out;
}

bool valid_event_name(const std::string& s) {
    if (s.empty() || s.size() > kMaxEventName) return false;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7f) return false;
    }
    return true;
}

// A token bucket: `rate` per second, at most `burst` saved up.
struct Bucket {
    double tokens = 0, rate = 0, burst = 0;
    Clock::time_point at{};
    void init(double r, double b) {
        rate = r;
        burst = b;
        tokens = b;
        at = Clock::now();
    }
    bool take(double n, Clock::time_point now) {
        const double dt = std::chrono::duration<double>(now - at).count();
        at = now;
        tokens = std::min(burst, tokens + dt * rate);
        if (tokens < n) return false;
        tokens -= n;
        return true;
    }
};

// Reliable events in one direction: the sender's unacked queue and the receiver's last cursor.
struct EventStream {
    struct Ev {
        std::uint64_t cursor;
        std::string name, json;
    };
    std::deque<Ev> unacked;
    std::uint64_t next_cursor = 1;
    std::uint64_t rx_last = 0;
    void reset() {
        unacked.clear();
        next_cursor = 1;
        rx_last = 0;
    }
};

}  // namespace

const char* member_state_name(MemberState s) {
    switch (s) {
    case MemberState::Title: return "title";
    case MemberState::Home: return "home";
    case MemberState::Joining: return "joining";
    case MemberState::InHostWorld: return "in_host_world";
    case MemberState::Dead: return "dead";
    case MemberState::Loading: return "loading";
    case MemberState::Prologue: return "prologue";
    }
    return "?";
}

const char* link_state_name(LinkState s) {
    switch (s) {
    case LinkState::Idle: return "idle";
    case LinkState::Hosting: return "hosting";
    case LinkState::Connecting: return "connecting";
    case LinkState::Connected: return "connected";
    case LinkState::Reconnecting: return "reconnecting";
    case LinkState::Rejected: return "rejected";
    case LinkState::Stopped: return "stopped";
    }
    return "?";
}

namespace {

std::string short_hex(const std::array<std::uint8_t, 32>& h) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 4; ++i) {
        s += d[h[i] >> 4];
        s += d[h[i] & 15];
    }
    return s;
}

bool all_zero(const std::array<std::uint8_t, 32>& h) {
    for (std::uint8_t b : h)
        if (b) return false;
    return true;
}

// "A, B, C +2 more" (at most `limit` names).
std::string name_list(const std::vector<std::string>& names, std::size_t limit = 6) {
    std::string s;
    for (std::size_t i = 0; i < names.size() && i < limit; ++i) s += (i ? ", " : "") + names[i];
    if (names.size() > limit) s += " +" + std::to_string(names.size() - limit) + " more";
    return s;
}

// One clause about a differing set: what only one side has, or that the contents differ.
std::string set_clause(const char* what, const std::array<std::uint8_t, 32>& host_hash,
                       const std::vector<std::string>& host_names, const std::array<std::uint8_t, 32>& guest_hash,
                       const std::vector<std::string>& guest_names) {
    std::vector<std::string> host_only, guest_only;
    for (const std::string& n : host_names)
        if (std::find(guest_names.begin(), guest_names.end(), n) == guest_names.end()) host_only.push_back(n);
    for (const std::string& n : guest_names)
        if (std::find(host_names.begin(), host_names.end(), n) == host_names.end()) guest_only.push_back(n);
    std::string s = std::string(what) + " differ (";
    if (host_only.empty() && guest_only.empty()) {
        if (all_zero(host_hash) || all_zero(guest_hash))
            s += all_zero(host_hash) ? "the host has none" : "you have none";
        else
            s += "same names, different contents: another version of them";
        s += "; host " + short_hex(host_hash) + ", yours " + short_hex(guest_hash);
    } else {
        s += "host only: " + (host_only.empty() ? std::string("-") : name_list(host_only));
        s += "; yours only: " + (guest_only.empty() ? std::string("-") : name_list(guest_only));
    }
    return s + ")";
}

}  // namespace

std::string host_no_answer_hint(double since_host_gone_s, bool auto_punch) {
    if (since_host_gone_s >= 0 && since_host_gone_s < kHostBackWaitS) {
        char b[160];
        std::snprintf(b, sizeof b, "the host left %.0f s ago (restarting?): waiting for the host to come back",
                      since_host_gone_s);
        return b;
    }
    // Never connected: first the likely causes on the host's side, then what punching can still do.
    const std::string causes =
        "the host is not reachable from here: its port is not open to the internet (no UPnP or port forward, a "
        "firewall, or its provider's CGNAT; the host's NAT line says which), or its game is not hosting yet; ";
    if (auto_punch)
        return causes + "behind a NAT the host finds you through the DHT if it has the DHT option on too; else it "
                        "needs your guest code";
    return causes + "behind a NAT: automatic punching is off: send the host your guest code, or turn on the DHT "
                    "option on both PCs";
}

UdpFallbackSocket udp_fallback_socket(bool external_transport, double heard_on_party_port_s_ago) {
    if (!external_transport) return UdpFallbackSocket::Ephemeral;  // the link's own socket anyway
    return heard_on_party_port_s_ago >= 0 && heard_on_party_port_s_ago < kPartyPortHeardS ? UdpFallbackSocket::PartyPort
                                                                                         : UdpFallbackSocket::Ephemeral;
}

std::string auth_reject_reason(bool host_has_password) {
    if (host_has_password)
        return "wrong password, or a party code from another hosting session";
    return "party code from another hosting session: ask the host for the current one (no password needed)";
}

std::string identity_mismatch(const LinkConfig& host, const LinkConfig& guest) {
    std::vector<std::string> parts;
    // Each clause: what differs (host's / yours) and what to do about it.
    if (host.check_build && host.build_id != guest.build_id)
        parts.push_back("bbport build differs (host " + (host.build_id.empty() ? std::string("unknown") : host.build_id) +
                        ", yours " + (guest.build_id.empty() ? std::string("unknown: an older build") : guest.build_id) +
                        "): install the same bbport release as the host");
    if (host.eboot_sha256 != guest.eboot_sha256)
        parts.push_back("eboot.bin differs (host " + short_hex(host.eboot_sha256) + ", yours " +
                        short_hex(guest.eboot_sha256) + ": another game version or dump; the port needs 1.09)");
    if (host.params_sha256 != guest.params_sha256)
        parts.push_back("game params differ (gameparam.parambnd.dcx: host " + short_hex(host.params_sha256) +
                        ", yours " + short_hex(guest.params_sha256) +
                        "): another game version or edited params; use the same game dump (1.09) as the host");
    if (host.patches_hash != guest.patches_hash)
        parts.push_back(set_clause("gameplay patches", host.patches_hash, host.patch_names, guest.patches_hash,
                                   guest.patch_names) +
                        ": turn on the same patches as the host (launcher Patches tab)");
    if (host.mods_hash != guest.mods_hash)
        parts.push_back(set_clause("gameplay mods", host.mods_hash, host.mod_names, guest.mods_hash, guest.mod_names) +
                        ": enable the same mods as the host (launcher Mods tab)");
    if (host.hooks_hash != guest.hooks_hash)
        parts.push_back(set_clause("party patches", host.hooks_hash, host.hook_names, guest.hooks_hash,
                                   guest.hook_names) +
                        ": use the same bbport build and party options (BB_PARTY_* switches) as the host");
    if (host.rules != guest.rules)
        parts.push_back("party rules differ (host " + (host.rules.empty() ? std::string("none") : host.rules) +
                        "; yours " + (guest.rules.empty() ? std::string("none: an older build") : guest.rules) +
                        "): every player needs the same Max players setting (BB_PARTY_MAX) and BB_PARTY_FOURP*");
    if (parts.empty()) return {};
    // Both versions up front (the build clause already names them when the builds differ).
    std::string s = "version mismatch with the host";
    if (!(host.check_build && host.build_id != guest.build_id))
        s += " (host bbport " + (host.build_id.empty() ? std::string("unknown") : host.build_id) + ", yours " +
             (guest.build_id.empty() ? std::string("unknown: an older build") : guest.build_id) + ")";
    s += ": ";
    for (std::size_t i = 0; i < parts.size(); ++i) s += (i ? "; " : "") + parts[i];
    return s;
}

bool diagnostic_hook_name(const std::string& name) {
    std::string n;
    for (char c : name) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* w : {"probe", "trace", "(log)", " log", "log (", "observer", "fixture", "test"})
        if (n.find(w) != std::string::npos) return true;
    return false;
}

std::array<std::uint8_t, 32> hook_set_hash(std::vector<std::string> names) {
    std::array<std::uint8_t, 32> h{};
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    if (names.empty()) return h;
    std::string blob("bbparty-hooks-v1\0", 17);
    for (const std::string& n : names) blob += n + '\n';
    const auto k = crypto::blake2b256(blob.data(), blob.size());
    std::memcpy(h.data(), k.data(), 32);
    return h;
}

bool parse_identity_file(const std::string& text, const std::string& item_key, std::array<std::uint8_t, 32>* hash,
                         std::vector<std::string>* items) {
    bool have = false;
    std::array<std::uint8_t, 32> h{};
    std::vector<std::string> names;
    std::size_t pos = 0;
    auto nibble = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const std::size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        const std::string key = line.substr(0, sp), value = line.substr(sp + 1);
        if (key == "hash") {
            if (value.size() != 64) return false;
            for (int i = 0; i < 32; ++i) {
                const int hi = nibble(value[2 * i]), lo = nibble(value[2 * i + 1]);
                if (hi < 0 || lo < 0) return false;
                h[i] = static_cast<std::uint8_t>(hi << 4 | lo);
            }
            have = true;
        } else if (key == item_key && !value.empty()) {
            names.push_back(value);
        }
    }
    if (!have) return false;
    if (hash) *hash = h;
    if (items) *items = std::move(names);
    return true;
}

bool valid_member_name(const std::string& name) {
    if (name.empty() || name.size() > 16) return false;
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return true;
}

bool sanitize_vitals(MemberVitals& v) {
    if (v.slot < 0 || v.slot >= kMaxVitals) return false;
    v.flags &= kVitalsKnownFlags;
    v.max_hp = std::min(v.max_hp, kMaxVitalsHp);
    v.hp = std::min(v.hp, v.max_hp);
    v.max_stamina = static_cast<std::uint16_t>(std::min<std::uint32_t>(v.max_stamina, kMaxVitalsStamina));
    v.stamina = std::min(v.stamina, v.max_stamina);
    if (!(v.flags & kVitalsValid)) {
        v.hp = v.max_hp = 0;
        v.stamina = v.max_stamina = 0;
        v.flags &= static_cast<std::uint8_t>(~kVitalsAlive);
    } else if (v.hp == 0) {
        v.flags &= static_cast<std::uint8_t>(~kVitalsAlive);
    }
    bool pos_ok = std::isfinite(v.yaw);
    for (float c : v.pos) pos_ok = pos_ok && std::isfinite(c) && std::fabs(c) <= kMaxVitalsCoord;
    if (!pos_ok) {
        v.flags &= static_cast<std::uint8_t>(~kVitalsHasPos);
        v.pos[0] = v.pos[1] = v.pos[2] = 0;
        v.yaw = 0;
    } else {
        v.yaw = std::remainder(v.yaw, 6.28318530718f);  // [-pi, pi]
    }
    v.age_ms = 0;
    return true;
}

void apply_link_env(LinkConfig& cfg) {
    if (const char* v = std::getenv("BB_PARTY_PORT"); v && *v) {
        long p = std::strtol(v, nullptr, 10);
        if (p > 0 && p <= 65535) cfg.port = static_cast<std::uint16_t>(p);
    }
    if (const char* v = std::getenv("BB_PARTY_MAX"); v && *v) {
        long m = std::strtol(v, nullptr, 10);
        cfg.max_players = static_cast<int>(std::clamp<long>(m, 2, kMaxPartyPlayers));
    }
    if (const char* v = std::getenv("BB_PARTY_PASSWORD"); v && *v) cfg.password = v;
    // F5: how long a host keeps a silent member's slot (and its events) for a resume.
    if (const char* v = std::getenv("BB_PARTY_SLOT_KEEP_MS"); v && *v) {
        long ms = std::strtol(v, nullptr, 10);
        if (ms > 0) cfg.slot_keep_ms = static_cast<int>(std::clamp<long>(ms, 1000, 600000));
    }
}

struct PartyLink::Impl {
    enum class Phase { Connecting, WaitHello, WaitAuth, WaitChallenge, WaitWelcome, Open };

    struct Conn {
        std::uint64_t id = 0;
        sock::Socket s = sock::kInvalid;
        Phase phase = Phase::WaitHello;
        std::vector<std::uint8_t> in, out;
        std::size_t out_off = 0;
        crypto::Aead tx, rx;
        Nonce hn{}, gn{};
        Key pending_tx{}, pending_rx{};  // guest: keys derived at AUTH, armed at WELCOME
        bool password_only = false;      // host: the guest proved the password with a plain address (no code secret)
        Clock::time_point created, last_rx, last_ping;
        bool dead = false;
        bool close_after_flush = false;
        int slot = -1;  // host side, after WELCOME
        std::array<std::uint8_t, 4> peer_ip{};
        std::uint16_t peer_port = 0;
        // Host: from HELLO.
        std::string name;
        Token token{};
        // Peer metering (bbport security pass). backlog: this connection's frames posted to the
        // callback thread and not run yet (shared: a callback may outlive the connection).
        std::shared_ptr<std::atomic<int>> backlog = std::make_shared<std::atomic<int>>(0);
        bool input_pending = false;  // complete frames left in `in` (kFramesPerPass)
        Bucket frames, bytes;        // host: the member's rate (admit)
        std::uint32_t flood_drops = 0;
        std::uint64_t flood_total = 0;
        // UDP path: the connection is a reliable stream over UDP instead of the TCP socket `s`.
        std::shared_ptr<rudp::Stream> us;
        // Network simulator (BB_NET_SIM, netsim.h): received bytes wait here until due.
        std::unique_ptr<bbnet::netsim::StreamDelay> sim;
        std::deque<std::pair<Clock::time_point, std::vector<std::uint8_t>>> held;
    };

    // Attaches the network simulator to a connection with a loopback peer (or any, any=1).
    void attach_sim(Conn* c) {
        if (!bbnet::netsim::enabled()) return;
        const bbnet::netsim::Config sc = bbnet::netsim::config();
        std::uint32_t ip = 0;
        std::memcpy(&ip, c->peer_ip.data(), 4);
        if (!bbnet::netsim::applies_to(sc, ip)) return;
        c->sim = std::make_unique<bbnet::netsim::StreamDelay>(sc, bbnet::netsim::epoch(), c->id);
        if (!sim_logged) {
            sim_logged = true;
            log("party link: network simulator on this link (" + bbnet::netsim::describe(sc) + ")");
        }
    }
    bool sim_logged = false;
    // Moves the held bytes that are due into `in`; true when any moved.
    bool release_held(Conn* c, Clock::time_point now) {
        bool moved = false;
        while (!c->held.empty() && c->held.front().first <= now) {
            auto& b = c->held.front().second;
            c->in.insert(c->in.end(), b.begin(), b.end());
            c->held.pop_front();
            moved = true;
        }
        return moved;
    }

    struct Member {
        int slot = 0;
        std::string name;
        Token token{};
        Conn* conn = nullptr;
        Clock::time_point lost_at;
        MemberState state = MemberState::Joining;
        std::uint32_t map_id = 0;
        std::uint32_t ping_ms = 0;
        EventStream ev;
        bool restored = false;  // from restore_members: the previous host process's member
        MemberVitals vitals;    // party HUD: the member's last VITALS (has_vitals)
        bool has_vitals = false;
        Clock::time_point vitals_at{};
    };

    struct Pending {
        Clock::time_point deadline;
        bool sync = false;
        bool done = false;
        bool ok = false;
        std::string reply;
        std::function<void(bool, const std::string&)> cb;
    };

    LinkConfig cfg;
    LinkCallbacks cb;
    Key party_key{};
    // Host with a password: the key of a guest that joined by plain host:port (no code, so no
    // secret: party_code.h "the password alone then keys the party"). Zero when no password.
    Key password_key{};
    bool host = false;

    mutable std::mutex mu;
    mutable std::condition_variable state_cv;
    LinkState state = LinkState::Idle;
    bool stopping = false;
    std::thread io_thread;
    Clock::time_point start_time = Clock::now();
    Clock::time_point freeze_until{};
    std::uint64_t next_conn_id = 1;
    std::vector<std::unique_ptr<Conn>> conns;

    // Local roster entry.
    MemberState local_state = MemberState::Title;
    std::uint32_t local_map = 0;

    // Host.
    sock::Socket listen_s = sock::kInvalid;
    std::uint16_t port = 0;
    std::map<int, Member> members;
    Clock::time_point last_roster_bcast{};
    std::set<std::string> banned;  // kicked names: refused for the rest of this session
    AuthThrottle auth_throttle;    // wrong AUTH proofs per address (password guessing)
    std::uint64_t locked_refusals = 0;
    std::uint64_t refused_conns = 0;

    // UDP path (party_rudp.h).
    rudp::Mux umux{static_cast<std::uint32_t>(Clock::now().time_since_epoch().count())};
    sock::Socket usock = sock::kInvalid;  // own UDP socket (no external transport)
    std::uint16_t uport = 0;
    PartyLink::UdpSend uext;
    std::function<int(std::uint32_t, std::uint16_t)> route_hint;  // set_udp_route_hint
    // Guest with uext: endpoints whose UDP fallback runs on usock (an ephemeral port, not the
    // party port: udp_fallback_socket), and when each endpoint was last heard on the party port.
    std::set<std::uint64_t> own_peers;
    std::map<std::uint64_t, Clock::time_point> ext_heard;
    std::mutex uin_mu;
    std::deque<std::pair<rudp::Endpoint, std::vector<std::uint8_t>>> uin;  // from on_udp_datagram
    bool udp_prefer = false;  // guest: the last working connection was UDP: dial it at once

    // Guest.
    std::string host_name;
    std::uint16_t host_port = 0;
    Conn* gconn = nullptr;
    int my_slot = -1;
    bool last_resumed = false;
    int host_max_players = 0;
    Token my_token{};
    EventStream gev;
    std::vector<RosterEntry> groster;
    std::array<MemberVitals, kMaxVitals> gvitals{};      // by slot, from the host's VITALS
    std::array<Clock::time_point, kMaxVitals> gvitals_at{};
    std::array<bool, kMaxVitals> ghas_vitals{};
    std::map<std::uint32_t, Pending> pending;
    std::uint32_t next_rpc = 1;
    int backoff_ms = 0;
    Clock::time_point next_attempt{};
    bool want_connect = false;  // timers: a reconnect is due (dialed outside mu)
    std::int64_t hold_max_us = 0;       // the IO thread's longest hold of mu (debug_lock_hold_max_us)
    std::uint64_t hold_max_cycles = 0;  // the same hold in the IO thread's CPU cycles (Windows)

    // What the game's main thread polls every frame (state, roster, local slot, its own entry),
    // published under snap_mu: those getters never wait for mu, which the IO thread holds while
    // it works (and may be preempted holding). Lock order: mu, then snap_mu.
    mutable std::mutex snap_mu;
    LinkState snap_state = LinkState::Idle;
    int snap_slot = -1;
    std::vector<RosterEntry> snap_roster;
    MemberState snap_local_state = MemberState::Title;
    std::uint32_t snap_local_map = 0;
    int snap_max_players = 0;  // host: cfg.max_players; guest: the host's (WELCOME), 0 before it
    // Party HUD. Ours: written by set_local_vitals (main thread), seq +1 per change. Others':
    // published from the members table (host) / the host's VITALS (guest).
    MemberVitals snap_local_vitals;
    bool snap_local_vitals_set = false;
    std::uint64_t snap_local_vitals_seq = 0;
    std::array<MemberVitals, kMaxVitals> snap_vitals{};
    std::array<Clock::time_point, kMaxVitals> snap_vitals_at{};
    int snap_vitals_n = 0;
    // IO thread: what was sent last.
    std::uint64_t sent_vitals_seq = 0;
    Clock::time_point last_vitals_send{};
    bool host_vitals_dirty = false;  // host: a member's VITALS arrived since the last broadcast
    void publish() {  // mu held
        std::vector<RosterEntry> r = host ? host_roster() : groster;
        std::array<MemberVitals, kMaxVitals> pv{};
        std::array<Clock::time_point, kMaxVitals> pat{};
        int pn = 0;
        if (host) {
            for (const auto& [slot, m] : members)
                if (m.has_vitals && pn < kMaxVitals) {
                    pv[pn] = m.vitals;
                    pat[pn++] = m.vitals_at;
                }
        } else {
            for (int i = 0; i < kMaxVitals; ++i)
                if (ghas_vitals[i] && i != my_slot) {
                    pv[pn] = gvitals[i];
                    pat[pn++] = gvitals_at[i];
                }
        }
        std::lock_guard<std::mutex> lk(snap_mu);
        snap_vitals = pv;
        snap_vitals_at = pat;
        snap_vitals_n = pn;
        snap_state = state;
        snap_slot = host ? kHostSlot : my_slot;
        snap_roster.swap(r);
        snap_local_state = local_state;
        snap_local_map = local_map;
        snap_max_players = host ? cfg.max_players : host_max_players;
    }
    // post_event: events the main thread could not hand over without waiting for mu.
    struct Posted {
        int slot;
        std::string name, json;
    };
    std::mutex post_mu;
    std::deque<Posted> posted;
    std::atomic<bool> any_posted{false};
    std::uint64_t send_event_locked(int slot, const std::string& name, const std::string& json);  // mu held
    void drain_posted() {  // mu held
        if (!any_posted.load(std::memory_order_acquire)) return;
        std::deque<Posted> q;
        {
            std::lock_guard<std::mutex> pl(post_mu);
            q.swap(posted);
            any_posted.store(false, std::memory_order_release);
        }
        for (const Posted& p : q) send_event_locked(p.slot, p.name, p.json);
    }
    std::int64_t clock_offset = 0;
    std::uint32_t rtt = 0;
    std::string observed;
    RejectCode reject = RejectCode::None;
    std::string reject_text;

    // Callback thread.
    std::mutex dmu;
    std::condition_variable dcv;
    std::deque<std::function<void()>> dq;
    bool dstop = false;
    std::thread dthread;

    Impl(LinkConfig c, LinkCallbacks k) : cfg(std::move(c)), cb(std::move(k)) {
        cfg.max_players = std::clamp(cfg.max_players, 2, kMaxPartyPlayers);
    }

    // ---------- callback thread ----------
    void post(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> lk(dmu);
            dq.push_back(std::move(f));
        }
        dcv.notify_one();
    }
    void dispatch_loop() {
        for (;;) {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> lk(dmu);
                dcv.wait(lk, [&] { return dstop || !dq.empty(); });
                if (dq.empty()) return;
                f = std::move(dq.front());
                dq.pop_front();
            }
            f();
        }
    }
    void log(std::string line) {
        if (cb.on_log) post([this, line = std::move(line)] { cb.on_log(line); });
    }
    // A callback a peer's frame asked for: counted in the connection's backlog until it ran.
    void post_peer(Conn* c, std::function<void()> f) {
        std::shared_ptr<std::atomic<int>> b = c->backlog;
        b->fetch_add(1);
        post([b, f = std::move(f)] {
            f();
            b->fetch_sub(1);
        });
    }
    void set_state(LinkState s, std::string detail) {  // mu held
        if (state == s) return;
        state = s;
        {
            std::lock_guard<std::mutex> lk(snap_mu);
            snap_state = s;
        }
        state_cv.notify_all();
        log(std::string("party link: ") + link_state_name(s) + (detail.empty() ? "" : ": " + detail));
        if (cb.on_state) post([this, s, detail = std::move(detail)] { cb.on_state(s, detail); });
    }
    std::int64_t now_ms() const { return ms_between(start_time, Clock::now()); }

    // ---------- framing ----------
    // While corked (> 0) frames are only queued; the corking code flushes once at the end, so
    // a burst (an event replay, a read full of events and their ACKs) is a few send() calls
    // instead of one per frame and the lock is held that much shorter.
    int corked = 0;
    struct Cork {
        Impl& i;
        explicit Cork(Impl& im) : i(im) { ++i.corked; }
        ~Cork() { --i.corked; }
    };

    void queue_frame(Conn* c, std::uint8_t type, const std::vector<std::uint8_t>& body) {
        if (c->dead) return;
        if (c->tx.ready()) {
            std::uint32_t len = static_cast<std::uint32_t>(1 + crypto::kMacSize + 1 + body.size());
            std::uint8_t hdr[5] = {static_cast<std::uint8_t>(len), static_cast<std::uint8_t>(len >> 8),
                                   static_cast<std::uint8_t>(len >> 16), static_cast<std::uint8_t>(len >> 24),
                                   kEncrypted};
            std::vector<std::uint8_t> pt;
            pt.reserve(1 + body.size());
            pt.push_back(type);
            pt.insert(pt.end(), body.begin(), body.end());
            std::size_t base = c->out.size();
            c->out.resize(base + 5 + crypto::kMacSize + pt.size());
            std::memcpy(c->out.data() + base, hdr, 5);
            c->tx.seal(c->out.data() + base + 5 + crypto::kMacSize, c->out.data() + base + 5, hdr, 5, pt.data(),
                       pt.size());
            crypto::wipe(pt.data(), pt.size());
        } else {
            std::uint32_t len = static_cast<std::uint32_t>(1 + body.size());
            Writer w;
            w.u32(len).u8(type).bytes(body.data(), body.size());
            c->out.insert(c->out.end(), w.b.begin(), w.b.end());
        }
        if (c->out.size() - c->out_off > kMaxOutBuffer) {
            lose(c, "peer is not reading (" + std::to_string((c->out.size() - c->out_off) >> 20) + " MiB queued)");
            return;
        }
        if (!corked) flush(c);
    }
    void send(Conn* c, std::uint8_t type, const Writer& w) { queue_frame(c, type, w.b); }

    void flush(Conn* c) {
        if (c->dead || c->phase == Phase::Connecting) return;
        if (freeze_until > Clock::now()) return;
        if (c->us) {
            if (c->out_off < c->out.size() && c->us->write(c->out.data() + c->out_off, c->out.size() - c->out_off)) {
                c->out.clear();
                c->out_off = 0;
            }
            if (c->out_off == c->out.size() && c->close_after_flush) close_conn(c);
            return;
        }
        while (c->out_off < c->out.size()) {
            long r = sock::send_some(c->s, c->out.data() + c->out_off, c->out.size() - c->out_off);
            if (r < 0) {
                if (sock::would_block(sock::last_error())) break;
                lose(c, "send failed");
                return;
            }
            c->out_off += static_cast<std::size_t>(r);
        }
        if (c->out_off == c->out.size()) {
            c->out.clear();
            c->out_off = 0;
            if (c->close_after_flush) close_conn(c);
        } else if (c->out_off > (1u << 20)) {
            c->out.erase(c->out.begin(), c->out.begin() + static_cast<std::ptrdiff_t>(c->out_off));
            c->out_off = 0;
        }
    }

    // ---------- UDP path ----------
    bool udp_enabled() const { return cfg.udp_link; }
    static std::uint64_t ep_key(const rudp::Endpoint& e) { return static_cast<std::uint64_t>(e.addr) << 16 | e.port; }
    void udp_out(const rudp::Datagram& d) {
        if (uext && !(usock != sock::kInvalid && own_peers.count(ep_key(d.to)))) {
            uext(d.to.addr, d.to.port, d.data.data(), d.data.size());
            return;
        }
        if (usock == sock::kInvalid) return;
        if (bbnet::netsim::config().nat) bbnet::netsim::nat_note_out(d.to.addr, d.to.port);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = d.to.addr;
        sa.sin_port = d.to.port;
        ::sendto(usock, reinterpret_cast<const char*>(d.data.data()), static_cast<int>(d.data.size()), 0,
                 reinterpret_cast<const sockaddr*>(&sa), sizeof sa);
    }
    bool open_udp_socket(std::uint16_t want, bool beside_uext = false) {  // mu held or before the threads
        if ((uext && !beside_uext) || usock != sock::kInvalid) return true;
        sock::Socket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == sock::kInvalid) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(want);
        if (sock::loopback_only() || cfg.bind_addr == "127.0.0.1") a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        else a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            sock::close(s);
            return false;
        }
        sock::set_nonblocking(s);
        sockaddr_in b{};
        sock::socklen bl = sizeof b;
        getsockname(s, reinterpret_cast<sockaddr*>(&b), &bl);
        usock = s;
        uport = ntohs(b.sin_port);
        return true;
    }
    // `external`: from the party port (on_udp_datagram), whose reader applied the simulated NAT to
    // the wire already: an IPv6 peer (its 240/4 key) or a guest a member relays (F2 0.7.1) arrives
    // from an endpoint we never sent to directly, and must not be filtered again here.
    void udp_in(rudp::Endpoint from, const std::uint8_t* d, std::size_t n, Clock::time_point now, bool external = false) {
        if (!external && bbnet::netsim::config().nat && !bbnet::netsim::nat_allow_in(from.addr, from.port)) return;
        umux.on_datagram(from, d, n, now);
    }
    // Guest: the TCP connect got no answer (or was refused): the same connection over UDP.
    bool switch_to_udp(Conn* c, const char* why) {
        if (host || c->us || !udp_enabled() || c->dead) return false;
        if (!uext && usock == sock::kInvalid && !open_udp_socket(0)) return false;
        rudp::Endpoint ep;
        std::memcpy(&ep.addr, c->peer_ip.data(), 4);
        ep.port = htons(c->peer_port);
        const auto heard = ext_heard.find(ep_key(ep));
        const double heard_s = heard == ext_heard.end() ? -1.0 : ms_between(heard->second, Clock::now()) / 1000.0;
        std::string via;
        const int routed = route_hint ? route_hint(ep.addr, ep.port) : 0;
        if (uext) {
            if (routed) {
                own_peers.erase(ep_key(ep));
                via = "; from the party port (its route table reaches the host another way)";
            } else if (udp_fallback_socket(true, heard_s) == UdpFallbackSocket::Ephemeral && open_udp_socket(0, true)) {
                if (own_peers.insert(ep_key(ep)).second)
                    via = "; from a separate UDP port " + std::to_string(uport) +
                          " until the host is heard on the party port (blind datagrams from the party port can "
                          "block that port's path to the host)";
            } else {
                if (own_peers.erase(ep_key(ep))) via = "; from the party port (the host was heard on it)";
            }
        }
        if (c->s != sock::kInvalid) {
            sock::close(c->s);
            c->s = sock::kInvalid;
        }
        c->us = umux.connect(ep, Clock::now(), cfg.connect_timeout_ms);
        c->created = c->last_rx = Clock::now();
        const double gone = since_host_gone_s();
        log(std::string("party link: ") + why + "; connecting over UDP (hole punching; " +
            (gone >= 0 && gone < kHostBackWaitS ? host_no_answer_hint(gone)
             : cfg.auto_punch ? std::string("the DHT announces us for a host behind a NAT; else give it your guest code")
                              : std::string("give the host your guest code if it is behind a NAT, or turn on the DHT "
                                            "option on both PCs")) +
            ")" + via);
        return true;
    }
    // Every pass (mu held): datagrams in, accepted streams, stream <-> connection bytes, datagrams out.
    void udp_pump() {
        const Clock::time_point now = Clock::now();
        if (usock != sock::kInvalid) {
            std::uint8_t buf[2048];
            for (int k = 0; k < 512; ++k) {
                sockaddr_in sa{};
                sock::socklen sl = sizeof sa;
                const long r = ::recvfrom(usock, reinterpret_cast<char*>(buf), sizeof buf, 0,
                                          reinterpret_cast<sockaddr*>(&sa), &sl);
                if (r <= 0) break;
                udp_in({sa.sin_addr.s_addr, sa.sin_port}, buf, static_cast<std::size_t>(r), now);
            }
        }
        {
            std::deque<std::pair<rudp::Endpoint, std::vector<std::uint8_t>>> q;
            {
                std::lock_guard<std::mutex> lk(uin_mu);
                q.swap(uin);
            }
            for (auto& [ep, d] : q) {
                if (!host) {
                    if (ext_heard.size() > 64) ext_heard.clear();
                    ext_heard[ep_key(ep)] = now;
                }
                udp_in(ep, d.data(), d.size(), now, true);
            }
        }
        if (host) {
            while (auto st = umux.accept()) {
                auto c = std::make_unique<Conn>();
                c->id = next_conn_id++;
                c->us = st;
                c->phase = Phase::WaitHello;
                c->created = c->last_rx = now;
                const rudp::Endpoint ep = st->peer();
                std::memcpy(c->peer_ip.data(), &ep.addr, 4);
                c->peer_port = ntohs(ep.port);
                log("party link: a guest connects over UDP from " + std::to_string(c->peer_ip[0]) + "." +
                    std::to_string(c->peer_ip[1]) + "." + std::to_string(c->peer_ip[2]) + "." +
                    std::to_string(c->peer_ip[3]) + ":" + std::to_string(c->peer_port));
                conns.push_back(std::move(c));
            }
        }
        for (std::size_t i = 0; i < conns.size(); ++i) {
            Conn* c = conns[i].get();
            if (c->dead || !c->us) continue;
            const auto st = c->us->state();
            if (st == rudp::Stream::State::Closed && c->phase != Phase::Connecting && c->us->readable()) {
                // The peer's last words came with its FIN (a BYE / REJECT): read them first.
                std::uint8_t buf[16384];
                for (std::size_t r; !c->input_pending && (r = c->us->read(buf, sizeof buf)) > 0;)
                    c->in.insert(c->in.end(), buf, buf + r);
                process_input(c);
                if (c->dead) continue;
            }
            if (st == rudp::Stream::State::Failed || st == rudp::Stream::State::Closed) {
                lose(c, c->phase == Phase::Connecting
                            ? "no UDP answer from the host (" + host_no_answer_hint(host ? -1 : since_host_gone_s(), cfg.auto_punch) + ")"
                            : std::string("UDP link closed"));
                continue;
            }
            if (c->phase == Phase::Connecting) {
                if (st != rudp::Stream::State::Open) continue;
                if (!host) udp_prefer = true;
                log("party link: connected over UDP");
                on_connected(c);
            }
            bool got = false;
            std::uint8_t buf[16384];
            while (!c->input_pending) {
                const std::size_t r = c->us->read(buf, sizeof buf);
                if (!r) break;
                c->in.insert(c->in.end(), buf, buf + r);
                got = true;
            }
            if (got) process_input(c);
            if (!c->dead && c->out_off < c->out.size()) flush(c);
        }
        std::vector<rudp::Datagram> out;
        umux.poll(now, out);
        for (const rudp::Datagram& d : out) udp_out(d);
    }

    void close_conn(Conn* c) {
        if (c->dead) return;
        c->dead = true;
        if (c->s != sock::kInvalid) {
            sock::close(c->s);
            c->s = sock::kInvalid;
        }
        if (c->us) {
            c->us->close();  // what was written, then a FIN, on the next pump
            umux.linger(std::move(c->us));
            c->us.reset();
        }
        c->tx.reset();
        c->rx.reset();
    }

    void reject_conn(Conn* c, RejectCode code, const std::string& reason, bool quiet = false) {
        if (!quiet) log("party link: rejecting " + (c->name.empty() ? std::string("connection") : c->name) + ": " + reason);
        Writer w;
        w.u8(static_cast<std::uint8_t>(code)).str16(reason);
        c->tx.reset();  // REJECT is always plaintext
        send(c, kReject, w);
        c->close_after_flush = true;
        c->phase = Phase::WaitHello;
        flush(c);
    }

    // A connection is gone (EOF, error, silence). mu held.
    void lose(Conn* c, const std::string& why) {
        if (c->dead) return;
        bool was_open = c->phase == Phase::Open;
        close_conn(c);
        if (host) {
            if (c->slot >= 0) {
                auto it = members.find(c->slot);
                if (it != members.end() && it->second.conn == c) {
                    Member& m = it->second;
                    m.conn = nullptr;
                    m.lost_at = Clock::now();
                    log("party link: lost " + m.name + " (slot " + std::to_string(m.slot) + "): " + why +
                        "; keeping the slot " + std::to_string(cfg.slot_keep_ms / 1000) + " s");
                    RosterEntry e = entry_of(m);
                    e.lost_why = why;
                    if (cb.on_member_left) post([this, e] { cb.on_member_left(e, true); });
                    broadcast_roster();
                }
            }
            return;
        }
        if (c != gconn) return;
        gconn = nullptr;
        fail_pending("connection to the host lost");
        if (stopping || state == LinkState::Rejected || state == LinkState::Stopped) return;
        if (was_open) note_host_gone();
        if (was_open || backoff_ms == 0) backoff_ms = cfg.backoff_initial_ms;
        int wait = backoff_ms;
        if (cfg.backoff_jitter_pct > 0) {
            std::uint32_t r = 0;
            crypto::random_bytes(reinterpret_cast<std::uint8_t*>(&r), sizeof r);
            const int span = backoff_ms * cfg.backoff_jitter_pct / 100;
            if (span > 0) wait += static_cast<int>(r % static_cast<std::uint32_t>(2 * span + 1)) - span;
        }
        next_attempt = Clock::now() + std::chrono::milliseconds(wait);
        set_state(LinkState::Reconnecting, why + "; retrying in " + std::to_string(wait) + " ms");
        backoff_ms = std::min(backoff_ms * 2, cfg.backoff_max_ms);
    }

    void fail_pending(const std::string& why) {
        for (auto it = pending.begin(); it != pending.end();) {
            Pending& p = it->second;
            if (p.sync) {
                p.done = true;
                p.ok = false;
                p.reply = why;
                ++it;
            } else {
                auto f = std::move(p.cb);
                if (f) post([f, why] { f(false, why); });
                it = pending.erase(it);
            }
        }
        state_cv.notify_all();
    }

    // ---------- roster ----------
    RosterEntry entry_of(const Member& m) const {
        RosterEntry e;
        e.slot = m.slot;
        e.name = m.name;
        e.state = m.state;
        e.connected = m.conn != nullptr;
        e.map_id = m.map_id;
        e.ping_ms = m.ping_ms;
        return e;
    }
    std::vector<RosterEntry> host_roster() const {
        std::vector<RosterEntry> r;
        RosterEntry me;
        me.slot = kHostSlot;
        me.name = cfg.name;
        me.state = local_state;
        me.connected = true;
        me.map_id = local_map;
        r.push_back(me);
        for (const auto& [slot, m] : members) r.push_back(entry_of(m));
        return r;
    }
    void broadcast_roster() {  // host, mu held
        last_roster_bcast = Clock::now();
        auto r = host_roster();
        Writer w;
        write_roster(w, r);
        for (auto& [slot, m] : members)
            if (m.conn && m.conn->phase == Phase::Open) send(m.conn, kRoster, w);
        if (cb.on_roster) post([this, r] { cb.on_roster(r); });
    }

    // ---------- events ----------
    void send_event_frame(Conn* c, const EventStream::Ev& e) {
        Writer w;
        w.u64(e.cursor).str16(e.name).str32(e.json);
        send(c, kEvent, w);
    }
    void on_event_frame(Conn* c, EventStream& es, int from_slot, Reader& r) {
        std::uint64_t cursor = r.u64();
        std::string name = r.str16();
        std::string json = r.str32();
        if (!r.ok) return;
        if (cursor > es.rx_last) {
            es.rx_last = cursor;
            if (!valid_event_name(name))
                log("party link: dropped an event with an invalid name from slot " + std::to_string(from_slot));
            else if (cb.on_event)
                post_peer(c, [this, from_slot, cursor, name = std::move(name), json = std::move(json)] {
                    cb.on_event(from_slot, cursor, name, json);
                });
        }
        Writer w;
        w.u64(cursor);
        send(c, kEventAck, w);
    }
    static void on_event_ack(EventStream& es, Reader& r) {
        std::uint64_t cursor = r.u64();
        while (!es.unacked.empty() && es.unacked.front().cursor <= cursor) es.unacked.pop_front();
    }

    // ---------- frame processing ----------
    void process_input(Conn* c) {
        {
            Cork cork(*this);
            process_frames(c);
        }
        // This connection's replies, and what the frames queued for others (a roster broadcast).
        for (auto& o : conns)
            if (!o->dead && o->out_off < o->out.size()) flush(o.get());
    }
    void process_frames(Conn* c) {
        std::size_t off = 0;
        std::size_t frames = 0;
        c->input_pending = false;
        while (!c->dead && c->in.size() - off >= 5) {
            if (frames++ >= kFramesPerPass || c->backlog->load() >= kMaxCallbackBacklog) {
                c->input_pending = true;  // the IO loop comes back for the rest
                break;
            }
            const std::uint8_t* h = c->in.data() + off;
            std::uint32_t len = static_cast<std::uint32_t>(h[0]) | (static_cast<std::uint32_t>(h[1]) << 8) |
                                (static_cast<std::uint32_t>(h[2]) << 16) | (static_cast<std::uint32_t>(h[3]) << 24);
            // Before WELCOME nobody is authenticated: a handshake frame is small.
            const std::uint32_t limit = c->phase == Phase::Open ? kMaxFrame : kMaxHandshakeFrame;
            if (len == 0 || len > limit) {
                lose(c, "bad frame length " + std::to_string(len));
                return;
            }
            if (c->in.size() - off < 4 + static_cast<std::size_t>(len)) break;
            std::uint8_t type = h[4];
            const std::uint8_t* body = h + 5;
            std::size_t body_len = len - 1;
            if (type == kEncrypted) {
                if (!c->rx.ready() || body_len < crypto::kMacSize + 1) {
                    lose(c, "unexpected encrypted frame");
                    return;
                }
                std::vector<std::uint8_t> pt(body_len - crypto::kMacSize);
                if (!c->rx.open(pt.data(), body, h, 5, body + crypto::kMacSize, pt.size())) {
                    lose(c, "frame failed authentication");
                    return;
                }
                handle(c, pt[0], pt.data() + 1, pt.size() - 1, true);
            } else {
                handle(c, type, body, body_len, false);
            }
            off += 4 + len;
        }
        if (!c->dead && off) c->in.erase(c->in.begin(), c->in.begin() + static_cast<std::ptrdiff_t>(off));
    }

    void handle(Conn* c, std::uint8_t type, const std::uint8_t* d, std::size_t n, bool enc) {
        if (c->close_after_flush) return;  // rejected / saying BYE: ignore the rest
        c->last_rx = Clock::now();
        Reader r(d, n);
        if (host)
            handle_host(c, type, r, enc);
        else
            handle_guest(c, type, r, enc);
    }

    void handle_common(Conn* c, std::uint8_t type, Reader& r) {
        if (type == kPing) {
            std::uint64_t t = r.u64();
            Writer w;
            w.u64(t).u64(static_cast<std::uint64_t>(host ? now_ms() : 0));
            send(c, kPong, w);
        }
    }

    // ---------- host ----------
    void handle_host(Conn* c, std::uint8_t type, Reader& r, bool enc) {
        if (c->phase == Phase::WaitHello) {
            if (type != kHello || enc) return lose(c, "expected HELLO");
            std::uint8_t magic[4];
            r.bytes(magic, 4);
            std::uint16_t ver = r.u16();
            if (!r.ok || std::memcmp(magic, kMagic, 4) != 0) return lose(c, "not a PartyLink client");
            {
                std::uint32_t ip = 0;
                std::memcpy(&ip, c->peer_ip.data(), 4);
                if (cfg.throttle_key) ip = cfg.throttle_key(ip, htons(c->peer_port));  // an IPv6 /64 counts as one address
                std::int64_t wait_ms = 0;
                bool global = false;
                if (cfg.auth_throttle && auth_throttle.Locked(ip, now_ms(), &wait_ms, &global)) {
                    ++locked_refusals;  // logged sparsely: a guesser may hammer the port
                    return reject_conn(c, RejectCode::Auth,
                                       std::string(global ? "the host got too many wrong passwords (from several "
                                                            "addresses) and takes no new players now"
                                                          : "too many wrong passwords from your address") +
                                           ": try again in " + std::to_string((wait_ms + 999) / 1000) + " s",
                                       locked_refusals != 1 && locked_refusals % 100 != 0);
                }
            }
            // The version before the rest: other versions lay HELLO out differently.
            if (ver != kLinkProtocolVersion)
                return reject_conn(c, RejectCode::Version,
                                   "different party protocol version (host " + std::to_string(kLinkProtocolVersion) +
                                       ", yours " + std::to_string(ver) + "): update the port");
            c->name = r.str16();
            LinkConfig theirs;
            r.bytes(theirs.eboot_sha256.data(), 32);
            r.bytes(theirs.mods_hash.data(), 32);
            r.bytes(c->token.data(), 16);
            r.bytes(theirs.patches_hash.data(), 32);
            theirs.patch_names = split_names(r.str16());
            theirs.mod_names = split_names(r.str16());
            if (r.ok && r.n) theirs.rules = r.str16();  // absent before the party rules existed
            if (r.ok && r.n) {  // A9 fingerprint: absent from older builds (they then differ)
                theirs.build_id = sanitize_text(r.str16(), 96);
                r.bytes(theirs.params_sha256.data(), 32);
                r.bytes(theirs.hooks_hash.data(), 32);
                theirs.hook_names = split_names(r.str16());
            }
            if (!r.ok) return lose(c, "short HELLO");
            const std::string mismatch = identity_mismatch(cfg, theirs);
            if (!mismatch.empty()) return reject_conn(c, RejectCode::Mismatch, mismatch);
            if (!valid_member_name(c->name))
                return reject_conn(c, RejectCode::Name, "invalid name (1-16 of A-Z a-z 0-9 _ -)");
            crypto::random_bytes(c->hn.data(), c->hn.size());
            Writer w;
            w.bytes(c->hn.data(), c->hn.size());
            send(c, kChallenge, w);
            c->phase = Phase::WaitAuth;
            return;
        }
        if (c->phase == Phase::WaitAuth) {
            if (type != kAuth || enc) return lose(c, "expected AUTH");
            std::uint8_t proof[32];
            r.bytes(c->gn.data(), c->gn.size());
            r.bytes(proof, 32);
            if (!r.ok) return lose(c, "short AUTH");
            Key expect = crypto::auth_proof(party_key, c->hn, c->gn);
            if (!crypto::equal32(expect.data(), proof)) {
                const Key by_password = crypto::auth_proof(password_key, c->hn, c->gn);
                if (cfg.password.empty() || !crypto::equal32(by_password.data(), proof)) {
                    std::uint32_t ip = 0;
                    std::memcpy(&ip, c->peer_ip.data(), 4);
                    if (cfg.throttle_key) ip = cfg.throttle_key(ip, htons(c->peer_port));
                    const std::uint64_t global_before = auth_throttle.global_lockouts();
                    if (cfg.auth_throttle && auth_throttle.Fail(ip, now_ms())) {
                        std::int64_t wait_ms = 0;
                        auth_throttle.Locked(ip, now_ms(), &wait_ms);
                        if (auth_throttle.global_lockouts() != global_before)
                            log("party link: more than " + std::to_string(kAuthGlobalFree) +
                                " wrong passwords in a minute from all addresses: new players refused for " +
                                std::to_string(wait_ms / 1000) + " s (members who were in still get in; global lockout " +
                                std::to_string(auth_throttle.global_lockouts()) + ")");
                        else
                            log("party link: " + format_peer(c) + " gave " + std::to_string(kAuthFailFree) +
                                "+ wrong passwords: refused for " + std::to_string(wait_ms / 1000) + " s (lockout " +
                                std::to_string(auth_throttle.lockouts()) + ")");
                    }
                    return reject_conn(c, RejectCode::Auth, auth_reject_reason(!cfg.password.empty()));
                }
                c->password_only = true;  // a plain address and the right password
            }
            {
                std::uint32_t ip = 0;
                std::memcpy(&ip, c->peer_ip.data(), 4);
                if (cfg.throttle_key) ip = cfg.throttle_key(ip, htons(c->peer_port));
                auth_throttle.Success(ip);
            }
            admit(c);
            return;
        }
        if (!enc) return lose(c, "plaintext frame after WELCOME");
        auto it = members.find(c->slot);
        if (it == members.end() || it->second.conn != c) return lose(c, "stale connection");
        Member& m = it->second;
        if (type != kPong && type != kEventAck && type != kBye) {
            const auto now = Clock::now();
            const bool fits = c->frames.take(1, now) && c->bytes.take(static_cast<double>(r.n), now);
            if (!fits) {
                ++c->flood_total;
                if (++c->flood_drops >= kFloodDisconnect)
                    return lose(c, "flooding the host (" + std::to_string(c->flood_total) +
                                       " frames over the rate limit)");
                if (c->flood_total == 1 || c->flood_total % 1000 == 0)
                    log("party link: " + m.name + " is over the rate limit; " + std::to_string(c->flood_total) +
                        " frames dropped");
                if (type == kRpcReq) {  // the caller is told rather than left to time out
                    const std::uint32_t id = r.u32();
                    Writer w;
                    w.u32(id).u8(0).str32("{\"ResKind\":8,\"Error\":\"rate limited\"}");
                    send(c, kRpcResp, w);
                }
                return;
            }
            c->flood_drops = 0;
        }
        switch (type) {
        case kPing: handle_common(c, type, r); break;
        case kPong: {
            // The echo of our own PING time: anything later than now (or absurd) is a lie, and
            // the subtraction must not overflow (fuzzing: a u64 near 2^63 did).
            std::uint64_t t = r.u64();
            const std::int64_t now = now_ms();
            if (r.ok && t <= static_cast<std::uint64_t>(now)) {
                const std::int64_t rtt_ms = now - static_cast<std::int64_t>(t);
                if (rtt_ms <= 3600000) m.ping_ms = static_cast<std::uint32_t>(rtt_ms);
            }
            break;
        }
        case kRoster: {  // the guest's own entry
            auto entries = read_roster(r);
            if (!r.ok || entries.empty()) break;
            if (entries[0].state != m.state || entries[0].map_id != m.map_id) {
                m.state = entries[0].state;
                m.map_id = entries[0].map_id;
                broadcast_roster();
            }
            break;
        }
        case kVitals: {  // the guest's own entry: its slot is the one it holds, whatever it says
            MemberVitals e;
            if (r.u8() != 1 || !read_vitals_entry(r, e)) break;
            read_vitals_extras(r, &e, 1);
            e.slot = m.slot;
            if (!sanitize_vitals(e)) break;
            m.vitals = e;
            m.has_vitals = true;
            m.vitals_at = Clock::now();
            host_vitals_dirty = true;
            break;
        }
        case kRpcReq: {
            std::uint32_t id = r.u32();
            std::string kind = r.str16();
            std::string json = r.str32();
            if (!r.ok) break;
            int slot = m.slot;
            std::uint64_t conn_id = c->id;
            post_peer(c, [this, slot, conn_id, id, kind = std::move(kind), json = std::move(json)] {
                bool ok = static_cast<bool>(cb.on_rpc);
                std::string reply = ok ? cb.on_rpc(slot, kind, json) : std::string("{\"error\":\"no rpc handler\"}");
                std::lock_guard<std::mutex> lk(mu);
                auto mit = members.find(slot);
                if (mit == members.end() || !mit->second.conn || mit->second.conn->id != conn_id) return;
                Writer w;
                w.u32(id).u8(ok ? 1 : 0).str32(reply);
                send(mit->second.conn, kRpcResp, w);
            });
            break;
        }
        case kEvent: on_event_frame(c, m.ev, m.slot, r); break;
        case kEventAck: on_event_ack(m.ev, r); break;
        case kPartyCmd: {
            std::string cmd = r.str16();
            std::string json = r.str32();
            int slot = m.slot;
            if (r.ok && cb.on_party_cmd)
                post_peer(c, [this, slot, cmd = std::move(cmd), json = std::move(json)] {
                    cb.on_party_cmd(slot, cmd, json);
                });
            break;
        }
        case kProgress: {
            std::string blob = r.str32();
            int slot = m.slot;
            if (r.ok && cb.on_progress)
                post_peer(c, [this, slot, b = std::vector<std::uint8_t>(blob.begin(), blob.end())] {
                    cb.on_progress(slot, b);
                });
            break;
        }
        case kBye: {
            log("party link: " + m.name + " left the party");
            release_member(m.slot, false);
            break;
        }
        default: break;
        }
    }

    void admit(Conn* c) {
        // Resume: the same token (or, for a restarted guest, the same name) on a kept slot.
        Member* m = nullptr;
        bool resumed = false;
        if (banned.count(c->name)) return reject_conn(c, RejectCode::Kicked, "kicked by the host");
        for (auto& [slot, mm] : members) {
            if (!is_zero(c->token) && mm.token == c->token) {
                m = &mm;
                break;
            }
        }
        // By name also while the old connection is not yet declared lost, when it comes from the
        // same address and has been silent for two ping intervals (a crashed guest restarted
        // before lost_timeout); a live player of the same name still gets RejectCode::Name.
        const auto silent = std::chrono::milliseconds(2 * cfg.ping_interval_ms);
        if (!m)
            for (auto& [slot, mm] : members)
                if (mm.name == c->name &&
                    (!mm.conn || (mm.conn->peer_ip == c->peer_ip && Clock::now() - mm.conn->last_rx > silent))) {
                    m = &mm;
                    break;
                }
        if (m) {
            if (m->conn) {  // the old connection has not timed out yet: replace it
                Conn* old = m->conn;
                m->conn = nullptr;
                close_conn(old);
            }
            resumed = !is_zero(c->token) && m->token == c->token && !m->restored;
            if (m->restored) {
                log("party link: " + c->name + " is back after a host restart (slot " + std::to_string(m->slot) +
                    ", " + (!is_zero(c->token) && m->token == c->token ? "by token" : "by name") + ")");
                m->restored = false;
            }
            if (!resumed) m->ev.reset();
        } else {
            for (auto& [slot, mm] : members)
                if (mm.name == c->name)
                    return reject_conn(c, RejectCode::Name, "the name " + c->name + " is already in the party");
            if (c->name == cfg.name)
                return reject_conn(c, RejectCode::Name, "the name " + c->name + " is already in the party");
            int slot = -1;
            for (int s = 1; s < cfg.max_players; ++s)
                if (!members.count(s)) {
                    slot = s;
                    break;
                }
            if (slot < 0)
                return reject_conn(c, RejectCode::Full,
                                   "the party is full (" + std::to_string(cfg.max_players) + " players)");
            Member nm;
            nm.slot = slot;
            nm.name = c->name;
            crypto::random_bytes(nm.token.data(), nm.token.size());
            m = &members.emplace(slot, std::move(nm)).first->second;
        }
        bool rejoined = m->conn == nullptr && (resumed || m->lost_at != Clock::time_point{});
        m->conn = c;
        m->state = MemberState::Joining;
        c->slot = m->slot;
        const Key& key = c->password_only ? password_key : party_key;
        c->tx.init(crypto::session_key(key, "bbp-h2g", c->hn, c->gn));
        c->rx.init(crypto::session_key(key, "bbp-g2h", c->hn, c->gn));
        c->phase = Phase::Open;
        c->last_ping = Clock::now();
        c->frames.init(kMemberFrameRate, kMemberFrameBurst);
        c->bytes.init(kMemberByteRate, kMemberByteBurst);

        Writer w;
        w.u8(static_cast<std::uint8_t>(m->slot)).u8(static_cast<std::uint8_t>(cfg.max_players)).u8(resumed ? 1 : 0);
        w.u64(static_cast<std::uint64_t>(now_ms()));
        w.bytes(c->peer_ip.data(), 4).u16(c->peer_port);
        w.bytes(m->token.data(), 16);
        write_roster(w, host_roster());
        send(c, kWelcome, w);
        for (const auto& e : m->ev.unacked) send_event_frame(c, e);
        log("party link: " + m->name + (rejoined ? " rejoined" : " joined") + " (slot " + std::to_string(m->slot) +
            ", from " + format_peer(c) + ")");
        RosterEntry e = entry_of(*m);
        if (cb.on_member_joined) post([this, e, rejoined] { cb.on_member_joined(e, rejoined); });
        broadcast_roster();
    }

    void release_member(int slot, bool notify_bye, RejectCode code = RejectCode::Kicked, const std::string& why = {}) {
        auto it = members.find(slot);
        if (it == members.end()) return;
        Member& m = it->second;
        if (m.conn) {
            if (notify_bye) {
                Writer w;
                w.u8(static_cast<std::uint8_t>(code)).str16(why);
                send(m.conn, kBye, w);
                m.conn->close_after_flush = true;
                flush(m.conn);
            } else {
                close_conn(m.conn);
            }
            m.conn->slot = -1;
        }
        RosterEntry e = entry_of(m);
        e.connected = false;
        members.erase(it);
        if (cb.on_member_left) post([this, e] { cb.on_member_left(e, false); });
        broadcast_roster();
    }

    static std::string format_peer(const Conn* c) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", c->peer_ip[0], c->peer_ip[1], c->peer_ip[2], c->peer_ip[3],
                      c->peer_port);
        return buf;
    }

    // ---------- guest ----------
    void handle_guest(Conn* c, std::uint8_t type, Reader& r, bool enc) {
        if (type == kReject && !enc) {
            RejectCode code = static_cast<RejectCode>(r.u8());
            std::string why = sanitize_text(r.str16(), 1024);
            reject = code;
            reject_text = why;
            close_conn(c);
            gconn = nullptr;
            fail_pending("rejected: " + why);
            set_state(LinkState::Rejected, why);
            return;
        }
        if (c->phase == Phase::WaitChallenge) {
            if (type != kChallenge || enc) return lose(c, "expected CHALLENGE");
            r.bytes(c->hn.data(), c->hn.size());
            if (!r.ok) return lose(c, "short CHALLENGE");
            crypto::random_bytes(c->gn.data(), c->gn.size());
            Key proof = crypto::auth_proof(party_key, c->hn, c->gn);
            Writer w;
            w.bytes(c->gn.data(), c->gn.size()).bytes(proof.data(), proof.size());
            send(c, kAuth, w);
            // The host answers with the first encrypted frame: arm the receive key now.
            c->rx.init(crypto::session_key(party_key, "bbp-h2g", c->hn, c->gn));
            c->pending_tx = crypto::session_key(party_key, "bbp-g2h", c->hn, c->gn);
            c->phase = Phase::WaitWelcome;
            return;
        }
        if (!enc) return lose(c, "unexpected plaintext frame");
        if (c->phase == Phase::WaitWelcome) {
            if (type != kWelcome) return lose(c, "expected WELCOME");
            int slot = r.u8();
            int maxp = r.u8();
            bool resumed = r.u8() != 0;
            std::uint64_t hclock = r.u64();
            std::array<std::uint8_t, 4> ip{};
            r.bytes(ip.data(), 4);
            std::uint16_t oport = r.u16();
            Token tok{};
            r.bytes(tok.data(), 16);
            auto ros = read_roster(r);
            if (!r.ok) return lose(c, "short WELCOME");
            if (slot < 1 || slot >= kMaxPartyPlayers || maxp < 2 || maxp > kMaxPartyPlayers || slot >= maxp)
                return lose(c, "bad WELCOME (slot " + std::to_string(slot) + " of " + std::to_string(maxp) + ")");
            c->tx.init(c->pending_tx);
            crypto::wipe(c->pending_tx.data(), 32);
            c->phase = Phase::Open;
            c->last_ping = Clock::now();
            my_slot = slot;
            host_max_players = maxp;
            {
                std::lock_guard<std::mutex> sl(snap_mu);
                snap_max_players = maxp;
            }
            my_token = tok;
            clock_offset = hclock <= static_cast<std::uint64_t>(kMaxClockMs) ? static_cast<std::int64_t>(hclock) - now_ms() : 0;
            char buf[32];
            std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], oport);
            observed = buf;
            if (!resumed) gev.reset();
            last_resumed = resumed;
            backoff_ms = 0;
            groster = ros;
            // Our own state, then any events the host has not acknowledged.
            send_local_state(c);
            for (const auto& e : gev.unacked) send_event_frame(c, e);
            set_state(LinkState::Connected, "slot " + std::to_string(slot) + (resumed ? " (resumed)" : ""));
            if (cb.on_roster) post([this, ros] { cb.on_roster(ros); });
            return;
        }
        switch (type) {
        case kPing: handle_common(c, type, r); break;
        case kPong: {
            std::uint64_t t = r.u64();
            std::uint64_t hclock = r.u64();
            if (!r.ok) break;
            std::int64_t now = now_ms();
            // A host's PONG: t must be one of our PING times, its clock a sane number of ms
            // (signed overflow here was the fuzzer's first find).
            if (t <= static_cast<std::uint64_t>(now) && hclock <= static_cast<std::uint64_t>(kMaxClockMs)) {
                const std::int64_t rt = now - static_cast<std::int64_t>(t);
                if (rt <= 3600000) {
                    rtt = static_cast<std::uint32_t>(rt);
                    clock_offset = static_cast<std::int64_t>(hclock) + rt / 2 - now;
                }
            }
            break;
        }
        case kRoster: {
            auto ros = read_roster(r);
            if (!r.ok) break;
            groster = ros;
            if (cb.on_roster) post([this, ros] { cb.on_roster(ros); });
            break;
        }
        case kVitals: {  // everyone's, from the host
            MemberVitals v[kMaxVitals];
            const int n = read_vitals(r, v, kMaxVitals);
            const auto now = Clock::now();
            for (int i = 0; i < n; ++i) {
                const int s = v[i].slot;  // 0..3 (sanitize_vitals)
                if (s == my_slot) continue;
                gvitals[s] = v[i];
                gvitals_at[s] = now;
                ghas_vitals[s] = true;
            }
            break;
        }
        case kRpcResp: {
            std::uint32_t id = r.u32();
            bool ok = r.u8() != 0;
            std::string reply = r.str32();
            if (!r.ok) break;
            auto it = pending.find(id);
            if (it == pending.end()) break;
            if (it->second.sync) {
                it->second.done = true;
                it->second.ok = ok;
                it->second.reply = std::move(reply);
                state_cv.notify_all();
            } else {
                auto f = std::move(it->second.cb);
                pending.erase(it);
                if (f) post([f, ok, reply = std::move(reply)] { f(ok, reply); });
            }
            break;
        }
        case kEvent: on_event_frame(c, gev, kHostSlot, r); break;
        case kEventAck: on_event_ack(gev, r); break;
        case kPartyCmd: {
            std::string cmd = r.str16();
            std::string json = r.str32();
            if (r.ok && cb.on_party_cmd)
                post_peer(c, [this, cmd = std::move(cmd), json = std::move(json)] {
                    cb.on_party_cmd(kHostSlot, cmd, json);
                });
            break;
        }
        case kProgress: {
            std::string blob = r.str32();
            if (r.ok && cb.on_progress)
                post_peer(c, [this, b = std::vector<std::uint8_t>(blob.begin(), blob.end())] {
                    cb.on_progress(kHostSlot, b);
                });
            break;
        }
        case kBye: {
            RejectCode code = static_cast<RejectCode>(r.u8());
            std::string why = sanitize_text(r.str16(), 1024);
            reject = code == RejectCode::None ? RejectCode::Shutdown : code;
            reject_text = why.empty() ? "the host ended the party" : why;
            if (reject == RejectCode::Shutdown) note_host_gone();
            close_conn(c);
            gconn = nullptr;
            fail_pending(reject_text);
            set_state(LinkState::Rejected, reject_text);
            break;
        }
        default: break;
        }
    }

    // Party HUD: our vitals as set_local_vitals left them; `changed` = new since the last send.
    bool local_vitals(MemberVitals* v, bool* changed) {  // mu held
        std::lock_guard<std::mutex> sl(snap_mu);
        if (!snap_local_vitals_set) return false;
        *v = snap_local_vitals;
        *changed = snap_local_vitals_seq != sent_vitals_seq;
        sent_vitals_seq = snap_local_vitals_seq;
        return true;
    }
    bool vitals_due(Clock::time_point now, bool changed) const {
        const auto since = ms_between(last_vitals_send, now);
        return since >= kVitalsSendMs && (changed || since >= kVitalsKeepaliveMs);
    }
    void send_vitals_guest(Clock::time_point now) {  // mu held
        if (!gconn || gconn->phase != Phase::Open || my_slot < 0 || my_slot >= kMaxVitals) return;
        if (ms_between(last_vitals_send, now) < kVitalsSendMs) return;
        std::uint64_t seq_before = sent_vitals_seq;
        MemberVitals v;
        bool changed = false;
        if (!local_vitals(&v, &changed)) return;
        if (!vitals_due(now, changed)) {
            sent_vitals_seq = seq_before;  // not sent: still pending
            return;
        }
        v.slot = my_slot;
        Writer w;
        write_vitals(w, &v, 1);
        send(gconn, kVitals, w);
        last_vitals_send = now;
    }
    void send_vitals_host(Clock::time_point now) {  // mu held
        if (state != LinkState::Hosting || ms_between(last_vitals_send, now) < kVitalsSendMs) return;
        std::uint64_t seq_before = sent_vitals_seq;
        MemberVitals v[kMaxVitals];
        int n = 0;
        bool changed = false;
        if (local_vitals(&v[0], &changed)) {
            v[0].slot = kHostSlot;
            n = 1;
        }
        for (const auto& [slot, m] : members)
            if (m.conn && m.has_vitals && ms_between(m.vitals_at, now) <= kVitalsStaleMs && n < kMaxVitals)
                v[n++] = m.vitals;
        changed = changed || host_vitals_dirty;
        if (n == 0 || !vitals_due(now, changed)) {
            sent_vitals_seq = seq_before;
            return;
        }
        host_vitals_dirty = false;
        Writer w;
        write_vitals(w, v, n);
        for (auto& [slot, m] : members)
            if (m.conn && m.conn->phase == Phase::Open) send(m.conn, kVitals, w);
        last_vitals_send = now;
    }

    void send_local_state(Conn* c) {
        RosterEntry me;
        me.slot = my_slot;
        me.name = cfg.name;
        me.state = local_state;
        me.connected = true;
        me.map_id = local_map;
        Writer w;
        write_roster(w, {me});
        send(c, kRoster, w);
    }

    // The host's IPv4 address. A literal is parsed; a name goes to the resolver, which can take
    // seconds: never called with mu held (every API call, the game's main thread included,
    // takes mu).
    static bool resolve_host(const std::string& name, std::array<std::uint8_t, 4>* ip) {
        in_addr lit{};
        if (inet_pton(AF_INET, name.c_str(), &lit) == 1) {
            std::memcpy(ip->data(), &lit, 4);
            return true;
        }
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        bool ok = false;
        if (getaddrinfo(name.c_str(), nullptr, &hints, &res) == 0 && res) {
            for (addrinfo* a = res; a; a = a->ai_next)
                if (a->ai_family == AF_INET) {
                    std::memcpy(ip->data(), &reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr, 4);
                    ok = true;
                    break;
                }
            freeaddrinfo(res);
        }
        return ok;
    }

    // One connection attempt's system calls: name resolution, socket(), a non-blocking
    // connect(). Done without mu: each can take milliseconds (seconds for a resolver).
    struct Dial {
        bool resolved = false;
        std::array<std::uint8_t, 4> ip{};
        sock::Socket s = sock::kInvalid;
        bool failed = false;     // connect() failed at once
        bool connected = false;  // connect() completed at once (loopback)
    };
    static Dial dial(const std::string& name, std::uint16_t port) {
        Dial d;
        d.resolved = resolve_host(name, &d.ip);
        if (!d.resolved) return d;
        d.s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (d.s == sock::kInvalid) return d;
        sock::set_nonblocking(d.s);
        int one = 1;
        setsockopt(d.s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        std::memcpy(&a.sin_addr, d.ip.data(), 4);
        const int rc = ::connect(d.s, reinterpret_cast<sockaddr*>(&a), sizeof a);
        d.connected = rc == 0;
        d.failed = rc != 0 && !sock::would_block(sock::last_error());
        return d;
    }

    void start_connect(Dial d) {  // guest, mu held
        want_connect = false;
        auto c = std::make_unique<Conn>();
        c->id = next_conn_id++;
        c->created = c->last_rx = Clock::now();
        c->phase = Phase::Connecting;
        Conn* raw = c.get();
        conns.push_back(std::move(c));
        gconn = raw;
        set_state(LinkState::Connecting, host_name + ":" + std::to_string(host_port));
        if (!d.resolved) return lose(raw, "cannot resolve " + host_name);
        raw->s = d.s;
        if (raw->s == sock::kInvalid) return lose(raw, "socket() failed");
        raw->peer_ip = d.ip;
        raw->peer_port = host_port;
        if (route_hint && uext) {
            std::uint32_t a = 0;
            std::memcpy(&a, d.ip.data(), 4);
            if (route_hint(a, htons(host_port)) >= 2 && switch_to_udp(raw, "the host is reached through a member relay"))
                return;
        }
        if (d.failed) {
            if (switch_to_udp(raw, "TCP connect to the host failed")) return;
            return lose(raw, "connect failed");
        }
        attach_sim(raw);
        if (udp_prefer && !d.connected && switch_to_udp(raw, "the last connection worked over UDP")) return;
        if (d.connected) on_connected(raw);
    }

    void on_connected(Conn* c) {
        c->phase = Phase::WaitChallenge;
        c->last_rx = Clock::now();
        Writer w;
        w.bytes(kMagic, 4).u16(kLinkProtocolVersion).str16(cfg.name);
        w.bytes(cfg.eboot_sha256.data(), 32).bytes(cfg.mods_hash.data(), 32).bytes(my_token.data(), 16);
        w.bytes(cfg.patches_hash.data(), 32).str16(join_names(cfg.patch_names)).str16(join_names(cfg.mod_names));
        w.str16(cfg.rules);
        w.str16(cfg.build_id).bytes(cfg.params_sha256.data(), 32).bytes(cfg.hooks_hash.data(), 32);
        w.str16(join_names(cfg.hook_names));
        send(c, kHello, w);
    }

    // ---------- IO loop ----------
    void accept_all() {
        for (;;) {
            sockaddr_in a{};
            sock::socklen al = sizeof a;
            sock::Socket s = ::accept(listen_s, reinterpret_cast<sockaddr*>(&a), &al);
            if (s == sock::kInvalid) return;
            sock::set_nonblocking(s);
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
            // Nobody is authenticated yet: a handful of handshakes at a time, fewer per address.
            std::size_t pending = 0, same_ip = 0;
            for (const auto& o : conns) {
                if (o->dead || o->phase == Phase::Open) continue;
                ++pending;
                if (std::memcmp(o->peer_ip.data(), &a.sin_addr, 4) == 0) ++same_ip;
            }
            if (pending >= kMaxPendingConns || same_ip >= kMaxPendingPerIp) {
                sock::close(s);
                ++refused_conns;
                if (refused_conns == 1 || refused_conns % 100 == 0)
                    log("party link: refused " + std::to_string(refused_conns) +
                        " connection(s): too many handshakes in progress");
                continue;
            }
            if (bbnet::netsim::config().nat && bbnet::netsim::applies_to(bbnet::netsim::config(), a.sin_addr.s_addr)) {
                sock::close(s);  // simulated NAT without a port forward: inbound TCP never arrives
                continue;
            }
            auto c = std::make_unique<Conn>();
            c->id = next_conn_id++;
            c->s = s;
            c->phase = Phase::WaitHello;
            c->created = c->last_rx = Clock::now();
            std::memcpy(c->peer_ip.data(), &a.sin_addr, 4);
            c->peer_port = ntohs(a.sin_port);
            attach_sim(c.get());
            conns.push_back(std::move(c));
        }
    }

    void read_conn(Conn* c) {
        std::uint8_t buf[65536];
        // At most kReadBudget per pass (the rest waits in the socket: the lock is held short and
        // `in` stays bounded), and nothing while unhandled frames are still buffered.
        std::size_t got = 0;
        while (!c->input_pending && got < kReadBudget) {
            long r = sock::recv_some(c->s, buf, sizeof buf);
            if (r > 0) {
                if (c->sim) {
                    c->held.emplace_back(c->sim->schedule(Clock::now()), std::vector<std::uint8_t>(buf, buf + r));
                } else {
                    c->in.insert(c->in.end(), buf, buf + r);
                }
                got += static_cast<std::size_t>(r);
                if (r < static_cast<long>(sizeof buf)) break;
                continue;
            }
            if (r == 0) {
                if (!c->held.empty()) break;  // the simulated in-flight bytes first (then 0 again)
                return lose(c, "connection closed by peer");
            }
            if (sock::would_block(sock::last_error())) break;
            if (!c->held.empty()) break;
            return lose(c, "connection reset");
        }
        if (c->sim) release_held(c, Clock::now());
        process_input(c);
    }

    void timers() {
        auto now = Clock::now();
        for (auto& cp : conns) {
            Conn* c = cp.get();
            if (c->dead) continue;
            if (c->phase == Phase::Connecting) {
                if (!c->us && !host && udp_enabled() && ms_between(c->created, now) > cfg.tcp_fallback_ms &&
                    switch_to_udp(c, "no TCP answer from the host"))
                    continue;
                if (!c->us && ms_between(c->created, now) > cfg.connect_timeout_ms) lose(c, "connect timed out");
                continue;
            }
            if (host && c->phase != Phase::Open &&
                ms_between(c->created, now) > std::max(cfg.connect_timeout_ms, cfg.lost_timeout_ms)) {
                lose(c, "handshake timed out");
                continue;
            }
            // A connection held back by its own callback backlog is not silent.
            if (c->backlog->load() >= kMaxCallbackBacklog) c->last_rx = now;
            if (ms_between(c->last_rx, now) > cfg.lost_timeout_ms) {
                lose(c, "no traffic for " + std::to_string(cfg.lost_timeout_ms / 1000) + " s");
                continue;
            }
            if (c->phase == Phase::Open && ms_between(c->last_ping, now) >= cfg.ping_interval_ms) {
                c->last_ping = now;
                Writer w;
                w.u64(static_cast<std::uint64_t>(now_ms()));
                send(c, kPing, w);
            }
            if (!c->out.empty()) flush(c);
        }
        if (host) {
            std::vector<int> expired;
            for (auto& [slot, m] : members)
                if (!m.conn && ms_between(m.lost_at, now) > cfg.slot_keep_ms) expired.push_back(slot);
            for (int s : expired) {
                log("party link: slot " + std::to_string(s) + " released (" + members[s].name + " did not come back)");
                release_member(s, false);
            }
            if (ms_between(last_roster_bcast, now) >= cfg.roster_refresh_ms) broadcast_roster();
            send_vitals_host(now);
        } else {
            send_vitals_guest(now);
            if (!gconn && state == LinkState::Reconnecting && now >= next_attempt) want_connect = true;
            for (auto it = pending.begin(); it != pending.end();) {
                if (!it->second.sync && now >= it->second.deadline) {
                    auto f = std::move(it->second.cb);
                    if (f) post([f] { f(false, "rpc timed out"); });
                    it = pending.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    void io_loop() {
        std::vector<sock::PollFd> fds;
        std::vector<Conn*> owners;
        for (;;) {
            fds.clear();
            owners.clear();
            bool busy = false;  // buffered frames to handle: poll without waiting
            {
                std::lock_guard<std::mutex> lk(mu);
                if (stopping) break;
                if (freeze_until <= Clock::now()) {
                    if (listen_s != sock::kInvalid) {
                        sock::PollFd p{};
                        p.fd = listen_s;
                        p.events = POLLIN;
                        fds.push_back(p);
                        owners.push_back(nullptr);
                    }
                    for (auto& c : conns) {
                        if (c->dead || c->s == sock::kInvalid) continue;
                        sock::PollFd p{};
                        p.fd = c->s;
                        // Backpressure: no reads while frames wait (in `in` or for callbacks).
                        const bool readable = !c->input_pending && c->backlog->load() < kMaxCallbackBacklog;
                        if (c->input_pending) busy = true;
                        p.events = readable ? POLLIN : 0;
                        if (c->phase == Phase::Connecting || c->out_off < c->out.size()) p.events |= POLLOUT;
                        if (!p.events) continue;
                        fds.push_back(p);
                        owners.push_back(c.get());
                    }
                }
            }
            int n = 0;
            if (fds.empty()) {
                if (!busy) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else {
                n = sock::poll(fds.data(), static_cast<unsigned long>(fds.size()), busy ? 0 : 10);
            }
            std::unique_lock<std::mutex> lk(mu);
            const auto locked_at = Clock::now();
            const std::uint64_t cycles_at = thread_cycles();
            if (stopping) break;
            drain_posted();  // the main thread's post_event()s, in order, before this pass's work
            if (freeze_until > Clock::now()) continue;
            if (n > 0) {
                for (std::size_t i = 0; i < fds.size(); ++i) {
                    short re = fds[i].revents;
                    if (!re) continue;
                    Conn* c = owners[i];
                    if (!c) {
                        accept_all();
                        continue;
                    }
                    if (c->dead) continue;
                    if (c->phase == Phase::Connecting) {
                        if (re & (POLLOUT | POLLERR | POLLHUP)) {
                            int err = 0;
                            sock::socklen el = sizeof err;
                            getsockopt(c->s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el);
                            if (err != 0 || (re & (POLLERR | POLLHUP) && !(re & POLLOUT))) {
                                if (!switch_to_udp(c, "TCP connect to the host refused"))
                                    lose(c, "cannot connect to the host (" + std::to_string(err) + ")");
                            }
                            else
                                on_connected(c);
                        }
                        continue;
                    }
                    if (re & (POLLIN | POLLERR | POLLHUP)) read_conn(c);
                    if (!c->dead && (re & POLLOUT)) flush(c);
                }
            }
            // Simulated in-flight bytes that are due now.
            for (std::size_t i = 0; i < conns.size(); ++i) {
                Conn* c = conns[i].get();
                if (!c->dead && !c->held.empty() && release_held(c, Clock::now())) process_input(c);
            }
            // Frames left over from an earlier pass (kFramesPerPass, callback backlog).
            for (std::size_t i = 0; i < conns.size(); ++i) {
                Conn* c = conns[i].get();
                if (!c->dead && c->input_pending && c->backlog->load() < kMaxCallbackBacklog) process_input(c);
            }
            timers();
            udp_pump();
            conns.erase(std::remove_if(conns.begin(), conns.end(), [](const std::unique_ptr<Conn>& c) { return c->dead; }),
                        conns.end());
            publish();
            const auto held = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - locked_at).count();
            if (held > hold_max_us) hold_max_us = held;
            const std::uint64_t cyc = thread_cycles() - cycles_at;
            if (cyc > hold_max_cycles) hold_max_cycles = cyc;
            if (want_connect) connect_unlocked(lk);
        }
    }

    // A due reconnect: dials (resolve, socket, connect) without mu, then installs it (mu held).
    void connect_unlocked(std::unique_lock<std::mutex>& lk) {
        const std::string name = host_name;
        const std::uint16_t port = host_port;
        lk.unlock();
        Dial d = dial(name, port);
        lk.lock();
        if (stopping || gconn || state != LinkState::Reconnecting) {
            want_connect = false;
            if (d.s != sock::kInvalid) sock::close(d.s);
            return;
        }
        start_connect(d);
    }

    void start_threads() {
        dstop = false;
        dthread = std::thread([this] { dispatch_loop(); });
        io_thread = std::thread([this] { io_loop(); });
    }
};

// ---------------- public API ----------------

PartyLink::PartyLink(LinkConfig cfg, LinkCallbacks cb) : impl_(std::make_unique<Impl>(std::move(cfg), std::move(cb))) {}

PartyLink::~PartyLink() { stop(); }

bool PartyLink::start_host(std::string* error) {
    Impl& I = *impl_;
    auto fail = [&](std::string m) {
        if (error) *error = std::move(m);
        return false;
    };
    if (I.state != LinkState::Idle) return fail("already started");
    if (!sock::startup()) return fail("winsock unavailable");
    if (!valid_member_name(I.cfg.name)) return fail("invalid name (1-16 of A-Z a-z 0-9 _ -)");
    I.host = true;
    I.party_key = crypto::derive_party_key(I.cfg.password, I.cfg.secret.data());
    if (!I.cfg.password.empty()) {
        const std::array<std::uint8_t, 8> no_secret{};
        I.password_key = crypto::derive_party_key(I.cfg.password, no_secret.data());
    }
    // Port 0 (ephemeral) must give a port free for both TCP and UDP: guests reach the UDP
    // link on the TCP port's number. Windows hands out TCP ephemeral ports in sequence and
    // reserves blocks of UDP ports (Hyper-V/WSL excluded port ranges) that are free for TCP,
    // so for a while every bind(0) lands on a number whose UDP twin is unavailable. Retry with
    // random ports of the dynamic range instead (a reserved block is hundreds of ports wide,
    // so stepping to the next ephemeral port can stay inside it for a long time).
    const bool any_port = I.cfg.port == 0 && I.cfg.udp_link;
    std::uint32_t rnd = static_cast<std::uint32_t>(Clock::now().time_since_epoch().count()) ^
                        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&I));
    sock::Socket s = sock::kInvalid;
    sockaddr_in b{};
    for (int attempt = 0;; ++attempt) {
        s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == sock::kInvalid) break;
#if defined(_WIN32)
        BOOL excl = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
#else
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        std::uint16_t want = I.cfg.port;
        if (attempt > 0) {
            rnd = rnd * 1664525u + 1013904223u;
            want = static_cast<std::uint16_t>(49152 + (rnd >> 16) % 16384);
        }
        a.sin_port = htons(want);
        if (sock::loopback_only()) a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        else if (inet_pton(AF_INET, I.cfg.bind_addr.c_str(), &a.sin_addr) != 1) a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            sock::close(s);
            s = sock::kInvalid;
            if (attempt > 0 && attempt < 63) continue;  // that random port is taken: another one
            break;
        }
        sock::socklen bl = sizeof b;
        getsockname(s, reinterpret_cast<sockaddr*>(&b), &bl);
        if (!I.cfg.udp_link || I.open_udp_socket(ntohs(b.sin_port))) break;
        if (!any_port || attempt >= 63) {
            I.log("party link: UDP port " + std::to_string(ntohs(b.sin_port)) + " unavailable: TCP only");
            break;
        }
        sock::close(s);
        s = sock::kInvalid;
    }
    if (s == sock::kInvalid) return fail("party port " + std::to_string(I.cfg.port) + " is in use (another game instance?)");
    if (I.cfg.udp_link) I.umux.accepting = true;
    // A simulated NAT without a port forward (netsim nat=1): the port is bound but never
    // listens, so a guest's TCP connect is refused as on the internet it would go unanswered.
    const bool nat_sim = bbnet::netsim::config().nat && sock::loopback_only();
    if (!nat_sim && ::listen(s, 8) != 0) {
        sock::close(s);
        return fail("listen() failed");
    }
    sock::set_nonblocking(s);
    {
        std::lock_guard<std::mutex> lk(I.mu);
        I.listen_s = s;
        if (nat_sim) {
            sock::close(s);  // nothing accepts TCP: the UDP path only
            I.listen_s = sock::kInvalid;
            I.log("party link: simulated NAT: no TCP listener (UDP path only)");
        }
        I.port = ntohs(b.sin_port);
        I.start_time = Clock::now();
        for (auto& [slot, m] : I.members)
            if (m.restored) m.lost_at = I.start_time;  // slot_keep_ms from now
        I.set_state(LinkState::Hosting, "port " + std::to_string(I.port) + ", up to " +
                                            std::to_string(I.cfg.max_players) + " players");
        I.publish();
    }
    I.start_threads();
    return true;
}

bool PartyLink::start_guest(const std::string& host, std::uint16_t port, std::string* error) {
    Impl& I = *impl_;
    auto fail = [&](std::string m) {
        if (error) *error = std::move(m);
        return false;
    };
    if (I.state != LinkState::Idle) return fail("already started");
    if (!sock::startup()) return fail("winsock unavailable");
    if (!valid_member_name(I.cfg.name)) return fail("invalid name (1-16 of A-Z a-z 0-9 _ -)");
    I.host = false;
    I.party_key = crypto::derive_party_key(I.cfg.password, I.cfg.secret.data());
    Impl::Dial d = Impl::dial(host, port);
    {
        std::lock_guard<std::mutex> lk(I.mu);
        // The UDP path's own socket at once: its port is the endpoint a host behind a NAT punches
        // towards (the guest code); the game passes its party port instead (set_udp_transport).
        if (I.cfg.udp_link && !I.uext) I.open_udp_socket(0);
        I.host_name = host;
        I.host_port = port;
        I.start_time = Clock::now();
        I.start_connect(d);
        I.publish();
    }
    I.start_threads();
    return true;
}

void PartyLink::stop(bool graceful) {
    Impl& I = *impl_;
    {
        std::lock_guard<std::mutex> lk(I.mu);
        if (I.state == LinkState::Idle) return;
        if (I.stopping) goto join;
        if (graceful) {
            I.freeze_until = {};
            Writer w;
            w.u8(static_cast<std::uint8_t>(I.host ? RejectCode::Shutdown : RejectCode::None)).str16(
                I.host ? "the host ended the party" : "left");
            for (auto& c : I.conns)
                if (!c->dead && c->phase == Impl::Phase::Open) I.send(c.get(), kBye, w);
        }
        I.stopping = true;
        for (auto& c : I.conns)
            if (!c->dead && c->us) I.flush(c.get());
        for (auto& c : I.conns) I.close_conn(c.get());
        {
            std::vector<rudp::Datagram> out;  // the BYEs and FINs of UDP connections
            I.umux.poll(Clock::now(), out);
            for (const rudp::Datagram& d : out) I.udp_out(d);
        }
        if (I.usock != sock::kInvalid) {
            sock::close(I.usock);
            I.usock = sock::kInvalid;
        }
        if (I.listen_s != sock::kInvalid) {
            sock::close(I.listen_s);
            I.listen_s = sock::kInvalid;
        }
        I.gconn = nullptr;
        I.fail_pending("party link stopped");
        I.set_state(LinkState::Stopped, graceful ? "" : "dropped");
        I.publish();
    }
join:
    if (I.io_thread.joinable()) I.io_thread.join();
    {
        std::lock_guard<std::mutex> lk(I.dmu);
        I.dstop = true;
    }
    I.dcv.notify_all();
    if (I.dthread.joinable()) I.dthread.join();
    std::lock_guard<std::mutex> lk(I.mu);
    I.conns.clear();
    I.members.clear();
    I.publish();
    crypto::wipe(I.party_key.data(), I.party_key.size());
    crypto::wipe(I.password_key.data(), I.password_key.size());
}

bool PartyLink::is_host() const { return impl_->host; }

LinkState PartyLink::state() const {
    std::lock_guard<std::mutex> lk(impl_->snap_mu);
    return impl_->snap_state;
}

bool PartyLink::wait_state(LinkState s, int timeout_ms) const {
    std::unique_lock<std::mutex> lk(impl_->mu);
    return impl_->state_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return impl_->state == s; });
}

int PartyLink::local_slot() const {
    std::lock_guard<std::mutex> lk(impl_->snap_mu);
    return impl_->snap_slot;
}

std::uint16_t PartyLink::bound_port() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->host ? impl_->port : impl_->host_port;
}

int PartyLink::max_players() const {
    // Polled every frame by the game's main thread: the published copy, never the IO lock.
    if (impl_->host) return impl_->cfg.max_players;  // fixed at construction
    std::lock_guard<std::mutex> lk(impl_->snap_mu);
    return impl_->snap_max_players;
}

std::vector<RosterEntry> PartyLink::roster() const {
    std::lock_guard<std::mutex> lk(impl_->snap_mu);  // refreshed by the IO thread every <= 10 ms
    return impl_->snap_roster;
}

std::int64_t PartyLink::host_clock_ms() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->now_ms() + (impl_->host ? 0 : impl_->clock_offset);
}

std::uint32_t PartyLink::rtt_ms() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->rtt;
}

std::uint64_t PartyLink::udp_resends() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::uint64_t n = 0;
    for (const auto& c : impl_->conns)
        if (c && c->us) n += c->us->resends();
    return n;
}

std::string PartyLink::observed_address() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->observed;
}

std::uint32_t PartyLink::member_ip(int slot) const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->host) return 0;
    auto it = impl_->members.find(slot);
    if (it == impl_->members.end() || !it->second.conn) return 0;
    std::uint32_t ip = 0;
    std::memcpy(&ip, it->second.conn->peer_ip.data(), 4);
    return ip;
}

bool PartyLink::member_endpoint(int slot, std::uint32_t* ip, std::uint16_t* port_nbo) const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->host) return false;
    auto it = impl_->members.find(slot);
    if (it == impl_->members.end() || !it->second.conn) return false;
    std::memcpy(ip, it->second.conn->peer_ip.data(), 4);
    *port_nbo = htons(it->second.conn->peer_port);
    return true;
}

RejectCode PartyLink::reject_code() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->reject;
}

std::string PartyLink::reject_reason() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->reject_text;
}

bool PartyLink::rpc_call(const std::string& kind, const std::string& json, std::string* reply, int timeout_ms) {
    Impl& I = *impl_;
    std::unique_lock<std::mutex> lk(I.mu);
    if (I.host || !I.gconn || I.gconn->phase != Impl::Phase::Open) {
        if (reply) *reply = I.host ? "rpc_call is guest-only" : "not connected to the host";
        return false;
    }
    std::uint32_t id = I.next_rpc++;
    if (id == 0) id = I.next_rpc++;
    Impl::Pending& p = I.pending[id];
    p.sync = true;
    p.deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    Writer w;
    w.u32(id).str16(kind).str32(json);
    I.send(I.gconn, kRpcReq, w);
    I.state_cv.wait_until(lk, p.deadline, [&] { return I.pending[id].done; });
    Impl::Pending done = std::move(I.pending[id]);
    I.pending.erase(id);
    if (!done.done) {
        if (reply) *reply = "rpc timed out";
        return false;
    }
    if (reply) *reply = std::move(done.reply);
    return done.ok;
}

std::uint32_t PartyLink::rpc_call_async(const std::string& kind, const std::string& json,
                                        std::function<void(bool, const std::string&)> done, int timeout_ms) {
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    if (I.host || !I.gconn || I.gconn->phase != Impl::Phase::Open) {
        std::string why = I.host ? "rpc_call is guest-only" : "not connected to the host";
        if (done) I.post([done, why] { done(false, why); });
        return 0;
    }
    std::uint32_t id = I.next_rpc++;
    if (id == 0) id = I.next_rpc++;
    Impl::Pending& p = I.pending[id];
    p.sync = false;
    p.deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    p.cb = std::move(done);
    Writer w;
    w.u32(id).str16(kind).str32(json);
    I.send(I.gconn, kRpcReq, w);
    return id;
}

std::uint64_t PartyLink::send_event(int slot, const std::string& name, const std::string& json) {
    Impl& I = *impl_;
    if (I.cb.on_event_sent) I.cb.on_event_sent(slot, name, json);
    std::lock_guard<std::mutex> lk(I.mu);
    I.drain_posted();  // posts made earlier go first
    return I.send_event_locked(slot, name, json);
}

void PartyLink::post_event(int slot, const std::string& name, const std::string& json) {
    Impl& I = *impl_;
    if (I.cb.on_event_sent) I.cb.on_event_sent(slot, name, json);
    {
        std::unique_lock<std::mutex> lk(I.mu, std::try_to_lock);
        if (lk.owns_lock()) {
            I.drain_posted();
            I.send_event_locked(slot, name, json);
            return;
        }
    }
    std::lock_guard<std::mutex> pl(I.post_mu);
    I.posted.push_back({slot, name, json});
    I.any_posted.store(true, std::memory_order_release);
}

std::uint64_t PartyLink::Impl::send_event_locked(int slot, const std::string& name, const std::string& json) {
    Impl& I = *this;
    if (I.state == LinkState::Stopped || I.state == LinkState::Idle) return 0;
    auto push = [&](EventStream& es, Impl::Conn* c) {
        EventStream::Ev e{es.next_cursor++, name, json};
        es.unacked.push_back(e);
        // A peer that never acknowledges must not grow the queue without end.
        while (es.unacked.size() > kMaxUnackedEvents) es.unacked.pop_front();
        if (c && c->phase == Impl::Phase::Open) I.send_event_frame(c, e);
        return e.cursor;
    };
    if (!I.host) {
        if (I.state == LinkState::Rejected) return 0;
        return push(I.gev, I.gconn);
    }
    std::uint64_t last = 0;
    for (auto& [s, m] : I.members)
        if (slot == kBroadcast || slot == s) last = push(m.ev, m.conn);
    return last;
}

bool PartyLink::send_party_cmd(int slot, const std::string& cmd, const std::string& json) {
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    Writer w;
    w.str16(cmd).str32(json);
    if (!I.host) {
        if (!I.gconn || I.gconn->phase != Impl::Phase::Open) return false;
        I.send(I.gconn, kPartyCmd, w);
        return true;
    }
    bool any = false;
    for (auto& [s, m] : I.members)
        if ((slot == kBroadcast || slot == s) && m.conn && m.conn->phase == Impl::Phase::Open) {
            I.send(m.conn, kPartyCmd, w);
            any = true;
        }
    return any;
}

bool PartyLink::send_progress(int slot, const std::vector<std::uint8_t>& blob) {
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    Writer w;
    w.u32(static_cast<std::uint32_t>(blob.size())).bytes(blob.data(), blob.size());
    if (!I.host) {
        if (!I.gconn || I.gconn->phase != Impl::Phase::Open) return false;
        I.send(I.gconn, kProgress, w);
        return true;
    }
    bool any = false;
    for (auto& [s, m] : I.members)
        if ((slot == kBroadcast || slot == s) && m.conn && m.conn->phase == Impl::Phase::Open) {
            I.send(m.conn, kProgress, w);
            any = true;
        }
    return any;
}

void PartyLink::set_local_state(MemberState state, std::uint32_t map_id) {
    Impl& I = *impl_;
    {
        // Called every frame; only a change needs the IO lock.
        std::lock_guard<std::mutex> sl(I.snap_mu);
        if (I.snap_local_state == state && I.snap_local_map == map_id) return;
    }
    std::lock_guard<std::mutex> lk(I.mu);
    if (I.local_state == state && I.local_map == map_id) return;
    I.local_state = state;
    I.local_map = map_id;
    if (I.host) {
        if (I.state == LinkState::Hosting) I.broadcast_roster();
    } else if (I.gconn && I.gconn->phase == Impl::Phase::Open) {
        I.send_local_state(I.gconn);
    }
    I.publish();
}

void PartyLink::set_local_vitals(const MemberVitals& v) {
    Impl& I = *impl_;
    MemberVitals c = v;
    c.slot = 0;  // filled in when sent
    if (!sanitize_vitals(c)) return;
    std::lock_guard<std::mutex> sl(I.snap_mu);
    const MemberVitals& o = I.snap_local_vitals;
    const bool same = I.snap_local_vitals_set && o.flags == c.flags && o.hp == c.hp && o.max_hp == c.max_hp &&
                      o.stamina == c.stamina && o.max_stamina == c.max_stamina && o.map_id == c.map_id &&
                      std::memcmp(o.pos, c.pos, sizeof c.pos) == 0 && o.yaw == c.yaw && o.level == c.level &&
                      o.deaths == c.deaths && o.rot == c.rot;
    if (same) return;
    I.snap_local_vitals = c;
    I.snap_local_vitals_set = true;
    ++I.snap_local_vitals_seq;
}

int PartyLink::vitals(MemberVitals* out, int max) const {
    const Impl& I = *impl_;
    const auto now = Clock::now();
    int n = 0;
    std::lock_guard<std::mutex> sl(I.snap_mu);
    if (n < max && I.snap_local_vitals_set && I.snap_slot >= 0 && I.snap_slot < kMaxVitals) {
        out[n] = I.snap_local_vitals;
        out[n].slot = I.snap_slot;
        out[n++].age_ms = 0;
    }
    for (int i = 0; i < I.snap_vitals_n && n < max; ++i) {
        const auto age = ms_between(I.snap_vitals_at[i], now);
        if (age < 0 || age > kVitalsStaleMs) continue;
        out[n] = I.snap_vitals[i];
        out[n++].age_ms = static_cast<std::uint32_t>(age);
    }
    return n;
}

std::vector<KeptMember> PartyLink::kept_members() const {
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    std::vector<KeptMember> out;
    if (!I.host) return out;
    for (const auto& [slot, m] : I.members) {
        KeptMember k;
        k.slot = slot;
        k.name = m.name;
        k.token = m.token;
        out.push_back(std::move(k));
    }
    return out;
}

void PartyLink::restore_members(const std::vector<KeptMember>& members) {
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    if (I.state != LinkState::Idle) return;
    for (const KeptMember& k : members) {
        if (k.slot < 1 || k.slot >= I.cfg.max_players || !valid_member_name(k.name) || k.name == I.cfg.name ||
            I.members.count(k.slot))
            continue;
        bool dup = false;
        for (const auto& [s, m] : I.members) dup = dup || m.name == k.name;
        if (dup) continue;
        Impl::Member m;
        m.slot = k.slot;
        m.name = k.name;
        m.token = k.token;
        if (is_zero(m.token)) crypto::random_bytes(m.token.data(), m.token.size());
        m.lost_at = Clock::now();
        m.state = MemberState::Home;
        m.restored = true;
        I.members.emplace(k.slot, std::move(m));
        I.log("party link: slot " + std::to_string(k.slot) + " kept for " + k.name + " (from before the restart)");
    }
}

bool PartyLink::last_welcome_resumed() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->last_resumed;
}

bool PartyLink::kick(int slot, const std::string& reason) {
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    if (!I.host || slot == kHostSlot || !I.members.count(slot)) return false;
    I.banned.insert(I.members[slot].name);
    I.log("party link: kicking " + I.members[slot].name + " (slot " + std::to_string(slot) + ")");
    I.release_member(slot, true, RejectCode::Kicked, reason.empty() ? "kicked by the host" : reason);
    I.publish();
    return true;
}

std::int64_t PartyLink::debug_lock_hold_max_us(bool reset, std::uint64_t* cycles) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    const std::int64_t v = impl_->hold_max_us;
    if (cycles) *cycles = impl_->hold_max_cycles;
    if (reset) {
        impl_->hold_max_us = 0;
        impl_->hold_max_cycles = 0;
    }
    return v;
}
bool PartyLink::kick_name(const std::string& name, const std::string& reason) {
    int slot = -1;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->host) return false;
        for (const auto& [s, m] : impl_->members)
            if (m.name == name) slot = s;
    }
    return slot > 0 && kick(slot, reason);
}

bool AuthThrottle::Locked(std::uint32_t ip, std::int64_t now_ms, std::int64_t* remaining_ms, bool* global) const {
    auto it = addrs_.find(ip);
    const bool locked = it != addrs_.end() && now_ms < it->second.until;
    std::int64_t left = locked ? it->second.until - now_ms : 0;
    bool by_global = false;
    if (now_ms < global_until_ && std::find(trusted_.begin(), trusted_.end(), ip) == trusted_.end()) {
        by_global = !locked;
        left = std::max(left, global_until_ - now_ms);
    }
    if (remaining_ms) *remaining_ms = left;
    if (global) *global = by_global;
    return left > 0;
}

bool AuthThrottle::Fail(std::uint32_t ip, std::int64_t now_ms) {
    auto it = addrs_.find(ip);
    if (it == addrs_.end()) {
        if (addrs_.size() >= kAuthTrackedAddrs) {  // forget the stalest address (never a locked one first)
            auto stalest = addrs_.begin();
            for (auto j = addrs_.begin(); j != addrs_.end(); ++j) {
                const bool jl = now_ms < j->second.until, sl = now_ms < stalest->second.until;
                if ((sl && !jl) || (sl == jl && j->second.last < stalest->second.last)) stalest = j;
            }
            addrs_.erase(stalest);
        }
        it = addrs_.emplace(ip, Addr{}).first;
        it->second.window = now_ms;
    }
    // The global budget: every wrong proof counts, whichever address sent it.
    bool global_started = false;
    if (global_last_ && now_ms - global_last_ > kAuthForgetMs) global_level_ = 0;
    global_last_ = now_ms;
    if (global_fails_ == 0 || now_ms - global_window_ > kAuthFailWindowMs) {
        global_fails_ = 0;
        global_window_ = now_ms;
    }
    if (++global_fails_ > kAuthGlobalFree && now_ms >= global_until_) {
        std::int64_t lock = kAuthGlobalLockoutMs;
        for (int i = 0; i < global_level_ && lock < kAuthLockoutMaxMs; ++i) lock *= 2;
        global_until_ = now_ms + std::min(lock, kAuthLockoutMaxMs);
        ++global_level_;
        ++global_lockouts_;
        global_fails_ = 0;
        global_started = true;
    }
    Addr& a = it->second;
    if (a.last && now_ms - a.last > kAuthForgetMs) a = Addr{};
    if (a.fails == 0 || now_ms - a.window > kAuthFailWindowMs) {
        a.fails = 0;
        a.window = now_ms;
    }
    a.last = now_ms;
    // After a lockout (until forgotten) one wrong proof locks again: a typo is long past.
    if (++a.fails <= (a.lockouts ? 0 : kAuthFailFree)) return global_started;
    std::int64_t lock = kAuthLockoutMs;
    for (int i = 0; i < a.lockouts && lock < kAuthLockoutMaxMs; ++i) lock *= 2;
    a.until = now_ms + std::min(lock, kAuthLockoutMaxMs);
    ++a.lockouts;
    a.fails = 0;
    ++lockouts_;
    return true;
}

void AuthThrottle::Success(std::uint32_t ip) {
    auto it = addrs_.find(ip);
    if (it != addrs_.end()) it->second.fails = 0;
    auto t = std::find(trusted_.begin(), trusted_.end(), ip);
    if (t != trusted_.end()) trusted_.erase(t);
    if (trusted_.size() >= kAuthTrustedAddrs) trusted_.erase(trusted_.begin());
    trusted_.push_back(ip);
}

bool PartyLink::is_banned(const std::string& name) const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->banned.count(name) != 0;
}

bool PartyLink::reconnect_now() {
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    if (I.host || I.state != LinkState::Reconnecting) return false;
    I.next_attempt = Clock::now();
    I.backoff_ms = 0;
    return true;
}

void PartyLink::set_udp_transport(UdpSend send) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->uext = std::move(send);
}

void PartyLink::set_udp_route_hint(std::function<int(std::uint32_t, std::uint16_t)> route) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->route_hint = std::move(route);
}

void PartyLink::on_udp_datagram(std::uint32_t addr, std::uint16_t port, const std::uint8_t* d, std::size_t n) {
    if (!rudp::is_rudp(d, n)) return;
    std::lock_guard<std::mutex> lk(impl_->uin_mu);
    if (impl_->uin.size() < 4096) impl_->uin.emplace_back(rudp::Endpoint{addr, port}, std::vector<std::uint8_t>(d, d + n));
}

void PartyLink::set_secret(const std::array<std::uint8_t, 8>& secret) {
    Impl& I = *impl_;
    std::string password;
    {
        std::lock_guard<std::mutex> lk(I.mu);
        password = I.cfg.password;
    }
    Key key = crypto::derive_party_key(password, secret.data());  // Argon2i: outside the lock
    {
        std::lock_guard<std::mutex> lk(I.mu);
        I.cfg.secret = secret;
        crypto::wipe(I.party_key.data(), I.party_key.size());
        I.party_key = key;
    }
    crypto::wipe(key.data(), key.size());
    I.log("party link: the party code's secret changed: new players need the new code (the members in now stay)");
}

void PartyLink::punch_guest(std::uint32_t addr, std::uint16_t port) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->umux.punch({addr, port}, Clock::now(), 60000, 200);
    char b[96];
    const auto* ip = reinterpret_cast<const std::uint8_t*>(&addr);
    std::snprintf(b, sizeof b, "party link: punching towards a guest at %u.%u.%u.%u:%u (60 s)", ip[0], ip[1], ip[2], ip[3],
                  static_cast<unsigned>(ntohs(port)));
    impl_->log(b);
}

std::uint16_t PartyLink::udp_local_port() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->uport;
}

bool PartyLink::connected_over_udp() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->gconn && impl_->gconn->us && impl_->gconn->phase == Impl::Phase::Open;
}

void PartyLink::debug_freeze(int ms) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->freeze_until = Clock::now() + std::chrono::milliseconds(ms);
}

void PartyLink::debug_drop_connections() {
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    for (auto& c : I.conns)
        if (!c->dead) I.lose(c.get(), "dropped (debug)");
}

}  // namespace party
