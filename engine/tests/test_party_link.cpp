// SPDX-License-Identifier: GPL-3.0-or-later
// PartyLink (gpu/shim/party): party codes, crypto, STUN against a local responder, and a
// loopback party: host + guests over 127.0.0.1 -- handshake, wrong password / mismatched hash
// rejected, encrypted RPC, events, disconnect detection, reconnect with the slot kept, slot
// expiry, max players, host shutdown.
// Build and run: ninja -C out/gpu party-link-test && out/gpu/party-link-test.exe
#include "party/party_addr.h"
#include "party/party_code.h"
#include "party/party_crypto.h"
#include "party/party_lan.h"
#include "party/party_link.h"
#include "party/party_rudp.h"
#include "net/netsim.h"
#include "party/party_sock.h"
#include "party/upnp_win.h"
#include "net/net_stun.h"

#include <atomic>
#include <algorithm>
#include <cctype>
#include <set>
#include <random>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace party;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                              \
    } while (0)

static bool wait_for(const std::function<bool()>& pred, int timeout_ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// Every single-symbol substitution and every swap of two neighbouring symbols of `code` is
// rejected (the BBP3 check has 9-12 bits: any burst of up to 9 bits is caught).
static void check_typos(const std::string& code) {
    const char* alpha = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    std::string err;
    int caught = 0, total = 0;
    std::vector<std::size_t> at;
    for (std::size_t i = 5; i < code.size(); ++i)
        if (code[i] != '-') at.push_back(i);
    for (std::size_t i : at) {
        for (const char* a = alpha; *a; ++a) {
            if (*a == code[i]) continue;
            std::string t = code;
            t[i] = *a;
            ++total;
            PartyCode x;
            if (!decode_party_code(t, &x, &err)) ++caught;
        }
    }
    int swaps = 0, swaps_caught = 0;
    for (std::size_t j = 0; j + 1 < at.size(); ++j) {
        if (code[at[j]] == code[at[j + 1]]) continue;
        std::string t = code;
        std::swap(t[at[j]], t[at[j + 1]]);
        ++swaps;
        PartyCode x;
        if (!decode_party_code(t, &x, &err)) ++swaps_caught;
    }
    CHECK(caught == total);
    CHECK(swaps_caught == swaps);
    std::printf("  %s: %d/%d typos, %d/%d swaps rejected\n", code.c_str(), caught, total, swaps_caught, swaps);
}

// F5: the slot keep outlives a 2 min outage by default; BB_PARTY_SLOT_KEEP_MS overrides it (clamped).
static void test_slot_keep_env() {
    auto put = [](const char* kv) {
#if defined(_WIN32)
        _putenv(kv);
#else
        putenv(const_cast<char*>(kv));
#endif
    };
    LinkConfig c;
    CHECK(c.slot_keep_ms == 130000);
    put("BB_PARTY_SLOT_KEEP_MS=90000");
    apply_link_env(c);
    CHECK(c.slot_keep_ms == 90000);
    put("BB_PARTY_SLOT_KEEP_MS=5");
    apply_link_env(c);
    CHECK(c.slot_keep_ms == 1000);
    put("BB_PARTY_SLOT_KEEP_MS=");
}

static void test_codes() {
    std::array<std::uint8_t, 8> secret = {0xde, 0xad, 0xbe, 0xef, 0x01, 0x23, 0, 0};  // 48 bits, padded
    const std::array<std::uint8_t, 8> secret4 = {0xde, 0xad, 0xbe, 0xef, 0, 0, 0, 0};  // with a password
    std::string code = make_party_code({203, 0, 113, 7}, 9307, secret4, kCodeFlagPasswordRequired);
    std::printf("  code: %s\n", code.c_str());
    CHECK(code.rfind("BBP3-", 0) == 0 && code.size() == 5 + 16 + 3);  // default port: no port bits
    PartyCode pc;
    std::string err;
    CHECK(decode_party_code(code, &pc, &err));
    CHECK(!pc.plain && pc.version == 3 && pc.port == 9307 && pc.secret == secret4 &&
          pc.flags == kCodeFlagPasswordRequired);
    CHECK(pc.address() == "203.0.113.7:9307");

    // The examples every prompt shows (launcher.py has the same strings) are real codes.
    const std::array<std::uint8_t, 8> ex_secret = {0x5e, 0x72, 0xa9, 0x13, 0, 0, 0, 0};
    CHECK(make_party_code({203, 0, 113, 7}, 9307, ex_secret, kCodeFlagPasswordRequired) == PARTY_CODE_EXAMPLE);
    CHECK(make_party_code({192, 168, 1, 42}, 9307, ex_secret, kCodeFlagPasswordRequired | kCodeFlagLan) ==
          PARTY_LAN_CODE_EXAMPLE);
    CHECK(decode_party_code(PARTY_CODE_EXAMPLE, &pc, &err) && pc.address() == "203.0.113.7:9307");

    // Every BBP3 shape: length, round trip, typos.
    const std::array<std::uint8_t, 8> secret6 = {0x5e, 0x72, 0xa9, 0x13, 0xc4, 0x8d, 0, 0};
    struct Shape {
        std::uint8_t flags;
        std::uint16_t port;
        std::size_t symbols;
    } shapes[] = {
        {kCodeFlagPasswordRequired, 9307, 16},
        {0, 9307, 19},
        {kCodeFlagLan | kCodeFlagPasswordRequired, 9307, 11},
        {kCodeFlagLan, 9307, 14},
        {kCodeFlagPasswordRequired, 48000, 19},
        {kCodeFlagRelayOnly, 48000, 22},
        {kCodeFlagLan | kCodeFlagPasswordRequired | kCodeFlagRelayOnly, 47001, 14},
        {kCodeFlagLan, 47001, 17},
    };
    for (const Shape& sh : shapes) {
        const auto& sec = (sh.flags & kCodeFlagPasswordRequired) ? ex_secret : secret6;
        const std::string c = make_party_code({198, 51, 100, 9}, sh.port, sec, sh.flags);
        std::size_t syms = 0;
        for (std::size_t i = 5; i < c.size(); ++i) syms += c[i] != '-';
        CHECK(c.rfind("BBP3-", 0) == 0 && syms == sh.symbols);
        PartyCode d;
        CHECK(decode_party_code(c, &d, &err) && d.flags == sh.flags && d.port == sh.port && d.secret == sec &&
              d.version == 3 && d.ipv4[3] == 9 && d.lan_partial == ((sh.flags & kCodeFlagLan) != 0));
        CHECK(encode_party_code(d) == c);
        check_typos(c);
    }
    // A password code whose secret has 6 bytes (a host from before BBP3) is a BBP2 code; one
    // without a password and 6 bytes is BBP3.
    const std::string v2pw = make_party_code({203, 0, 113, 7}, 9307, secret, kCodeFlagPasswordRequired);
    CHECK(v2pw.rfind("BBP2-", 0) == 0 && v2pw.size() == 5 + 20 + 4);
    CHECK(decode_party_code(v2pw, &pc, &err) && pc.version == 2 && pc.secret == secret &&
          pc.flags == kCodeFlagPasswordRequired && pc.address() == "203.0.113.7:9307");
    CHECK(make_party_code({203, 0, 113, 7}, 9307, secret, 0).rfind("BBP3-", 0) == 0);
    {
        PartyCode a, b;
        CHECK(code_fits_v3(a));
        b.flags = kCodeFlagPasswordRequired;
        b.secret = secret;
        CHECK(!code_fits_v3(b));
        b.flags = 0;
        CHECK(code_fits_v3(b));
    }
    // A BBP2 code from before this version (fixed text) still decodes.
    {
        const std::array<std::uint8_t, 8> s16 = {1, 2, 3, 4, 5, 6, 0, 0};
        PartyCode old;
        CHECK(decode_party_code("BBP2-1B5G-0W87-0410-6105-0SBG", &old, &err) && old.version == 2 &&
              old.secret == s16 && old.flags == kCodeFlagPasswordRequired && old.address() == "203.0.113.7:9307");
        CHECK(decode_party_code("bbp2-1cn0-20g3-og2g-cwo", &old, &err) && old.lan_partial && old.ipv4[3] == 42 &&
              old.secret == s16 && old.flags == (kCodeFlagLan | kCodeFlagPasswordRequired));
    }

    // Case-insensitive, blanks, I/L -> 1 and O -> 0.
    std::string sloppy;
    for (char c : code) sloppy += static_cast<char>(c == '1' ? 'l' : c == '0' ? 'O' : std::tolower(c));
    sloppy = "  " + sloppy + " ";
    PartyCode pc2;
    CHECK(decode_party_code(sloppy, &pc2, &err));
    CHECK(pc2.secret == secret4 && pc2.ipv4 == pc.ipv4);
    std::string spaced = code;
    for (auto& c : spaced)
        if (c == '-') c = ' ';
    CHECK(decode_party_code(spaced, &pc2, &err));

    check_typos(code);
    check_typos(v2pw);

    std::string bad = code;
    bad[7] = 'U';  // not in Crockford's alphabet
    CHECK(!decode_party_code(bad, &pc2, &err) && err.find("invalid character") != std::string::npos);
    CHECK(!decode_party_code(code.substr(0, code.size() - 2), &pc2, &err));
    std::printf("  short code: %s\n", err.c_str());

    CHECK(decode_party_code("192.168.1.20:9400", &pc2, &err) && pc2.plain && pc2.port == 9400 &&
          pc2.host == "192.168.1.20" && pc2.ipv4[3] == 20);
    CHECK(decode_party_code("myhost.example", &pc2, &err) && pc2.plain && pc2.port == kDefaultPartyPort);
    CHECK(!decode_party_code("1.2.3.4:70000", &pc2, &err));
    CHECK(!decode_party_code("", &pc2, &err));
    CHECK(crc16_ccitt(reinterpret_cast<const std::uint8_t*>("123456789"), 9) == 0x29B1);
    CHECK(crc8(reinterpret_cast<const std::uint8_t*>("123456789"), 9) == 0xF4);

    // Another port: 16 more bits (22 symbols without a password); round trip with every field.
    const std::string ported = make_party_code({198, 51, 100, 9}, 48000, secret, kCodeFlagRelayOnly);
    CHECK(ported.rfind("BBP3-", 0) == 0 && ported.size() == 5 + 22 + 5);
    CHECK(decode_party_code(ported, &pc2, &err) && pc2.port == 48000 && pc2.flags == kCodeFlagRelayOnly &&
          pc2.address() == "198.51.100.9:48000" && pc2.secret == secret && !pc2.lan_partial);
    std::printf("  port 48000: %s\n", ported.c_str());

    // BBP1 still decodes; an 8-byte secret (an old host's) is encoded as BBP1.
    const std::array<std::uint8_t, 8> secret8 = {0xde, 0xad, 0xbe, 0xef, 0x01, 0x23, 0x45, 0x67};
    const std::string v1 = make_party_code({203, 0, 113, 7}, 9307, secret8, kCodeFlagPasswordRequired);
    CHECK(v1.rfind("BBP1-", 0) == 0 && v1.size() == 5 + 29 + 7);
    CHECK(decode_party_code(v1, &pc2, &err) && pc2.version == 1 && pc2.secret == secret8 && pc2.port == 9307 &&
          pc2.flags == kCodeFlagPasswordRequired && pc2.address() == "203.0.113.7:9307");
    PartyCode v1_struct;
    v1_struct.version = 1;
    v1_struct.flags = kCodeFlagPasswordRequired;
    v1_struct.ipv4 = {203, 0, 113, 7};
    v1_struct.secret = secret;
    const std::string v1b = encode_party_code(v1_struct);
    CHECK(v1b.rfind("BBP1-", 0) == 0);
    CHECK(decode_party_code(v1b, &pc2, &err) && pc2.secret == secret && pc2.address() == "203.0.113.7:9307");

    // The key a BBP2 guest derives equals the host's (the 6-byte secret zero padded to 8), and
    // the same secret's BBP1 code gives the same key; so does a BBP3 one without a password.
    PartyCode from_v2, from_v1, from_v3;
    CHECK(decode_party_code(v2pw, &from_v2, &err) && decode_party_code(v1b, &from_v1, &err));
    CHECK(decode_party_code(make_party_code({203, 0, 113, 7}, 9307, secret, 0), &from_v3, &err));
    CHECK(crypto::derive_party_key("pw", from_v3.secret.data()) == crypto::derive_party_key("pw", secret.data()));
    const auto host_key = crypto::derive_party_key("pw", secret.data());
    CHECK(crypto::derive_party_key("pw", from_v2.secret.data()) == host_key);
    CHECK(crypto::derive_party_key("pw", from_v1.secret.data()) == host_key);

    // A typo in the flags symbol changes the expected length: still rejected.
    std::string flagtypo = code;
    flagtypo[5] = flagtypo[5] == '0' ? '1' : '0';
    CHECK(!decode_party_code(flagtypo, &pc2, &err));

    // LAN code: only the last octet; the guest completes it from its own address.
    const std::string lan_code = make_party_code({192, 168, 1, 42}, 9307, secret, kCodeFlagLan);
    CHECK(lan_code.rfind("BBP3-", 0) == 0 && lan_code.size() == 5 + 14 + 3);
    CHECK(decode_party_code(lan_code, &pc2, &err) && pc2.lan_partial && (pc2.flags & kCodeFlagLan) &&
          pc2.ipv4[3] == 42 && pc2.host.empty() && pc2.secret == secret && pc2.port == 9307);
    CHECK(pc2.address() == "*.*.*.42:9307");
    fill_lan_prefix(&pc2, {10, 0, 7, 3});
    CHECK(pc2.address() == "10.0.7.42:9307" && pc2.ipv4 == (std::array<std::uint8_t, 4>{10, 0, 7, 42}));
    const std::string lan_port = make_party_code({192, 168, 1, 42}, 47001, secret, kCodeFlagLan);
    CHECK(decode_party_code(lan_port, &pc2, &err) && pc2.port == 47001 && pc2.lan_partial &&
          lan_port.size() == 5 + 17 + 4);
    std::printf("  LAN code: %s, port 47001: %s\n", lan_code.c_str(), lan_port.c_str());

    std::array<std::uint8_t, 4> lan{};
    if (best_local_ipv4(&lan)) {
        std::string lc;
        CHECK(make_lan_code(9307, secret, 0, &lc));
        CHECK(decode_party_code(lc, &pc2, &err) && (pc2.flags & kCodeFlagLan) && pc2.ipv4[3] == lan[3]);
        std::printf("  LAN address %s -> %s\n", format_ipv4(lan).c_str(), lc.c_str());
    } else {
        std::printf("  (no LAN adapter)\n");
    }
}

static void test_crypto() {
    std::uint8_t secret[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto t0 = std::chrono::steady_clock::now();
    crypto::Key k1 = crypto::derive_party_key("pw", secret);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  argon2i: %lld ms\n", static_cast<long long>(ms));
    crypto::Key k2 = crypto::derive_party_key("pw", secret);
    crypto::Key k3 = crypto::derive_party_key("pX", secret);
    CHECK(k1 == k2 && k1 != k3);
    crypto::Nonce a{}, b{};
    crypto::random_bytes(a.data(), a.size());
    crypto::random_bytes(b.data(), b.size());
    CHECK(a != b);
    crypto::Aead tx, rx;
    tx.init(crypto::session_key(k1, "bbp-h2g", a, b));
    rx.init(crypto::session_key(k1, "bbp-h2g", a, b));
    const char msg[] = "hello party";
    std::uint8_t ct[sizeof msg], mac[16], pt[sizeof msg];
    std::uint8_t ad[5] = {1, 2, 3, 4, 5};
    tx.seal(ct, mac, ad, 5, reinterpret_cast<const std::uint8_t*>(msg), sizeof msg);
    std::uint8_t ct2[sizeof msg], mac2[16];
    std::memcpy(ct2, ct, sizeof ct);
    std::memcpy(mac2, mac, 16);
    ct2[0] ^= 1;
    CHECK(!rx.open(pt, mac2, ad, 5, ct2, sizeof ct2));  // tampered
    CHECK(rx.open(pt, mac, ad, 5, ct, sizeof ct) && std::memcmp(pt, msg, sizeof msg) == 0);
    CHECK(!rx.open(pt, mac, ad, 5, ct, sizeof ct));  // replay: the counter moved on
}

static void test_stun_local() {
    sock::startup();
    sock::Socket srv = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof a);
    sock::socklen al = sizeof a;
    getsockname(srv, reinterpret_cast<sockaddr*>(&a), &al);
    std::uint16_t srv_port = ntohs(a.sin_port);
    std::thread responder([srv] {
        std::uint8_t buf[1500];
        sockaddr_in from{};
        sock::socklen fl = sizeof from;
        long n = ::recvfrom(srv, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) return;
        std::uint8_t txid[net::stun::kTxid], tok[net::stun::kTokenLen];
        bool hello = false;
        if (!net::stun::parse_binding_request(buf, static_cast<std::size_t>(n), txid, &hello, tok)) return;
        std::uint8_t out[net::stun::kMaxResponse];
        std::uint32_t addr;
        std::memcpy(&addr, &from.sin_addr, 4);
        std::size_t len = net::stun::build_binding_response(out, txid, addr, ntohs(from.sin_port));
        ::sendto(srv, reinterpret_cast<const char*>(out), static_cast<int>(len), 0, reinterpret_cast<sockaddr*>(&from),
                 fl);
    });
    std::array<std::uint8_t, 4> ip{};
    std::uint16_t port = 0;
    std::string err;
    bool ok = stun_query(-1, "127.0.0.1:" + std::to_string(srv_port), 0, &ip, &port, &err, 1000, 2);
    responder.join();
    sock::close(srv);
    CHECK(ok);
    CHECK(ip[0] == 127 && ip[3] == 1 && port != 0);
    std::printf("  STUN (local responder): %s:%u %s\n", format_ipv4(ip).c_str(), port, err.c_str());

#if defined(_WIN32)
    _putenv("BB_PARTY_PUBLIC_ADDR=198.51.100.9:9999");
#else
    setenv("BB_PARTY_PUBLIC_ADDR", "198.51.100.9:9999", 1);
#endif
    PublicAddress pa = resolve_public_address(9307);
    CHECK(pa.ok && pa.source == "override" && pa.port == 9999 && pa.ip[0] == 198);
#if defined(_WIN32)
    _putenv("BB_PARTY_PUBLIC_ADDR=");
    _putenv("BB_PARTY_UPNP=0");
#else
    unsetenv("BB_PARTY_PUBLIC_ADDR");
    setenv("BB_PARTY_UPNP", "0", 1);
#endif
    UpnpMapper upnp;
    upnp.start(9307);
    UpnpResult ur = upnp.wait(100);
    CHECK(ur.done && ur.disabled && !ur.ok);
    std::printf("  %s\n", ur.message.c_str());
}

// ---------------- loopback party ----------------

struct Recorder {
    std::mutex mu;
    std::vector<std::string> log;
    std::atomic<int> joined{0}, rejoined{0}, left_kept{0}, left_released{0}, events{0}, cmds{0}, progress{0},
        rosters{0};
    std::vector<std::uint64_t> cursors;
    std::vector<std::string> event_names;
    void add(std::string s) {
        std::lock_guard<std::mutex> lk(mu);
        log.push_back(std::move(s));
    }
    bool has_event(const std::string& n) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& e : event_names)
            if (e == n) return true;
        return false;
    }
    int count_event(const std::string& n) {
        std::lock_guard<std::mutex> lk(mu);
        int k = 0;
        for (auto& e : event_names)
            if (e == n) ++k;
        return k;
    }
};

static LinkConfig base_cfg(const char* name, const char* password) {
    LinkConfig c;
    c.name = name;
    c.password = password;
    c.secret = {9, 8, 7, 6, 5, 4, 3, 2};
    c.eboot_sha256.fill(0xAB);
    c.mods_hash.fill(0x11);
    c.patches_hash.fill(0x22);
    c.patch_names = {"Party: Bells anywhere", "Party: Skip Online/Offline Choice (Online)"};
    c.mod_names = {"Better Params (12 files)"};
    c.build_id = "0.5 (v0.5-42-gabcdef1234)";
    c.params_sha256.fill(0x33);
    c.hook_names = {"4p H1 cooperator slots (SessionSlotSummary) @+0x186fe40", "travel lamp warp (0x13CDF30) @+0x13cdf30"};
    c.hooks_hash = hook_set_hash(c.hook_names);
    c.port = 0;  // ephemeral: tests never collide with a running game
    c.bind_addr = "127.0.0.1";
    c.max_players = 3;
    c.ping_interval_ms = 100;
    c.lost_timeout_ms = 700;
    c.slot_keep_ms = 2500;
    c.backoff_initial_ms = 100;
    c.backoff_max_ms = 400;
    c.connect_timeout_ms = 1000;
    c.roster_refresh_ms = 300;
    return c;
}

static LinkCallbacks recorder_callbacks(Recorder& r, const char* who) {
    LinkCallbacks cb;
    std::string w = who;
    cb.on_member_joined = [&r, w](const RosterEntry& m, bool rejoined) {
        (rejoined ? r.rejoined : r.joined)++;
        r.add(w + " joined " + m.name + " slot " + std::to_string(m.slot) + (rejoined ? " (rejoined)" : ""));
    };
    cb.on_member_left = [&r, w](const RosterEntry& m, bool kept) {
        (kept ? r.left_kept : r.left_released)++;
        r.add(w + " left " + m.name + (kept ? " (slot kept)" : " (released)"));
    };
    cb.on_event = [&r](int, std::uint64_t cursor, const std::string& name, const std::string&) {
        std::lock_guard<std::mutex> lk(r.mu);
        r.cursors.push_back(cursor);
        r.event_names.push_back(name);
        r.events++;
    };
    cb.on_party_cmd = [&r](int, const std::string&, const std::string&) { r.cmds++; };
    cb.on_progress = [&r](int, const std::vector<std::uint8_t>& b) {
        if (b.size() == 100000 && b[99999] == 0x5a) r.progress++;
    };
    cb.on_roster = [&r](const std::vector<RosterEntry>&) { r.rosters++; };
    if (std::getenv("PARTY_TEST_VERBOSE"))
        cb.on_log = [w](const std::string& l) { std::printf("    [%s] %s\n", w.c_str(), l.c_str()); };
    return cb;
}

static void test_party() {
    Recorder hr, ar, br, cr;
    LinkCallbacks hcb = recorder_callbacks(hr, "host");
    hcb.on_rpc = [](int slot, const std::string& kind, const std::string& json) {
        return "{\"slot\":" + std::to_string(slot) + ",\"kind\":\"" + kind + "\",\"echo\":" + json + "}";
    };
    PartyLink host(base_cfg("Host", "hunter2"), hcb);
    std::string err;
    CHECK(host.start_host(&err));
    std::uint16_t port = host.bound_port();
    std::printf("  host listening on 127.0.0.1:%u\n", port);
    CHECK(port != 0 && host.state() == LinkState::Hosting);

    PartyLink a(base_cfg("Alice", "hunter2"), recorder_callbacks(ar, "alice"));
    PartyLink b(base_cfg("Bob_2", "hunter2"), recorder_callbacks(br, "bob"));
    CHECK(a.start_guest("127.0.0.1", port, &err));
    CHECK(b.start_guest("127.0.0.1", port, &err));
    CHECK(a.wait_connected(5000));
    CHECK(b.wait_connected(5000));
    CHECK(a.local_slot() != b.local_slot() && a.local_slot() >= 1 && b.local_slot() >= 1);
    CHECK(a.max_players() == 3);
    CHECK(wait_for([&] { return hr.joined.load() == 2; }, 2000));
    CHECK(wait_for([&] { return a.roster().size() == 3 && b.roster().size() == 3; }, 2000));
    std::printf("  alice slot %d (seen at %s), bob slot %d\n", a.local_slot(), a.observed_address().c_str(),
                b.local_slot());
    CHECK(a.observed_address().rfind("127.0.0.1:", 0) == 0);

    // Wrong password, mismatched hash, duplicate name, party full.
    {
        Recorder xr;
        PartyLink bad(base_cfg("Mallory", "wrong"), recorder_callbacks(xr, "mallory"));
        CHECK(bad.start_guest("127.0.0.1", port, &err));
        CHECK(bad.wait_state(LinkState::Rejected, 5000));
        CHECK(bad.reject_code() == RejectCode::Auth);
        CHECK(bad.reject_reason() == auth_reject_reason(true));  // the host has a password
        std::printf("  wrong password: %s\n", bad.reject_reason().c_str());
    }
    {
        Recorder xr;
        LinkConfig c = base_cfg("Modder", "hunter2");
        c.mods_hash[3] ^= 1;
        PartyLink bad(c, recorder_callbacks(xr, "modder"));
        CHECK(bad.start_guest("127.0.0.1", port, &err));
        CHECK(bad.wait_state(LinkState::Rejected, 5000));
        CHECK(bad.reject_code() == RejectCode::Mismatch);
        CHECK(bad.reject_reason().find("gameplay mods differ") != std::string::npos);
        CHECK(bad.reject_reason().find("patches") == std::string::npos);
        CHECK(bad.reject_reason().find("eboot") == std::string::npos);
        std::printf("  mismatched mods: %s\n", bad.reject_reason().c_str());
    }
    {
        // Another gameplay patch set and another eboot: the guest learns both, by name.
        Recorder xr;
        LinkConfig c = base_cfg("Cheater", "hunter2");
        c.patches_hash[0] ^= 1;
        c.patch_names = {"Party: Skip Online/Offline Choice (Online)", "Player No Dead (Read note)"};
        c.eboot_sha256[0] = 0x01;
        PartyLink bad(c, recorder_callbacks(xr, "cheater"));
        CHECK(bad.start_guest("127.0.0.1", port, &err));
        CHECK(bad.wait_state(LinkState::Rejected, 5000));
        CHECK(bad.reject_code() == RejectCode::Mismatch);
        const std::string why = bad.reject_reason();
        CHECK(why.find("eboot.bin differs") != std::string::npos);
        CHECK(why.find("host only: Party: Bells anywhere") != std::string::npos);
        CHECK(why.find("yours only: Player No Dead (Read note)") != std::string::npos);
        CHECK(why.find("mods") == std::string::npos);
        std::printf("  mismatched eboot + patches: %s\n", why.c_str());
    }
    {
        // A9 over the wire: another bbport build and another party hook set.
        Recorder xr;
        LinkConfig c = base_cfg("Oldbuild", "hunter2");
        c.build_id = "0.5 (v0.5-7-g1111111111-dirty)";
        c.hook_names.pop_back();
        c.hooks_hash = hook_set_hash(c.hook_names);
        PartyLink bad(c, recorder_callbacks(xr, "oldbuild"));
        CHECK(bad.start_guest("127.0.0.1", port, &err));
        CHECK(bad.wait_state(LinkState::Rejected, 5000));
        CHECK(bad.reject_code() == RejectCode::Mismatch);
        const std::string why = bad.reject_reason();
        CHECK(why.find("bbport build differs") != std::string::npos && why.find("v0.5-7-g1111111111-dirty") != std::string::npos);
        CHECK(why.find("party patches differ (host only: travel lamp warp") != std::string::npos);
        CHECK(why.find("eboot") == std::string::npos && why.find("game params") == std::string::npos);
        std::printf("  other build + hooks: %s\n", why.c_str());
    }
    {
        // Cosmetic patches never reach the hash: only the gameplay set is compared, so a guest
        // with the same gameplay set (whatever its graphics patches) joins. Here: same everything.
        CHECK(identity_mismatch(base_cfg("A", ""), base_cfg("B", "")).empty());
    }
    {
        Recorder xr;
        PartyLink bad(base_cfg("Alice", "hunter2"), recorder_callbacks(xr, "alice2"));
        CHECK(bad.start_guest("127.0.0.1", port, &err));
        CHECK(bad.wait_state(LinkState::Rejected, 5000));
        CHECK(bad.reject_code() == RejectCode::Name);
    }
    {
        Recorder xr;
        PartyLink extra(base_cfg("Carol", "hunter2"), recorder_callbacks(xr, "carol"));
        CHECK(extra.start_guest("127.0.0.1", port, &err));
        CHECK(extra.wait_state(LinkState::Rejected, 5000));
        CHECK(extra.reject_code() == RejectCode::Full);
        std::printf("  third guest: %s\n", extra.reject_reason().c_str());
    }
    CHECK(hr.joined.load() == 2);

    // Encrypted RPC, sync and async.
    std::string reply;
    CHECK(a.rpc_call("ss.info", "{\"x\":1}", &reply, 2000));
    std::printf("  rpc reply: %s\n", reply.c_str());
    CHECK(reply == "{\"slot\":" + std::to_string(a.local_slot()) + ",\"kind\":\"ss.info\",\"echo\":{\"x\":1}}");
    std::atomic<int> async_ok{0};
    std::string big(200000, 'j');
    b.rpc_call_async("big", "\"" + big + "\"", [&](bool ok, const std::string& r) {
        if (ok && r.size() > 200000) async_ok++;
    }, 2000);
    CHECK(wait_for([&] { return async_ok.load() == 1; }, 3000));
    CHECK(!host.rpc_call("x", "{}", &reply, 100));  // host side has no rpc_call

    // Events: targeted, broadcast, guest -> host; party commands, progress blobs.
    CHECK(host.send_event(a.local_slot(), "only_alice", "{}") == 1);
    CHECK(host.send_event(kBroadcast, "everyone", "{\"n\":1}") != 0);
    CHECK(wait_for([&] { return ar.has_event("only_alice") && ar.has_event("everyone") && br.has_event("everyone"); },
                   2000));
    CHECK(!br.has_event("only_alice"));
    CHECK(a.send_event(0, "guest_said", "{}") == 1);
    CHECK(wait_for([&] { return hr.has_event("guest_said"); }, 2000));
    // post_event (the game's main thread): never waits for the IO lock; same delivery.
    for (int k = 0; k < 50; ++k) host.post_event(kBroadcast, "posted_" + std::to_string(k), "{}");
    a.post_event(0, "guest_posted", "{}");
    CHECK(wait_for([&] { return ar.has_event("posted_49") && br.has_event("posted_49") && hr.has_event("guest_posted"); },
                   2000));
    CHECK(ar.has_event("posted_0") && br.has_event("posted_0"));
    CHECK(host.send_party_cmd(kBroadcast, "warp", "{\"map\":1}"));
    CHECK(b.send_party_cmd(0, "ready", "{}"));
    std::vector<std::uint8_t> blob(100000, 1);
    blob[99999] = 0x5a;
    CHECK(host.send_progress(b.local_slot(), blob));
    CHECK(wait_for([&] { return ar.cmds.load() == 1 && br.cmds.load() == 1 && hr.cmds.load() == 1; }, 2000));
    CHECK(wait_for([&] { return br.progress.load() == 1; }, 2000));

    // Roster state from a guest reaches everyone.
    a.set_local_state(MemberState::InHostWorld, 0x0A000000);
    CHECK(wait_for([&] {
        for (auto& e : b.roster())
            if (e.name == "Alice" && e.state == MemberState::InHostWorld && e.map_id == 0x0A000000) return true;
        return false;
    }, 2000));
    CHECK(wait_for([&] { return a.rtt_ms() < 1000 && std::llabs(a.host_clock_ms() - host.host_clock_ms()) < 100; },
                   2000));

    // Disconnect detection: Alice's network freezes past the lost timeout. The host notices the
    // silence and keeps the slot; an event queued meanwhile arrives after Alice reconnects.
    int alice_slot = a.local_slot();
    a.debug_freeze(1200);
    CHECK(wait_for([&] { return hr.left_kept.load() == 1; }, 3000));
    CHECK(host.send_event(alice_slot, "while_away", "{}") != 0);
    bool kept = false;
    for (auto& e : host.roster())
        if (e.slot == alice_slot && !e.connected) kept = true;
    CHECK(kept);
    CHECK(wait_for([&] { return hr.rejoined.load() == 1; }, 5000));
    CHECK(a.wait_connected(3000));
    CHECK(a.local_slot() == alice_slot);
    CHECK(wait_for([&] { return ar.has_event("while_away"); }, 2000));
    CHECK(ar.count_event("everyone") == 1);  // no duplicate delivery after the replay
    CHECK(a.rpc_call("after", "{}", &reply, 2000));

    // A hard drop (no BYE) of Alice's socket: the guest reconnects by itself.
    a.debug_drop_connections();
    CHECK(wait_for([&] { return hr.rejoined.load() == 2; }, 5000));
    CHECK(a.wait_connected(3000) && a.local_slot() == alice_slot);

    // Bob crashes: slot kept, then released after slot_keep; Carol can then join.
    int lost_before = hr.left_kept.load();
    b.stop(false);
    CHECK(wait_for([&] { return hr.left_kept.load() == lost_before + 1; }, 3000));
    {
        Recorder xr;
        PartyLink early(base_cfg("Carol", "hunter2"), recorder_callbacks(xr, "carol"));
        CHECK(early.start_guest("127.0.0.1", port, &err));
        CHECK(early.wait_state(LinkState::Rejected, 5000) && early.reject_code() == RejectCode::Full);
    }
    CHECK(wait_for([&] { return hr.left_released.load() == 1; }, 5000));
    PartyLink c(base_cfg("Carol", "hunter2"), recorder_callbacks(cr, "carol"));
    CHECK(c.start_guest("127.0.0.1", port, &err));
    CHECK(c.wait_connected(5000));
    CHECK(c.rpc_call("hello", "{}", &reply, 2000));

    // Graceful leave frees the slot immediately; host shutdown ends the party for the rest.
    c.stop();
    CHECK(wait_for([&] { return hr.left_released.load() == 2; }, 2000));
    host.stop();
    CHECK(a.wait_state(LinkState::Rejected, 3000));
    CHECK(a.reject_code() == RejectCode::Shutdown);
    std::printf("  after host stop: alice %s (%s)\n", link_state_name(a.state()), a.reject_reason().c_str());
    a.stop();

    std::printf("  host saw: joined %d, rejoined %d, lost %d, released %d, rosters %d\n", hr.joined.load(),
                hr.rejoined.load(), hr.left_kept.load(), hr.left_released.load(), hr.rosters.load());
}

static void test_max_players_two() {
    LinkCallbacks none;
    LinkConfig hc = base_cfg("Host", "");
    hc.max_players = 2;
    PartyLink host(hc, none);
    std::string err;
    CHECK(host.start_host(&err));
    PartyLink a(base_cfg("A", ""), none), b(base_cfg("B", ""), none);
    CHECK(a.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(a.wait_connected(5000));
    CHECK(b.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(b.wait_state(LinkState::Rejected, 5000) && b.reject_code() == RejectCode::Full);
    // Code secret alone (no password): a guest with another secret is rejected.
    LinkConfig other = base_cfg("D", "");
    other.secret[0] ^= 0xFF;
    PartyLink d(other, none);
    CHECK(d.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(d.wait_state(LinkState::Rejected, 5000) && d.reject_code() == RejectCode::Auth);
    CHECK(d.reject_reason() == auth_reject_reason(false));  // an old code, not a password problem
}

// A plain host:port (no party code, so no secret) with the right password joins a host that has
// a password (party_code.h; the in-game party flow saves such addresses); without the password
// (or when the host has none) it is refused.
static void test_plain_address_password() {
    LinkCallbacks none;
    PartyLink host(base_cfg("Host", "pw"), none);
    std::string err;
    CHECK(host.start_host(&err));
    LinkConfig plain = base_cfg("Plain", "pw");
    plain.secret = {};
    PartyLink p(plain, none);
    CHECK(p.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(p.wait_connected(5000));
    CHECK(wait_for([&] { return host.roster().size() == 2; }, 2000));
    LinkConfig wrong = base_cfg("Wrong", "nope");
    wrong.secret = {};
    PartyLink w(wrong, none);
    CHECK(w.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(w.wait_state(LinkState::Rejected, 5000) && w.reject_code() == RejectCode::Auth);
    PartyLink open(base_cfg("Host2", ""), none);
    CHECK(open.start_host(&err));
    LinkConfig nopw = base_cfg("NoPw", "");
    nopw.secret = {};
    PartyLink n(nopw, none);
    CHECK(n.start_guest("127.0.0.1", open.bound_port(), &err));
    CHECK(n.wait_state(LinkState::Rejected, 5000) && n.reject_code() == RejectCode::Auth);
    std::printf("  plain address + password: joined; wrong / no password: refused\n");
}

// Four players: host + 3 guests (slots 1..3, roster of 4, events to all), a 4th guest is
// refused, a guest's own max is overridden by the host's WELCOME, a guest with other game
// rules (party_fourp.h tag) is rejected with a reason naming both.
static void test_four_players() {
    Recorder hr, ar, br, cr;
    LinkConfig hc = base_cfg("Host", "");
    hc.max_players = 4;
    hc.rules = "max 4 players, 4p:v1:H1,H2,H3,H4,E6";
    PartyLink host(hc, recorder_callbacks(hr, "host"));
    std::string err;
    CHECK(host.start_host(&err));
    CHECK(host.max_players() == 4 && kMaxPartyPlayers == 5);
    {
        LinkConfig big = base_cfg("Big", "");
        big.max_players = 9;  // clamped to kMaxPartyPlayers
        PartyLink b(big, LinkCallbacks{});
        std::string e2;
        CHECK(b.start_host(&e2) && b.max_players() == kMaxPartyPlayers);
    }
    const std::uint16_t port = host.bound_port();
    auto guest_cfg = [&](const char* name) {
        LinkConfig c = base_cfg(name, "");
        c.max_players = 3;  // the guest's own setting: the host's counts
        c.rules = hc.rules;
        return c;
    };
    PartyLink a(guest_cfg("A"), recorder_callbacks(ar, "a")), b(guest_cfg("B"), recorder_callbacks(br, "b")),
        c(guest_cfg("C"), recorder_callbacks(cr, "c"));
    CHECK(a.start_guest("127.0.0.1", port, &err));
    CHECK(b.start_guest("127.0.0.1", port, &err));
    CHECK(c.start_guest("127.0.0.1", port, &err));
    CHECK(a.wait_connected(5000) && b.wait_connected(5000) && c.wait_connected(5000));
    CHECK(a.max_players() == 4 && b.max_players() == 4 && c.max_players() == 4);
    const int sa = a.local_slot(), sb = b.local_slot(), sc = c.local_slot();
    CHECK(sa >= 1 && sa <= 3 && sb >= 1 && sb <= 3 && sc >= 1 && sc <= 3 && sa != sb && sb != sc && sa != sc);
    CHECK(wait_for([&] { return hr.joined.load() == 3; }, 3000));
    CHECK(wait_for([&] { return a.roster().size() == 4 && b.roster().size() == 4 && c.roster().size() == 4; }, 3000));
    std::printf("  slots %d %d %d, roster %zu\n", sa, sb, sc, host.roster().size());
    host.send_event(kBroadcast, "four", "{}");
    CHECK(wait_for([&] { return ar.count_event("four") == 1 && br.count_event("four") == 1 && cr.count_event("four") == 1; }, 3000));
    LinkCallbacks none;
    PartyLink d(guest_cfg("D"), none);
    CHECK(d.start_guest("127.0.0.1", port, &err));
    CHECK(d.wait_state(LinkState::Rejected, 5000) && d.reject_code() == RejectCode::Full);
    LinkConfig ec = guest_cfg("E");
    ec.rules = "max 3 players, 4p:v1:H1,H2,H3,H4,E6";
    PartyLink e(ec, none);
    c.stop();  // a free slot: the rejection below is about the rules, not the size
    CHECK(wait_for([&] { return host.roster().size() == 3; }, 3000));
    CHECK(e.start_guest("127.0.0.1", port, &err));
    CHECK(e.wait_state(LinkState::Rejected, 5000) && e.reject_code() == RejectCode::Mismatch);
    std::printf("  other rules: %s\n", e.reject_reason().c_str());
    CHECK(e.reject_reason().find("party rules differ") != std::string::npos &&
          e.reject_reason().find("max 3 players") != std::string::npos &&
          e.reject_reason().find("max 4 players") != std::string::npos);
}

// Party HUD vitals: the sanitizer, then host + 2 guests exchanging VITALS (each sees the others).
static void test_vitals() {
    MemberVitals v;
    v.slot = 2;
    v.flags = 0xff;
    v.hp = 500000;
    v.max_hp = 4000000;
    v.stamina = 60000;
    v.max_stamina = 60000;
    v.pos[0] = 1e9f;
    CHECK(sanitize_vitals(v));
    CHECK(v.flags == (kVitalsValid | kVitalsAlive) && v.max_hp == kMaxVitalsHp && v.hp == kMaxVitalsHp);
    CHECK(v.max_stamina == kMaxVitalsStamina && v.stamina == kMaxVitalsStamina && v.pos[0] == 0.0f);
    MemberVitals w;
    w.slot = kMaxVitals;
    CHECK(!sanitize_vitals(w));
    w.slot = 1;
    w.flags = kVitalsValid | kVitalsAlive | kVitalsHasPos;
    w.hp = 0;
    w.max_hp = 100;
    w.pos[1] = std::nanf("");
    w.yaw = 7.0f;
    CHECK(sanitize_vitals(w));
    CHECK(w.flags == kVitalsValid);  // dead, no position
    w.pos[1] = 3.0f;
    w.flags = kVitalsHasPos;
    w.hp = 50;
    w.yaw = 7.0f;
    CHECK(sanitize_vitals(w) && w.hp == 0 && w.max_hp == 0 && w.flags == kVitalsHasPos && w.yaw < 3.1416f &&
          w.yaw > 0.7f);

    PartyLink host(base_cfg("Host", ""), LinkCallbacks{});
    std::string err;
    CHECK(host.start_host(&err));
    const std::uint16_t port = host.bound_port();
    PartyLink a(base_cfg("Alice", ""), LinkCallbacks{}), b(base_cfg("Bob", ""), LinkCallbacks{});
    CHECK(a.start_guest("127.0.0.1", port, &err) && b.start_guest("127.0.0.1", port, &err));
    CHECK(a.wait_connected(5000) && b.wait_connected(5000));
    auto mk = [](std::uint32_t hp, float x) {
        MemberVitals m;
        m.slot = 3;  // ignored: the link fills in its own slot
        m.flags = kVitalsValid | kVitalsAlive | kVitalsHasPos;
        m.hp = hp;
        m.max_hp = 1000;
        m.stamina = 80;
        m.max_stamina = 100;
        m.map_id = 0x18000000;
        m.pos[0] = x;
        m.pos[1] = 1;
        m.pos[2] = -x;
        m.yaw = 0.5f;
        return m;
    };
    host.set_local_vitals(mk(900, 1));
    a.set_local_vitals(mk(800, 2));
    b.set_local_vitals(mk(700, 3));
    auto find = [](const PartyLink& l, int slot, MemberVitals* out) {
        MemberVitals all[kMaxVitals];
        const int n = l.vitals(all, kMaxVitals);
        for (int i = 0; i < n; ++i)
            if (all[i].slot == slot) {
                *out = all[i];
                return true;
            }
        return false;
    };
    MemberVitals got;
    const int sa = a.local_slot(), sb = b.local_slot();
    CHECK(wait_for([&] { return find(host, sa, &got) && got.hp == 800; }, 3000));
    CHECK(got.pos[0] == 2.0f && got.map_id == 0x18000000 && got.stamina == 80 && got.yaw == 0.5f);
    CHECK(wait_for([&] { return find(a, 0, &got) && got.hp == 900; }, 3000));
    CHECK(wait_for([&] { return find(a, sb, &got) && got.hp == 700 && got.pos[2] == -3.0f; }, 3000));
    CHECK(wait_for([&] { return find(b, sa, &got) && got.hp == 800; }, 3000));
    CHECK(find(a, sa, &got) && got.hp == 800 && got.age_ms == 0);  // its own
    // A change gets through within a few send intervals.
    a.set_local_vitals(mk(10, 2));
    CHECK(wait_for([&] { return find(b, sa, &got) && got.hp == 10; }, 3000));
    // The overhead display extras (the VITALS trailer): guest -> host -> the other guest.
    MemberVitals ext = mk(10, 2);
    ext.level = 42;
    ext.deaths = 7;
    ext.rot = 3;
    a.set_local_vitals(ext);
    CHECK(wait_for([&] { return find(b, sa, &got) && got.level == 42; }, 3000));
    CHECK(got.deaths == 7 && got.rot == 3 && got.hp == 10);
    CHECK(wait_for([&] { return find(host, sa, &got) && got.level == 42 && got.rot == 3; }, 3000));
    // A guest that leaves goes stale on the others.
    b.stop();
    CHECK(wait_for([&] { return !find(a, sb, &got); }, kVitalsStaleMs + 3000));
    std::printf("  vitals relayed (slots %d, %d)\n", sa, sb);
}

// A crashed host restarted on the same port with the same secret and the member table it kept:
// every guest gets its old slot back (a new session: not resumed), nobody is rejected.
static void test_host_restart() {
    Recorder hr, ar, br, h2r;
    PartyLink* host = new PartyLink(base_cfg("Host", ""), recorder_callbacks(hr, "host"));
    std::string err;
    CHECK(host->start_host(&err));
    const std::uint16_t port = host->bound_port();
    PartyLink a(base_cfg("Alice", ""), recorder_callbacks(ar, "alice"));
    PartyLink b(base_cfg("Bob", ""), recorder_callbacks(br, "bob"));
    CHECK(a.start_guest("127.0.0.1", port, &err));
    CHECK(a.wait_connected(5000));
    CHECK(b.start_guest("127.0.0.1", port, &err));
    CHECK(b.wait_connected(5000));
    const int sa = a.local_slot(), sb = b.local_slot();
    const std::vector<KeptMember> kept = host->kept_members();
    CHECK(kept.size() == 2);
    host->stop(false);  // the crash: no BYE
    CHECK(a.wait_state(LinkState::Reconnecting, 3000));
    CHECK(b.wait_state(LinkState::Reconnecting, 3000));
    // Reversed join order on purpose: Bob first. Slots come from the kept table, not the order.
    LinkConfig hc = base_cfg("Host", "");
    hc.port = port;
    PartyLink host2(hc, recorder_callbacks(h2r, "host2"));
    host2.restore_members(kept);
    bool up = false;
    for (int i = 0; i < 50 && !(up = host2.start_host(&err)); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(up);
    CHECK(wait_for([&] { return a.state() == LinkState::Connected && b.state() == LinkState::Connected; }, 5000));
    CHECK(a.local_slot() == sa && b.local_slot() == sb);
    CHECK(!a.last_welcome_resumed() && !b.last_welcome_resumed());
    CHECK(wait_for([&] {
        int conn = 0;
        for (const RosterEntry& e : host2.roster()) conn += e.connected;
        return conn == 3;
    }, 3000));
    CHECK(wait_for([&] { return h2r.rejoined.load() == 2; }, 2000) && h2r.joined.load() == 0);  // callbacks lag
    // Events flow on the new streams.
    host2.send_event(kBroadcast, "after_restart", "{}");
    CHECK(wait_for([&] { return ar.events.load() >= 1 && br.events.load() >= 1; }, 3000));
    std::printf("  restarted host on :%u: alice slot %d, bob slot %d back\n", port, a.local_slot(), b.local_slot());
    a.stop();
    b.stop();
    host2.stop();
}

// The version check's explanation and the identity files patches.py / mods.py write.
static void test_identity() {
    LinkConfig host, guest;
    host.eboot_sha256.fill(1);
    guest.eboot_sha256.fill(1);
    CHECK(identity_mismatch(host, guest).empty());
    guest.eboot_sha256[31] = 2;
    std::string why = identity_mismatch(host, guest);
    CHECK(why.find("eboot.bin differs") != std::string::npos);
    CHECK(why.find("patches") == std::string::npos && why.find("mods") == std::string::npos);
    guest.eboot_sha256 = host.eboot_sha256;
    // Patches: names only on one side, then the same names with other bytes.
    host.patch_names = {"A", "B", "C"};
    host.patches_hash.fill(3);
    guest.patch_names = {"B", "D"};
    guest.patches_hash.fill(4);
    why = identity_mismatch(host, guest);
    CHECK(why.find("gameplay patches differ (host only: A, C; yours only: D)") != std::string::npos);
    guest.patch_names = host.patch_names;
    why = identity_mismatch(host, guest);
    CHECK(why.find("same names, different contents") != std::string::npos);
    guest.patches_hash = host.patches_hash;
    // Mods: the host has none (zero hash), the guest one.
    guest.mod_names = {"Texture-free param mod (3 files)"};
    guest.mods_hash.fill(5);
    why = identity_mismatch(host, guest);
    CHECK(why.find("gameplay mods differ (host only: -; yours only: Texture-free param mod (3 files))") !=
          std::string::npos);
    CHECK(why.find("patches") == std::string::npos);
    // Many names are cut.
    guest.mod_names.clear();
    for (int i = 0; i < 10; ++i) guest.mod_names.push_back("m" + std::to_string(i));
    CHECK(identity_mismatch(host, guest).find("+4 more") != std::string::npos);
    std::printf("  %s\n", identity_mismatch(host, guest).c_str());

    std::array<std::uint8_t, 32> h{};
    std::vector<std::string> items;
    const std::string text =
        "# comment\r\nhash 00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF\r\n"
        "patch Party: Bells anywhere\r\npatch Player No Dead (Read note)\r\n# cosmetic 60 FPS++\r\nmod x\r\n";
    CHECK(parse_identity_file(text, "patch", &h, &items));
    CHECK(h[0] == 0x00 && h[1] == 0x11 && h[15] == 0xff && h[31] == 0xff);
    CHECK(items.size() == 2 && items[0] == "Party: Bells anywhere" && items[1] == "Player No Dead (Read note)");
    CHECK(parse_identity_file(text, "mod", &h, &items) && items.size() == 1 && items[0] == "x");
    CHECK(!parse_identity_file("patch A\n", "patch", &h, &items));
    CHECK(!parse_identity_file("hash 0011\n", "patch", &h, &items));
    CHECK(!parse_identity_file("hash " + std::string(64, 'g') + "\n", "patch", &h, &items));
    const std::array<std::uint8_t, 32> zero{};
    CHECK(parse_identity_file("hash " + std::string(64, '0') + "\n", "mod", &h, &items) && items.empty() && h == zero);

    // A9: build, params, party hooks; each clause says what to do.
    {
        LinkConfig hb = base_cfg("H", ""), gb = base_cfg("G", "");
        CHECK(identity_mismatch(hb, gb).empty());
        gb.build_id = "0.5 (v0.5-40-g0123456789)";
        std::string w = identity_mismatch(hb, gb);
        CHECK(w.find("bbport build differs (host 0.5 (v0.5-42-gabcdef1234), yours 0.5 (v0.5-40-g0123456789))") !=
              std::string::npos);
        CHECK(w.find("install the same bbport release as the host") != std::string::npos);
        CHECK(w.find("version mismatch with the host: bbport build differs") == 0);  // named once
        CHECK(w.find("params") == std::string::npos && w.find("party patches") == std::string::npos);
        hb.check_build = false;  // BB_PARTY_CHECK_BUILD=0 on the host
        CHECK(identity_mismatch(hb, gb).empty());
        hb.check_build = true;
        gb.build_id.clear();  // an older build sends none
        CHECK(identity_mismatch(hb, gb).find("unknown: an older build") != std::string::npos);
        gb.build_id = hb.build_id;
        gb.params_sha256[7] ^= 0x40;
        w = identity_mismatch(hb, gb);
        CHECK(w.find("game params differ (gameparam.parambnd.dcx") != std::string::npos);
        // Same build, something else differs: the message still names both versions.
        CHECK(w.find("version mismatch with the host (host bbport 0.5 (v0.5-42-gabcdef1234), yours 0.5 "
                     "(v0.5-42-gabcdef1234)): ") == 0);
        CHECK(w.find("use the same game dump (1.09) as the host") != std::string::npos);
        gb.params_sha256 = hb.params_sha256;
        gb.hook_names = {"4p H1 cooperator slots (SessionSlotSummary) @+0x186fe40",
                         "talk IsClientPlayer (guest services) @+0x1234"};
        gb.hooks_hash = hook_set_hash(gb.hook_names);
        w = identity_mismatch(hb, gb);
        CHECK(w.find("party patches differ (host only: travel lamp warp (0x13CDF30) @+0x13cdf30; yours only: talk "
                     "IsClientPlayer (guest services) @+0x1234)") != std::string::npos);
        CHECK(w.find("same bbport build and party options") != std::string::npos);
        std::printf("  A9: %s\n", w.c_str());
        // Order and duplicates do not matter; the empty set hashes to zeros.
        CHECK(hook_set_hash({"b", "a", "a"}) == hook_set_hash({"a", "b"}));
        CHECK(hook_set_hash({"a"}) != hook_set_hash({"a", "b"}));
        CHECK(hook_set_hash({}) == zero);
        // Diagnostics stay out of the hook set.
        for (const char* d : {"ss.info parse log @+0x1", "level-up menu factory (log) @+0x2", "B2 leave trace (0x1ED07A0)",
                              "HUD tick (item-get probe)", "NPC fixture: SOS filter observer (0x1874710)",
                              "action-button registration (test probe)", "Lua event log (LuaEvent_DispatchByName)",
                              "FROM client drop-offline log (0x1ea5ce0)", "item give log (0x17D89F0)"})
            CHECK(diagnostic_hook_name(d));
        for (const char* g : {"4p H1 cooperator slots (SessionSlotSummary)", "travel funnel (SessionWorldTransition)",
                              "main-thread tick (SprjFlipper::Update)", "progress: SetEventFlag",
                              "lamp multiplayer gate +0x49 (party lamps)", "EMEVD instruction dispatch filter",
                              "chalice: carry the slot record into load mode 1"})
            CHECK(!diagnostic_hook_name(g));
    }
}

// Kick by name: BYE Kicked "kicked by the host" to that guest, its slot freed, and the name refused
// for the rest of the session (no auto-rejoin, not even with a fresh link after a restart).
static void test_host_no_answer_hint() {
    CHECK(host_no_answer_hint(-1).find("NAT") != std::string::npos);    // never connected: the NAT advice
    CHECK(host_no_answer_hint(5).find("come back") != std::string::npos);  // the host just left: restarting
    CHECK(host_no_answer_hint(5).find("NAT") == std::string::npos);
    CHECK(host_no_answer_hint(kHostBackWaitS + 1).find("NAT") != std::string::npos);
    // The DHT rendezvous is opt-in: off says how to get through (guest code / the option on both PCs).
    CHECK(host_no_answer_hint(-1).find("automatic punching is off") != std::string::npos);
    CHECK(host_no_answer_hint(-1).find("guest code") != std::string::npos);
    CHECK(host_no_answer_hint(-1, true).find("automatic punching is off") == std::string::npos);
    CHECK(host_no_answer_hint(-1, true).find("DHT option on too") != std::string::npos);
    CHECK(LinkConfig{}.auto_punch == false);
}

static void test_kick() {
    Recorder hr;
    PartyLink host(base_cfg("Host", ""), recorder_callbacks(hr, "host"));
    std::string err;
    CHECK(host.start_host(&err));
    LinkCallbacks none;
    PartyLink a(base_cfg("Alice", ""), none), b(base_cfg("Bob", ""), none);
    CHECK(a.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(b.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(a.wait_connected(5000) && b.wait_connected(5000));
    CHECK(!host.kick_name("Nobody"));
    CHECK(!host.kick_name("Host"));
    CHECK(!a.kick_name("Bob"));  // guests cannot kick
    CHECK(host.kick_name("Alice"));
    CHECK(a.wait_state(LinkState::Rejected, 3000));
    CHECK(a.reject_code() == RejectCode::Kicked && a.reject_reason() == "kicked by the host");
    CHECK(wait_for([&] { return hr.left_released.load() == 1; }, 2000));
    CHECK(wait_for([&] { return host.roster().size() == 2; }, 2000));
    CHECK(host.is_banned("Alice") && !host.is_banned("Bob"));
    CHECK(b.state() == LinkState::Connected);
    // A restarted Alice (new link, no token) is refused.
    PartyLink a2(base_cfg("Alice", ""), none);
    CHECK(a2.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(a2.wait_state(LinkState::Rejected, 5000) && a2.reject_code() == RejectCode::Kicked);
    std::printf("  kicked Alice: %s; restarted Alice: %s\n", a.reject_reason().c_str(), a2.reject_reason().c_str());
    // reconnect_now: only while Reconnecting.
    CHECK(!b.reconnect_now());
    b.debug_drop_connections();
    CHECK(b.wait_state(LinkState::Reconnecting, 2000));
    b.reconnect_now();
    CHECK(b.wait_connected(3000));
    CHECK(!host.reconnect_now());
}

// ---- reliable UDP stream (party_rudp.h): a lossy, reordering channel on a virtual clock ----
static void test_rudp() {
    using namespace party::rudp;
    // Guest codes.
    {
        std::uint32_t a = 0x0a01a8c0;  // 192.168.1.10 (network order)
        std::uint16_t p = 0x5b24;      // 9307 (network order)
        const std::string code = encode_guest_code(a, p);
        std::uint32_t a2 = 0;
        std::uint16_t p2 = 0;
        CHECK(code.rfind("BBG1-", 0) == 0 && code.size() == 19);
        CHECK(decode_guest_code(code, &a2, &p2) && a2 == a && p2 == p);
        std::string lower = code;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        CHECK(decode_guest_code(lower, &a2, &p2) && a2 == a);
        std::string bad = code;
        bad[6] = bad[6] == 'A' ? 'B' : 'A';
        CHECK(!decode_guest_code(bad, &a2, &p2));
        CHECK(!decode_guest_code("BBP1-0000", &a2, &p2));
        std::printf("  guest code %s\n", code.c_str());
    }
    struct Flight {
        double at;
        int to;  // 0 host, 1 guest
        Endpoint from;
        std::vector<std::uint8_t> data;
    };
    // `nat`: each side drops datagrams from an endpoint it has not sent to yet (restricted cone),
    // the host does not punch (first pass) or punches the guest's endpoint (second pass).
    auto run = [](double loss, double lat, double jit, bool nat, bool punch, std::size_t bytes, double* secs,
                  std::uint64_t* resends) -> int {
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> u(0, 1);
        const Endpoint H{0x0100007f, 0x1111}, G{0x0100007f, 0x2222};
        Mux host(3), guest(5);
        host.accepting = true;
        std::set<std::pair<int, std::uint64_t>> sent_to;  // side, endpoint key
        auto key = [](const Endpoint& e) { return (static_cast<std::uint64_t>(e.addr) << 16) | e.port; };
        std::vector<Flight> wire;
        double now_ms = 0;
        const auto t0 = Clock::now();
        auto at = [&](double m) { return t0 + std::chrono::microseconds(static_cast<long long>(m * 1000)); };
        if (punch) host.punch(G, at(0), 30000, 200);
        auto gs = guest.connect(H, at(0), 8000);
        std::shared_ptr<Stream> hs;
        std::vector<std::uint8_t> src(bytes), got_h, got_g;
        for (std::size_t i = 0; i < bytes; ++i) src[i] = static_cast<std::uint8_t>(rng());
        std::size_t wrote_g = 0, wrote_h = 0;
        for (int step = 0; step < 200000; ++step, now_ms += 2) {
            const auto now = at(now_ms);
            std::vector<Datagram> out;
            host.poll(now, out);
            for (auto& d : out) {
                sent_to.insert({0, key(d.to)});
                if (u(rng) >= loss) wire.push_back({now_ms + lat + (u(rng) * 2 - 1) * jit, 1, H, d.data});
            }
            out.clear();
            guest.poll(now, out);
            for (auto& d : out) {
                sent_to.insert({1, key(d.to)});
                if (u(rng) >= loss) wire.push_back({now_ms + lat + (u(rng) * 2 - 1) * jit, 0, G, d.data});
            }
            for (auto it = wire.begin(); it != wire.end();) {
                if (it->at > now_ms) {
                    ++it;
                    continue;
                }
                if (!nat || sent_to.count({it->to, key(it->from)})) {
                    (it->to == 0 ? host : guest).on_datagram(it->from, it->data.data(), it->data.size(), now);
                }
                it = wire.erase(it);
            }
            if (!hs) hs = host.accept();
            if (gs->state() == Stream::State::Failed) return -1;
            if (gs->state() == Stream::State::Open && wrote_g < bytes) {
                const std::size_t k = std::min<std::size_t>(64 * 1024, bytes - wrote_g);
                if (gs->write(src.data() + wrote_g, k)) wrote_g += k;
            }
            if (hs && wrote_h < bytes) {
                const std::size_t k = std::min<std::size_t>(64 * 1024, bytes - wrote_h);
                if (hs->write(src.data() + wrote_h, k)) wrote_h += k;
            }
            std::uint8_t buf[8192];
            if (hs)
                for (std::size_t r; (r = hs->read(buf, sizeof buf)) > 0;) got_h.insert(got_h.end(), buf, buf + r);
            for (std::size_t r; (r = gs->read(buf, sizeof buf)) > 0;) got_g.insert(got_g.end(), buf, buf + r);
            if (got_h.size() == bytes && got_g.size() == bytes) {
                *secs = now_ms / 1000;
                *resends = gs->resends() + (hs ? hs->resends() : 0);
                return (got_h == src && got_g == src) ? 1 : 0;
            }
        }
        return -2;
    };
    double secs = 0;
    std::uint64_t resends = 0;
    CHECK(run(0, 1, 0, false, false, 200000, &secs, &resends) == 1);
    std::printf("  clean: 2 x 200 KB in %.2f s\n", secs);
    CHECK(run(0.10, 60, 20, false, false, 300000, &secs, &resends) == 1);
    std::printf("  10 %% loss, 60 +- 20 ms: 2 x 300 KB intact in %.2f s (%llu resends)\n", secs,
                static_cast<unsigned long long>(resends));
    CHECK(resends > 0);
    // NAT on both sides: without the host's punch the guest's SYNs never get in.
    CHECK(run(0, 30, 5, true, false, 1000, &secs, &resends) == -1);
    // With the punch (the guest code given to the host): connects and transfers.
    CHECK(run(0.02, 30, 5, true, true, 100000, &secs, &resends) == 1);
    std::printf("  simulated NAT both sides: no punch -> no connection; punch -> 2 x 100 KB in %.2f s\n", secs);
    // smokefix: bytes written just before close() leave ahead of the FIN, even when the owner lets
    // the stream go at once (Mux::linger): the peer reads them and then sees the stream closed.
    {
        const Endpoint H{0x0100007f, 0x1111}, G{0x0100007f, 0x2222};
        Mux host(3), guest(5);
        host.accepting = true;
        const auto t = Clock::now();
        auto gs = guest.connect(H, t, 8000);
        std::shared_ptr<Stream> hs;
        auto pump = [&] {
            for (int i = 0; i < 4; ++i) {
                std::vector<Datagram> out;
                host.poll(t, out);
                for (auto& d : out) guest.on_datagram(H, d.data.data(), d.data.size(), t);
                out.clear();
                guest.poll(t, out);
                for (auto& d : out) host.on_datagram(G, d.data.data(), d.data.size(), t);
                if (!hs) hs = host.accept();
            }
        };
        pump();
        CHECK(hs && gs->state() == Stream::State::Open);
        std::vector<std::uint8_t> bye(3000, 0x5a);  // more than one segment
        CHECK(hs && hs->write(bye.data(), bye.size()));
        if (hs) {
            hs->close();
            host.linger(std::move(hs));
        }
        hs = nullptr;
        pump();
        std::vector<std::uint8_t> got(8192);
        const std::size_t r = gs->read(got.data(), got.size());
        CHECK(r == bye.size() && std::equal(bye.begin(), bye.end(), got.begin()));
        CHECK(gs->state() == Stream::State::Closed);
        std::printf("  close after write: %zu bytes, then the FIN\n", r);
    }
}

// The link under the network simulator (BB_NET_SIM, netsim.h; configured process-wide, so this
// runs last): 60 ms one way + 20 jitter + 5 % segment loss (retransmission delays) + a 1.5 s
// total outage mid-stream. Every event arrives once, in order; the link never drops; the ping
// shows the latency.
static void test_netsim_link() {
    bbnet::netsim::Config sc;
    std::string err;
    CHECK(bbnet::netsim::parse("lat=60,jit=20,loss=5,seed=3,outage=1500,at=2500", &sc, &err));
    bbnet::netsim::configure(sc);
    CHECK(bbnet::netsim::enabled());
    Recorder hr, ar;
    // The game's timeouts are 10 s; the tests' 0.7 s would call the 1.5 s outage a lost link.
    LinkConfig hc = base_cfg("Host", "hunter2"), gc = base_cfg("Alice", "hunter2");
    hc.lost_timeout_ms = gc.lost_timeout_ms = 4000;
    hc.connect_timeout_ms = gc.connect_timeout_ms = 4000;
    PartyLink host(hc, recorder_callbacks(hr, "host"));
    CHECK(host.start_host(&err));
    PartyLink a(gc, recorder_callbacks(ar, "alice"));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(a.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(a.wait_connected(8000));
    const auto joined_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  joined in %lld ms under the simulator\n", static_cast<long long>(joined_ms));
    CHECK(joined_ms >= 2 * 60);  // the handshake's round trips pay the latency
    const int kEvents = 300;
    for (int k = 0; k < kEvents; ++k) {
        host.post_event(kBroadcast, "e_" + std::to_string(k), "{}");
        std::this_thread::sleep_for(std::chrono::milliseconds(15));  // ~4.5 s: across the outage
    }
    CHECK(wait_for([&] { return ar.count_event("e_" + std::to_string(kEvents - 1)) >= 1; }, 10000));
    {
        std::lock_guard<std::mutex> lk(ar.mu);
        int next = 0, bad = 0;
        for (const std::string& n : ar.event_names) {
            if (n.rfind("e_", 0) != 0) continue;
            if (n != "e_" + std::to_string(next)) ++bad;
            ++next;
        }
        CHECK(next == kEvents && bad == 0);
        std::printf("  %d events in order through 5 %% loss and a 1.5 s outage (%d out of place)\n", next, bad);
    }
    CHECK(a.state() == LinkState::Connected);
    CHECK(hr.left_kept.load() == 0 && hr.left_released.load() == 0);
    std::uint32_t ping = 0;
    CHECK(wait_for([&] {
        for (const RosterEntry& e : a.roster())
            if (e.slot != kHostSlot && e.ping_ms > 0) ping = e.ping_ms;
        return ping >= 100;
    }, 5000));
    std::printf("  ping %u ms (2 x 60 ms simulated)\n", ping);
    a.stop();
    host.stop();
}

// A host behind a NAT without a port forward (netsim nat=1: unsolicited inbound UDP dropped,
// inbound TCP refused). Without the guest's code the guest cannot get in; with it (the host
// punches the guest's UDP endpoint) the link comes up over UDP and carries events in order.
static void test_udp_nat_link() {
    bbnet::netsim::Config sc;
    std::string err;
    CHECK(bbnet::netsim::parse("lat=0,nat=1", &sc, &err) && sc.nat);
    bbnet::netsim::reconfigure_for_tests(sc);
    Recorder hr, ar, br;
    LinkConfig hc = base_cfg("Host", "hunter2"), gc = base_cfg("Alice", "hunter2"), bc = base_cfg("Bob_2", "hunter2");
    for (LinkConfig* c : {&hc, &gc, &bc}) {
        c->lost_timeout_ms = 15000;  // a loaded machine stalls threads for seconds
        c->connect_timeout_ms = 15000;
        c->tcp_fallback_ms = 500;
    }
    PartyLink host(hc, recorder_callbacks(hr, "host"));
    CHECK(host.start_host(&err));
    const std::uint32_t lo = htonl(INADDR_LOOPBACK);
    // Bob has no code with the host: refused TCP, then UDP SYNs the host's NAT drops.
    PartyLink b(bc, recorder_callbacks(br, "bob"));
    CHECK(b.start_guest("127.0.0.1", host.bound_port(), &err));
    // Alice gave the host her guest code (her UDP endpoint): the host punches it.
    PartyLink a(gc, recorder_callbacks(ar, "alice"));
    CHECK(a.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(a.udp_local_port() != 0);
    host.punch_guest(lo, htons(a.udp_local_port()));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(a.wait_connected(20000));  // generous: other test binaries / builds may load the machine
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(a.connected_over_udp());
    std::printf("  alice (punched) connected over UDP in %lld ms\n", static_cast<long long>(ms));
    for (int k = 0; k < 200; ++k) host.post_event(kBroadcast, "u_" + std::to_string(k), "{}");
    a.post_event(0, "from_alice", "{}");
    CHECK(wait_for([&] { return ar.count_event("u_199") == 1 && hr.has_event("from_alice"); }, 20000));
    {
        std::lock_guard<std::mutex> lk(ar.mu);
        int next = 0, bad = 0;
        for (const std::string& n : ar.event_names) {
            if (n.rfind("u_", 0) != 0) continue;
            if (n != "u_" + std::to_string(next)) ++bad;
            ++next;
        }
        CHECK(next == 200 && bad == 0);
    }
    std::uint32_t ping = 0;
    CHECK(wait_for([&] {
        for (const RosterEntry& e : a.roster())
            if (e.slot != kHostSlot && e.ping_ms > 0) ping = e.ping_ms;
        return ping > 0;
    }, 15000));
    CHECK(!b.wait_connected(1500) && b.state() != LinkState::Connected);
    std::printf("  200 events in order over UDP, ping %u ms; bob (no code) not connected: %s\n", ping,
                link_state_name(b.state()));
    b.stop();
    // smokefix: the host's Leave (graceful stop) reaches a guest linked over UDP as the BYE ("the
    // host ended the party"), not as a silent link 10 s later (the BYE used to be dropped at close).
    host.stop();
    CHECK(a.wait_state(LinkState::Rejected, 5000) && a.reject_code() == RejectCode::Shutdown);
    std::printf("  host Leave over UDP: alice %s (%s)\n", link_state_name(a.state()), a.reject_reason().c_str());
    a.stop();
}

// F5: long outages and a stalled host, scaled 1:10 (lost timeout 1 s for the game's 10 s, slot
// keep 6 s for 60 s). A 3 s total outage ("30 s"): both sides call the link lost, the host keeps
// the slot, the guest reconnects on its own and resumes; every event the host posted during the
// outage arrives once, in order. A 12 s outage ("120 s"): past the slot keep the host releases the
// member; the guest still comes back by itself, as a new member (not resumed: the host's director
// then sends the full flag snapshot, party_progress_bridge.cpp). A host whose event callback is
// blocked for 4 lost timeouts (the game paused / alt-tabbed / a long load on the host) keeps its
// members: the link's own thread answers pings, the callback backlog only stops reading.
static void test_long_outages() {
    bbnet::netsim::Config sc;
    std::string err;
    CHECK(bbnet::netsim::parse("lat=5", &sc, &err));
    bbnet::netsim::reconfigure_for_tests(sc);
    Recorder hr, ar;
    LinkConfig hc = base_cfg("Host", "hunter2"), gc = base_cfg("Alice", "hunter2");
    for (LinkConfig* c : {&hc, &gc}) {
        c->ping_interval_ms = 150;
        c->lost_timeout_ms = 1000;
        c->slot_keep_ms = 6000;
        c->backoff_initial_ms = 100;
        c->backoff_max_ms = 400;
        c->connect_timeout_ms = 1500;
    }
    std::atomic<int> stall_ms{0};
    LinkCallbacks hcb = recorder_callbacks(hr, "host");
    auto rec = hcb.on_event;
    hcb.on_event = [&, rec](int slot, std::uint64_t cur, const std::string& name, const std::string& body) {
        if (const int ms = stall_ms.exchange(0)) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        rec(slot, cur, name, body);
    };
    PartyLink host(hc, hcb);
    CHECK(host.start_host(&err));
    PartyLink a(gc, recorder_callbacks(ar, "alice"));
    CHECK(a.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(a.wait_connected(5000));
    auto in_order = [&](const std::string& prefix, int n) {
        std::lock_guard<std::mutex> lk(ar.mu);
        int next = 0, bad = 0;
        for (const std::string& e : ar.event_names) {
            if (e.rfind(prefix, 0) != 0) continue;
            if (e != prefix + std::to_string(next)) ++bad;
            ++next;
        }
        return next == n && bad == 0;
    };

    // "30 s": lost, kept, back, resumed, nothing lost or doubled.
    for (int k = 0; k < 20; ++k) host.post_event(kBroadcast, "a_" + std::to_string(k), "{}");
    CHECK(wait_for([&] { return ar.count_event("a_19") == 1; }, 5000));
    const auto t0 = std::chrono::steady_clock::now();
    bbnet::netsim::outage_now(3000);
    for (int k = 20; k < 60; ++k) {
        host.post_event(kBroadcast, "a_" + std::to_string(k), "{}");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 2 s inside the outage
    }
    CHECK(wait_for([&] { return hr.left_kept.load() >= 1; }, 5000));
    CHECK(wait_for([&] { return hr.rejoined.load() >= 1 && a.state() == LinkState::Connected; }, 10000));
    const auto back_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(a.last_welcome_resumed());
    CHECK(wait_for([&] { return ar.count_event("a_59") == 1; }, 5000));
    CHECK(in_order("a_", 60));
    CHECK(hr.left_released.load() == 0);
    std::printf("  3 s outage (\"30 s\"): lost %d, back and resumed %lld ms after it began; 60 events once, in order\n",
                hr.left_kept.load(), static_cast<long long>(back_ms));

    // "120 s": released, then back as a new member by itself.
    const int joined_before = hr.joined.load();
    bbnet::netsim::outage_now(12000);
    for (int k = 0; k < 10; ++k) host.post_event(kBroadcast, "lost_" + std::to_string(k), "{}");
    CHECK(wait_for([&] { return hr.left_released.load() >= 1; }, 12000));  // after the 6 s keep
    CHECK(wait_for([&] { return hr.joined.load() > joined_before && a.state() == LinkState::Connected; }, 15000));
    CHECK(!a.last_welcome_resumed());  // a fresh session: the director re-sends the snapshot
    for (int k = 0; k < 20; ++k) host.post_event(kBroadcast, "b_" + std::to_string(k), "{}");
    CHECK(wait_for([&] { return ar.count_event("b_19") == 1; }, 5000));
    CHECK(in_order("b_", 20));
    std::printf("  12 s outage (\"120 s\"): released after the keep, rejoined as new (%d of 10 events posted into "
                "the released slot arrived; the snapshot covers them)\n",
                [&] {
                    int n = 0;
                    for (int k = 0; k < 10; ++k) n += ar.count_event("lost_" + std::to_string(k));
                    return n;
                }());

    // A stalled host: its event callback blocks 4 s (4 lost timeouts).
    const int left_before = hr.left_kept.load() + hr.left_released.load();
    stall_ms = 4000;
    a.post_event(kHostSlot, "wake", "{}");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for (int k = 0; k < 30; ++k) a.post_event(kHostSlot, "s_" + std::to_string(k), "{}");
    std::this_thread::sleep_for(std::chrono::milliseconds(4500));
    CHECK(a.state() == LinkState::Connected);
    CHECK(hr.left_kept.load() + hr.left_released.load() == left_before);
    CHECK(wait_for([&] { return hr.count_event("s_29") == 1; }, 5000));
    {
        std::lock_guard<std::mutex> lk(hr.mu);
        int next = 0, bad = 0;
        for (const std::string& e : hr.event_names) {
            if (e.rfind("s_", 0) != 0) continue;
            if (e != "s_" + std::to_string(next)) ++bad;
            ++next;
        }
        CHECK(next == 30 && bad == 0);
    }
    std::printf("  host callback stalled 4 s: member kept, 30 events in order afterwards\n");
    a.stop();
    host.stop();
}

// LAN discovery (party_lan.h): probe / reply encode and verify, the reply limiter, a loopback
// discover() against a responder, and the guest's password rule for codes without one.
static void test_lan() {
    const std::array<std::uint8_t, 8> secret = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::array<std::uint8_t, 8> other = {1, 2, 3, 4, 5, 6, 7, 9};
    const crypto::Key key = crypto::derive_party_key("", secret.data());
    const crypto::Key wrong = crypto::derive_party_key("", other.data());
    const crypto::Key with_pw = crypto::derive_party_key("pw", secret.data());
    lan::Nonce nonce = {9, 8, 7, 6, 5, 4, 3, 2};

    const auto probe = lan::encode_probe(key, nonce);
    CHECK(probe.size() == lan::kProbeSize && lan::is_lan(probe.data(), probe.size()));
    lan::Nonce got{};
    CHECK(lan::verify_probe(key, probe.data(), probe.size(), &got) && got == nonce);
    CHECK(!lan::verify_probe(wrong, probe.data(), probe.size(), nullptr));    // another party
    CHECK(!lan::verify_probe(with_pw, probe.data(), probe.size(), nullptr));  // a password changes the key
    // The secret is not in the probe.
    CHECK(std::search(probe.begin(), probe.end(), secret.begin(), secret.end()) == probe.end());
    for (std::size_t i = 0; i < probe.size(); ++i) {  // any flipped byte fails
        auto bad = probe;
        bad[i] ^= 0x01;
        CHECK(!lan::verify_probe(key, bad.data(), bad.size(), nullptr));
    }
    CHECK(!lan::verify_probe(key, probe.data(), probe.size() - 1, nullptr));
    // The rudp magic is not LAN discovery.
    const std::uint8_t rudp_hdr[16] = {0xfa, 'L'};
    CHECK(!lan::is_lan(rudp_hdr, sizeof rudp_hdr));

    const auto reply = lan::encode_reply(key, nonce, {192, 168, 1, 112}, 9307);
    CHECK(reply.size() == lan::kReplySize && reply.size() <= probe.size() + 8);  // no amplification to speak of
    std::array<std::uint8_t, 4> ip{};
    std::uint16_t port = 0;
    CHECK(lan::parse_reply(key, nonce, reply.data(), reply.size(), &ip, &port));
    CHECK(ip == (std::array<std::uint8_t, 4>{192, 168, 1, 112}) && port == 9307);
    lan::Nonce stale = nonce;
    stale[0] ^= 1;
    CHECK(!lan::parse_reply(key, stale, reply.data(), reply.size(), &ip, &port));  // not our probe
    CHECK(!lan::parse_reply(wrong, nonce, reply.data(), reply.size(), &ip, &port));
    auto forged = reply;
    forged[17] ^= 1;  // the port
    CHECK(!lan::parse_reply(key, nonce, forged.data(), forged.size(), &ip, &port));
    CHECK(!lan::parse_reply(key, nonce, probe.data(), probe.size(), &ip, &port));  // a probe is no reply

    // Host: answers only its own party's probes, at most kRepliesPerSecond a second.
    const auto t0 = lan::Clock::now();
    lan::ReplyLimiter lim;
    std::vector<crypto::Key> keys{wrong, key};  // the second key matches (password-only key case)
    CHECK(lan::answer_probe({wrong}, probe.data(), probe.size(), ip, 9307, &lim, t0).empty());
    int answered = 0;
    for (int i = 0; i < 20; ++i)
        answered += !lan::answer_probe(keys, probe.data(), probe.size(), {10, 0, 0, 2}, 9400, &lim, t0).empty();
    CHECK(answered == lan::kRepliesPerSecond);
    const auto ans = lan::answer_probe(keys, probe.data(), probe.size(), {10, 0, 0, 2}, 9400, &lim,
                                       t0 + std::chrono::milliseconds(1000));
    CHECK(ans.size() == lan::kReplySize);
    CHECK(lan::parse_reply(key, nonce, ans.data(), ans.size(), &ip, &port) && port == 9400);

    CHECK(!lan::broadcast_targets().empty() && lan::broadcast_targets()[0] == 0xffffffffu);

    // discover() over loopback against a responder thread on a 127.0.0.1 UDP socket.
    sock::startup();
    sock::Socket rs = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in ra{};
    ra.sin_family = AF_INET;
    ra.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(rs, reinterpret_cast<sockaddr*>(&ra), sizeof ra) == 0);
    sock::socklen rl = sizeof ra;
    getsockname(rs, reinterpret_cast<sockaddr*>(&ra), &rl);
    const std::uint16_t rport = ntohs(ra.sin_port);
    std::atomic<bool> stop{false};
    std::atomic<int> probes{0};
    std::thread responder([&] {
        lan::ReplyLimiter rlim;
        while (!stop) {
            sock::PollFd p{};
            p.fd = rs;
            p.events = POLLIN;
            if (sock::poll(&p, 1, 20) <= 0) continue;
            std::uint8_t buf[256];
            sockaddr_in from{};
            sock::socklen fl = sizeof from;
            const long n = ::recvfrom(rs, reinterpret_cast<char*>(buf), sizeof buf, 0,
                                      reinterpret_cast<sockaddr*>(&from), &fl);
            if (n <= 0) continue;
            ++probes;
            const auto r = lan::answer_probe({key}, buf, static_cast<std::size_t>(n), {127, 0, 0, 1}, 9555, &rlim,
                                             lan::Clock::now());
            if (!r.empty())
                ::sendto(rs, reinterpret_cast<const char*>(r.data()), static_cast<int>(r.size()), 0,
                         reinterpret_cast<sockaddr*>(&from), fl);
        }
    });
    lan::Found f;
    std::string note;
    const auto d0 = lan::Clock::now();
    CHECK(lan::discover(key, {0x0100007fu}, rport, 2000, true, &f, &note));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(lan::Clock::now() - d0).count();
    std::printf("  discover: %s:%u in %lld ms (%s)\n", "127.0.0.1", f.port, static_cast<long long>(took), note.c_str());
    CHECK(f.addr_nbo == 0x0100007fu && f.port == 9555 && took < 1000);
    // Another party's guest: no answer, the budget is kept.
    const auto w0 = lan::Clock::now();
    CHECK(!lan::discover(wrong, {0x0100007fu}, rport, 600, true, &f, &note));
    const auto wtook = std::chrono::duration_cast<std::chrono::milliseconds>(lan::Clock::now() - w0).count();
    CHECK(wtook >= 550 && wtook < 1500);
    stop = true;
    responder.join();
    sock::close(rs);
    CHECK(probes.load() >= 2);  // three rounds for the unanswered one (some may still be queued)

    // tcp_reachable: a listening loopback port answers, a closed one does not.
    sock::Socket ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in la{};
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(ls, reinterpret_cast<sockaddr*>(&la), sizeof la) == 0 && ::listen(ls, 4) == 0);
    sock::socklen ll = sizeof la;
    getsockname(ls, reinterpret_cast<sockaddr*>(&la), &ll);
    CHECK(lan::tcp_reachable(0x0100007fu, ntohs(la.sin_port), 1000));
    const std::uint16_t closed = ntohs(la.sin_port);
    sock::close(ls);
    CHECK(!lan::tcp_reachable(0x0100007fu, closed, 1000));

    // The guest's password rule: a BBP1 code without kCodeFlagPasswordRequired ignores a saved one.
    PartyCode pc;
    CHECK(decode_party_code(make_party_code({203, 0, 113, 9}, 9307, secret, 0), &pc, nullptr));
    CHECK(code_ignores_password(pc));
    CHECK(decode_party_code(make_party_code({203, 0, 113, 9}, 9307, secret, kCodeFlagPasswordRequired), &pc, nullptr));
    CHECK(!code_ignores_password(pc));
    CHECK(decode_party_code("192.168.1.5:9307", &pc, nullptr));
    CHECK(!code_ignores_password(pc));  // plain host:port: the password alone keys the party
    // The Auth reject reasons tell the two cases apart.
    CHECK(auth_reject_reason(true).find("wrong password") == 0);
    CHECK(auth_reject_reason(false).find("another hosting session") != std::string::npos);
    CHECK(auth_reject_reason(false).find("no password") != std::string::npos);
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
    std::printf("reliable udp stream\n");
    test_rudp();
    std::printf("party codes\n");
    test_codes();
    std::printf("crypto\n");
    test_crypto();
    std::printf("LAN discovery\n");
    test_lan();
    std::printf("stun / env\n");
    test_stun_local();
    std::printf("version check\n");
    test_identity();
    std::printf("loopback party\n");
    test_party();
    std::printf("max players 2, secret-only key\n");
    test_max_players_two();
    test_plain_address_password();
    std::printf("kick by name\n");
    test_kick();
    std::printf("host gone: wait for it, not the NAT advice\n");
    test_host_no_answer_hint();
    std::printf("four players (host + 3 guests), game rules\n");
    test_four_players();
    std::printf("party HUD vitals\n");
    test_vitals();
    std::printf("host restart: slots restored\n");
    test_host_restart();
    std::printf("network simulator on the link (last: process-wide)\n");
    test_netsim_link();
    std::printf("host behind a simulated NAT: the link over UDP with hole punching\n");
    test_udp_nat_link();
    std::printf("long outages (30 s / 120 s, scaled 1:10) and a stalled host\n");
    test_long_outages();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    std::printf(g_failures ? "FAILED\n" : "OK\n");
    return g_failures ? 1 : 0;
}
