// SPDX-License-Identifier: GPL-3.0-or-later
// 0.7.8 give items to a party member, the game side (party_give.h). Main thread: Tick; OnEvent on the link's callback
// thread only queues; the the item menu calls MenuOffers / MenuMembers / MenuAmounts / MenuGive.
#include "party_give.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_async_io.h"
#include "party_b2.h"
#include "party_display_name.h"
#include "party_items.h"
#include "party_link.h"
#include "party_status.h"
#include "seamless_rules.h"
#include "../bbport_settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace coop::give {

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party give: ") + line + "\n");
}

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr std::size_t kMaxQueued = 64;

std::mutex g_mu;
std::deque<std::pair<int, std::string>> g_in;  // under g_mu: (sender slot, body)
struct Request {
    u32 goods = 0;
    int n = 0, to = 0;
    Kind kind = Kind::Goods;  // gear: `goods` is the weapon / armor id, n 1
    u32 handle = 0;           // gear: the inventory entry's GaItem handle (0: the first giveable entry with that id)
};
std::deque<Request> g_requests;  // under g_mu

// main thread only
Giver g_giver;
Receiver g_recv;
GearItem g_gear;      // giver of gear: the offered entry with its gems (put back identically on a failed transfer)
Msg g_recv_gear;      // receiver of gear: the reserved OFFER (its gems)
double g_last_give_at = -1e9;
u32 g_next_id = 0;
double g_steady_since = -1;
bool g_test_parsed = false;
struct TestGive {
    u32 goods = 0;
    int n = 0, to = 0;
    double at = 0;
    bool done = false;
    int kind = 0;  // 0 give, 1 switch on, 2 switch off, 3 gear switch on, 4 gear switch off, 5 give a weapon, 6 give armor
};
struct TestSetup {  // BB_PARTY_GEAR_SETUP: gear added to the own inventory (a test fixture)
    Kind kind = Kind::Weapon;
    u32 id = 0;
    int gems = 0;
    double at = 0;
    bool done = false;
};
std::vector<TestSetup> g_setup;
double g_gear_steady = -1;
double g_probe_at = -1;  // BB_PARTY_GEAR_PROBE=<s>: logs the gear entries once the world is steady for s seconds
bool g_watch_gear = false;
std::vector<u32> g_watch;  // BB_PARTY_GIVE_WATCH: goods whose holdings are logged every 10 s
double g_next_watch = 0;
std::vector<TestGive> g_test;
int g_fault = 0;  // 1 mute, 2 linkdrop

/// A line to the party notice corner.
void Notice(const std::string& text) {
    party::status::SetLastEvent(text);
    Log("notice: %s", text.c_str());
}

std::string ItemName(u32 goods) {
    const std::string n = PartyItemName(4, static_cast<std::int32_t>(goods));
    return n.empty() ? "Goods " + std::to_string(goods) : n;
}

std::string GearName(Kind kind, u32 id) {
    const std::string n = PartyItemName(kind == Kind::Weapon ? 0 : 1, static_cast<std::int32_t>(id));
    return n.empty() ? std::string(KindWord(kind)) + " " + std::to_string(id) : n;
}

/// "Saw Cleaver +5 (2 blood gems)" for the notices.
std::string GearLabel(const GearItem& g) {
    std::string n = GearName(g.kind, g.id);
    if (g.gem_count > 0) n += " (" + std::to_string(g.gem_count) + (g.gem_count == 1 ? " blood gem)" : " blood gems)");
    return n;
}
std::string GearLabel(const Msg& m) {
    GearItem g;
    g.kind = m.kind;
    g.id = m.goods;
    g.gem_count = m.gem_count;
    return GearLabel(g);
}

/// What the notices call the transferred thing: gear by its name, goods with the count.
std::string GaveLine(const std::string& label, bool gear, int n, const std::string& to) {
    return gear ? "Gave " + label + " to " + to : GaveText(label, n, to);
}
std::string GotLine(const std::string& label, bool gear, int n, const std::string& from) {
    return gear ? label + " from " + from : GotText(label, n, from);
}

std::string MemberName(party::PartyLink* link, int slot) {
    if (link) {
        for (const party::RosterEntry& e : link->roster()) {
            if (e.slot == slot) return bbname::Display(e.name);
        }
    }
    return "hunter " + std::to_string(slot);
}

bool MemberConnected(party::PartyLink* link, int slot) {
    if (!link) return false;
    for (const party::RosterEntry& e : link->roster()) {
        if (e.slot == slot) return e.connected;
    }
    return false;
}

GateView ReadGate(const GameSnapshot& s, bool host, bool gear = false) {
    GateView v;
    v.enabled = gear ? EnabledGear() : Enabled();
    v.world_up = s.world_up && s.player;
    v.loading = s.loading;
    v.alive = b2::LocalHp() > 0;
    v.spectating = b2::Spectating();
    v.boss_fight = BossBarShown();
    v.cutscene = CutscenePlaying();
    v.travelling = b2::RejoinRunning() || b2::GuestTravelPending() || (host && b2::HostRejoinRunning());
    return v;
}

void Send(party::PartyLink* link, bool host, const Msg& m) {
    if (!link) return;
    const std::string body = Encode(m);
    // A host addresses the member; a guest hands it to the host, who relays (and signs with the real sender).
    link->post_event(host ? m.to : party::kHostSlot, kEventName, body);
    Log("send %s id %u %d -> %d%s%s", OpWord(m.op), m.id, m.from, m.to,
        m.why != Why::None ? " why " : "", m.why != Why::None ? WhyWord(m.why) : "");
}

void ParseTest() {
    if (g_test_parsed) return;
    g_test_parsed = true;
    if (const char* f = std::getenv("BB_PARTY_GIVE_FAULT")) {
        g_fault = std::strcmp(f, "mute") == 0 ? 1 : std::strcmp(f, "linkdrop") == 0 ? 2 : 0;
        if (g_fault) Log("test fault: %s", f);
    }
    g_watch_gear = std::getenv("BB_PARTY_GEAR_WATCH") != nullptr;
    if (const char* w = std::getenv("BB_PARTY_GIVE_WATCH")) {
        for (const char* q = w; *q;) {
            g_watch.push_back(static_cast<u32>(std::strtoul(q, const_cast<char**>(&q), 10)));
            while (*q == ',') ++q;
        }
    }
    if (const char* pr = std::getenv("BB_PARTY_GEAR_PROBE")) {
        g_probe_at = std::atof(pr);
        Log("gear probe armed: %.0f s after the world is steady", g_probe_at);
    }
    // BB_PARTY_GEAR_SETUP="w<id>:<gems>@<s>,a<id>@<s>": a weapon with <gems> blood gems / an armor piece added to the own
    // inventory (the fixture of the smoke phases; the same code that receives a gift builds it).
    if (const char* su = std::getenv("BB_PARTY_GEAR_SETUP")) {
        const std::string str = su;
        std::size_t q = 0;
        while (q < str.size()) {
            std::size_t e = str.find(',', q);
            if (e == std::string::npos) e = str.size();
            const std::string it = str.substr(q, e - q);
            q = e + 1;
            unsigned id = 0;
            int gems = 0;
            double at = 0;
            if (std::sscanf(it.c_str(), "w%u:%d@%lf", &id, &gems, &at) == 3) {
                g_setup.push_back({Kind::Weapon, id, gems, at, false});
                Log("test aid: add weapon %u with %d gems at +%.0f s", id, gems, at);
            } else if (std::sscanf(it.c_str(), "a%u@%lf", &id, &at) == 2) {
                g_setup.push_back({Kind::Armor, id, 0, at, false});
                Log("test aid: add armor %u at +%.0f s", id, at);
            }
        }
    }
    const char* t = std::getenv("BB_PARTY_GIVE_TEST");
    if (!t) return;
    std::string s = t;
    std::size_t p = 0;
    while (p < s.size()) {
        std::size_t e = s.find(',', p);
        if (e == std::string::npos) e = s.size();
        const std::string item = s.substr(p, e - p);
        p = e + 1;
        unsigned goods = 0;
        int n = 0, to = 0;
        double at = 0;
        if (std::sscanf(item.c_str(), "gon@%lf", &at) == 1) {
            g_test.push_back({0, 0, 0, at, false, 3});
            Log("test aid: gear switch on at +%.0f s", at);
        } else if (std::sscanf(item.c_str(), "goff@%lf", &at) == 1) {
            g_test.push_back({0, 0, 0, at, false, 4});
            Log("test aid: gear switch off at +%.0f s", at);
        } else if (std::sscanf(item.c_str(), "w%u:%d@%lf", &goods, &to, &at) == 3) {
            g_test.push_back({goods, 1, to, at, false, 5});
            Log("test aid: give weapon %u to slot %d at +%.0f s", goods, to, at);
        } else if (std::sscanf(item.c_str(), "a%u:%d@%lf", &goods, &to, &at) == 3) {
            g_test.push_back({goods, 1, to, at, false, 6});
            Log("test aid: give armor %u to slot %d at +%.0f s", goods, to, at);
        } else if (std::sscanf(item.c_str(), "on@%lf", &at) == 1) {
            g_test.push_back({0, 0, 0, at, false, 1});
            Log("test aid: switch on at +%.0f s", at);
        } else if (std::sscanf(item.c_str(), "off@%lf", &at) == 1) {
            g_test.push_back({0, 0, 0, at, false, 2});
            Log("test aid: switch off at +%.0f s", at);
        } else if (std::sscanf(item.c_str(), "%u:%d:%d@%lf", &goods, &n, &to, &at) == 4) {
            g_test.push_back({goods, n, to, at, false, 0});
            Log("test aid: give %d of goods %u to slot %d at +%.0f s", n, goods, to, at);
        }
    }
}

/// Puts `n` back into the own inventory (a refund); logs the counts.
void Refund(u32 goods, int n, const char* why) {
    if (n <= 0) return;
    const int before = ItemCount(kItemGoods, goods);
    const int got = GiveGoodsToOwn(goods, n, false);
    Log("refund %d of goods %u (%s): held %d -> %d (arrived %d)", n, goods, why, before, ItemCount(kItemGoods, goods), got);
    RequestGameSave("give refund");
}

// ---- giver ----

void DoGiverStep(const Step& s, party::PartyLink* link, bool host, const GameSnapshot& gs, double now);

void AfterRemoved(int removed, party::PartyLink* link, bool host, const GameSnapshot& gs, double now) {
    const Step s = g_giver.Removed(removed, now);
    DoGiverStep(s, link, host, gs, now);
}

bool SameGear(const GearItem& a, const GearItem& b) {
    if (a.kind != b.kind || a.id != b.id || a.handle != b.handle || a.gem_count != b.gem_count || a.aux != b.aux) return false;
    for (int i = 0; i < kGearSlots; ++i) {
        if (a.masks[i] != b.masks[i]) return false;
    }
    for (int i = 0; i < a.gem_count; ++i) {
        if (!(a.gems[i] == b.gems[i])) return false;
    }
    return true;
}

/// Puts the gear (with its gems) back into the own inventory: a refund of a failed transfer. A refund that does not
/// verify is logged as an error (the one case in which an item could be lost; the log names the item).
void RefundGear(const char* why) {
    GearGiveResult r;
    const int got = GiveGearToOwn(g_gear, false, &r);
    Log("refund of %s %u (%s): arrived %d, gems placed %d loose %d lost %d%s", KindWord(g_gear.kind), g_gear.id, why, got,
        r.gems_placed, r.gems_loose, r.gems_lost, got == 1 && r.gems_lost == 0 ? "" : "  REFUND INCOMPLETE");
    RequestGameSave("give refund");
}

/// Giver of gear, after READY: the moment and the entry are checked again (it must be the same entry with the same gems),
/// then the entry is removed by its handle and must be gone.
void RemoveGear(party::PartyLink* link, bool host, const GameSnapshot& gs, double now) {
    Why why = Gate(ReadGate(gs, host, true), false, true);
    GearItem now_item;
    if (why == Why::None) {
        if (!ReadGearEntry(g_gear.kind, g_gear.id, g_gear.handle, &now_item) || !SameGear(now_item, g_gear)) {
            why = Why::NotHeld;  // gone or changed since the offer (a gem socketed, the weapon sold ...)
        } else {
            GearFacts f;
            f.kind = now_item.kind;
            f.id = now_item.id;
            f.held = true;
            f.equipped = now_item.equipped;
            why = GearGiverVerdict(f);
        }
    }
    int removed = 0;
    if (why == Why::None) {
        removed = TakeGearFromOwn(g_gear.handle) ? 1 : 0;
        Log("removed %s %u (handle %#x, %d gems): %s", KindWord(g_gear.kind), g_gear.id, g_gear.handle, g_gear.gem_count,
            removed ? "gone from the inventory" : "STILL IN THE INVENTORY");
    } else {
        Log("not removed: %s", WhyWord(why));
    }
    AfterRemoved(removed, link, host, gs, now);
}

void DoGiverStep(const Step& s, party::PartyLink* link, bool host, const GameSnapshot& gs, double now) {
    const u32 goods = g_giver.goods();
    switch (s.act) {
    case Act::None:
        return;
    case Act::Remove: {
        if (g_giver.kind() != Kind::Goods) {
            RemoveGear(link, host, gs, now);
            return;
        }
        // The moment and the holdings are checked again: the offer was a few frames ago.
        Why why = Gate(ReadGate(gs, host));
        GoodsFacts f;
        if (why == Why::None) {
            why = ReadGiveFacts(goods, &f) ? GiverVerdict(f, g_giver.n()) : Why::Failed;
        }
        int removed = 0;
        if (why == Why::None) {
            const int before = ItemCount(kItemGoods, goods);
            removed = TakeGoodsFromOwn(goods, g_giver.n());
            Log("removed goods %u x%d: held %d -> %d (removed %d)", goods, g_giver.n(), before, ItemCount(kItemGoods, goods),
                removed);
            if (removed < 0) removed = 0;
        } else {
            Log("not removed: %s", WhyWord(why));
        }
        AfterRemoved(removed, link, host, gs, now);
        return;
    }
    case Act::SendCommit: {
        Msg m;
        m.op = Op::Commit;
        m.id = g_giver.id();
        m.from = link ? link->local_slot() : 0;
        m.to = g_giver.to();
        Send(link, host, m);
        RequestGameSave("give removed");
        if (g_fault == 2 && link) {
            Log("test fault linkdrop: the connection is dropped after COMMIT");
            link->debug_drop_connections();
        }
        return;
    }
    case Act::SendAbort: {
        Msg m;
        m.op = Op::Abort;
        m.id = g_giver.id();
        m.from = link ? link->local_slot() : 0;
        m.to = g_giver.to();
        Send(link, host, m);
        const bool gear = g_giver.kind() != Kind::Goods;
        if (s.refund > 0) {
            if (gear) RefundGear("removal did not verify");
            else Refund(goods, s.refund, "removal did not verify");
        }
        if (s.why != Why::None) {  // the removal did not verify (an ABORT asking a receiver has no reason)
            Notice(RefusedText(gear ? GearLabel(g_gear) : ItemName(goods), MemberName(link, m.to), s.why));
        }
        return;
    }
    case Act::Finished: {
        const int to = g_giver.to();
        const bool gear = g_giver.kind() != Kind::Goods;
        if (s.refund > 0) {
            if (gear) RefundGear("receiver took none");
            else Refund(goods, s.refund, "receiver took fewer");
        }
        const std::string label = gear ? GearLabel(g_gear) : ItemName(goods);
        if (s.got > 0) {
            Notice(GaveLine(label, gear, s.got, MemberName(link, to)));
        } else {
            Notice(RefusedText(label, MemberName(link, to), s.why));
        }
        Log("transfer ended: got %d of %d, refund %d", s.got, s.got + s.refund, s.refund);
        RequestGameSave("give done");
        return;
    }
    case Act::Failed:
        Notice(RefusedText(g_giver.kind() != Kind::Goods ? GearLabel(g_gear) : ItemName(goods), MemberName(link, g_giver.to()), s.why));
        return;
    case Act::RolledBack:
        if (g_giver.kind() != Kind::Goods) {
            RefundGear("no confirmation");
            Notice(RefusedText(GearLabel(g_gear), MemberName(link, g_giver.to()), Why::RolledBack));
            Log("transfer of %s %u rolled back (no DONE from the receiver)", KindWord(g_giver.kind()), goods);
            return;
        }
        Refund(goods, s.refund, "no confirmation");
        Notice(RefusedText(ItemName(goods), MemberName(link, g_giver.to()), Why::RolledBack));
        Log("transfer of goods %u x%d rolled back (no DONE from the receiver)", goods, s.refund);
        return;
    }
}

/// The own gear entry a request names: its handle when given, else the first giveable, unworn entry with that id.
bool FindOwnGear(const Request& r, GearItem* out) {
    if (r.handle) return ReadGearEntry(r.kind, r.goods, r.handle, out);
    for (const GearItem& g : OwnGear()) {
        if (g.kind != r.kind || g.id != r.goods || g.equipped) continue;
        *out = g;
        return true;
    }
    return false;
}

u32 NextTransferId() {
    if (g_next_id == 0) {
        g_next_id = static_cast<u32>(std::chrono::steady_clock::now().time_since_epoch().count() & 0x3fffffff) | 1u;
    }
    return ++g_next_id;
}

void StartGiveGear(const Request& r, const GameSnapshot& gs, party::PartyLink* link, bool host, double now) {
    const std::string item = GearName(r.kind, r.goods);
    const int me = link ? link->local_slot() : -1;
    auto refuse = [&](Why w) {
        Notice(RefusedText(item, MemberName(link, r.to), w));
        Log("request %s %u to slot %d refused: %s", KindWord(r.kind), r.goods, r.to, WhyWord(w));
    };
    if (!link || me < 0) return refuse(Why::NoMember);
    if (r.to == me) return refuse(Why::Self);
    if (!MemberConnected(link, r.to)) return refuse(Why::NoMember);
    if (g_giver.Busy()) return refuse(Why::Busy);
    if (now - g_last_give_at < kCooldownS) return refuse(Why::Cooldown);
    Why w = Gate(ReadGate(gs, host, true), false, true);
    if (w != Why::None) return refuse(w);
    GearItem it;
    if (!FindOwnGear(r, &it)) return refuse(Why::NotHeld);
    GearFacts f;
    f.kind = it.kind;
    f.id = it.id;
    f.held = true;
    f.equipped = it.equipped;
    w = GearGiverVerdict(f);
    if (w != Why::None) return refuse(w);
    const u32 id = NextTransferId();
    g_last_give_at = now;
    if (!g_giver.Begin(id, r.to, it.id, 1, now, it.kind)) return refuse(Why::Busy);
    g_gear = it;
    Msg m;
    m.op = Op::Offer;
    m.id = id;
    m.from = me;
    m.to = r.to;
    m.goods = it.id;
    m.n = 1;
    m.kind = it.kind;
    m.gem_count = it.gem_count;
    for (int i = 0; i < it.gem_count; ++i) m.gems[i] = it.gems[i];
    for (int i = 0; i < kGearSlots; ++i) m.masks[i] = it.masks[i];
    m.aux = it.aux;
    Send(link, host, m);
    Log("offer id %u: %s %u (handle %#x, %d gems) to slot %d", id, KindWord(it.kind), it.id, it.handle, it.gem_count, r.to);
}

void StartGive(const Request& r, const GameSnapshot& gs, party::PartyLink* link, bool host, double now) {
    if (r.kind != Kind::Goods) return StartGiveGear(r, gs, link, host, now);
    const std::string item = ItemName(r.goods);
    const int me = link ? link->local_slot() : -1;
    auto refuse = [&](Why w) {
        Notice(RefusedText(item, MemberName(link, r.to), w));
        Log("request goods %u x%d to slot %d refused: %s", r.goods, r.n, r.to, WhyWord(w));
    };
    if (!link || me < 0) return refuse(Why::NoMember);
    if (r.to == me) return refuse(Why::Self);
    if (!MemberConnected(link, r.to)) return refuse(Why::NoMember);
    if (g_giver.Busy()) return refuse(Why::Busy);
    if (now - g_last_give_at < kCooldownS) return refuse(Why::Cooldown);
    Why w = Gate(ReadGate(gs, host));
    if (w != Why::None) return refuse(w);
    GoodsFacts f;
    if (!ReadGiveFacts(r.goods, &f)) return refuse(Why::Failed);
    w = GiverVerdict(f, r.n);
    if (w != Why::None) return refuse(w);
    if (g_next_id == 0) {
        g_next_id = static_cast<u32>(std::chrono::steady_clock::now().time_since_epoch().count() & 0x3fffffff) | 1u;
    }
    const u32 id = ++g_next_id;
    g_last_give_at = now;
    if (!g_giver.Begin(id, r.to, r.goods, r.n, now)) return refuse(Why::Busy);
    Msg m;
    m.op = Op::Offer;
    m.id = id;
    m.from = me;
    m.to = r.to;
    m.goods = r.goods;
    m.n = r.n;
    Send(link, host, m);
    Log("offer id %u: goods %u x%d (held %d) to slot %d", id, r.goods, r.n, f.held, r.to);
}

// ---- receiver ----

void ReplyDone(party::PartyLink* link, bool host, const RStep& d) {
    Msg m;
    m.op = Op::Done;
    m.id = d.id;
    m.from = link ? link->local_slot() : 0;
    m.to = d.from;
    m.got = d.got;
    m.why = d.why;
    Send(link, host, m);
}

void HandleGearOffer(const Msg& m, const GameSnapshot& gs, party::PartyLink* link, bool host, double now) {
    Msg r;
    r.id = m.id;
    r.from = link->local_slot();
    r.to = m.from;
    Why why = Gate(ReadGate(gs, host, true), true, true);
    GearFacts f;
    f.kind = m.kind;
    f.id = m.goods;
    f.held = true;
    f.free_slots = GearFreeSlots();
    if (why == Why::None) {
        why = GearReceiverVerdict(f, m.gem_count, m.gems, m.masks, m.aux);
    }
    if (why == Why::None && !GearBuildable(m.kind, m.goods, m.gem_count, m.gems)) why = Why::BadGems;
    if (why == Why::None) {
        switch (g_recv.Reserve(m.from, m.id, m.goods, 1, now, m.kind)) {
        case Receiver::Result::Duplicate:
            Log("offer id %u from slot %d seen before: ignored", m.id, m.from);
            return;
        case Receiver::Result::Busy:
            why = Why::Busy;
            break;
        case Receiver::Result::Ok:
            g_recv_gear = m;
            break;
        }
    }
    if (why != Why::None) {
        r.op = Op::Refuse;
        r.why = why;
        Log("offer id %u %s %u from slot %d refused: %s", m.id, KindWord(m.kind), m.goods, m.from, WhyWord(why));
    } else {
        r.op = Op::Ready;
        r.room = f.free_slots;
        Log("offer id %u %s %u (%d gems) from slot %d accepted (free slots %d)", m.id, KindWord(m.kind), m.goods, m.gem_count,
            m.from, f.free_slots);
    }
    Send(link, host, r);
}

void HandleOffer(const Msg& m, const GameSnapshot& gs, party::PartyLink* link, bool host, double now) {
    if (m.kind != Kind::Goods) return HandleGearOffer(m, gs, link, host, now);
    Msg r;
    r.id = m.id;
    r.from = link->local_slot();
    r.to = m.from;
    Why why = Gate(ReadGate(gs, host), true);
    int room = 0;
    GoodsFacts f;
    if (why == Why::None) {
        why = ReadGiveFacts(m.goods, &f) ? ReceiverVerdict(f, m.n, GiveStorageRoom(m.goods), &room) : Why::Failed;
    }
    if (why == Why::None) {
        switch (g_recv.Reserve(m.from, m.id, m.goods, m.n, now)) {
        case Receiver::Result::Duplicate:
            Log("offer id %u from slot %d seen before: ignored", m.id, m.from);
            return;
        case Receiver::Result::Busy:
            why = Why::Busy;
            break;
        case Receiver::Result::Ok:
            break;
        }
    }
    if (why != Why::None) {
        r.op = Op::Refuse;
        r.why = why;
        Log("offer id %u goods %u x%d from slot %d refused: %s", m.id, m.goods, m.n, m.from, WhyWord(why));
    } else {
        r.op = Op::Ready;
        r.room = room;
        Log("offer id %u goods %u x%d from slot %d accepted (held %d, room %d)", m.id, m.goods, m.n, m.from, f.held, room);
    }
    Send(link, host, r);
}

void HandleCommit(const Msg& m, const GameSnapshot& gs, party::PartyLink* link, bool host, double now) {
    if (g_fault == 1) {
        Log("test fault mute: COMMIT id %u ignored", m.id);
        return;
    }
    RStep st = g_recv.OnCommit(m.from, m.id, now);
    if (st.act == RAct::Apply && st.kind != Kind::Goods) {
        // Gear: the moment and the room again, then the weapon / armor is built with its gems.
        const Msg offer = g_recv_gear;
        Why why = Gate(ReadGate(gs, host, true), true, true);
        GearFacts f;
        f.kind = st.kind;
        f.id = st.goods;
        f.held = true;
        f.free_slots = GearFreeSlots();
        if (why == Why::None) why = GearReceiverVerdict(f, offer.gem_count, offer.gems, offer.masks, offer.aux);
        if (why == Why::None && (offer.id != m.id || offer.goods != st.goods)) why = Why::Failed;
        int got = 0;
        if (why == Why::None) {
            GearItem want;
            want.kind = st.kind;
            want.id = st.goods;
            want.gem_count = offer.gem_count;
            for (int i = 0; i < offer.gem_count; ++i) want.gems[i] = offer.gems[i];
            for (int i = 0; i < kGearSlots; ++i) want.masks[i] = offer.masks[i];
            want.aux = offer.aux;
            GearGiveResult res;
            const int r = GiveGearToOwn(want, true, &res);
            Log("applied %s %u from slot %d: arrived %d (handle %#x), gems placed %d loose %d lost %d", KindWord(st.kind),
                st.goods, m.from, res.arrived, res.handle, res.gems_placed, res.gems_loose, res.gems_lost);
            got = r == 1 ? 1 : 0;
            if (got == 0) why = Why::Failed;
        } else {
            Log("COMMIT id %u: not applied now: %s", m.id, WhyWord(why));
        }
        st = g_recv.Applied(got, why);
        if (got > 0) {
            Notice(GotLine(GearLabel(offer), true, 1, MemberName(link, m.from)));
            RequestGameSave("give received");
        }
        if (st.act == RAct::SendDone) ReplyDone(link, host, st);
        return;
    }
    if (st.act == RAct::Apply) {
        // The moment and the room again: the player may have loaded, died or picked up since READY.
        Why why = Gate(ReadGate(gs, host), true);
        GoodsFacts f;
        int room = 0;
        if (why == Why::None) {
            why = ReadGiveFacts(st.goods, &f) ? ReceiverVerdict(f, st.n, GiveStorageRoom(st.goods), &room) : Why::Failed;
        }
        int got = 0;
        if (why == Why::None) {
            const int inv0 = ItemCount(kItemGoods, st.goods), box0 = StorageItemCount(kItemGoods, st.goods);
            got = GiveGoodsToOwn(st.goods, st.n, true);
            Log("applied goods %u x%d from slot %d: inventory %d -> %d, storage %d -> %d (arrived %d)", st.goods, st.n, m.from,
                inv0, ItemCount(kItemGoods, st.goods), box0, StorageItemCount(kItemGoods, st.goods), got);
            if (got < 0) {
                got = 0;
                why = Why::Failed;
            } else if (got < st.n) {
                why = Why::NoRoom;
            }
            if (got > st.n) got = st.n;
        } else {
            Log("COMMIT id %u: not applied now: %s", m.id, WhyWord(why));
        }
        st = g_recv.Applied(got, why);
        if (got > 0) {
            Notice(GotText(ItemName(st.goods), got, MemberName(link, m.from)));
            RequestGameSave("give received");
        }
    }
    if (st.act == RAct::SendDone) {
        ReplyDone(link, host, st);
    }
}

// ---- the inbound dispatch ----

void Handle(int sender, Msg m, const GameSnapshot& gs, party::PartyLink* link, bool host, double now) {
    const int me = link->local_slot();
    if (host) {
        // The sender is who the link says it is.
        if (sender != 0) m.from = sender;
        if (m.to != me) {
            if (m.to == m.from || !MemberConnected(link, m.to)) {
                Log("relay %s id %u from slot %d to slot %d dropped: %s", OpWord(m.op), m.id, m.from, m.to,
                    m.to == m.from ? "to itself" : "not connected");
                if (m.op == Op::Offer) {  // the giver learns it at once
                    Msg r;
                    r.op = Op::Refuse;
                    r.id = m.id;
                    r.from = m.to;
                    r.to = m.from;
                    r.why = m.to == m.from ? Why::Self : Why::NoMember;
                    link->post_event(m.from, kEventName, Encode(r));
                }
                return;
            }
            link->post_event(m.to, kEventName, Encode(m));
            Log("relay %s id %u slot %d -> %d", OpWord(m.op), m.id, m.from, m.to);
            return;
        }
    } else if (m.to != me) {
        Log("%s id %u for slot %d is not ours (slot %d): dropped", OpWord(m.op), m.id, m.to, me);
        return;
    }
    Log("recv %s id %u from slot %d", OpWord(m.op), m.id, m.from);
    switch (m.op) {
    case Op::Offer:
        HandleOffer(m, gs, link, host, now);
        return;
    case Op::Ready:
        DoGiverStep(g_giver.OnReady(m.id, m.from, now), link, host, gs, now);
        return;
    case Op::Refuse:
        DoGiverStep(g_giver.OnRefuse(m.id, m.from, m.why), link, host, gs, now);
        return;
    case Op::Done:
        DoGiverStep(g_giver.OnDone(m.id, m.from, m.got, m.why), link, host, gs, now);
        return;
    case Op::Commit:
        HandleCommit(m, gs, link, host, now);
        return;
    case Op::Abort: {
        if (g_fault == 1) {
            Log("test fault mute: ABORT id %u ignored", m.id);
            return;
        }
        const RStep d = g_recv.OnAbort(m.from, m.id);
        if (d.act == RAct::SendDone) ReplyDone(link, host, d);
        return;
    }
    }
}

}  // namespace

bool Enabled() {
    static const bool env = [] {
        const char* e = std::getenv("BB_PARTY_GIVE_ITEMS");
        return e && std::strcmp(e, "1") == 0;
    }();
    return env || BbSettings::Get().party_give_items.load();
}

bool EnabledGear() {
    static const bool env = [] {
        const char* e = std::getenv("BB_PARTY_GIVE_GEAR");
        return e && std::strcmp(e, "1") == 0;
    }();
    return env || BbSettings::Get().party_give_gear.load();
}

namespace {
bool GearCategory(std::uint32_t item, Kind* kind) {
    const std::uint32_t c = item & 0xf0000000u;
    if (c == 0x00000000u) {
        *kind = Kind::Weapon;
        return true;
    }
    if (c == 0x10000000u) {
        *kind = Kind::Armor;
        return true;
    }
    return false;
}
/// The inventory entry an item menu names (type bits | id at +0x54, inventory index at +0x50): the entry at that index when
/// it holds the item, else the only entry that does (two copies of one weapon are told apart by the index only).
bool MenuGear(std::uint32_t item, std::uint32_t index, GearItem* out) {
    Kind kind;
    if (!GearCategory(item, &kind)) return false;
    const std::uint32_t id = item & 0x0fffffffu;
    std::vector<GearItem> same;
    for (const GearItem& g : OwnGear()) {
        if (g.kind == kind && g.id == id) {
            if (g.index == static_cast<int>(index)) {
                *out = g;
                return true;
            }
            same.push_back(g);
        }
    }
    if (same.size() == 1) {
        *out = same[0];
        return true;
    }
    return false;
}
bool OtherMemberConnected() {
    const party::status::Board b = party::status::Snapshot();
    if (!b.enabled) return false;
    for (const party::status::Member& m : b.members) {
        if (!m.local && m.connected) return true;
    }
    return false;
}
}  // namespace

bool MenuOffersGear(std::uint32_t item, std::uint32_t index) {
    Kind kind;
    if (!EnabledGear() || !GearCategory(item, &kind) || !OtherMemberConnected()) return false;
    GearItem g;
    if (!MenuGear(item, index, &g)) return false;
    GearFacts f;
    f.kind = g.kind;
    f.id = g.id;
    f.held = true;
    f.equipped = g.equipped;
    return GearGiverVerdict(f) == Why::None;
}

bool MenuIsGear(std::uint32_t item) {
    Kind kind;
    return GearCategory(item, &kind);
}

void MenuGiveGear(std::uint32_t item, std::uint32_t index, int slot) {
    Kind kind;
    GearItem g;
    if (!GearCategory(item, &kind) || !MenuGear(item, index, &g)) {
        Log("item menu: give gear %#x (index %u): the entry is gone", item, index);
        return;
    }
    Log("item menu: give %s %u (handle %#x) to slot %d", KindWord(kind), g.id, g.handle, slot);
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_requests.size() < 4) g_requests.push_back({g.id, 1, slot, kind, g.handle});
}

bool MenuOffers(std::uint32_t item) {
    if (!Enabled() || (item & 0xf0000000u) != 0x40000000u) return false;
    const party::status::Board b = party::status::Snapshot();
    if (!b.enabled) return false;
    int others = 0;
    for (const party::status::Member& m : b.members) {
        if (!m.local && m.connected) ++others;
    }
    if (others == 0) return false;
    GoodsFacts f;
    if (!ReadGiveFacts(item & 0x0fffffffu, &f)) return false;
    return GiverVerdict(f, 1) == Why::None;
}

std::vector<std::pair<int, std::string>> MenuMembers() {
    std::vector<std::pair<int, std::string>> out;
    const party::status::Board b = party::status::Snapshot();
    for (const party::status::Member& m : b.members) {
        if (!m.local && m.connected && m.slot >= 0) out.emplace_back(m.slot, b.ShownName(m.name));
    }
    return out;
}

std::vector<int> MenuAmounts(std::uint32_t item) {
    std::vector<int> out;
    GoodsFacts f;
    if (!ReadGiveFacts(item & 0x0fffffffu, &f)) return out;
    int most = std::min(f.held, kMaxCount);
    if (f.quick) most = std::min(most, f.held - 1);
    if (most < 1 || GiverVerdict(f, 1) != Why::None) return out;
    for (const int n : {1, 2, 5, 10, 20, 50}) {
        if (n < most) out.push_back(n);
    }
    out.push_back(most);
    return out;
}

void MenuGive(std::uint32_t item, int n, int slot) {
    Log("item menu: give %d of item %#x to slot %d", n, item, slot);
    RequestGive(item & 0x0fffffffu, n, slot);
}

void MenuChosen(std::uint32_t item, std::uint32_t index) {
    Log("item menu: Give chosen for item %#x (inventory index %u)", item, index);
}

void RequestGive(std::uint32_t goods, int n, int to_slot) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_requests.size() < 4) g_requests.push_back({goods, n, to_slot});
}

void OnEvent(int slot, const std::string& body) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_in.size() < kMaxQueued) g_in.emplace_back(slot, body);
}

void Tick(const GameSnapshot& gs, party::PartyLink* link, bool host) {
    ParseTest();
    EnsureGearTable();
    std::deque<std::pair<int, std::string>> in;
    std::deque<Request> reqs;
    const double now = Now();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        in.swap(g_in);
        reqs.swap(g_requests);
    }
    if (!link || link->local_slot() < 0) {
        return;
    }
    // A switch that is off: events are answered by the gate (a receiver refuses "their switch is off"), no UI.
    for (auto& [sender, body] : in) {
        Msg m;
        if (!Decode(body, &m)) {
            Log("bad event from slot %d dropped", sender);
            continue;
        }
        Handle(sender, m, gs, link, host, now);
    }
    // A member that left ends what hangs on it.
    if (g_giver.Busy()) {
        if (!MemberConnected(link, g_giver.to())) {
            DoGiverStep(g_giver.OnMemberGone(g_giver.to()), link, host, gs, now);
        }
        const bool link_up = MemberConnected(link, g_giver.to()) &&
                             (host || link->state() == party::LinkState::Connected);
        DoGiverStep(g_giver.Tick(now, link_up), link, host, gs, now);
    }
    g_recv.Tick(now);
    for (const Request& r : reqs) {
        StartGive(r, gs, link, host, now);
    }
    // Test aid: a give requested a while after the world is steady.
    if (!g_test.empty()) {
        // The test clock runs while the world is steady and another member is connected.
        bool others = false;
        for (const party::RosterEntry& e : link->roster()) {
            if (e.slot != link->local_slot() && e.connected) others = true;
        }
        GateView gv = ReadGate(gs, host);
        gv.enabled = true;  // the switch itself is what the tokens test
        const bool steady = others && Gate(gv) == Why::None;
        if (!steady) g_steady_since = -1;
        else if (g_steady_since < 0) g_steady_since = now;
        for (TestGive& t : g_test) {
            if (!t.done && g_steady_since >= 0 && now - g_steady_since >= t.at) {
                t.done = true;
                if (t.kind == 1 || t.kind == 2) {
                    BbSettings::Get().party_give_items.store(t.kind == 1);
                    Log("test aid: switch %s", t.kind == 1 ? "on" : "off");
                } else if (t.kind == 3 || t.kind == 4) {
                    BbSettings::Get().party_give_gear.store(t.kind == 3);
                    Log("test aid: gear switch %s", t.kind == 3 ? "on" : "off");
                } else if (t.kind == 5 || t.kind == 6) {
                    Request gr;
                    gr.goods = t.goods;
                    gr.n = 1;
                    gr.to = t.to;
                    gr.kind = t.kind == 5 ? Kind::Weapon : Kind::Armor;
                    Log("test aid: give %s %u to slot %d", KindWord(gr.kind), t.goods, t.to);
                    StartGive(gr, gs, link, host, now);
                } else {
                    Log("test aid: give goods %u x%d to slot %d", t.goods, t.n, t.to);
                    StartGive({t.goods, t.n, t.to}, gs, link, host, now);
                }
            }
        }
    }
    if (!g_setup.empty() || g_probe_at >= 0) {
        GateView gv = ReadGate(gs, host);
        gv.enabled = true;
        if (Gate(gv) != Why::None) {
            g_gear_steady = -1;
        } else if (g_gear_steady < 0) {
            g_gear_steady = now;
        }
        for (TestSetup& t : g_setup) {
            if (t.done || g_gear_steady < 0 || now - g_gear_steady < t.at) continue;
            t.done = true;
            GearItem want;
            want.kind = t.kind;
            want.id = t.id;
            GearGiveResult res;
            const int r = GiveGearToOwn(want, false, &res);
            GearGem fix[kGearSlots];
            std::uint32_t fm[kGearSlots] = {};
            std::uint32_t fa = 0;
            const int placed = r == 1 && t.kind == Kind::Weapon && t.gems > 0
                                   ? GearFixtureGems(res.handle, std::min(t.gems, kGearSlots), fix, fm, &fa) : 0;
            for (int i = 0; i < placed; ++i) {
                Log("test setup gem: slot %d id %d data %d %d %d %d %d %d %d %d", fix[i].slot, fix[i].id, fix[i].data[0],
                    fix[i].data[1], fix[i].data[2], fix[i].data[3], fix[i].data[4], fix[i].data[5], fix[i].data[6], fix[i].data[7]);
            }
            Log("test setup: %s %u with %d gems: result %d (handle %#x), gems placed %d", KindWord(t.kind), t.id, placed, r,
                res.handle, placed);
        }
        if (g_probe_at >= 0 && g_gear_steady >= 0 && now - g_gear_steady >= g_probe_at) {
            g_probe_at = now - g_gear_steady + 15;  // again 15 s later
            GearProbe();
        }
    }
    if (!g_watch.empty() && now >= g_next_watch) {
        g_next_watch = now + 10.0;
        for (const u32 g : g_watch) {
            Log("holdings goods %u = %d (storage %d)", g, ItemCount(kItemGoods, g), StorageItemCount(kItemGoods, g));
        }
        if (g_watch_gear) {
            int w = 0, a = 0;
            for (const GearItem& g : OwnGear()) (g.kind == Kind::Weapon ? w : a)++;
            Log("holdings gear: %d weapons, %d armor, free slots %d", w, a, GearFreeSlots());
        }
    }
}

}  // namespace coop::give
