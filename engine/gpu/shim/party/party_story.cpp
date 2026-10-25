// SPDX-License-Identifier: GPL-3.0-or-later
// Party story, phase C4 (see party_story.h; the RE is docs/party/cutscenes_endings.md).
//
// Every site below was disassembled from smoketest/out/eboot.elf (1.09) and is compared byte for
// byte before anything is written. Offsets are ours (raw ELF VA). The game's code is System V:
// every call into it goes through a BB_COOP_SYSV pointer at Guest(off).
#include "party_story.h"
#include "party_hud.h"
#include "party_async_io.h"
#include "party_perf.h"
#include "party_ids.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#ifndef BB_PARTY_STORY_NO_GAME
#include "coop_hooks.h"
#include "game_state.h"
#include "party_b2.h"
#include "party_director.h"
#include "party_progress.h"
#include "party_runtime.h"
#include "party_travel.h"
#include "seamless_rules.h"
#include "party_split.h"
#include "party_worldact.h"
#include "party_balance.h"
#include "party_status.h"
#include "../bbport_game_menu_pages.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#endif

namespace coop {

// ---------------------------------------------------------------------------------------------
// Pure part (unit-tested)
// ---------------------------------------------------------------------------------------------

namespace {

struct KindName {
    StoryKind kind;
    const char* name;
};
constexpr KindName kKindNames[] = {
    {StoryKind::Unknown, "unknown"},
    {StoryKind::Cutscene, "cutscene"},
    {StoryKind::Ending, "ending"},
    {StoryKind::TimeOfDay, "time_of_day"},
};

// remo/sAA_BB_1NNN.remobnd.dcx siblings (cutscenes_endings.md 4.2).
constexpr std::uint32_t kSexPairs[][2] = {
    {21000010, 21001010}, // A ending
    {21000030, 21001030}, // C ending
    {24010005, 24011005}, // character creation
    {28000040, 28001040}, // Lecture Building grab
    {33000000, 33001000}, // Patches push
};

struct StoryScene {
    std::uint32_t remo;
    std::uint32_t flags[2]; // 0 = none
};
// Scenes a guest replays in its own world when it missed them (4.3 / 4.6 / 3.1). Flags [data],
// set ON after the remo exactly like the event bodies that follow the 2002 instruction.
constexpr StoryScene kStoryScenes[] = {
    {24000020, {0, 0}},        // m24_00 12400750 day -> evening (tod 1)
    {24000030, {0, 0}},        // m24_00 12401803 Willem's memory, evening -> night (tod 2)
    {32000000, {70002802, 0}}, // m32 13201803 Rom -> Blood Moon (tod 3)
    {21000000, {12417810, 0}}, // m21 9401 first death -> Hunter's Dream
    {28000010, {9423, 0}},     // common 9422 first kidnap
    {22000030, {0, 0}},        // m22 carriage
    {22000040, {0, 0}},
    {26000000, {0, 0}},        // m26 12601854 Micolash post-fight
    {26000005, {0, 0}},
    {26000040, {0, 0}},        // m26 12600026 brain shutdown
    {28000040, {12800434, 0}}, // m28 12800431 Lecture Building grab
    {33000000, {0, 0}},        // m33 13300200 Patches push
    {34000040, {0, 0}},        // m34 13401800 Ludwig
    {36000010, {0, 0}},        // m36 13601803 Orphan / Kos
    {24000000, {12401000, 0}}, // m24_00 12405263 DLC entry grab (m34 13401000 consumes it)
};

constexpr int kTodTable[4] = {0, 4, 6, 7}; // 0x4925010

std::uint32_t BaseRemo(std::uint32_t id) {
    for (const auto& p : kSexPairs) {
        if (id == p[1]) {
            return p[0];
        }
    }
    return id;
}

} // namespace

const char* StoryKindName(StoryKind k) {
    for (const KindName& n : kKindNames) {
        if (n.kind == k) {
            return n.name;
        }
    }
    return "unknown";
}

StoryKind StoryKindFromName(const std::string& name) {
    for (const KindName& n : kKindNames) {
        if (name == n.name) {
            return n.kind;
        }
    }
    return StoryKind::Unknown;
}

std::uint32_t RemoForSex(std::uint32_t remo_id, bool sex_variant) {
    for (const auto& p : kSexPairs) {
        if (remo_id == p[0] || remo_id == p[1]) {
            return sex_variant ? p[1] : p[0];
        }
    }
    return remo_id;
}

int EndingTypeOfEvent(std::uint32_t event_id) {
    switch (event_id) {
    case 12100180: return 1;
    case 12100000: return 2;
    case 12100002: return 3;
    default: return 0;
    }
}

int EndingTypeOfRemo(std::uint32_t remo_id) {
    switch (BaseRemo(remo_id)) {
    case 21000010: return 1;
    case 21000020: return 2;
    case 21000030: return 3;
    default: return 0;
    }
}

std::vector<std::uint32_t> StoryFlagsForRemo(std::uint32_t remo_id) {
    const std::uint32_t base = BaseRemo(remo_id);
    std::vector<std::uint32_t> out;
    for (const StoryScene& s : kStoryScenes) {
        if (s.remo == base) {
            for (std::uint32_t f : s.flags) {
                if (f) {
                    out.push_back(f);
                }
            }
        }
    }
    return out;
}

bool ReplayIgnoresCalm(std::uint32_t remo_id) {
    return BaseRemo(remo_id) == 28000010;
}

bool ReplayWorthy(const StoryIntent& s) {
    if (s.force) {
        return true;
    }
    switch (s.kind) {
    case StoryKind::Ending:
    case StoryKind::TimeOfDay:
        return true;
    case StoryKind::Cutscene: {
        if (s.tod >= 0 || !s.flags.empty()) {
            return true;
        }
        const std::uint32_t base = BaseRemo(s.id);
        for (const StoryScene& sc : kStoryScenes) {
            if (sc.remo == base) {
                return true;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

bool StoryRemoKnown(std::uint32_t remo_id) {
    return ids::RemoKnown(remo_id);
}

bool SanitizePeerStory(StoryIntent* s, std::string* why) {
    if (s->kind == StoryKind::Cutscene && !StoryRemoKnown(s->id)) {
        if (why) {
            *why = "cutscene " + std::to_string(s->id) + " is not a game remo";
        }
        return false;
    }
    const std::vector<std::uint32_t> table = StoryFlagsForRemo(s->kind == StoryKind::Cutscene ? s->id : 0);
    std::vector<std::uint32_t> keep;
    for (std::uint32_t f : s->flags) {
        if (std::find(table.begin(), table.end(), f) != table.end() &&
            std::find(keep.begin(), keep.end(), f) == keep.end()) {
            keep.push_back(f);
        }
    }
    s->flags = std::move(keep);
    return true;
}

bool GuestPressedHostOnlyEvent(std::uint32_t event_id, std::uint32_t remo_id) {
    // D5 / D10 (pard4): events whose action button only the host's chr had (1003[6] ends a
    // client's copy, 4[3] chr type 0 next to the 3[24] button) and that change the world's time
    // of day without a warp (2002[6] point -1). A guest's own copy now runs them (built-in client
    // skip rules); the host plays its 2002[6] as its own and sets the event's flag.
    struct Row {
        std::uint32_t event, remo;
    };
    static constexpr Row rows[] = {
        {12401803, 24000030},  // m24_00 Amelia's skull (Willem's memory): evening -> night
    };
    for (const Row& r : rows) {
        if (r.event == event_id && r.remo == BaseRemo(remo_id)) {
            return true;
        }
    }
    return false;
}

bool GuestPhaseChangeOk(const StoryIntent& s, int host_tod_value, std::uint32_t host_map, std::string* why) {
    const char* no = nullptr;
    const bool no_warp = s.warp_point == kStoryNone && GuestPressedHostOnlyEvent(s.event_id, s.id);
    if (s.kind != StoryKind::Cutscene || s.instr != 6) {
        no = "not a 2002[6] play";
    } else if (!StoryRemoKnown(s.id)) {
        no = "not a game remo";
    } else if (s.tod < 1 || s.tod > 3) {
        no = "no time of day";
    } else if (s.tod <= TodFromFlagValue(host_tod_value < 0 ? 0 : host_tod_value)) {
        no = "the host's time of day is there already";
    } else if ((s.warp_point == kStoryNone && !no_warp) || s.warp_point == 0 || s.warp_map == kStoryNone ||
               (s.warp_map & 0xffff0000u) != (host_map & 0xffff0000u)) {
        no = "its warp is not in the host's map";
    }
    if (no && why) {
        *why = no;
    }
    return !no;
}

int TodFlagValue(int tod) {
    return tod >= 0 && tod < 4 ? kTodTable[tod] : -1;
}

int TodFromFlagValue(int value) {
    int tod = 0;
    for (int i = 0; i < 4; ++i) {
        if ((value & kTodTable[i]) == kTodTable[i]) {
            tod = i;
        }
    }
    return tod;
}

std::string DescribeStory(const StoryIntent& s) {
    char text[320];
    std::string flags;
    for (std::uint32_t f : s.flags) {
        flags += (flags.empty() ? "" : ",") + std::to_string(f);
    }
    std::snprintf(text, sizeof text, "#%llu %s %u (mode %u, 2002[%u], event %u, map %08x, tod %d, warp %d @%08x%s%s)%s",
                  static_cast<unsigned long long>(s.seq), StoryKindName(s.kind), s.id, s.mode, s.instr, s.event_id,
                  s.map, s.tod, static_cast<int>(s.warp_point), s.warp_map, flags.empty() ? "" : ", flags ",
                  flags.c_str(), s.force ? " [forced]" : "");
    return text;
}

json::Value StoryToJson(const StoryIntent& s) {
    json::Value v = json::Value::make_object();
    v.set("seq", json::hex(s.seq));
    v.set("kind", StoryKindName(s.kind));
    v.set("id", s.id);
    v.set("mode", s.mode);
    v.set("instr", s.instr);
    v.set("event", s.event_id);
    v.set("map", s.map);
    v.set("tod", s.tod);
    v.set("warp_point", s.warp_point);
    v.set("warp_map", s.warp_map);
    json::Value f = json::Value::make_array();
    for (std::uint32_t id : s.flags) {
        f.push(id);
    }
    v.set("flags", f);
    if (s.force) {
        v.set("force", true);
    }
    return v;
}

bool StoryFromJson(const json::Value& v, StoryIntent* out, std::string* error) {
    try {
        if (v.type != json::Value::Type::Object) {
            throw std::runtime_error("not an object");
        }
        StoryIntent s;
        s.seq = json::u64(v, "seq");
        s.kind = StoryKindFromName(json::str(v, "kind"));
        if (s.kind == StoryKind::Unknown) {
            throw std::runtime_error("kind: unknown");
        }
        s.id = json::u32(v, "id");
        s.mode = json::u32(v, "mode");
        s.instr = json::u32(v, "instr");
        s.event_id = json::u32(v, "event");
        s.map = json::u32(v, "map");
        const double tod = json::num(v, "tod");
        if (tod < -1 || tod > 3 || tod != static_cast<int>(tod)) {
            throw std::runtime_error("tod: out of range");
        }
        s.tod = static_cast<int>(tod);
        s.warp_point = json::u32(v, "warp_point");
        s.warp_map = json::u32(v, "warp_map");
        if (json::arr(v, "flags").size() > 64) {
            throw std::runtime_error("flags: too many");
        }
        for (const json::Value& f : json::arr(v, "flags")) {
            s.flags.push_back(static_cast<std::uint32_t>(json::as_u64(f, "flags")));
        }
        if (const json::Value* f = v.find("force")) {
            s.force = f->type == json::Value::Type::Bool && f->boolean;
        }
        if (s.seq == 0) {
            throw std::runtime_error("seq 0");
        }
        if (s.kind == StoryKind::Ending && (s.id < 1 || s.id > 3)) {
            throw std::runtime_error("ending: type not 1..3");
        }
        if (s.kind == StoryKind::TimeOfDay && s.id > 3) {
            throw std::runtime_error("time_of_day: not 0..3");
        }
        *out = s;
        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

std::string StoryToJsonText(const StoryIntent& s) {
    return json::dump(StoryToJson(s), 0);
}

bool StoryFromJsonText(const std::string& text, StoryIntent* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) {
        if (error) {
            *error = err;
        }
        return false;
    }
    return StoryFromJson(v, out, error);
}

const char* StoryActionName(StoryAction::Kind k) {
    switch (k) {
    case StoryAction::None: return "none";
    case StoryAction::PlayRemo: return "play remo 0x131A9F0";
    case StoryAction::SetFlags: return "set flags";
    case StoryAction::ApplyTod: return "time of day (0x1CECEB0 body)";
    case StoryAction::NgCycle: return "NG cycle +1 (2003[21])";
    case StoryAction::TravelDream: return "Hunter's Dream warp 0x13CDF30(2102961)";
    case StoryAction::ForceEnding: return "ending request (GSM+0x1550)";
    case StoryAction::AskEnding: return "ending question";
    }
    return "?";
}

bool HostStoryQueue::Push(const StoryIntent& s) {
    if (s.seq <= last_seq_) {
        return false;
    }
    last_seq_ = s.seq;
    if (q_.size() >= kMax) {
        q_.pop_front();
    }
    q_.push_back(s);
    return true;
}

bool HostStoryQueue::Pop(StoryIntent* out) {
    if (q_.empty()) {
        return false;
    }
    *out = q_.front();
    q_.pop_front();
    return true;
}

// ---- GuestStory ----

bool GuestStory::Offer(const StoryIntent& s, bool in_host_world, double now) {
    if (s.seq <= last_seq_) {
        return false;
    }
    last_seq_ = s.seq;
    dirty_ = true;
    if (s.kind == StoryKind::Cutscene && in_host_world && mirror_enabled && EndingTypeOfRemo(s.id) == 0) {
        mirror_ = s;
        mirror_since_ = now;
    }
    if (s.kind == StoryKind::Ending) {
        mirror_.reset(); // the ending is replayed as a whole in the own world
        for (const StoryIntent& q : queue_) {
            if (q.kind == StoryKind::Ending && q.id == s.id) {
                return true; // the event and the flag poll both report it
            }
        }
    }
    if (ReplayWorthy(s) && !(s.kind == StoryKind::Cutscene && EndingTypeOfRemo(s.id) != 0)) {
        if (queue_.size() >= kMaxQueue) {
            queue_.pop_front();
        }
        queue_.push_back(s);
    }
    return true;
}

bool GuestStory::Busy() const {
    return mirror_.has_value() || !queue_.empty() || running_;
}

bool GuestStory::Seen(std::uint32_t remo_id) const {
    return seen_.count(BaseRemo(remo_id)) != 0;
}

void GuestStory::NoteSeen(std::uint32_t remo_id) {
    if (seen_.insert(BaseRemo(remo_id)).second) {
        dirty_ = true;
    }
}

void HostStoryUpGuard::NotePlayed(std::uint32_t remo_id, double now) {
    played_[BaseRemo(remo_id)] = now;
}

const char* HostStoryUpGuard::Refuse(std::uint32_t remo_id, bool boss_fight, double now) const {
    const auto it = played_.find(BaseRemo(remo_id));
    if (it != played_.end() && now - it->second < kRecent) {
        return "the host played this cutscene in the last 10 minutes";
    }
    if (boss_fight) {
        return "the host is in a boss fight";
    }
    return nullptr;
}

bool SceneBlockLoaded(const StoryState& st, std::uint32_t remo) {
    const std::uint32_t block = MapOfRemo(remo);
    return std::find(st.loaded_blocks.begin(), st.loaded_blocks.end(), block) != st.loaded_blocks.end();
}

bool OwnSceneHold::Hold(const Item& it) {
    if (Holding(it.remo)) {
        return true;  // the same scene again: the first hold stands
    }
    if (items_.size() >= kMax) {
        return false;
    }
    items_.push_back(it);
    return true;
}

std::optional<OwnSceneHold::Item> OwnSceneHold::HostCopy(std::uint32_t remo) {
    const std::uint32_t base = BaseRemo(remo);
    std::optional<Item> first;
    for (auto it = items_.begin(); it != items_.end();) {
        if (BaseRemo(it->remo) == base) {
            if (!first) first = *it;
            it = items_.erase(it);
        } else {
            ++it;
        }
    }
    return first;
}

std::optional<OwnSceneHold::Item> OwnSceneHold::Due(double now, double wait) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (now - it->at >= wait) {
            const Item i = *it;
            items_.erase(it);
            return i;
        }
    }
    return std::nullopt;
}

std::size_t OwnSceneHold::Prune(double now, double max_age) {
    const std::size_t before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const Item& i) { return now - i.at > max_age; }),
                 items_.end());
    return before - items_.size();
}

bool OwnSceneHold::Holding(std::uint32_t remo) const {
    const std::uint32_t base = BaseRemo(remo);
    return std::any_of(items_.begin(), items_.end(), [base](const Item& i) { return BaseRemo(i.remo) == base; });
}

bool GuestStory::TakeDirty() {
    const bool d = dirty_;
    dirty_ = false;
    return d;
}

void GuestStory::Build(const StoryIntent& s, bool mirror, bool summoned) {
    ops_.clear();
    cur_ = s;
    running_ = true;
    running_mirror_ = mirror;
    running_summoned_ = summoned && !mirror;
    op_started_ = false;
    host_ended_ = end_mirror_ = false;
    auto push = [this](Op op) { ops_.push_back(std::move(op)); };
    if (mirror) {
        push({Op::Play, s.id, 2});
        push({Op::WaitRemoStart});
        push({Op::WaitRemoEnd});
        return;
    }
    switch (s.kind) {
    case StoryKind::Cutscene: {
        // The Gaol waking scene 28000010 too (user decision 2026-10-22, kidnapall: every member sees
        // it, whoever was grabbed; it replaced 0.7.2's "guests skip it"): the guest's loading screen
        // holds through the host's copy, then it plays here in the cell.
        if (!Seen(s.id) || s.force) {
            // Summoned: the phantom path (mode 2) the live mirror uses; no warp variant.
            push({Op::Play, s.id, summoned ? 2u : 0u});
            push({Op::WaitRemoStart});
            push({Op::WaitRemoEnd});
        }
        if (s.tod >= 0) {
            Op t{Op::Tod};
            t.tod = s.tod;
            push(t);
        }
        std::vector<std::uint32_t> flags = s.flags;
        for (std::uint32_t f : StoryFlagsForRemo(s.id)) {
            if (std::find(flags.begin(), flags.end(), f) == flags.end()) {
                flags.push_back(f);
            }
        }
        if (!flags.empty()) {
            Op f{Op::Flags};
            f.flags = flags;
            push(f);
        }
        break;
    }
    case StoryKind::TimeOfDay: {
        Op t{Op::Tod};
        t.tod = static_cast<int>(s.id);
        push(t);
        break;
    }
    case StoryKind::Ending: {
        if (ask_ending_ && !s.force) {
            Op a{Op::AskEnding};
            a.ending = static_cast<int>(s.id);
            push(a);
            break;
        }
        PushEndingBody(static_cast<int>(s.id));
        break;
    }
    default:
        break;
    }
}

void GuestStory::PushEndingBody(int ending) {
    auto push = [this](Op op) { ops_.push_back(std::move(op)); };
    const std::uint32_t id = static_cast<std::uint32_t>(ending);
    push({Op::TravelDream});
    push({Op::WaitArrive});
    auto flags = [&](std::vector<std::uint32_t> f, bool on) {
        Op op{Op::Flags};
        op.flags = std::move(f);
        op.on = on;
        push(op);
    };
    if (id == 1) {
        // m21 12100180 waits for 72100130 and does NG+1, the remo by sex, 6604, 21, 6600, 6603.
        flags({72100130}, true);
        Op e{Op::EndingCheck};
        e.ending = 1;
        push(e);
        return;
    }
    const bool c = id == 3;
    flags(c ? std::vector<std::uint32_t>{12101800, 12101850, 9180} : std::vector<std::uint32_t>{12101800, 9180},
          true);
    push({Op::Play, c ? 21000030u : 21000020u, 0});
    push({Op::WaitRemoStart});
    push({Op::WaitRemoEnd});
    flags({9180}, false);
    push({Op::NgCycle});
    flags({6604, c ? 6602u : 6601u, 6603}, true);
    flags({c ? 23u : 22u}, true);
    Op e{Op::EndingCheck};
    e.ending = static_cast<int>(id);
    push(e);
}

void GuestStory::AnswerEnding(bool yes, int ending) {
    if (ops_.empty() || ops_.front().kind != Op::AskEnding) {
        return;
    }
    ops_.pop_front();
    op_started_ = false;
    if (yes) {
        cur_.id = static_cast<std::uint32_t>(ending);
        PushEndingBody(ending);
    } else {
        ops_.push_back(Op{Op::TravelDream});  // home to the own Dream; the save keeps its cycle
        ops_.push_back(Op{Op::WaitArrive});
    }
    dirty_ = true;
}

int GuestEndingType(int host_ending, bool own_three_cords) {
    if (host_ending == 1) {
        return 1;
    }
    return own_three_cords ? 3 : 2;
}

void GuestStory::Finish(bool ok) {
    (void)ok;
    running_ = false;
    running_mirror_ = false;
    running_summoned_ = false;
    host_ended_ = end_mirror_ = false;
    ops_.clear();
    dirty_ = true;
}

std::optional<StoryAction> GuestStory::RunOps(const StoryState& st, double now) {
    const bool gate = st.world_up && !st.loading && !st.transition_requested;
    if (running_mirror_ && st.session_role != 6) {
        Finish(false); // left the host world: the replay queue takes over
        return std::nullopt;
    }
    while (!ops_.empty()) {
        Op& op = ops_.front();
        if (!op_started_) {
            op_started_ = true;
            op_since_ = now;
            load_seen_ = false;
        }
        switch (op.kind) {
        case Op::Play: {
            if (!gate || st.remo_playing) {
                return std::nullopt;
            }
            StoryAction a;
            a.kind = StoryAction::PlayRemo;
            a.id = RemoForSex(op.id, st.sex_variant);
            a.mode = op.mode;
            a.mirror = running_mirror_;
            a.why = running_mirror_ ? "live mirror" : "replay";
            ops_.pop_front();
            op_started_ = false;
            return a;
        }
        case Op::WaitRemoStart:
            if (st.remo_playing) {
                ops_.pop_front();
                op_started_ = false;
                continue;
            }
            if (now - op_since_ > kRemoStartTimeout) {
                // Never started (queue refused or a one-frame scene): skip the end wait.
                ops_.pop_front();
                if (!ops_.empty() && ops_.front().kind == Op::WaitRemoEnd) {
                    ops_.pop_front();
                }
                op_started_ = false;
                continue;
            }
            return std::nullopt;
        case Op::WaitRemoEnd:
            if (!st.remo_playing || now - op_since_ > kRemoTimeout) {
                const bool cut = running_mirror_ && !host_ended_ && !st.remo_playing && now - op_since_ < kMirrorMinSeen;
                if (cut) {
                    mirror_cut_ = BaseRemo(cur_.id);
                }
                if (cur_.kind == StoryKind::Cutscene && !cut) {
                    seen_.insert(BaseRemo(cur_.id));
                    dirty_ = true;
                }
                ops_.pop_front();
                op_started_ = false;
                continue;
            }
            return std::nullopt;
        case Op::Flags: {
            if (!gate) {
                return std::nullopt;
            }
            StoryAction a;
            a.kind = StoryAction::SetFlags;
            a.flags = op.flags;
            a.on = op.on;
            a.why = StoryKindName(cur_.kind);
            ops_.pop_front();
            op_started_ = false;
            return a;
        }
        case Op::Tod: {
            if (!gate) {
                return std::nullopt;
            }
            const int want = TodFlagValue(op.tod);
            ops_.pop_front();
            op_started_ = false;
            if (want <= st.tod_value) {
                continue; // never turn the guest's clock back
            }
            StoryAction a;
            a.kind = StoryAction::ApplyTod;
            a.tod = op.tod;
            a.why = "time of day";
            return a;
        }
        case Op::NgCycle: {
            if (!gate) {
                return std::nullopt;
            }
            ops_.pop_front();
            op_started_ = false;
            StoryAction a;
            a.kind = StoryAction::NgCycle;
            a.why = "ending";
            return a;
        }
        case Op::TravelDream: {
            if (!gate) {
                return std::nullopt;
            }
            ops_.pop_front();
            op_started_ = false;
            if (st.own_world && (st.map_id >> 16) == 0x1500) { // m21_00: already in the own Dream
                if (!ops_.empty() && ops_.front().kind == Op::WaitArrive) {
                    ops_.pop_front();
                }
                continue;
            }
            StoryAction a;
            a.kind = StoryAction::TravelDream;
            a.id = kDreamLamp;
            a.why = "ending";
            return a;
        }
        case Op::WaitArrive:
            load_seen_ = load_seen_ || st.loading || !st.world_up;
            if (load_seen_ && gate && (st.map_id >> 16) == 0x1500 && stable_own_ >= kStableTicks) {
                ops_.pop_front();
                op_started_ = false;
                continue;
            }
            if (now - op_since_ > kTravelTimeout) {
                Finish(false); // never arrived: drop the ending (logged by the caller)
                return std::nullopt;
            }
            return std::nullopt;
        case Op::AskEnding: {
            if (!gate || st.remo_playing) {
                return std::nullopt;
            }
            if (!op.asked) {
                op.asked = true;
                op_since_ = now;
                StoryAction a;
                a.kind = StoryAction::AskEnding;
                a.ending = op.ending;
                a.why = "the host's ending";
                return a;
            }
            if (now - op_since_ > kEndingAskTimeout) {
                AnswerEnding(false, op.ending);  // no answer: not now
                continue;
            }
            return std::nullopt;
        }
        case Op::EndingCheck:
            // The flag (21/22/23) is set; 0x193AC10 should request the ending step itself (a
            // loading screen follows). Probe C7: if not, request it like RequestEnding.
            load_seen_ = load_seen_ || st.loading || !st.world_up;
            if (load_seen_) {
                ops_.pop_front();
                op_started_ = false;
                continue;
            }
            if (now - op_since_ > (op.ending == 1 ? kRemoTimeout : kEndingWait)) {
                const int ending = op.ending;
                ops_.pop_front();
                op_started_ = false;
                if (ending == 1) {
                    continue; // A is the game's own event: never forced (it does NG+ itself)
                }
                StoryAction a;
                a.kind = StoryAction::ForceEnding;
                a.ending = ending;
                a.why = "no ending step after the flag";
                return a;
            }
            return std::nullopt;
        }
    }
    Finish(true);
    return std::nullopt;
}

std::optional<StoryAction> GuestStory::Run(const StoryState& st, double now) {
    std::optional<StoryAction> a = RunOps(st, now);
    if (a && running_ && ops_.empty()) {
        Finish(true); // that was the last step
    }
    return a;
}

std::optional<StoryAction> GuestStory::Step(const StoryState& st, double now) {
    const bool base = st.world_up && !st.loading && !st.transition_requested && !st.remo_playing;
    stable_mirror_ = base && st.session_role == 6 ? stable_mirror_ + 1 : 0;
    const bool own = st.own_world && st.session_role != 4 && st.session_role != 6 && st.session_role != 7;
    stable_own_ = st.world_up && !st.loading && !st.transition_requested && own ? stable_own_ + 1 : 0;

    if (running_) {
        return Run(st, now);
    }
    if (mirror_) {
        if (now - mirror_since_ > kMirrorTimeout || st.session_role != 6) {
            mirror_.reset();
        } else if (stable_mirror_ >= kStableTicks &&
                   !MirrorSceneOk(mirror_->id, st.map_id, SceneBlockLoaded(st, mirror_->id), st.host_dist)) {
            mirror_other_map_ = mirror_->id;
            mirror_other_map_at_ = st.map_id;
            mirror_.reset();
            return std::nullopt;
        } else if (stable_mirror_ >= kStableTicks) {
            const StoryIntent s = *mirror_;
            mirror_.reset();
            if (!MirrorMapOk(s.id, st.map_id)) {
                mirror_nearby_ = s.id;
            }
            Build(s, true);
            return Run(st, now);
        } else {
            return std::nullopt;
        }
    }
    if (!queue_.empty() && stable_own_ >= kStableTicks && !st.remo_playing) {
        const StoryIntent s = queue_.front();
        queue_.pop_front();
        dirty_ = true;
        Build(s, false);
        return Run(st, now);
    }
    // Seamless play: a guest may stay summoned for the whole session. Everything but an ending
    // runs as a phantom too (mode-2 play, own-pool writes). An ending comes last: the guest
    // leaves the host's world by the lamp warp to its own Hunter's Dream (a client's warp loads
    // its own world, as every B1 travel follow does) and the ending program runs there - the
    // native ending step, staff roll, NG+. The director rings no bell while it runs (Busy).
    const bool waking_queued = std::any_of(queue_.begin(), queue_.end(), [](const StoryIntent& q) {
        return q.kind == StoryKind::Cutscene && ReplayIgnoresCalm(q.id);
    });
    if (!queue_.empty() && summoned_replay && !st.own_world && stable_mirror_ >= kStableTicks &&
        (st.calm || waking_queued) && !st.remo_playing) {
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (it->kind == StoryKind::Ending) {
                continue;
            }
            if (!st.calm && !(it->kind == StoryKind::Cutscene && ReplayIgnoresCalm(it->id))) {
                continue;
            }
            // A scene of another map waits (for its map, or for the own world as before): played
            // here it drew without its scenery, as the live mirror did (MirrorMapOk).
            if (it->kind == StoryKind::Cutscene &&
                !MirrorSceneOk(it->id, st.map_id, SceneBlockLoaded(st, it->id), st.host_dist)) {
                continue;
            }
            const StoryIntent s = *it;
            queue_.erase(it);
            dirty_ = true;
            Build(s, false, true);
            return Run(st, now);
        }
        if (summoned_ending) {
            for (auto it = queue_.begin(); it != queue_.end(); ++it) {
                if (it->kind != StoryKind::Ending) {
                    continue;
                }
                const StoryIntent s = *it;
                queue_.erase(it);
                dirty_ = true;
                Build(s, false, false); // the warp home first: the rest runs in the own Dream
                return Run(st, now);
            }
        }
    }
    return std::nullopt;
}

json::Value GuestStory::ToJson() const {
    json::Value v = json::Value::make_object();
    v.set("last_seq", json::hex(last_seq_));
    json::Value q = json::Value::make_array();
    // An item under way is kept (replayed again after a restart: its steps are idempotent).
    if (running_ && !running_mirror_) {
        q.push(StoryToJson(cur_));
    }
    for (const StoryIntent& s : queue_) {
        q.push(StoryToJson(s));
    }
    v.set("queue", q);
    json::Value seen = json::Value::make_array();
    for (std::uint32_t id : seen_) {
        seen.push(id);
    }
    v.set("seen", seen);
    return v;
}

bool GuestStory::FromJson(const json::Value& v, std::string* error) {
    try {
        if (v.type != json::Value::Type::Object) {
            throw std::runtime_error("not an object");
        }
        std::deque<StoryIntent> q;
        for (const json::Value& item : json::arr(v, "queue")) {
            StoryIntent s;
            std::string err;
            if (!StoryFromJson(item, &s, &err)) {
                throw std::runtime_error("queue: " + err);
            }
            q.push_back(s);
        }
        std::set<std::uint32_t> seen;
        for (const json::Value& id : json::arr(v, "seen")) {
            seen.insert(static_cast<std::uint32_t>(json::as_u64(id, "seen")));
        }
        last_seq_ = std::max(last_seq_, json::u64(v, "last_seq"));
        queue_ = std::move(q);
        seen_ = std::move(seen);
        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

// ---------------------------------------------------------------------------------------------
// Game part
// ---------------------------------------------------------------------------------------------
bool StoryNoticeMessage(std::uint32_t message_id) {
    return message_id == 10012014u;  // m28_00 12800140 (host only): "Ritual secret broken. ..."
}

std::string StoryNoticeToJsonText(const StoryNotice& n) {
    json::Value v = json::Value::make_object();
    v.set("seq", json::hex(n.seq));
    v.set("message", static_cast<unsigned>(n.message));
    v.set("event", static_cast<unsigned>(n.event_id));
    return json::dump(v, 0);
}

bool StoryNoticeFromJsonText(const std::string& text, StoryNotice* out, std::string* error) {
    json::Value v;
    std::string err;
    if (text.size() > 512 || !json::parse(text, v, err)) {
        if (error) {
            *error = err.empty() ? "too long" : err;
        }
        return false;
    }
    try {
        if (v.type != json::Value::Type::Object) {
            throw std::runtime_error("not an object");
        }
        StoryNotice n;
        n.seq = json::u64(v, "seq");
        n.message = json::u32(v, "message");
        n.event_id = json::u32(v, "event");
        if (n.seq == 0 || !StoryNoticeMessage(n.message)) {
            throw std::runtime_error("not a story notice");
        }
        *out = n;
        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

bool StoryNoticeShowNow(bool world_up, bool loading, bool cutscene, bool dialog_up, bool calm) {
    return world_up && !loading && !cutscene && !dialog_up && calm;
}

#ifndef BB_PARTY_STORY_NO_GAME

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using ull = unsigned long long;
using Clock = std::chrono::steady_clock;

constexpr u64 kRemoHandler = 0x17c53b0;  // bank-2002 EMEVD handler (this, SprjEmkEventIns*)
constexpr u64 kPlayRemo = 0x131a9f0;     // (unused, remo id, mode, a4, a5, player) -> al
// 2002[6]'s call (0x17C5853): (unused, remo id, mode, &packed map, point, 0, [0, player, tod]) -> al
constexpr u64 kPlayRemoWarpTod = 0x131bc10;
constexpr u64 kSprjRemo = 0x5540058;     // singleton slot; *(+8) = remo player; +0x168 bit 0 playing
constexpr u64 kEventFlagMan = 0x553b100; // singleton slot
constexpr u64 kSetEventFlag = 0x13cfcc0; // (man, id, on)
constexpr u64 kGetFlagValue = 0x13cfd80; // (man, first, bits) -> value
constexpr u64 kSetFlagValue = 0x13d0060; // (man, first, bits, value)
constexpr u64 kTodWorld = 0x553b148;     // *(slot)+0x81 = 1: re-evaluate time of day (0x1CECEB0)
constexpr u64 kGameDataMan = 0x553b130;  // +0x68 NG cycle, +0x6C ending type
constexpr u64 kWts = 0x5556678;          // +0x08 transition requested, +0x1550 ending request, +0x1592 own world
constexpr u64 kWorldChrMan = 0x553e878;  // +0x60 local PlayerIns (vtable +0x1C8 -> chr data, +0xCA sex)

using PlayRemoFn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64);
using PlayRemoWarpTodFn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64, u64, u64, u64);
bool g_warp_tod_ok = false;  // 0x131BC10 has the 1.09 bytes
using SetFlagFn = void(BB_COOP_SYSV*)(u64, u32, u32);
using GetFlagValueFn = u32(BB_COOP_SYSV*)(u64, u32, u32);
using SetFlagValueFn = void(BB_COOP_SYSV*)(u64, u32, u32, u32);
using ChrDataFn = u64(BB_COOP_SYSV*)(u64);

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    perf::Scope z(perf::Zone::Log);
    coop::asyncio::Print(std::string("Party story: ") + line + "\n"); // never blocks the caller (party_async_io.h)
}

bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0' && !v[1];
}

double Now() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

std::atomic<bool> g_story_on{false};
std::atomic<double> g_last_ending_at{0};
std::atomic<u64> g_seq{0};
std::atomic<u32> g_last_remo{0};  // the last remo a 2002 play / our play asked for (the test aid's log)
u64 NextSeq() {
    u64 cur = g_seq.load();
    if (cur == 0) {
        const u64 seed = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
        g_seq.compare_exchange_strong(cur, seed);
    }
    return g_seq.fetch_add(1) + 1;
}

std::mutex g_host_mu;
HostStoryQueue g_host_queue; // under g_host_mu
std::mutex g_guest_mu;
GuestStory g_guest;          // under g_guest_mu
i32 g_player_arg = 10000;

// Main-thread state.
struct Tick {
    bool host_baseline = false;
    bool ending_sent[4] = {};
    int last_tod_value = -1;
    double world_since = 0;
    bool world_was_up = false;
    // test
    bool test_on = false, test_done = false, test_mirror = false;
    bool test_host = false;     // host:<remo>: the host plays it and sends it like a captured 2002 play
    bool test_serve = false;    // serve:<remo>: as host:, the delay after the host sent a title joiner its room (A11 race)
    bool test_leave = false;    // leave_after_mirror: a guest leaves the party once a live mirror ended
    int test_ending = 0;        // ending:<1|2|3>: queue that ending as if the host had sent it
    bool test_summoned = false; // ...only once this game is a phantom (session role 6)
    u32 last_ng = 0xffffffffu;  // GameDataMan +0x68 at the last world-up (logged when it changes)
    double ng_logged_for = -1;  // the world-up (world_since) the NG cycle was logged for
    double ending_at = 0;       // our ending replay set its flag (21/22/23) or requested the step
    double ending_log = 0;      // last "staff roll" progress line (every 10 s until the next world)
    double test_coop_since = 0; // host test: when a cooperator was first seen (0 = not yet)
    double test_coop_poll = 0;
    u32 test_remo = 0;
    bool test_up = false;       // up:<remo>: the guest plays it as its own event would and sends it up (D10)
    double test_delay = 10.0;
    // remo watch (logs)
    bool remo_was_playing = false;
    double remo_since = 0;
    std::string sidecar;
    // The ending question (GuestStory::AnswerEnding): the host's ending type, the open dialog.
    int ask_ending = 0;         // 0: none pending
    u64 ask_dialog = 0;
    double ask_since = 0, ask_test_at = 0;
    int ask_test_step = 0;
};
Tick g_tick;
}  // namespace
}  // namespace coop
extern "C" void runtime_pad_director_press(std::uint32_t buttons); // src/runtime_pad.c
namespace coop {
namespace {

u64 Slot(u64 off) {
    u64 p = 0;
    return SafeGet(Guest(off), &p) ? p : 0;
}

template <class T>
T Field(u64 base, u64 off, T fallback) {
    T v{};
    return base && SafeGet(base + off, &v) ? v : fallback;
}

bool RemoPlaying() {
    const u64 remo = Slot(kSprjRemo);
    const u64 player = Field<u64>(remo, 8, 0);
    return (Field<u8>(player, 0x168, 0) & 1) != 0;
}

bool OwnWorld() {
    return Field<u8>(Slot(kWts), 0x1592, 0) == 1;
}

int FlagValue(u32 first, u32 bits) {
    const u64 man = Slot(kEventFlagMan);
    if (!man) {
        return -1;
    }
    return static_cast<int>(reinterpret_cast<GetFlagValueFn>(Guest(kGetFlagValue))(man, first, bits));
}

/// EMEVD sex 1 (the +1000 remo files) = chr data +0xCA == 0 (1003[12] at 0x17bedd0 case 0xc).
bool SexVariant() {
    const u64 wcm = Slot(kWorldChrMan);
    const u64 player = Field<u64>(wcm, 0x60, 0);
    const u64 vt = Field<u64>(player, 0, 0);
    const u64 fn = Field<u64>(vt, 0x1c8, 0);
    if (!fn) {
        return false;
    }
    const u64 data = reinterpret_cast<ChrDataFn>(fn)(player);
    return data && Field<u8>(data, 0xca, 1) == 0;
}

PartyRole Role() {
    return PartyDirector::Get().Role();
}

// Host: the scenes it played (its own events and guests' story_up), against replays (bug 14).
std::mutex g_up_guard_mu;
HostStoryUpGuard g_up_guard;

void PushHost(const StoryIntent& s) {
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        g_host_queue.Push(s);
    }
    if (s.kind == StoryKind::Cutscene) {
        std::lock_guard<std::mutex> lk(g_up_guard_mu);
        g_up_guard.NotePlayed(s.id, Now());
    }
    Log("host %s", DescribeStory(s).c_str());
}

// ---- Host capture: the bank-2002 handler entry ----

// D10 (see the header): guest -> host plays, the guest's own recent plays, the host's queue.
std::mutex g_up_mu;
std::deque<StoryIntent> g_up;                              // guest: to send
std::vector<std::pair<u32, double>> g_self_played;         // guest: (remo, time) it played itself
std::deque<std::pair<int, StoryIntent>> g_from_guests;     // host: to play and broadcast

BB_COOP_SYSV void RemoHandlerEntry(u64, u64 event, u64, u64, u64, u64) {
    perf::Scope z(perf::Zone::HookRemo);
    if (g_story_on.load() && event && Role() == PartyRole::Guest) {
        // Diagnostics (B2): which event plays a remo on a guest in the host's world.
        u64 gi = 0;
        i32 gb = 0, gid = 0;
        u32 gev = 0, gmap = 0;
        if (SafeGet(event + 0xb0, &gi) && gi && SafeGet(gi, &gb) && gb == 2002 && SafeGet(gi + 4, &gid)) {
            SafeGet(event + 0x28, &gev);
            SafeGet(event + 0x68, &gmap);
            Log("guest: EMEVD 2002[%d] (remo) from event %u, map %08x, own world %d", gid, gev, gmap, int(OwnWorld()));
            {
                u64 a0 = 0;
                u32 r0 = 0;
                if (SafeGet(event + 0xb8, &a0) && a0 && SafeGet(a0, &r0) && r0) g_last_remo.store(r0);
            }
            // A scene of the guest's own world (e.g. its own first Dream): a later catch-up replay
            // of the host's copy keeps the flags but does not play it again.
            u64 own_args = 0;
            u32 own_remo = 0;
            if (OwnWorld() && gid >= 1 && gid <= 7 && gid != 5 && SafeGet(event + 0xb8, &own_args) && own_args &&
                SafeGet(own_args, &own_remo) && StoryRemoKnown(own_remo)) {
                std::lock_guard<std::mutex> lk(g_guest_mu);
                g_guest.NoteSeen(own_remo);
            }
            // D10: a plain play (1, 3, 7: no warp) of the guest's own event in the host's world
            // goes up to the host, who plays it for everyone.
            u64 args = 0;
            u32 remo = 0, mode = 0;
            SafeGet(event + 0xb8, &args);
            // G3: a world phase change (2002[6]: scene + time of day + a warp in this map) goes up
            // too; the host plays it as its own if it fits (GuestPhaseChangeOk), else as a scene.
            u8 raw6[0x18] = {};
            if (!OwnWorld() && gid == 6 && args && SafeRead(args, raw6, sizeof raw6)) {
                StoryIntent up;
                up.kind = StoryKind::Cutscene;
                up.instr = 6;
                std::memcpy(&up.id, raw6, 4);
                std::memcpy(&up.mode, raw6 + 4, 4);
                std::memcpy(&up.warp_point, raw6 + 8, 4);
                up.warp_map = u32(raw6[0xc]) << 24 | u32(raw6[0xd]) << 16;
                up.tod = raw6[0x14] < 4 ? raw6[0x14] : -1;
                up.event_id = gev;
                up.map = gmap;
                if (StoryRemoKnown(up.id) && up.tod >= 1) {
                    up.seq = NextSeq();
                    Log("guest: its own world phase change %s goes to the host", DescribeStory(up).c_str());
                    std::lock_guard<std::mutex> lk(g_up_mu);
                    if (g_up.size() < 8) {
                        g_up.push_back(up);
                    }
                    g_self_played.emplace_back(up.id, Now());
                }
            }
            if (!OwnWorld() && (gid == 1 || gid == 3 || gid == 7) && args && SafeGet(args, &remo) &&
                SafeGet(args + 4, &mode) && StoryRemoKnown(remo)) {
                StoryIntent up;
                up.kind = StoryKind::Cutscene;
                up.instr = static_cast<u32>(gid);
                up.id = remo;
                up.mode = mode;
                up.event_id = gev;
                up.map = gmap;
                up.seq = NextSeq();
                std::lock_guard<std::mutex> lk(g_up_mu);
                if (g_up.size() < 8) {
                    g_up.push_back(up);
                }
                g_self_played.emplace_back(remo, Now());
            }
        }
    }
    if (!g_story_on.load() || Role() != PartyRole::Host || !event) {
        return;
    }
    u64 instr = 0;
    if (!SafeGet(event + 0xb0, &instr) || !instr) {
        return;
    }
    i32 bank = 0, id = 0;
    SafeGet(instr, &bank);
    SafeGet(instr + 4, &id);
    if (bank != 2002 || id < 1 || id > 7 || id == 5) {
        return; // 8 = dummy remo + warp (B1 travel covers it), 5 unused in 1.09
    }
    if (!OwnWorld()) {
        return; // a summoned host? only plays of our own world are the party's
    }
    // The arguments exactly as 0x17C53B0 finds them.
    u64 args = 0;
    SafeGet(event + 0xb8, &args);
    if (!args) {
        u64 runtime = 0, base = 0, section = 0;
        i64 arg_off = 0;
        if (SafeGet(event + 0xa8, &runtime) && runtime && SafeGet(runtime + 8, &base) && base &&
            SafeGet(base + 0x78, &section) && SafeGet(instr + 0x10, &arg_off)) {
            args = base + section + u64(arg_off);
        }
    }
    u8 raw[0x18] = {};
    if (!args || !SafeRead(args, raw, sizeof raw)) {
        return;
    }
    auto rd32 = [&](int off) {
        u32 v;
        std::memcpy(&v, raw + off, 4);
        return v;
    };
    StoryIntent s;
    s.kind = StoryKind::Cutscene;
    s.instr = static_cast<u32>(id);
    s.id = rd32(0);
    s.mode = rd32(4);
    g_last_remo.store(s.id);
    SafeGet(event + 0x28, &s.event_id);
    SafeGet(event + 0x68, &s.map);
    switch (id) {
    case 2:
    case 4:
        s.warp_point = rd32(8);
        s.warp_map = u32(raw[0xc]) << 24 | u32(raw[0xd]) << 16;
        break;
    case 6:
        s.warp_point = rd32(8);
        s.warp_map = u32(raw[0xc]) << 24 | u32(raw[0xd]) << 16;
        s.tod = raw[0x14] < 4 ? raw[0x14] : -1;
        break;
    case 7:
        s.tod = raw[0xc] < 4 ? raw[0xc] : -1;
        break;
    default:
        break;
    }
    const int ending = EndingTypeOfEvent(s.event_id);
    if (ending) {
        // Before the room closes (the flag 21/22/23 comes after the remo).
        StoryIntent e;
        e.seq = NextSeq();
        e.kind = StoryKind::Ending;
        e.id = static_cast<u32>(ending);
        e.event_id = s.event_id;
        e.map = s.map;
        if (!g_tick.ending_sent[ending]) { // a benign race with the tick: worst case a duplicate the guest drops
            g_tick.ending_sent[ending] = true;
            PushHost(e);
        }
        return;
    }
    s.flags = StoryFlagsForRemo(s.id);
    s.seq = NextSeq();
    PushHost(s);
}

// ---- Guest actions ----

/// Summoned (flag store load mode 1): the live store is the host's overlay; story state goes into
/// the own save's pool instead (C2's pool write), the overlay is never touched.
bool Summoned() {
    return progress::FlagLoadMode() == 1;
}

int OwnTodValue() {
    int v = 0;
    for (u32 i = 0; i < 3; ++i) {
        bool b = false;
        if (!progress::ReadFlagOwnSave(9800 + i, &b)) {
            return -1;
        }
        v |= (b ? 1 : 0) << (2 - i); // 9800 = bit 2
    }
    return v;
}

void SetFlags(const std::vector<u32>& flags, bool on) {
    if (Summoned()) {
        for (u32 f : flags) {
            if (!progress::WriteFlagOwnSave(f, on)) {
                Log("flag %u: not in the own save's pool; not set while summoned", f);
            }
        }
        RequestGameSave("C4 story flags");
        return;
    }
    const u64 man = Slot(kEventFlagMan);
    if (!man) {
        Log("no EventFlagMan: flags not set");
        return;
    }
    for (u32 f : flags) {
        reinterpret_cast<SetFlagFn>(Guest(kSetEventFlag))(man, f, on ? 1 : 0);
    }
}

/// No HP lost for 15 s (ChrIns +0x3B0 -> +0x20: +0xF8 HP), as party_items' popup rule.
bool Calm(double now) {
    static u64 last_player = 0;
    static i32 last_hp = -1;
    static double last_hit = -1e9;
    const u64 player = Field<u64>(Slot(kWorldChrMan), 0x60, 0);
    const u64 a = Field<u64>(player, 0x3b0, 0);
    const u64 b = Field<u64>(a, 0x20, 0);
    const i32 hp = Field<i32>(b, 0xf8, -1);
    if (!player || !a || !b || hp < 0) {
        return true;
    }
    if (player == last_player && last_hp >= 0 && hp < last_hp) {
        last_hit = now;
    }
    last_player = player;
    last_hp = hp;
    return now - last_hit >= 15.0;
}

void Run(const StoryAction& a) {
    switch (a.kind) {
    case StoryAction::PlayRemo: {
        g_last_remo.store(a.id);
        const u64 ok = reinterpret_cast<PlayRemoFn>(Guest(kPlayRemo))(0, a.id, a.mode, 0xffffffffu, 0xffffffffu,
                                                                       static_cast<u32>(g_player_arg));
        Log("%s: remo %u mode %u player %d -> %s", a.why.c_str(), a.id, a.mode, g_player_arg,
            (ok & 0xff) ? "queued" : "NOT queued (no remo player)");
        break;
    }
    case StoryAction::SetFlags: {
        const bool summoned = Summoned();
        SetFlags(a.flags, a.on);
        std::string list;
        for (u32 f : a.flags) {
            bool v = false;
            const int value = summoned ? (progress::ReadFlagOwnSave(f, &v) ? int(v) : -1) : FlagValue(f, 1);
            list += (list.empty() ? "" : ",") + std::to_string(f) + "=" + std::to_string(value);
        }
        Log("%s: flags set %s%s (read back: %s)", a.why.c_str(), a.on ? "ON" : "OFF",
            summoned ? " in the own save's pool (summoned; overlay untouched)" : "", list.c_str());
        break;
    }
    case StoryAction::ApplyTod: {
        const u64 man = Slot(kEventFlagMan);
        const int v = TodFlagValue(a.tod);
        if (v >= 0 && Summoned()) {
            // The own save only; the world shown stays the host's (no refresh byte).
            const int before = OwnTodValue();
            for (u32 i = 0; i < 3; ++i) {
                progress::WriteFlagOwnSave(9800 + i, ((v >> (2 - i)) & 1) != 0);
            }
            Log("time of day %d while summoned: own save 9800..9802 %d -> %d (pool; overlay untouched)", a.tod,
                before, OwnTodValue());
            RequestGameSave("C4 time of day");
            break;
        }
        if (man && v >= 0) {
            const int before = FlagValue(9800, 3);
            reinterpret_cast<SetFlagValueFn>(Guest(kSetFlagValue))(man, 9800, 3, static_cast<u32>(v));
            if (const u64 w = Slot(kTodWorld)) {
                const u8 one = 1;
                SafeWrite(w + 0x81, &one, 1);
            }
            Log("time of day %d: 9800..9802 %d -> %d", a.tod, before, FlagValue(9800, 3));
        }
        break;
    }
    case StoryAction::NgCycle: {
        const u64 gdm = Slot(kGameDataMan);
        u32 ng = Field<u32>(gdm, 0x68, 0xffffffffu);
        if (ng != 0xffffffffu) {
            const u32 next = ng + 1 > 7 ? 7 : ng + 1;
            SafeWrite(gdm + 0x68, &next, 4);
            Log("NG cycle %u -> %u", ng, next);
        }
        break;
    }
    case StoryAction::TravelDream: {
        // As a travel replay (the travel / guest-lamp hooks stand aside): a phantom's lamp warp
        // leaves the host's world and loads its own (B1's follow), an own-world one just warps.
        const GameSnapshot g = ReadGameState(false);
        Log("ending: warping to the own Hunter's Dream (%u)%s", a.id,
            g.session_role == RoleClient ? " - leaving the host's world (summoned)" : "");
        RunLampWarpReplay(a.id);
        break;
    }
    case StoryAction::AskEnding:
        g_tick.ask_ending = a.ending;
        g_tick.ask_dialog = 0;
        g_tick.ask_since = Now();
        g_tick.ask_test_step = 0;
        Log("ending %d: the host reached it; asking this player \"play this ending now?\"", a.ending);
        break;
    case StoryAction::ForceEnding: {
        const u64 gdm = Slot(kGameDataMan), wts = Slot(kWts);
        const u32 type = static_cast<u32>(a.ending);
        const u8 req = Field<u8>(wts, 0x1550, 0);
        Log("ending %d: no ending step yet (GSM+0x1550 %u, GameDataMan+0x6C %u); requesting it", a.ending, req,
            Field<u32>(gdm, 0x6c, 0));
        if (gdm && wts) {
            const u8 one = 1;
            SafeWrite(gdm + 0x6c, &type, 4);
            SafeWrite(wts + 0x1550, &one, 1);
        }
        break;
    }
    case StoryAction::None:
        break;
    }
}

// ---- Sidecar ----

void LoadSidecar() {
    const char* dir = std::getenv("BB_GPU_USER_DIR");
    if (!dir || !dir[0]) {
        return;
    }
    g_tick.sidecar = std::string(dir) + "/party_story.json";
    std::ifstream in(g_tick.sidecar, std::ios::binary);
    if (!in) {
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    json::Value v;
    std::string err;
    std::lock_guard<std::mutex> lk(g_guest_mu);
    if (!json::parse(ss.str(), v, err) || !g_guest.FromJson(v, &err)) {
        Log("%s: %s; ignored", g_tick.sidecar.c_str(), err.c_str());
        return;
    }
    g_guest.TakeDirty();
    Log("%s: %zu queued, last seq %llu", g_tick.sidecar.c_str(), g_guest.QueueSize(), ull(g_guest.LastSeq()));
}

void SaveSidecar() {
    if (g_tick.sidecar.empty()) {
        return;
    }
    perf::Scope z(perf::Zone::StorySidecar);
    std::string text;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        if (!g_guest.TakeDirty()) {
            return;
        }
        text = json::dump(g_guest.ToJson(), 1);
    }
    // .tmp, remove, rename - on the party's writer thread, never on the game's main thread.
    coop::asyncio::Replace(g_tick.sidecar, std::move(text));
}

// ---- Host polls: endings (flags 21/22/23) and time of day ----

std::atomic<bool> g_member_joined{false};

void HostPoll(const GameSnapshot& g) {
    if (!g.world_up || g.loading || !OwnWorld()) {
        g_tick.host_baseline = false;
        return;
    }
    const int tod_value = FlagValue(9800, 3);
    int endings[4] = {0, FlagValue(21, 1), FlagValue(22, 1), FlagValue(23, 1)};
    if (!g_tick.host_baseline) {
        // A loaded save may already hold them: only changes from here on are news.
        g_tick.host_baseline = true;
        g_tick.last_tod_value = tod_value;
        for (int i = 1; i <= 3; ++i) {
            g_tick.ending_sent[i] = g_tick.ending_sent[i] || endings[i] == 1;
        }
        return;
    }
    for (int i = 1; i <= 3; ++i) {
        if (endings[i] == 1 && !g_tick.ending_sent[i]) {
            g_tick.ending_sent[i] = true;
            StoryIntent e;
            e.seq = NextSeq();
            e.kind = StoryKind::Ending;
            e.id = static_cast<u32>(i);
            PushHost(e);
        }
    }
    // Only once the member is in the host's world (a cooperator): a time-of-day item queued on a
    // guest still at home made its story queue busy, and a busy queue holds its bell - live, the
    // guest never rang and never joined.
    static const bool tod_catchup = [] {  // opt-in for 0.5.2 (BB_PARTY_TOD_CATCHUP=1): not re-run live
        const char* v = std::getenv("BB_PARTY_TOD_CATCHUP");
        return v && v[0] == '1';
    }();
    if (tod_catchup && g_member_joined.load() && tod_value > 0 && ReadGameState(true).cooperators > 0 &&
        g_member_joined.exchange(false)) {
        // Catch-up (user decision: a guest behind the host gets the Blood Moon too): a member that
        // joined late never saw the change; the guest's planner never turns its clock back.
        StoryIntent t;
        t.seq = NextSeq();
        t.kind = StoryKind::TimeOfDay;
        t.id = static_cast<u32>(TodFromFlagValue(tod_value));
        PushHost(t);
    }
    if (tod_value > g_tick.last_tod_value && tod_value >= 0) {
        StoryIntent t;
        t.seq = NextSeq();
        t.kind = StoryKind::TimeOfDay;
        t.id = static_cast<u32>(TodFromFlagValue(tod_value));
        PushHost(t);
    }
    if (tod_value >= 0) {
        g_tick.last_tod_value = tod_value;
    }
}

void ParseTest() {
    const char* t = std::getenv("BB_PARTY_STORY_TEST");
    if (!t || !t[0]) {
        return;
    }
    std::string s(t);
    if (s == "leave_after_mirror") {
        g_tick.test_leave = true;
        Log("test: the guest leaves the party when a live mirror has ended");
    } else if (s.rfind("host:", 0) == 0) {
        g_tick.test_on = g_tick.test_host = true;
        g_tick.test_remo = static_cast<u32>(std::strtoul(s.c_str() + 5, nullptr, 0));
        if (const char* d = std::getenv("BB_PARTY_STORY_TEST_DELAY"); d && d[0]) {
            g_tick.test_delay = std::atof(d);
        }
        Log("test: host plays remo %u and sends it to the party %.0f s after a cooperator is in the world",
            g_tick.test_remo, g_tick.test_delay);
    } else if (s.rfind("serve:", 0) == 0) {
        // A11 (pard S4 / parab a11c): a title joiner admitted just before a cutscene begins.
        g_tick.test_on = g_tick.test_host = g_tick.test_serve = true;
        g_tick.test_remo = static_cast<u32>(std::strtoul(s.c_str() + 6, nullptr, 0));
        g_tick.test_delay = 1.5;
        if (const char* d = std::getenv("BB_PARTY_STORY_TEST_DELAY"); d && d[0]) {
            g_tick.test_delay = std::atof(d);
        }
        Log("test: host plays remo %u and sends it to the party %.1f s after it sent a title joiner its room",
            g_tick.test_remo, g_tick.test_delay);
    } else if (s.rfind("up:", 0) == 0) {
        g_tick.test_on = g_tick.test_up = true;
        g_tick.test_remo = static_cast<u32>(std::strtoul(s.c_str() + 3, nullptr, 0));
        if (const char* d = std::getenv("BB_PARTY_STORY_TEST_DELAY"); d && d[0]) {
            g_tick.test_delay = std::atof(d);
        }
        Log("test: the guest plays remo %u as if its own event did, %.0f s after it is summoned, and sends it up (D10)",
            g_tick.test_remo, g_tick.test_delay);
    } else if (s.rfind("ending:", 0) == 0) {
        g_tick.test_on = true;
        g_tick.test_ending = std::atoi(s.c_str() + 7);
        g_tick.test_summoned = s.find(",summoned") != std::string::npos;
        if (const char* d = std::getenv("BB_PARTY_STORY_TEST_DELAY"); d && d[0]) {
            g_tick.test_delay = std::atof(d);
        }
        if (g_tick.test_ending < 1 || g_tick.test_ending > 3) {
            g_tick.test_on = false;
            Log("BB_PARTY_STORY_TEST: ending must be 1, 2 or 3; ignored");
        } else {
            Log("test: queue ending %d %.0f s after the world is up%s", g_tick.test_ending, g_tick.test_delay,
                g_tick.test_summoned ? ", once summoned" : "");
        }
    } else if (s.rfind("cutscene:", 0) == 0) {
        g_tick.test_on = true;
        g_tick.test_remo = static_cast<u32>(std::strtoul(s.c_str() + 9, nullptr, 0));
        g_tick.test_mirror = s.find(",mirror") != std::string::npos;
        if (const char* d = std::getenv("BB_PARTY_STORY_TEST_DELAY"); d && d[0]) {
            g_tick.test_delay = std::atof(d);
        }
        Log("test: replay remo %u %.0f s after the world is up%s", g_tick.test_remo, g_tick.test_delay,
            g_tick.test_mirror ? " (mirror path, mode 2)" : "");
    } else {
        Log("BB_PARTY_STORY_TEST: '%s' is not cutscene:<id>[,mirror], ending:<n>[,summoned], host:<id> or "
            "leave_after_mirror; ignored",
            t);
    }
}

} // namespace

bool StoryTestRequested() {
    const char* t = std::getenv("BB_PARTY_STORY_TEST");
    return t && t[0];
}

void InstallStoryHooks() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) {
        return;
    }
    {
        const char* ask = std::getenv("BB_PARTY_ENDING_ASK");
        const bool on = !(ask && ask[0] == '0');
        std::lock_guard<std::mutex> lk(g_guest_mu);
        g_guest.SetAskEnding(on);
        Log("ending question for guests: %s (BB_PARTY_ENDING_ASK=0: the host's ending replays at once)",
            on ? "on" : "off");
    }
    if (EnvOff("BB_PARTY_STORY")) {
        Log("BB_PARTY_STORY=0: no cutscene / ending sync");
        return;
    }
    if (!Image()) {
        Log("no image; story off");
        return;
    }
    if (const char* p = std::getenv("BB_PARTY_STORY_PLAYER"); p && p[0]) {
        g_player_arg = static_cast<i32>(std::strtol(p, nullptr, 0));
    }
    g_guest.mirror_enabled = !EnvOff("BB_PARTY_STORY_MIRROR");
    g_guest.summoned_replay = !EnvOff("BB_PARTY_STORY_SUMMONED");
    g_guest.summoned_ending = !EnvOff("BB_PARTY_STORY_ENDING_SUMMONED");
    // Reference sites (cutscenes_endings.md 5.1): play remo, time-of-day apply; the calls need them.
    const bool play_ok = Matches(kPlayRemo, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54});
    const bool tod_ok =
        Matches(0x1ceceb0, {0x55, 0x48, 0x89, 0xe5, 0x48, 0x89, 0xf8, 0x83, 0x78, 0x0c, 0x01, 0x75, 0x41});
    g_warp_tod_ok = Matches(kPlayRemoWarpTod, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                                0x53, 0x48, 0x81, 0xec, 0x28, 0x03, 0x00, 0x00});
    if (!play_ok || !tod_ok) {
        Log("0x131A9F0 / 0x1CECEB0 are not the 1.09 bytes (%s / %s); story off", play_ok ? "ok" : "MISMATCH",
            tod_ok ? "ok" : "MISMATCH");
        return;
    }
    // push rbp; mov rbp, rsp; push r15, r14, r13, r12, rbx; sub rsp, 0x68 (17 bytes; next is the
    // rip-relative stack-guard load).
    HookPrologue(kRemoHandler,
                 {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x68},
                 &RemoHandlerEntry, "story capture (EMEVD bank 2002 handler)");
    ParseTest();
    LoadSidecar();
    g_story_on = true;
    Log("on: mirror %s, player argument %d", g_guest.mirror_enabled ? "on" : "off", g_player_arg);
}

// Guest: EMEVD events whose cutscene the live mirror played (event id -> Now()). The host-world
// copy of the same event runs on the client too and plays the scene again once the host's event
// flag reaches it (live: Mergo's arena entry 26000010 played twice, the 2nd 0.1 s after the
// mirror's ended, `guest: EMEVD 2002[3] ... from event 12601802`); the EMEVD skip predicate
// (party_b2 ClientIntroSkip) drops the client's 2002 of such an event.
constexpr double kMirrorDedupeSeconds = 600.0;
std::mutex g_mirrored_mu;
std::map<std::uint32_t, double> g_mirrored;
// Guest: its own plays of boss intros held for the host's copy (StoryHoldClientOwnScene).
std::mutex g_hold_mu;
OwnSceneHold g_hold;
// Guest: the host's scene is pending / playing here as the live mirror (the A11 notice waits).
std::atomic<bool> g_mirror_playing{false};

namespace {
std::mutex g_notice_mu;
std::deque<StoryNotice> g_notice_host;                      // host: to broadcast
std::deque<std::pair<StoryNotice, double>> g_notice_guest;  // guest: to show (since)
std::uint64_t g_notice_last_seq = 0;
}  // namespace

void NoteHostStoryNotice(std::uint32_t message_id, std::uint32_t event_id) {
    if (!g_story_on.load() || Role() != PartyRole::Host || !StoryNoticeMessage(message_id)) {
        return;
    }
    StoryNotice n;
    n.seq = NextSeq();
    n.message = message_id;
    n.event_id = event_id;
    std::lock_guard<std::mutex> lk(g_notice_mu);
    if (g_notice_host.size() < 8) {
        g_notice_host.push_back(n);
    }
    Log("host: story notice %u (event %u) shown here: relayed to the party", message_id, event_id);
}

bool PopHostStoryNotice(StoryNotice* out) {
    std::lock_guard<std::mutex> lk(g_notice_mu);
    if (g_notice_host.empty()) {
        return false;
    }
    *out = g_notice_host.front();
    g_notice_host.pop_front();
    return true;
}

void RequestGuestStoryNotice(const StoryNotice& n) {
    std::lock_guard<std::mutex> lk(g_notice_mu);
    if (n.seq <= g_notice_last_seq || g_notice_guest.size() >= 8) {
        return;
    }
    g_notice_last_seq = n.seq;
    g_notice_guest.emplace_back(n, Now());
    Log("guest: the host's story notice %u (event %u) queued", n.message, n.event_id);
}

// Guest, main thread: one queued notice as the game's one-button message once calm.
void GuestNoticeTick(const GameSnapshot& g, double now) {
    StoryNotice n;
    double since = 0;
    {
        std::lock_guard<std::mutex> lk(g_notice_mu);
        if (g_notice_guest.empty()) {
            return;
        }
        n = g_notice_guest.front().first;
        since = g_notice_guest.front().second;
        if (now - since > kStoryNoticeMaxWait) {
            g_notice_guest.pop_front();
            Log("guest: story notice %u dropped (not shown in %.0f s)", n.message, kStoryNoticeMaxWait);
            return;
        }
    }
    // Opened: is it on screen (menu +0x78)? A box opened in the frames a load ends never shows
    // (live R1: "shown" under the loading screen, gone with it): open it again, 3 times at most.
    static std::uint64_t open_seq = 0;
    static double open_at = 0;
    static int opens = 0;
    if (open_seq == n.seq) {
        if (split::DialogUp()) {
            std::lock_guard<std::mutex> lk(g_notice_mu);
            if (!g_notice_guest.empty() && g_notice_guest.front().first.seq == n.seq) {
                g_notice_guest.pop_front();
            }
            Log("guest: story notice %u on screen (game message, OK) %.1f s after the host's", n.message, now - since);
            open_seq = 0;
            return;
        }
        if (now - open_at < 2.5) {
            return;
        }
        if (opens >= 3) {
            std::lock_guard<std::mutex> lk(g_notice_mu);
            if (!g_notice_guest.empty() && g_notice_guest.front().first.seq == n.seq) {
                g_notice_guest.pop_front();
            }
            Log("guest: story notice %u did not show after %d tries: dropped", n.message, opens);
            open_seq = 0;
            return;
        }
        Log("guest: story notice %u not on screen 2.5 s after opening: again", n.message);
    }
    const bool cut = g.world_up && RemoPlaying();
    const bool settled = g_tick.world_was_up && now - g_tick.world_since >= 3.0;
    if (!settled || !StoryNoticeShowNow(g.world_up, g.loading, cut, false, Calm(now))) {
        return;
    }
    const u32 text = BbGameMenu::Pages::StoryNoticeText(n.message);
    if (!text || !split::ShowMessage(text)) {
        return;  // another dialog / menu is up: next frame
    }
    opens = open_seq == n.seq ? opens + 1 : 1;
    open_seq = n.seq;
    open_at = now;
    Log("guest: story notice %u opened (try %d)", n.message, opens);
}

bool PopHostStory(StoryIntent* out) {
    std::lock_guard<std::mutex> lk(g_host_mu);
    return g_host_queue.Pop(out);
}

void RequestGuestStory(const StoryIntent& s) {
    if (!g_story_on.load()) {
        return;
    }
    // Role from SprjSessionManager without calling the game (any thread).
    const GameSnapshot g = ReadGameState(false);
    const bool phantom = g.session_role == RoleClient;
    if (s.kind == StoryKind::Cutscene) {
        std::lock_guard<std::mutex> lk(g_up_mu);
        for (const auto& [remo, at] : g_self_played) {
            if (remo == s.id && Now() - at < 180.0) {
                Log("guest %s: this player triggered and watched it itself; the host's copy is dropped",
                    DescribeStory(s).c_str());
                return;
            }
        }
    }
    bool taken, mirror;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        taken = g_guest.Offer(s, phantom, Now());
        mirror = taken && g_guest.MirrorPending();
    }
    if (mirror && s.kind == StoryKind::Cutscene) {
        g_mirror_playing.store(true, std::memory_order_relaxed);  // before the host's A11 hold notice
    }
    if (taken && phantom && s.kind == StoryKind::Cutscene && s.event_id) {
        std::lock_guard<std::mutex> lk(g_mirrored_mu);
        g_mirrored[s.event_id] = Now();
    }
    Log("guest %s %s%s", DescribeStory(s).c_str(), taken ? "taken" : "ignored (not newer than the last)",
        taken && phantom ? " (phantom: live mirror)" : "");
    if (mirror && s.kind == StoryKind::Cutscene) {
        std::optional<OwnSceneHold::Item> held;
        {
            std::lock_guard<std::mutex> lk(g_hold_mu);
            held = g_hold.HostCopy(s.id);
        }
        if (held) {
            Log("guest: the host's copy of cutscene %u came %.2f s after this game's own play of it (held): the "
                "host's scene plays here live; the own copy never plays",
                s.id, Now() - held->at);
        }
    }
}

bool StoryHoldClientOwnScene(std::uint32_t event_id, int id, std::uint64_t event, std::uint64_t instr) {
    if (!g_story_on.load() || Role() != PartyRole::Guest || OwnWorld() || !event || !instr) {
        return false;
    }
    bool mirror_on;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        mirror_on = g_guest.mirror_enabled;
    }
    const bool intro = worldact::FirstFightSpotFor(event_id) != nullptr;
    if (!intro || !mirror_on || (id != 1 && id != 3)) {
        return false;
    }
    u32 size = 0;
    const u64 args = EmevdInstructionArgs(event, instr, &size);
    u32 remo = 0, mode = 0, map = 0;
    if (!args || size < 8 || !SafeGet(args, &remo) || !SafeGet(args + 4, &mode)) {
        return false;
    }
    SafeGet(event + 0x68, &map);
    if (!HoldOwnSceneForHost(true, mirror_on, 2002, id, intro, StoryRemoKnown(remo))) {
        return false;
    }
    OwnSceneHold::Item it;
    it.event_id = event_id;
    it.remo = remo;
    it.mode = mode;
    it.instr = static_cast<u32>(id);
    it.map = map;
    it.at = Now();
    {
        std::lock_guard<std::mutex> lk(g_hold_mu);
        if (!g_hold.Hold(it)) {
            return false;  // full: the game plays it as before
        }
    }
    // D10 as RemoHandlerEntry sends it (the host's guard drops it when its own event played it).
    StoryIntent up;
    up.kind = StoryKind::Cutscene;
    up.instr = static_cast<u32>(id);
    up.id = remo;
    up.mode = mode;
    up.event_id = event_id;
    up.map = map;
    up.seq = NextSeq();
    {
        std::lock_guard<std::mutex> lk(g_up_mu);
        if (g_up.size() < 8) {
            g_up.push_back(up);
        }
    }
    Log("guest: event %u's own 2002[%d] (boss intro %u, mode %u) held: the host plays this intro; its copy plays "
        "here as the live mirror (this game's own copy only if it does not come in %.0f s)",
        event_id, id, remo, mode, kOwnIntroHold);
    return true;
}

/// Guest, main thread: a held own play whose host copy did not come plays now (as the game's
/// 2002 would have: the remo with the event's mode); it is this player's own copy then.
static void GuestHeldSceneTick(const GameSnapshot& g, const StoryState& st, double now) {
    std::optional<OwnSceneHold::Item> due;
    std::size_t pruned = 0;
    {
        std::lock_guard<std::mutex> lk(g_hold_mu);
        if (!g_hold.Size()) {
            return;
        }
        if (g.session_role != RoleClient) {
            pruned = g_hold.Prune(now, 0);  // left the host's world: the scene is not this world's now
        } else {
            pruned = g_hold.Prune(now);
            bool busy;
            {
                std::lock_guard<std::mutex> lk2(g_guest_mu);
                busy = g_guest.Running() || g_guest.MirrorPending();
            }
            if (g.world_up && !g.loading && !st.transition_requested && !st.remo_playing && !busy) {
                due = g_hold.Due(now);
            }
        }
    }
    if (pruned) {
        Log("guest: %zu held own boss intro play(s) dropped (left the host's world or never steady)", pruned);
    }
    if (!due) {
        return;
    }
    StoryAction a;
    a.kind = StoryAction::PlayRemo;
    a.id = RemoForSex(due->remo, SexVariant());
    a.mode = due->mode;
    a.why = "held own boss intro (the host's copy did not come)";
    Log("guest: the host's copy of cutscene %u (event %u) did not come in %.1f s: this player's own copy plays",
        due->remo, due->event_id, now - due->at);
    Run(a);
    {
        std::lock_guard<std::mutex> lk(g_up_mu);
        g_self_played.emplace_back(due->remo, now);  // a late host copy of it is dropped
    }
    std::lock_guard<std::mutex> lk(g_mirrored_mu);
    g_mirrored[due->event_id] = now;  // the event's 2002 again (a rerun) is a duplicate too
}

void StoryHostSceneEnded() {
    if (!g_story_on.load() || Role() != PartyRole::Guest) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_guest_mu);
    g_guest.HostSceneEnded();
}

bool StoryMirrorPlaying() {
    return g_mirror_playing.load(std::memory_order_relaxed);
}

bool StoryMirroredEvent(std::uint32_t event_id) {
    // Test aid BB_PARTY_TEST_MIRRORED=ID: that event counts as mirrored from the start (smoke iw).
    static const std::uint32_t test_id = [] {
        const char* v = std::getenv("BB_PARTY_TEST_MIRRORED");
        return v && *v ? static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    if (test_id && test_id == event_id) {
        return true;
    }
    std::lock_guard<std::mutex> lk(g_mirrored_mu);
    const auto it = g_mirrored.find(event_id);
    return it != g_mirrored.end() && Now() - it->second < kMirrorDedupeSeconds;
}

double StoryLastEndingAt() {
    return g_last_ending_at.load();
}

bool StoryBusy() {
    if (!g_story_on.load()) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_guest_mu);
    return g_guest.ReplayBusy();
}

void HostPlayGuestStory(const GameSnapshot& g, const StoryState& st);

void StoryTick() {
    if (!g_story_on.load()) {
        return;
    }
    const double now = Now();
    const GameSnapshot g = ReadGameState(false);
    if (g.world_up && !g.loading) {
        if (!g_tick.world_was_up) {
            g_tick.world_was_up = true;
            g_tick.world_since = now;
        }
    } else {
        g_tick.world_was_up = false;
    }
    const PartyRole role = Role();
    if (role == PartyRole::Host) {
        HostPoll(g);
    } else if (role == PartyRole::Guest) {
        GuestNoticeTick(g, now);
    }
    if (role != PartyRole::None) {
        // 0.7.4: "Cutscenes live with the host" is the host's session rule (party_status HostRuleOn).
        const bool mirror = party::status::HostRuleOn(party::status::HostRule::StoryMirror);
        std::lock_guard<std::mutex> lk(g_guest_mu);
        if (g_guest.mirror_enabled != mirror) {
            g_guest.mirror_enabled = mirror;
            Log("party rules: cutscenes live with the host %s (%s)", mirror ? "on" : "off",
                role == PartyRole::Guest ? "the host's" : "own");
        }
    }
    StoryState st;
    st.world_up = g.world_up;
    st.loading = g.loading;
    st.session_role = g.session_role;
    st.transition_requested = Field<u8>(Slot(kWts), 0x08, 1) != 0;
    st.own_world = OwnWorld();
    st.remo_playing = g.world_up && RemoPlaying();
    st.map_id = g.map_id;
    bool busy;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        busy = g_guest.Busy();
    }
    if (g.world_up && !g.loading && (busy || (g_tick.test_on && !g_tick.test_done))) {
        st.sex_variant = SexVariant(); // a virtual call into the game: only when a play may follow
    }
    if (busy && g.world_up && !g.loading && g.session_role == RoleClient) {
        // A mirror / summoned replay outside the scene's own block (MirrorSceneOk): the blocks this
        // game has loaded (its loaded enemies' map entity ids) and the host's distance.
        for (const EnemyKillCandidate& e : balance::LoadedEnemies()) {
            const u32 m = MapOfEntity(e.entity);
            if (m && std::find(st.loaded_blocks.begin(), st.loaded_blocks.end(), m) == st.loaded_blocks.end()) {
                st.loaded_blocks.push_back(m);
            }
        }
        b2::Where host;
        float me[3] = {};
        const u64 player = Field<u64>(Slot(kWorldChrMan), 0x60, 0);
        if (b2::MemberWhere(0, &host) && host.has_pos && host.age_ms < 3000 && host.map &&
            (host.map >> 24) == (g.map_id >> 24) && player && balance::ChrPosition(player, me)) {
            const float dx = host.pos[0] - me[0], dy = host.pos[1] - me[1], dz = host.pos[2] - me[2];
            st.host_dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    if (g.world_up && !g.loading) {
        // Summoned: the own save's clock (the overlay shows the host's).
        const int tv = Summoned() ? OwnTodValue() : FlagValue(9800, 3);
        st.tod_value = tv < 0 ? 0 : tv;
        st.calm = Calm(now);
    }
    party::hud::SetCutscene(st.remo_playing);  // no party HUD / tags over a cutscene
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        // pending too: the host's hold notice (A11) comes right after its story, before our remo starts
        g_mirror_playing.store(role == PartyRole::Guest && (g_guest.MirrorPending() || g_guest.RunningMirror()),
                               std::memory_order_relaxed);
    }
    // Remo watch: the playing bit's edges (both roles; the evidence for tests).
    if (st.remo_playing != g_tick.remo_was_playing) {
        if (st.remo_playing) {
            g_tick.remo_since = now;
            Log("remo playing (map %s)", MapName(g.map_id).c_str());
        } else {
            Log("remo ended after %.1f s; control back (%s)", now - g_tick.remo_since, Describe(g).c_str());
            if (g_tick.test_leave && g.session_role == RoleClient) {
                g_tick.test_leave = false;
                Log("test: live mirror over; the guest leaves the party (back to its own world)");
                party::runtime_leave();
            }
        }
        g_tick.remo_was_playing = st.remo_playing;
    }
    bool end_mirror;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        end_mirror = g_guest.TakeEndMirror();
    }
    if (end_mirror && st.remo_playing) {
        // The host's scene is over (its end or its skip): the mirror of it ends here too, as the
        // game's skip ends a scene (its end actions run); it counts as seen.
        bool skippable = false;
        const bool ok = EndRemoNow(&skippable);
        Log("the host's cutscene ended: the live mirror of remo %u ends here too after %.1f s (%s, skippable bit %d)",
            g_last_remo.load(), now - g_tick.remo_since, ok ? "ended" : "NOT possible", skippable ? 1 : 0);
    }
    // Test aid (BB_TEST_REMO_SKIP_AFTER=S, off by default): every remo on this game is skipped
    // after S s of it, the game's own skip / end path (a faster smoke: the scene played, its end
    // actions still run).
    static const double skip_after = [] {
        const char* v = std::getenv("BB_TEST_REMO_SKIP_AFTER");
        return v && *v ? std::atof(v) : 0.0;
    }();
    static double skip_tried_at = 0;
    if (skip_after > 0 && st.remo_playing && now - g_tick.remo_since >= skip_after &&
        now - skip_tried_at >= 1.0) {
        const bool first = skip_tried_at < g_tick.remo_since;
        skip_tried_at = now;
        bool skippable = false;
        const bool ok = EndRemoNow(&skippable);
        Log("test: remo %u skipped after %.0f s%s (%s, skippable bit %d)", g_last_remo.load(), skip_after,
            first ? "" : " (again: still playing)", ok ? "the game's skip / end" : "NOT possible", skippable ? 1 : 0);
    }
    HostPlayGuestStory(g, st);  // D10: a guest's cutscene, for everyone
    if (role == PartyRole::Guest) {
        GuestHeldSceneTick(g, st, now);  // a held own boss intro whose host copy did not come
    }
    if (g_tick.test_up && !g_tick.test_done && g.world_up && !g.loading && g.session_role == RoleClient &&
        !st.remo_playing) {
        if (!g_tick.test_coop_since) {
            g_tick.test_coop_since = now;
        } else if (now - g_tick.test_coop_since >= g_tick.test_delay) {
            g_tick.test_done = true;
            StoryAction a;  // what the guest's own 2002 play does on a phantom: the remo, here, now
            a.kind = StoryAction::PlayRemo;
            a.id = RemoForSex(g_tick.test_remo, SexVariant());
            a.mode = 2;
            a.why = "test: the guest's own cutscene";
            Run(a);
            StoryIntent up;
            up.kind = StoryKind::Cutscene;
            up.instr = 1;
            up.id = g_tick.test_remo;
            up.mode = 0;
            up.seq = NextSeq();
            std::lock_guard<std::mutex> lk(g_up_mu);
            g_up.push_back(up);
            g_self_played.emplace_back(up.id, Now());
        }
    }
    // Host test: as if an EMEVD 2002 play of our own world had been captured (the hook's intent),
    // once a cooperator has been in the world for the delay; the host plays it too.
    if (g_tick.test_host && !g_tick.test_done && role == PartyRole::Host && g.world_up && !g.loading && OwnWorld() &&
        !st.remo_playing) {
        if (g_tick.test_serve) {
            // serve:<remo>: the clock starts when a title joiner got the room (it is loading in).
            const double served = b2::TitleRoomServedAt();
            if (!g_tick.test_coop_since && served > 0 && now - served < 600.0) {
                g_tick.test_coop_since = now - (b2::NowSeconds() - served);
                Log("test: a title joiner got the room %.2f s ago; host remo in %.1f s", b2::NowSeconds() - served,
                    g_tick.test_delay);
            }
        } else if (!g_tick.test_coop_since && now - g_tick.test_coop_poll >= 1.0) {
            g_tick.test_coop_poll = now;
            const GameSnapshot full = ReadGameState(true);
            if (full.cooperators > 0) {
                g_tick.test_coop_since = now;
                Log("test: a cooperator is in the world; host remo in %.0f s", g_tick.test_delay);
            }
        }
        if (g_tick.test_coop_since && now - g_tick.test_coop_since >= g_tick.test_delay) {
            g_tick.test_done = true;
            StoryIntent s;
            s.kind = StoryKind::Cutscene;
            s.id = g_tick.test_remo;
            s.instr = 1;
            s.flags = StoryFlagsForRemo(s.id);
            s.seq = NextSeq();
            StoryAction a;
            a.kind = StoryAction::PlayRemo;
            a.id = RemoForSex(s.id, SexVariant());
            a.mode = 0;
            a.why = "test host play";
            Run(a);
            PushHost(s);
        }
    }
    // NG cycle (GameDataMan +0x68) once per world-up and on every change: the evidence for
    // ending replays (after the staff roll the next cycle loads with it).
    if (g.world_up && !g.loading && now - g_tick.world_since >= 2.0) {
        const u32 ng = Field<u32>(Slot(kGameDataMan), 0x68, 0xffffffffu);
        if (ng != 0xffffffffu && (ng != g_tick.last_ng || g_tick.ng_logged_for != g_tick.world_since)) {
            Log("NG cycle %u%s (map %s, %s)", ng,
                g_tick.last_ng == 0xffffffffu || ng == g_tick.last_ng ? " at world-up" : " (changed)",
                MapName(g.map_id).c_str(), Describe(g).c_str());
            g_tick.last_ng = ng;
            g_tick.ng_logged_for = g_tick.world_since;
        }
    }
    // The ending question: open it when no other dialog is up, then read its answer.
    if (g_tick.ask_ending && g.world_up && !g.loading && !st.remo_playing) {
        if (!g_tick.ask_dialog) {
            g_tick.ask_dialog = split::AskYesNo(BbGameMenu::Pages::EndingQuestionText());
            if (g_tick.ask_dialog) {
                Log("ending %d: question open (dialog %#llx)", g_tick.ask_ending,
                    static_cast<unsigned long long>(g_tick.ask_dialog));
            }
        } else {
            const int r = split::QuestionResult(g_tick.ask_dialog);
            // Test answer (BB_PARTY_ENDING_TEST=yes|no): YES = Cross; NO = Right, then Cross.
            static const char* test = std::getenv("BB_PARTY_ENDING_TEST");
            if (test && test[0] && r == -1 && now - g_tick.ask_since > 2.0 && now - g_tick.ask_test_at > 1.2 &&
                g_tick.ask_test_step < 6) {
                const bool yes = std::strcmp(test, "yes") == 0;
                runtime_pad_director_press(yes || g_tick.ask_test_step % 2 == 1 ? 0x4000u : 0x20u);
                g_tick.ask_test_at = now;
                ++g_tick.ask_test_step;
                Log("test: ending answer %s (step %d)", test, g_tick.ask_test_step);
            } else if (test && test[0] && g_tick.ask_test_step > 0 && now - g_tick.ask_test_at > 0.25) {
                runtime_pad_director_press(0);
            }
            if (r != -1 && r != 1 && r != 2) {
                // Closed without an answer (a load, the host's ending session drop): ask again.
                Log("ending %d: the question was closed without an answer (result %d): asking again",
                    g_tick.ask_ending, r);
                g_tick.ask_dialog = 0;
            } else if (r != -1) {
                const bool yes = r == 1;
                bool cords = false;
                progress::ReadFlagOwnSave(9900, &cords);
                const int type = GuestEndingType(g_tick.ask_ending, cords);
                {
                    std::lock_guard<std::mutex> lk(g_guest_mu);
                    g_guest.AnswerEnding(yes, type);
                }
                Log("ending %d: answer %s (result %d)%s", g_tick.ask_ending, yes ? "YES: play it now" : "NOT NOW", r,
                    yes ? (type != g_tick.ask_ending ? " - its own ending (three cords: Childhood's Beginning)"
                                                     : " - the host's ending")
                        : ": home to the own Dream, the save keeps its cycle");
                if (yes) {
                    Log("ending: this player's ending type %d (host %d, own 9900 %d)", type, g_tick.ask_ending,
                        cords ? 1 : 0);
                }
                g_tick.ask_ending = 0;
                g_tick.ask_dialog = 0;
            }
        }
    }
    if (g_tick.ask_ending && now - g_tick.ask_since > kEndingAskTimeout + 5.0) {
        g_tick.ask_ending = 0;  // the program took NOT NOW itself (no answer)
        g_tick.ask_dialog = 0;
    }
    // Test: an ending as if the host had sent it (once summoned, if asked).
    if (g_tick.test_on && g_tick.test_ending && !g_tick.test_done && g_tick.world_was_up &&
        now - g_tick.world_since >= g_tick.test_delay && !st.remo_playing &&
        (!g_tick.test_summoned || g.session_role == RoleClient)) {
        g_tick.test_done = true;
        StoryIntent s;
        s.seq = NextSeq();
        s.kind = StoryKind::Ending;
        s.id = static_cast<u32>(g_tick.test_ending);
        bool taken;
        {
            std::lock_guard<std::mutex> lk(g_guest_mu);
            taken = g_guest.Offer(s, g.session_role == RoleClient, now);
        }
        Log("test: %s %s (session %s, NG cycle %u)", DescribeStory(s).c_str(), taken ? "queued" : "refused",
            SessionRoleName(g.session_role), Field<u32>(Slot(kGameDataMan), 0x68, 0xffffffffu));
    }
    // Test: one forced replay (or mirror) item.
    if (g_tick.test_on && !g_tick.test_ending && !g_tick.test_host && !g_tick.test_done && g_tick.world_was_up &&
        now - g_tick.world_since >= g_tick.test_delay && !st.remo_playing && !busy) {
        g_tick.test_done = true;
        StoryIntent s;
        s.seq = NextSeq();
        s.kind = StoryKind::Cutscene;
        s.id = g_tick.test_remo;
        s.instr = 3;
        s.force = true;
        bool taken = true;
        if (g_tick.test_mirror) {
            // The mirror program needs session role 6 (a phantom); run its play call directly.
            StoryAction a;
            a.kind = StoryAction::PlayRemo;
            a.id = RemoForSex(s.id, st.sex_variant);
            a.mode = 2;
            a.mirror = true;
            a.why = "test mirror";
            Run(a);
        } else {
            std::lock_guard<std::mutex> lk(g_guest_mu);
            taken = g_guest.Offer(s, false, now);
        }
        Log("test: %s (%s; sex variant %d, tod value %d)", DescribeStory(s).c_str(), taken ? "queued" : "refused",
            st.sex_variant ? 1 : 0, st.tod_value);
    }
    std::optional<StoryAction> a;
    static bool prologue_held = false;
    const char* prologue_why = nullptr;
    if (PartyPrologueHold(&prologue_why)) {
        // Playtest 1 #19: no cutscene / story replay into a prologue world (G2:21127 replayed the
        // host's first Dream scene in the guest's world); the queue waits.
        if (!prologue_held) {
            prologue_held = true;
            Log("story replays held: %s (#19: no sync into a prologue world)", prologue_why);
        }
    } else {
        if (prologue_held) {
            prologue_held = false;
            Log("story replays: prologue over; the queue goes on");
        }
        std::lock_guard<std::mutex> lk(g_guest_mu);
        a = g_guest.Step(st, now);
        if (const u32 cut = g_guest.TakeMirrorCut()) {
            Log("live mirror of remo %u stopped after less than %.0f s (a load or travel cut it): not seen; the "
                "queued replay plays it once this game is steady", cut, GuestStory::kMirrorMinSeen);
        }
        u32 other_at = 0;
        if (const u32 other = g_guest.TakeMirrorOtherMap(&other_at)) {
            Log("live mirror of remo %u dropped: its map is not loaded here (this guest is in %s, %zu block(s) "
                "loaded, the host %.1f m away); no host cutscene over another map's scenery", other,
                MapName(other_at).c_str(), st.loaded_blocks.size(), double(st.host_dist));
        }
        if (const u32 near = g_guest.TakeMirrorNearby()) {
            Log("live mirror of remo %u plays here though this guest is in %s: the scene's map %s is %s, the host "
                "%.1f m away", near, MapName(st.map_id).c_str(), MapName(MapOfRemo(near)).c_str(),
                SceneBlockLoaded(st, near) ? "loaded" : "not seen loaded", double(st.host_dist));
        }
    }
    if (a) {
        Run(*a);
        if ((a->kind == StoryAction::SetFlags && a->on && a->flags.size() == 1 && a->flags[0] >= 21 &&
             a->flags[0] <= 23) ||
            (a->kind == StoryAction::SetFlags && a->on && a->flags == std::vector<u32>{72100130}) ||
            a->kind == StoryAction::ForceEnding) {
            g_tick.ending_at = now;
            g_tick.ending_log = now;
            g_last_ending_at = now;
        }
    }
    // After our ending: the ending step (staff roll movie) has no world; a progress line every
    // 10 s until the next cycle's world is up (also keeps the test harness from reaping it).
    if (g_tick.ending_at > 0) {
        if (g.world_up && !g.loading && now - g_tick.ending_at > 30.0 && now - g_tick.world_since >= 2.0 &&
            g_tick.world_since > g_tick.ending_at) {
            Log("ending: the next cycle's world is up %.0f s after the ending started (NG cycle %u, map %s)",
                now - g_tick.ending_at, Field<u32>(Slot(kGameDataMan), 0x68, 0xffffffffu), MapName(g.map_id).c_str());
            g_tick.ending_at = 0;
        } else if (now - g_tick.ending_log >= 10.0) {
            g_tick.ending_log = now;
            Log("ending: staff roll / ending step t=%.0f s (%s)", now - g_tick.ending_at, Describe(g).c_str());
        }
    }
    SaveSidecar(); // writes only when the queue / seen set changed
}

bool PopGuestStoryUp(StoryIntent* out) {
    std::lock_guard<std::mutex> lk(g_up_mu);
    if (g_up.empty()) {
        return false;
    }
    *out = g_up.front();
    g_up.pop_front();
    return true;
}

bool HostStoryFromGuest(int slot, const std::string& body, std::string* why) {
    if (!g_story_on.load() || Role() != PartyRole::Host) {
        if (why) {
            *why = "story off / not the host";
        }
        return false;
    }
    StoryIntent s;
    if (!StoryFromJsonText(body, &s, why) || !SanitizePeerStory(&s, why)) {
        return false;
    }
    if (s.kind != StoryKind::Cutscene || !(s.instr == 1 || s.instr == 3 || s.instr == 6 || s.instr == 7) ||
        !StoryRemoKnown(s.id)) {
        if (why) {
            *why = "not a plain cutscene play";
        }
        return false;
    }
    if (LocalPrologueHold()) {
        Log("host: story_up %u (event %u) from slot %d refused: the host is in its prologue (#19)", s.id, s.event_id,
            slot);
        if (why) {
            *why = "the host is in its prologue";
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_up_guard_mu);
        if (const char* no = g_up_guard.Refuse(s.id, BossBarShown(), Now())) {
            Log("host: story_up %u (event %u) from slot %d refused: %s", s.id, s.event_id, slot, no);
            if (why) {
                *why = no;
            }
            return false;
        }
    }
    std::lock_guard<std::mutex> lk(g_up_mu);
    for (const auto& [sl, q] : g_from_guests) {
        if (q.id == s.id) {
            return true;  // already queued (two guests in the same scene)
        }
    }
    if (g_from_guests.size() < 8) {
        g_from_guests.emplace_back(slot, s);
    }
    return true;
}

/// Host, main thread: a guest's cutscene, played here (mode 0) and sent to every guest.
void HostPlayGuestStory(const GameSnapshot& g, const StoryState& st) {
    if (Role() != PartyRole::Host || !g.world_up || g.loading || !OwnWorld() || st.remo_playing) {
        return;
    }
    std::pair<int, StoryIntent> item;
    {
        std::lock_guard<std::mutex> lk(g_up_mu);
        if (g_from_guests.empty()) {
            return;
        }
        item = g_from_guests.front();
        g_from_guests.pop_front();
    }
    StoryIntent s = item.second;
    static std::map<std::uint64_t, double> phase_held;  // G3: guest seq -> when the host got it
    {
        std::lock_guard<std::mutex> lk(g_up_guard_mu);
        if (const char* no = g_up_guard.Refuse(s.id, BossBarShown(), Now())) {
            if (s.instr == 6 && phase_held.erase(s.seq)) {
                Log("host: slot %d's world phase change %u (event %u): the host's own copy of the event played it "
                    "(the game sends the guest's action to the host's event)",
                    item.first, s.id, s.event_id);
            } else {
                Log("host: slot %d's cutscene %u (event %u) dropped: %s", item.first, s.id, s.event_id, no);
            }
            return;
        }
    }
    if (s.instr != 6) {
        static std::map<std::uint64_t, double> intro_held;  // guest seq -> when the host got it
        const bool intro = worldact::GuestFirstFightEnabled() && worldact::FirstFightSpotFor(s.event_id) != nullptr;
        auto it = intro_held.find(s.seq);
        if (intro && it == intro_held.end()) {
            it = intro_held.emplace(s.seq, Now()).first;
            Log("host: slot %d's cutscene %u is event %u's first-fight intro: waiting %.0f s for the host's own copy "
                "of the event (one play here)", item.first, s.id, s.event_id, kGuestIntroWait);
        }
        if (HoldGuestIntroForHostEvent(intro, intro ? Now() - it->second : 0.0)) {
            std::lock_guard<std::mutex> lk(g_up_mu);
            g_from_guests.push_front(item);
            return;
        }
        if (intro) {
            intro_held.erase(it);
            Log("host: slot %d's cutscene %u: the host's own event did not play it in %.0f s; played from the guest",
                item.first, s.id, kGuestIntroWait);
        }
    }
    if (s.instr == 6) {
        // G3: the game itself sends a guest's action-button press to the host's copy of the event
        // (live g3b: the host's own 12400750 ran its 2002[6], scene + warp + time of day). Wait for
        // it; play the guest's copy here only when the host's own event does not come.
        constexpr double kPhaseWait = 6.0;
        auto it = phase_held.find(s.seq);
        if (it == phase_held.end()) {
            it = phase_held.emplace(s.seq, Now()).first;
            Log("host: slot %d's world phase change %u (event %u): waiting %.0f s for the host's own event", item.first,
                s.id, s.event_id, kPhaseWait);
        }
        if (Now() - it->second < kPhaseWait) {
            std::lock_guard<std::mutex> lk(g_up_mu);
            g_from_guests.push_front(item);
            return;
        }
        phase_held.erase(it);
    }
    std::string phase_no;
    static const bool phase_on = !EnvOff("BB_PARTY_STORY_PHASE");
    if (s.instr == 6 && phase_on && g_warp_tod_ok && GuestPhaseChangeOk(s, FlagValue(9800, 3), g.map_id, &phase_no)) {
        // G3: the host's own copy of the guest's event, as 0x17C53B0 runs 2002[6]: the scene, the
        // warp to the same point of this map, the time of day at its end. The host's tod poll then
        // sends the time of day; the scene goes to the other guests below.
        static u32 packed = 0;
        packed = s.warp_map;
        const u32 remo = RemoForSex(s.id, SexVariant());
        const u64 ok = reinterpret_cast<PlayRemoWarpTodFn>(Guest(kPlayRemoWarpTod))(
            0, remo, 0, reinterpret_cast<u64>(&packed), s.warp_point, 0, 0, static_cast<u32>(g_player_arg),
            static_cast<u32>(s.tod));
        Log("host: slot %d changed the world's time of day: the host's own event did not come; remo %u with warp %u "
            "@%08x and tod %d played here (as 2002[6]) -> %s",
            item.first, remo, s.warp_point, s.warp_map, s.tod, (ok & 0xff) ? "queued" : "NOT queued (no remo player)");
        if ((ok & 0xff) && GuestPressedHostOnlyEvent(s.event_id, s.id)) {
            // D5 / D10 (pard4): the host's own copy still waits for the host's press (Amelia's
            // skull): its flag ends it at the next load and goes into every save (C2).
            SetFlags({s.event_id}, true);
            Log("host: slot %d pressed host-only event %u (its own copy ran): the scene and the time of day here, "
                "event flag %u set in the host's world (%d)", item.first, s.event_id, s.event_id,
                FlagValue(s.event_id, 1));
        }
        s.flags = StoryFlagsForRemo(s.id);
        s.seq = NextSeq();
        PushHost(s);
        return;
    }
    if (s.instr == 6) {
        Log("host: slot %d's 2002[6] %u (event %u): %s; the scene only", item.first, s.id, s.event_id,
            g_warp_tod_ok ? phase_no.c_str() : "0x131BC10 is not the 1.09 bytes");
    }
    s.instr = 1;
    s.tod = -1;
    s.warp_point = kStoryNone;
    s.warp_map = kStoryNone;
    s.flags = StoryFlagsForRemo(s.id);
    s.seq = NextSeq();
    StoryAction a;
    a.kind = StoryAction::PlayRemo;
    a.id = RemoForSex(s.id, SexVariant());
    a.mode = 0;
    a.why = "a guest's cutscene (D10)";
    Run(a);
    Log("host: slot %d triggered cutscene %u (event %u): played here and sent to every guest", item.first, s.id,
        s.event_id);
    PushHost(s);
}

void StoryMemberJoined() {
    g_member_joined.store(true);
}

#endif // BB_PARTY_STORY_NO_GAME
} // namespace coop
