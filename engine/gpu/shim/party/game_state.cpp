// SPDX-License-Identifier: GPL-3.0-or-later
// Game state reads for the party director (see game_state.h). Offsets from droogie/bbhost
// (GPL-3.0-or-later; src/engine/np_test.cpp, loading.cpp, player_data.cpp and
// include/bbhost/engine/sprj/*.hpp @8f2746c), each checked against our 1.09 eboot.
#include "game_state.h"

#include "coop_hooks.h"
#include "party_start.h"
#include "party_async_io.h"
#include "../bbport_threads.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <initializer_list>
#include <mutex>
#include <string>

namespace coop {
namespace {

constexpr std::uint64_t kWorldChrMan = 0x553e878;
constexpr std::uint64_t kSessionManager = 0x5540290;
constexpr std::uint64_t kGameDataMan = 0x553b130;
constexpr std::uint64_t kNowLoadingRequested = 0x556286b;
constexpr std::uint64_t kNowLoadingHelper = 0x553e8b8;
constexpr std::uint64_t kNetFlow = 0x5556678;
constexpr std::uint64_t kNpManager = 0x56c7048;
constexpr std::uint64_t kCooperatorCount = 0x15bdc20;

constexpr std::uint64_t kRecInsight = 0x84, kRecLevel = 0x90, kRecEchoes = 0x94;

// Campaign start (game_state.h).
constexpr std::uint64_t kEventFlagMan = 0x553b100;
constexpr std::uint64_t kItemGetMenu = 0x553d6e0;
constexpr std::uint64_t kGetEventFlag = 0x13cfd80;
constexpr std::uint64_t kSetEventFlag = 0x13cfcc0;
constexpr std::uint64_t kInventoryFind = 0x14d9e80;
constexpr std::uint64_t kAwardItemLot = 0x17ddc50;
constexpr std::uint64_t kGiveItemDirect = 0x131cb70;
constexpr std::uint64_t kLampWarp = 0x13cdf30; // void (u32 ReturnPoint id)
constexpr std::uint64_t kRecInventory = 0x328;
constexpr std::uint32_t kTypeGoods = 0x40000000u;

/// The function's first bytes match 1.09 (checked once; a mismatch is logged once).
bool CodeOk(std::uint64_t off, std::initializer_list<std::uint8_t> bytes, const char* name, int* state) {
    if (*state == 0) {
        *state = MatchesOriginal(off, bytes) ? 1 : -1; // our own entry hooks do not count
        if (*state < 0) {
            char line[160];
            std::snprintf(line, sizeof line, "Party: start: %s (0x%llx) differs from 1.09; off\n", name,
                          static_cast<unsigned long long>(off));
            coop::asyncio::Print(line);
        }
    }
    return *state > 0;
}

bool GetFlagOk() {
    static int st = 0;
    return CodeOk(kGetEventFlag, {0x55, 0x41, 0x57, 0x41, 0x56, 0x53, 0x41, 0x89, 0xd0, 0x44, 0x8b, 0x4f, 0x1c},
                  "GetEventFlagValue", &st);
}
bool SetFlagOk() {
    static int st = 0;
    return CodeOk(kSetEventFlag, {0x41, 0x89, 0xd0, 0x8b, 0x4f, 0x1c, 0x31, 0xd2, 0x89, 0xf0, 0xf7, 0xf1},
                  "SetEventFlag", &st);
}
bool FindOk() {
    static int st = 0;
    return CodeOk(kInventoryFind, {0x53, 0x81, 0xe6, 0x00, 0x00, 0x00, 0xf0, 0x81, 0xe2, 0xff, 0xff, 0xff, 0x0f},
                  "inventory index", &st);
}
bool AwardOk() {
    static int st = 0;
    return CodeOk(kAwardItemLot, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                  0x81, 0xec, 0xa8, 0x01, 0x00, 0x00, 0x41, 0x89, 0xd6},
                  "AwardItemLot", &st);
}
bool GiveOk() {
    static int st = 0;
    return CodeOk(kGiveItemDirect, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
                                    0x48, 0x83, 0xec, 0x68, 0x89, 0xcb, 0x41, 0x89, 0xd6, 0x41, 0x89, 0xf7},
                  "GiveItemDirect", &st);
}

constexpr std::uint64_t kItemGetList = 0x553b1c0;  // the item-get dialog's list (0x17D9341)
constexpr std::uint64_t kItemGetPush = 0x16d8850;  // (list, &handle, type bits, id, count, stored)
bool ItemGetOk() {
    static int st = 0;
    return CodeOk(kItemGetPush, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                 0x83, 0xec, 0x28, 0x48, 0x89, 0xfb},
                  "item-get list push", &st);
}
constexpr std::uint64_t kRemoSkip = 0x1d07400;  // (SprjRemo*, int* remo id or -1)
constexpr std::uint64_t kSprjRemo = 0x5540058;
int& st2() {
    static int st = 0;
    return st;
}
bool RemoSkipOk() {
    static int st = 0;
    return CodeOk(kRemoSkip, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x18, 0x48, 0x8b, 0x1d, 0xd0, 0xc7, 0x6d,
                              0x03},
                  "remo skip request", &st) &&
           CodeOk(0x1cf9d40, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x48,
                              0x89, 0xfb},
                  "remo player skip", &st2());
}
constexpr std::uint64_t kSaveRequest = 0x1336da0; // Lua SaveRequest (void)
constexpr std::uint64_t kLuaEventMan = 0x553b0c8;  // SprjLuaEventMan (the binding asserts it)
bool SaveRequestOk() {
    static int st = 0;
    return CodeOk(kSaveRequest, {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8d, 0x05, 0x1d, 0x43, 0x20, 0x04, 0x48, 0x83, 0x38,
                                 0x00, 0x75},
                  "SaveRequest", &st);
}

bool LampWarpOk() {
    static int st = 0;
    return CodeOk(kLampWarp, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x38},
                  "lamp warp", &st);
}

std::uint64_t Slot(std::uint64_t off) {
    std::uint64_t v = 0;
    const std::uint64_t at = Guest(off);
    return at && SafeGet(at, &v) ? v : 0;
}

template <class T>
T Field(std::uint64_t base, std::uint64_t off, T fallback) {
    T v{};
    return base && SafeGet(base + off, &v) ? v : fallback;
}

bool Plausible(std::uint64_t address, std::size_t n) {
    return address >= 0x10000 && address + n < (1ull << 47) && address + n >= address;
}

} // namespace

__attribute__((noinline)) bool SafeRead(std::uint64_t address, void* out, std::size_t n) {
    if (!Plausible(address, n)) {
        return false;
    }
    BbRecoverBuf* const prev = runtime_fault_recover;
    BbRecoverBuf recover;
    if (BB_RECOVER_SET(recover)) {
        runtime_fault_recover = prev; // the fault handler cleared it
        return false;
    }
    runtime_fault_recover = &recover;
    // One copy (a fault anywhere in it lands in BB_RECOVER_SET above); small fixed sizes become
    // single loads. The party layer makes thousands of these per frame (flag tree, game state).
    std::memcpy(out, reinterpret_cast<const void*>(address), n);
    runtime_fault_recover = prev;
    return true;
}

__attribute__((noinline)) bool SafeWrite(std::uint64_t address, const void* in, std::size_t n) {
    if (!Plausible(address, n)) {
        return false;
    }
    BbRecoverBuf* const prev = runtime_fault_recover;
    BbRecoverBuf recover;
    if (BB_RECOVER_SET(recover)) {
        runtime_fault_recover = prev;
        return false;
    }
    runtime_fault_recover = &recover;
    volatile unsigned char* dst = reinterpret_cast<volatile unsigned char*>(address);
    const unsigned char* src = static_cast<const unsigned char*>(in);
    for (std::size_t i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
    runtime_fault_recover = prev;
    return true;
}

const char* SessionRoleName(int role) {
    switch (role) {
    case RoleIdle: return "idle";
    case RoleTryingToHost: return "trying-to-host";
    case RoleFailedToHost: return "failed-to-host";
    case RoleHost: return "host";
    case RoleTryingToJoin: return "trying-to-join";
    case RoleJoinFailed: return "join-failed";
    case RoleClient: return "client";
    case RoleLeaving: return "leaving";
    default: return "?";
    }
}

int LocalSessionRole() {
    if (!Image()) {
        return -1;
    }
    return Field<std::int32_t>(Slot(kSessionManager), 0x124, -1);
}

GameSnapshot ReadGameState(bool call_game) {
    GameSnapshot s;
    if (!Image()) {
        return s;
    }
    s.image = true;
    s.world_chr_man = Slot(kWorldChrMan);
    s.player = Field<std::uint64_t>(s.world_chr_man, 0x60, 0);
    s.session_man = Slot(kSessionManager);
    s.world_up = s.world_chr_man && s.player && s.session_man;
    if (s.session_man) {
        s.session_sub_state = Field<std::int32_t>(s.session_man, 0x120, -1);
        s.session_role = Field<std::int32_t>(s.session_man, 0x124, -1);
        s.matching_status = Field<std::int32_t>(s.session_man, 0x270, -1);
    }
    std::uint8_t requested = 0;
    const std::uint64_t req_at = Guest(kNowLoadingRequested);
    SafeGet(req_at, &requested);
    const std::uint64_t helper = Slot(kNowLoadingHelper);
    s.in_game_frame = helper && (Field<std::uint8_t>(helper, 0x50, 0) || Field<std::uint8_t>(helper, 0x51, 0));
    s.loading = requested && !s.in_game_frame;
    s.game_data_man = Slot(kGameDataMan);
    s.player_rec = Field<std::uint64_t>(s.game_data_man, 8, 0);
    if (s.player_rec) {
        s.insight = Field<std::int32_t>(s.player_rec, kRecInsight, -1);
        s.level = Field<std::int32_t>(s.player_rec, kRecLevel, -1);
        s.echoes = Field<std::int32_t>(s.player_rec, kRecEchoes, -1);
    }
    if (s.player) {
        // PlayerIns_GetBloodMarkMap (0x19046c0): *(*(player + 0x400) + 0x48).
        const std::uint64_t history = Field<std::uint64_t>(s.player, 0x400, 0);
        s.map_id = Field<std::uint32_t>(history, 0x48, 0xffffffffu);
    }
    const std::uint64_t flow = Slot(kNetFlow);
    if (flow) {
        s.online_mode = Field<std::uint8_t>(flow, 0x1590, 0xff);
    }
    if (const std::uint64_t np = Slot(kNpManager)) {
        s.np_online = Field<std::uint8_t>(np, 0xd8, 0xff);
    }
    // The game's own count (0x15bdc20): it asserts (and then reads through NULL) without
    // WorldChrMan or, for its own slot, SprjSessionManager; asked only in the world, not loading.
    const std::uint64_t table = Field<std::uint64_t>(flow, 0x16f8, 0);
    if (call_game && s.world_up && !s.loading && table) {
        using CountFn = std::int32_t(BB_COOP_SYSV*)(std::uint64_t);
        s.cooperators = reinterpret_cast<CountFn>(Guest(kCooperatorCount))(table);
    }
    return s;
}

std::string MapName(std::uint32_t map_id) {
    if (map_id == 0xffffffffu) {
        return "-";
    }
    char text[24];
    std::snprintf(text, sizeof text, "m%02u_%02u_%02u_%02u", (map_id >> 24) & 0xff, (map_id >> 16) & 0xff,
                  (map_id >> 8) & 0xff, map_id & 0xff);
    return text;
}

std::string Describe(const GameSnapshot& s) {
    char text[384];
    std::snprintf(text, sizeof text,
                  "world %s, loading %s, map %s (0x%08x), level %d, Insight %d, session %s (%d, sub %d, matching %d), "
                  "cooperators %d, online mode %d, NP online %d",
                  s.world_up ? "up" : "down", s.loading ? "yes" : "no", MapName(s.map_id).c_str(), s.map_id, s.level,
                  s.insight, SessionRoleName(s.session_role), s.session_role, s.session_sub_state, s.matching_status,
                  s.cooperators, s.online_mode, s.np_online);
    return text;
}

bool WritePlayerInsight(int insight) {
    const std::uint64_t gdm = Slot(kGameDataMan);
    const std::uint64_t rec = Field<std::uint64_t>(gdm, 8, 0);
    if (!rec) {
        return false;
    }
    const std::int32_t v = insight;
    return SafeWrite(rec + kRecInsight, &v, 4);
}

bool WritePlayerLevel(int level) {
    const std::uint64_t rec = Field<std::uint64_t>(Slot(kGameDataMan), 8, 0);
    if (!rec || level < 1) {
        return false;
    }
    const std::int32_t v = level;
    return SafeWrite(rec + kRecLevel, &v, 4);
}

bool WritePlayerEchoes(int echoes) {
    const std::uint64_t rec = Field<std::uint64_t>(Slot(kGameDataMan), 8, 0);
    if (!rec || echoes < 0) {
        return false;
    }
    const std::int32_t v = echoes;
    return SafeWrite(rec + 0x94, &v, 4);
}

// ---- Campaign start ----

bool ReadEventFlag(std::uint32_t flag, bool* value) {
    const std::uint64_t man = Slot(kEventFlagMan);
    if (!man || !GetFlagOk()) {
        return false;
    }
    using Fn = std::uint32_t(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t, std::uint32_t);
    *value = reinterpret_cast<Fn>(Guest(kGetEventFlag))(man, flag, 1) != 0;
    return true;
}

bool ReadEventFlagValue(std::uint32_t first, std::uint32_t bits, std::uint32_t* value) {
    const std::uint64_t man = Slot(kEventFlagMan);
    if (!man || !GetFlagOk() || bits < 1 || bits > 32) {
        return false;
    }
    using Fn = std::uint32_t(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t, std::uint32_t);
    *value = reinterpret_cast<Fn>(Guest(kGetEventFlag))(man, first, bits);
    return true;
}

bool WriteEventFlag(std::uint32_t flag, bool value) {
    const std::uint64_t man = Slot(kEventFlagMan);
    if (!man || !SetFlagOk()) {
        return false;
    }
    using Fn = void(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t, std::uint32_t);
    reinterpret_cast<Fn>(Guest(kSetEventFlag))(man, flag, value ? 1 : 0);
    return true;
}

int GoodsCount(std::uint32_t goods_id) {
    return ItemCount(kTypeGoods, goods_id);
}

namespace {
/// Entry count of item (type_bits | id) in an item list laid out like the inventory (the inventory at
/// GameDataMan rec + 0x328, the storage box at player data + 0x5b0: 0x14D9A80 reads both the same way).
int CountInList(std::uint64_t list, std::uint32_t type_bits, std::uint32_t id) {
    if (!list || !FindOk()) {
        return -1;
    }
    // The lookup reads [list+0x68] (bucket table) and [list+0x78] (chain); both must exist.
    if (!Field<std::uint64_t>(list, 0x68, 0) || !Field<std::uint64_t>(list, 0x78, 0)) {
        return -1;
    }
    using Fn = std::int32_t(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t, std::uint32_t);
    const std::int32_t index = reinterpret_cast<Fn>(Guest(kInventoryFind))(list, type_bits, id);
    if (index < 0) {
        return 0;
    }
    const std::int32_t last = Field<std::int32_t>(list, 0x88, -1);
    const std::uint32_t split = Field<std::uint32_t>(list, 0x24, 0);
    if (last < 0 || std::uint32_t(index) > std::uint32_t(last)) {
        return -1;
    }
    const std::uint64_t entry = std::uint32_t(index) < split
                                    ? Field<std::uint64_t>(list, 0x58, 0) + 16ull * std::uint32_t(index)
                                    : Field<std::uint64_t>(list, 0x48, 0) + 16ull * (std::uint32_t(index) - split);
    std::int32_t handle = 0, count = -1;
    if (!SafeGet(entry, &handle) || !SafeGet(entry + 8, &count)) {
        return -1;
    }
    return handle == 0 ? 0 : count;
}
} // namespace

int ItemCount(std::uint32_t type_bits, std::uint32_t id) {
    const std::uint64_t rec = Field<std::uint64_t>(Slot(kGameDataMan), 8, 0);
    return rec ? CountInList(rec + kRecInventory, type_bits, id) : -1;
}

int StorageItemCount(std::uint32_t type_bits, std::uint32_t id) {
    // The storage box: player (WorldChrMan +0x60) vtable +0x1c8 -> player data, +0x5b0 (as StorageRoom).
    const std::uint64_t player = Field<std::uint64_t>(Slot(kWorldChrMan), 0x60, 0);
    std::uint64_t vt = 0, fn = 0;
    if (!player || !SafeGet(player, &vt) || !SafeGet(vt + 0x1c8, &fn) || !fn) {
        return -1;
    }
    const std::uint64_t data = reinterpret_cast<std::uint64_t(BB_COOP_SYSV*)(std::uint64_t)>(fn)(player);
    const std::uint64_t st = data ? Field<std::uint64_t>(data, 0x5b0, 0) : 0;
    return CountInList(st, type_bits, id);
}

std::vector<std::pair<std::uint32_t, int>> KeySectionItems() {
    std::vector<std::pair<std::uint32_t, int>> out;
    const std::uint64_t rec = Field<std::uint64_t>(Slot(kGameDataMan), 8, 0);
    if (!rec) {
        return out;
    }
    const std::uint64_t list = rec + kRecInventory;
    const std::int32_t last = Field<std::int32_t>(list, 0x88, -1);
    const std::uint32_t split = Field<std::uint32_t>(list, 0x24, 0);
    const std::uint64_t first = Field<std::uint64_t>(list, 0x58, 0);
    if (last < 0 || last > 4096 || split > 4096 || !first) {
        return out;
    }
    for (std::uint32_t i = 0; i < split && i <= std::uint32_t(last); ++i) {
        std::uint32_t handle = 0, item = 0;
        std::int32_t count = 0;
        if (SafeGet(first + 16ull * i, &handle) && handle != 0 && SafeGet(first + 16ull * i + 4, &item) &&
            SafeGet(first + 16ull * i + 8, &count) && count > 0) {
            out.emplace_back(item, count);
        }
    }
    return out;
}

int ItemStacks(std::uint32_t type_bits, std::uint32_t id) {
    const std::uint64_t rec = Field<std::uint64_t>(Slot(kGameDataMan), 8, 0);
    if (!rec) {
        return -1;
    }
    const std::uint64_t list = rec + kRecInventory;
    const std::int32_t last = Field<std::int32_t>(list, 0x88, -1);
    const std::uint32_t split = Field<std::uint32_t>(list, 0x24, 0);
    const std::uint64_t first = Field<std::uint64_t>(list, 0x58, 0), second = Field<std::uint64_t>(list, 0x48, 0);
    if (last < 0 || last > 4096 || split > 4096 || !first || !second) {
        return -1;
    }
    int n = 0;
    for (std::uint32_t i = 0; i <= std::uint32_t(last); ++i) {
        const std::uint64_t entry = i < split ? first + 16ull * i : second + 16ull * (i - split);
        std::uint32_t handle = 0, item = 0;
        if (SafeGet(entry, &handle) && handle != 0 && SafeGet(entry + 4, &item) && item == (type_bits | id)) {
            ++n;
        }
    }
    // The key section not in view (items.md 10.7: the list keeps the saved key section at +0x28
    // and the summoned one at +0x38; the key view +0x58 is one of them). The save
    // writes both, so an entry there is a stack in the save too.
    const std::uint64_t saved = Field<std::uint64_t>(list, 0x28, 0), summoned = Field<std::uint64_t>(list, 0x38, 0);
    const std::uint64_t hidden = first == saved ? summoned : first == summoned ? saved : 0;
    for (std::uint32_t i = 0; hidden && i < split; ++i) {
        std::uint32_t handle = 0, item = 0;
        if (SafeGet(hidden + 16ull * i, &handle) && handle != 0 && SafeGet(hidden + 16ull * i + 4, &item) &&
            item == (type_bits | id)) {
            ++n;
        }
    }
    return n;
}

namespace {
/// The inventory entry's gaitem handle for an item (0 = not held / unknown).
std::uint32_t ItemHandle(std::uint32_t type_bits, std::uint32_t id) {
    const std::uint64_t rec = Field<std::uint64_t>(Slot(kGameDataMan), 8, 0);
    if (!rec || !FindOk()) {
        return 0;
    }
    const std::uint64_t list = rec + kRecInventory;
    if (!Field<std::uint64_t>(list, 0x68, 0) || !Field<std::uint64_t>(list, 0x78, 0)) {
        return 0;
    }
    using Fn = std::int32_t(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t, std::uint32_t);
    const std::int32_t index = reinterpret_cast<Fn>(Guest(kInventoryFind))(list, type_bits, id);
    const std::int32_t last = Field<std::int32_t>(list, 0x88, -1);
    if (index < 0 || last < 0 || index > last) {
        return 0;
    }
    const std::uint32_t split = Field<std::uint32_t>(list, 0x24, 0);
    const std::uint64_t entry = std::uint32_t(index) < split
                                    ? Field<std::uint64_t>(list, 0x58, 0) + 16ull * std::uint32_t(index)
                                    : Field<std::uint64_t>(list, 0x48, 0) + 16ull * (std::uint32_t(index) - split);
    std::uint32_t handle = 0;
    return SafeGet(entry, &handle) ? handle : 0;
}
} // namespace

bool ShowItemGet(std::uint32_t type_bits, std::uint32_t id, int count) {
    const std::uint64_t list = Slot(kItemGetList);
    // 0x16D8850 takes a node from the free list (+0x10) or the tail of the live list (+8); with
    // neither it would write through null.
    if (!list || (!Field<std::uint64_t>(list, 0x10, 0) && !Field<std::uint64_t>(list, 8, 0)) || !ItemGetOk()) {
        return false;
    }
    std::uint32_t handle[2] = {ItemHandle(type_bits, id), 0};
    if (!handle[0]) {
        return false;
    }
    // The give list copies the handle into a wrapper (ref +1) around the call and drops it after
    // (ref -1); ours lives across the call unchanged, the push takes its own reference.
    using Fn = void(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t*, std::uint32_t, std::uint32_t, std::int32_t, std::uint32_t);
    reinterpret_cast<Fn>(Guest(kItemGetPush))(list, handle, type_bits, id & 0x0fffffffu, count, 0);
    return true;
}

bool ShowItemGetNotice(std::uint32_t type_bits, std::uint32_t id, int count) {
    const std::uint64_t list = Slot(kItemGetList);
    if (!list || (!Field<std::uint64_t>(list, 0x10, 0) && !Field<std::uint64_t>(list, 8, 0)) || !ItemGetOk()) {
        return false;
    }
    // The talk scripts' item notice (ESD command 0x41 in 0x203FFD0): a null handle, no inventory entry.
    std::uint32_t handle[2] = {0, 0};
    using Fn = void(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t*, std::uint32_t, std::uint32_t, std::int32_t, std::uint32_t);
    reinterpret_cast<Fn>(Guest(kItemGetPush))(list, handle, type_bits, id & 0x0fffffffu, count, 0);
    return true;
}

int ItemGetEntries() {
    const std::uint64_t list = Slot(kItemGetList);
    if (!list) {
        return -1;
    }
    int n = 0;
    std::uint64_t node = Field<std::uint64_t>(list, 8, 0);
    while (node && n < 64) {
        ++n;
        node = Field<std::uint64_t>(node, 0, 0);
    }
    return n;
}

bool ItemGetConfirmArmed() {
    // 0x16D86E0: the confirm check runs unless the player chr's flag +0x41 bit 7 is set, and only
    // with the action-button manager *0x5540050.
    const std::uint64_t c = Field<std::uint64_t>(
        Field<std::uint64_t>(Field<std::uint64_t>(Slot(kWorldChrMan), 0x60, 0), 0x58, 0), 0x38, 0);
    const bool busy = c && (Field<std::uint8_t>(c, 0x41, 0) & 0x80) != 0;
    return !busy && Slot(0x5540050) != 0;
}

int ClearItemGet() {
    constexpr std::uint64_t kRelease = 0x1a8bc00; // void (u32* gaitem handle): reference -1
    constexpr std::uint64_t kMenuState = 0x5562878; // +0x130: the dialog is up
    static int st = 0;
    const std::uint64_t list = Slot(kItemGetList);
    if (!list || !CodeOk(kRelease, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb, 0x8b, 0x33},
                         "gaitem release", &st)) {
        return -1;
    }
    // The confirm path of 0x16D86E0: every live node's handle released, the node back on the free
    // list (+0x10), the live list (+8) emptied, then the "dialog up" word cleared.
    using Fn = void(BB_COOP_SYSV*)(std::uint64_t);
    int n = 0;
    std::uint64_t node = Field<std::uint64_t>(list, 8, 0);
    while (node && n < 64) {
        const std::uint64_t next = Field<std::uint64_t>(node, 0, 0);
        reinterpret_cast<Fn>(Guest(kRelease))(node + 8);
        const std::uint64_t free_head = Field<std::uint64_t>(list, 0x10, 0);
        SafeWrite(node, &free_head, sizeof free_head);
        SafeWrite(list + 0x10, &node, sizeof node);
        node = next;
        ++n;
    }
    const std::uint64_t zero = 0;
    SafeWrite(list + 8, &zero, sizeof zero);
    if (const std::uint64_t menu = Slot(kMenuState)) {
        const std::uint32_t down = 0;
        SafeWrite(menu + 0x130, &down, sizeof down);
    }
    return n;
}

StartFlags ReadStartFlags() {
    StartFlags f;
    struct Want {
        std::uint32_t id;
        bool* out;
    };
    const Want want[] = {
        {kFlagOpeningDone, &f.opening_done}, {kFlagCutscenePending, &f.cutscene},
        {kFlagFirstDream, &f.first_dream},   {kFlagFirstDeathToBase, &f.first_death},
        {kFlagWeaponRight, &f.weapon_right}, {kFlagWeaponLeft, &f.weapon_left},
        {kFlagDollAwake, &f.doll_awake},     {kFlagBeckoningLot, &f.beckoning_lot},
        {kFlagResonantShop, &f.resonant_shop},
    };
    f.flags_ok = true;
    for (const Want& w : want) {
        if (!ReadEventFlag(w.id, w.out)) {
            f.flags_ok = false;
            break;
        }
    }
    {
        const std::uint32_t ng = Field<std::uint32_t>(Slot(kGameDataMan), 0x68, 0u);  // GameDataMan +0x68: NG cycle
        f.ng_plus = ng > 0 && ng <= 7;
    }
    f.beckoning_count = GoodsCount(kGoodsBeckoning);
    f.resonant_count = GoodsCount(kGoodsSmallResonant);
    return f;
}

bool AwardItemLot(std::uint32_t lot) {
    const std::uint64_t menu = Slot(kItemGetMenu);
    const std::uint64_t rec = Field<std::uint64_t>(Slot(kGameDataMan), 8, 0);
    if (!menu || !rec || !Slot(kEventFlagMan) || !Slot(kSessionManager) || !AwardOk()) {
        return false;
    }
    using Fn = void(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t, std::uint32_t);
    reinterpret_cast<Fn>(Guest(kAwardItemLot))(menu, lot, 0);
    return true;
}

bool GiveGoods(std::uint32_t goods_id, int count) {
    return GiveItem(kTypeGoods, goods_id, count);
}

bool GiveItem(std::uint32_t type_bits, std::uint32_t id, int count) {
    const std::uint64_t rec = Field<std::uint64_t>(Slot(kGameDataMan), 8, 0);
    if (!rec || !Slot(kItemGetMenu) || !GiveOk()) {
        return false;
    }
    using Fn = void(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t, std::uint32_t, std::int32_t);
    reinterpret_cast<Fn>(Guest(kGiveItemDirect))(0, type_bits, id, count);
    return true;
}

// The game's own equip of one slot (EquipGameData = PlayerGameData +0x1D0): (equip, slot, &gaitem
// handle, inventory index, force) - the handle by the refcounted assign (0x1A8BD80, never a plain
// write: the GaItem counts), the item id from the handle, the index, the slot's changed flag, and
// the same into the chr's ChrAsm. Record: handle +0x264 + slot * 4, id +0x2A4 + slot * 4.
constexpr std::uint64_t kSetEquipSlot = 0x14ccb90;
constexpr std::uint64_t kRecEquip = 0x1d0, kRecEquipHandle = 0x264, kRecEquipId = 0x2a4;

bool EquipSlotItem(int slot, std::uint32_t type_bits, std::uint32_t id) {
    static const bool ok = MatchesOriginal(kSetEquipSlot, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                                           0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28, 0x89, 0xcb});
    if (!ok || slot < 0 || slot > 31) return false;
    if (ItemCount(type_bits, id) <= 0) GiveItem(type_bits, id, 1);
    const std::uint64_t rec = Field<std::uint64_t>(Slot(kGameDataMan), 8, 0);
    if (!rec || !FindOk()) return false;
    const std::uint64_t list = rec + kRecInventory;
    if (!Field<std::uint64_t>(list, 0x68, 0) || !Field<std::uint64_t>(list, 0x78, 0)) return false;
    using FindFn = std::int32_t(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t, std::uint32_t);
    const std::int32_t index = reinterpret_cast<FindFn>(Guest(kInventoryFind))(list, type_bits, id);
    const std::int32_t last = Field<std::int32_t>(list, 0x88, -1);
    if (index < 0 || index > last) return false;
    const std::uint32_t split = Field<std::uint32_t>(list, 0x24, 0);
    const std::uint32_t i = static_cast<std::uint32_t>(index);
    const std::uint64_t entry = i < split ? Field<std::uint64_t>(list, 0x58, 0) + 16ull * i
                                          : Field<std::uint64_t>(list, 0x48, 0) + 16ull * (i - split);
    std::uint32_t handle = 0;
    if (!SafeGet(entry, &handle) || !handle) return false;
    using SetFn = void(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t, std::uint32_t*, std::uint32_t, std::uint32_t);
    reinterpret_cast<SetFn>(Guest(kSetEquipSlot))(rec + kRecEquip, static_cast<std::uint32_t>(slot), &handle, i, 1);
    return Field<std::uint32_t>(rec, kRecEquipHandle + static_cast<std::uint64_t>(slot) * 4, 0) == handle;
}

std::uint32_t EquippedSlotId(int slot) {
    const std::uint64_t rec = Field<std::uint64_t>(Slot(kGameDataMan), 8, 0);
    return slot < 0 || slot > 31 ? 0 : Field<std::uint32_t>(rec, kRecEquipId + static_cast<std::uint64_t>(slot) * 4, 0);
}

constexpr std::uint64_t kQuickNormalise = 0x14cf920;  // (EquipGameData): the selection made valid
constexpr std::uint64_t kRecQuickIndex = 0x3ec, kRecQuickSelected = 0x418;

namespace {
std::int32_t InventoryIndex(std::uint64_t rec, std::uint32_t type_bits, std::uint32_t id) {
    const std::uint64_t list = rec + kRecInventory;
    if (!FindOk() || !Field<std::uint64_t>(list, 0x68, 0) || !Field<std::uint64_t>(list, 0x78, 0)) return -1;
    using FindFn = std::int32_t(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t, std::uint32_t);
    const std::int32_t index = reinterpret_cast<FindFn>(Guest(kInventoryFind))(list, type_bits, id);
    return index <= Field<std::int32_t>(list, 0x88, -1) ? index : -1;
}

/// The item id of inventory entry `index` (the entry's GaItem handle's item, through the list).
std::uint32_t InventoryItemId(std::uint64_t rec, std::int32_t index) {
    const std::uint64_t list = rec + kRecInventory;
    if (index < 0 || index > Field<std::int32_t>(list, 0x88, -1)) return 0;
    const std::uint32_t split = Field<std::uint32_t>(list, 0x24, 0);
    const std::uint32_t i = static_cast<std::uint32_t>(index);
    const std::uint64_t entry = i < split ? Field<std::uint64_t>(list, 0x58, 0) + 16ull * i
                                          : Field<std::uint64_t>(list, 0x48, 0) + 16ull * (i - split);
    return Field<std::uint32_t>(entry, 4, 0) & 0x0fffffffu;  // +0 handle, +4 type bits | id
}
}  // namespace

bool SetQuickItem(int slot, std::uint32_t id, bool select) {
    static const bool ok = MatchesOriginal(kQuickNormalise, {0x8b, 0x87, 0x48, 0x02, 0x00, 0x00, 0x83, 0xf8, 0xff});
    if (!ok || slot < 0 || slot > 5) return false;
    if (ItemCount(kItemGoods, id) <= 0) GiveItem(kItemGoods, id, 1);
    const std::uint64_t rec = Field<std::uint64_t>(Slot(kGameDataMan), 8, 0);
    if (!rec) return false;
    const std::int32_t index = InventoryIndex(rec, kItemGoods, id);
    if (index < 0) return false;
    if (!SafeWrite(rec + kRecQuickIndex + 8ull * static_cast<std::uint32_t>(slot), &index, 4)) return false;
    if (select) SafeWrite(rec + kRecQuickSelected, &index, 4);
    reinterpret_cast<void(BB_COOP_SYSV*)(std::uint64_t)>(Guest(kQuickNormalise))(rec + kRecEquip);
    return Field<std::int32_t>(rec, kRecQuickIndex + 8ull * static_cast<std::uint32_t>(slot), -1) == index &&
           (!select || Field<std::int32_t>(rec, kRecQuickSelected, -1) == index);
}

std::uint32_t QuickItemId(int slot, bool* selected) {
    if (selected) *selected = false;
    const std::uint64_t rec = Field<std::uint64_t>(Slot(kGameDataMan), 8, 0);
    if (!rec || slot < 0 || slot > 5) return 0;
    const std::int32_t index = Field<std::int32_t>(rec, kRecQuickIndex + 8ull * static_cast<std::uint32_t>(slot), -1);
    if (selected) *selected = index >= 0 && Field<std::int32_t>(rec, kRecQuickSelected, -1) == index;
    return InventoryItemId(rec, index);
}

bool CutscenePlaying() {
    const std::uint64_t player = Field<std::uint64_t>(Slot(kSprjRemo), 8, 0);
    return (Field<std::uint8_t>(player, 0x168, 0) & 1) != 0;
}

bool RequestRemoSkip(bool* skippable) {
    const std::uint64_t remo = Slot(kSprjRemo);
    const std::uint64_t player = Field<std::uint64_t>(remo, 8, 0);
    const std::uint8_t bits = Field<std::uint8_t>(player, 0x168, 0);
    if (skippable) {
        *skippable = (bits & 2) != 0;
    }
    if (!remo || !player || !RemoSkipOk()) {
        return false;
    }
    std::int32_t any = -1;
    using Fn = std::uint64_t(BB_COOP_SYSV*)(std::uint64_t, std::int32_t*);
    reinterpret_cast<Fn>(Guest(kRemoSkip))(remo, &any);
    return true;
}

bool EndRemoNow(bool* skippable) {
    const std::uint64_t player = Field<std::uint64_t>(Slot(kSprjRemo), 8, 0);
    const std::uint8_t bits = Field<std::uint8_t>(player, 0x168, 0);
    if (skippable) {
        *skippable = (bits & 2) != 0;
    }
    if (!player || !(bits & 1) || !RemoSkipOk()) {
        return false;
    }
    RequestRemoSkip();
    const std::uint8_t now = Field<std::uint8_t>(player, 0x168, 0);
    if (now & 1) {
        const std::uint8_t set = static_cast<std::uint8_t>(now | 0x20);
        SafeWrite(player + 0x168, &set, 1);
    }
    return true;
}

namespace {
std::atomic<bool> g_save_pending{false};
std::mutex g_save_mu;
std::string g_save_why; // under g_save_mu
double g_save_last = -1e9;
int g_save_steady = 0;
} // namespace

void RequestGameSave(const char* why) {
    std::lock_guard<std::mutex> lk(g_save_mu);
    if (why && g_save_why.find(why) == std::string::npos) {
        g_save_why += (g_save_why.empty() ? "" : ", ") + std::string(why);
    }
    g_save_pending = true;
}

void GameSaveTick() {
    const GameSnapshot s = ReadGameState(false);
    g_save_steady = s.world_up && !s.loading ? g_save_steady + 1 : 0;
    if (!g_save_pending.load() || g_save_steady < 30) {
        return;
    }
    const double now =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - g_save_last < 15.0) {
        return;
    }
    std::string why;
    {
        std::lock_guard<std::mutex> lk(g_save_mu);
        why.swap(g_save_why);
        g_save_pending = false;
    }
    g_save_last = now;
    const std::uint64_t gsm = Slot(kNetFlow); // GSM (WorldTransitionState)
    if (!gsm || !Slot(kLuaEventMan) || !SaveRequestOk()) {
        coop::asyncio::Print("Party: save request (" + why + ") not possible (no game state / code differs)\n");
        return;
    }
    const std::uint8_t allowed = Field<std::uint8_t>(gsm, 0x1578, 0);
    using Fn = void(BB_COOP_SYSV*)();
    reinterpret_cast<Fn>(Guest(kSaveRequest))();
    coop::asyncio::Print("Party: save requested (" + why + "): save allowed " + std::to_string(allowed) +
                         ", request " + std::to_string(Field<std::uint8_t>(gsm, 0x1548, 0)) + "\n");
}

bool LampWarp(std::uint32_t return_point) {
    if (!Field<std::uint64_t>(Slot(kWorldChrMan), 0x60, 0) || !LampWarpOk()) {
        return false;
    }
    using Fn = void(BB_COOP_SYSV*)(std::uint32_t);
    reinterpret_cast<Fn>(Guest(kLampWarp))(return_point);
    return true;
}

} // namespace coop
