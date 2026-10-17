// SPDX-License-Identifier: GPL-3.0-or-later
// FromApi: the FROM game server's HTTP API, answered by the party host in-process
// (docs/PARTY_COOP_PLAN.md A3). The game reaches it at
//   https://<...>.scej-network.jp/.../ss.info   the server-status document (XML <ss>), which
//                                                sends the game's API calls to our gameurl
//   <gameurl>/basic_utils/{login,get_datetime,get_normal_notice,get_emergency_notice,
//                          get_user_agreement,sync_chara_id}
//   <gameurl>/summon_messenger/{create,get,delete,request}   (the party sign board)
//   <gameurl>/channel/*                                       (Chalice Dungeons, from_api_channel.h)
//   https://bb-playlog-{test,prod}.s3.amazonaws.com/...       (play logs: PUT, answered 200)
// and everything else under gameurl (messages, bloodstains, ghosts) gets an
// empty success. Bodies and answers whose exact format is not confirmed live in one table,
// from_api_formats.inc. BB_PARTY_TRACE=1 logs every request and answer.
//
// The sign board is party-only by construction (only party members reach the host): a
// guest's create puts its sign up, the host's get lists every other member's signs, and the
// host's request queues a guest_invite (party_transport.h) for the sign's owner with the
// host's room and addresses - what bbhost's np_matching2.cpp on_server_event reads.
// Chalice Dungeons (docs/party/chalice.md): a host whose sign query is in area 29 gets signs only
// while its ChannelId is on the channel board: the summoned game fetches that dungeon by id and
// quick-joins it, so nobody is summoned into a dungeon layout its own game cannot build.
#pragma once

#include "from_api_channel.h"
#include "json.h"
#include "party_host_service.h"

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace bbnet::party {

struct HttpRequest {
    std::string method = "GET";  // GET, POST, PUT, HEAD
    std::string url;
    std::vector<std::string> headers;  // "Name: value"
    std::string body;
};

struct HttpResponse {
    int status = 0;  // 0: not answered (the caller reports a network error)
    std::string content_type;
    std::string body;
};

// Peer input limits (bbport security pass): a guest's "http" call and its signs.
constexpr std::size_t kMaxUrl = 4096;
constexpr std::size_t kMaxHeaders = 64;
constexpr std::size_t kSummonDataSize = 0xE0;  // SummonData decodes to exactly this (A0 4.5)
constexpr std::size_t kMaxSignsPerUser = 8;

// SummonData content (docs/party/from_api_schema.md 5), checked before the host's board passes a
// member's sign to the other members' games (bbport security pass). nullptr when the blob is one
// the member's own game could have built for `online_id`, else why not:
//   0x40   sender online id: 1-16 of [A-Za-z0-9_-], NUL padded, == online_id (the receivers'
//          dedupe key: another member's id would replace or expire that member's sign)
//   0x58   AreaId: a map of the game (party_ids.h MapKnown), == the request's AreaId when given
//   0x5C   position x, y, z and 0x68 yaw: finite, |v| < 100000
//   0x70   MatchingLevel 1..544, == the request's MatchingLevel when given
//   0x76   sign type: one the game's session descriptor table (0x553d750, 0x22 rows) has a known
//          meaning for: 7 NormalCoop, 1/5/19 other friendly kinds, 2 ForceJoin, 8 NormalInvade,
//          0xA InvadeBounty
//   0x7A   OnlineID serialization length == 0x25, 0x7C == 2, handle 0x7D..0x8C == online_id,
//          terminator 0x8D == 0 (the guest a host's join goes to)
//   0xCC   NAT type 0..3 (sceNetCtl: unknown, type 1, 2, 3)
// `body` is the create request (AreaId, MatchingLevel); nullptr skips those cross-checks.
constexpr int kMaxMatchingLevel = 544;
const char* summon_data_problem(const std::uint8_t* data, std::size_t size, const std::string& online_id,
                                const json::Value* body = nullptr);

// The fixed host name of our gameurl (ss.info points the game at http://<it>:18671).
constexpr const char* kGameHost = "bbparty.invalid";

class FromApi {
public:
    explicit FromApi(PartyHostService& service);

    // True for the URLs FromApi answers (the FROM hosts, the play-log buckets, our gameurl).
    static bool routes(const std::string& url);
    // Answers one request from `caller` (the host's own online id for its game; a guest's for
    // a request that came over PartyLink).
    void handle(const Caller& caller, const HttpRequest& rq, HttpResponse& out);
    // The same as the "http" transport call's JSON (party_transport.h).
    void handle_json(const Caller& caller, const json::Value& rq, json::Value& reply);

    // The sign board (tests, status).
    struct Sign {
        std::uint64_t id = 0;
        std::uint64_t user_id = 0;
        std::string online_id;
        std::uint32_t area = 0;
        int region = 0;
        int summon_type = 0;
        std::string data_b64;
        long long chara_id = 0;
        long long version = 3;  // SummonDataVersion as sent
        long long channel = 0;  // ChannelId as sent (the Chalice Dungeon the sign is in)
        json::Value request;  // the create body as sent
        std::int64_t posted_ms = 0;  // steady clock when it was put up (A4: young signs are not shown)
    };
    // A4 (join_timing.md): with a bell sign timer below 2.0 s (0.5 s by default since 0.6.2; BB_PARTY_SIGN_DELAY) a
    // sign is shown to a host's poll only once it is (2.0 - timer) s old, so the sign path keeps its
    // proven ~2 s from the ring (0.5 s there let a host summon a guest whose bell was barely up).
    // 0 (the default): shown at once.
    static constexpr std::int64_t kMinSignAgeMs = 1500;  // the 0.5 s timer's hold (tests)
    void set_min_sign_age_ms(std::int64_t ms);  // unit tests (0: shown at once)
    // Signs on the board that are old enough to be shown (a change lets the host poll at once).
    std::size_t shown_signs();
    std::vector<Sign> signs() const;
    // A member's link went away (crash, timeout, leave): its signs leave the board, so no host
    // summons a game that is not there (returns how many). A restarted game puts up a new one.
    std::size_t drop_signs(const std::string& online_id);
    // The host's game gave up a summon of this member before it joined (it deactivated the
    // signaling connection to a peer that never became a room member). Its signs still on the
    // board get new ids, so the host's game sees a new sign at its next poll and summons again
    // at once instead of waiting for the member's 30 s sign resend (a failed LAN summon cost
    // ~30 s each, docs/party/join_timing.md). Returns how many.
    std::size_t renew_signs(const std::string& online_id);
    // A member that was in (or joining) the host's world lost its link: the host's game keeps its
    // transport connection to that address until the transport's own timeout (25 s), and summoning
    // the restarted game over it fails (the guest waits for summon data that goes to the dead
    // connection). Its new signs are not offered to anyone for `ms` (docs/party/join_timing.md).
    void hold_signs(const std::string& online_id, int ms);
    // Playtest 1 #19: the members (online ids) still in their prologue. Their signs are not shown
    // and a member in its prologue is shown none: no summon touches a prologue world (the party
    // director sets this on the host every tick; a manual bell in the prologue finds nothing).
    void set_prologue_members(const std::set<std::string>& ids);
    // The longest sign hold still running, ms (0: none). The title join waits it out too.
    std::int64_t hold_left_ms();
    // How long ago the host's game asked to summon a sign (summon_messenger/request), ms.
    std::int64_t ms_since_request();
    std::uint64_t user_id_of(const std::string& online_id);
    /// The Chalice Dungeon channel board (tests, the party layer).
    ChannelBoard& channels() { return channels_; }

    // The process's (the host's), over PartyHostService::instance().
    static FromApi& instance();

private:
    std::string dispatch(const Caller& caller, const std::string& path, const json::Value& body, int* status,
                         std::string* content_type);
    std::string sign_create(const Caller& caller, const json::Value& body);
    std::string sign_get(const Caller& caller, const json::Value& body);
    std::string sign_delete(const Caller& caller, const json::Value& body);
    std::string sign_request(const Caller& caller, const json::Value& body);

    PartyHostService& service_;
    ChannelBoard channels_;
    mutable std::mutex mu_;
    std::int64_t min_sign_age_ms_ = 0;
    std::vector<Sign> signs_;
    std::vector<std::pair<std::string, std::int64_t>> holds_;
    std::set<std::string> prologue_;  // under mu_  // online id -> steady ms until
    std::uint64_t next_sign_ = 1;
    std::int64_t last_request_ms_ = -(1LL << 40);
    std::vector<std::pair<std::string, std::uint64_t>> users_;  // online id -> UserId
    std::uint64_t next_user_ = 1001;
};

// The ss.info document as FromApi serves it (tests).
std::string from_api_ss_info();

}  // namespace bbnet::party
