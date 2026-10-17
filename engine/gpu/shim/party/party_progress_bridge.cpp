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

#include "game_state.h"
#include "party_link.h"
#include "party_perf.h"

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

/// Guest, main thread: the host digest's enemy census against this game's (party_desync.h).
struct CensusCheck {
    bool ask_due = false;
    double ask_at = 0;
    u32 ask_map = 0;
    std::vector<int> ask_buckets;
    u64 digests = 0, matched = 0, differed = 0, confirmed = 0, transient = 0, other_map = 0, no_world = 0;
    int test_after = -1;
    bool test_fired = false;
    u32 fake_dead = 0;  // BB_PARTY_CENSUS_TEST: a living enemy this game reports as dead
};
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
                if (c.matched == 1 || c.matched % 20 == 0) {
                    BLog("enemy census: digest %llu matches the host's (%s in %s)",
                         static_cast<unsigned long long>(c.matched), CensusCounts(mine).c_str(), MapName(map).c_str());
                }
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
            BLog("enemy census: DIFFERS from the host's in %s: %s (report only; %llu of %llu digests matched)",
                 MapName(map).c_str(), desync::DescribeCensusReport(r).c_str(),
                 static_cast<unsigned long long>(c.matched), static_cast<unsigned long long>(c.digests));
            if (c.fake_dead && std::find(r.alive_on_host.begin(), r.alive_on_host.end(), c.fake_dead) != r.alive_on_host.end()) {
                BLog("census test: the fake kill of entity %u was found by the digest and named by the host's list; "
                     "forgotten again", c.fake_dead);
                c.fake_dead = 0;
            }
        }
    }
}

void GuestDesync(party::PartyLink* link) {
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
        GuestDesync(link);
        break;
    default:
        break;
    }
}

} // namespace coop::progress
