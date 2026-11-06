// SPDX-License-Identifier: GPL-3.0-or-later
// 0.7.8 (giveitem078, CL-63e235): give items to a party member. The pure rules: what may be given, the wire, and the
// two state machines that make a transfer atomic across the link. Game side: party_give.cpp. Unit test:
// tests/test_party_give.cpp (ninja -C out/gpu party-give-test).
//
// The player opens Insert > Party > "Give items" (a compact list of the own goods), picks an item, a count and a
// member, and presses Give. Both players need the switch on (bbport.ini party_give_items, default OFF; System > My
// party: Your items, launcher My Party): the giver's switch shows the list, the receiver's is its consent.
//
// Two-phase transfer (the giver is the one who must never lose or duplicate an item):
//
//   giver                          receiver
//   OFFER {id, goods, n}  ------>  checks its gate (loading, boss, cutscene ...), the goods rules and its room
//                         <------  READY {room} or REFUSE {why}     (nothing has moved yet)
//   removes n (verified: the count
//   dropped by exactly n)
//   COMMIT {id}           ------>  gives n (native item-get dialog), verified by the count
//                         <------  DONE {got}
//   a short DONE (got < n): the giver takes the difference back
//
//   - the giver removes only after READY and puts back what it removed when the removal does not verify (ABORT);
//   - the receiver gives only on a COMMIT of a live reservation (kReserveS), never twice (the record of the last
//     transfers answers a repeated COMMIT with the same DONE), and a late COMMIT after an ABORT or an expired
//     reservation gives nothing (DONE got 0);
//   - no DONE within kDoneWaitS: the giver asks (ABORT). The receiver answers with what it did: applied -> DONE
//     {got}, else it marks the id as aborted for good and says DONE {0}; the giver then refunds exactly the
//     difference. Only when the receiver cannot be asked (the member's link is down for kLinkDownS, or the question
//     stays unanswered for kAbortWaitS) the giver rolls the whole amount back.
//   - the host relays between guests, replacing "from" with the sender's slot, so a guest cannot sign for another.
//
// Header only, no game dependency.
#pragma once

#include <algorithm>
#include <mutex>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace coop::give {

constexpr const char* kEventName = "give";
constexpr int kMaxCount = 99;          ///< one transfer carries at most this many (the game's largest stack)
constexpr int kMaxSlot = 8;            ///< slots on the wire: 0..7
constexpr double kOfferTimeoutS = 8.0; ///< giver: OFFER unanswered (nothing has moved: the give just fails)
constexpr double kReserveS = 20.0;     ///< receiver: READY sent, COMMIT must come within this
constexpr double kDoneWaitS = 10.0;    ///< giver: COMMIT sent, DONE within this, else ask (ABORT)
constexpr double kAbortWaitS = 20.0;   ///< giver: ABORT unanswered with the link up: roll back
constexpr double kLinkDownS = 90.0;    ///< giver: the receiver unreachable this long after COMMIT: roll back
constexpr double kCooldownS = 1.0;     ///< between two gives of one player (a held button)

/// Why a give does not happen (the word travels in REFUSE / DONE; the text is for the player).
enum class Why : std::uint8_t {
    None,
    Off,        ///< the giver's switch is off
    TheirOff,   ///< the receiver's switch is off (no consent)
    NotHeld,    ///< the giver does not hold that many
    KeyItem,    ///< key item (goodsType 1) or a goods type that is not plain
    Unique,     ///< unique goods (maxNum 1: tools, badges, the Eye ...)
    Story,      ///< goods that carry story progress (key range 4000-4999, gestures)
    Equipped,   ///< the whole stack of a quick-slot item
    NoRoom,     ///< the receiver's inventory and storage cannot take it
    Busy,       ///< another transfer is open
    Loading,
    Dead,
    BossFight,
    Cutscene,
    Travelling,
    NoMember,   ///< the member is not (or no longer) in the party
    NoAnswer,   ///< no reply in time (nothing moved)
    Failed,     ///< the game refused / could not be read (nothing moved, or put back)
    RolledBack, ///< no reply after the item left: it was put back
    Cooldown,
    Self,       ///< to oneself
    GearOff,       ///< the giver's gear switch is off (weapons / armor)
    TheirGearOff,  ///< the receiver's gear switch is off (no consent)
    GearUnique,    ///< shared / quest / not-a-normal-item gear (the table does not list it)
    GearEquipped,  ///< worn or wielded
    BadGems,       ///< gem data of a weapon the receiver cannot build
};
constexpr int kWhyCount = int(Why::BadGems) + 1;

inline const char* WhyWord(Why w) {
    static const char* const k[kWhyCount] = {"none",     "off",     "their_off", "not_held", "key_item", "unique",
                                             "story",    "equipped", "no_room",   "busy",     "loading",  "dead",
                                             "boss",     "cutscene", "travelling", "no_member", "no_answer",
                                             "failed",   "rolled_back", "cooldown", "self",
                                             "gear_off", "their_gear_off", "gear_unique", "gear_equipped", "bad_gems"};
    const int i = int(w);
    return i >= 0 && i < kWhyCount ? k[i] : "failed";
}
inline Why WhyFromWord(const std::string& s) {
    for (int i = 0; i < kWhyCount; ++i) {
        if (s == WhyWord(Why(i))) return Why(i);
    }
    return Why::Failed;
}
/// The player text ("cannot be given: ...").
inline const char* WhyText(Why w) {
    switch (w) {
    case Why::None: return "";
    case Why::Off: return "giving items is off (System > My party: Your items)";
    case Why::TheirOff: return "does not take gifts (giving items is off for them)";
    case Why::NotHeld: return "you do not hold that many";
    case Why::KeyItem: return "key items cannot be given";
    case Why::Unique: return "unique items cannot be given";
    case Why::Story: return "story items cannot be given";
    case Why::Equipped: return "keep one of the item in the quick slot";
    case Why::NoRoom: return "no room in their inventory and storage";
    case Why::Busy: return "busy with another gift";
    case Why::Loading: return "loading";
    case Why::Dead: return "dead";
    case Why::BossFight: return "in a boss fight";
    case Why::Cutscene: return "in a cutscene";
    case Why::Travelling: return "travelling";
    case Why::NoMember: return "not in the party";
    case Why::NoAnswer: return "no answer";
    case Why::Failed: return "could not be given";
    case Why::RolledBack: return "no confirmation, the item was put back";
    case Why::Cooldown: return "too fast";
    case Why::Self: return "that is you";
    case Why::GearOff: return "giving weapons and armor is off (System > My party: Your items)";
    case Why::TheirGearOff: return "does not take weapons and armor (giving gear is off for them)";
    case Why::GearUnique: return "unique and shared gear cannot be given";
    case Why::GearEquipped: return "take it off first";
    case Why::BadGems: return "its blood gems could not be read";
    }
    return "";
}

// ---- what may be given ----

/// Goods whose count is not a plain stack of a consumable even when the param row looks like one: the gestures
/// 800-822 and the whole key / story band 4000-4999 (keys, hunter badges 4110-4119, the Eye 4311, the Old Hunter
/// Bell 4312, the workshop tools, Chalice lamps ...). Extra to goodsType / maxNum, so a row that is wrong in the
/// param data cannot let progress through.
inline bool StoryGoods(std::uint32_t id) {
    return (id >= 800 && id <= 822) || (id >= 4000 && id <= 4999);
}

/// Param facts of a goods item (EquipParamGoods) and the player's state for it.
struct GoodsFacts {
    std::uint32_t id = 0;
    int held = -1;         ///< inventory count of the player (-1 unknown)
    int max_num = -1;      ///< maxNum (-1 unknown)
    int max_storage = -1;  ///< maxRepositoryNum (-1 unknown)
    int goods_type = -1;   ///< goodsType (0 plain, 1 key item, 2 material; -1 unknown)
    bool quick = false;    ///< assigned to a quick-item slot ("equipped")
};

/// The goods rules alone (no count): only plain stackable goods. A receiver applies them to what a giver names, too:
/// a peer cannot make a game take a key or unique item.
inline Why GoodsAllowed(const GoodsFacts& f) {
    if (f.id == 0 || f.goods_type < 0 || f.max_num < 0) return Why::Failed;  // unknown: never
    // 0 = consumables, 2 = materials (blood stones 3000-3030, ritual / chalice materials 7000-7200: 27 rows, all
    // maxNum 99). 1 = key items and every other type (6 = unique rows) stay out.
    if (f.goods_type != 0 && f.goods_type != 2) return Why::KeyItem;
    if (f.max_num <= 1) return Why::Unique;
    if (StoryGoods(f.id)) return Why::Story;
    return Why::None;
}

/// The giver's check for `n` of the item: held, the goods rules, and a quick-slot item keeps one in its stack (the
/// slot points at the inventory entry; the game removes an emptied stack itself only through its own use).
inline Why GiverVerdict(const GoodsFacts& f, int n) {
    if (n < 1 || n > kMaxCount) return Why::NotHeld;
    if (f.held < n) return Why::NotHeld;
    const Why a = GoodsAllowed(f);
    if (a != Why::None) return a;
    if (f.quick && n >= f.held) return Why::Equipped;
    return Why::None;
}

/// Room for goods: the inventory up to maxNum plus the storage box (the game's give list fills the inventory, the
/// rest goes into the storage). `storage_room` -1 = unknown = none.
inline int Room(int held, int max_num, int storage_room) {
    const int inv = max_num > held && held >= 0 ? max_num - held : 0;
    return inv + (storage_room > 0 ? storage_room : 0);
}

/// The receiver's check of an OFFER: the goods rules on its own param data, then room.
inline Why ReceiverVerdict(const GoodsFacts& f, int n, int storage_room, int* room_out = nullptr) {
    if (n < 1 || n > kMaxCount) return Why::Failed;
    const Why a = GoodsAllowed(f);
    const int room = Room(f.held, f.max_num, storage_room);
    if (room_out) *room_out = room;
    if (a != Why::None) return a;
    if (f.held < 0) return Why::Failed;
    return room >= n ? Why::None : Why::NoRoom;
}

// ---- gear (0.7.9, givegear079): weapons and armor ----
//
// A weapon moves with its upgrade level (the id carries it: base + 100 * level) and with its blood gems: the gems
// travel as data (GearGem: slot, GemGenParam id, the 8 instance ints) and the receiver builds them again on the new
// weapon, so a gem is never in two places: the giver's weapon (and the gems it carried) is removed first, exactly as
// goods are, and a failed or lost transfer puts the same weapon back with the same gems. Armor has no state.

enum class Kind : std::uint8_t { Goods, Weapon, Armor };
inline const char* KindWord(Kind k) { return k == Kind::Weapon ? "weapon" : k == Kind::Armor ? "armor" : "goods"; }

constexpr int kGearSlots = 5;  ///< gem slots on a weapon instance (handles at +0x3C, +0x4C ... +0x7C)

struct GearGem {
    int slot = 0;                  ///< 0..4
    std::int32_t id = 0;           ///< GemGenParam id (the gem item id)
    std::int32_t data[8] = {};     ///< the gem instance's 8 ints (flags, rank, 6 effect ids)
    bool operator==(const GearGem& o) const {
        if (slot != o.slot || id != o.id) return false;
        for (int i = 0; i < 8; ++i)
            if (data[i] != o.data[i]) return false;
        return true;
    }
};

/// Gem data a peer may send: slots 0..4 once each, flags 0..3, rank 0..1000, effect ids -1 or 0..99,999,999.
inline bool GemsPlausible(const GearGem* g, int n) {
    if (n < 0 || n > kGearSlots) return false;
    unsigned seen = 0;
    for (int i = 0; i < n; ++i) {
        if (g[i].slot < 0 || g[i].slot >= kGearSlots || (seen & (1u << g[i].slot))) return false;
        seen |= 1u << g[i].slot;
        if (g[i].id <= 0 || g[i].id > 99999999) return false;
        if (g[i].data[0] < 0 || g[i].data[0] > 3 || g[i].data[1] < 0 || g[i].data[1] > 1000) return false;
        for (int k = 2; k < 8; ++k)
            if (g[i].data[k] < -1 || g[i].data[k] > 99999999) return false;
    }
    return true;
}

/// The slot layout of a weapon instance: per slot the shape mask (0x80000000 = no such slot, else a small bit set) and the
/// word at +0x28 (`aux`). A fresh weapon from the game has no slots; the layout is part of the weapon's state (a saved weapon
/// stores it), so it travels with the gems: without it a gem has no slot to sit in.
constexpr std::uint32_t kNoSlot = 0x80000000u;
inline bool GearShapePlausible(const std::uint32_t* masks, std::uint32_t aux, const GearGem* g, int n) {
    if (aux > 0xffffu) return false;
    for (int i = 0; i < kGearSlots; ++i) {
        if (masks[i] != kNoSlot && (masks[i] == 0 || masks[i] > 0xffffu)) return false;
    }
    for (int i = 0; i < n; ++i) {
        if (masks[g[i].slot] == kNoSlot) return false;  // a gem in a slot the weapon does not have
    }
    return true;
}

/// The giveable gear: built once at run time from the loaded game params (party_items.cpp BuildGearTable: the EquipParamWeapon /
/// EquipParamProtector rows with isDeposit 1 that no shared lot gives), so a mod that changes the params is respected. Sorted ids,
/// binary search. Not built yet (or no params): nothing is giveable.
class GearTable {
public:
    static GearTable& Get() {
        static GearTable t;
        return t;
    }
    void Set(std::vector<std::uint32_t> weapons, std::vector<std::uint32_t> armor) {
        std::sort(weapons.begin(), weapons.end());
        std::sort(armor.begin(), armor.end());
        std::lock_guard<std::mutex> lk(mu_);
        weapons_ = std::move(weapons);
        armor_ = std::move(armor);
        ready_ = true;
    }
    bool Ready() const {
        std::lock_guard<std::mutex> lk(mu_);
        return ready_;
    }
    bool Has(bool weapon, std::uint32_t id) const {
        std::lock_guard<std::mutex> lk(mu_);
        const std::vector<std::uint32_t>& t = weapon ? weapons_ : armor_;
        return std::binary_search(t.begin(), t.end(), id);
    }
    std::size_t Weapons() const {
        std::lock_guard<std::mutex> lk(mu_);
        return weapons_.size();
    }
    std::size_t Armor() const {
        std::lock_guard<std::mutex> lk(mu_);
        return armor_.size();
    }

private:
    mutable std::mutex mu_;
    bool ready_ = false;
    std::vector<std::uint32_t> weapons_, armor_;
};

/// Is this weapon / armor row on the giveable list?
inline bool GearGiveable(Kind kind, std::uint32_t id) {
    if (kind == Kind::Goods || id == 0) return false;
    return GearTable::Get().Has(kind == Kind::Weapon, id);
}

struct GearFacts {
    Kind kind = Kind::Weapon;
    std::uint32_t id = 0;
    bool held = false;        ///< the giver holds that entry
    bool equipped = false;    ///< worn / wielded
    int free_slots = -1;      ///< receiver: free inventory entries (-1 unknown)
};

/// The gear rules alone (a receiver applies them to what a giver names, too).
inline Why GearAllowed(const GearFacts& f) {
    if (f.kind == Kind::Goods || f.id == 0) return Why::Failed;
    if (!GearGiveable(f.kind, f.id)) return Why::GearUnique;
    return Why::None;
}
inline Why GearGiverVerdict(const GearFacts& f) {
    if (!f.held) return Why::NotHeld;
    const Why a = GearAllowed(f);
    if (a != Why::None) return a;
    if (f.equipped) return Why::GearEquipped;
    return Why::None;
}
/// The receiver's check of a gear OFFER: the rules on its own table, then a free inventory entry.
inline Why GearReceiverVerdict(const GearFacts& f, int gems, const GearGem* g, const std::uint32_t* masks = nullptr, std::uint32_t aux = 0) {
    const Why a = GearAllowed(f);
    if (a != Why::None) return a;
    if (f.kind == Kind::Armor && gems != 0) return Why::BadGems;
    if (!GemsPlausible(g, gems)) return Why::BadGems;
    if (masks && !GearShapePlausible(masks, aux, g, gems)) return Why::BadGems;
    if (f.free_slots < 0) return Why::Failed;
    return f.free_slots >= 1 ? Why::None : Why::NoRoom;
}

// ---- the moment: nothing while the world is not steady ----

struct GateView {
    bool enabled = false;  ///< the player's own switch
    bool world_up = false;
    bool loading = false;
    bool alive = true;
    bool spectating = false;
    bool boss_fight = false;  ///< a boss health bar is up (fog walk-ins, fights)
    bool cutscene = false;
    bool travelling = false;  ///< a party travel / re-join / seamless load runs
};
inline Why Gate(const GateView& v, bool receiving = false, bool gear = false) {
    if (!v.enabled) return gear ? (receiving ? Why::TheirGearOff : Why::GearOff) : (receiving ? Why::TheirOff : Why::Off);
    if (!v.world_up || v.loading) return Why::Loading;
    if (v.travelling) return Why::Travelling;
    if (!v.alive || v.spectating) return Why::Dead;
    if (v.boss_fight) return Why::BossFight;
    if (v.cutscene) return Why::Cutscene;
    return Why::None;
}

// ---- the wire ----

enum class Op : std::uint8_t { Offer, Ready, Refuse, Commit, Abort, Done };
inline const char* OpWord(Op o) {
    static const char* const k[] = {"offer", "ready", "refuse", "commit", "abort", "done"};
    return k[int(o)];
}

struct Msg {
    Op op = Op::Offer;
    std::uint32_t id = 0;
    int from = 0;  ///< the giver's slot (the host sets it on a relay)
    int to = 0;    ///< the receiver's slot
    std::uint32_t goods = 0;  ///< Offer
    int n = 0;                ///< Offer
    int room = 0;             ///< Ready
    int got = 0;              ///< Done
    Why why = Why::None;      ///< Refuse, Done
    Kind kind = Kind::Goods;  ///< Offer: goods (the goods rules) or gear (`goods` is then the weapon / armor id, n 1)
    int gem_count = 0;        ///< Offer, weapon: the blood gems on it
    GearGem gems[kGearSlots];
    std::uint32_t masks[kGearSlots] = {kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot};  ///< Offer, weapon: slot shape masks
    std::uint32_t aux = 0;    ///< Offer, weapon: the word at +0x28
};

inline std::string Encode(const Msg& m) {
    char b[256];
    std::snprintf(b, sizeof b,
                  "{\"op\":\"%s\",\"id\":%u,\"from\":%d,\"to\":%d,\"goods\":%u,\"n\":%d,\"room\":%d,\"got\":%d,"
                  "\"why\":\"%s\"",
                  OpWord(m.op), m.id, m.from, m.to, m.goods, m.n, m.room, m.got, WhyWord(m.why));
    std::string out = b;
    if (m.op == Op::Offer && m.kind != Kind::Goods) {
        out += std::string(",\"kind\":\"") + KindWord(m.kind) + "\",\"gems\":\"";
        for (int i = 0; i < m.gem_count && i < kGearSlots; ++i) {
            const GearGem& g = m.gems[i];
            char e[160];
            std::snprintf(e, sizeof e, "%s%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", i ? ";" : "", g.slot, g.id, g.data[0], g.data[1],
                          g.data[2], g.data[3], g.data[4], g.data[5], g.data[6], g.data[7]);
            out += e;
        }
        out += "\"";
        char mb[160];
        std::snprintf(mb, sizeof mb, ",\"masks\":\"%u,%u,%u,%u,%u,%u\"", m.masks[0], m.masks[1], m.masks[2], m.masks[3], m.masks[4], m.aux);
        out += mb;
    }
    return out + "}";
}

namespace detail {
inline bool Num(const std::string& s, const char* key, long long* out) {
    const std::string k = std::string("\"") + key + "\":";
    const std::size_t p = s.find(k);
    if (p == std::string::npos) return false;
    const char* a = s.c_str() + p + k.size();
    char* end = nullptr;
    const long long v = std::strtoll(a, &end, 10);
    if (end == a) return false;
    *out = v;
    return true;
}
inline bool Str(const std::string& s, const char* key, std::string* out) {
    const std::string k = std::string("\"") + key + "\":\"";
    const std::size_t p = s.find(k);
    if (p == std::string::npos) return false;
    const std::size_t a = p + k.size(), e = s.find('"', a);
    if (e == std::string::npos || e - a > 600) return false;
    *out = s.substr(a, e - a);
    return true;
}
}  // namespace detail

/// A peer's message, every field range-checked; false on anything malformed. Fields an op does not use are 0.
inline bool Decode(const std::string& body, Msg* out) {
    if (body.size() > 1200) return false;
    std::string op;
    if (!detail::Str(body, "op", &op)) return false;
    Msg m;
    bool known = false;
    for (int i = 0; i <= int(Op::Done); ++i) {
        if (op == OpWord(Op(i))) {
            m.op = Op(i);
            known = true;
        }
    }
    if (!known) return false;
    long long id = 0, from = 0, to = 0, goods = 0, n = 0, room = 0, got = 0;
    if (!detail::Num(body, "id", &id) || id <= 0 || id > 0xffffffffll) return false;
    if (!detail::Num(body, "from", &from) || from < 0 || from >= kMaxSlot) return false;
    if (!detail::Num(body, "to", &to) || to < 0 || to >= kMaxSlot) return false;
    detail::Num(body, "goods", &goods);
    detail::Num(body, "n", &n);
    detail::Num(body, "room", &room);
    detail::Num(body, "got", &got);
    if (m.op == Op::Offer && (goods <= 0 || goods > 99999999 || n < 1 || n > kMaxCount)) return false;
    if (m.op == Op::Offer) {
        std::string kind;
        if (detail::Str(body, "kind", &kind)) {
            if (kind == "weapon") m.kind = Kind::Weapon;
            else if (kind == "armor") m.kind = Kind::Armor;
            else if (kind != "goods") return false;
        }
        if (m.kind != Kind::Goods) {
            if (n != 1) return false;
            std::string gems;
            if (detail::Str(body, "gems", &gems) && !gems.empty()) {
                std::size_t p = 0;
                while (p < gems.size()) {
                    if (m.gem_count >= kGearSlots) return false;
                    GearGem g;
                    long long v[10];
                    const char* c = gems.c_str() + p;
                    for (int k = 0; k < 10; ++k) {
                        char* end = nullptr;
                        v[k] = std::strtoll(c, &end, 10);
                        if (end == c) return false;
                        c = end;
                        if (k < 9) {
                            if (*c != ',') return false;
                            ++c;
                        }
                    }
                    g.slot = int(v[0]);
                    g.id = std::int32_t(v[1]);
                    for (int k = 0; k < 8; ++k) g.data[k] = std::int32_t(v[2 + k]);
                    m.gems[m.gem_count++] = g;
                    p = std::size_t(c - gems.c_str());
                    if (p < gems.size()) {
                        if (gems[p] != ';') return false;
                        ++p;
                    }
                }
                if (!GemsPlausible(m.gems, m.gem_count)) return false;
            }
            if (m.kind == Kind::Armor && m.gem_count != 0) return false;
            std::string masks;
            if (m.kind == Kind::Weapon && detail::Str(body, "masks", &masks)) {
                unsigned long long v[6];
                const char* c = masks.c_str();
                for (int k = 0; k < 6; ++k) {
                    char* end = nullptr;
                    v[k] = std::strtoull(c, &end, 10);
                    if (end == c || v[k] > 0xffffffffull) return false;
                    c = end;
                    if (k < 5) {
                        if (*c != ',') return false;
                        ++c;
                    }
                }
                for (int k = 0; k < kGearSlots; ++k) m.masks[k] = static_cast<std::uint32_t>(v[k]);
                m.aux = static_cast<std::uint32_t>(v[5]);
            }
            if (m.kind == Kind::Weapon && !GearShapePlausible(m.masks, m.aux, m.gems, m.gem_count)) return false;
        }
    }
    if (m.op == Op::Ready && (room < 0 || room > 100000)) return false;
    if (m.op == Op::Done && (got < 0 || got > kMaxCount)) return false;
    std::string why;
    if (detail::Str(body, "why", &why)) m.why = WhyFromWord(why);
    m.id = static_cast<std::uint32_t>(id);
    m.from = int(from);
    m.to = int(to);
    m.goods = m.op == Op::Offer ? static_cast<std::uint32_t>(goods) : 0;
    m.n = m.op == Op::Offer ? int(n) : 0;
    m.room = m.op == Op::Ready ? int(room) : 0;
    m.got = m.op == Op::Done ? int(got) : 0;
    *out = m;
    return true;
}

// ---- the giver ----

enum class Act : std::uint8_t {
    None,
    Remove,      ///< take `n` out of the inventory now, then call Removed(removed)
    SendCommit,
    SendAbort,
    Finished,    ///< the transfer ended: `got` arrived; `refund` (n - got) goes back to the giver
    Failed,      ///< ended with nothing to give back (`why`)
    RolledBack,  ///< ended, the whole amount (`refund`) goes back (`why`)
};
struct Step {
    Act act = Act::None;
    int refund = 0;  ///< put back into the giver's inventory
    int got = 0;
    Why why = Why::None;
};

class Giver {
public:
    enum class State : std::uint8_t { Idle, Offered, Removing, Committed, Aborting };

    State state() const { return st_; }
    bool Busy() const { return st_ != State::Idle; }
    std::uint32_t id() const { return id_; }
    int to() const { return to_; }
    std::uint32_t goods() const { return goods_; }
    int n() const { return n_; }
    Kind kind() const { return kind_; }

    /// A new transfer (the caller checked the rules); false while one is open.
    bool Begin(std::uint32_t id, int to, std::uint32_t goods, int n, double now, Kind kind = Kind::Goods) {
        if (st_ != State::Idle || id == 0) return false;
        kind_ = kind;
        st_ = State::Offered;
        id_ = id;
        to_ = to;
        goods_ = goods;
        n_ = n;
        at_ = now;
        down_since_ = -1.0;
        return true;
    }
    /// READY: the caller must now remove (Act::Remove) and report with Removed().
    Step OnReady(std::uint32_t id, int from, double now) {
        if (st_ != State::Offered || id != id_ || from != to_) return {};
        st_ = State::Removing;
        at_ = now;
        return {Act::Remove, 0, 0, Why::None};
    }
    Step OnRefuse(std::uint32_t id, int from, Why why) {
        if (st_ != State::Offered || id != id_ || from != to_) return {};
        st_ = State::Idle;
        return {Act::Failed, 0, 0, why == Why::None ? Why::Failed : why};
    }
    /// The removal ran: `removed` = how many the inventory really lost. Exactly n: COMMIT. Anything else: ABORT and the
    /// removed ones go back (nothing is lost, nothing created).
    Step Removed(int removed, double now) {
        if (st_ != State::Removing) return {};
        if (removed == n_) {
            st_ = State::Committed;
            at_ = now;
            return {Act::SendCommit, 0, 0, Why::None};
        }
        st_ = State::Idle;
        Step s{Act::SendAbort, removed > 0 ? removed : 0, 0, Why::Failed};
        return s;
    }
    /// The receiver could not be reached before the removal started (it left): nothing moved.
    Step OnMemberGone(int slot) {
        if (st_ != State::Offered || slot != to_) return {};
        st_ = State::Idle;
        return {Act::Failed, 0, 0, Why::NoMember};
    }
    Step OnDone(std::uint32_t id, int from, int got, Why why) {
        if ((st_ != State::Committed && st_ != State::Aborting) || id != id_ || from != to_) return {};
        st_ = State::Idle;
        if (got < 0) got = 0;
        if (got > n_) got = n_;
        Step s{Act::Finished, n_ - got, got, got == n_ ? Why::None : (why == Why::None ? Why::Failed : why)};
        return s;
    }
    /// `link_up`: the receiver can be asked (our link is up and the member is connected).
    Step Tick(double now, bool link_up) {
        switch (st_) {
        case State::Idle:
        case State::Removing:
            return {};
        case State::Offered:
            if (now - at_ > kOfferTimeoutS) {
                st_ = State::Idle;
                return {Act::Failed, 0, 0, Why::NoAnswer};
            }
            return {};
        case State::Committed:
        case State::Aborting:
            if (!link_up) {
                if (down_since_ < 0) down_since_ = now;
                if (now - down_since_ >= kLinkDownS) return Roll();
                return {};
            }
            if (down_since_ >= 0) {  // the link is back: the receiver's answers (queued) get a fresh wait
                down_since_ = -1.0;
                at_ = now;
                return {};
            }
            if (st_ == State::Committed && now - at_ > kDoneWaitS) {
                st_ = State::Aborting;
                at_ = now;
                return {Act::SendAbort, 0, 0, Why::None};
            }
            if (st_ == State::Aborting && now - at_ > kAbortWaitS) return Roll();
            return {};
        }
        return {};
    }

private:
    Step Roll() {
        st_ = State::Idle;
        return {Act::RolledBack, n_, 0, Why::RolledBack};
    }
    State st_ = State::Idle;
    std::uint32_t id_ = 0, goods_ = 0;
    int to_ = 0, n_ = 0;
    Kind kind_ = Kind::Goods;
    double at_ = 0, down_since_ = -1.0;
};

// ---- the receiver ----

enum class RAct : std::uint8_t {
    None,
    Apply,      ///< give `n` of `goods` now, then call Applied(got)
    SendDone,   ///< reply DONE {got, why}
    Ignore,
};
struct RStep {
    RAct act = RAct::None;
    std::uint32_t goods = 0;
    int n = 0;
    int got = 0;
    Why why = Why::None;
    int from = 0;
    std::uint32_t id = 0;
    Kind kind = Kind::Goods;
};

class Receiver {
public:
    bool Reserved() const { return reserved_; }
    /// READY was sent for the OFFER: hold the slot for the COMMIT. False: another reservation is live (Busy) or this
    /// id was seen already (a repeated OFFER: ignore).
    enum class Result : std::uint8_t { Ok, Busy, Duplicate };
    Result Reserve(int from, std::uint32_t id, std::uint32_t goods, int n, double now, Kind kind = Kind::Goods) {
        if (Find(from, id)) return Result::Duplicate;
        if (reserved_) return Result::Busy;
        reserved_ = true;
        from_ = from;
        id_ = id;
        goods_ = goods;
        n_ = n;
        kind_ = kind;
        at_ = now;
        return Result::Ok;
    }
    RStep OnCommit(int from, std::uint32_t id, double now) {
        if (const Rec* r = Find(from, id)) {  // seen: applied -> the same DONE again; aborted / expired -> 0
            return {RAct::SendDone, 0, 0, r->got, r->got > 0 ? Why::None : Why::Failed, from, id};
        }
        if (reserved_ && from == from_ && id == id_ && now - at_ <= kReserveS) {
            return {RAct::Apply, goods_, n_, 0, Why::None, from, id, kind_};
        }
        Remember(from, id, 0);  // unknown or too late: the id is dead for good
        reserved_ = reserved_ && !(from == from_ && id == id_);
        return {RAct::SendDone, 0, 0, 0, Why::Failed, from, id};
    }
    /// The Apply ran: `got` items arrived (0 = none). Replies DONE.
    RStep Applied(int got, Why why) {
        if (!reserved_) return {};
        reserved_ = false;
        Remember(from_, id_, got);
        return {RAct::SendDone, goods_, n_, got, got >= n_ ? Why::None : (why == Why::None ? Why::Failed : why), from_, id_, kind_};
    }
    RStep OnAbort(int from, std::uint32_t id) {
        if (const Rec* r = Find(from, id)) {
            return {RAct::SendDone, 0, 0, r->got, r->got > 0 ? Why::None : Why::Failed, from, id};
        }
        if (reserved_ && from == from_ && id == id_) reserved_ = false;
        Remember(from, id, 0);
        return {RAct::SendDone, 0, 0, 0, Why::Failed, from, id};
    }
    /// A reservation that outlived kReserveS ends; its id is dead.
    void Tick(double now) {
        if (reserved_ && now - at_ > kReserveS) {
            Remember(from_, id_, 0);
            reserved_ = false;
        }
    }
    /// The giver left: its reservation ends.
    void OnMemberGone(int slot) {
        if (reserved_ && from_ == slot) {
            Remember(from_, id_, 0);
            reserved_ = false;
        }
    }

private:
    struct Rec {
        int from = -1;
        std::uint32_t id = 0;
        int got = 0;
    };
    static constexpr int kRecords = 32;
    const Rec* Find(int from, std::uint32_t id) const {
        for (const Rec& r : rec_) {
            if (r.id == id && r.from == from) return &r;
        }
        return nullptr;
    }
    void Remember(int from, std::uint32_t id, int got) {
        rec_[next_ % kRecords] = {from, id, got};
        ++next_;
    }
    bool reserved_ = false;
    int from_ = 0, n_ = 0;
    std::uint32_t id_ = 0, goods_ = 0;
    Kind kind_ = Kind::Goods;
    double at_ = 0;
    Rec rec_[kRecords];
    unsigned next_ = 0;
};

// ---- texts ----

inline std::string Plural(const std::string& item, int n) {
    return (item.empty() ? std::string("Item") : item) + " x" + std::to_string(n);
}
inline std::string GaveText(const std::string& item, int n, const std::string& name) {
    return "Gave " + Plural(item, n) + " to " + name;
}
inline std::string GotText(const std::string& item, int n, const std::string& name) {
    return Plural(item, n) + " from " + name;
}
inline std::string RefusedText(const std::string& item, const std::string& name, Why w) {
    return "Give " + (item.empty() ? std::string("item") : item) + " to " + name + ": " + WhyText(w);
}

}  // namespace coop::give
