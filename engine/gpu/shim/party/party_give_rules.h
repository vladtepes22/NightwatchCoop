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
};
constexpr int kWhyCount = int(Why::Self) + 1;

inline const char* WhyWord(Why w) {
    static const char* const k[kWhyCount] = {"none",     "off",     "their_off", "not_held", "key_item", "unique",
                                             "story",    "equipped", "no_room",   "busy",     "loading",  "dead",
                                             "boss",     "cutscene", "travelling", "no_member", "no_answer",
                                             "failed",   "rolled_back", "cooldown", "self"};
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
    int goods_type = -1;   ///< goodsType (0 plain, 1 key item; -1 unknown)
    bool quick = false;    ///< assigned to a quick-item slot ("equipped")
};

/// The goods rules alone (no count): only plain stackable goods. A receiver applies them to what a giver names, too:
/// a peer cannot make a game take a key or unique item.
inline Why GoodsAllowed(const GoodsFacts& f) {
    if (f.id == 0 || f.goods_type < 0 || f.max_num < 0) return Why::Failed;  // unknown: never
    if (f.goods_type != 0) return Why::KeyItem;
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
inline Why Gate(const GateView& v, bool receiving = false) {
    if (!v.enabled) return receiving ? Why::TheirOff : Why::Off;
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
};

inline std::string Encode(const Msg& m) {
    char b[256];
    std::snprintf(b, sizeof b,
                  "{\"op\":\"%s\",\"id\":%u,\"from\":%d,\"to\":%d,\"goods\":%u,\"n\":%d,\"room\":%d,\"got\":%d,"
                  "\"why\":\"%s\"}",
                  OpWord(m.op), m.id, m.from, m.to, m.goods, m.n, m.room, m.got, WhyWord(m.why));
    return b;
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
    if (e == std::string::npos || e - a > 24) return false;
    *out = s.substr(a, e - a);
    return true;
}
}  // namespace detail

/// A peer's message, every field range-checked; false on anything malformed. Fields an op does not use are 0.
inline bool Decode(const std::string& body, Msg* out) {
    if (body.size() > 400) return false;
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

    /// A new transfer (the caller checked the rules); false while one is open.
    bool Begin(std::uint32_t id, int to, std::uint32_t goods, int n, double now) {
        if (st_ != State::Idle || id == 0) return false;
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
};

class Receiver {
public:
    bool Reserved() const { return reserved_; }
    /// READY was sent for the OFFER: hold the slot for the COMMIT. False: another reservation is live (Busy) or this
    /// id was seen already (a repeated OFFER: ignore).
    enum class Result : std::uint8_t { Ok, Busy, Duplicate };
    Result Reserve(int from, std::uint32_t id, std::uint32_t goods, int n, double now) {
        if (Find(from, id)) return Result::Duplicate;
        if (reserved_) return Result::Busy;
        reserved_ = true;
        from_ = from;
        id_ = id;
        goods_ = goods;
        n_ = n;
        at_ = now;
        return Result::Ok;
    }
    RStep OnCommit(int from, std::uint32_t id, double now) {
        if (const Rec* r = Find(from, id)) {  // seen: applied -> the same DONE again; aborted / expired -> 0
            return {RAct::SendDone, 0, 0, r->got, r->got > 0 ? Why::None : Why::Failed, from, id};
        }
        if (reserved_ && from == from_ && id == id_ && now - at_ <= kReserveS) {
            return {RAct::Apply, goods_, n_, 0, Why::None, from, id};
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
        return {RAct::SendDone, goods_, n_, got, got >= n_ ? Why::None : (why == Why::None ? Why::Failed : why), from_, id_};
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
