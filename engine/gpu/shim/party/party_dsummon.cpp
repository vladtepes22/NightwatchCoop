// SPDX-License-Identifier: GPL-3.0-or-later
// Direct summon delivery: wire format, payload and decisions (party_dsummon.h). Payload layout
// ported from droogie/bbhost (GPL-3.0+) src/engine/summon_invite.cpp.
#include "party/party_dsummon.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace coop::dsummon {

bool SwitchOn(const char* v, bool default_on) {
    if (!v || !v[0]) return default_on;
    return !(std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0 || std::strcmp(v, "false") == 0);
}

Kind KindOf(const std::string& b) {
    if (b.find("\"op\":\"dsum_req\"") != std::string::npos) return Kind::Req;
    if (b.find("\"op\":\"dsum_room\"") != std::string::npos) return Kind::Room;
    if (b.find("\"op\":\"dsum_done\"") != std::string::npos) return Kind::Done;
    if (b.find("\"op\":\"dsum_busy\"") != std::string::npos) return Kind::Busy;
    return Kind::None;
}

std::string EncodeReq() { return "{\"op\":\"dsum_req\"}"; }
Busy HostBusy(bool boss_bar_up, bool cutscene_playing) {
    return boss_bar_up ? Busy::Boss : cutscene_playing ? Busy::Cutscene : Busy::None;
}

Busy TitleJoinBusy(bool boss_bar_up, bool cutscene_playing, bool host_ready, bool member_in_prologue) {
    const Busy b = HostBusy(boss_bar_up, cutscene_playing);
    return b != Busy::None ? b : !host_ready ? Busy::Prologue : member_in_prologue ? Busy::MemberPrologue : Busy::None;
}

const char* BusyWord(Busy b) {
    switch (b) {
    case Busy::Boss: return "boss";
    case Busy::Cutscene: return "cutscene";
    case Busy::Prologue: return "prologue";
    case Busy::OtherMap: return "map";
    case Busy::MemberPrologue: return "member_prologue";
    case Busy::Settle: return "settle";
    default: return "none";
    }
}

std::string BusyText(Busy b, bool host) {
    switch (b) {
    case Busy::Boss: return host ? "Joiners wait: you are in a boss fight" : "Waiting: the host is in a boss fight";
    case Busy::Cutscene: return host ? "Joiners wait: a cutscene is playing" : "Waiting: the host is watching a cutscene";
    case Busy::Prologue: return host ? "Joiners wait: finish your prologue first" : "Waiting: the host is in its prologue";
    case Busy::OtherMap: return host ? "A joiner travels to your area first" : "Travelling to the host's area first";
    case Busy::MemberPrologue:
        return host ? "Joiners wait: a party member is in the prologue" : "Waiting: a party member is in the prologue";
    default: return {};
    }
}

Busy WaitOf(const std::string& body) {
    if (body.find("\"wait\":\"boss\"") != std::string::npos) return Busy::Boss;
    if (body.find("\"wait\":\"cutscene\"") != std::string::npos) return Busy::Cutscene;
    if (body.find("\"wait\":\"prologue\"") != std::string::npos) return Busy::Prologue;
    if (body.find("\"wait\":\"map\"") != std::string::npos) return Busy::OtherMap;
    if (body.find("\"wait\":\"member_prologue\"") != std::string::npos) return Busy::MemberPrologue;
    if (body.find("\"wait\":\"settle\"") != std::string::npos) return Busy::Settle;
    return Busy::None;
}

std::string EncodeRefuse(Busy why) {
    if (why == Busy::None) return "{\"op\":\"dsum_room\",\"room\":\"\"}";
    return std::string("{\"op\":\"dsum_room\",\"room\":\"\",\"wait\":\"") + BusyWord(why) + "\"}";
}

std::string EncodeBusy(Busy b) { return std::string("{\"op\":\"dsum_busy\",\"wait\":\"") + BusyWord(b) + "\"}"; }
std::string EncodeDone(bool ok) { return ok ? "{\"op\":\"dsum_done\",\"ok\":1}" : "{\"op\":\"dsum_done\",\"ok\":0}"; }

std::string EncodeRoom(const Room& r) {
    std::string s = "{\"op\":\"dsum_room\",\"room\":\"";
    char h[3];
    for (std::uint8_t b : r.data) {
        std::snprintf(h, sizeof h, "%02x", b);
        s += h;
    }
    char tail[160];
    std::snprintf(tail, sizeof tail, "\",\"area\":%u,\"pos\":[%.4f,%.4f,%.4f],\"level\":%u}", r.area, double(r.pos[0]),
                  double(r.pos[1]), double(r.pos[2]), r.level);
    return s + tail;
}

namespace {
int Nib(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
}  // namespace

bool DecodeRoom(const std::string& body, Room* out, bool* refused, std::string* err) {
    *refused = false;
    const char* b = body.c_str();
    const char* p = std::strstr(b, "\"room\":\"");
    if (KindOf(body) != Kind::Room || !p) {
        *err = "not a dsum_room";
        return false;
    }
    p += 8;
    out->data.clear();
    while (*p && *p != '"') {
        const int a = Nib(p[0]), c = p[1] ? Nib(p[1]) : -1;
        if (a < 0 || c < 0 || out->data.size() >= kMaxRoomData) {
            *err = "bad room data";
            return false;
        }
        out->data.push_back(std::uint8_t(a << 4 | c));
        p += 2;
    }
    if (out->data.empty()) {
        *refused = true;
        *err = "refused by the host";
        return false;
    }
    if (out->data.size() <= 8) {
        *err = "room data too short";
        return false;
    }
    const char* pa = std::strstr(b, "\"area\":");
    const char* pp = std::strstr(b, "\"pos\":[");
    const char* pl = std::strstr(b, "\"level\":");
    unsigned area = 0, level = 0;
    float x = 0, y = 0, z = 0;
    if (!pa || !pp || !pl || std::sscanf(pa, "\"area\":%u", &area) != 1 ||
        std::sscanf(pp, "\"pos\":[%f,%f,%f]", &x, &y, &z) != 3 || std::sscanf(pl, "\"level\":%u", &level) != 1) {
        *err = "missing area / pos / level";
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || std::fabs(x) >= 1e5f || std::fabs(y) >= 1e5f ||
        std::fabs(z) >= 1e5f || level > 544) {
        *err = "position or level out of range";
        return false;
    }
    out->area = area;
    out->pos[0] = x;
    out->pos[1] = y;
    out->pos[2] = z;
    out->level = level;
    return true;
}

bool DecodeDone(const std::string& body, bool* ok) {
    if (KindOf(body) != Kind::Done) return false;
    *ok = body.find("\"ok\":1") != std::string::npos;
    return true;
}

std::vector<std::uint8_t> BuildPayload(const std::uint8_t entry_id[8], const Room& r) {
    std::vector<std::uint8_t> p(kHeadSize + r.data.size(), 0);
    std::memcpy(&p[0x00], entry_id, 8);
    std::memcpy(&p[0x0c], &r.area, 4);
    std::memcpy(&p[0x10], r.pos, 12);
    std::memcpy(&p[0x1c], &r.level, 4);
    const std::uint64_t no_channel = 0x8000000000000000ull;
    std::memcpy(&p[0x28], &no_channel, 8);
    const std::uint32_t n = std::uint32_t(r.data.size());
    std::memcpy(&p[0x30], &n, 4);
    if (n) std::memcpy(&p[kHeadSize], r.data.data(), n);
    return p;
}

bool MapKnown(std::uint32_t m) { return m != 0 && m != 0xffffffffu; }
bool SameMap(std::uint32_t a, std::uint32_t b) { return MapKnown(a) && MapKnown(b) && (a >> 16) == (b >> 16); }
bool CarriesAcrossMaps(std::uint32_t host_map, bool experiment) {
    return experiment || (MapKnown(host_map) && ((host_map >> 24) & 0xff) == 29);
}

namespace {
bool OtherMap(const HostView& v) {
    return !v.carry && MapKnown(v.host_map) && MapKnown(v.guest_map) && !SameMap(v.host_map, v.guest_map);
}
}  // namespace

const char* HostRefusal(const HostView& v) {
    if (!v.world_up || v.loading) return "the host is not in its world";
    if (v.travel_pending) return "the host is about to travel (its loading screen is next): the guest joins it there";
    if (!v.host_ready) return "the host is still in its prologue: joiners wait until it is done";
    if (v.guest_in_prologue) return "the guest is still in its own prologue";
    if (v.member_in_prologue) return "another party member is still in its prologue: summons start once everybody is ready";
    if (v.busy == Busy::Boss) return "the host is in a boss fight: joiners wait outside";
    if (v.busy == Busy::Cutscene) return "the host is watching a cutscene: joiners wait";
    if (OtherMap(v)) return "the guest is in another map: it gathers to the host's lamp first (loading screen)";
    if (v.cooperators >= v.max_cooperators) return "the host's world is full";
    if (v.ms_since_sign_summon < 20000) return "the host's game is summoning a sign";
    if (v.settling) return "a member just left this host's room and its game is still ending that session (a fresh room follows)";
    if (v.other_hold_ms > 0) return "a crashed member's connection is still held";
    return nullptr;
}

Busy RefusalWait(const HostView& v) {
    if (!v.world_up || v.loading || v.travel_pending) return Busy::None;  // the sign path follows the host
    if (!v.host_ready) return Busy::Prologue;
    if (v.guest_in_prologue) return Busy::None;
    if (v.member_in_prologue) return Busy::MemberPrologue;
    if (v.busy != Busy::None) return v.busy;
    if (OtherMap(v)) return Busy::OtherMap;
    if (v.cooperators < v.max_cooperators && v.ms_since_sign_summon >= 20000 && v.settling) return Busy::Settle;
    return Busy::None;
}

bool HostBellHeld(const HostBellView& v) {
    if (!v.direct_on) return false;
    if (v.since_done < v.since_served && v.since_done < kHostBellHold) return v.done_ok;  // joined: no bell; failed: ring now
    if (v.since_served < kHostBellHold) return true;  // a room is on its way to the guest
    if (v.since_req < kHostBellHold) {
        // Batch 6 (live): a request that arrived this frame is not answered yet (the room is
        // served on the next tick): the bell rang 0.03 s after the guest's request.
        if (v.since_refused > v.since_req) return true;
        return v.refused_wait;  // refused: the sign path is the fallback, unless the guest must wait / gather
    }
    return v.waiting_s < kHostBellGrace;               // the guest's own ring (and its request) comes first
}

bool HostTravelHeld(const HostTravelHoldView& v) {
    // par_c6 (live): a title joiner gets the room without a dsum_done; a lamp travel 4 s after the
    // room was sent left it loading into the map the host had left (B1 fallback, re-summon 50 s
    // later). Held until its chr is in the host's world (or the hold runs out).
    // Regression run 0.6.1 (live, "FALLBACK to B1" on the first follow): the title joiner's chr is
    // made on the host ~4 s before the joiner's own loading screen ends; a travel released then
    // reached the guest mid title join, its load left it outside the session and the re-join took
    // ~50 s. Held until the joiner says it is in the world (roster no longer joining / loading).
    if (v.since_title < kHostTravelHoldMax && (!v.joiner_in_world || v.joiner_loading)) return true;
    if (!v.direct_on || v.since_served >= kHostTravelHoldMax) return false;  // nothing served lately / waited enough
    const bool done_for_this = v.since_done <= v.since_served;
    if (done_for_this && !v.done_ok) return false;                // the join failed: travel now
    if (done_for_this && v.joiner_in_world) return false;          // joined and in the world: it follows
    return true;
}

bool JoinBesideWaits(Busy host_busy, const float host_pos[3]) {
    if (host_busy == Busy::Boss || host_busy == Busy::Cutscene) {
        return true;
    }
    return std::fabs(host_pos[0]) < 0.5f && std::fabs(host_pos[1]) < 0.5f && std::fabs(host_pos[2]) < 0.5f;
}

const char* PhaseName(Phase p) {
    switch (p) {
    case Phase::Idle: return "idle";
    case Phase::Asked: return "asked";
    case Phase::HaveRoom: return "have room";
    case Phase::Delivered: return "delivered";
    case Phase::Done: return "done";
    case Phase::Failed: return "failed";
    }
    return "?";
}

void OnAsked(Guest& g, double now) {
    g.phase = Phase::Asked;
    g.since = now;
    g.deliver_tries = 0;
    g.fail_why = "";
    g.gather = false;
    g.waited = false;
    g.reask_at = 0;
    g.reasks = 0;
    g.rearmed = false;
}

void OnRoom(Guest& g, double now, bool refused, Busy wait) {
    if (g.phase != Phase::Asked) return;  // late or unasked: ignored
    if (refused && wait == Busy::Settle && g.reasks < kMaxReasks) {
        g.reask_at = now + kReaskWait;  // the host's game is ending a session: ask again soon
        g.since = now;
        return;
    }
    g.phase = refused ? Phase::Failed : Phase::HaveRoom;
    g.since = now;
    if (refused) {
        g.fail_why = "refused by the host";
        g.gather = wait == Busy::OtherMap;
        g.waited = wait != Busy::None && wait != Busy::OtherMap;
    }
}

bool RoomElsewhere(Guest& g, double now, std::uint32_t room_area, std::uint32_t own_map, bool carry) {
    if (g.phase != Phase::HaveRoom || carry || SameMap(room_area, own_map)) return false;
    g.phase = Phase::Failed;
    g.since = now;
    g.fail_why = MapKnown(own_map) ? "the host's room is in another map (gather first)"
                                   : "this guest's own map is not known yet";
    g.gather = MapKnown(own_map);
    return true;
}

void OnDelivered(Guest& g, double now, bool ok) {
    if (ok) {
        g.phase = Phase::Delivered;
        g.since = now;
    } else {
        ++g.deliver_tries;
    }
}

bool GatherFirst(bool direct_on, Phase last) { return !direct_on || last == Phase::Failed; }
bool GatherFirst(bool direct_on, Phase last, bool last_waited) {
    return !direct_on || (last == Phase::Failed && !last_waited);
}

Action Step(Guest& g, double now, bool entry_waiting, bool joined) {
    switch (g.phase) {
    case Phase::Asked:
        if (g.reask_at > 0 && now >= g.reask_at) {
            g.reask_at = 0;
            ++g.reasks;
            g.since = now;
            return Action::Reask;
        }
        if (g.reask_at > 0) return Action::None;
        if (now - g.since > kAnswerWait) {
            g.phase = Phase::Failed;
            g.fail_why = "no answer from the host";
            return Action::None;  // the host never answered: it holds nothing
        }
        return Action::None;
    case Phase::HaveRoom:
        if (joined) {  // the sign path won the race
            g.phase = Phase::Done;
            return Action::ReportOk;
        }
        if (!entry_waiting && !g.rearmed && g.deliver_tries == 0 && now - g.since > kRearmAfter) {
            g.rearmed = true;
            g.since = now;  // the entry wait counts from the re-ring
            return Action::Rearm;
        }
        if (now - g.since > kEntryWait || g.deliver_tries >= 3) {
            g.phase = Phase::Failed;
            g.fail_why = g.deliver_tries >= 3 ? "the game refused the summon message 3 times"
                         : g.rearmed          ? "the bell's own summon entry did not come up, not even after a re-ring"
                                              : "the bell's own summon entry did not come up within 8 s";
            return Action::ReportFail;
        }
        return entry_waiting ? Action::Deliver : Action::None;
    case Phase::Delivered:
        if (joined) {
            g.phase = Phase::Done;
            return Action::ReportOk;
        }
        if (now - g.since > kJoinWait) {
            g.phase = Phase::Failed;
            g.fail_why = "delivered, but the game did not join the host's room within 8 s";
            return Action::ReportFail;
        }
        return Action::None;
    default:
        return Action::None;
    }
}

}  // namespace coop::dsummon
