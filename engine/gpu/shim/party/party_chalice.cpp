// SPDX-License-Identifier: GPL-3.0-or-later
// Chalice Dungeons in party play: the local game's dungeon (party_chalice.h).
#include "party_chalice.h"

#include "net/json.h"

#include <algorithm>
#include <iterator>

namespace coop::chalice {

bool IsChaliceGoods(std::uint32_t goods_id) {
    return std::binary_search(std::begin(kChaliceGoods), std::end(kChaliceGoods), goods_id);
}

std::string GoodsToJsonText(const std::vector<std::uint32_t>& ids) {
    json::Value v = json::Value::make_object();
    json::Value a = json::Value::make_array();
    for (std::uint32_t id : ids) {
        a.push(id);
    }
    v.set("goods", a);
    return json::dump(v, 0);
}

bool GoodsFromJsonText(const std::string& text, std::vector<std::uint32_t>* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err) || v.type != json::Value::Type::Object) {
        if (error) *error = err.empty() ? "not an object" : err;
        return false;
    }
    const json::Value* a = v.find("goods");
    if (!a || a->type != json::Value::Type::Array || a->array.size() > 64) {
        if (error) *error = "goods: not an array of at most 64";
        return false;
    }
    out->clear();
    for (const json::Value& x : a->array) {
        if (x.type == json::Value::Type::Number && x.number >= 0 && x.number < 100000 &&
            IsChaliceGoods(static_cast<std::uint32_t>(x.number)) &&
            std::find(out->begin(), out->end(), static_cast<std::uint32_t>(x.number)) == out->end()) {
            out->push_back(static_cast<std::uint32_t>(x.number));
        }
    }
    return true;
}

} // namespace coop::chalice

#ifndef BB_PARTY_CHALICE_NO_GAME
#include "coop_hooks.h"
#include "game_state.h"

#include "../bbport_game_menu.h"
#include "../net/from_api.h"
#include "../net/party_util.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace coop::chalice {

namespace {

constexpr std::uint64_t kEventFlagManSlot = 0x553b100;
constexpr std::uint64_t kLoadModeOff = 0x80;

std::atomic<std::uint64_t> g_local{0};
std::mutex g_mu;
std::vector<std::uint32_t> g_host_goods, g_host_pending, g_guest_goods;
bool g_host_dirty = false, g_guest_dirty = false;
std::string g_form_pending;      // guest: {"id":"N","form":"b64"} for the host's board
double g_host_sent_at = 0;

// The party's dungeon (host): the newest ritual of another member, offered at the host's altar as
// "Enter the party's dungeon" (menu item 262002 "Search by Chalice Glyph" renamed, its glyph
// confirmed in the ImeDialog without typing).
constexpr std::uint32_t kGlyphSearchText = 262002;
constexpr std::uint32_t kGlyphLength = 8;
std::string g_host_name;
char g_party_glyph[16] = {};               // "" when none (read by the ImeDialog)
char16_t g_party_label[64] = {};           // the altar menu item
char16_t g_party_help[160] = {};           // its help line
std::atomic<bool> g_party_dungeon{false};
std::atomic<bool> g_altar_auto{false};     // the glyph box took the party glyph: walk the rest

void CopyU16(char16_t* out, std::size_t cap, const std::string& text) {
    std::size_t n = 0;
    for (; n + 1 < cap && n < text.size(); ++n) out[n] = static_cast<unsigned char>(text[n]);
    out[n] = 0;
}

const char16_t* AltarText(std::uint32_t, std::uint32_t id, const char16_t* original) {
    if (id != kGlyphSearchText || !g_party_dungeon.load(std::memory_order_acquire) || !original) {
        return nullptr;
    }
    // The same id names the menu item (short) and its help line (long), in two message tables.
    std::size_t len = 0;
    while (original[len] && len < 200) ++len;
    return len <= 40 ? g_party_label : g_party_help;
}

const char* PartyGlyph(std::uint32_t max_length) {
    if (max_length != kGlyphLength || !g_party_dungeon.load(std::memory_order_acquire)) {
        return nullptr;
    }
    // Only for the glyph box the altar opened (its menu shown within the last 20 s).
    const std::int64_t seen = BbGameMenu::LastLookupMs(kGlyphSearchText);
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    if (!seen || now - seen > 20000) {
        return nullptr;
    }
    std::printf("Party: chalice: the altar's glyph box gets the party dungeon's glyph %s\n", g_party_glyph);
    std::fflush(stdout);
    g_altar_auto.store(true, std::memory_order_release);
    return g_party_glyph;
}

double Seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

// A summon message as a guest's game receives it (0x14ba340(SSM, ?, wrapper); wrapper +8 -> message):
// +1 type (1 = invited, 2 = ...), +4/+8 ids, +0x10 the host's map (FrpgNetMan +0xa7c), +0x2c the
// host's ChannelId (FrpgNetMan +0xa88 when the map is in area 29, else 0x8000000000000000; the
// sender 0x14b8625), +0x34 room data size. A ChannelId makes the guest fetch and quick-join it.
BB_COOP_SYSV void SummonMessageProbe(u64, u64, u64 wrapper, u64, u64, u64) {
    std::uint64_t msg = 0;
    if (!wrapper || !SafeGet(wrapper + 8, &msg) || !msg) {
        return;
    }
    std::uint8_t type = 0;
    std::uint32_t map = 0, size = 0;
    std::uint64_t channel = 0;
    SafeGet(msg + 1, &type);
    SafeGet(msg + 0x10, &map);
    SafeGet(msg + 0x2c, &channel);
    SafeGet(msg + 0x34, &size);
    // The room data's head (0x1872360 reads u32 -> GSM +0x16f0, u32 (low byte) -> GSM +0x16f4,
    // then the session descriptor 0xc90dd0): logged for the title join (party_b2 TitleJoin).
    std::uint8_t head[24] = {};
    char hex[sizeof head * 2 + 1] = {};
    if (size >= sizeof head && SafeRead(msg + 0x38, head, sizeof head)) {
        for (std::size_t i = 0; i < sizeof head; ++i) {
            std::snprintf(hex + i * 2, 3, "%02x", head[i]);
        }
    }
    std::printf("Party: chalice: summon message type %u, host map 0x%08x, ChannelId 0x%llx, room data %u bytes (head %s)\n",
                type, map, static_cast<unsigned long long>(channel), size, hex);
    std::fflush(stdout);
}

namespace {
constexpr std::uint32_t kRecordBits = 1376; // +992 name 0x30 bytes ends here
std::atomic<double> g_quick_join_at{0};
void* g_set_load_mode = nullptr;
} // namespace

// The ritual helper's steps around a summoned member's quick join (step table 0x1aaa8c0):
// request 6 (0x1aa5280) -> STEP_Exec_forQuickJoin 0x1aa81d0 (the fetched channel -> the helper's
// slot record), request 7 -> STEP_Exec_forCancel 0x1aa8410 (the slot record reset to empty).
namespace {
void LogHelperStep(const char* what, u64 helper) {
    std::uint32_t slot = 0xffffffffu, state = 0, busy = 0;
    std::uint64_t ch = 0, id = 0;
    SafeGet(helper + 0xc8, &slot);
    SafeGet(helper + 0xd0, &ch);
    SafeGet(helper + 0x25c, &state);
    SafeGet(helper + 0x264, &busy);
    if (ch) SafeGet(ch + 0x10, &id);
    std::uint32_t bits = 0;
    ReadEventFlagValue(9020, 7, &bits);
    std::printf("Party: chalice: helper %s: slot %u (current slots 0x%x), state %u, busy %u, channel object %s (id 0x%llx)\n",
                what, slot, bits, state, busy, ch ? "set" : "none", static_cast<unsigned long long>(id));
    std::fflush(stdout);
}
} // namespace

BB_COOP_SYSV void QuickJoinRequestProbe(u64 helper, u64 channel_at, u64, u64, u64, u64) {
    std::uint64_t want = 0, net = 0;
    std::uint8_t on = 0, b9f6 = 0, b9f8 = 0, ba50 = 0;
    SafeGet(channel_at, &want);
    const std::uint64_t at = Guest(0x553b120);
    if (at && SafeGet(at, &net) && net) {
        SafeGet(net + 8, &on);
        SafeGet(net + 0x9f6, &b9f6);
        SafeGet(net + 0x9f8, &b9f8);
        SafeGet(net + 0xa50, &ba50);
    }
    std::printf("Party: chalice: quick join request for 0x%llx (net online %u, 9f6 %u, 9f8 %u, a50 %u)\n",
                static_cast<unsigned long long>(want), on, b9f6, b9f8, ba50);
    LogHelperStep("quick join request", helper);
}

BB_COOP_SYSV void QuickJoinStepProbe(u64 helper, u64, u64, u64, u64, u64) {
    g_quick_join_at.store(Seconds());
    LogHelperStep("STEP_Exec_forQuickJoin", helper);
}

BB_COOP_SYSV void CancelStepProbe(u64 helper, u64, u64, u64, u64, u64) {
    LogHelperStep("STEP_Exec_forCancel", helper);
}

// A summoned member's game quick-joins the host's dungeon into its slot record (0x1aa81d0), then
// the session load switches its event flags to load mode 1 (SetLoadMode 0x13bb590: the flag groups
// move onto other storage, only the slot bit 9020+k is carried over). The map load (0x1928680) then
// reads the slot record's dungeon map (+96) from that storage, finds none and loads m255 (live: the
// guest fell through an empty world). So the record is carried over too: snapshotted before the
// switch, written into the load-mode-1 storage after it — transient, never the member's save.

BB_COOP_SYSV void SetLoadModeHook(u64 man, u64 mode) {
    using Fn = void(BB_COOP_SYSV*)(u64, u64);
    using Get = std::uint32_t(BB_COOP_SYSV*)(u64, std::uint32_t, std::uint32_t);
    using Set = void(BB_COOP_SYSV*)(u64, std::uint32_t, std::uint32_t, std::uint32_t);
    std::int32_t cur = -1;
    const double joined = g_quick_join_at.load();
    const bool carry = static_cast<std::uint32_t>(mode) == 1 && man && SafeGet(man + kLoadModeOff, &cur) && cur == 0 &&
                       joined > 0 && Seconds() - joined < 180;
    std::uint32_t rec[kRecordBits / 32] = {};
    int slot = -1;
    if (carry) {
        const Get get = reinterpret_cast<Get>(Guest(0x13cfd80));
        slot = SlotFromBits(get(man, kSlotFlag, kSlots));
        if (slot >= 0) {
            const std::uint32_t base = kRecordBase + static_cast<std::uint32_t>(slot) * kRecordStride;
            for (std::uint32_t i = 0; i < kRecordBits / 32; ++i) rec[i] = get(man, base + 32 * i, 32);
        }
    }
    reinterpret_cast<Fn>(g_set_load_mode)(man, mode);
    if (!carry || slot < 0) {
        return;
    }
    g_quick_join_at.store(0);
    if ((rec[3] & 0xff000000u) != 0x1d000000u) {
        std::printf("Party: chalice: load mode 1: slot %d's record holds no dungeon (map 0x%08x): not carried\n", slot, rec[3]);
        std::fflush(stdout);
        return;
    }
    const Get get = reinterpret_cast<Get>(Guest(0x13cfd80));
    const Set set = reinterpret_cast<Set>(Guest(0x13d0060));
    const std::uint32_t base = kRecordBase + static_cast<std::uint32_t>(slot) * kRecordStride;
    const std::uint32_t before = get(man, base + 96, 32);
    for (std::uint32_t i = 0; i < kRecordBits / 32; ++i) set(man, base + 32 * i, 32, rec[i]);
    const std::uint32_t after = get(man, base + 96, 32);
    std::printf("Party: chalice: load mode 1: slot %d's record (dungeon %llu, map 0x%08x) carried over (map read 0x%08x -> 0x%08x)\n",
                slot, static_cast<unsigned long long>((static_cast<std::uint64_t>(rec[2]) << 32) | rec[1]), rec[3],
                before, after);
    std::fflush(stdout);
}

void InstallProbes() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    BbGameMenu::SetTextOverride(&AltarText);
    BbGameMenu::SetImePreset(&PartyGlyph);
    HookPrologue(0x14ba340, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &SummonMessageProbe, "chalice: summon message probe");
    HookPrologue(0x1aa5280, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &QuickJoinRequestProbe, "chalice: quick join request probe");
    HookPrologue(0x1aa81d0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &QuickJoinStepProbe, "chalice: quick join step probe");
    HookPrologue(0x1aa8410, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &CancelStepProbe, "chalice: cancel step probe");
    ReplacePrologue(0x13bb590, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, reinterpret_cast<const void*>(&SetLoadModeHook),
                    &g_set_load_mode, "chalice: carry the slot record into load mode 1");
}

namespace {

// SprjHolygrail (0x553e9e8) +8 -> obj[7] (SprjDungeonGateIns); +0x108 its CSDungeonRitualHelper:
// +0xc8 slot, +0xd0 channel object (= obj +0x1d8, what the map load reads), +0xd8 ChannelId,
// +0xe0 u32, +0xe8 chalice goods id, +0xf8 18 u32 layout, +0x140 16-byte online id (0x1a9c490).
std::uint64_t HelperOf(int slot) {
    std::uint64_t grail = 0, arr = 0, obj = 0;
    const std::uint64_t at = Guest(0x553e9e8);
    if (slot < 0 || slot >= kSlots || !at || !SafeGet(at, &grail) || grail < 0x10000 ||
        !SafeGet(grail + 8, &arr) || arr < 0x10000 || !SafeGet(arr + 8 * slot, &obj) || obj < 0x10000) {
        return 0;
    }
    return obj + 0x108;
}

bool HelperFields(int slot, std::uint32_t out[0x1e]) {
    const std::uint64_t h = HelperOf(slot);
    if (!h) return false;
    // FormData v4 words: {4, 0x7c, +0xe0, +0xe8, +0xf8 x18, +0x140 x4}
    out[0] = 4;
    out[1] = 0x7c;
    bool ok = SafeGet(h + 0xe0, &out[2]) && SafeGet(h + 0xe8, &out[3]);
    for (int i = 0; ok && i < 18; ++i) ok = SafeGet(h + 0xf8 + 4 * i, &out[4 + i]);
    for (int i = 0; ok && i < 4; ++i) ok = SafeGet(h + 0x140 + 4 * i, &out[22 + i]);
    return ok;
}

std::string HelperDump(int slot) {
    const std::uint64_t h = HelperOf(slot);
    if (!h) return "-";
    std::uint64_t ch_obj = 0, id = 0;
    std::uint32_t hs = 0, mode = 0;
    SafeGet(h + 0xc8, &hs);
    SafeGet(h + 0xd0, &ch_obj);
    SafeGet(h + 0xd8, &id);
    SafeGet(h - 0x108 + 0x364, &mode);
    std::string s = "slot " + std::to_string(hs) + ", gate mode " + std::to_string(mode) + ", channel object " +
                    (ch_obj ? "set" : "none") + ", id " + std::to_string(id) + ", fields";
    std::uint32_t w[0x1e] = {};
    if (HelperFields(slot, w)) {
        char b[16];
        for (int i = 2; i < 26; ++i) {
            std::snprintf(b, sizeof b, " %x", w[i]);
            s += b;
        }
    }
    return s;
}

} // namespace

std::vector<std::uint8_t> SlotFormData(int slot) {
    // The live helper first (what the game itself would upload, 0x1a9a8f0), else the slot record.
    {
        std::uint32_t w[0x1f] = {};
        if (HelperFields(slot, w) && w[3] != 0) {
            std::vector<std::uint8_t> out(0x7c);
            std::memcpy(out.data(), w, 0x7c);
            return out;
        }
    }
    std::vector<std::uint8_t> out;
    if (slot < 0 || slot >= kSlots) {
        return out;
    }
    const std::uint32_t base = kRecordBase + static_cast<std::uint32_t>(slot) * kRecordStride;
    std::vector<std::uint32_t> words = {4, 0x7c};
    auto get = [&](std::uint32_t bit) {
        std::uint32_t v = 0;
        if (!ReadEventFlagValue(base + bit, 32, &v)) {
            words.clear();
        }
        return v;
    };
    std::uint32_t e0 = get(96), e8 = get(128);
    if (words.empty()) {
        return out;
    }
    words.push_back(e0);
    words.push_back(e8);
    for (int i = 0; i < 18 && !words.empty(); ++i) {
        const std::uint32_t v = get(160 + 32 * i);
        if (!words.empty()) words.push_back(v);
    }
    for (int i = 0; i < 4 && !words.empty(); ++i) {
        const std::uint32_t v = get(736 + 32 * i);
        if (!words.empty()) words.push_back(v);
    }
    if (words.empty()) {
        return out;
    }
    while (words.size() < 0x7c / 4) {
        words.push_back(0);
    }
    out.resize(0x7c);
    std::memcpy(out.data(), words.data(), 0x7c);
    return out;
}

State Read() {
    State s;
    std::uint64_t man = 0;
    const std::uint64_t slot_at = Guest(kEventFlagManSlot);
    if (!slot_at || !SafeGet(slot_at, &man) || man < 0x10000) {
        return s;
    }
    std::int32_t mode = -1;
    if (SafeGet(man + kLoadModeOff, &mode)) {
        s.load_mode = mode;
    }
    std::uint32_t bits = 0;
    if (!ReadEventFlagValue(kSlotFlag, kSlots, &bits)) {
        return s;
    }
    s.valid = true;
    s.slot = SlotFromBits(bits);
    if (s.slot >= 0) {
        const std::uint32_t base = kRecordBase + static_cast<std::uint32_t>(s.slot) * kRecordStride;
        std::uint32_t lo = 0, hi = 0x80000000u;
        if (ReadEventFlagValue(base + kChannelLoBit, 32, &lo) && ReadEventFlagValue(base + kChannelHiBit, 32, &hi)) {
            s.channel = ChannelFromDwords(lo, hi);
        }
    }
    return s;
}

void Tick(bool host) {
    static double next = 0;
    static State last;
    const double now = Seconds();
    if (now < next) {
        return;
    }
    next = now + 2.0;
    const GameSnapshot g = ReadGameState(false);
    if (!g.world_up || g.loading) {
        return;
    }
    const State s = Read();
    if (!s.valid) {
        return;
    }
    {
        // The game's current ChannelId (FrpgNetMan +0xa88: what summon messages and signs carry).
        static std::uint64_t last_net_channel = 1;
        std::uint64_t net = 0, ch = 0;
        const std::uint64_t at = Guest(0x553b120);
        if (at && SafeGet(at, &net) && net && SafeGet(net + 0xa88, &ch) && ch != last_net_channel) {
            last_net_channel = ch;
            std::printf("Party: chalice: FrpgNetMan ChannelId 0x%llx\n", static_cast<unsigned long long>(ch));
            std::fflush(stdout);
        }
    }
    if (host) {
        // The party's dungeon: another member's newest ritual (within an hour) that the host's
        // current slot does not hold already.
        bbnet::party::ChannelBoard::Channel c;
        std::string member, name;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            name = g_host_name;
        }
        const bool any = !name.empty() &&
                         bbnet::party::FromApi::instance().channels().latest_ritual(name, std::chrono::hours(1), &c, &member) &&
                         c.glyph.size() == kGlyphLength && !(s.load_mode == 0 && s.slot >= 0 && s.channel == c.id);
        static std::string shown;
        const std::string key = any ? c.glyph + "/" + member : "";
        if (key != shown) {
            shown = key;
            g_party_dungeon.store(false, std::memory_order_release);
            if (any) {
                std::snprintf(g_party_glyph, sizeof g_party_glyph, "%s", c.glyph.c_str());
                CopyU16(g_party_label, sizeof g_party_label / sizeof g_party_label[0], "Enter the party's dungeon");
                CopyU16(g_party_help, sizeof g_party_help / sizeof g_party_help[0],
                        "Enter the Chalice Dungeon " + member + " made (glyph " + c.glyph + ") with the party.");
                g_party_dungeon.store(true, std::memory_order_release);
            }
            std::printf("Party: chalice: party dungeon at the altar: %s\n",
                        any ? (std::to_string(c.id) + " by " + member + ", glyph " + c.glyph).c_str() : "none");
            std::fflush(stdout);
        }
    }
    if (!host && s.slot >= 0 && s.channel && (s.load_mode == 0 || s.slot != 0)) {
        // A guest's own ritual dungeon: its FormData for the host's board (a shared fixed dungeon is
        // never uploaded; the host's glyph join and a summon need it). Once per dungeon and minute.
        // Also while summoned (load mode 1) from its own ritual slot (1..): a guest that title-joined
        // never stands in its own world (live g1b: no FormData, no party dungeon at the host's
        // altar); slot 0 in load mode 1 is the host's quick-joined dungeon, not the guest's.
        static std::uint64_t sent_for = 0;
        static double sent_at = -100;
        if (sent_for != s.channel || now - sent_at >= 60.0) {
            const std::vector<std::uint8_t> fd = SlotFormData(s.slot);
            if (!fd.empty()) {
                const std::string b64 = bbnet::party::b64_encode(fd.data(), fd.size());
                std::lock_guard<std::mutex> lk(g_mu);
                g_form_pending = "{" + std::string("\"id\":\"") + std::to_string(s.channel) + "\",\"form\":\"" + b64 + "\"}";
                if (sent_for != s.channel) {
                    std::printf("Party: chalice: dungeon %llu: FormData of slot %d (%zu bytes) for the host's board\n",
                                static_cast<unsigned long long>(s.channel), s.slot, fd.size());
                    std::fflush(stdout);
                }
                sent_for = s.channel;
                sent_at = now;
            }
        }
    }
    if (host && s.load_mode == 0 && s.slot >= 0 && s.channel) {
        // The host's dungeon on the channel board with its FormData: a shared fixed dungeon was
        // joined through search and never uploaded, so the board's entry had none and a summoned
        // member's quick join built nothing (live: the guest fell through an empty map).
        static std::uint64_t done_for = 0;
        if (done_for != s.channel) {
            bbnet::party::ChannelBoard& board = bbnet::party::FromApi::instance().channels();
            if (!board.known(s.channel)) {
                // Entered from an earlier session's ritual (the board is new each session).
                const std::vector<std::uint8_t> fd = SlotFormData(s.slot);
                std::string name;
                {
                    std::lock_guard<std::mutex> lk(g_mu);
                    name = g_host_name;
                }
                if (!fd.empty()) {
                    board.adopt(s.channel, bbnet::party::b64_encode(fd.data(), fd.size()), name.empty() ? "host" : name);
                }
            }
            if (board.known(s.channel)) {
                const std::vector<std::uint8_t> fd = SlotFormData(s.slot);
                const std::string b64 = fd.empty() ? "" : bbnet::party::b64_encode(fd.data(), fd.size());
                if (!b64.empty() && board.form_data(s.channel).empty()) {
                    const bool ok = board.set_form_data(s.channel, b64);
                    std::printf("Party: chalice: dungeon %llu: FormData from slot %d's record (%zu bytes) %s\n",
                                static_cast<unsigned long long>(s.channel), s.slot, fd.size(),
                                ok ? "on the board" : "NOT taken");
                    std::fflush(stdout);
                }
                done_for = s.channel;
            }
        }
    }
    {
        // Every member's chalices (the host's and, since any member may perform the ritual,
        // each guest's): sent when they change, and every 60 s for members that (re)joined.
        std::vector<std::uint32_t> own;
        for (std::uint32_t id : kChaliceGoods) {
            if (GoodsCount(id) > 0) {
                own.push_back(id);
            }
        }
        std::lock_guard<std::mutex> lk(g_mu);
        if (own != g_host_goods || now - g_host_sent_at >= 60.0) {
            if (own != g_host_goods) {
                std::printf("Party: chalice: the %s owns %zu chalice(s)\n", host ? "host" : "guest", own.size());
                std::fflush(stdout);
            }
            g_host_goods = own;
            g_host_pending = own;
            g_host_dirty = true;
            g_host_sent_at = now;
        }
    }
    if (s.load_mode == 0) {
        // In the own world (a host always): the peers' chalices this game lacks.
        std::vector<std::uint32_t> want;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_guest_dirty) {
                want = g_guest_goods;
                g_guest_dirty = false;
            }
        }
        for (std::uint32_t id : want) {
            if (IsChaliceGoods(id) && GoodsCount(id) == 0) {
                const bool ok = GiveGoods(id, 1);
                std::printf("Party: chalice: a party member's chalice %u %s\n", id, ok ? "given" : "NOT given");
                std::fflush(stdout);
            }
        }
    }
    if (s.load_mode == 0) {
        g_local.store(s.slot >= 0 ? s.channel : 0);
    }
    // The summon request a guest's game got (SummonStepManager [FrpgNetMan+0xc50] +0x158, the
    // passive list: entry +0xc8 state (2 = invited), +0x38 the host's map, +0xb0/+0xb8 room data,
    // +0xc0 the channel request made from the message's ChannelId (0x14ba340), whose +0x10 is the id).
    {
        std::uint64_t net = 0, ssm = 0, head = 0;
        const std::uint64_t at = Guest(0x553b120);
        if (at && SafeGet(at, &net) && net && SafeGet(net + 0xc50, &ssm) && ssm && SafeGet(ssm + 0x158, &head) && head) {
            std::uint64_t node = 0;
            SafeGet(head, &node);
            static std::string last_seen;
            std::string now_seen;
            for (int n = 0; node && node != head && n < 8; ++n) {
                std::uint64_t entry = 0, next = 0, chan = 0, chan_id = 0;
                std::int32_t state = 0, map = 0, size = 0;
                SafeGet(node + 0x10, &entry);
                SafeGet(node, &next);
                if (entry) {
                    SafeGet(entry + 0xc8, &state);
                    SafeGet(entry + 0x38, &map);
                    SafeGet(entry + 0xb0, &size);
                    SafeGet(entry + 0xc0, &chan);
                    if (chan) SafeGet(chan + 0x10, &chan_id);
                    char b[160];
                    std::snprintf(b, sizeof b, " [state %d, map 0x%08x, room data %d bytes, channel request %s (id %llu)]",
                                  state, static_cast<std::uint32_t>(map), size, chan ? "yes" : "none",
                                  static_cast<unsigned long long>(chan_id));
                    now_seen += b;
                }
                node = next;
            }
            if (now_seen != last_seen) {
                last_seen = now_seen;
                std::printf("Party: chalice: summon requests:%s\n", now_seen.empty() ? " none" : now_seen.c_str());
                std::fflush(stdout);
            }
        }
    }
    if (s.slot != last.slot || s.channel != last.channel || s.load_mode != last.load_mode) {
        std::printf("Party: chalice: slot %d, dungeon %llu, load mode %d (map %s)\n", s.slot,
                    static_cast<unsigned long long>(s.channel), s.load_mode, MapName(g.map_id).c_str());
        std::printf("Party: chalice: slot %d helper: %s\n", s.slot, HelperDump(s.slot).c_str());
        std::fflush(stdout);
        last = s;
    }
}

bool TakeAltarAutopilot() {
    return g_altar_auto.exchange(false, std::memory_order_acq_rel);
}

void SetHostName(const std::string& name) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_host_name = name;
}

bool PopGuestForm(std::string* body) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_form_pending.empty()) {
        return false;
    }
    *body = g_form_pending;
    g_form_pending.clear();
    return true;
}

void OnGuestForm(int slot, const std::string& member, const std::string& body) {
    json::Value v;
    std::string err;
    const json::Value* id = nullptr;
    const json::Value* form = nullptr;
    if (!json::parse(body, v, err) || v.type != json::Value::Type::Object || !(id = v.find("id")) ||
        id->type != json::Value::Type::String || !(form = v.find("form")) || form->type != json::Value::Type::String ||
        form->string.size() > 4096) {
        std::printf("Party: chalice: FormData event from slot %d refused (malformed)\n", slot);
        std::fflush(stdout);
        return;
    }
    const std::uint64_t channel = std::strtoull(id->string.c_str(), nullptr, 10);
    if (!channel || member.empty()) {
        return;
    }
    bbnet::party::ChannelBoard& board = bbnet::party::FromApi::instance().channels();
    // A dungeon the board does not know (an earlier session's ritual) is added; one it knows
    // without a FormData (a shared fixed dungeon a ritual made by search) gets it; an uploaded
    // one is never replaced.
    const char* what = "already on the board";
    if (!board.known(channel)) {
        what = board.adopt(channel, form->string, member) ? "added to the board" : "refused";
    } else if (board.form_data(channel).empty()) {
        what = board.set_form_data(channel, form->string) ? "on the board" : "refused";
    }
    if (board.known(channel)) {
        board.note_made(member, channel);  // the party's dungeon at the host's altar
    }
    static std::string last;
    const std::string line = std::to_string(channel) + "/" + member + "/" + what;
    if (line != last) {
        last = line;
        std::printf("Party: chalice: dungeon %llu of %s (slot %d): FormData %s\n", static_cast<unsigned long long>(channel),
                    member.c_str(), slot, what);
        std::fflush(stdout);
    }
}

std::uint64_t LocalChannel() {
    return g_local.load();
}

bool PopHostGoods(std::vector<std::uint32_t>* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_host_dirty) {
        return false;
    }
    g_host_dirty = false;
    *out = g_host_pending;
    return true;
}

void RequestGuestGoods(const std::vector<std::uint32_t>& ids) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_guest_dirty) {
        g_guest_goods.clear();  // lists that arrive before the next Tick (several guests) add up
    }
    for (std::uint32_t id : ids) {
        if (IsChaliceGoods(id) && std::find(g_guest_goods.begin(), g_guest_goods.end(), id) == g_guest_goods.end()) {
            g_guest_goods.push_back(id);
        }
    }
    g_guest_dirty = true;
}

} // namespace coop::chalice
#endif
