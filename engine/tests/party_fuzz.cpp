// SPDX-License-Identifier: GPL-3.0-or-later
// In-process fuzzing of everything that parses a party peer's bytes (bbport security pass):
//
//   json       json::parse / dump round trip (depth, numbers, escapes, size)
//   code       party codes, identity files
//   stun       STUN Binding request / response parsers
//   udp        vport header, relay server (STUN HELLO, relay frames), relay client
//   travel     TravelFromJsonText + SanitizePeerTravel + ChooseReplay + GuestTravel
//   story      StoryFromJsonText + SanitizePeerStory + GuestStory
//   items      ItemsFromJsonText + FilterPeerItems + GuestItems
//   phantom    PhantomEventFromJsonText
//   peerev     the other peer events: chalice_goods (GoodsFromJsonText), drop_seed (DropSeedFromJson),
//              the B2 direct summon bodies (dsummon DecodeRoom / DecodeDone / WaitOf), gift_up
//              (GiftUpFromJsonText), sync_digest / sync_resync (party_desync.h) and map_ping
//              (party_ping.h FromJson + the relay rate + the marker board)
//   balance    balance::FromJson (the host's party rules, guest side): values within the menu's ranges
//   lamp       LampRequestFromJsonText + SanitizePeerLampRequest (a guest's lamp request, host side),
//              LampLitFromJsonText, worldact::RequestFromJson (EVENT world_action: levers, doors, NPC talk)
//   progress   ChangesFromJson / SnapshotFromJson / GuestApplier queue / WorldImageFromJson +
//              GuestWorldMirror / BbpfDecode
//   hostsvc    PartyHostService::handle and FromApi::handle_json (guest RPCs, the sign board)
//   linkhost   a real PartyLink host over loopback: garbage and pre-auth frames, then
//              authenticated encrypted frames of every type (RPC_REQ into the host service)
//   linkguest  a real PartyLink guest against a fake (malicious) host: mutated CHALLENGE /
//              WELCOME / ROSTER / EVENT / RPC_RESP / BYE frames; events go through the
//              runtime's decoders + sanitizers
//
// Inputs: random bytes and structure-aware mutations of a seed corpus (the unit tests' shapes,
// built with the encoders). Checks: no crash, no exception escaping a non-throwing API, no hang
// (a watchdog), allocations capped (operator new is counted), plus each parser's own
// postconditions (a sanitized travel only names plausible ids, filtered items are allowed, ...).
// A failing input is written to fuzz-fail-<target>.bin before the process stops.
//
// Also the regression tests for the bugs this pass fixed (always run first).
//
//   party-fuzz.exe [--seconds S] [--iters N] [--seed X] [target...]      (default: all, 60 s)
//   party-security-test.exe                                                (regressions + a short run)
// Built with -fsanitize=undefined -fsanitize-undefined-trap-on-error -D_GLIBCXX_ASSERTIONS as
// party-fuzz-ubsan (MinGW GCC has no ASan): UB and container bounds become a trap.
#include "bbnet_internal.h"
#include "from_api.h"
#include "json.h"
#include "net_stun.h"
#include "party_host_service.h"
#include "party_udp.h"
#include "party_util.h"
#include "party_sign_blob.h"
#include "party/party_balance_rules.h"
#include "party/party_chalice.h"
#include "party/party_droproll.h"
#include "party/party_ping.h"
#include "party/party_dsummon.h"
#include "party/party_desync.h"
#include "party/party_code.h"
#include "party/party_ids.h"
#include "party/party_crypto.h"
#include "party/party_guestsvc.h"
#include "party/party_items.h"
#include "party/party_link.h"
#include "party/party_phantom.h"
#include "party/party_progress.h"
#include "party/party_sock.h"
#include "party/party_story.h"
#include "party/party_travel.h"
#include "party/party_worldact.h"
#include "party/party_npctalk.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

// --- Runtime stand-ins (src/probe.c, runtime_thread.c, runtime.c) ---
extern "C" {
BBNET_ABI void restore_guest_fs(void) {}
void runtime_thread_attach_host(const char*) {}
const char* runtime_symbol(const char*) { return nullptr; }
}

namespace {

// ---------------------------------------------------------------------------------------------
// allocation accounting

std::atomic<std::int64_t> g_live{0};
std::atomic<std::int64_t> g_peak{0};
std::atomic<std::size_t> g_biggest{0};
constexpr std::size_t kMaxSingleAlloc = 160u << 20;  // a 16 MiB frame and its copies fit
constexpr std::int64_t kMaxLive = 1536ll << 20;

[[noreturn]] void die(const char* why);

void* counted_alloc(std::size_t n) {
    if (n > kMaxSingleAlloc) {
        std::fprintf(stderr, "allocation of %zu bytes\n", n);
        die("allocation over the cap");
    }
    void* p = std::malloc(n + 16);
    if (!p) throw std::bad_alloc();
    std::memcpy(p, &n, sizeof n);
    const std::int64_t live = g_live.fetch_add(static_cast<std::int64_t>(n)) + static_cast<std::int64_t>(n);
    if (live > g_peak.load()) g_peak.store(live);
    if (n > g_biggest.load()) g_biggest.store(n);
    if (live > kMaxLive) die("live memory over the cap");
    return static_cast<char*>(p) + 16;
}
void counted_free(void* p) {
    if (!p) return;
    char* b = static_cast<char*>(p) - 16;
    std::size_t n;
    std::memcpy(&n, b, sizeof n);
    g_live.fetch_sub(static_cast<std::int64_t>(n));
    std::free(b);
}

}  // namespace

void* operator new(std::size_t n) { return counted_alloc(n); }
void* operator new[](std::size_t n) { return counted_alloc(n); }
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, std::size_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::size_t) noexcept { counted_free(p); }

namespace {

using u8 = std::uint8_t;
using Bytes = std::vector<u8>;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------------------------
// failure reporting, watchdog

std::mutex g_cur_mu;
std::string g_cur_target = "startup";
Bytes g_cur_input;
std::atomic<std::int64_t> g_beat{0};
std::atomic<bool> g_watch{true};

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void save_input(const char* why) {
    std::string name = "fuzz-fail-" + g_cur_target + ".bin";
    if (FILE* f = std::fopen(name.c_str(), "wb")) {
        std::fwrite(g_cur_input.data(), 1, g_cur_input.size(), f);
        std::fclose(f);
    }
    std::fprintf(stderr, "FUZZ FAILURE in %s: %s (input %zu bytes saved to %s)\n", g_cur_target.c_str(), why,
                 g_cur_input.size(), name.c_str());
}

[[noreturn]] void die(const char* why) {
    save_input(why);
    std::fflush(stderr);
    std::_Exit(3);
}

#if defined(_WIN32)
LONG WINAPI on_crash(EXCEPTION_POINTERS* ep) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "crash: exception 0x%08lx", ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0ul);
    save_input(buf);
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void set_input(const std::string& target, const void* d, std::size_t n) {
    std::lock_guard<std::mutex> lk(g_cur_mu);
    if (g_cur_target != target) g_cur_target = target;
    g_cur_input.assign(static_cast<const u8*>(d), static_cast<const u8*>(d) + n);
    g_beat.store(now_ms());
}
void set_input(const std::string& target, const std::string& s) { set_input(target, s.data(), s.size()); }

void watchdog(int limit_ms) {
    while (g_watch.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const std::int64_t b = g_beat.load();
        if (b && now_ms() - b > limit_ms) {
            std::lock_guard<std::mutex> lk(g_cur_mu);
            die("hang: one input took longer than the watchdog limit");
        }
    }
}

int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_fail;                                                            \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)
// A postcondition broken by a fuzz input: fatal, with the input saved.
#define INVARIANT(cond)                                    \
    do {                                                   \
        if (!(cond)) die("invariant broken: " #cond);      \
    } while (0)

// ---------------------------------------------------------------------------------------------
// mutation

struct Rng {
    std::mt19937_64 g;
    explicit Rng(std::uint64_t seed) : g(seed) {}
    std::uint64_t next() { return g(); }
    std::size_t below(std::size_t n) { return n ? static_cast<std::size_t>(g() % n) : 0; }
    bool chance(int one_in) { return below(static_cast<std::size_t>(one_in)) == 0; }
};

const char* const kTokens[] = {"{",      "}",     "[",          "]",        "\"",       ",",    ":",     "\\u0000",
                               "\\ud800", "\\udc00", "\\u00e9", "1e999",    "-1e999",   "1e-999", "-0",   "NaN",
                               "null",   "true",  "false",      "\\",       "0x7fffffff", "4294967296", "18446744073709551616",
                               "9007199254740993", "-1", "0.5", "\x00",     "\xff",     "\n",   "[[[[[[[[[[[[[[[[",
                               "{\"a\":", "\"\\u", "1.7976931348623157e308"};

const double kNumbers[] = {0, -1, 1, 0.5, -0.5, 255, 256, 65535, 65536, 2147483647.0, 2147483648.0, 4294967295.0,
                           4294967296.0, 9007199254740992.0, 1e18, 9.3e18, 1e300, -1e300, -2147483648.0, 1e-300,
                           2102961, 2412952, 24000030, 12401803, 70002802, 50002560, 43710, 0xffffffff, 99, 100, 1000};

void mutate_bytes(Rng& r, Bytes& b, const std::vector<Bytes>& corpus) {
    const int rounds = 1 + static_cast<int>(r.below(6));
    for (int k = 0; k < rounds; ++k) {
        switch (r.below(10)) {
        case 0:  // flip a bit
            if (!b.empty()) b[r.below(b.size())] ^= static_cast<u8>(1u << r.below(8));
            break;
        case 1:  // random byte
            if (!b.empty()) b[r.below(b.size())] = static_cast<u8>(r.next());
            break;
        case 2: {  // insert random bytes
            const std::size_t n = 1 + r.below(16);
            Bytes ins(n);
            for (auto& x : ins) x = static_cast<u8>(r.next());
            b.insert(b.begin() + static_cast<std::ptrdiff_t>(r.below(b.size() + 1)), ins.begin(), ins.end());
            break;
        }
        case 3:  // delete a range
            if (!b.empty()) {
                const std::size_t at = r.below(b.size());
                const std::size_t n = std::min(b.size() - at, 1 + r.below(32));
                b.erase(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n));
            }
            break;
        case 4: {  // a token
            const char* t = kTokens[r.below(std::size(kTokens))];
            const std::size_t n = std::strlen(t) ? std::strlen(t) : 1;
            b.insert(b.begin() + static_cast<std::ptrdiff_t>(r.below(b.size() + 1)), t, t + n);
            break;
        }
        case 5:  // duplicate a chunk (sometimes many times)
            if (!b.empty() && b.size() < (1u << 20)) {
                const std::size_t at = r.below(b.size());
                const std::size_t n = std::min(b.size() - at, 1 + r.below(64));
                Bytes chunk(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n));
                const std::size_t times = r.chance(8) ? 1 + r.below(2000) : 1;
                for (std::size_t i = 0; i < times; ++i) b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), chunk.begin(), chunk.end());
            }
            break;
        case 6:  // splice another seed
            if (!corpus.empty()) {
                const Bytes& o = corpus[r.below(corpus.size())];
                if (!o.empty()) {
                    const std::size_t from = r.below(o.size());
                    const std::size_t n = std::min(o.size() - from, 1 + r.below(64));
                    b.insert(b.begin() + static_cast<std::ptrdiff_t>(r.below(b.size() + 1)), o.begin() + static_cast<std::ptrdiff_t>(from),
                             o.begin() + static_cast<std::ptrdiff_t>(from + n));
                }
            }
            break;
        case 7:  // truncate
            if (!b.empty()) b.resize(r.below(b.size()));
            break;
        case 8: {  // interesting integer bytes
            if (b.size() >= 4) {
                const std::uint32_t vals[] = {0, 1, 0x7f, 0x80, 0xff, 0xffff, 0x7fffffff, 0x80000000u, 0xffffffffu,
                                              16u << 20, (16u << 20) + 1, 64u << 10};
                const std::uint32_t v = vals[r.below(std::size(vals))];
                const std::size_t at = r.below(b.size() - 3);
                std::memcpy(b.data() + at, &v, 4);
            }
            break;
        }
        default:  // swap two bytes
            if (b.size() >= 2) std::swap(b[r.below(b.size())], b[r.below(b.size())]);
            break;
        }
    }
    if (b.size() > (4u << 20)) b.resize(4u << 20);
}

// Structure-aware: a random node of a parsed seed changed in type or value, a member added /
// dropped, an array grown, a level of nesting added.
void mutate_value(Rng& r, json::Value& v, int depth) {
    using T = json::Value::Type;
    const bool container = v.type == T::Array || v.type == T::Object;
    if (container && depth < 12 && !r.chance(4)) {
        if (v.type == T::Array && !v.array.empty()) {
            if (r.chance(6) && v.array.size() < 100000) {  // grow (the copy's size keeps it bounded)
                const json::Value copy = v.array[r.below(v.array.size())];
                const std::size_t unit = json::dump(copy, 0).size() + 1;
                std::size_t times = r.chance(4) ? r.below(5000) : r.below(4);
                if (unit * times > (2u << 20)) times = (2u << 20) / unit;
                for (std::size_t i = 0; i < times; ++i) v.array.push_back(copy);
                return;
            }
            if (r.chance(8)) {
                v.array.erase(v.array.begin() + static_cast<std::ptrdiff_t>(r.below(v.array.size())));
                return;
            }
            mutate_value(r, v.array[r.below(v.array.size())], depth + 1);
            return;
        }
        if (v.type == T::Object && !v.object.empty()) {
            if (r.chance(8)) {
                v.object.erase(v.object.begin() + static_cast<std::ptrdiff_t>(r.below(v.object.size())));
                return;
            }
            if (r.chance(10)) {
                v.object.emplace_back("x" + std::to_string(r.below(1000)), json::Value(static_cast<double>(r.below(100))));
                return;
            }
            mutate_value(r, v.object[r.below(v.object.size())].second, depth + 1);
            return;
        }
    }
    switch (r.below(9)) {
    case 0: v = json::Value(kNumbers[r.below(std::size(kNumbers))]); break;
    case 1: v = json::Value(static_cast<double>(static_cast<std::int64_t>(r.next() >> r.below(64)))); break;
    case 2: v = json::Value(std::string(r.chance(2) ? "0x" : "") + std::to_string(r.next() >> r.below(64))); break;
    case 3: v = json::Value(); break;
    case 4: v = json::Value(r.chance(2)); break;
    case 5: {
        json::Value a = json::Value::make_array();
        a.push(v);
        v = a;
        break;
    }
    case 6: v = json::Value::make_object(); break;
    case 7: {
        const char* names[] = {"cutscene", "ending", "time_of_day", "lamp", "transform", "award", "flag", "full",
                               "mark", "test", "insight", "rested", "bogus", "", "AA==", "lamp_unlocked"};
        v = json::Value(names[r.below(std::size(names))]);
        break;
    }
    default: v = json::Value(std::string(r.below(300), 'A')); break;
    }
}

struct Corpus {
    std::vector<Bytes> seeds;
    void add(const std::string& s) { seeds.emplace_back(s.begin(), s.end()); }
    void add(const Bytes& b) { seeds.push_back(b); }
};

// One input: a seed, mutated as bytes or (when it parses) as JSON, or plain random bytes.
Bytes next_input(Rng& r, const Corpus& c, bool json_aware) {
    if (c.seeds.empty() || r.chance(16)) {
        Bytes b(r.below(256));
        for (auto& x : b) x = static_cast<u8>(r.next());
        return b;
    }
    Bytes b = c.seeds[r.below(c.seeds.size())];
    if (json_aware && !r.chance(3)) {
        json::Value v;
        std::string err;
        if (json::parse(std::string(b.begin(), b.end()), v, err)) {
            const int n = 1 + static_cast<int>(r.below(4));
            for (int i = 0; i < n; ++i) mutate_value(r, v, 0);
            const std::string s = json::dump(v, 0);
            return Bytes(s.begin(), s.end());
        }
    }
    if (!r.chance(8)) mutate_bytes(r, b, c.seeds);
    return b;
}

std::string text_of(const Bytes& b) { return std::string(b.begin(), b.end()); }

// ---------------------------------------------------------------------------------------------
// seeds

std::string travel_seed(coop::TravelKind k, bool pos) {
    coop::TravelIntent t;
    t.seq = 7;
    t.kind = k;
    t.call_site = 0x1381a93;
    t.lamp_id = 2412952;
    t.packed_map = 0x18000000;
    t.warp_point = 2402200;
    t.respawn_mode = 1;
    t.respawn_record = 2412952;
    t.last_lamp = 2102961;
    if (pos) {
        t.has_pos = true;
        t.pos_map = 0x18000000;
        t.pos[0] = 10.5f;
        t.pos[1] = -3.0f;
        t.pos[2] = 100.0f;
        t.rot[1] = 1.5f;
    }
    return coop::TravelToJsonText(t);
}

Corpus json_corpus() {
    Corpus c;
    for (const char* s : {"{}", "[]", "null", "0", "-1.5e10", "\"a\\u00e9\\ud83d\\ude00\"", "{\"a\":[1,2,{\"b\":null}]}",
                          "[1,[2,[3,[4]]]]", "{\"ResKind\":0,\"Error\":\"x\"}", "\"\\b\\f\\n\\r\\t\\/\\\\\""})
        c.add(std::string(s));
    using coop::TravelKind;
    for (TravelKind k : {TravelKind::Lamp, TravelKind::ScriptedWarp, TravelKind::LuaBonfireWarp, TravelKind::HostDeath,
                         TravelKind::Transform, TravelKind::GuestDied})
        c.add(travel_seed(k, k == TravelKind::Transform));
    coop::StoryIntent s;
    s.seq = 3;
    s.kind = coop::StoryKind::Cutscene;
    s.id = 24000030;
    s.event_id = 12401803;
    s.map = 0x18000000;
    s.tod = 2;
    s.flags = {70002802, 9423};
    c.add(coop::StoryToJsonText(s));
    s.kind = coop::StoryKind::Ending;
    s.id = 2;
    s.tod = -1;
    s.flags.clear();
    c.add(coop::StoryToJsonText(s));
    std::vector<coop::ItemGrant> items(3);
    items[0] = {1700000000123ull, 2400450, 52400450, coop::kItemNone, coop::ItemSource::Flag};
    items[1] = {1700000000124ull, 31000, coop::kItemNone, 25, coop::ItemSource::Award};
    items[2] = {7, 80000000, 5000, coop::kItemNone, coop::ItemSource::Full};
    c.add(coop::ItemsToJsonText(items));
    coop::PhantomEvent pe;
    pe.kind = coop::PhantomEventKind::Insight;
    pe.seq = 4;
    pe.insight = 3;
    pe.source_event = 12401800;
    c.add(coop::PhantomEventToJsonText(pe));
    pe.kind = coop::PhantomEventKind::Lamp;
    pe.lamp = 2102961;
    c.add(coop::PhantomEventToJsonText(pe));
    coop::LampRequest lr;
    lr.id = 2102950;
    lr.seq = 3;
    c.add(coop::LampRequestToJsonText(lr));
    c.add(coop::LampLitToJsonText({12417830, 12417831, 72410201}));
    {
        coop::worldact::Request wa;
        wa.map = 0x18010000;
        wa.set = {12410200, 12410202, 12100110};
        wa.clr = {12410203};
        c.add(coop::worldact::RequestToJson(wa));
        wa.kick = {2410000, 2410001};
        c.add(coop::worldact::RequestToJson(wa));
    }
    {
        coop::npctalk::TalkEvent te;  // EVENT npc_talk (shared NPC dialogue)
        te.npc = 2410782;
        te.talk = te.msg = 1900002;
        te.voice = 1900002;
        te.map = 0x18010000;
        te.has_pos = true;
        te.pos[0] = -12.5f;
        te.who = "Hunter1";
        te.slot = 1;
        c.add(coop::npctalk::ToJsonText(te));
        te.kind = coop::npctalk::Kind::Gift;
        te.lot = 10050;
        te.item_cat = 4;
        te.item_id = 4312;
        c.add(coop::npctalk::ToJsonText(te));
    }
    std::vector<coop::progress::FlagChange> ch(2);
    ch[0] = {12401800, true, coop::progress::CategoryOf(12401800), 5};
    ch[1] = {70002802, false, coop::progress::CategoryOf(70002802), 6};
    c.add(coop::progress::ChangesToJson(77, ch));
    coop::progress::FlagSnapshot snap;
    snap.epoch = 77;
    snap.seq = 6;
    snap.map_id = 0x18000000;
    snap.blocks[12401] = {800, 801};
    snap.blocks[70002] = {802};
    c.add(coop::progress::SnapshotToJson(snap));
    coop::progress::WorldImage img;  // host-world mirror groups (EVENT world_groups)
    img.epoch = 77;
    img.full = true;
    img.groups[coop::progress::PoolKeyOfId(72410201)][25] = 0x40;
    img.groups[coop::progress::PoolKeyOfId(2200)][25] = 0x80;
    c.add(coop::progress::WorldImageToJson(img));
    // Host service / FromApi requests.
    for (const char* s :
         {R"({"OnlineId":"fz0","SignalingAddr":"10.0.0.2","SignalingPort":3658,"MappedAddr":"203.0.113.9","MappedPort":3658,"RelayPort":50002})",
          R"({"OnlineId":"fz0","MaxMembers":4,"HostArea":402718720,"HostLevel":10,"HostPos":[1.5,2,3],"MemberTag":1,"LocalAddr":"10.0.0.2","LocalPort":3658})",
          R"({"RoomId":1001,"LocalAddr":"10.0.0.3","LocalPort":3658})", R"({"SessionId":"abc","MemberId":2})",
          R"({"SessionId":"abc","MemberId":2,"KickerMemberId":1,"OptData":"AAAA"})", R"({"OnlineId":"fz1"})",
          R"({"MappedAddr":"203.0.113.10","MappedPort":4000,"RelayPort":50003})",
          R"({"MessageId":"SummonDataGetListRequest","AreaId":402718720,"GetMaxCount":20,"SummonTypeList":[{"SummonType":0,"GetLimitCount":5}]})",
          R"({"MessageId":"SummonDataSummonRequest","TargetUserId":1002,"SummonDataId":1})"})
        c.add(std::string(s));
    // The other peer events (target peerev).
    c.add(coop::chalice::GoodsToJsonText({6100, 6109, 6539}));
    {
        party::ping::PingEvent pe;  // EVENT map_ping (I2)
        pe.id = 7;
        pe.map = 0x18010000;
        pe.pos[0] = 12.5f, pe.pos[1] = -3.25f, pe.pos[2] = 100.0f;
        pe.hit = true;
        pe.t = 123456;
        c.add(party::ping::ToJson(pe));
        pe.slot = 2;
        pe.who = "Hunter1";
        pe.clear = true;
        c.add(party::ping::ToJson(pe));
    }
    {
        coop::droproll::DropSeed ds;
        ds.salt = 0x1234567890abcdefull;
        ds.gen = 3;
        ds.discovery = 120;
        c.add(coop::droproll::DropSeedToJson(ds));
    }
    {
        coop::dsummon::Room room;
        room.data.assign(40, 0x5a);
        room.area = 0x18000000;
        room.pos[0] = 1.5f;
        room.level = 30;
        c.add(coop::dsummon::EncodeRoom(room));
        c.add(coop::dsummon::EncodeRefuse(coop::dsummon::Busy::Boss));
        c.add(coop::dsummon::EncodeDone(true));
        c.add(coop::dsummon::EncodeBusy(coop::dsummon::Busy::Cutscene));
        c.add(coop::dsummon::EncodeReq());
    }
    c.add(coop::GiftUpToJsonText(2000, 50000060));
    {
        coop::desync::Digest d;
        d.epoch = 77;
        d.seq = 9;
        d.has_world = d.has_flags = true;
        d.world[3] = 0xdeadbeef;
        c.add(coop::desync::DigestToJson(d));
        coop::desync::Resync rs;
        rs.epoch = 77;
        rs.world = {1, 30};
        rs.flags = {4};
        rs.gap = true;
        c.add(coop::desync::ResyncToJson(rs));
    }
    const std::string blob = bbnet::party::b64_encode(party_test::sign_blob("fz0"));
    c.add(R"({"MessageId":"SummonDataCreateRequest","CharaId":1,"AreaId":402718720,"AreaRegionId":1,"SummonType":0,"SummonData":")" +
          blob + R"(","SummonDataVersion":3})");
    return c;
}

// ---------------------------------------------------------------------------------------------
// targets (each: one input -> no crash, no exception, postconditions)

using Target = std::function<void(Rng&, const Corpus&)>;

void run_json(Rng& r, const Corpus& c) {
    const Bytes in = next_input(r, c, true);
    const std::string s = text_of(in);
    set_input("json", s);
    json::Value v;
    std::string err;
    if (json::parse(s, v, err)) {
        const std::string d = json::dump(v, 0);
        json::Value v2;
        std::string err2;
        INVARIANT(json::parse(d, v2, err2));
        INVARIANT(json::dump(v2, 0) == d);
        (void)json::dump(v, 2);
        // The lenient readers on every member.
        if (v.type == json::Value::Type::Object)
            for (const auto& kv : v.object) {
                (void)bbnet::party::int_of(v, kv.first.c_str(), 0);
                (void)bbnet::party::str_of(v, kv.first.c_str());
            }
    } else {
        INVARIANT(!err.empty());
    }
}

void run_code(Rng& r, const Corpus& c) {
    const Bytes in = next_input(r, c, false);
    const std::string s = text_of(in);
    set_input("code", s);
    party::PartyCode pc;
    std::string err;
    if (party::decode_party_code(s, &pc, &err) && !pc.plain) {
        party::PartyCode again;
        INVARIANT(party::decode_party_code(party::encode_party_code(pc), &again, &err));
        INVARIANT(again.ipv4 == pc.ipv4 && again.port == pc.port && again.secret == pc.secret && pc.port != 0);
    }
    std::array<u8, 32> h{};
    std::vector<std::string> names;
    (void)party::parse_identity_file(s, "patch", &h, &names);
    std::array<u8, 4> ip{};
    if (party::parse_ipv4(s, &ip)) INVARIANT(party::format_ipv4(ip).size() <= 15);
}

void run_stun(Rng& r, const Corpus& c) {
    Bytes in = next_input(r, c, false);
    set_input("stun", in.data(), in.size());
    u8 txid[net::stun::kTxid] = {};
    u8 token[net::stun::kTokenLen] = {};
    bool hello = false;
    (void)net::stun::parse_binding_request(in.data(), in.size(), txid, &hello, token);
    // A response to the transaction id the input carries (the parser checks it).
    u8 want[net::stun::kTxid] = {};
    if (in.size() >= 20) std::memcpy(want, in.data() + 4, 16);
    std::uint32_t a = 0;
    std::uint16_t p = 0;
    net::stun::Relay relay;
    if (net::stun::parse_binding_response(in.data(), in.size(), want, &a, &p, &relay)) INVARIANT(a != 0 && p != 0);
}

void run_udp(Rng& r, const Corpus& c) {
    static bbnet::udp::RelayServer server(9307);
    static bbnet::udp::RelayClient client;
    Bytes in = next_input(r, c, false);
    set_input("udp", in.data(), in.size());
    std::uint16_t src = 0, dst = 0;
    const std::size_t h = bbnet::udp::p2p_header(in.data(), in.size(), &src, &dst);
    INVARIANT(h <= in.size());
    const std::uint32_t addr = 0x0100007f + (static_cast<std::uint32_t>(r.below(64)) << 24);
    const std::uint16_t port = static_cast<std::uint16_t>(r.next());
    u8 out[net::stun::kMaxResponse];
    const std::size_t n = server.answer_stun(in.data(), in.size(), addr, port, true, out);
    INVARIANT(n <= sizeof out);
    Bytes buf = in;
    buf.resize(std::max<std::size_t>(buf.size(), 1));
    u8* frame = nullptr;
    std::size_t len = 0;
    std::uint32_t to_addr = 0;
    std::uint16_t to_port = 0;
    if (server.forward(buf.data(), in.size(), addr, port, &frame, &len, &to_addr, &to_port)) {
        INVARIANT(frame >= buf.data() && frame + len <= buf.data() + in.size());
        INVARIANT(len + 8 == in.size());
    }
    INVARIANT(server.clients() <= bbnet::udp::RelayServer::kMaxClients);
    if (r.chance(64)) server.expire(Clock::now() + std::chrono::hours(1));
    std::uint16_t from = 0;
    (void)client.unwrap(in.data(), in.size(), addr, port, &from);
    if (n) {
        std::uint32_t ma = 0;
        std::uint16_t mp = 0;
        net::stun::Relay rel;
        INVARIANT(net::stun::parse_binding_response(out, n, in.data() + 4, &ma, &mp, &rel) || mp == 0);
        client.on_answer(addr, port, rel);
    }
}

void run_balance(Rng& r, const Corpus& c) {
    const std::string s = text_of(next_input(r, c, true));
    set_input("balance", s);
    coop::balance::Rules rules;
    std::string err;
    if (!coop::balance::FromJson(s, &rules, &err)) return;
    INVARIANT(rules == coop::balance::Clamped(rules));
    for (int n = 0; n <= 4; ++n) {
        const coop::balance::Doping b = coop::balance::Boss(rules, n), e = coop::balance::Enemy(rules, n);
        INVARIANT(b.hp_rate >= 1.0f && b.hp_rate <= 7.0f && e.hp_rate >= 1.0f && e.hp_rate <= 7.0f);
        INVARIANT(b.attack_rate >= 1.0f && b.attack_rate <= 4.0f && std::abs(b.super_armor) <= 270);
    }
}

void run_travel(Rng& r, const Corpus& c) {
    static coop::GuestTravel guest;
    const std::string s = text_of(next_input(r, c, true));
    set_input("travel", s);
    coop::TravelIntent t;
    std::string err;
    if (!coop::TravelFromJsonText(s, &t, &err)) return;
    (void)coop::DescribeTravel(t);
    if (!coop::SanitizePeerTravel(&t, &err)) {
        INVARIANT(!coop::TravelKindBroadcast(t.kind));
        return;
    }
    INVARIANT(coop::TravelKindBroadcast(t.kind));
    INVARIANT(t.lamp_id == coop::kTravelNone || coop::TravelLampKnown(t.lamp_id));
    INVARIANT(t.packed_map == coop::kTravelNone || coop::TravelMapKnown(t.packed_map));
    if (t.has_pos) INVARIANT(coop::TravelMapKnown(t.pos_map));
    if (t.has_pos)
        for (int i = 0; i < 4; ++i) INVARIANT(std::isfinite(t.pos[i]) && std::isfinite(t.rot[i]));
    const coop::ReplayPlan plan = coop::ChooseReplay(t);
    if (plan.method == coop::ReplayMethod::LampWarp) INVARIANT(coop::TravelLampKnown(plan.id));
    if (plan.method == coop::ReplayMethod::BonfireWarp) INVARIANT(coop::TravelBonfireKnown(plan.id));
    if (plan.method == coop::ReplayMethod::StageWarp) {
        INVARIANT(coop::TravelMapKnown(t.packed_map));
        INVARIANT(t.warp_point == coop::kTravelNone || coop::ids::WarpPointKnown(t.packed_map, t.warp_point));
    }
    if (plan.method == coop::ReplayMethod::Transform) INVARIANT(t.has_pos);
    const std::uint32_t lamp = coop::HostLampFromTravel(t);
    INVARIANT(lamp == coop::kTravelNone || coop::TravelLampKnown(lamp));
    static double clock = 0;
    clock += 1;
    guest.Offer(t, clock);
    coop::ReplayState st;
    st.world_up = true;
    st.session_role = 0;
    for (int i = 0; i < 3; ++i) (void)guest.Step(st, clock + i * 0.1);
}

void run_story(Rng& r, const Corpus& c) {
    static coop::GuestStory guest;
    const std::string s = text_of(next_input(r, c, true));
    set_input("story", s);
    coop::StoryIntent st;
    std::string err;
    if (!coop::StoryFromJsonText(s, &st, &err)) return;
    (void)coop::DescribeStory(st);
    if (!coop::SanitizePeerStory(&st, &err)) {
        INVARIANT(st.kind == coop::StoryKind::Cutscene && !coop::StoryRemoKnown(st.id));
        return;
    }
    const std::vector<std::uint32_t> table = coop::StoryFlagsForRemo(st.id);
    for (std::uint32_t f : st.flags) INVARIANT(std::find(table.begin(), table.end(), f) != table.end());
    if (st.kind == coop::StoryKind::Cutscene) INVARIANT(coop::StoryRemoKnown(st.id));
    static double clock = 0;
    clock += 1;
    guest.Offer(st, r.chance(2), clock);
    INVARIANT(guest.QueueSize() <= coop::GuestStory::kMaxQueue);
}

void run_items(Rng& r, const Corpus& c) {
    static coop::GuestItems guest;
    const std::string s = text_of(next_input(r, c, true));
    set_input("items", s);
    std::vector<coop::ItemGrant> items;
    std::string err;
    if (!coop::ItemsFromJsonText(s, &items, &err)) return;
    INVARIANT(items.size() <= coop::kMaxItemsPerEvent);
    std::size_t rejected = 0;
    const std::vector<coop::ItemGrant> ok = coop::FilterPeerItems(items, &rejected);
    INVARIANT(ok.size() + rejected == items.size());
    for (const coop::ItemGrant& g : ok) {
        INVARIANT(coop::ItemGrantFromPeerAllowed(g));
        INVARIANT(g.source != coop::ItemSource::Mark && !coop::ItemLotDenied(g.lot, g.flag));
        (void)coop::DescribeItem(g);
    }
    guest.Offer(ok);
    INVARIANT(guest.Pending() <= coop::GuestItems::kMaxQueue);
}

void run_phantom(Rng& r, const Corpus& c) {
    const std::string s = text_of(next_input(r, c, true));
    set_input("phantom", s);
    coop::PhantomEvent e;
    std::string err;
    if (!coop::PhantomEventFromJsonText(s, &e, &err)) return;
    if (e.kind == coop::PhantomEventKind::Insight) INVARIANT(e.insight >= 0 && e.insight <= 99);
    if (coop::SanitizePeerPhantom(e, &err) && e.kind == coop::PhantomEventKind::Lamp)
        INVARIANT(e.lamp == coop::kTravelNone || coop::TravelLampKnown(e.lamp));
}

// The peer events not covered above: each decoder's own validation is the postcondition.
void run_peerev(Rng& r, const Corpus& c) {
    const std::string s = text_of(next_input(r, c, true));
    set_input("peerev", s);
    std::string err;
    std::vector<std::uint32_t> goods;
    if (coop::chalice::GoodsFromJsonText(s, &goods, &err)) {
        INVARIANT(goods.size() <= 64);
        for (std::uint32_t g : goods) INVARIANT(coop::chalice::IsChaliceGoods(g));
    }
    party::ping::PingEvent pe;  // EVENT map_ping (I2): both sides
    if (party::ping::FromJson(s, &pe, &err)) {
        for (float f : pe.pos) INVARIANT(std::isfinite(f) && std::fabs(f) <= 1e5f);
        INVARIANT(pe.slot >= -1 && pe.slot < party::ping::kMaxSlots && pe.who.size() <= 16);
        INVARIANT(pe.clear || (pe.map != 0 && pe.map != 0xffffffffu));
        party::ping::PingEvent back;
        std::string again;
        INVARIANT(party::ping::FromJson(party::ping::ToJson(pe), &back, &again) && back.map == pe.map &&
                  back.slot == pe.slot && back.who == pe.who && back.clear == pe.clear && back.t == pe.t);
        static party::ping::RelayRate rate;
        if (pe.slot >= 0 && rate.Take(pe.slot, double(pe.t) / 1000.0)) {
            party::ping::Marker m;
            m.slot = pe.slot;
            m.map = pe.map;
            m.until_ms = 1000 + party::ping::LifeLeft(pe.t, 5000);
            party::ping::Place(m);
            party::ping::Marker out[party::ping::kMaxSlots];
            INVARIANT(party::ping::Read(out, 1000) <= party::ping::kMaxSlots);
        }
    }
    coop::droproll::DropSeed ds;
    if (coop::droproll::DropSeedFromJson(s, &ds, &err)) {
        INVARIANT(ds.salt != 0);
        INVARIANT(ds.discovery == 0.0f || (ds.discovery >= 1.0f && ds.discovery <= 1000.0f));
    }
    coop::dsummon::Room room;
    bool refused = false;
    if (coop::dsummon::DecodeRoom(s, &room, &refused, &err)) {
        INVARIANT(!refused && room.data.size() > 8 && room.data.size() <= coop::dsummon::kMaxRoomData);
        for (float p : room.pos) INVARIANT(std::isfinite(p) && std::fabs(p) < 1e5f);
        INVARIANT(room.level <= 544);
        const u8 id[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        const std::vector<u8> pay = coop::dsummon::BuildPayload(id, room);
        INVARIANT(pay.size() == coop::dsummon::kHeadSize + room.data.size());
    }
    bool ok = false;
    (void)coop::dsummon::DecodeDone(s, &ok);
    (void)coop::dsummon::WaitOf(s);
    std::int32_t lot = 0, flag = 0;
    if (coop::GiftUpFromJsonText(s, &lot, &flag, &err)) INVARIANT(coop::GuestGiftShareable(lot, flag));
    coop::desync::Digest dg;  // F5: sync_digest (guest side) / sync_resync (host side)
    if (coop::desync::DigestFromJson(s, &dg, &err)) {
        coop::desync::Digest back;
        INVARIANT(coop::desync::DigestFromJson(coop::desync::DigestToJson(dg), &back, &err));
        INVARIANT(back.epoch == dg.epoch && back.seq == dg.seq && back.has_world == dg.has_world);
    }
    coop::desync::Resync rs;
    if (coop::desync::ResyncFromJson(s, &rs, &err)) {
        INVARIANT(rs.world.size() <= coop::desync::kBuckets && rs.flags.size() <= coop::desync::kBuckets);
        for (int b : rs.world) INVARIANT(b >= 0 && b < coop::desync::kBuckets);
        for (int b : rs.flags) INVARIANT(b >= 0 && b < coop::desync::kBuckets);
        const coop::progress::WorldImage part = coop::desync::WorldBuckets(coop::progress::WorldImage{}, rs.world);
        INVARIANT(part.groups.size() <= coop::progress::kPoolBlocks);
    }
}

void run_lamp(Rng& r, const Corpus& c) {
    const std::string s = text_of(next_input(r, c, true));
    set_input("lamp", s);
    coop::LampRequest q;
    std::string err;
    std::vector<std::uint32_t> lit;
    if (coop::LampLitFromJsonText(s, &lit, &err)) {  // EVENT lamp_lit (a guest lit a lamp)
        INVARIANT(lit.size() <= coop::kMaxLampLitIds);
        for (std::uint32_t id : lit) INVARIANT(coop::LampLitFlagAllowed(id));
    }
    coop::npctalk::TalkEvent te;  // EVENT npc_talk (both sides)
    if (coop::npctalk::FromJsonText(s, &te, &err)) {
        INVARIANT(te.who.size() <= 16 && te.item_cat <= 15 && te.voice <= 999999999u);
        for (float f : te.pos) INVARIANT(std::isfinite(f) && std::fabs(f) <= 1e6f);
        coop::npctalk::TalkEvent back;
        std::string again;
        INVARIANT(coop::npctalk::FromJsonText(coop::npctalk::ToJsonText(te), &back, &again) && back.kind == te.kind &&
                  back.npc == te.npc && back.talk == te.talk && back.msg == te.msg && back.who == te.who);
    }
    coop::worldact::Request wa;  // EVENT world_action (a guest's lever / door / NPC talk, host side)
    if (coop::worldact::RequestFromJson(s, &wa, &err)) {
        INVARIANT(wa.set.size() <= coop::worldact::kMaxSet && wa.clr.size() <= coop::worldact::kMaxClear);
        INVARIANT(!wa.set.empty() || !wa.clr.empty() || !wa.kick.empty());
        INVARIANT(wa.kick.size() <= coop::worldact::kMaxKicks);
        for (std::int32_t k : wa.kick) INVARIANT(k > 0 && k <= 99999999);
        for (std::uint32_t id : wa.set) {
            const std::uint32_t area = id >= 10000000u ? id / 100000u % 100u : 21u;
            INVARIANT(coop::worldact::WorldActionAllowed(id, false, area << 24));
            INVARIANT(!coop::progress::NeverWritable(id));
            const auto cat = coop::progress::CategoryOf(id);
            INVARIANT(cat != coop::progress::Category::boss_defeated &&
                      (cat != coop::progress::Category::item_lot_picked || coop::worldact::NpcGiftTrigger(id)) &&
                      cat != coop::progress::Category::session_runtime && cat != coop::progress::Category::never_sync);
        }
        for (std::uint32_t id : wa.clr) {
            const std::uint32_t area = id >= 10000000u ? id / 100000u % 100u : 21u;
            INVARIANT(coop::worldact::WorldActionAllowed(id, true, area << 24));
        }
        std::string again;
        coop::worldact::Request back;
        INVARIANT(coop::worldact::RequestFromJson(coop::worldact::RequestToJson(wa), &back, &again) && back == wa);
    }
    if (!coop::LampRequestFromJsonText(s, &q, &err)) return;
    if (coop::SanitizePeerLampRequest(q, &err)) INVARIANT(coop::TravelLampKnown(q.id));
}

void run_progress(Rng& r, const Corpus& c) {
    static coop::progress::GuestApplier applier;
    static const coop::progress::Policy policy = coop::progress::Policy::Defaults();
    const Bytes in = next_input(r, c, true);
    const std::string s = text_of(in);
    set_input("progress", s);
    std::uint64_t epoch = 0;
    std::vector<coop::progress::FlagChange> ch;
    std::string err;
    if (coop::progress::ChangesFromJson(s, &epoch, &ch, &err)) {
        INVARIANT(ch.size() <= coop::progress::kMaxChangesPerEvent);
        for (const auto& x : ch) {
            const coop::progress::SyncMode m = coop::progress::ModeFor(policy, x.id);
            if (m != coop::progress::SyncMode::Off) INVARIANT(!coop::progress::NeverWritable(x.id));
        }
        applier.QueueChanges(epoch, ch);
    }
    coop::progress::FlagSnapshot snap;
    if (coop::progress::SnapshotFromJson(s, &snap, &err)) {
        INVARIANT(snap.blocks.size() <= coop::progress::kMaxSnapshotBlocks);
        for (const auto& [b, bits] : snap.blocks)
            for (std::uint32_t x : bits) INVARIANT(x < coop::progress::kBitsPerBlock);
        applier.QueueSnapshot(snap);
    }
    INVARIANT(applier.Pending() <= coop::progress::kMaxPendingItems);
    static coop::progress::GuestWorldMirror mirror;
    coop::progress::WorldImage img;
    if (coop::progress::WorldImageFromJson(s, &img, &err)) {
        INVARIANT(img.groups.size() <= coop::progress::kPoolBlocks);
        for (const auto& [key, g] : img.groups) INVARIANT(coop::progress::WorldMirrorKey(int(key)));
        mirror.Receive(img);
        INVARIANT(mirror.Dirty() <= coop::progress::WorldMirrorKeys().size());
    }
    coop::progress::BbpfHeader h;
    std::vector<coop::progress::BbpfBlock> blocks;
    (void)coop::progress::BbpfDecode(in, &h, &blocks);
}

void run_hostsvc(Rng& r, const Corpus& c) {
    static bbnet::party::PartyHostService* svc = [] {
        auto* s = new bbnet::party::PartyHostService;
        s->set_heartbeat_timeout_ms(1000000);
        return s;
    }();
    static bbnet::party::FromApi* api = new bbnet::party::FromApi(*svc);
    static const char* const kinds[] = {"context_start", "create_room", "join_room", "leave_room", "heartbeat",
                                        "kick_member", "signaling_resolve", "signaling_update", "http", "bogus"};
    static const char* const paths[] = {"/summon_messenger/create", "/summon_messenger/get", "/summon_messenger/delete",
                                        "/summon_messenger/request", "/basic_utils/login", "/ss.info", "/x"};
    const std::string s = text_of(next_input(r, c, true));
    set_input("hostsvc", s);
    json::Value rq, reply;
    std::string err;
    if (!json::parse(s, rq, err)) return;
    bbnet::party::Caller who;
    who.online_id = "fz" + std::to_string(r.below(4));
    who.link_addr = r.chance(2) ? 0x0100007f : 0x0a00000a;
    const char* kind = kinds[r.below(std::size(kinds))];
    if (std::string(kind) == "http") {
        json::Value h = json::Value::make_object();
        h.set("Method", "POST");
        h.set("Url", std::string("http://bbparty.invalid:18671/api") + paths[r.below(std::size(paths))] +
                         (r.chance(4) ? "?user_id=1002" : ""));
        h.set("Body", bbnet::party::b64_encode(s));
        api->handle_json(who, h, reply);
        INVARIANT(reply.type == json::Value::Type::Object && reply.find("Status"));
        std::map<std::string, int> per_user;
        for (const auto& sign : api->signs()) {
            std::vector<u8> data;
            INVARIANT(bbnet::party::b64_decode_strict(sign.data_b64, &data) && data.size() == bbnet::party::kSummonDataSize);
            INVARIANT(++per_user[sign.online_id] <= static_cast<int>(bbnet::party::kMaxSignsPerUser));
            INVARIANT(!bbnet::party::summon_data_problem(data.data(), data.size(), sign.online_id));
        }
    } else {
        svc->handle(who, kind, rq, reply);
        INVARIANT(reply.type == json::Value::Type::Object && reply.find("ResKind"));
    }
    bbnet::party::PartyHostService::RoomView v;
    if (svc->room_of(who.online_id, &v)) INVARIANT(v.members.size() <= 8);
    std::vector<json::Value> evs = svc->wait_events(who.online_id, 0, 0, 64);
    if (!evs.empty() && r.chance(2))
        svc->ack_events(who.online_id, static_cast<std::uint64_t>(bbnet::party::int_of(evs.back(), "EventId", 0)));
    (void)svc->status_line();
}

// ---------------------------------------------------------------------------------------------
// PartyLink over loopback

using party::sock::Socket;

Socket tcp_connect(std::uint16_t port) {
    Socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        party::sock::close(s);
        return party::sock::kInvalid;
    }
    DWORD to = 1500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
    return s;
}

bool send_all(Socket s, const u8* d, std::size_t n) {
    while (n) {
        const long r = party::sock::send_some(s, d, n);
        if (r <= 0) return false;
        d += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}
bool recv_all(Socket s, u8* d, std::size_t n) {
    while (n) {
        const long r = party::sock::recv_some(s, d, n);
        if (r <= 0) return false;
        d += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

struct W {
    Bytes b;
    W& u8_(u8 v) {
        b.push_back(v);
        return *this;
    }
    W& u16(std::uint16_t v) { return u8_(static_cast<u8>(v)).u8_(static_cast<u8>(v >> 8)); }
    W& u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) u8_(static_cast<u8>(v >> (8 * i)));
        return *this;
    }
    W& u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) u8_(static_cast<u8>(v >> (8 * i)));
        return *this;
    }
    W& raw(const void* p, std::size_t n) {
        b.insert(b.end(), static_cast<const u8*>(p), static_cast<const u8*>(p) + n);
        return *this;
    }
    W& str16(const std::string& s) { return u16(static_cast<std::uint16_t>(s.size())).raw(s.data(), s.size()); }
    W& str32(const std::string& s) { return u32(static_cast<std::uint32_t>(s.size())).raw(s.data(), s.size()); }
};

Bytes plain_frame(u8 type, const Bytes& body) {
    W w;
    w.u32(static_cast<std::uint32_t>(1 + body.size())).u8_(type).raw(body.data(), body.size());
    return w.b;
}
Bytes sealed_frame(party::crypto::Aead& tx, u8 type, const Bytes& body) {
    const std::uint32_t len = static_cast<std::uint32_t>(1 + party::crypto::kMacSize + 1 + body.size());
    Bytes out(5 + party::crypto::kMacSize + 1 + body.size());
    u8 hdr[5] = {static_cast<u8>(len), static_cast<u8>(len >> 8), static_cast<u8>(len >> 16), static_cast<u8>(len >> 24), 0xE0};
    std::memcpy(out.data(), hdr, 5);
    Bytes pt;
    pt.push_back(type);
    pt.insert(pt.end(), body.begin(), body.end());
    tx.seal(out.data() + 5 + party::crypto::kMacSize, out.data() + 5, hdr, 5, pt.data(), pt.size());
    return out;
}
// One frame: type + body (decrypted with rx when it is sealed).
bool read_frame(Socket s, party::crypto::Aead* rx, u8* type, Bytes* body) {
    u8 h[5];
    if (!recv_all(s, h, 5)) return false;
    const std::uint32_t len = h[0] | h[1] << 8 | h[2] << 16 | static_cast<std::uint32_t>(h[3]) << 24;
    if (len == 0 || len > (16u << 20)) return false;
    Bytes rest(len - 1);
    if (!recv_all(s, rest.data(), rest.size())) return false;
    if (h[4] != 0xE0) {
        *type = h[4];
        *body = rest;
        return true;
    }
    if (!rx || rest.size() < party::crypto::kMacSize + 1) return false;
    Bytes pt(rest.size() - party::crypto::kMacSize);
    if (!rx->open(pt.data(), rest.data(), h, 5, rest.data() + party::crypto::kMacSize, pt.size())) return false;
    *type = pt[0];
    body->assign(pt.begin() + 1, pt.end());
    return true;
}

enum : u8 { kHello = 1, kChallenge = 2, kAuth = 3, kWelcome = 4, kReject = 5, kPing = 10, kPong = 11, kRoster = 12,
            kRpcReq = 13, kRpcResp = 14, kEvent = 15, kEventAck = 16, kPartyCmd = 17, kProgress = 18, kBye = 19,
            kVitals = 20 };

// A VITALS body: up to 6 entries (the link reads at most kMaxVitals), random fields, sometimes
// absurd floats (NaN / inf / huge) and out-of-range slots.
void vitals_body(Rng& r, W& w) {
    const int n = static_cast<int>(r.below(7));
    w.u8_(static_cast<u8>(r.chance(8) ? r.below(256) : n));
    static const std::uint32_t floats[] = {0x00000000u, 0x7fc00000u, 0x7f800000u, 0xff800000u, 0x4f000000u,
                                           0x42c80000u, 0xc2c80000u, 0x7f7fffffu, 0x00000001u};
    for (int i = 0; i < n; ++i) {
        w.u8_(static_cast<u8>(r.chance(3) ? r.below(256) : r.below(4))).u8_(static_cast<u8>(r.below(256)));
        w.u32(static_cast<std::uint32_t>(r.next())).u32(static_cast<std::uint32_t>(r.next()));
        w.u16(static_cast<std::uint16_t>(r.next())).u16(static_cast<std::uint16_t>(r.next()));
        w.u32(static_cast<std::uint32_t>(r.next()));
        for (int k = 0; k < 4; ++k)
            w.u32(r.chance(2) ? floats[r.below(std::size(floats))] : static_cast<std::uint32_t>(r.next()));
    }
}

// What a link publishes after any VITALS: bounded and sane.
void check_vitals(const party::PartyLink& link) {
    party::MemberVitals v[party::kMaxVitals + 2];
    const int n = link.vitals(v, party::kMaxVitals + 2);
    INVARIANT(n >= 0 && n <= party::kMaxVitals);
    for (int i = 0; i < n; ++i) {
        INVARIANT(v[i].slot >= 0 && v[i].slot < party::kMaxVitals);
        INVARIANT(v[i].hp <= v[i].max_hp && v[i].max_hp <= party::kMaxVitalsHp);
        INVARIANT(v[i].stamina <= v[i].max_stamina && v[i].max_stamina <= party::kMaxVitalsStamina);
        INVARIANT((v[i].flags & ~party::kVitalsKnownFlags) == 0);
        if (v[i].flags & party::kVitalsHasPos)
            for (float p : v[i].pos) INVARIANT(std::isfinite(p) && std::fabs(p) <= party::kMaxVitalsCoord);
        INVARIANT(std::isfinite(v[i].yaw));
    }
}

Bytes hello_body(const std::string& name, std::uint16_t version = party::kLinkProtocolVersion) {
    W w;
    w.raw("BBPL", 4).u16(version).str16(name);
    u8 zero[32] = {};
    w.raw(zero, 32).raw(zero, 32).raw(zero, 16).raw(zero, 32).str16("").str16("").str16("");
    return w.b;
}

const std::string kPassword = "fuzz-password";

party::LinkConfig fuzz_cfg(const std::string& name) {
    party::LinkConfig cfg;
    cfg.name = name;
    cfg.password = kPassword;
    cfg.port = 0;
    cfg.bind_addr = "127.0.0.1";
    cfg.max_players = 4;
    cfg.ping_interval_ms = 200;
    cfg.lost_timeout_ms = 2000;
    cfg.slot_keep_ms = 300;
    cfg.backoff_initial_ms = 5;
    cfg.backoff_max_ms = 20;
    cfg.backoff_jitter_pct = 0;
    cfg.connect_timeout_ms = 1500;
    cfg.roster_refresh_ms = 200;
    cfg.auth_throttle = false;  // random handshakes share 127.0.0.1 with the real members
    return cfg;
}

const party::crypto::Key& party_key() {
    static const party::crypto::Key k = [] {
        const std::array<u8, 8> secret{};
        return party::crypto::derive_party_key(kPassword, secret.data());
    }();
    return k;
}

// A client that completes the handshake as `name`; false when the host refused or the network
// failed (fuzzing may have left the slot taken).
struct AuthedClient {
    Socket s = party::sock::kInvalid;
    party::crypto::Aead tx, rx;
    ~AuthedClient() {
        if (s != party::sock::kInvalid) party::sock::close(s);
    }
    bool open(std::uint16_t port, const std::string& name) {
        s = tcp_connect(port);
        if (s == party::sock::kInvalid) return false;
        const Bytes hello = plain_frame(kHello, hello_body(name));
        if (!send_all(s, hello.data(), hello.size())) return false;
        u8 type = 0;
        Bytes body;
        if (!read_frame(s, nullptr, &type, &body) || type != kChallenge || body.size() != 24) return false;
        party::crypto::Nonce hn{}, gn{};
        std::memcpy(hn.data(), body.data(), 24);
        party::crypto::random_bytes(gn.data(), gn.size());
        const party::crypto::Key proof = party::crypto::auth_proof(party_key(), hn, gn);
        W a;
        a.raw(gn.data(), 24).raw(proof.data(), 32);
        const Bytes auth = plain_frame(kAuth, a.b);
        if (!send_all(s, auth.data(), auth.size())) return false;
        rx.init(party::crypto::session_key(party_key(), "bbp-h2g", hn, gn));
        tx.init(party::crypto::session_key(party_key(), "bbp-g2h", hn, gn));
        if (!read_frame(s, &rx, &type, &body) || type != kWelcome) return false;
        return true;
    }
};

struct LinkHostRig {
    std::unique_ptr<bbnet::party::PartyHostService> svc;
    std::unique_ptr<bbnet::party::FromApi> api;
    std::unique_ptr<party::PartyLink> link;  // last: stopped first (its callbacks use the others)
    std::uint16_t port = 0;
    std::atomic<std::uint64_t> rpcs{0};
    LinkHostRig() {
        svc = std::make_unique<bbnet::party::PartyHostService>();
        svc->set_heartbeat_timeout_ms(1000000);
        api = std::make_unique<bbnet::party::FromApi>(*svc);
        party::LinkCallbacks cb;
        // What party_runtime's on_rpc does: the authenticated name, the JSON parse, the service.
        cb.on_rpc = [this](int slot, const std::string& kind, const std::string& body) -> std::string {
            ++rpcs;
            bbnet::party::Caller who;
            for (const party::RosterEntry& e : link->roster())
                if (e.slot == slot) who.online_id = e.name;
            who.link_addr = link->member_ip(slot);
            json::Value rq, reply;
            std::string err;
            if (who.online_id.empty() || !json::parse(body, rq, err)) return "{\"ResKind\":1}";
            if (kind == "http") api->handle_json(who, rq, reply);
            else svc->handle(who, kind, rq, reply);
            return json::dump(reply, 0);
        };
        cb.on_event = [](int, std::uint64_t, const std::string&, const std::string&) {};
        cb.on_party_cmd = [](int, const std::string&, const std::string&) {};
        cb.on_progress = [](int, const std::vector<u8>&) {};
        link = std::make_unique<party::PartyLink>(fuzz_cfg("Host"), cb);
        std::string err;
        if (!link->start_host(&err)) {
            std::fprintf(stderr, "fuzz host: %s\n", err.c_str());
            std::exit(2);
        }
        port = link->bound_port();
    }
};

// A random encrypted frame (a member's, after WELCOME).
Bytes member_frame(Rng& r, const Corpus& c, party::crypto::Aead& tx) {
    static const char* const kinds[] = {"context_start", "create_room", "join_room", "leave_room", "heartbeat",
                                        "kick_member", "signaling_resolve", "signaling_update", "http", "x"};
    const u8 types[] = {kPing, kPong, kRoster, kRpcReq, kEvent, kEventAck, kPartyCmd, kProgress, kVitals, kHello, kWelcome, 0, 200};
    const u8 type = types[r.below(std::size(types))];
    W w;
    const std::string json = text_of(next_input(r, c, true));
    switch (type) {
    case kPing: w.u64(r.next()); break;
    case kPong: w.u64(r.next()).u64(r.next()); break;
    case kRoster:
        w.u8_(static_cast<u8>(r.below(4)));
        for (int i = 0; i < 3; ++i)
            w.u8_(static_cast<u8>(r.below(9))).str16(std::string(r.below(40), 'n')).u8_(static_cast<u8>(r.below(9))).u8_(1).u32(static_cast<std::uint32_t>(r.next())).u32(0);
        break;
    case kRpcReq: {
        std::string body = json;
        const char* kind = kinds[r.below(std::size(kinds))];
        if (std::string(kind) == "http") {
            json::Value h = json::Value::make_object();
            h.set("Method", "POST");
            h.set("Url", "http://bbparty.invalid:18671/summon_messenger/create");
            h.set("Body", bbnet::party::b64_encode(json));
            body = json::dump(h, 0);
        }
        w.u32(static_cast<std::uint32_t>(r.next())).str16(kind).str32(body);
        break;
    }
    case kEvent: w.u64(r.next() >> r.below(64)).str16(r.chance(4) ? std::string(100, 'e') : "travel").str32(json); break;
    case kEventAck: w.u64(r.next()); break;
    case kPartyCmd: w.str16("cmd").str32(json); break;
    case kProgress: w.str32(json); break;
    case kVitals: vitals_body(r, w); break;
    default: w.raw(json.data(), std::min<std::size_t>(json.size(), 64)); break;
    }
    Bytes body = w.b;
    if (r.chance(5)) mutate_bytes(r, body, c.seeds);  // lengths inside the body lie
    return sealed_frame(tx, type, body);
}

void run_linkhost(Rng& r, const Corpus& c) {
    static LinkHostRig* rig = new LinkHostRig;
    static int round = 0;
    ++round;
    set_input("linkhost", "round " + std::to_string(round));
    const int mode = static_cast<int>(r.below(4));
    if (mode == 0) {  // garbage / pre-auth frames
        Socket s = tcp_connect(rig->port);
        if (s == party::sock::kInvalid) return;
        DWORD to = 30;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
        Bytes b;
        if (r.chance(2)) {
            b = plain_frame(r.chance(2) ? static_cast<u8>(kHello) : static_cast<u8>(r.below(256)),
                            hello_body("fz" + std::to_string(r.below(9)), r.chance(8) ? 1 : party::kLinkProtocolVersion));
            if (!r.chance(3)) mutate_bytes(r, b, c.seeds);
        } else {
            b = next_input(r, c, false);
        }
        set_input("linkhost", b.data(), b.size());
        send_all(s, b.data(), b.size());
        u8 type;
        Bytes body;
        (void)read_frame(s, nullptr, &type, &body);  // CHALLENGE / REJECT / nothing
        party::sock::close(s);
        return;
    }
    // An authenticated member sending mutated encrypted frames.
    AuthedClient cl;
    const std::string name = "fz" + std::to_string(r.below(3));
    if (!cl.open(rig->port, name)) return;
    const int n = 1 + static_cast<int>(r.below(mode == 3 ? 400 : 20));
    Bytes all;
    for (int i = 0; i < n; ++i) {
        Bytes f = member_frame(r, c, cl.tx);
        all.insert(all.end(), f.begin(), f.end());
    }
    if (r.chance(10)) {  // a forged / out-of-order frame at the end
        Bytes f = member_frame(r, c, cl.tx);
        if (f.size() > 10) f[6] ^= 1;
        all.insert(all.end(), f.begin(), f.end());
    }
    set_input("linkhost", all.data(), all.size());
    send_all(cl.s, all.data(), all.size());
    // Drain replies for a moment (RPC_RESP, PONG, ROSTER) so the host's writes do not block.
    u8 type;
    Bytes body;
    const auto until = Clock::now() + std::chrono::milliseconds(mode == 3 ? 100 : 20);
    DWORD to = 20;
    setsockopt(cl.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
    while (Clock::now() < until && read_frame(cl.s, &cl.rx, &type, &body)) {
    }
    check_vitals(*rig->link);
    if (r.chance(2)) {
        const Bytes bye = sealed_frame(cl.tx, kBye, {});
        send_all(cl.s, bye.data(), bye.size());
    }
}

// The malicious host side: a listener the real guest connects to.
struct LinkGuestRig {
    Socket listen_s = party::sock::kInvalid;
    std::uint16_t port = 0;
    std::unique_ptr<party::PartyLink> guest;
    std::atomic<std::uint64_t> events{0};
    LinkGuestRig() {
        listen_s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listen_s, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::listen(listen_s, 8);
        party::sock::socklen al = sizeof a;
        getsockname(listen_s, reinterpret_cast<sockaddr*>(&a), &al);
        port = ntohs(a.sin_port);
        party::LinkCallbacks cb;
        // What party_runtime's guest on_event does with each payload.
        cb.on_event = [this](int, std::uint64_t, const std::string& name, const std::string& body) {
            ++events;
            std::string err;
            if (name == coop::kTravelEventName) {
                coop::TravelIntent t;
                if (coop::TravelFromJsonText(body, &t, &err) && coop::SanitizePeerTravel(&t, &err))
                    INVARIANT(t.lamp_id == coop::kTravelNone || coop::TravelLampKnown(t.lamp_id));
            } else if (name == coop::kStoryEventName) {
                coop::StoryIntent s;
                if (coop::StoryFromJsonText(body, &s, &err)) (void)coop::SanitizePeerStory(&s, &err);
            } else if (name == coop::kItemsEventName) {
                std::vector<coop::ItemGrant> items;
                if (coop::ItemsFromJsonText(body, &items, &err)) (void)coop::FilterPeerItems(items);
            } else if (name == coop::kPhantomEventName) {
                coop::PhantomEvent e;
                (void)coop::PhantomEventFromJsonText(body, &e, &err);
            } else {
                std::uint64_t epoch;
                std::vector<coop::progress::FlagChange> ch;
                (void)coop::progress::ChangesFromJson(body, &epoch, &ch, &err);
            }
        };
        cb.on_roster = [](const std::vector<party::RosterEntry>& ros) {
            INVARIANT(ros.size() <= party::kMaxRosterEntries);
            for (const auto& e : ros) {
                INVARIANT(e.name.size() <= 16 && e.slot <= 7);
                for (char ch : e.name) INVARIANT(static_cast<unsigned char>(ch) >= 0x20 && static_cast<unsigned char>(ch) < 0x7f);
            }
        };
        cb.on_party_cmd = [](int, const std::string&, const std::string&) {};
        cb.on_progress = [](int, const std::vector<u8>&) {};
        guest = std::make_unique<party::PartyLink>(fuzz_cfg("Guest"), cb);
        std::string err;
        guest->start_guest("127.0.0.1", port, &err);
    }
};

void run_linkguest(Rng& r, const Corpus& c) {
    static LinkGuestRig* rig = new LinkGuestRig;
    static int round = 0;
    ++round;
    set_input("linkguest", "round " + std::to_string(round));
    // A rejected guest stops trying: start it afresh.
    if (rig->guest->state() == party::LinkState::Rejected) {
        rig->guest->stop(false);
        rig->guest.reset();
        party::sock::close(rig->listen_s);
        delete rig;
        rig = new LinkGuestRig;
    }
    // Wait for the guest's connection.
    party::sock::PollFd p{};
    p.fd = rig->listen_s;
    p.events = POLLIN;
    if (party::sock::poll(&p, 1, 1000) <= 0) {
        rig->guest->reconnect_now();
        return;
    }
    Socket s = ::accept(rig->listen_s, nullptr, nullptr);
    if (s == party::sock::kInvalid) return;
    DWORD to = 300;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
    u8 type;
    Bytes body;
    if (!read_frame(s, nullptr, &type, &body) || type != kHello) {
        party::sock::close(s);
        return;
    }
    party::crypto::Nonce hn{}, gn{};
    party::crypto::random_bytes(hn.data(), hn.size());
    Bytes out;
    const int mode = static_cast<int>(r.below(5));
    if (mode == 0) {  // a pre-auth answer: mutated CHALLENGE / REJECT / garbage
        W w;
        if (r.chance(2)) w.u8_(static_cast<u8>(r.below(9))).str16(std::string(r.below(3000), 'r'));
        else w.raw(hn.data(), 24);
        out = plain_frame(r.chance(2) ? kChallenge : kReject, w.b);
        mutate_bytes(r, out, c.seeds);
        set_input("linkguest", out.data(), out.size());
        send_all(s, out.data(), out.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        party::sock::close(s);
        return;
    }
    {
        W w;
        w.raw(hn.data(), 24);
        out = plain_frame(kChallenge, w.b);
        send_all(s, out.data(), out.size());
    }
    if (!read_frame(s, nullptr, &type, &body) || type != kAuth || body.size() != 56) {
        party::sock::close(s);
        return;
    }
    std::memcpy(gn.data(), body.data(), 24);
    party::crypto::Aead tx, rx;
    tx.init(party::crypto::session_key(party_key(), "bbp-h2g", hn, gn));
    rx.init(party::crypto::session_key(party_key(), "bbp-g2h", hn, gn));
    // WELCOME (sometimes with a bad slot / max / roster).
    W wl;
    const bool bad = r.chance(3);
    wl.u8_(bad ? static_cast<u8>(r.next()) : 1).u8_(bad ? static_cast<u8>(r.next()) : 4).u8_(static_cast<u8>(r.below(2)));
    wl.u64(r.next()).u32(0x0100007f).u16(1234);
    u8 tok[16] = {};
    wl.raw(tok, 16);
    const unsigned n_ros = static_cast<unsigned>(r.below(bad ? 256 : 4));
    wl.u8_(static_cast<u8>(n_ros));
    for (unsigned i = 0; i < n_ros && i < 40; ++i)
        wl.u8_(static_cast<u8>(r.below(bad ? 256 : 4))).str16(bad ? std::string(r.below(100), '\x07') : "Host").u8_(static_cast<u8>(r.below(10))).u8_(1).u32(0).u32(0);
    Bytes wb = wl.b;
    if (r.chance(4)) mutate_bytes(r, wb, c.seeds);
    out = sealed_frame(tx, kWelcome, wb);
    // Then frames of every kind.
    const int n = static_cast<int>(r.below(mode == 4 ? 300 : 20));
    static const char* const names[] = {"travel", "story", "items", "items_full", "phantom", "flags", "flag_snapshot", "room_member_joined"};
    for (int i = 0; i < n; ++i) {
        const u8 types[] = {kPing, kPong, kRoster, kRpcResp, kEvent, kEvent, kEvent, kPartyCmd, kProgress, kBye, kEventAck, kVitals, 77};
        const u8 t = types[r.below(std::size(types))];
        W w;
        const std::string json = text_of(next_input(r, c, true));
        switch (t) {
        case kEvent: w.u64(static_cast<std::uint64_t>(round) * 1000 + static_cast<std::uint64_t>(i) + 1).str16(names[r.below(std::size(names))]).str32(json); break;
        case kRoster:
            w.u8_(static_cast<u8>(r.below(256)));
            for (int k = 0; k < 4; ++k) w.u8_(static_cast<u8>(r.below(256))).str16(std::string(r.below(64), static_cast<char>(r.below(256)))).u8_(static_cast<u8>(r.below(256))).u8_(1).u32(0).u32(0);
            break;
        case kRpcResp: w.u32(static_cast<std::uint32_t>(r.below(8))).u8_(1).str32(json); break;
        case kBye: if (!r.chance(8)) continue; w.u8_(static_cast<u8>(r.below(10))).str16(std::string(r.below(2000), '\x1b')); break;
        case kPong: w.u64(r.next()).u64(r.next()); break;
        case kVitals: vitals_body(r, w); break;
        default: w.u64(r.next()).str16("x").str32(json); break;
        }
        Bytes b = w.b;
        if (r.chance(6)) mutate_bytes(r, b, c.seeds);
        Bytes f = sealed_frame(tx, t, b);
        out.insert(out.end(), f.begin(), f.end());
    }
    set_input("linkguest", out.data(), out.size());
    send_all(s, out.data(), out.size());
    to = 5;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
    const auto until = Clock::now() + std::chrono::milliseconds(15);
    while (Clock::now() < until && read_frame(s, &rx, &type, &body)) {
    }
    party::sock::close(s);
    check_vitals(*rig->guest);
    rig->guest->reconnect_now();
}

// ---------------------------------------------------------------------------------------------
// regression tests for the bugs this pass fixed

void regress_json() {
    json::Value v;
    std::string err;
    // A wide object with distinct keys parses in linear-ish time (was O(n^2): 10^10 compares).
    std::string wide = "{";
    for (int i = 0; i < 200000; ++i) wide += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":0";
    wide += "}";
    const auto t0 = Clock::now();
    CHECK(json::parse(wide, v, err));
    CHECK(Clock::now() - t0 < std::chrono::seconds(3));
    CHECK(!json::parse("{\"a\":1,\"a\":2}", v, err));
    std::string wide_dup = wide;
    wide_dup.insert(wide_dup.size() - 1, ",\"k5\":1");
    CHECK(!json::parse(wide_dup, v, err));
    // Infinity is not a number (a peer's 1e999 used to become inf, then UB in integer casts).
    CHECK(!json::parse("1e999", v, err));
    CHECK(!json::parse("[-1e400]", v, err));
    CHECK(json::parse("1e308", v, err));
    CHECK(!json::parse("\"a\\u0000b\"", v, err));
    CHECK(!json::parse("\"\\udc00\"", v, err));
    CHECK(json::parse("\"\\ud83d\\ude00\"", v, err) && v.string.size() == 4);
    CHECK(!json::parse(std::string(json::kMaxNumberChars + 1, '1'), v, err));
    std::string deep(json::kMaxDepth + 2, '[');
    CHECK(!json::parse(deep + std::string(json::kMaxDepth + 2, ']'), v, err));
    // Value amplification: more than kMaxValues values is refused.
    std::string many = "[";
    for (std::size_t i = 0; i <= json::kMaxValues; ++i) many += i ? ",0" : "0";
    many += "]";
    CHECK(!json::parse(many, v, err));
    // int_of on numbers an integer cannot hold: clamped, not undefined.
    json::Value o = json::Value::make_object();
    o.set("n", 1e300);
    o.set("m", -1e300);
    CHECK(bbnet::party::int_of(o, "n", 0) > 0 && bbnet::party::int_of(o, "m", 0) < 0);
}

void regress_udp() {
    using bbnet::udp::RelayServer;
    // A full relay table refuses newcomers instead of handing out a taken port.
    RelayServer srv(9307);
    u8 req[net::stun::kMaxRequest], txid[net::stun::kTxid], out[net::stun::kMaxResponse];
    std::size_t registered = 0;
    for (std::uint32_t i = 0; i < 200; ++i) {
        const std::size_t n = net::stun::build_binding_request(req, txid, true, nullptr);
        const std::uint32_t addr = 0x0100000a + ((i / 8) << 24);  // 10.0.0.x, 8 per address
        const std::size_t len = srv.answer_stun(req, n, addr, static_cast<std::uint16_t>(1000 + i), true, out);
        CHECK(len > 0);
        std::uint32_t a;
        std::uint16_t p;
        net::stun::Relay relay;
        if (net::stun::parse_binding_response(out, len, txid, &a, &p, &relay) && relay.present) ++registered;
    }
    CHECK(registered == RelayServer::kMaxClients);
    CHECK(srv.clients() == RelayServer::kMaxClients && srv.stats().refused > 0);
    // Per address cap.
    RelayServer one(9307);
    for (int i = 0; i < 20; ++i) {
        const std::size_t n = net::stun::build_binding_request(req, txid, true, nullptr);
        one.answer_stun(req, n, 0x0100000a, static_cast<std::uint16_t>(2000 + i), true, out);
    }
    CHECK(one.clients() == RelayServer::kMaxClientsPerAddr);
    // The admit filter: no answer (no reflection) and no relay port for strangers.
    RelayServer gated(9307);
    gated.set_admit([](std::uint32_t a) { return a == 0x0100000a; });
    std::size_t n = net::stun::build_binding_request(req, txid, true, nullptr);
    CHECK(gated.answer_stun(req, n, 0x0200000a, 5000, true, out) == 0);
    CHECK(gated.answer_stun(req, n, 0x0100000a, 5000, true, out) > 0 && gated.clients() == 1);
    u8 frame[64] = {0xfb, 'R', 1, 2, 3, 4, 5, 6, 7, 8, 0xc3, 0x51, 'x'};
    u8* o = nullptr;
    std::size_t ol = 0;
    std::uint32_t ta = 0;
    std::uint16_t tp = 0;
    CHECK(!gated.forward(frame, 13, 0x0200000a, 6000, &o, &ol, &ta, &tp));  // unknown token, stranger
    CHECK(gated.clients() == 1);
}

// The generated id tables (party_ids.inc, tools/party/ids_tool.py) and the checks built on them.
template <typename T, std::size_t N>
bool strictly_sorted(const T (&a)[N]) {
    for (std::size_t i = 1; i < N; ++i)
        if (!(a[i - 1] < a[i])) return false;
    return true;
}

void regress_ids() {
    namespace ids = coop::ids;
    std::string err;
    CHECK(strictly_sorted(ids::kPartyMaps) && strictly_sorted(ids::kPartyReturnPoints) &&
          strictly_sorted(ids::kPartyEntities) && strictly_sorted(ids::kPartyRemos));
    CHECK(std::size(ids::kPartyReturnPoints) >= 80 && std::size(ids::kPartyRemos) >= 40 &&
          std::size(ids::kPartyEntities) >= 1000 && std::size(ids::kPartyMaps) >= 20);
    // Maps: folders and MSB variants; not a shape (area 24 block 3 has the right shape, no map).
    CHECK(ids::MapKnown(0x18010000) && ids::MapKnown(0x15000000) && ids::MapKnown(0x24000000));
    CHECK(!ids::MapKnown(0x18030000) && !ids::MapKnown(0x14010000) && !ids::MapKnown(0) && !ids::MapKnown(0xffffffffu));
    // ReturnPointParam rows (0x13CDF30 ids): the observed ones (travel.md 1.1), not area-0 rows.
    for (std::uint32_t id : {2412952u, 2412951u, 2802952u, 2102950u, 2102952u, 2102961u, 3602952u})
        CHECK(coop::TravelLampKnown(id));
    for (std::uint32_t id : {1u, 9902950u, 2412954u, 2402952u, 2412960u, 0u, 2412000u}) CHECK(!coop::TravelLampKnown(id));
    // Remos: the files (sAA_BB_NNNN) incl. the +1000 sex variants; shaped ids without a file are not.
    for (std::uint32_t id : {21000000u, 24011005u, 28001040u, 32000000u, 36000010u}) CHECK(coop::StoryRemoKnown(id));
    for (std::uint32_t id : {24000010u, 27000000u, 21000060u, 32000010u}) CHECK(!coop::StoryRemoKnown(id));
    // Entities: a lamp's own point, EMEVD warp points; an id of another map is not a warp point there.
    CHECK(ids::WarpPointKnown(0x18010000, 2412952) && ids::WarpPointKnown(0x21000000, 3302171));
    CHECK(!ids::WarpPointKnown(0x18000000, 2412952) && !ids::WarpPointKnown(0x18010000, 2419999));
    CHECK(coop::TravelBonfireKnown(2412952) && !coop::TravelBonfireKnown(2419999) && !coop::TravelBonfireKnown(99999999));

    // Travel: every id goes through the tables.
    coop::TravelIntent t;
    t.seq = 1;
    t.kind = coop::TravelKind::Lamp;
    t.lamp_id = 2412954;                       // lamp-shaped, no row
    t.respawn_record = 0x00018b2b0024d198ull;  // aux 101163 << 32 | 2412952
    t.last_lamp = 2412958;                     // shaped, no row
    CHECK(coop::SanitizePeerTravel(&t, &err) && t.lamp_id == coop::kTravelNone &&
          t.respawn_record == 0x00018b2b0024d198ull && t.last_lamp == ~0ull);
    CHECK(coop::ChooseReplay(t).method == coop::ReplayMethod::LampWarp && coop::ChooseReplay(t).id == 2412952);
    t = {};
    t.seq = 2;
    t.kind = coop::TravelKind::LuaStageWarp;
    t.packed_map = 0x18030000;  // shaped like m24_03, no such map
    t.warp_point = 2432000;
    CHECK(coop::SanitizePeerTravel(&t, &err) && t.packed_map == coop::kTravelNone && t.warp_point == coop::kTravelNone);
    CHECK(coop::ChooseReplay(t).method == coop::ReplayMethod::None);
    t.packed_map = 0x18010000;
    t.warp_point = 2412952;
    CHECK(coop::SanitizePeerTravel(&t, &err) && t.packed_map == 0x18010000 && t.warp_point == 2412952);
    t.warp_point = 2602950;  // a point of another map: the destination's default entry instead
    CHECK(coop::SanitizePeerTravel(&t, &err) && t.packed_map == 0x18010000 && t.warp_point == coop::kTravelNone);
    CHECK(coop::ChooseReplay(t).method == coop::ReplayMethod::StageWarp);
    t = {};
    t.seq = 3;
    t.kind = coop::TravelKind::LuaBonfireWarp;
    t.warp_point = 2419999;  // shaped, not an entity of m24_01
    CHECK(coop::SanitizePeerTravel(&t, &err) && t.warp_point == coop::kTravelNone);
    t.warp_point = 2412952;
    CHECK(coop::SanitizePeerTravel(&t, &err) && coop::ChooseReplay(t).method == coop::ReplayMethod::BonfireWarp);
    t = {};
    t.seq = 4;
    t.kind = coop::TravelKind::Transform;
    t.has_pos = true;
    t.pos_map = 0x18030000;
    CHECK(coop::SanitizePeerTravel(&t, &err) && !t.has_pos);
    t.has_pos = true;
    t.pos_map = 0x18010000;
    CHECK(coop::SanitizePeerTravel(&t, &err) && t.has_pos && coop::ChooseReplay(t).method == coop::ReplayMethod::Transform);

    // Story: a remo-shaped id without a file is refused; a real one passes.
    coop::StoryIntent s;
    s.seq = 1;
    s.kind = coop::StoryKind::Cutscene;
    s.id = 27000000;
    CHECK(!coop::SanitizePeerStory(&s, &err));
    s.id = 24011005;
    CHECK(coop::SanitizePeerStory(&s, &err));

    // Phantom: the death redirect lamp.
    coop::PhantomEvent pe;
    pe.kind = coop::PhantomEventKind::Lamp;
    pe.lamp = 2412952;
    CHECK(coop::SanitizePeerPhantom(pe, &err));
    pe.lamp = coop::kTravelNone;
    CHECK(coop::SanitizePeerPhantom(pe, &err));
    for (std::uint32_t bad : {2412954u, 1u, 9902950u, 0x7fffffffu}) {
        pe.lamp = bad;
        CHECK(!coop::SanitizePeerPhantom(pe, &err));
    }

    // Items: lots outside the tables are refused (docs/party/items.md 6.5), whatever the flag.
    CHECK(!coop::ItemGrantFromPeerAllowed({1, 77777777, 990000000, coop::kItemNone, coop::ItemSource::Full}, &err));
    CHECK(coop::ItemGrantFromPeerAllowed({1, 80000000, 5000, coop::kItemNone, coop::ItemSource::Award}, &err));  // key item pair
    CHECK(!coop::ItemGrantFromPeerAllowed({1, 1234567, 61234567, coop::kItemNone, coop::ItemSource::Award}, &err));
    CHECK(!coop::ItemGrantFromPeerAllowed({1, 2400450, 12401800, coop::kItemNone, coop::ItemSource::Flag}, &err));
    CHECK(coop::ItemGrantFromPeerAllowed({1, 2400450, 52400450, coop::kItemNone, coop::ItemSource::Flag}, &err));

    // Sign content (from_api_schema.md 5).
    auto problem = [](const std::string& blob, const std::string& who, const char* body = nullptr) {
        json::Value b;
        std::string e;
        if (body) CHECK(json::parse(body, b, e));
        return bbnet::party::summon_data_problem(reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size(), who,
                                                 body ? &b : nullptr);
    };
    using party_test::sign_blob;
    CHECK(!problem(sign_blob("Alice"), "Alice"));
    CHECK(!problem(sign_blob("Alice"), "Alice", R"({"AreaId":402718720,"MatchingLevel":50})"));
    CHECK(!problem(sign_blob("Hunter_012345678"), "Hunter_012345678"));  // 16 characters
    CHECK(!problem(sign_blob("Alice", 0x18010000, 544, 8, 3), "Alice"));
    CHECK(problem(sign_blob("Alice"), "Bob"));                             // someone else's id
    CHECK(problem(sign_blob("Alice").substr(0, 0xDF), "Alice"));           // size
    CHECK(problem(sign_blob("Alice", 0x18030000), "Alice"));               // no such map
    CHECK(problem(sign_blob("Alice", 0), "Alice"));
    CHECK(problem(sign_blob("Alice"), "Alice", R"({"AreaId":352321536})"));  // blob and request disagree
    CHECK(problem(sign_blob("Alice", 0x18010000, 0), "Alice"));            // level
    CHECK(problem(sign_blob("Alice", 0x18010000, 545), "Alice"));
    CHECK(problem(sign_blob("Alice", 0x18010000, -3), "Alice"));
    CHECK(problem(sign_blob("Alice"), "Alice", R"({"MatchingLevel":51})"));
    CHECK(problem(sign_blob("Alice", 0x18010000, 50, 0x21), "Alice"));    // sign type
    CHECK(problem(sign_blob("Alice", 0x18010000, 50, 0xff), "Alice"));
    CHECK(problem(sign_blob("Alice", 0x18010000, 50, 7, 4), "Alice"));     // NAT type
    std::string b = sign_blob("Alice");
    b[0x45] = 'x';  // a byte after the id's NUL
    CHECK(problem(b, "Alice"));
    b = sign_blob("Al\x01" "ce");
    CHECK(problem(b, "Al\x01" "ce"));  // not an online id
    b = sign_blob("Alice");
    b[0x7A] = 0x50;  // serialization length
    CHECK(problem(b, "Alice"));
    b = sign_blob("Alice");
    b[0x7D] = 'B';  // the SceNpId of another player
    CHECK(problem(b, "Alice"));
    b = sign_blob("Alice");
    const float nan = std::nanf("");
    std::memcpy(&b[0x60], &nan, 4);
    CHECK(problem(b, "Alice"));
    b = sign_blob("Alice");
    const float big = 1e6f;
    std::memcpy(&b[0x68], &big, 4);
    CHECK(problem(b, "Alice"));
}

void regress_decoders() {
    std::string err;
    // Items: a host cannot pair a known lot with an arbitrary flag, or send guest-local rows.
    std::vector<coop::ItemGrant> rows(4);
    rows[0] = {1, 2400450, 52400450, coop::kItemNone, coop::ItemSource::Flag};    // the table's pair
    rows[1] = {2, 2400450, 12401800, coop::kItemNone, coop::ItemSource::Flag};    // another flag: refused
    rows[2] = {3, 43710, 50002560, coop::kItemNone, coop::ItemSource::Mark};      // guest-local source
    rows[3] = {4, 31000, coop::kItemNone, 999, coop::ItemSource::Award};          // ledger row of another lot
    std::size_t rejected = 0;
    const auto ok = coop::FilterPeerItems(rows, &rejected);
    CHECK(rejected >= 3 && ok.size() + rejected == 4);
    if (!ok.empty()) CHECK(ok[0].lot == 2400450 && ok[0].flag == 52400450);
    // A lot outside the tables is taken once whatever flags come with it.
    coop::GuestItems gi;
    std::vector<coop::ItemGrant> adhoc;
    for (int i = 0; i < 50; ++i)
        adhoc.push_back({static_cast<std::uint64_t>(i + 1), 77777777, 990000000 + i, coop::kItemNone, coop::ItemSource::Full});
    CHECK(gi.Offer(adhoc) == 1);
    // 1e300 in an item row: refused, not undefined behaviour.
    std::vector<coop::ItemGrant> out;
    CHECK(!coop::ItemsFromJsonText("{\"items\":[[1e300,2400450,52400450,-1,\"flag\"]]}", &out, &err));
    CHECK(!coop::ItemsFromJsonText("{\"items\":[[-5,2400450,52400450,-1,\"flag\"]]}", &out, &err));
    // Story: no arbitrary flags, no file names the game does not have.
    coop::StoryIntent s;
    s.seq = 1;
    s.kind = coop::StoryKind::Cutscene;
    s.id = 32000000;
    s.flags = {70002802, 12345678, 9000};
    CHECK(coop::SanitizePeerStory(&s, &err) && s.flags == std::vector<std::uint32_t>{70002802});
    s.id = 99000000;
    CHECK(!coop::SanitizePeerStory(&s, &err));
    s.id = 0xffffffffu;
    CHECK(!coop::SanitizePeerStory(&s, &err));
    // Travel: ids and transforms.
    coop::TravelIntent t;
    t.seq = 1;
    t.kind = coop::TravelKind::Lamp;
    t.lamp_id = 0x7fffffff;
    t.last_lamp = 2102961;
    CHECK(coop::SanitizePeerTravel(&t, &err) && t.lamp_id == coop::kTravelNone && coop::HostLampFromTravel(t) == 2102961);
    t.kind = coop::TravelKind::GuestDied;
    CHECK(!coop::SanitizePeerTravel(&t, &err));
    t.kind = coop::TravelKind::Transform;
    t.has_pos = true;
    t.pos_map = 0x18000000;
    t.pos[0] = std::nanf("");
    CHECK(coop::SanitizePeerTravel(&t, &err) && !t.has_pos && coop::ChooseReplay(t).method != coop::ReplayMethod::Transform);
    t.kind = coop::TravelKind::ScriptedWarp;
    t.packed_map = 0x63000000;  // area 99
    CHECK(coop::SanitizePeerTravel(&t, &err) && t.packed_map == coop::kTravelNone);
    CHECK(coop::TravelLampKnown(2412952) && coop::TravelLampKnown(2102961) && !coop::TravelLampKnown(9999999));
    regress_ids();
    // Progress: caps and no truncation of 64-bit ids.
    std::uint64_t epoch;
    std::vector<coop::progress::FlagChange> ch;
    CHECK(!coop::progress::ChangesFromJson("{\"v\":1,\"epoch\":1,\"c\":[[4294979296,1,\"boss_defeated\",1]]}", &epoch, &ch, &err));
    coop::progress::GuestApplier ap;
    coop::progress::FlagSnapshot snap;
    snap.epoch = 1;
    for (int i = 0; i < 3000; ++i) {
        snap.seq = static_cast<std::uint64_t>(i);
        ap.QueueSnapshot(snap);
        ap.QueueChanges(1, {{12401800, true, coop::progress::CategoryOf(12401800), static_cast<std::uint64_t>(i) + 100000}});
    }
    CHECK(ap.Pending() <= coop::progress::kMaxPendingItems);
}

void regress_hostsvc() {
    bbnet::party::PartyHostService svc;
    bbnet::party::FromApi api(svc);
    bbnet::party::Caller a{"fzA", 0x0a00000a};
    json::Value rq, reply;
    std::string err;
    // Addresses must be IPv4, ports 1..65535.
    CHECK(json::parse(R"({"SignalingAddr":"evil.example","SignalingPort":70000,"MappedAddr":"1.2.3.4","MappedPort":4000})", rq, err));
    svc.handle(a, "context_start", rq, reply);
    json::Value rec;
    CHECK(svc.resolve("fzA", &rec) && bbnet::party::str_of(rec, "LocalAddr").empty() &&
          bbnet::party::int_of(rec, "LocalPort", -1) == 0);
    // Room extras are typed; HostPos finite.
    CHECK(json::parse(R"({"HostArea":"x","HostPos":[1e300,{"a":1},3],"MemberTag":[1]})", rq, err));
    svc.handle(a, "create_room", rq, reply);
    bbnet::party::PartyHostService::RoomView v;
    CHECK(svc.room_of("fzA", &v) && !v.extra.find("HostArea") && !v.extra.find("MemberTag"));
    const json::Value* pos = v.extra.find("HostPos");
    CHECK(pos && pos->array.size() == 3 && pos->array[0].number == 0.0);
    // Signs: 0xE0 bytes of strict base64, at most kMaxSignsPerUser per member.
    auto create = [&](int type, const std::string& data) {
        json::Value h = json::Value::make_object();
        h.set("Method", "POST");
        h.set("Url", "http://bbparty.invalid:18671/summon_messenger/create");
        h.set("Body", bbnet::party::b64_encode("{\"SummonType\":" + std::to_string(type) + ",\"SummonData\":\"" + data + "\"}"));
        json::Value r;
        api.handle_json(a, h, r);
    };
    const std::string good = bbnet::party::b64_encode(party_test::sign_blob("fzA"));
    create(0, good.substr(0, good.size() - 4));
    create(0, good + "AAAA");
    create(0, "!" + good.substr(1));
    CHECK(api.signs().empty());
    for (int t = 0; t < 40; ++t) create(t, good);
    CHECK(api.signs().size() == bbnet::party::kMaxSignsPerUser);
    create(1000, good);
    CHECK(api.signs().size() == bbnet::party::kMaxSignsPerUser);
}

bool wait_until(const std::function<bool()>& pred, int ms) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// The PartyLink limits over loopback.
void regress_link() {
    LinkHostRig rig;
    // A pre-auth frame over the handshake limit is dropped at once (no 16 MiB buffer per stranger).
    {
        Socket s = tcp_connect(rig.port);
        W w;
        w.u32(party::kMaxHandshakeFrame + 1).u8_(kHello);
        send_all(s, w.b.data(), w.b.size());
        u8 b;
        CHECK(party::sock::recv_some(s, &b, 1) <= 0);  // closed
        party::sock::close(s);
    }
    // Pending handshakes are capped per address.
    {
        std::vector<Socket> socks;
        for (std::size_t i = 0; i < party::kMaxPendingPerIp + 4; ++i) socks.push_back(tcp_connect(rig.port));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        int closed = 0;
        for (Socket s : socks) {
            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);
            u8 b;
            const long r = party::sock::recv_some(s, &b, 1);
            if (r == 0 || (r < 0 && !party::sock::would_block(party::sock::last_error()))) ++closed;
            party::sock::close(s);
        }
        CHECK(closed >= 4);
    }
    // A handshake that never completes times out.
    {
        Socket s = tcp_connect(rig.port);
        DWORD to = 4000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
        u8 b;
        const auto t0 = Clock::now();
        CHECK(party::sock::recv_some(s, &b, 1) <= 0);
        CHECK(Clock::now() - t0 < std::chrono::milliseconds(3500));
        party::sock::close(s);
    }
    // A member's flood: frames over the rate limit are dropped, RPCs get "rate limited", the
    // host's lock is never held long and another member's ping still comes back.
    {
        AuthedClient flood, other;
        CHECK(flood.open(rig.port, "flooder"));
        CHECK(other.open(rig.port, "bystander"));
        rig.link->debug_lock_hold_max_us(true);
        const std::uint64_t rpcs_before = rig.rpcs.load();
        Bytes burst;
        for (int i = 0; i < 6000; ++i) {
            W w;
            w.u32(static_cast<std::uint32_t>(i)).str16("heartbeat").str32("{\"SessionId\":\"x\",\"MemberId\":1}");
            Bytes f = sealed_frame(flood.tx, kRpcReq, w.b);
            burst.insert(burst.end(), f.begin(), f.end());
        }
        std::thread sender([&] { send_all(flood.s, burst.data(), burst.size()); });
        // The bystander's ping is answered within a second while the flood is processed.
        W pw;
        pw.u64(42);
        const Bytes ping = sealed_frame(other.tx, kPing, pw.b);
        send_all(other.s, ping.data(), ping.size());
        u8 type = 0;
        Bytes body;
        bool pong = false;
        const auto until = Clock::now() + std::chrono::seconds(3);
        while (!pong && Clock::now() < until && read_frame(other.s, &other.rx, &type, &body)) pong = type == kPong;
        CHECK(pong);
        int limited = 0, replies = 0;
        DWORD to = 300;
        setsockopt(flood.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
        while (read_frame(flood.s, &flood.rx, &type, &body)) {
            if (type != kRpcResp) continue;
            ++replies;
            if (std::string(body.begin(), body.end()).find("rate limited") != std::string::npos) ++limited;
        }
        sender.join();
        std::printf("  flood: %d replies, %d rate limited, %llu reached the service; longest lock hold %lld us\n", replies,
                    limited, static_cast<unsigned long long>(rig.rpcs.load() - rpcs_before),
                    static_cast<long long>(rig.link->debug_lock_hold_max_us()));
        CHECK(limited > 0);
        CHECK(rig.rpcs.load() - rpcs_before <= static_cast<std::uint64_t>(party::kMemberFrameBurst) + 1000);
        CHECK(rig.link->debug_lock_hold_max_us() < 200000);
    }
    // A guest refuses a WELCOME with an impossible slot.
    {
        LinkGuestRig g;
        party::sock::PollFd p{};
        p.fd = g.listen_s;
        p.events = POLLIN;
        CHECK(party::sock::poll(&p, 1, 2000) > 0);
        Socket s = ::accept(g.listen_s, nullptr, nullptr);
        DWORD to = 2000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
        u8 type;
        Bytes body;
        CHECK(read_frame(s, nullptr, &type, &body) && type == kHello);
        party::crypto::Nonce hn{}, gn{};
        W c;
        c.raw(hn.data(), 24);
        Bytes f = plain_frame(kChallenge, c.b);
        send_all(s, f.data(), f.size());
        CHECK(read_frame(s, nullptr, &type, &body) && type == kAuth);
        std::memcpy(gn.data(), body.data(), 24);
        party::crypto::Aead tx;
        tx.init(party::crypto::session_key(party_key(), "bbp-h2g", hn, gn));
        W wl;
        u8 tok[16] = {};
        wl.u8_(200).u8_(4).u8_(0).u64(0).u32(0).u16(0).raw(tok, 16).u8_(0);
        f = sealed_frame(tx, kWelcome, wl.b);
        send_all(s, f.data(), f.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(g.guest->state() != party::LinkState::Connected && g.guest->local_slot() != 200);
        party::sock::close(s);
        // The guest comes back: a valid WELCOME, then PONGs whose times would overflow the clock
        // arithmetic (the fuzzer's find: signed overflow in the clock offset).
        CHECK(party::sock::poll(&p, 1, 3000) > 0);
        s = ::accept(g.listen_s, nullptr, nullptr);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
        CHECK(read_frame(s, nullptr, &type, &body) && type == kHello);
        send_all(s, f.data(), 0);
        c.b.clear();
        c.raw(hn.data(), 24);
        f = plain_frame(kChallenge, c.b);
        send_all(s, f.data(), f.size());
        CHECK(read_frame(s, nullptr, &type, &body) && type == kAuth);
        std::memcpy(gn.data(), body.data(), 24);
        tx.init(party::crypto::session_key(party_key(), "bbp-h2g", hn, gn));
        W ok;
        ok.u8_(1).u8_(4).u8_(0).u64(0x7fffffffffffffffull).u32(0).u16(0).raw(tok, 16).u8_(0);
        f = sealed_frame(tx, kWelcome, ok.b);
        send_all(s, f.data(), f.size());
        for (std::uint64_t t : {0x8000000000000000ull, 0xffffffffffffffffull, 1ull}) {
            W pong;
            pong.u64(t).u64(0x7fffffffffffffffull);
            f = sealed_frame(tx, kPong, pong.b);
            send_all(s, f.data(), f.size());
        }
        CHECK(wait_until([&] { return g.guest->state() == party::LinkState::Connected; }, 2000));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(g.guest->rtt_ms() <= 3600000);
        const std::int64_t hc = g.guest->host_clock_ms();
        CHECK(hc >= 0 && hc < (1ll << 40));
        party::sock::close(s);
        g.guest->stop(false);
        party::sock::close(g.listen_s);
    }
}

// One handshake with `key` as the party key: 'W' WELCOME, 'R' REJECT (*reason), '?' otherwise.
char try_handshake(std::uint16_t port, const party::crypto::Key& key, const std::string& name, std::string* reason) {
    Socket s = tcp_connect(port);
    if (s == party::sock::kInvalid) return '?';
    DWORD to = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
    const Bytes hello = plain_frame(kHello, hello_body(name));
    send_all(s, hello.data(), hello.size());
    u8 type = 0;
    Bytes body;
    char out = '?';
    auto reject_reason = [&] {
        if (body.size() >= 3) {
            const std::size_t n = std::min<std::size_t>(body[1] | body[2] << 8, body.size() - 3);
            *reason = std::string(body.begin() + 3, body.begin() + 3 + static_cast<std::ptrdiff_t>(n));
        }
        return 'R';
    };
    if (read_frame(s, nullptr, &type, &body)) {
        if (type == kReject) {
            out = reject_reason();
        } else if (type == kChallenge && body.size() == 24) {
            party::crypto::Nonce hn{}, gn{};
            std::memcpy(hn.data(), body.data(), 24);
            party::crypto::random_bytes(gn.data(), gn.size());
            const party::crypto::Key proof = party::crypto::auth_proof(key, hn, gn);
            W a;
            a.raw(gn.data(), 24).raw(proof.data(), 32);
            const Bytes auth = plain_frame(kAuth, a.b);
            send_all(s, auth.data(), auth.size());
            party::crypto::Aead rx;
            rx.init(party::crypto::session_key(key, "bbp-h2g", hn, gn));
            if (read_frame(s, &rx, &type, &body)) out = type == kReject ? reject_reason() : type == kWelcome ? 'W' : '?';
        }
    }
    party::sock::close(s);
    return out;
}

// F3: online password guessing (party_link.h AuthThrottle).
void regress_auth_throttle() {
    using party::AuthThrottle;
    {  // the pure rule
        AuthThrottle t;
        const std::uint32_t a = 0x0100007f, b = 0x0200007f;
        std::int64_t now = 1000, left = 0;
        for (int i = 0; i < party::kAuthFailFree; ++i) CHECK(!t.Fail(a, now + i));
        CHECK(!t.Locked(a, now + 10));
        CHECK(t.Fail(a, now + 10));  // the 6th wrong proof in a minute
        CHECK(t.Locked(a, now + 11, &left) && left > party::kAuthLockoutMs - 100 && left <= party::kAuthLockoutMs);
        CHECK(!t.Locked(b, now + 11));  // per address
        now += 10 + party::kAuthLockoutMs;
        CHECK(!t.Locked(a, now));
        CHECK(t.Fail(a, now));  // after a lockout one wrong proof locks again, twice as long
        CHECK(t.Locked(a, now + 2 * party::kAuthLockoutMs - 1) && !t.Locked(a, now + 2 * party::kAuthLockoutMs));
        for (int k = 0; k < 20; ++k) {  // the doubling stops at the cap
            now += party::kAuthLockoutMaxMs;
            t.Fail(a, now);
        }
        CHECK(t.Locked(a, now + party::kAuthLockoutMaxMs - 1, &left) && left <= party::kAuthLockoutMaxMs);
        CHECK(!t.Locked(a, now + party::kAuthLockoutMaxMs));
        now += party::kAuthLockoutMaxMs + party::kAuthForgetMs + 1;  // forgotten: free tries again
        for (int i = 0; i < party::kAuthFailFree; ++i) CHECK(!t.Fail(a, now));
        // Slow guessing (5 a minute) never locks; a good proof resets the free count.
        AuthThrottle slow;
        for (int i = 0; i < 50; ++i) CHECK(!slow.Fail(b, 1000 + i * (party::kAuthFailWindowMs / 4)));
        AuthThrottle ok;
        for (int i = 0; i < party::kAuthFailFree; ++i) ok.Fail(b, 5);
        ok.Success(b);
        CHECK(!ok.Fail(b, 6));
        // A stranger spraying addresses: the table stays bounded and a locked address stays tracked.
        AuthThrottle many;
        for (int i = 0; i <= party::kAuthFailFree; ++i) many.Fail(a, 10);
        for (std::uint32_t ip = 1; ip < party::kAuthTrackedAddrs + 500; ++ip) many.Fail(0x0a000000 + ip, 20 + ip);
        CHECK(many.Tracked() <= party::kAuthTrackedAddrs);
        CHECK(many.Locked(a, 30));
    }
    {  // the global budget: a guesser spread over many addresses (one try each)
        AuthThrottle g;
        const std::uint32_t member = 0x0100007f, stranger = 0x05050505;
        g.Success(member);  // was in the party before
        std::int64_t now = 1000, left = 0;
        bool global = false;
        bool started = false;
        for (int i = 0; i < party::kAuthGlobalFree; ++i) started |= g.Fail(0x0a000000 + i, now + i);
        CHECK(!started && !g.GlobalLocked(now + 100));
        CHECK(g.Fail(0x0b000000, now + 100));  // one more within the minute: the global lockout
        CHECK(g.GlobalLocked(now + 101) && g.global_lockouts() == 1);
        CHECK(g.Locked(stranger, now + 101, &left, &global) && global && left <= party::kAuthGlobalLockoutMs);
        CHECK(!g.Locked(member, now + 101));  // a member who was in still gets in
        CHECK(!g.Locked(stranger, now + 100 + party::kAuthGlobalLockoutMs));
        // spread slowly (30 a minute) it never starts
        AuthThrottle s;
        bool any = false;
        for (int i = 0; i < 300; ++i) any |= s.Fail(0x0c000000 + i, 1000 + i * (party::kAuthFailWindowMs / party::kAuthGlobalFree + 50));
        CHECK(!any && s.global_lockouts() == 0);
        // the next global lockout doubles, capped
        now += 100 + party::kAuthGlobalLockoutMs;
        for (int i = 0; i <= party::kAuthGlobalFree; ++i) g.Fail(0x0d000000 + i, now + i);
        CHECK(g.global_lockouts() == 2 && g.Locked(stranger, now + 2 * party::kAuthGlobalLockoutMs - 100));
    }
    {  // a real host over loopback
        party::LinkCallbacks cb;
        cb.on_event = [](int, std::uint64_t, const std::string&, const std::string&) {};
        cb.on_party_cmd = [](int, const std::string&, const std::string&) {};
        cb.on_progress = [](int, const std::vector<u8>&) {};
        cb.on_rpc = [](int, const std::string&, const std::string&) { return std::string("{}"); };
        party::LinkConfig cfg = fuzz_cfg("Host");
        cfg.auth_throttle = true;
        party::PartyLink host(cfg, cb);
        std::string err;
        CHECK(host.start_host(&err));
        const std::uint16_t port = host.bound_port();
        const std::array<u8, 8> secret{};
        const party::crypto::Key wrong = party::crypto::derive_party_key("not-the-password", secret.data());
        std::string why;
        int rejected = 0;
        for (int i = 0; i < party::kAuthFailFree + 1; ++i) rejected += try_handshake(port, wrong, "guess", &why) == 'R';
        CHECK(rejected == party::kAuthFailFree + 1);
        // Locked out: refused at HELLO, the right password included, with a reason the guest shows.
        why.clear();
        CHECK(try_handshake(port, party_key(), "friend", &why) == 'R');
        CHECK(why.find("too many wrong passwords") != std::string::npos);
        std::printf("  auth throttle: %d wrong proofs -> \"%s\"\n", rejected, why.c_str());
        host.stop(false);
    }
    {  // the default host lets a few typos through, then the right password in
        party::LinkCallbacks cb;
        cb.on_event = [](int, std::uint64_t, const std::string&, const std::string&) {};
        cb.on_party_cmd = [](int, const std::string&, const std::string&) {};
        cb.on_progress = [](int, const std::vector<u8>&) {};
        cb.on_rpc = [](int, const std::string&, const std::string&) { return std::string("{}"); };
        party::LinkConfig cfg = fuzz_cfg("Host");
        cfg.auth_throttle = true;
        party::PartyLink host(cfg, cb);
        std::string err;
        CHECK(host.start_host(&err));
        const std::array<u8, 8> secret{};
        const party::crypto::Key wrong = party::crypto::derive_party_key("typo", secret.data());
        std::string why;
        for (int i = 0; i < 3; ++i) CHECK(try_handshake(host.bound_port(), wrong, "friend", &why) == 'R');
        CHECK(try_handshake(host.bound_port(), party_key(), "friend", &why) == 'W');
        host.stop(false);
    }
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetUnhandledExceptionFilter(on_crash);
    _putenv("BB_PARTY_LOOPBACK=1");
#else
    setenv("BB_PARTY_LOOPBACK", "1", 1);
#endif
    party::sock::startup();
#ifdef PARTY_FUZZ_QUICK
    double seconds = 20;
#else
    double seconds = 60;
#endif
    long long iters = -1;
    std::uint64_t seed = static_cast<std::uint64_t>(now_ms());
    bool regress = true;
    std::vector<std::string> only;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--iters" && i + 1 < argc) iters = std::atoll(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--no-regress") regress = false;
        else only.push_back(a);
    }
    std::thread dog(watchdog, 20000);
    if (regress) {
        std::printf("regressions\n");
        set_input("regress", "");
        regress_json();
        regress_udp();
        regress_decoders();
        regress_hostsvc();
        regress_link();
        regress_auth_throttle();
        std::printf("  %d checks, %d failures\n", g_checks, g_fail);
    }
    const Corpus jc = json_corpus();
    Corpus bin;  // binary seeds: STUN, relay frames, vport headers, party codes, identity files
    {
        u8 req[net::stun::kMaxRequest], txid[net::stun::kTxid], resp[net::stun::kMaxResponse];
        std::size_t n = net::stun::build_binding_request(req, txid, true, nullptr);
        bin.add(Bytes(req, req + n));
        n = net::stun::build_binding_request(req, txid, false, nullptr);
        bin.add(Bytes(req, req + n));
        net::stun::Relay rel;
        rel.present = true;
        rel.vport = 50002;
        n = net::stun::build_binding_response(resp, txid, 0x0100007f, 3658, &rel);
        bin.add(Bytes(resp, resp + n));
        bin.add(Bytes{0xfb, 'R', 1, 2, 3, 4, 5, 6, 7, 8, 0xc3, 0x52, 0xff, 0xc3, 30, 40, 'g'});
        bin.add(Bytes{0xfb, 'r', 0xc3, 0x52, 0xff, 0xc3, 30, 40, 'g'});
        bin.add(Bytes{0xff, 0xc3, 30, 40, 'h', 'i'});
        bin.add(Bytes{0xff, 0x83, 0, 30, 0, 40, 'h'});
        bin.add(Bytes(std::begin(bbnet::udp::kProbe), std::end(bbnet::udp::kProbe)));
        std::array<u8, 8> secret = {1, 2, 3, 4, 5, 6, 7, 8};
        bin.add(party::make_party_code({203, 0, 113, 7}, 9307, secret, party::kCodeFlagPasswordRequired));
        bin.add(std::string("bbp1-0000 0000"));
        bin.add(std::string("example.com:9307"));
        bin.add(std::string("hash " + std::string(64, 'a') + "\npatch a.patch\n# x\n"));
        for (const auto& s : jc.seeds) bin.add(s);
    }
    struct Named {
        const char* name;
        Target fn;
        const Corpus* corpus;
        double share;  // of the time budget
    };
    std::vector<Named> targets = {
        {"json", run_json, &jc, 1},       {"code", run_code, &bin, 0.5},     {"stun", run_stun, &bin, 0.5},
        {"udp", run_udp, &bin, 0.7},      {"travel", run_travel, &jc, 1},    {"story", run_story, &jc, 1},
        {"items", run_items, &jc, 1},     {"phantom", run_phantom, &jc, 0.5}, {"lamp", run_lamp, &jc, 0.3}, {"peerev", run_peerev, &jc, 0.5}, {"balance", run_balance, &jc, 0.2}, {"progress", run_progress, &jc, 1},
        {"hostsvc", run_hostsvc, &jc, 1.5}, {"linkhost", run_linkhost, &jc, 2}, {"linkguest", run_linkguest, &jc, 2},
    };
    double total_share = 0;
    for (const Named& t : targets)
        if (only.empty() || std::find(only.begin(), only.end(), t.name) != only.end()) total_share += t.share;
    std::printf("fuzzing, seed 0x%llx, %.0f s\n", static_cast<unsigned long long>(seed), seconds);
    long long total = 0;
    for (const Named& t : targets) {
        if (!only.empty() && std::find(only.begin(), only.end(), t.name) == only.end()) continue;
        Rng r(seed ^ std::hash<std::string>()(t.name));
        const auto budget = std::chrono::duration<double>(seconds * t.share / total_share);
        const auto start = Clock::now();
        long long n = 0;
        while ((iters < 0 || n < iters) && Clock::now() - start < budget) {
            try {
                t.fn(r, *t.corpus);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "exception: %s\n", e.what());
                die("an exception escaped");
            } catch (...) {
                die("a non-std exception escaped");
            }
            ++n;
        }
        total += n;
        const double secs = std::chrono::duration<double>(Clock::now() - start).count();
        std::printf("  %-10s %9lld inputs  %7.0f/s  peak live %lld MiB, biggest alloc %zu KiB\n", t.name, n, n / std::max(secs, 1e-9),
                    static_cast<long long>(g_peak.load() >> 20), g_biggest.load() >> 10);
        std::fflush(stdout);
    }
    g_beat.store(0);
    g_watch = false;
    dog.join();
    std::printf("%lld inputs, %d regression checks, %d failures\n", total, g_checks, g_fail);
    std::fflush(stdout);
    std::_Exit(g_fail ? 1 : 0);  // the rigs' threads stay up: no teardown
}
