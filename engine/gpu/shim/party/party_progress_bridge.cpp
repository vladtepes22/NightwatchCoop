// SPDX-License-Identifier: GPL-3.0-or-later
// C2 progress sync <-> PartyDirector / PartyLink glue (party_progress.h, DirectorTick). Kept out
// of party_director.cpp so the director only calls one function per frame.
//
// Host:  every frame, the captured flag changes go to every member (EVENT "flags", reliable:
//        replayed to a member that resumes); each member that is newly connected (a new slot, a
//        resumed slot: connected false -> true, or another name in the slot) gets the full
//        snapshot (EVENT "flag_snapshot") once the host has a baseline (retried every frame until
//        then). Session boundaries: hosting started / first member connected -> "host",
//        last member gone -> ended.
// Guest: entering the host's world (session role Client) -> "guest", leaving it -> ended. The
//        EVENTs themselves arrive in party_runtime's on_event (progress::OnLinkEvent).
#include "party_progress.h"
#include "party_async_io.h"
#include "party_desync.h"
#include "party_balance.h"
#include "party_kill_record.h"
#include "party_status.h"
#include "../bbport_settings.h"

#include "game_state.h"
#include "party_link.h"
#include "party_perf.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <iterator>
#include <chrono>
#include <map>
#include <set>
#include <string>

namespace coop::progress {
namespace {

void BLog(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void BLog(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    perf::Scope z(perf::Zone::Log);
    coop::asyncio::Print(std::string("Party progress: ") + line + "\n"); // never blocks the caller (party_async_io.h)
}

struct Bridge {  // main thread only
    party::PartyLink* link = nullptr;
    bool hosting_seen = false;
    bool host_session = false;           // at least one member connected
    bool guest_in_world = false;
    std::map<int, std::string> connected;  // slot -> name, members connected at the last tick
    std::set<int> want_snapshot;
    bool waiting_logged = false;
    int host_past_prologue = -1;  // host: 12410000 && 9401 in its own world at the last check (-1 unknown)
    double prologue_checked = 0;
    // F5 desync detector (party_desync.h): host digest clock, resync limits.
    double desync_period = -1;  // -1: not read yet
    double desync_next = 0;
    u64 desync_sent_seq = 0;    // the last flag change seq sent to the party
    bool desync_seq_known = false;
    desync::ServeLimit desync_serve;
};

double NowS() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// The loaded map's enemies by map entity id (the enemy census, seamless research #3): false
/// without a steady world. Main thread.
bool EnemyCensusNow(u32* map, std::vector<desync::CensusEntry>* out) {
    const GameSnapshot g = ReadGameState(false);
    if (!g.world_up || g.loading || !g.player || g.map_id == 0xffffffffu) {
        return false;
    }
    out->clear();
    for (const EnemyKillCandidate& e : balance::LoadedEnemies()) {
        out->push_back({e.entity, e.alive});
    }
    *map = g.map_id;
    return true;
}

// Host: the periodic digest to every member, and the resyncs members asked for (main thread,
// right after this frame's changes went out, so the digest matches the stream).
void HostDesync(Bridge& b, party::PartyLink* link, const std::map<int, std::string>& connected) {
    if (b.desync_period < 0) {
        b.desync_period = desync::PeriodFromEnv();
        BLog("desync check: %s", b.desync_period > 0 ? ("a digest every " + std::to_string(static_cast<int>(b.desync_period)) + " s").c_str() : "off");
    }
    const double now = NowS();
    for (auto& [slot, rq] : desync::HostPopResyncs()) {
        if (!connected.count(slot)) {
            continue;
        }
        if (!b.desync_serve.Take(slot, now)) {
            BLog("desync: slot %d asked again within %.0f s: not served", slot, desync::kServeGapS);
            continue;
        }
        if (rq.epoch != HostEpoch()) {
            BLog("desync: slot %d asked for another session's state: full snapshot", slot);
            b.want_snapshot.insert(slot);
            continue;
        }
        std::size_t groups = 0;
        if (!rq.world.empty()) {
            WorldImage full;
            if (HostWorldImage(&full)) {
                const WorldImage part = desync::WorldBuckets(full, rq.world);
                groups = part.groups.size();
                link->post_event(slot, kEventWorldGroups, WorldImageToJson(part));
            }
        }
        bool snap_sent = false;
        if (!rq.flags.empty() || rq.gap) {
            FlagSnapshot snap;
            if (FullSnapshot(&snap)) {
                link->post_event(slot, kEventFlagSnapshot, SnapshotToJson(snap));
                snap_sent = true;
            }
        }
        BLog("desync: resync for slot %d: %zu world group(s) of %zu bucket(s), flag snapshot %s (%zu flag bucket(s)%s)",
             slot, groups, rq.world.size(), snap_sent ? "sent" : "not needed", rq.flags.size(), rq.gap ? ", seq gap" : "");
    }
    // Enemy census asks (a guest's census differed from the digest): the entries of those buckets.
    for (auto& [slot, ask] : desync::HostPopCensusAsks()) {
        if (!connected.count(slot)) {
            continue;
        }
        u32 map = 0;
        std::vector<desync::CensusEntry> list;
        if (!EnemyCensusNow(&map, &list) || map != ask.first) {
            BLog("enemy census: slot %d asked about %s; this world is %s now: not answered", slot,
                 MapName(ask.first).c_str(), MapName(map).c_str());
            continue;
        }
        const desync::CensusList l = desync::CensusListOf(map, list, ask.second);
        link->post_event(slot, desync::kCensusListEvent, desync::CensusListToJson(l));
        BLog("enemy census: %zu entries of %zu bucket(s) sent to slot %d", l.entries.size(), l.buckets.size(), slot);
    }
    // A member whose snapshot is due this frame would compare against its previous connection's model.
    if (b.desync_period <= 0 || connected.empty() || now < b.desync_next || !b.want_snapshot.empty()) {
        return;
    }
    b.desync_next = now + b.desync_period;
    desync::Digest d;
    d.epoch = HostEpoch();
    FlagSnapshot snap;
    if (FullSnapshot(&snap) && (!b.desync_seq_known || snap.seq == b.desync_sent_seq)) {
        // Every change up to snap.seq is in the stream already (the drain above): comparable.
        d.has_flags = true;
        d.seq = snap.seq;
        d.flags = desync::FlagsDigest(snap.blocks);
    }
    WorldImage image;
    if (HostWorldImage(&image)) {
        d.has_world = true;
        d.world = desync::WorldDigest(image);
    }
    {
        u32 map = 0;
        std::vector<desync::CensusEntry> list;
        if (EnemyCensusNow(&map, &list)) {
            d.census = desync::MakeCensusDigest(map, list);
        }
    }
    if (d.has_flags || d.has_world) {
        link->post_event(party::kBroadcast, desync::kDigestEvent, desync::DigestToJson(d));
    }
}

// ---- The party's kill record (party_kill_record.h) ----
struct KillRecordGame {
    killrec::KillRecord rec;
    bool started = false;  // host: its first epoch; guest: ran once
    bool on = false;       // the switch at the last tick
    bool was_down = true;  // loading / no world at the last tick
    bool was_active = false;
    std::set<int> members;  // host: the members its full record went to
    double next = 0;
    u64 respawns = 0, remote = 0, sent = 0;
};
KillRecordGame& KR() {
    static KillRecordGame k;
    return k;
}

std::string IdList(const std::vector<u32>& v) {
    std::string o;
    for (std::size_t i = 0; i < v.size() && i < 12; ++i) o += (i ? " " : "") + std::to_string(v[i]);
    if (v.size() > 12) o += " ...";
    return o;
}

void PostKills(party::PartyLink* link, int slot, const std::vector<u32>& ids) {
    desync::KillRecordMsg m;
    m.epoch = static_cast<u32>(KR().rec.epoch());
    m.kills = ids;
    if (m.kills.size() > desync::kKillRecordMaxIds) m.kills.resize(desync::kKillRecordMaxIds);
    link->post_event(slot, desync::kKillRecordEvent, desync::KillRecordToJson(m));
}

/// Every frame in a party session (host: a member connected; guest: in the host's world): the
/// party's kill record. `members`: the host's connected member slots (a new one gets the record).
void KillRecordTick(party::PartyLink* link, bool host, bool active, const std::map<int, std::string>& members) {
    // The host's rule (System > Party rules: World, bbport.ini party_kill_record, BB_PARTY_KILL_RECORD
    // at start): the host's own setting, a guest follows the host's (party_balance Rules). Live.
    const bool on = host ? BbSettings::Get().party_kill_record.load()
                         : party::status::HostRuleOn(party::status::HostRule::KillRecord);
    if (!link) {
        balance::NoteKillWorld(false, false, "no link");
        return;
    }
    KillRecordGame& k = KR();
    const double now = NowS();
    // 0.7.2 rot (party_rot_rules.h): a killer's death and the world resets go to party_balance.
    const auto new_world = [&](const char* why) { balance::NoteKillWorld(!host, true, why); };
    if (!on) {
        balance::NoteKillWorld(false, false, "kill record off");
        // The host relays nothing while off. A guest keeps what came in: the host's record of a
        // new world can arrive just before its rules turn the switch on again (live: 40 ms).
        if (host) (void)desync::PopKillRecords();
        if (k.on) BLog("kill record: off (%s)", host ? "System > Party rules: World" : "the host's rule");
        k.on = false;
        return;
    }
    if (!k.on && k.started) {
        // On again: kills from before may be stale (a rest in between). The host starts a new
        // world (its members get it afresh); a guest waits for the host's.
        if (host) {
            k.rec.Reset(k.rec.epoch() + 1);
            k.members.clear();
        } else {
            k.rec.Reset(0);
        }
        new_world("the kill record on again");
        BLog("kill record: on again: world %llu", static_cast<unsigned long long>(k.rec.epoch()));
    }
    k.on = true;
    k.started = k.started || !host;
    if (host && !k.started) {
        k.started = true;
        k.rec.Reset(1);
    }
    for (auto& [slot, m] : desync::PopKillRecords()) {
        if (host) {
            if (m.epoch != k.rec.epoch()) continue;  // sent before the host's last load
            std::vector<u32> fresh;
            for (u32 id : m.kills)
                if (k.rec.AddRemote(id, now)) fresh.push_back(id);
            for (u32 id : fresh) balance::NotePartyKill(id, "a member's kill");
            if (fresh.empty()) continue;
            k.remote += fresh.size();
            PostKills(link, party::kBroadcast, fresh);
            BLog("kill record: %zu kill(s) seen on slot %d's game (%s): recorded, relayed to the party", fresh.size(),
                 slot, IdList(fresh).c_str());
            continue;
        }
        if (m.epoch != k.rec.epoch()) {
            BLog("kill record: the host's world %u (was %llu): record cleared, %zu kill(s) in it", m.epoch,
                 static_cast<unsigned long long>(k.rec.epoch()), m.kills.size());
            k.rec.Reset(m.epoch);
            new_world("the host's new world");
        }
        std::vector<u32> fresh;
        for (u32 id : m.kills)
            if (k.rec.AddRemote(id, now)) fresh.push_back(id);
        for (u32 id : fresh) balance::NotePartyKill(id, "a party kill from the host");
        k.remote += fresh.size();
        if (!fresh.empty()) BLog("kill record: %zu party kill(s) from the host (%s)", fresh.size(), IdList(fresh).c_str());
    }
    const GameSnapshot g = ReadGameState(false);
    const bool down = !g.world_up || g.loading || !g.player;
    if (down && !k.was_down && g.loading) {
        if (host) {
            // A rest, a death, a travel: every enemy respawned, for the whole party.
            k.rec.Reset(k.rec.epoch() + 1);
            new_world("the host's load");
            if (active) PostKills(link, party::kBroadcast, {});
            BLog("kill record: a load: world %llu, record cleared", static_cast<unsigned long long>(k.rec.epoch()));
        } else {
            k.rec.LocalLoad(now);
        }
    }
    k.was_down = down;
    if (host && active && !k.was_active && k.rec.size()) {
        // Alone in between (single-player rules): the world as the game left it.
        k.rec.Reset(k.rec.epoch() + 1);
        new_world("a party again");
        BLog("kill record: a party again: world %llu, record cleared", static_cast<unsigned long long>(k.rec.epoch()));
    }
    k.was_active = active;
    balance::NoteKillWorld(!host && k.rec.epoch() != 0, false, "");
    if (host) {
        for (auto it = k.members.begin(); it != k.members.end();) it = members.count(*it) ? std::next(it) : k.members.erase(it);
        for (const auto& [slot, name] : members) {
            if (k.members.insert(slot).second) {
                PostKills(link, slot, k.rec.Entities());
                BLog("kill record: %zu kill(s) of world %llu sent to %s (slot %d)", k.rec.size(),
                     static_cast<unsigned long long>(k.rec.epoch()), name.c_str(), slot);
            }
        }
    }
    if (!active || down || now < k.next) return;
    k.next = now + 0.5;
    std::vector<killrec::Obs> obs;
    for (const EnemyKillCandidate& e : balance::LoadedEnemies()) obs.push_back({e.entity, e.alive, e.chr, e.id});
    std::vector<u32> died;
    const std::vector<killrec::Action> acts = k.rec.Update(obs, now, &died);
    for (u32 id : died) balance::NotePartyKill(id, "seen dead here");
    if (!died.empty() && (host || k.rec.epoch() != 0)) {
        k.sent += died.size();
        PostKills(link, host ? party::kBroadcast : party::kHostSlot, died);
        BLog("kill record: %zu death(s) here (%s) sent to %s", died.size(), IdList(died).c_str(),
             host ? "the party" : "the host");
    }
    for (const killrec::Action& a : acts) {
        const bool ok = a.silent ? balance::SetEnemyDeadQuietly(a.chr) : balance::SetEnemyHpZero(a.chr);
        if (a.respawn) ++k.respawns;
        BLog("kill record: entity %u %s: %s", a.entity,
             a.respawn ? "alive again after its map streamed in (no rest, no death load)"
             : a.silent ? "streamed in alive, killed by the party before this game had it loaded"
                        : "alive here, killed by the party 3 s ago",
             ok ? (a.silent ? "put to death again (no echoes, no drop)" : "put to death (the kill's echoes and drop roll)")
                : "could not be put to death");
    }
}

/// Guest, main thread: the host digest's enemy census against this game's (party_desync.h).
struct CensusCheck {
    bool ask_due = false;
    double ask_at = 0;
    u32 ask_map = 0;
    std::vector<int> ask_buckets;
    u64 digests = 0, matched = 0, differed = 0, confirmed = 0, transient = 0, other_map = 0, no_world = 0, repaired = 0;
    int test_after = -1;
    bool test_fired = false;
    u32 fake_dead = 0;  // BB_PARTY_CENSUS_TEST: a living enemy this game reports as dead
    bool last_matched = false;  // the last compared digest matched (a match after a difference is logged)
};
std::atomic<const char*> g_census_log_next{nullptr};  // CensusLogNext: why
CensusCheck& CC() {
    static CensusCheck c;
    return c;
}

void ApplyCensusFake(std::vector<desync::CensusEntry>* list) {
    if (!CC().fake_dead) return;
    for (desync::CensusEntry& e : *list)
        if (e.entity == CC().fake_dead) e.alive = false;
}

std::string CensusCounts(const desync::CensusDigest& d) {
    char b[96];
    std::snprintf(b, sizeof b, "%d alive, %d dead (%d / %d without an entity id)", d.alive, d.dead, d.unnamed_alive,
                  d.unnamed_dead);
    return b;
}

void GuestCensus(party::PartyLink* link) {
    CensusCheck& c = CC();
    const double now = NowS();
    desync::CensusDigest host;
    if (desync::GuestPopCensusDigest(&host)) {
        ++c.digests;
        u32 map = 0;
        std::vector<desync::CensusEntry> list;
        if (!EnemyCensusNow(&map, &list)) {
            ++c.no_world;
        } else if (map != host.map) {
            if (c.other_map++ % 20 == 0) {
                BLog("enemy census: the host is in %s, this game in %s: not compared", MapName(host.map).c_str(),
                     MapName(map).c_str());
            }
        } else {
            ApplyCensusFake(&list);
            const desync::CensusDigest mine = desync::MakeCensusDigest(map, list);
            bool unnamed = false;
            const std::vector<int> diff = desync::CensusDiffBuckets(host, mine, &unnamed);
            if (diff.empty() && !unnamed) {
                ++c.matched;
                const char* why = g_census_log_next.exchange(nullptr);
                if (c.matched == 1 || c.matched % 20 == 0 || why || !c.last_matched) {
                    BLog("enemy census: digest %llu matches the host's (%s in %s)%s%s",
                         static_cast<unsigned long long>(c.matched), CensusCounts(mine).c_str(), MapName(map).c_str(),
                         why ? ", after " : !c.last_matched && c.matched > 1 ? " again" : "", why ? why : "");
                }
                c.last_matched = true;
                if (c.test_after < 0) c.test_after = desync::CensusTestAfterFromEnv();
                if (c.test_after > 0 && !c.test_fired && c.matched >= static_cast<u64>(c.test_after)) {
                    for (const desync::CensusEntry& e : list) {
                        if (e.entity && e.alive) {
                            c.fake_dead = e.entity;
                            break;
                        }
                    }
                    c.test_fired = true;
                    BLog("census test: entity %u now counts as dead in this game's census (BB_PARTY_CENSUS_TEST)",
                         c.fake_dead);
                }
            } else {
                ++c.differed;
                c.last_matched = false;
                g_census_log_next.store(nullptr);
                BLog("enemy census: differs from the host's digest in %zu bucket(s)%s (host %s, here %s); checking "
                     "again in %.0f s",
                     diff.size(), unnamed ? " and the enemies without an entity id" : "", CensusCounts(host).c_str(),
                     CensusCounts(mine).c_str(), desync::kCensusConfirmS);
                if (!c.ask_due) {
                    c.ask_due = true;
                    c.ask_at = now + desync::kCensusConfirmS;
                    c.ask_map = map;
                    c.ask_buckets = diff;
                }
            }
        }
    }
    if (c.ask_due && now >= c.ask_at) {
        c.ask_due = false;
        if (link && !link->is_host()) {
            link->post_event(party::kHostSlot, desync::kCensusAskEvent, desync::CensusAskToJson(c.ask_map, c.ask_buckets));
        }
    }
    desync::CensusList hl;
    if (desync::GuestPopCensusList(&hl)) {
        u32 map = 0;
        std::vector<desync::CensusEntry> list;
        if (!EnemyCensusNow(&map, &list) || map != hl.map) {
            BLog("enemy census: the host's list of %s came after this game left it", MapName(hl.map).c_str());
            return;
        }
        ApplyCensusFake(&list);
        const desync::CensusReport r = desync::CompareCensusList(hl, list);
        if (r.dead_on_host.empty() && r.alive_on_host.empty() && !r.empty()) {
            // Only which enemies are loaded differs (the two players stand far apart: each game
            // streams the chr sets near its own player), or the enemies without an entity id,
            // whose count follows the same streaming: no enemy is alive on one game and dead on
            // the other.
            ++c.transient;
            BLog("enemy census: loaded differently on the two games (players apart), no alive / dead difference: %s",
                 desync::DescribeCensusReport(r).c_str());
        } else if (r.empty()) {
            ++c.transient;
            BLog("enemy census: the same as the host's again (%zu entries checked; a change in flight)", hl.entries.size());
        } else {
            ++c.confirmed;
            BLog("enemy census: DIFFERS from the host's in %s: %s (%s; %llu of %llu digests matched)",
                 MapName(map).c_str(), desync::DescribeCensusReport(r).c_str(),
                 desync::CensusRepairFromEnv() && !r.dead_on_host.empty() ? "repairing the dead on the host" : "report only",
                 static_cast<unsigned long long>(c.matched), static_cast<unsigned long long>(c.digests));
            if (!r.dead_on_host.empty() && desync::CensusRepairFromEnv()) {
                // soak67: a host kill of a neighbouring block's enemy never arrives through the
                // game's own enemy sync; the confirmed difference puts it to death here (HP 0: the
                // game's own death, drop roll and kill echoes follow as for a kill).
                std::map<u32, int> want;
                for (const auto& [id, n] : desync::CensusRepairTargets(hl, list)) want[id] = n;
                int done = 0;
                std::string ids;
                for (const EnemyKillCandidate& e : balance::LoadedEnemies()) {
                    auto it = want.find(e.entity);
                    // Already paid for here (the kill record: it died on this game before): quietly.
                    if (it == want.end() || it->second <= 0 || !e.alive ||
                        !(KR().rec.PaidHere(e.entity) ? balance::SetEnemyDeadQuietly(e.chr) : balance::SetEnemyHpZero(e.chr)))
                        continue;
                    --it->second;
                    ++done;
                    ids += (ids.empty() ? "" : " ") + std::to_string(e.entity);
                }
                c.repaired += static_cast<u64>(done);
                BLog("enemy census: repair: %d enemy chr(s) dead on the host put to death here (HP 0)%s%s; %llu so far "
                     "(BB_PARTY_CENSUS_REPAIR=0: report only)",
                     done, ids.empty() ? "" : ": ", ids.c_str(), static_cast<unsigned long long>(c.repaired));
            }
            if (c.fake_dead && std::find(r.alive_on_host.begin(), r.alive_on_host.end(), c.fake_dead) != r.alive_on_host.end()) {
                BLog("census test: the fake kill of entity %u was found by the digest and named by the host's list; "
                     "forgotten again", c.fake_dead);
                c.fake_dead = 0;
            }
        }
    }
}

void GuestDesync(party::PartyLink* link, bool in_world) {
    KillRecordTick(link, false, in_world, {});
    std::string body;
    if (link && !link->is_host() && desync::GuestPopResync(&body)) {
        link->post_event(party::kHostSlot, desync::kResyncEvent, body);
    }
    GuestCensus(link);
}

Bridge& B() {
    static Bridge b;
    return b;
}

void HostSide(Bridge& b, party::PartyLink* link) {
    std::vector<FlagChange> changes = PopHostFlagChanges();  // always drained (no backlog)
    if (!link || !link->is_host()) {
        return;
    }
    if (!b.hosting_seen && link->state() == party::LinkState::Hosting) {
        b.hosting_seen = true;
        SessionStarted("host");
    }
    const std::vector<party::RosterEntry> roster = link->roster();
    std::map<int, std::string> now;
    bool any_member = false;  // connected or slot kept (broadcasts queue for kept slots)
    for (const party::RosterEntry& e : roster) {
        if (e.slot == party::kHostSlot) {
            continue;
        }
        any_member = true;
        if (e.connected) {
            now[e.slot] = e.name;
        }
    }
    for (const auto& [slot, name] : now) {
        const auto it = b.connected.find(slot);
        if (it == b.connected.end() || it->second != name) {
            b.want_snapshot.insert(slot);
            BLog("member %s (slot %d) connected: flag snapshot due", name.c_str(), slot);
        }
    }
    {
        // The guests' late-joiner catch-up (party_catchup) decides from the host's snapshot whether
        // the host is past its prologue; the prologue flags are not in the change stream. A host
        // that finishes its prologue after the members connected (both sides started a new game
        // together: the guest waited in its first Dream for good, soak 2026-10-11) re-sends the
        // snapshot once it is past it.
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (t - b.prologue_checked >= 1.0) {
            b.prologue_checked = t;
            bool opening = false, dream = false;
            if (FlagLoadMode() == 0 && ReadFlag(12410000, &opening) && ReadFlag(9401, &dream)) {
                const int past = opening && dream ? 1 : 0;
                if (b.host_past_prologue == 0 && past == 1) {
                    for (const auto& [slot, name] : now) {
                        b.want_snapshot.insert(slot);
                    }
                    BLog("the host is past its prologue now: flag snapshot due to %zu member(s)", now.size());
                }
                b.host_past_prologue = past;
            }
        }
    }
    for (auto it = b.want_snapshot.begin(); it != b.want_snapshot.end();) {
        it = now.count(*it) ? std::next(it) : b.want_snapshot.erase(it);
    }
    b.connected = std::move(now);

    if (!b.host_session && !b.connected.empty()) {
        b.host_session = true;
        SessionStarted("host");
    } else if (b.host_session && b.connected.empty()) {
        b.host_session = false;
        SessionEnded();
    }

    WorldImage world = PopHostWorldChanges();  // always drained too
    if (!world.groups.empty() && any_member) {
        // The host-world mirror: the groups that changed (absolute bytes; guests write the bits
        // that differ into their overlay).
        link->post_event(party::kBroadcast, kEventWorldGroups, WorldImageToJson(world));
        BLog("world mirror: %zu changed group(s) sent to the party", world.groups.size());
    }
    if (!changes.empty()) {
        b.desync_sent_seq = changes.back().seq;
        b.desync_seq_known = any_member;  // dropped (nobody to send to): the next snapshot covers it
    }
    if (!changes.empty() && any_member) {
        link->post_event(party::kBroadcast, kEventFlags, ChangesToJson(HostEpoch(), changes));
        BLog("%zu flag changes (seq %llu..%llu) sent to the party", changes.size(),
             static_cast<unsigned long long>(changes.front().seq), static_cast<unsigned long long>(changes.back().seq));
    }
    HostDesync(b, link, b.connected);
    KillRecordTick(link, true, !b.connected.empty(), b.connected);
    if (!b.want_snapshot.empty()) {
        FlagSnapshot snap;
        WorldImage image;
        if (!FullSnapshot(&snap) || !HostWorldImage(&image)) {
            if (!b.waiting_logged) {
                b.waiting_logged = true;
                BLog("flag snapshot waits for the host's baseline (world not steady yet)");
            }
            return;
        }
        b.waiting_logged = false;
        const std::string body = SnapshotToJson(snap);
        const std::string world_body = WorldImageToJson(image);
        for (int slot : b.want_snapshot) {
            link->post_event(slot, kEventWorldGroups, world_body);
            BLog("world mirror: full image (%zu non-zero groups, %zu bytes) sent to slot %d", image.groups.size(),
                 world_body.size(), slot);
            link->post_event(slot, kEventFlagSnapshot, body);
            BLog("flag snapshot (%zu blocks, seq %llu, %zu bytes) sent to slot %d", snap.blocks.size(),
                 static_cast<unsigned long long>(snap.seq), body.size(), slot);
        }
        b.want_snapshot.clear();
    }
}

void GuestSide(Bridge& b, const GameSnapshot& s) {
    const bool in_world = s.session_role == RoleClient;
    if (in_world != b.guest_in_world) {
        b.guest_in_world = in_world;
        BLog("guest %s the host's world", in_world ? "entered" : "left");
        if (in_world) {
            SessionStarted("guest");
        } else {
            SessionEnded();
        }
    }
}

} // namespace

void DirectorTick(const GameSnapshot& s, party::PartyLink* link) {
    if (!Enabled()) {
        return;
    }
    {
        // 0.7.3: "Covenant outcomes per player" (System > Party rules: World, bbport.ini
        // party_covenant_per_player): the host's setting; a guest follows the host's party rules.
        const bool on = CurrentRole() == Role::Guest
                            ? party::status::HostRuleOn(party::status::HostRule::CovenantPerPlayer)
                            : BbSettings::Get().party_covenant_per_player.load();
        static bool logged = false;  // the first value, then every change
        if (!logged || on != CovenantPerPlayer()) {
            logged = true;
            BLog("covenant outcomes per player: %s (%s)", on ? "on" : "off",
                 CurrentRole() == Role::Guest ? "the host's rule" : "this game's setting");
        }
        SetCovenantPerPlayer(on);
    }
    Tick(s);
    perf::Scope z(perf::Zone::ProgressBridge);
    Bridge& b = B();
    if (link != b.link) {  // a new link (or none): its roster starts over
        b.link = link;
        b.hosting_seen = false;
        b.connected.clear();
        b.want_snapshot.clear();
        b.desync_seq_known = false;
        desync::GuestReset();
        if (b.host_session) {
            b.host_session = false;
            SessionEnded();
        }
    }
    switch (CurrentRole()) {
    case Role::Host:
        HostSide(b, link);
        break;
    case Role::Guest:
        GuestSide(b, s);
        GuestDesync(link, b.guest_in_world);
        break;
    default:
        break;
    }
}

void CensusLogNext(const char* why) {
    g_census_log_next.store(why);
}

} // namespace coop::progress
