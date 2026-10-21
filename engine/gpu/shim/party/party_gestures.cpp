// SPDX-License-Identifier: GPL-3.0-or-later
// Party gestures: shared by every party member, kept in each one's own save (party_gestures.h).
#include "party_gestures.h"

#include "net/json.h"
#include "party_gamedata.h"

#include <algorithm>
#include <sstream>

namespace coop::gestures {

bool IsGestureGoods(std::uint32_t goods_id) {
    return goods_id >= kFirstGoods && goods_id < kFirstGoods + kCount;
}

std::int32_t TeachFlag(std::uint32_t goods_id) {
    // Scan of every talk ESD (script/talk/*.talkesdbnd): the teaching state's arguments (index,
    // goods, flag): 804 6744 (t240325, t241325), 805 6745 (t240343), 806 6746 (t240318), 807 6747
    // (t240322), 809 6749, 810 6750, 811 6751, 813 6753, 814 6754, 815 6755, 816 6756, 817 6757,
    // 818 6758, 819 6759 (t250307), 820 6760, 822 6770 (t270503).
    switch (goods_id) {
    case 804: case 805: case 806: case 807: case 809: case 810: case 811: case 813: case 814:
    case 815: case 816: case 817: case 818: case 819: case 820:
        return static_cast<std::int32_t>(5940 + goods_id);
    case 822:
        return 6770;
    default:
        return -1;
    }
}

const char* GestureName(std::uint32_t goods_id) {
    // The game's own text (message files, read when the party tables are built).
    return IsGestureGoods(goods_id) ? gamedata::GestureText(goods_id) : "?";
}

std::uint32_t EntryGoods(std::uint32_t entry, bool* learned) {
    if (learned) *learned = (entry & 1) != 0;
    if (entry >= 0xffff0000u || (entry & 0xfffe) == 0xfffe) {
        return 0;  // empty (0x14EE2E0 prints "empty")
    }
    const std::uint32_t param = (entry & 0xffff) >> 1;  // the gesture table row: index + 1
    return param >= 1 && param <= static_cast<std::uint32_t>(kCount) ? kFirstGoods + param - 1 : 0;
}

std::string ToJsonText(const std::vector<std::uint32_t>& goods) {
    json::Value v = json::Value::make_object();
    json::Value a = json::Value::make_array();
    for (std::uint32_t id : goods) {
        a.push(id);
    }
    v.set("goods", a);
    return json::dump(v, 0);
}

bool FromJsonText(const std::string& text, std::vector<std::uint32_t>* out, std::string* error) {
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
        if (x.type != json::Value::Type::Number || x.number < 0 || x.number >= 100000) {
            continue;
        }
        const auto id = static_cast<std::uint32_t>(x.number);
        if (static_cast<double>(id) == x.number && IsGestureGoods(id) &&
            std::find(out->begin(), out->end(), id) == out->end()) {
            out->push_back(id);
        }
    }
    return true;
}

bool Store::Add(int slot, std::uint32_t goods, bool summoned) {
    if (Find(slot, goods) || !IsGestureGoods(goods)) {
        return false;
    }
    owed.push_back({goods, slot, summoned});
    return true;
}

Store::Owed* Store::Find(int slot, std::uint32_t goods) {
    for (Owed& o : owed) {
        if (o.slot == slot && o.goods == goods) {
            return &o;
        }
    }
    return nullptr;
}

void Store::Drop(int slot, std::uint32_t goods) {
    owed.erase(std::remove_if(owed.begin(), owed.end(),
                              [&](const Owed& o) { return o.slot == slot && o.goods == goods; }),
               owed.end());
}

std::string Store::Dump() const {
    std::ostringstream s;
    for (const Owed& o : owed) {
        s << "owed " << o.slot << ' ' << o.goods << ' ' << (o.summoned ? 1 : 0) << '\n';
    }
    return s.str();
}

Store Store::Parse(const std::string& text) {
    Store st;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream l(line);
        std::string word;
        int slot = -1;
        long long goods = -1;
        int summoned = 0;
        if (!(l >> word >> slot >> goods >> summoned) || word != "owed" || goods < 0 ||
            !IsGestureGoods(static_cast<std::uint32_t>(goods)) || slot < -1 || slot > 99) {
            continue;  // a torn or foreign line: skipped
        }
        st.Add(slot, static_cast<std::uint32_t>(goods), summoned != 0);
    }
    return st;
}

std::vector<std::uint32_t> NewlyOwed(const std::vector<std::uint32_t>& peer, const std::set<std::uint32_t>& learned,
                                     const Store& store, int slot) {
    std::vector<std::uint32_t> out;
    for (std::uint32_t g : peer) {
        if (!IsGestureGoods(g) || learned.count(g) || std::find(out.begin(), out.end(), g) != out.end()) {
            continue;
        }
        bool owed = false;
        for (const Store::Owed& o : store.owed) {
            owed = owed || (o.slot == slot && o.goods == g);
        }
        if (!owed) {
            out.push_back(g);
        }
    }
    return out;
}

const char* StepName(Step s) {
    switch (s) {
    case Step::Have: return "have";
    case Step::Learn: return "learn";
    case Step::Wait: return "wait";
    }
    return "?";
}

Step Decide(bool home, int learned, bool summoned_before) {
    if (learned < 0) {
        return Step::Wait;
    }
    if (!learned) {
        return Step::Learn;  // missing (lost, or another save's state): a bit, never a duplicate
    }
    // Learned while summoned: kept owed until the own world reads it.
    return !home && summoned_before ? Step::Wait : Step::Have;
}

} // namespace coop::gestures

#ifndef BB_PARTY_GESTURES_NO_GAME
#include "coop_hooks.h"
#include "game_state.h"
#include "party_flow.h"
#include "party_progress.h"

#include "../net/bbnet_internal.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace coop::gestures {

namespace {

constexpr std::uint64_t kGameDataMan = 0x553b130;  // +8 PlayerGameData
constexpr std::uint64_t kGestureData = 0x5b8;      // PlayerGameData +0x5B8: GestureGameData (0x14F1510)
constexpr int kEntries = 30;                       // GestureGameData +0x10: 30 u32 (0x78 bytes saved)
constexpr std::uint64_t kWts = 0x5556678, kWtsOwnWorld = 0x1592;  // GSM: 1 own world, 0 summoned

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("[coop] Party gestures: %s\n", line);
    std::fflush(stdout);
}

std::mutex g_mu;
std::vector<std::uint32_t> g_peer;        // peers' gestures not looked at yet
std::vector<std::uint32_t> g_own_pending;  // the own list to send
bool g_own_dirty = false;
Store g_store;
bool g_store_loaded = false;

std::filesystem::path StorePath() {
    std::string dir = bbnet::settings().user_dir;
    if (dir.empty()) {
        if (const char* u = std::getenv("BB_GPU_USER_DIR")) dir = u;
        else if (const char* u2 = std::getenv("BB_USER_DIR")) dir = u2;
        else dir = std::string(std::getenv("BB_DATA_DIR") ? std::getenv("BB_DATA_DIR") : ".") + "/user";
    }
    return std::filesystem::path(dir) / "party_gestures.txt";
}

/// Caller holds g_mu.
void LoadLocked() {
    if (g_store_loaded) {
        return;
    }
    g_store_loaded = true;
    std::ifstream f(StorePath(), std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    g_store = Store::Parse(s.str());
    if (!g_store.owed.empty()) {
        Log("%zu owed gesture(s) on disk", g_store.owed.size());
    }
}

/// Caller holds g_mu.
void SaveLocked() {
    std::error_code ec;
    const std::filesystem::path path = StorePath();
    if (g_store.owed.empty()) {
        std::filesystem::remove(path, ec);
        return;
    }
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
        f << g_store.Dump();
    }
    std::filesystem::rename(tmp, path, ec);
}

bool Off() {
    const char* v = std::getenv("BB_PARTY_GESTURES");
    return v && v[0] == '0' && !v[1];
}

int OwnWorld() {
    std::uint64_t wts = 0;
    std::uint8_t v = 0xff;
    return SafeGet(Guest(kWts), &wts) && wts && SafeGet(wts + kWtsOwnWorld, &v) ? v : -1;
}

bool OwnFlag(std::int32_t flag) {
    bool v = false;
    return flag > 0 && progress::ReadFlagOwnSave(static_cast<std::uint32_t>(flag), &v) && v;
}

std::uint64_t GestureData() {
    std::uint64_t gdm = 0, pgd = 0, ggd = 0;
    if (!SafeGet(Guest(kGameDataMan), &gdm) || !gdm || !SafeGet(gdm + 8, &pgd) || !pgd ||
        !SafeGet(pgd + kGestureData, &ggd)) {
        return 0;
    }
    return ggd;
}

/// learned[i] for goods 800 + i: 1 / 0, -1 when its entry is missing.
bool ReadLearned(int (&learned)[kCount]) {
    for (int& v : learned) v = -1;
    const std::uint64_t ggd = GestureData();
    if (!ggd) {
        return false;
    }
    for (int i = 0; i < kEntries; ++i) {
        std::uint32_t e = 0;
        bool on = false;
        if (!SafeGet(ggd + 0x10 + 4ull * i, &e)) {
            return false;
        }
        const std::uint32_t g = EntryGoods(e, &on);
        if (g) learned[g - kFirstGoods] = on ? 1 : 0;
    }
    return true;
}

/// Sets bit 0 of the gesture's entry, as the talk scripts' command 0x42 does.
bool SetLearned(std::uint32_t goods) {
    const std::uint64_t ggd = GestureData();
    if (!ggd) {
        return false;
    }
    for (int i = 0; i < kEntries; ++i) {
        std::uint32_t e = 0;
        if (!SafeGet(ggd + 0x10 + 4ull * i, &e)) {
            return false;
        }
        if (EntryGoods(e) == goods) {
            e |= 1u;
            return SafeWrite(ggd + 0x10 + 4ull * i, &e, 4);
        }
    }
    return false;
}

std::string InventorySummary();

std::string List(const std::set<std::uint32_t>& s) {
    std::string out;
    for (std::uint32_t g : s) {
        out += (out.empty() ? "" : " ") + std::to_string(g);
    }
    return out.empty() ? "none" : out;
}

struct TestItem {
    std::uint32_t goods = 0;
    bool summoned = false;
};
std::vector<TestItem> ParseTest() {
    std::vector<TestItem> out;
    const char* v = std::getenv("BB_PARTY_GESTURE_TEST");
    const std::string s = v ? v : "";
    std::size_t at = 0;
    while (at < s.size()) {
        std::size_t end = s.find(',', at);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(at, end - at);
        TestItem t;
        t.goods = static_cast<std::uint32_t>(std::strtoul(item.c_str(), nullptr, 10));
        t.summoned = item.find(":summoned") != std::string::npos;
        if (IsGestureGoods(t.goods)) out.push_back(t);
        at = end + 1;
    }
    return out;
}

/// Learns a gesture as a talk script teaches it: the bit, the taught flag (own pool), the item-get
/// notice; then a save request.
bool Learn(std::uint32_t goods, const char* why, bool notice) {
    const std::int32_t flag = TeachFlag(goods);
    bool flag_ok = true;
    if (flag > 0 && !OwnFlag(flag)) {
        flag_ok = progress::WriteFlagOwnSave(static_cast<std::uint32_t>(flag), true);
    }
    const bool ok = SetLearned(goods);
    const bool shown = ok && notice && ShowItemGetNotice(kItemGoods, goods, 1);
    char f[64] = "";
    if (flag > 0) std::snprintf(f, sizeof f, "; taught flag %d %s", flag, flag_ok ? "set (own pool)" : "NOT set");
    Log("%s: gesture %u (%s) %s%s%s; load mode %d", why, goods, GestureName(goods),
        ok ? "learned" : "NOT learned (no GestureGameData entry)", shown ? " (item-get notice)" : "", f,
        progress::FlagLoadMode());
    if (ok) {
        RequestGameSave("party gesture");
    }
    return ok;
}

/// The inventory (PlayerGameData +0x328) for the test log: its two sections (index < +0x24 at
/// +0x58, the rest at +0x48; 16-byte entries {gaitem handle, item id, count}), the used entries of
/// the first and the goods held in the second.
std::string InventorySummary() {
    std::uint64_t gdm = 0, pgd = 0;
    if (!SafeGet(Guest(kGameDataMan), &gdm) || !gdm || !SafeGet(gdm + 8, &pgd) || !pgd) {
        return "no inventory";
    }
    const std::uint64_t list = pgd + 0x328;
    std::uint32_t split = 0;
    std::int32_t last = -1;
    std::uint64_t a = 0, b = 0;
    if (!SafeGet(list + 0x24, &split) || !SafeGet(list + 0x88, &last) || !SafeGet(list + 0x58, &a) ||
        !SafeGet(list + 0x48, &b) || last < 0 || last > 4096 || split > 4096) {
        return "inventory unreadable";
    }
    int used_a = 0, used_b = 0;
    std::string ids;
    for (std::int32_t i = 0; i <= last; ++i) {
        const bool first = static_cast<std::uint32_t>(i) < split;
        const std::uint64_t e = first ? a + 16ull * i : b + 16ull * (i - split);
        std::uint32_t h = 0, id = 0;
        std::int32_t n = 0;
        if (!SafeGet(e, &h) || !SafeGet(e + 4, &id) || !SafeGet(e + 8, &n) || h == 0) {
            continue;
        }
        if (first) {
            ++used_a;
        } else {
            ++used_b;
            if (ids.size() < 400) ids += (ids.empty() ? "" : " ") + std::to_string(id & 0xfffffff) + "x" + std::to_string(n);
        }
    }
    char head[128];
    std::snprintf(head, sizeof head, "inventory: split %u, last %d; section 1 %d used, section 2 %d used: ", split,
                  last, used_a, used_b);
    return head + (ids.empty() ? std::string("none") : ids);
}

} // namespace

void Tick(bool host) {
    static double next = 0;
    static const bool off = Off();
    if (off) {
        return;
    }
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now < next) {
        return;
    }
    next = now + 1.0;
    const GameSnapshot gs = ReadGameState(false);
    if (!gs.world_up || gs.loading || gs.map_id == 0xffffffffu || !gs.player_rec) {
        return;
    }
    // Steady for 3 s after a load (the character and the flag store are the new world's).
    static std::uint32_t steady_map = 0xffffffffu;
    static int steady_mode = -2;
    static double steady_at = 0;
    const int mode = progress::FlagLoadMode();
    if (gs.map_id != steady_map || mode != steady_mode) {
        steady_map = gs.map_id;
        steady_mode = mode;
        steady_at = now;
        return;
    }
    if (now - steady_at < 3.0 || mode < 0) {
        return;
    }
    const bool home = mode == 0 && OwnWorld() == 1;
    const bool summoned = mode == 1 && OwnWorld() == 0;
    if (!home && !summoned) {
        return;
    }
    int learned_of[kCount];
    if (!ReadLearned(learned_of)) {
        return;
    }
    const int slot = party::flow::CurrentSaveSlot();
    const char* where = home ? "own world" : "summoned";

    // Test aid (BB_PARTY_GESTURE_TEST): learn these gestures once; the state around it.
    static std::vector<TestItem> test = ParseTest();
    static const bool test_env = std::getenv("BB_PARTY_GESTURE_TEST") != nullptr;
    if (test_env) {
        static std::string last_inv;
        const std::string inv = std::string(where) + " " + InventorySummary();
        if (inv != last_inv) {
            last_inv = inv;
            Log("test: %s", inv.c_str());
        }
    }
    for (auto it = test.begin(); it != test.end();) {
        if (it->summoned == summoned && (!host || gs.cooperators != 0)) {
            if (learned_of[it->goods - kFirstGoods] == 1) {
                Log("test: gesture %u (%s) is learned already", it->goods, GestureName(it->goods));
            } else if (Learn(it->goods, it->summoned ? "test (summoned)" : "test (own world)", true)) {
                learned_of[it->goods - kFirstGoods] = 1;
            }
            it = test.erase(it);
        } else {
            ++it;
        }
    }

    std::lock_guard<std::mutex> lk(g_mu);
    LoadLocked();
    bool dirty = false;
    std::set<std::uint32_t> learned;
    for (int i = 0; i < kCount; ++i) {
        if (learned_of[i] == 1) learned.insert(kFirstGoods + i);
    }
    static std::set<std::uint32_t> seen;  // learned at the last read
    static bool seen_valid = false;
    static bool seen_home = false;
    if (seen_valid && summoned) {
        // Learned in the host's world (an NPC's talk there, the test aid): checked again at home.
        for (std::uint32_t g : learned) {
            if (!seen.count(g) && g_store.Add(slot, g, true)) {
                dirty = true;
                Log("gesture %u (%s) learned while summoned: checked again in the own world", g, GestureName(g));
            }
        }
    }
    static std::string last_line;
    const std::string line = std::string(where) + ", save slot " + std::to_string(slot) + ": learned " + List(learned);
    if (line != last_line || seen_home != home) {
        last_line = line;
        Log("%s", line.c_str());
    }
    seen = learned;
    seen_valid = true;
    seen_home = home;

    if (home) {
        // A taught flag without the bit: a gesture an older build lost in another world.
        for (int i = 0; i < kCount; ++i) {
            const std::uint32_t g = kFirstGoods + i;
            if (learned_of[i] == 0 && OwnFlag(TeachFlag(g)) && !g_store.Find(slot, g)) {
                g_store.Add(slot, g);
                dirty = true;
                Log("gesture %u (%s): taught flag %d set but not learned (lost in another world): learned again", g,
                    GestureName(g), TeachFlag(g));
            }
        }
    }
    // A peer's gestures this save lacks are owed; learned below (a host: a guest's at once).
    if (!g_peer.empty()) {
        for (std::uint32_t g : NewlyOwed(g_peer, learned, g_store, slot)) {
            if (g_store.Add(slot, g)) {
                dirty = true;
                Log("gesture %u (%s) from a party member: owed to save slot %d", g, GestureName(g), slot);
            }
        }
        g_peer.clear();
    }
    for (std::size_t i = 0; i < g_store.owed.size();) {
        Store::Owed& o = g_store.owed[i];
        if (o.slot != slot) {
            ++i;  // another character's
            continue;
        }
        const Step st = Decide(home, learned_of[o.goods - kFirstGoods], o.summoned);
        if (st == Step::Have) {
            Log("gesture %u (%s): in this save (%s, learned)%s; no longer owed", o.goods, GestureName(o.goods), where,
                o.summoned ? ", the one learned while summoned kept" : "");
            g_store.owed.erase(g_store.owed.begin() + static_cast<std::ptrdiff_t>(i));
            dirty = true;
            continue;
        }
        if (st == Step::Learn) {
            if (home && o.summoned) {
                Log("gesture %u (%s): learned while summoned but missing in the own world", o.goods,
                    GestureName(o.goods));
            }
            if (Learn(o.goods, home ? "a party member's gesture (own world)" : "a party member's gesture (summoned)",
                      true)) {
                learned.insert(o.goods);
                learned_of[o.goods - kFirstGoods] = 1;
                o.summoned = summoned;  // summoned: the own world reads it back; home: dropped next read
                dirty = true;
            }
        }
        ++i;
    }
    if (dirty) {
        SaveLocked();
    }

    // The own list when it changed, and every 60 s (members that (re)joined).
    static std::set<std::uint32_t> sent;
    static double sent_at = -1000;
    if (learned != sent || now - sent_at >= 60.0) {
        if (learned != sent) {
            Log("%s's gestures for the party: %zu (%s)", host ? "the host" : "this guest", learned.size(),
                List(learned).c_str());
        }
        sent = learned;
        sent_at = now;
        g_own_pending.assign(learned.begin(), learned.end());
        g_own_dirty = true;
    }
    seen = learned;  // the ones learned above are not "learned while summoned" by an NPC
}

bool PopOwn(std::vector<std::uint32_t>* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_own_dirty) {
        return false;
    }
    g_own_dirty = false;
    *out = g_own_pending;
    return true;
}

void RequestPeer(const std::vector<std::uint32_t>& goods) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (std::uint32_t g : goods) {
        if (IsGestureGoods(g) && std::find(g_peer.begin(), g_peer.end(), g) == g_peer.end() && g_peer.size() < 64) {
            g_peer.push_back(g);
        }
    }
}

} // namespace coop::gestures
#endif
