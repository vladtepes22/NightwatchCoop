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
};
std::deque<Request> g_requests;  // under g_mu

// main thread only
Giver g_giver;
Receiver g_recv;
double g_last_give_at = -1e9;
u32 g_next_id = 0;
double g_steady_since = -1;
bool g_test_parsed = false;
struct TestGive {
    u32 goods = 0;
    int n = 0, to = 0;
    double at = 0;
    bool done = false;
    int kind = 0;  // 0 give, 1 switch on, 2 switch off
};
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

GateView ReadGate(const GameSnapshot& s, bool host) {
    GateView v;
    v.enabled = Enabled();
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
    if (const char* w = std::getenv("BB_PARTY_GIVE_WATCH")) {
        for (const char* q = w; *q;) {
            g_watch.push_back(static_cast<u32>(std::strtoul(q, const_cast<char**>(&q), 10)));
            while (*q == ',') ++q;
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
        if (std::sscanf(item.c_str(), "on@%lf", &at) == 1) {
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

void DoGiverStep(const Step& s, party::PartyLink* link, bool host, const GameSnapshot& gs, double now) {
    const u32 goods = g_giver.goods();
    switch (s.act) {
    case Act::None:
        return;
    case Act::Remove: {
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
        if (s.refund > 0) {
            Refund(goods, s.refund, "removal did not verify");
        }
        if (s.why != Why::None) {  // the removal did not verify (an ABORT asking a receiver has no reason)
            Notice(RefusedText(ItemName(goods), MemberName(link, m.to), s.why));
        }
        return;
    }
    case Act::Finished: {
        const int to = g_giver.to();
        if (s.refund > 0) {
            Refund(goods, s.refund, "receiver took fewer");
        }
        if (s.got > 0) {
            Notice(GaveText(ItemName(goods), s.got, MemberName(link, to)));
        } else {
            Notice(RefusedText(ItemName(goods), MemberName(link, to), s.why));
        }
        Log("transfer ended: got %d of %d, refund %d", s.got, s.got + s.refund, s.refund);
        RequestGameSave("give done");
        return;
    }
    case Act::Failed:
        Notice(RefusedText(ItemName(goods), MemberName(link, g_giver.to()), s.why));
        return;
    case Act::RolledBack:
        Refund(goods, s.refund, "no confirmation");
        Notice(RefusedText(ItemName(goods), MemberName(link, g_giver.to()), Why::RolledBack));
        Log("transfer of goods %u x%d rolled back (no DONE from the receiver)", goods, s.refund);
        return;
    }
}

void StartGive(const Request& r, const GameSnapshot& gs, party::PartyLink* link, bool host, double now) {
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

void HandleOffer(const Msg& m, const GameSnapshot& gs, party::PartyLink* link, bool host, double now) {
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
                } else {
                    Log("test aid: give goods %u x%d to slot %d", t.goods, t.n, t.to);
                    StartGive({t.goods, t.n, t.to}, gs, link, host, now);
                }
            }
        }
    }
    if (!g_watch.empty() && now >= g_next_watch) {
        g_next_watch = now + 10.0;
        for (const u32 g : g_watch) {
            Log("holdings goods %u = %d (storage %d)", g, ItemCount(kItemGoods, g), StorageItemCount(kItemGoods, g));
        }
    }
}

}  // namespace coop::give
