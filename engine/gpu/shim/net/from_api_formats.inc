// SPDX-License-Identifier: GPL-3.0-or-later
// FromApi wire formats - THE ONE PLACE TO CORRECT THEM (from_api.cpp includes this file).
//
// Status of each entry: CONFIRMED = read from the 1.09 eboot (offsets are ours, raw ELF VA) or
// from what droogie/bbhost's test client exchanges with The Hunter's Dream server
// (tools/nettest/simclient.py @8f2746c); GUESS = not confirmed, best effort. When
// docs/party/from_api_schema.md (A0) pins a format down, change it here and flip the status.
//
// ---------------------------------------------------------------------------------------------
// ss.info (CONFIRMED structure, eboot parser 0x1eb65e0 called from 0x1e89a21):
//   - the document is read whole (0x400-byte reads appended) and scanned for "<ss>"; the text
//     between <ss> and </ss> must be a base-10 number (stored at out+8, default 999) - GUESS:
//     0 means "service up";
//   - every other element name ends in a number N = *(u32*)(FrpgNetMan + 0x9e8) (FrpgNetMan
//     slot at our 0x553b120), e.g. <SummonDataCreateInterval7>30</SummonDataCreateInterval7>;
//     an optional <msgN><langM>text</langM></msgN> (M from the language table 0x4736570
//     indexed by *(u32*)0x55992f0);
//   - <gameurlN> must hold all 37 <api_X>...</api_X> elements (0x1ecb390, one call per API in
//     this order; a missing one fails the parse); each value is the base the game formats its
//     request URL from ("%s/basic_utils/login", "%s/summon_messenger/create", ...).
//   N is read from the game at answer time; when it cannot be read (no image, FrpgNetMan not
//   up) every index in kSsInfoFallbackIndices is emitted.
// ---------------------------------------------------------------------------------------------

namespace formats {

// The <ss> status value (CONFIRMED, A0: anything but 0 -> offline message 0x1131/0x1132).
constexpr const char* kSsStatus = "0";

// N when FrpgNetMan+0x9e8 cannot be read (CONFIRMED-safe, A0: real N = 0x4736550[region], 1..4).
constexpr unsigned kSsInfoFallbackIndices[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

// Where N lives (CONFIRMED: 0x1e899b3..0x1e899e6).
constexpr std::uint64_t kFrpgNetManSlot = 0x553b120;
constexpr std::uint32_t kFrpgNetManServerIndexOff = 0x9e8;

// The 37 API elements inside <gameurlN>, in the parser's order (CONFIRMED: string table order,
// 37 = the parser's bound 0x25).
constexpr const char* kApiNames[] = {
    "api_Login", "api_ServerTimeGet", "api_SyncCharaId", "api_NoticeNormalGet", "api_NoticeEmergencyGet",
    "api_UserAgreementGet", "api_BloodMessCreate", "api_BloodMessGetList", "api_BloodMessEvaluate",
    "api_BloodMessGetEvaluate", "api_BloodMessRemove", "api_BloodMessSearchAdd", "api_ChannelUpload",
    "api_ChannelShare", "api_ChannelSearch", "api_ChannelWordSearch", "api_ChannelGetDetailsInfo",
    "api_ChannelGetInfo", "api_ChannelRandomJoin", "api_ChannelAddMaterial", "api_ChannelAddMaterialCompleteNotify",
    "api_MessengerShellUpload", "api_MultiPlayNetError", "api_UserPropertiesMoveCount",
    "api_UserPropertiesMoveCountCheck", "api_SummonDataCreate", "api_SummonDataGetList", "api_SummonDataRemove",
    "api_SummonDataSummon", "api_ChairMessGetList", "api_ChairMessRespawnPointNotice", "api_TombMessCreate",
    "api_TombMessGetList", "api_DeathVisionGet", "api_TombMessRemove", "api_WanderingGhostCreate",
    "api_WanderingGhostGet",
};
static_assert(sizeof(kApiNames) / sizeof(kApiNames[0]) == 37, "the eboot's parser reads 37 APIs");

// The base every API gets (CONFIRMED shape, A0: scheme://host[:port], no trailing slash; the host is
// ours, http_hle answers it in-process or forwards it to the party host).
constexpr const char* kApiBase = "http://bbparty.invalid:18671";

// Top-level numeric elements, emitted as <NameN>value</NameN> (names CONFIRMED from the
// eboot's format strings; values GUESS - vanilla-ish, with the party's wider level range and
// quick sign refreshes).
struct SsValue {
    const char* name;
    const char* value;
};
constexpr SsValue kSsValues[] = {
    {"PlayLog", "0"},  // GUESS: 0 = no play-log upload
    {"PlaylogServerURL", ""},
    {"MessengerShellUploadInterval", "600"},
    {"NoticeEmergencyGetInterval", "3600"},
    {"ChannelGetInfoInterval", "600"},
    {"ChairMessGetListInterval", "60"},
    {"ChairMessRespawnPointNoticeWaitTime", "60"},
    {"ChairMessRespawnPointNoticeInterval", "60"},
    {"WanderingGhostGetMaxCount", "0"},
    {"WanderingGhostGetCountPerArea", "0"},
    {"WanderingGhostGetInterval", "600"},
    {"WanderingGhostCreateInterval", "600"},
    {"BloodMessGetEvaluateInterval", "600"},
    {"BloodMessGetListGetMaxCount", "0"},
    {"BloodMessGetListGetCountPerArea", "0"},
    {"BloodMessGetListInterval", "600"},
    {"TombMessGetEvaluateInterval", "600"},
    {"TombMessGetListGetMaxCount", "0"},
    {"TombMessGetListGetCountPerArea", "0"},
    {"TombMessGetListInterval", "600"},
    // Party: any level. The window is LowerAbs + L*(LowerRel/100+1) .. UpperAbs + L*(UpperRel/100+1)
    // (CONFIRMED, A0 sec. 2.4); positive Lower* values make it empty.
    {"SummonDataCoopMatchingLevelLowerRel", "0"},
    {"SummonDataCoopMatchingLevelLowerAbs", "-1000"},
    {"SummonDataCoopMatchingLevelUpperRel", "0"},
    {"SummonDataCoopMatchingLevelUpperAbs", "1000"},
    {"SummonDataGetListInvationGuardTimerObserveTimeRate", "0"},
    {"SummonDataGetListInvationGuardTimerLimitationTimeRate", "0"},
    {"SummonDataGetListCoopGuardTimerObserveTimeRate", "0"},
    {"SummonDataGetListCoopGuardTimerLimitationTimeRate", "0"},
    {"SummonDataGetListInvationGuardTimerClampTime", "0"},
    {"SummonDataGetListCoopGuardTimerClampTime", "0"},
    {"SummonDataBloodMadHunterPercent", "0"},
    {"SummonDataCreateNaturalEnemyPercentFixed", "0"},
    {"SummonDataGetListGetMaxCount", "8"},
    {"SummonDataGetListGetCountPerSummonType", "8"},
    {"SummonDataGetListAllAreaMethodWaitCount", "0"},
    {"SummonDataGetListNaturalEnemyWaitCountFixed", "0"},
    {"SummonDataGetListCombinationInvasionWaitCount", "0"},
    {"SummonDataGetListNomalCoopWaitCount", "0"},
    {"SummonDataGetListGuardTimer", "0"},
    {"SummonDataGetListInterval", "5"},
    {"SummonDataCreateInterval", "5"},
    {"ReloadServerStatusInfoInterval", "600"},
};

// BB_PARTY_FAST_JOIN (default on; docs/party/join_timing.md): these replace kSsValues. The get-list
// builder 0x1e946e0 waits [client+0x100] = SummonDataGetListInterval whole seconds between two
// summon_messenger/get polls of a ringing host (CONFIRMED); 0 would poll every frame (no in-flight
// check), so 1. SummonDataCreateInterval is copied to SummonStepManager+0x190 (0x1e89aad) but no
// reader was found; 1 for symmetry.
constexpr SsValue kSsFastJoinValues[] = {
    {"SummonDataGetListInterval", "1"},
    {"SummonDataCreateInterval", "1"},
};

// ---------------------------------------------------------------------------------------------
// The API answers. Paths are matched as suffixes of the request path. Bodies are templates:
// ${Name} is replaced by a JSON value FromApi computes (SessionId, UserId, Now, NowText,
// CharaId, SummonDataId, SummonDataList, Result). Request bodies are parsed as JSON when they
// are JSON (bbhost's simclient sends {"MessageId": ..., ...}); otherwise they stay opaque and
// the answer is the template as is.
//
// CONFIRMED (simclient.py): login answers SessionId and UserId; summon_messenger/get answers
// SummonDataList whose entries carry UserId; requests carry MessageId, SessionId, UserId,
// AreaId, AreaRegionId, SummonType, SummonData (base64 of 0xE0 bytes), SummonDataVersion,
// TargetUserId, and ?user_id=<UserId> on summon_messenger calls.
// GUESS: everything else below (MessageId names of answers, the other keys, empty lists).
// ---------------------------------------------------------------------------------------------
struct Reply {
    const char* path;          // suffix of the request path
    int status;                // HTTP status
    const char* content_type;  //
    const char* body;          // template
    bool confirmed;
};
constexpr Reply kReplies[] = {
    // CONFIRMED (A0 4.1, reader 0x1e7c1d0): SessionId non-empty string, UserId integer;
    // success iff ResKind == 0. MessageId/ServerTime are not read.
    {"/basic_utils/login", 200, "application/json",
     R"({"ResKind":0,"SessionId":${SessionId},"UserId":${UserId},"UserStatus":0,"LanguageId":1,"ServerVersion":109})",
     true},
    // Never requested in 1.09 (A0 3.3).
    {"/basic_utils/get_datetime", 200, "application/json", R"({"ResKind":0})", true},
    {"/basic_utils/get_normal_notice", 200, "application/json", R"({"ResKind":0,"NoticeList":[]})", true},
    {"/basic_utils/get_emergency_notice", 200, "application/json",
     R"({"ResKind":0,"NoticeList":[],"CheckTime":${NowText}})", true},
    {"/basic_utils/get_user_agreement", 200, "application/json",
     R"({"ResKind":0,"UserAgreementText":"","UserAgreementID":0})", false},
    // CONFIRMED (A0 4.2): the array is required; its first PublishCharaId becomes the CharaId.
    {"/basic_utils/sync_chara_id", 200, "application/json",
     R"({"ResKind":0,"PublishCharacterIdList":[{"PublishCharaId":${CharaId}}]})", true},
    // CONFIRMED (A0 4.5): only the code is read; SummonDataId is ours (tests, logs).
    {"/summon_messenger/create", 200, "application/json", R"({"ResKind":0,"SummonDataId":${SummonDataId}})", true},
    {"/summon_messenger/get", 200, "application/json", R"({"ResKind":0,"SummonDataList":${SummonDataList}})",
     true},
    {"/summon_messenger/delete", 200, "application/json", R"({"ResKind":0})", true},
    {"/summon_messenger/request", 200, "application/json", R"({"ResKind":0,"Result":${Result}})", true},
    // List APIs: the list key must be an array or the answer is invalid (A0 4.4, CONFIRMED).
    {"/blood_messenger/create", 200, "application/json", R"({"ResKind":0,"BloodMessIdList":[]})", true},
    {"/blood_messenger/message_area", 200, "application/json", R"({"ResKind":0,"BloodMessList":[]})", true},
    {"/blood_messenger/evaluation", 200, "application/json", R"({"ResKind":0,"BloodMessEvaluationList":[]})", true},
    {"/blood_messenger/exist_messages", 200, "application/json",
     R"({"ResKind":0,"BloodMessEvaluationList":[],"LostBloodMessIdList":[]})", true},
    {"/chair_messenger/get", 200, "application/json", R"({"ResKind":0,"ChairMessList":[],"ChairMessCount":0})", true},
    {"/tomb_messenger/message_area", 200, "application/json", R"({"ResKind":0,"TombMessList":[]})", true},
    {"/wandering_ghost/get", 200, "application/json", R"({"ResKind":0,"WanderingGhostList":[]})", true},
    // /channel/*: the Chalice Dungeon board (from_api_channel.h) answers them.
};

// Anything else under the gameurl (messages, bloodstains, ghosts, channels, net errors):
// an empty success (CONFIRMED for the generic APIs, A0 4.4).
constexpr Reply kDefaultReply = {"", 200, "application/json", R"({"ResKind":0})", false};

// A play-log PUT (bb-playlog-*.s3.amazonaws.com): S3's empty 200 (CONFIRMED: the uploader's
// PUT, bbhost http.cpp; S3 answers 200 with no body).
constexpr Reply kPlaylogReply = {"", 200, "text/plain", "", true};

// A SummonDataList entry (CONFIRMED, A0 4.5): SummonDataVersion must be an integer and SummonData
// must decode to exactly 0xE0 bytes or the entry is skipped; SummonType, CharaId, UserId are read;
// the rest is ignored by the game (kept for logs and tests).
constexpr const char* kSignTemplate =
    R"({"SummonDataVersion":${SummonDataVersion},"SummonData":${SummonData},"SummonType":${SummonType},)"
    R"("CharaId":${CharaId},"UserId":${UserId},"SummonDataId":${SummonDataId},"AreaId":${AreaId},)"
    R"("AreaRegionId":${AreaRegionId},"OnlineId":${OnlineId}})";

}  // namespace formats
