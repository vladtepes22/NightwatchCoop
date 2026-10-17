// SPDX-License-Identifier: GPL-3.0-or-later
// The game's online state as the FROM client and the network flow keep it, logged on every
// change ("online:" lines), so a party log shows where the title's online chain stops
// (docs/party/from_api_schema.md §1.1, §3.1, §7):
//   FrpgNetMan (slot 0x553b120): +0x8 net up, +0xa online flag (title choice), +0xb, +0x9f6 /
//     +0x9f8 request blockers, +0xa30 the queued error messages (a set; its size at +0xa40), +0xa50 server-offline flag,
//     +0xa90 CharaId used in the summon / message requests
//   SprjNetworkClientMan (slot 0x5540288): +0x18 parsed ss.info (its +8 <ss>), +0x3e4 ss.info
//     failures, +0x60 UserId (0x8000000000000000 = not logged in), +0x88 SessionId length
//   GameData (slot 0x553b130) +8 -> +0x690 the save's CharaId (sync_chara_id runs only while 0)
//   network flow (slot 0x5556678) +0x1590 online mode in the world
// A request builder (e.g. summon_messenger/create 0x1e90e30) sends nothing unless: net up,
// +0x9f6 / +0x9f8 / +0xa50 clear, ss.info parsed with <ss> 0, UserId set, SessionId non-empty.
// Read-only polling (4 Hz) on its own thread from the first sceHttpInit under BB_PARTY;
// BB_PARTY_ONLINE_WATCH=0 turns it off.
#include "bbnet_internal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace bbnet {

namespace {

constexpr std::uint64_t kFrpgNetManSlot = 0x553b120;
constexpr std::uint64_t kClientManSlot = 0x5540288;
constexpr std::uint64_t kGameDataSlot = 0x553b130;
constexpr std::uint64_t kNetFlowSlot = 0x5556678;
constexpr std::int64_t kNoUser = INT64_MIN;

template <class T>
bool rd(std::uintptr_t addr, T* out) {
    return addr && guest_read(addr, out, sizeof(T));
}
std::uintptr_t slot(std::uint64_t off) {
    void* p = guest_image_at(off, 8);
    std::uint64_t v = 0;
    if (!p || !guest_read(reinterpret_cast<std::uintptr_t>(p), &v, 8)) return 0;
    return static_cast<std::uintptr_t>(v);
}

struct Snapshot {
    // -1 / -2: object missing / unreadable
    int net_up = -1, online = -1, flag_b = -1, block_9f6 = -1, block_9f8 = -1, server_offline = -1;
    std::int64_t error_msg = -1;
    std::int64_t chara_id = -1;
    int ss = -1;  // -1 no client, -2 not parsed, else <ss>
    int ss_fails = -1;
    std::int64_t user_id = -1;
    std::int64_t session_len = -1;
    std::int64_t save_chara = -1;
    int owner_type = -1;      // [PlayerGameData+0x5d0] -> +0xc
    std::string owner_a, owner_b;  // its wide strings at +0x18 / +0xc8
    int flow_online = -1;

    bool operator==(const Snapshot& o) const {
        return net_up == o.net_up && online == o.online && flag_b == o.flag_b && block_9f6 == o.block_9f6 &&
               block_9f8 == o.block_9f8 && server_offline == o.server_offline && error_msg == o.error_msg &&
               chara_id == o.chara_id && ss == o.ss && ss_fails == o.ss_fails && user_id == o.user_id &&
               session_len == o.session_len && save_chara == o.save_chara && owner_type == o.owner_type &&
               owner_a == o.owner_a && owner_b == o.owner_b && flow_online == o.flow_online;
    }
};

std::string wide_at(std::uintptr_t at);

Snapshot take() {
    Snapshot s;
    if (const std::uintptr_t fm = slot(kFrpgNetManSlot)) {
        std::uint8_t b = 0;
        std::int64_t q = 0;
        s.net_up = rd(fm + 0x8, &b) ? b : -2;
        s.online = rd(fm + 0xa, &b) ? b : -2;
        s.flag_b = rd(fm + 0xb, &b) ? b : -2;
        s.block_9f6 = rd(fm + 0x9f6, &b) ? b : -2;
        s.block_9f8 = rd(fm + 0x9f8, &b) ? b : -2;
        s.server_offline = rd(fm + 0xa50, &b) ? b : -2;
        s.error_msg = rd(fm + 0xa40, &q) ? q : -2;
        s.chara_id = rd(fm + 0xa90, &q) ? q : -2;
    }
    if (const std::uintptr_t cm = slot(kClientManSlot)) {
        std::uint64_t ss = 0;
        std::int32_t st = 0;
        s.ss = rd(cm + 0x18, &ss) ? (ss ? (rd(ss + 8, &st) ? st : -3) : -2) : -3;
        if (!rd(cm + 0x3e4, &s.ss_fails)) s.ss_fails = -2;
        if (!rd(cm + 0x60, &s.user_id)) s.user_id = -2;
        if (!rd(cm + 0x88, &s.session_len)) s.session_len = -2;
    }
    if (const std::uintptr_t gd = slot(kGameDataSlot)) {
        std::uint64_t pgd = 0;
        if (rd(gd + 8, &pgd) && pgd) {
            if (!rd(pgd + 0x690, &s.save_chara)) s.save_chara = -2;
            std::uint64_t holder = 0, obj = 0;
            if (rd(pgd + 0x5d0, &holder) && holder && rd(holder, &obj)) {
                if (!obj) {
                    s.owner_type = -2;
                } else {
                    std::int32_t t = 0;
                    s.owner_type = rd(obj + 0xc, &t) ? t : -3;
                    s.owner_a = wide_at(obj + 0x18);
                    s.owner_b = wide_at(obj + 0xc8);
                }
            }
        }
    }
    if (const std::uintptr_t flow = slot(kNetFlowSlot)) {
        std::uint8_t b = 0;
        s.flow_online = rd(flow + 0x1590, &b) ? b : -2;
    }
    return s;
}

// An MSVC std::wstring (2-byte chars) at `at`: +8 inline buffer or pointer (capacity +0x20 > 7),
// size +0x18; as ASCII ('?' for others), at most 40 characters.
std::string wide_at(std::uintptr_t at) {
    std::uint64_t size = 0, cap = 0, data = at + 8;
    if (!rd(at + 0x18, &size) || !rd(at + 0x20, &cap)) return "?";
    if (cap > 7 && !rd(at + 8, &data)) return "?";
    if (size > 40) size = 40;
    std::string out;
    for (std::uint64_t i = 0; i < size; ++i) {
        std::uint16_t c = 0;
        if (!rd(data + 2 * i, &c)) return out + "?";
        out += c >= 32 && c < 127 ? static_cast<char>(c) : '?';
    }
    return out;
}

std::string id_str(std::int64_t v) {
    if (v == kNoUser) return "none";
    return std::to_string(v);
}

std::string describe(const Snapshot& s) {
    char b[768];
    const char* ss = s.ss == -1 ? "no client" : s.ss == -2 ? "not parsed" : s.ss == -3 ? "?" : nullptr;
    char ssb[16];
    if (!ss) {
        std::snprintf(ssb, sizeof ssb, "<ss>%d", s.ss);
        ss = ssb;
    }
    // A request builder sends only when all of these hold (0x1e90e30 and the others).
    const bool can_send = s.net_up > 0 && s.block_9f6 == 0 && s.block_9f8 == 0 && s.server_offline == 0 &&
                          s.ss == 0 && s.user_id != kNoUser && s.user_id >= 0 && s.session_len > 0;
    std::snprintf(b, sizeof b,
                  "FrpgNetMan net %d online %d b %d blockers 9f6=%d 9f8=%d server-offline %d queued msgs %lld chara %s; "
                  "client ss.info %s (failures %d) user %s session %lld chars; save chara %s; save owner type %d '%s' / '%s'; "
                  "world online mode %d; requests %s",
                  s.net_up, s.online, s.flag_b, s.block_9f6, s.block_9f8, s.server_offline,
                  static_cast<long long>(s.error_msg), id_str(s.chara_id).c_str(), ss, s.ss_fails,
                  id_str(s.user_id).c_str(), static_cast<long long>(s.session_len), id_str(s.save_chara).c_str(),
                  s.owner_type, s.owner_a.c_str(), s.owner_b.c_str(), s.flow_online, can_send ? "can go out" : "are refused client-side");
    return b;
}

void watch_main() {
    runtime_thread_attach_host("bb:online-watch");
    Snapshot last;
    bool first = true;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const Snapshot s = take();
        if (first || !(s == last)) {
            const OnlineProgress p = online_progress();
            log("online: %s (http: ss.info %d, login %d, chara id %d, notice %d, failures %d)", describe(s).c_str(),
                p.ss_info, p.login, p.chara_id, p.notice, p.failures);
            last = s;
            first = false;
        }
    }
}

}  // namespace

void online_watch_start() {
    static std::atomic<bool> started{false};
    if (!settings().party || started.exchange(true)) return;
    const char* e = std::getenv("BB_PARTY_ONLINE_WATCH");
    if (e && e[0] == '0') return;
    std::thread(watch_main).detach();
}

}  // namespace bbnet
