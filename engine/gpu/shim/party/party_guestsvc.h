// SPDX-License-Identifier: GPL-3.0-or-later
// Guest services: a summoned party guest uses the Doll (level up), the Bath Messengers' shops
// (echoes and Insight) and the workshop tools (weapon upgrade, Blood Gem workbench, storage) in
// the host's Hunter's Dream, on its own character (docs/party/guest_services.md).
//
// The gate (static RE, docs/party/guest_services.md 2): every Hunter's Dream talk script that opens
// one of those menus (t210140 Doll, t21069x workshop / messenger menus, ...) parks in its
// "クライアント待機" (client wait) state machine while the talk test function 19
// `IsClientPlayer()` is 1. That function is case 19 of the talk environment's test-function switch
// 0x203AD60 (ids 0..62, `cmp eax,0x3e` at 0x203AD97): `movzx eax, byte [r15+0x25]` at 0x203BA78,
// then `jmp 0x203EECF` (result type 2 = int, value eax). The override returns 0 there while the
// gate is open; the scripts then run their normal (host) branch on the guest's own game. Menus
// act on the local PlayerGameData (level, echoes, Insight, inventory, storage) - the guest's own
// character, written to the guest's own save by the game's save writer; nothing goes to the host.
// Talk-script flags (SetFlagState) on a summoned guest go to the load-mode-1 overlay (discarded),
// except 6000-8999 (personal: Doll / messenger / shop states), which stay in the guest's own pool.
//
// Gate open = BB_PARTY_GUEST_SERVICES on (default in party mode) and the local player is a client
// (session role 6) in the world (not loading) on map m21 (Hunter's Dream). Everywhere else the
// game's own answer stands (quest NPCs in the host's world keep refusing guests).
//
// Lamps and headstones (docs/party/guest_services.md 3): one party travel, led by the host.
//   - World lamps: the lamp object's multiplayer byte +0x49 (0x12F8323, set from the session's
//     member counts) is forced to 0 in party mode (0x12F831C: test/setg/or -> xor ecx, ecx), for
//     host and guests: the host's own lamp works with guests present, and a guest's lamp runs
//     common event 7200 (its `1003[6] End if Client` at index 0 is skipped: rule 7200:1003:6@0).
//   - Dream headstones (t210000-t210003, t210721): IsMultiplayerInProgress() (talk test 52, case
//     0x203C505; filtered at 0x203C564) answers 0 for them in party mode (host and guests), and
//     IsClientPlayer() 0 for a guest.
//   - A guest's resulting warp 0x13CDF30(id) is consumed (party_travel's interceptor) and sent to
//     the host as EVENT "lamp_request" {id}; the host's director checks it (a ReturnPointParam row,
//     host in the world and not loading, one per 10 s) and runs the same warp itself: the B1 travel
//     then brings every guest along (the host's own warp: no loop; guests' replays are ours).
//
// Env: BB_PARTY_GUEST_SERVICES=0 turns it off (vanilla: guests cannot use them in the host's world);
// BB_PARTY_GUEST_LAMPS=1 turns the lamp / headstone part on (opt-in until it has run live; it
// also lifts the lamp gate for the host).
//
// The pure part has no game dependency and is unit-tested (tests/test_party_guestsvc.cpp, built
// with BB_PARTY_GUESTSVC_NO_GAME).
#pragma once

#include "party_phantom.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop {

constexpr int kSessionRoleClient = 6;

/// m21_xx_xx_xx (Hunter's Dream, both versions). `map_id` = 0xAABBCCDD.
bool IsDreamMap(std::uint32_t map_id);

struct GuestServicesInput {
    bool enabled = false;   ///< BB_PARTY_GUEST_SERVICES (and party mode)
    bool world_up = false;  ///< WorldChrMan + local player + session manager
    bool loading = false;   ///< a loading screen is up
    int session_role = -1;  ///< SprjSessionManager +0x124
    std::uint32_t map_id = 0xffffffffu;
};
/// True: the talk test `IsClientPlayer()` answers 0 on this game.
bool GuestServicesGate(const GuestServicesInput& in);

/// The Hunter's Dream talk scripts a guest may use (the talk environment's +0x10 is the script id;
/// MSB m21_00_00_00 entity in brackets): 210304 Doll (2100700: level up), 210693 storage (2100200),
/// 210692 workbench (2100201: upgrade, Blood Gems, repair, crush), 210696 mirror (2100202),
/// 210699 Memory Altar (2100203), 210691 / 210694 / 210695 Bath Messengers (shops, sell), 210697 dress-up messenger,
/// 210698 Insight messenger. Not: Gehrman 210305 (only outside his last choice: GehrmanLastEvent), 210306, headstones
/// 210000-210003, the Dream lamp / warp menus 210720 / 210721 (travel: the lamp phase), chalice
/// altars 210050 / 210100 / 210110.
bool GuestServiceTalk(std::uint32_t talk);
/// Empty `extra` = the built-in list only; "all" = any talk script (diagnostics); else a comma
/// list of more talk script ids (BB_PARTY_GUEST_SERVICES_TALK).
bool GuestServiceTalkAllowed(std::uint32_t talk, const std::string& extra);

/// Gehrman in his wheelchair (MSB 2100600, talk t210305). Its StateMachine 1 runs the talk only
/// while IsClientPlayer() is 0 (else its "client wait" machine), so a guest could not talk to him
/// at all. Its machine 0x7FFFFFF7 picks the last event (the Submit / Refuse choice: 72100130 /
/// 72100131, also its "attacked" branch) when flag 1028 is on or 1029 is on and 1030 off (m21
/// events 12100143 / 12100144 / 12100145); else the ordinary lines (72100120-72100129,
/// 72100133-72100135). A guest talks to him unless the last event is up.
constexpr std::uint32_t kGehrmanTalk = 210305;
bool GehrmanLastEvent(bool f1028, bool f1029, bool f1030);

/// Dream headstones and warp menus whose multiplayer / client tests the lamp part lifts:
/// 210000-210003 (headstones), 210721 (warp menu with StartWarpMenuInit).
bool HeadstoneTalk(std::uint32_t talk);
/// Lamp talk scripts: tAAB000-tAAB003 of a map (m24_01 -> t241000..t241003; the Dream's are its
/// headstones t210000..t210003). Their talk tests IsMultiplayerInProgress / IsClientPlayer keep a
/// lamp shut while a session is up; the party lifts them (host: party mode, guest: BB_PARTY_GUEST_LAMPS).
bool LampTalk(std::uint32_t talk);
/// The Chalice Dungeons' lamp scripts t290110..t290119 (lamps 2902950..2902959: their "Return to the
/// Hunter's Dream" sets 72900100 + n, m29's event 12907020 warps to the altar spot 2102954 + slot).
/// Their IsMultiplayerInProgress / IsClientPlayer tests shut every dungeon lamp while anyone is
/// summoned (a tester's report: lamps could be lit, nothing else); the party lifts them as for the
/// world lamps, always in party mode (BB_PARTY_CHALICE_LAMPS=0: test / diagnostic escape hatch only).
bool ChaliceLampTalk(std::uint32_t talk);

// ---- Lamp requests (guest -> host) ----
constexpr const char* kLampRequestEventName = "lamp_request";
/// A guest lit a lamp of the host's world: the flags its game set in the host-world overlay, for
/// the host to set in its own world (the mirror brings them back). Never the guest's own save.
constexpr const char* kLampLitEventName = "lamp_lit";
constexpr std::size_t kMaxLampLitIds = 16;
/// Ids a guest may ask the host to set for a lamp it lit: the lamp's own pair base+10 / base+11 of
/// its object flag base 1AAB78x0 (T1 zone 7, x800..x899, id % 20 = 10 or 11) and the lamp-lit
/// flags 7AAB02xx (T7 zone 0, 200..299).
bool LampLitFlagAllowed(std::uint32_t id);
/// `own`: lamps lit in the guest's OWN save (statsaudit finding 5, shared lamps: the host applies
/// them only while its "Shared lamps" rule is on); otherwise a lamp the guest lit in the host's world.
std::string LampLitToJsonText(const std::vector<std::uint32_t>& ids, bool own = false);
bool LampLitFromJsonText(const std::string& text, std::vector<std::uint32_t>* ids, std::string* error,
                         bool* own = nullptr);
struct LampRequest {
    std::uint32_t id = kTravelNone; ///< the ReturnPointParam id the guest's lamp / headstone warp asked for
    std::uint64_t seq = 0;          ///< guest counter (diagnostics)
    /// The guest rested at lamp `id` and travels ALONE (split travel NO): the host does not move,
    /// it runs a rest load where it stands (the world resets for the party; playtest 1 bug 3).
    bool rest = false;
};
std::string LampRequestToJsonText(const LampRequest& r);
bool LampRequestFromJsonText(const std::string& text, LampRequest* out, std::string* error = nullptr);
/// A guest's request before the host acts on it: the id must be a ReturnPointParam row.
bool SanitizePeerLampRequest(const LampRequest& r, std::string* why = nullptr);

/// Host: what a guest's lamp request does (pure; unit-tested). A rest: the host's rest load. A
/// lamp of the loaded map: nothing (the guest loads alone; the host never moves for it, bug 10).
/// Otherwise the party travels there with the host.
enum class LampRequestAction { RestLoad, IgnoreSameMap, PartyTravel };
LampRequestAction ChooseLampRequestAction(const LampRequest& r, bool lamp_in_loaded_map);
/// Host: at most one accepted request per kMinInterval s, and only with the host ready (in the
/// world, not loading).
class LampRequestGate {
public:
    static constexpr double kMinInterval = 10.0;
    bool Accept(bool host_ready, double now, std::string* why = nullptr);

private:
    double last_ = -1e9;
};

/// The built-in EMEVD rules of the lamp part: 7200:1003:6@0 (world lamp -> Dream: the client end).
std::vector<EmevdSkipRule> LampSkipRules();
/// chalicelamp (a guest's report: "guests in chalice dungeons can't use lamps"): a dungeon lamp's
/// "Return to the Hunter's Dream" sets 72900100 + n on the guest too, but m29's event 12907020 starts
/// with 1003[6] "end the event on a client" (state 1), exactly as the world lamps' 7200, so the guest's
/// warp never came (the report: the flag set three times at lamp 2902954, nothing else). 12907020:1003:6@0
/// skips that end, so its 2003[49] warp runs and becomes the guest's lamp travel (split question / the
/// party's travel), as a world lamp's does. Applied with the guest lamps and the dungeon lamps on.
std::vector<EmevdSkipRule> ChaliceLampSkipRules();

/// The Doll stands where the host's Doll stands (client-only rules): m21_00 event 12100110
/// (人形のヒロイン_立ち位置制御, every Dream load, no client guard) clears and re-rolls the stand-spot
/// flags 12100500-12100509 (index 25 `2003[22]`, index 26 `2003[17]`) unless flag 100 is on, then
/// short-warps the Doll (2004[41]) to spot 2102301 / 2102302 (workshop) / 2102303 (lower garden).
/// A guest rolls its own spot, so its Doll can stand somewhere else than the host's. Skipping the
/// two instructions on a client keeps the host's flags from the join snapshot (map m21 zone 0).
std::vector<EmevdSkipRule> DollPlacementRules();

/// The Doll's level-up handshake (live + static finding): talk t210304 State_10 sets 72100100 and
/// waits for 72100101 AND 72100102 before OpenSoul (State_7). m21_00 event 12100112
/// (人形のヒロイン_レベルアップアニメ_PC, the player's kneel) sets 72100101 and 12100113
/// (人形のヒロイン_レベルアップアニメ_人形, the Doll's gesture) sets 72100102; 12100114 is the
/// level-up SFX (flag 7500). All three end at once unless the local player's chr type is 0
/// (index 1-2 `4[3] [15, 10000, 0]` + `1000[2]` end; 12100112 repeats the test per distance branch). On a client (chr type 1) the
/// rewriter makes those tests ask for the local player's own type (args byte 8: 0 -> 1) while it
/// executes them, and puts 0 back when the same bytes run on a non-client (the EVD data is shared).
/// True when an instruction (event, bank, id, entity, type) is one of those tests.
bool DollLevelUpCharTypeTest(int event_id, int bank, int id, int entity, int type); // 12100112-114

/// The lower bath's shop messengers' rise / sink animation event (m21_00 12105040: messengers
/// 2100211-2100213, waits for talk flag 12105041 ON, then OFF, then starts again). The flag is per
/// player (the talk script of whoever opens the shop sets it in its own game; nothing clears it
/// until a load) but the event's state is network-synced (packet 0x31): each game passed the
/// other's wait, so while one player used the shop the event ping-ponged on every game in the
/// session (rise, sink, rise ... every 2 s, with their sounds; live bathsync). Its 0x31 packets
/// are dropped: each game animates them from its own player's talk. The Insight messenger's
/// 12105043 is not one: it ends on a client (1003[101]) and the host's copy drives every game's
/// (one rise when the host opens the shop, one sink when it closes; its talk clears 12105044).
/// 0.7.3: also the gift messengers' events 12101020-12101028 (starting weapons, Notebook, bells, Eye):
/// another game's end of its copy ended this game's (the host's messengers sank on a guest's pick).
bool PerPlayerMessengerEvent(std::uint32_t event_id);

/// The local character's record (PlayerGameData +0x90 level, +0x94 echoes, +0x84 Insight).
struct PlayerRecord {
    int level = -1, echoes = -1, insight = -1;
    bool operator==(const PlayerRecord& o) const {
        return level == o.level && echoes == o.echoes && insight == o.insight;
    }
    bool operator!=(const PlayerRecord& o) const { return !(*this == o); }
};
/// "level 10 -> 11, echoes 5000 -> 3200" (the changed fields only; "" when equal or unknown).
std::string DescribeRecordChange(const PlayerRecord& before, const PlayerRecord& after);

/// "BB_PARTY_GUEST_LAMPS" value: "0" off, "1" on, anything else -> `fallback` (the Party page
/// setting party_guest_lamps, on by default).
bool GuestLampsEnabledFromEnv(const char* value, bool fallback = true);
/// "BB_PARTY_GUEST_SERVICES" value -> on/off (unset / "" / anything but "0" = on).
bool GuestServicesEnabledFromEnv(const char* value);

#ifndef BB_PARTY_GUESTSVC_NO_GAME
struct GameSnapshot;
}
namespace party {
class PartyLink;
}
namespace coop {
/// PartyInit: installs the filters and patches (party mode and enabled only; byte-verified).
void GuestServicesInit();
/// seamless_rules (EMEVD filter set-up): the Doll placement rules and the lamp part's rules
/// (party mode, guest services on; the lamp rule only with lamps on).
std::vector<EmevdSkipRule> GuestServicesSkipRules();
/// Director tick (main thread): opens / closes the gate, logs the guest's record changes; guest:
/// sends lamp requests; host: runs accepted ones.
void GuestServicesTick(const GameSnapshot& s, party::PartyLink* link);
/// Host: EVENT "lamp_request" from guest `slot` (PartyLink callback thread). False + why: refused.
bool GuestServicesOnLampRequest(int slot, const std::string& body, std::string* why);
/// Guest: a lamp / headstone warp `id` becomes a request to the host (the party travels together;
/// split travel's "with the party"). False: lamps off or not a guest.
bool GuestLampRequestNow(std::uint32_t id);
/// Guest: it rested at lamp `id` and travels alone: asks the host for a rest load of its world.
bool GuestRestRequestNow(std::uint32_t id);
/// A guest lit a lamp in the host's world (EVENT lamp_lit, host side): queued for the host's tick.
bool GuestServicesOnLampLit(int slot, const std::string& body, std::string* why);
/// Open right now (diagnostics, the Party tab).
bool GuestServicesOpen();
/// The ChrIns of a map character by its talk script / entity id (WorldChrMan chr sets), 0 if none
/// (main thread; live test fixtures).
std::uint64_t GuestSvcFindChr(std::uint32_t id);
/// An action-button prompt text was looked up in the last 400 ms (*id: its text id).
bool GuestSvcPromptUp(std::uint32_t* id);
/// Tests: the action prompt `id` is on screen and no other action-button text (`*other`: the first
/// other one seen, e.g. a floor message's "View Notes" next to Gehrman's "Talk").
bool GuestSvcOnlyPrompt(std::uint32_t id, std::uint32_t* other);
#endif

} // namespace coop
