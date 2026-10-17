// SPDX-License-Identifier: GPL-3.0-or-later
// The Chalice Dungeon channel board: FROM's /channel/* APIs answered by the party host
// (docs/party/chalice.md). A "channel" is one uploaded Chalice Dungeon: its FormData (the
// dungeon definition the game generates the layout from, base64), the chalice type
// (HolyGrailTypeId), depth (RitualLevel), fixed / root (FixedOrGeneral), rites (SubFeatureFlag),
// and the server-assigned ChannelId and 8-character glyph (DiscernmentWord). A member's game
// uploads its dungeon when it performs the ritual or enters it online; another member's game
// finds it with the glyph (word_search), the altar's search (search) or "join random"
// (random_join) and generates the same dungeon from the FormData. Summoned into a dungeon, a
// member's game gets the host's ChannelId with the host's summon message (P2P, 0x14ba340), fetches
// the entry with get_details_info and quick-joins it into its slot 0 (0x14bb200 -> 0x1ac3ef0 ->
// 0x1aa5280): it does not need the dungeon beforehand (docs/party/chalice.md 4).
//
// Party-only by construction (only party members reach the host). Formats (eboot 1.09, our
// offsets; docs/party/chalice.md 3):
//   upload        (12, builder 0x1e9af40)  -> ChannelId (int), DiscernmentWord (string)  [0x1e804be]
//   share         (13, 0x1e9c0a0)          -> ResKind only
//   search        (14, 0x1e9ee50)          -> ChannelList: [entry]                         [0x1e8054f]
//   word_search   (15, 0x1ea0420)          -> the root object IS one entry                 [0x1e81c1c]
//   get_details_info (16, 0x1e9d5b0)       -> ChannelList: [entry]
//   get_info      (17, 0x1e9cf60)          -> ChannelInfoList: [{ChannelId, Status, ShareLevel,
//                                             TurnoutLevel}] (ResKind 0x1040a: LostChannelIdList)
//   random_join   (18, 0x1e9dbf0)          -> the root object IS one entry
//   add_material  (19, 0x1e9c4d0)          -> CompleteFlag
//   notify_add_material_complete (20)      -> ResKind only
// An entry (reader 0x1e89da0): ChannelId, ShareLevel, Status, FormDataVersion (must be 0 for
// FormData to be read), FormData (base64), DiscernmentWord, RitualLevel, HolyGrailTypeId,
// FixedOrGeneral (0/1/2), SubFeatureFlag, CreateUserId, CreateCharaId, CreateDate, LastPlayDate
// ("YYYY-MM-DDTHH:MM:SS"), TurnoutLevel, WishMaterialList [{WishMaterialMainTypeId,
// WishMaterialSubTypeId, WishMaterialCount, SpendMaterialCount}].
//
// No game dependency (unit-tested in tests/test_party_host.cpp).
#pragma once

#include "json.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bbnet::party {

// Peer input limits (bbport security pass).
constexpr std::size_t kMaxFormDataB64 = 256u << 10;  // FormData text; a dungeon definition is a few KiB
constexpr std::size_t kMaxChannels = 64;              // on the board (oldest unused dropped)
constexpr std::size_t kMaxChannelsPerUser = 16;
constexpr std::size_t kMaxWishMaterials = 16;
constexpr std::size_t kMaxChannelQuery = 64;          // ids / criteria per request
// The refusal the game gets when a glyph / random join finds nothing, or an id is unknown
// (ResKind 0x0001040a "channel lost", the code get_info's LostChannelIdList goes with).
constexpr int kChannelLost = 0x1040a;

/// True for a packed map / AreaId (area << 24 | ...) inside the Chalice Dungeons (area 29).
inline bool ChaliceArea(long long area) {
    return area > 0 && ((static_cast<unsigned long long>(area) >> 24) & 0xff) == 29;
}

class ChannelBoard {
public:
    struct Channel {
        std::uint64_t id = 0;
        std::string glyph;          ///< DiscernmentWord
        std::string owner;          ///< online id of the member that uploaded it first
        std::uint64_t owner_uid = 0;
        long long chara_id = 0;
        long long holy_grail = 0;   ///< HolyGrailTypeId (the chalice)
        long long ritual_level = 0; ///< depth
        long long fixed_or_general = 0;
        long long sub_feature = 0;  ///< rites bit field
        long long status = 0;
        long long share_level = 0;
        long long turnout = 0;      ///< TurnoutLevel
        std::string form_data;      ///< base64, as uploaded
        json::Value wish_materials = json::Value::make_array();
        std::string created;        ///< "YYYY-MM-DDTHH:MM:SS"
        std::string last_play;
        std::uint64_t touched = 0;  ///< board clock of the last upload / join / sign seen in it
    };

    /// Answers one /channel/* call (path suffix after "/channel/"): the reply JSON text. `now` is
    /// "YYYY-MM-DDTHH:MM:SS". Unknown names -> {"ResKind":0}.
    std::string handle(const std::string& api, const std::string& caller, std::uint64_t caller_uid,
                       const json::Value& body, const std::string& now);

    /// A member's game is in channel `id` (its signs / sign queries carry it): random_join
    /// prefers the channels members currently play.
    void note_member_channel(const std::string& member, std::uint64_t id);
    /// The channel a member's game reported last (0: none).
    std::uint64_t member_channel(const std::string& member) const;

    /// The dungeon the newest ritual of a member other than `except` made or joined (its fixed
    /// search or its upload) within `within`: the party's dungeon a host can enter from its altar
    /// without typing the glyph. False when none.
    bool latest_ritual(const std::string& except, std::chrono::seconds within, Channel* out, std::string* member) const;

    std::vector<Channel> channels() const;
    /// The board has this dungeon (a summoned member's game can fetch it).
    bool known(std::uint64_t id) const;
    /// The host's own game: the FormData of channel `id` rebuilt from its slot record (a shared
    /// fixed dungeon joined through search was never uploaded). False when unknown / not base64.
    bool set_form_data(std::uint64_t id, const std::string& b64);
    /// The host's own game is in dungeon `id` the board does not know (entered from an earlier
    /// session's ritual: the board lives in the host's process): add it with its FormData, so
    /// summons into it work. False when known already or the FormData is unusable.
    bool adopt(std::uint64_t id, const std::string& b64, const std::string& owner);
    /// A member's game holds dungeon `id` in its current slot (its own ritual, this session or an
    /// earlier one): it counts as that member's latest ritual (latest_ritual).
    void note_made(const std::string& member, std::uint64_t id);
    /// The channel's FormData as the board has it ("" when unknown or none).
    std::string form_data(std::uint64_t id) const;
    /// The glyph for a FormData (deterministic: the same dungeon keeps its glyph and id).
    static std::string GlyphOf(const std::string& form_data);
    static std::uint64_t IdOf(const std::string& form_data);
    /// The key of a shared fixed dungeon (one channel per chalice, depth and rites).
    static std::string FixedKey(long long grail, long long level, long long rites);

private:
    std::string upload(const std::string& caller, std::uint64_t uid, const json::Value& body, const std::string& now);
    std::string share(const std::string& caller, const json::Value& body);
    std::string search(const std::string& caller, const json::Value& body);
    std::string word_search(const std::string& caller, const json::Value& body, const std::string& now);
    std::string details(const std::string& caller, const json::Value& body);
    std::string info(const json::Value& body);
    std::string random_join(const std::string& caller, const json::Value& body, const std::string& now);
    std::string entry(const Channel& c, bool root, long long share_level = -1) const;
    Channel* find_id(std::uint64_t id);
    void drop_excess();

    mutable std::mutex mu_;

    /// member -> (shared fixed channel it searched for its ritual, when): its ritual is in progress.

    std::map<std::string, std::pair<std::uint64_t, std::chrono::steady_clock::time_point>> ritual_;

    /// member -> (the dungeon of its last ritual: fixed search or upload, when).
    std::map<std::string, std::pair<std::uint64_t, std::chrono::steady_clock::time_point>> made_;
    static constexpr std::chrono::seconds kRitualWindow{120};
    std::vector<Channel> channels_;
    std::vector<std::pair<std::string, std::pair<std::uint64_t, std::uint64_t>>> member_channel_;  // member -> {id, clock}
    std::uint64_t clock_ = 0;
};

}  // namespace bbnet::party
