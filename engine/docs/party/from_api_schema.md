# FROM API + ss.info schema (Bloodborne 1.09) — what the in-process server must answer

Phase A0 RE result. Addresses are **our offsets** (raw ELF VA of `E:\games\bloodborne\out\eboot.elf`,
VA 0 at file offset 0x4000; bbhost = ours + 0x400000). Confidence: **C** = confirmed in disassembly,
**L** = likely (strong indirect evidence), **G** = guess.

Tools used: capstone helper `scratchpad/re/bb.py` (relocations applied, eh_frame function bounds,
PLT import names), Ghidra via `tools/re/decomp.py`.

---------------------------------------------------------------------------------------------------

## 0. TL;DR — the server must answer

| Request | Method | Must answer (HTTP 200, body) | Notes |
|---|---|---|---|
| `GET https://ss4.scej-network.jp:20443/bb-{jp,us,eu,asia}/ss.info` (or `Game.Network.SsInfoUrl`) | GET | XML-ish text, see §2. `<ss>0</ss>` + `<gameurlN>` with all 37 `<api_X>BASE</api_X>` | `<ss>` must be exactly `0`; any non-200 / missing api tag = parse fail = retry 60 s, after 4 failures offline msg 0x1133 |
| `BASE/basic_utils/login` | POST | `{"ResKind":0,"SessionId":"s1","UserId":1001,"UserStatus":0,"LanguageId":1,"ServerVersion":109}` | SessionId non-empty string, UserId non-negative integer. Only ResKind 0 = success |
| `BASE/basic_utils/sync_chara_id?user_id=N` | POST | `{"ResKind":0,"PublishCharacterIdList":[{"PublishCharaId":1}]}` | list **must be an array**; first element's PublishCharaId becomes the CharaId (GameData+0x690, FrpgNetMan+0xa90) used in later requests |
| `BASE/basic_utils/get_normal_notice?user_id=N` | POST | `{"ResKind":0,"NoticeList":[]}` | NoticeList must be an array |
| `BASE/basic_utils/get_emergency_notice?user_id=N` | POST | `{"ResKind":0,"NoticeList":[],"CheckTime":"2026-10-10T12:00:00"}` | CheckTime optional, format `%d-%02d-%02dT%02d:%02d:%02d` |
| `BASE/penalty/check_user_priority_move_count?user_id=N` | POST | `{"ResKind":0}` | only `(code & 0xffff0000)==0` checked |
| `BASE/summon_messenger/create?user_id=N` | POST | `{"ResKind":0}` | nothing else read |
| `BASE/summon_messenger/get?user_id=N` | POST | `{"ResKind":0,"SummonDataList":[{"SummonDataVersion":3,"SummonData":"<base64 0xE0 bytes>","SummonType":0,"CharaId":1,"UserId":1002}]}` | SummonDataList must be an array; entry skipped unless SummonDataVersion == 3 and SummonData decodes to exactly 0xE0 bytes; echo the guest's create blob |
| `BASE/summon_messenger/request?user_id=N` | POST | `{"ResKind":0}` | nothing else read |
| `BASE/summon_messenger/delete?user_id=N` | POST | `{"ResKind":0}` | |
| every other `BASE/...` | POST | `{"ResKind":0, <its list key>:[]}` | list keys in §4.4 must be arrays, else the answer is marked invalid |
| play log PUT `bb-playlog-*.s3.amazonaws.com` | PUT | 200, empty | AES string `AES/CBC/PKCS5/MWC` is the play-log uploader's (0x1fe5530), not the FROM API |

Send JSON numbers as plain integer literals (§3.2: both the double and the int64 field get
filled either way, but integers are exact). `SummonDataVersion` in every SummonDataList entry
must equal **3**; `UserId` must fit in int32 (it is stored as int32 in the sign blob).

---------------------------------------------------------------------------------------------------

## 1. HTTP layer (FD4HttpMan, 0xfd0f50..0xfd3200) — C

* Template: `sceHttpCreateTemplate(ctx, ua, 2 /*HTTP/1.1*/, 1)` + `sceHttpSetNonblock(tmpl,1)` +
  `sceHttpCreateEpoll` (0xfd1cb0).
* Request (0xfd2740): `sceHttpCreateConnectionWithURL(tmpl, url, 1)`;
  method from req+0x70: **0 → GET (0)**, **1 → POST (1)**, **2 → PUT (4)**;
  `sceHttpCreateRequestWithURL(conn, method, url, 0)`; for POST/PUT
  `sceHttpSetRequestContentLength(req, body_len)`; `sceHttpSetConnectTimeOut(req, secs*K)` when
  the timeout float > 0; `sceHttpSetEpoll`.
* State machine 0xfd2970 (state at +0x84): 1 send (`sceHttpSendRequest(req, body, len)`),
  2 `sceHttpGetStatusCode(req, &status)` (status → req+0x80), 3
  `sceHttpGetResponseContentLength(req, &result, &len)` — **3 arguments** (C, 0xfd2a44:
  `edi=req, rsi=&int(rbp-0x444), rdx=&u64(rbp-0x450)`; neither value is used afterwards),
  4 `sceHttpReadData(req, buf, 0x400)` appended until it returns 0 → state 5 (done).
  Negative returns: `0x80431082` (EAGAIN) retry next tick, `0x80431021` (busy) retry,
  `0x80431068` (timeout) → state 6, anything else → state 5 with status forced
  (`[+0x80]=0xffffffff`, state 5).
  Also an app-side timeout (req+0xd0 seconds) → state 6.
* No request headers are added for FROM API calls (sceHttpAddRequestHeader callers are only the
  play-log uploader 0x1fe4240/0x1fe5530: `Content-Type: text/txt`, `Host`, `Authorization`).
  The game does not look at response headers. Chunked or not doesn't matter (reads until 0).
* Request bodies: JSON object built as wide strings, serialized, converted UTF-16 → UTF-8
  (0x29a2a50, mode 1) — **plain JSON, no encryption** (C). Responses: body bytes converted
  UTF-8 → UTF-16 (0x27cafa0) and parsed by a picojson-like parser 0x1eaf2f0 (C).

### 1.1 Result code the game computes per response (dispatcher 0x1e7f240, at 0x1e7f644) — C

```
if request state == 6 (timeout/socket error)  code = 0x80000003
elif HTTP status != 200                         code = 0x80000001
elif SprjNetworkClientMan+0x480 != 0 (abort)    code = 0x80000001
elif JSON parse error text non-empty            code = 0x80000004
elif api == ss.info (0x26)                      code = 0
else code = (int) root["ResKind"]  (double field; root not an object -> 0x80000004;
                                    key missing -> 0)
```
Then while reading the payload a "bad shape" flag (byte rsp+0xd64 bit0) is set when the root or a
list element is not an object or a required list is not an array. Final code handed to the
caller: `(code & 0xffff0000)==0 && flag ? 0x80000004 : code`.

**ResKind semantics** (category = ResKind >> 16):
* `0x0000xxxx` → success category. Login only accepts exactly **0** (0x1c48ca0: `0 → state 2`
  success, `0x10202/0x10203/0x100108 → error with code`, anything else → error). Use 0 everywhere.
* `0x0001xxxx` → API-level refusal codes (e.g. 0x1040a channel lost, 0x10101 → msg 0x11fa,
  0x10401 → msg 0x11fc, 0x1040c/0x1040d → msg 0x11f9).
* `0x0010xxxx` → server-global: **0x100108** = "reload ss.info" (re-fetch, 0x1e895f6),
  **0x10010a** → disconnect msg 0x1069, **0x10010b** → disconnect msg 0x10cd,
  **0x100181** → msg 0xfa6/0xfa7/0xfa9/0x11f9 depending on API.
* `0x8000xxxx` → client-side errors above.

"Drop offline" = 0x1ea5ce0(mgr, msgId): clears mgr UserId (+0x60 = 0x8000000000000000) and
SessionId (+0x70), queues the error message id into FrpgNetMan+0xa30, sets
**FrpgNetMan+0xa50 = 1** (server-offline flag; request builders refuse to send while it, +0x9f6,
+0x9f8 are set or FrpgNetMan+8 is 0). Called for: ss.info `<ss>`≠0 (0x1131/0x1132), 4 consecutive
ss.info failures (0x1133), WanderingGhostGet failing ≥3 times (0xfa1), 0x10010a/0x10010b, and
HTTP failure (0x80000001/3) of certain APIs (0x11f9 etc., switch 0x1e887a1 over APIs 0xc..0x23).

**Second drop path, outside the FROM responses (found live, 2026-10-10):** on entering the world
online, 0x193a3e0 (0x193aa94..0x193abd7) checks the character-name verdict at
`[[PlayerGameData+0x5d0]]+0xc` (1 = check pending, 2 = accepted, 3 = rejected / filter failed). Unless
it is 2 it calls 0x1e8bb10 (clears client UserId +0x60 / SessionId +0x70), queues msg **0x125d**
and sets **FrpgNetMan+0xa50 = 1** directly (not through 0x1ea5ce0). The verdict comes from the
Score comment filter run on the name when the save loads (0xcd09a0 `sceNpScoreSanitizeComment`,
0xcd0720 `sceNpScoreCensorComment`: 0 → accepted). The runtime's offline Score stubs answered
SIGNED_OUT, so every party member went offline on its first world load; the party NP layer
(np_manager.cpp) now answers both (comment unchanged / accepted). The only other writer of
+0xa50 = 1 is 0x1ea5ce0 (above); `Party online: the FROM client drops offline` logs its calls
(party_online.cpp), `Net: online:` lines log the state (online_watch.cpp).

---------------------------------------------------------------------------------------------------

## 2. ss.info

### 2.1 Where it comes from — C
* Fetch: 0x1e8aa40 (SprjNetworkClientMan). If `Game.Network.UseLocalSsInfo` (0x182d5c0 check)
  → reads `testdata:/server/ss.info.xml` (0x1e7e6b0). Else URL = config
  `Game.Network.SsInfoUrl` (UTF-16 string, read at 0x1e8abc7) if non-empty, otherwise table
  0x5358440 indexed by region `*(u32*)0x5128948` (<6):
  0 `https://ss4.scej-network.jp:20443/bb-jp/ss.info`, 1 `.../bb-us/ss.info`,
  2 `.../bb-eu/ss.info`, 3 `.../bb-asia/ss.info`, 4 = bb-eu, 5 = empty.
  Method GET, no body, timeout float 0x5590698.
* Triggered by the title task 0x1c492c0 (vtable 0x5348ed0), 0x1789b60, 0x195de30, the reload
  timer (mgr+0xd0, `ReloadServerStatusInfoInterval`) and ResKind 0x100108.
* Completion: 0x1e7f240 → (api 0x26) → 0x1e89850: requires request status OK; reads whole body;
  calls parser **0x1eb65e0**(alloc, text, &N, &L, out 0x9c0-byte object at mgr+0x18).
  On failure mgr+0x18 is freed; retry in 60 s (mgr+0xd0 = 60.0), fail counter mgr+0x3e4,
  4th failure → offline msg 0x1133.

### 2.2 Indices — C
* `N` (gameurl/all numeric tags/msg) = `*(u32*)(FrpgNetMan+0x9e8)`, set in the FrpgNetMan ctor
  (0x1489330 ← 0x201bd00) from table **0x4736550[region]** = `{jp:3, us:1, eu:2, asia:4, 4:2, 5:3}`.
* `L` (lang) = table 0x4736570[`*(u32*)0x55992f0` system language] = 1..21.
* Emitting the same content for every N in 0..9 is safe (each lookup is a substring search).

### 2.3 Grammar — C (it is not a real XML parser: substring searches, no whitespace trimming)
```
<ss>0</ss>                                   REQUIRED. text must be fully consumed by strtol(…,10)
                                             (no spaces). Stored out+8 (default 999).
                                             Must be 0: any other value → offline msg 0x1131/0x1132.
<msgN><langL>text</langL></msgN>             optional (out+0x10, shown as server message)
<BetaTestWebUrlN><langL>url</langL></BetaTestWebUrlN>   optional (out+0x48,
                                             default http://www.jp.playstation.com/scej/title/bloodborne/)
<gameurlN> <api_Login>BASE</api_Login> … all 37 … </gameurlN>   REQUIRED (see below)
<ReloadServerStatusInfoIntervalN>3600</…>    optional ints, defaults below
…
<PlaylogServerURLN>url</PlaylogServerURLN>   optional string (out+0x938)
<PlayLogN>0</PlayLogN>                       optional int (out+0x970, default 0)
<Playlog_xxxN>…</Playlog_xxxN>               optional bool bytes (0x1ecb9e0), default 0
```
`<gameurlN>`: helper 0x1ecb390 finds `<gameurlN>`, `</gameurlN>` after it, then `<api_X>` and
`</api_X>` searched from the gameurl start; success iff all found and both lie before
`</gameurlN>`. **Every one of the 37 APIs is required** (each failure returns 0 from the parser,
0x1eb7de6). Order of the table (0x5350dd0 open / 0x5350ca0 close tags, index = API id):
```
0 Login 1 ServerTimeGet 2 SyncCharaId 3 NoticeNormalGet 4 NoticeEmergencyGet 5 UserAgreementGet
6 BloodMessCreate 7 BloodMessGetList 8 BloodMessEvaluate 9 BloodMessGetEvaluate 10 BloodMessRemove
11 BloodMessSearchAdd 12 ChannelUpload 13 ChannelShare 14 ChannelSearch 15 ChannelWordSearch
16 ChannelGetDetailsInfo 17 ChannelGetInfo 18 ChannelRandomJoin 19 ChannelAddMaterial
20 ChannelAddMaterialCompleteNotify 21 MessengerShellUpload 22 MultiPlayNetError
23 UserPropertiesMoveCount 24 UserPropertiesMoveCountCheck 25 SummonDataCreate
26 SummonDataGetList 27 SummonDataRemove 28 SummonDataSummon 29 ChairMessGetList
30 ChairMessRespawnPointNotice 31 TombMessCreate 32 TombMessGetList 33 DeathVisionGet
34 TombMessRemove 35 WanderingGhostCreate 36 WanderingGhostGet
```
Value = URL base; the request URL is `BASE + "/basic_utils/login"` etc. (0x1eb3d10, wide format
`%s/...`), plus `?user_id=%lu` for every API except Login (0) and UserAgreementGet (5). So BASE is
`scheme://host[:port]` without trailing slash (C).

### 2.4 Numeric tags (out offset, default, parser) — C
Parsed with 0x1ecb620 (unsigned, whole text must be digits) except the two `Lower*` with
0x1ecb800 (signed). Missing/invalid → default kept.

| Tag | out+ | default |
|---|---|---|
| ReloadServerStatusInfoInterval | 0x898 | 3600 (s; copied to mgr+0xd0 when ss==0) |
| SummonDataCreateInterval | 0x89c | 30 (→ SummonStepManager+0x190) |
| SummonDataGetListInterval | 0x8a0 | 60 |
| SummonDataGetListGuardTimer | 0x8a4 | 60 |
| SummonDataGetListNomalCoopWaitCount | 0x8a8 | 1 |
| SummonDataGetListCombinationInvasionWaitCount | 0x8ac | 2 |
| SummonDataGetListNaturalEnemyWaitCountFixed | 0x8b0 | 4 |
| SummonDataGetListAllAreaMethodWaitCount | 0x8b4 | 5 |
| SummonDataGetListGetCountPerSummonType | 0x8b8 | 5 |
| SummonDataGetListGetMaxCount | 0x8bc | 20 |
| SummonDataCreateNaturalEnemyPercentFixed | 0x8c0 | 20 |
| SummonDataBloodMadHunterPercent | 0x8c4 | 5 |
| SummonDataGetListCoopGuardTimerClampTime | 0x8c8 | 60 |
| SummonDataGetListInvationGuardTimerClampTime | 0x8cc | 0 |
| SummonDataGetListCoopGuardTimerLimitationTimeRate | 0x8d0 | 100 |
| SummonDataGetListCoopGuardTimerObserveTimeRate | 0x8d4 | 100 |
| SummonDataGetListInvationGuardTimerLimitationTimeRate | 0x8d8 | 100 |
| SummonDataGetListInvationGuardTimerObserveTimeRate | 0x8dc | 100 |
| SummonDataCoopMatchingLevelUpperAbs | 0x8e0 | 20 |
| SummonDataCoopMatchingLevelUpperRel | 0x8e4 | 20 |
| SummonDataCoopMatchingLevelLowerAbs (signed) | 0x8e8 | -20 |
| SummonDataCoopMatchingLevelLowerRel (signed) | 0x8ec | -20 |
| TombMessGetListInterval | 0x8f0 | 120 |
| TombMessGetListGetCountPerArea | 0x8f4 | 30 |
| TombMessGetListGetMaxCount | 0x8f8 | 100 |
| TombMessGetEvaluateInterval | 0x8fc | 600 |
| BloodMessGetListInterval | 0x900 | 120 |
| BloodMessGetListGetCountPerArea | 0x904 | 30 |
| BloodMessGetListGetMaxCount | 0x908 | 100 |
| BloodMessGetEvaluateInterval | 0x90c | 600 |
| WanderingGhostCreateInterval | 0x910 | 60 |
| WanderingGhostGetInterval | 0x914 | 120 |
| WanderingGhostGetCountPerArea | 0x918 | 5 |
| WanderingGhostGetMaxCount | 0x91c | 10 |
| ChairMessRespawnPointNoticeInterval | 0x920 | 600 |
| ChairMessRespawnPointNoticeWaitTime | 0x924 | 300 |
| ChairMessGetListInterval | 0x928 | 300 |
| ChannelGetInfoInterval | 0x92c | 600 (→ mgr+0x160) |
| NoticeEmergencyGetInterval | 0x930 | 60 |
| MessengerShellUploadInterval | 0x934 | 600 |

**Co-op level window** (0x1582a70, constants 0x4926b2c..b38, C): a level `P` is accepted for
own level `L` iff
`LowerAbs + L*(LowerRel/100 + 1) <= P <= UpperAbs + L*(UpperRel/100 + 1)`
(defaults: `0.8L-20 .. 1.2L+20`). For "any level" send **LowerAbs = -1000, LowerRel = 0,
UpperAbs = 1000, UpperRel = 0**. (Positive 1000 for the Lower* tags, as the current
from_api_formats.inc does, makes the window empty.)

### 2.5 Example ss.info (index 1 shown; repeat the gameurl and numeric tags for N = 0..9)
```xml
<?xml version="1.0" encoding="utf-8"?>
<ssinfo><ss>0</ss>
<gameurl1><api_Login>http://bbparty.invalid:18671</api_Login><api_ServerTimeGet>http://bbparty.invalid:18671</api_ServerTimeGet>…(all 37)…</gameurl1>
<SummonDataGetListInterval1>5</SummonDataGetListInterval1>
<SummonDataCreateInterval1>5</SummonDataCreateInterval1>
<SummonDataCoopMatchingLevelLowerAbs1>-1000</SummonDataCoopMatchingLevelLowerAbs1>
<SummonDataCoopMatchingLevelLowerRel1>0</SummonDataCoopMatchingLevelLowerRel1>
<SummonDataCoopMatchingLevelUpperAbs1>1000</SummonDataCoopMatchingLevelUpperAbs1>
<SummonDataCoopMatchingLevelUpperRel1>0</SummonDataCoopMatchingLevelUpperRel1>
</ssinfo>
```
The root element name is irrelevant (never searched); no XML declaration is needed.

---------------------------------------------------------------------------------------------------

## 3. JSON details

### 3.1 Request envelope (0x1eb58d0) — C
POST body (UTF-8 JSON object) always has `"MessageId": "<Name>Request"` (table 0x5350b70, e.g.
`LoginRequest`, `SummonDataGetListRequest`). For every API except Login and UserAgreementGet
it also has `"SessionId": <string from login>` and `"UserId": <number>` and the URL gets
`?user_id=<UserId>`. Requests are refused client-side unless logged in
(mgr+0x60 ≠ 0x8000000000000000, SessionId non-empty, ss.info loaded with ss==0,
FrpgNetMan+0xa50/+0x9f6/+0x9f8 clear) — e.g. 0x1e942e0.

### 3.2 Parser value types (0x1eaf2f0) — C
`2` double (number text containing `-`, `.`, `e` or `E`; int64 field = (long)double),
`3` integer (digits only, strtoull base 10; double field = (double)value) — readers never
check 2 vs 3, they read the field they want, `5` string, `6` array, `7` object (`t`/`f`/`n` literals
supported). Whitespace: space, tab, CR, LF. Readers use: int64 field (node+0x68: UserId,
CharaId, PublishCharaId, ids), double field (node+0x60: ResKind, UserStatus, LanguageId,
ServerVersion), string (SessionId, CheckTime, SummonData). Missing scalar keys read as 0 /
empty without error; only a non-object root or a non-array list sets the bad-shape flag.

### 3.3 Datetime — C
`%d-%02d-%02dT%02d:%02d:%02d` (UTF-16 0x49a86ca): written in NoticeEmergencyGet requests
(`CheckTime`, 0x1ea487d) and parsed with swscanf from responses (0x1e7c880 ← NoticeEmergencyGet
`CheckTime`, 0x1e89da0). No zone suffix (treat as UTC). `basic_utils/get_datetime`
(ServerTimeGet, API 1) has **no request builder** in 1.09 and its answer goes to the generic
handler — never sent (L).

---------------------------------------------------------------------------------------------------

## 4. Endpoints

Response handler per API = switch table at 0x1e896f4 inside 0x1e7f240.
Request builders all call 0x1e8a880(mgr, …, apiId, …) except SummonDataRemove (0x1e942e0).

### 4.1 basic_utils/login (API 0) — C
* Callers: title task 0x1c48aa0 (callback vtable 0x5348dd0 → 0x1c48ca0), 0x1ea7a70.
* Request (0x1e8b040): `MessageId:"LoginRequest", PlatformAccountId, AuthorizationCode, NatType,
  RegionId, LanguageId, IssuerId, ApplicationVersion` (no SessionId/UserId, no query).
* Response reader 0x1e7c1d0:
  `UserId` (int64 → mgr+0x60, also GameData+0x688), `UserStatus` (number → +0x68),
  `LanguageId` (number → +0x6c), `SessionId` (string → mgr+0x70), `ServerVersion`
  (number → mgr+0xa8 → FrpgNetMan+0xa54, title shows it as " %d"),
  optional `WarningMessage` (string, only processed when present; shown as a warning).
* Success iff ResKind == 0 (0x1c48e75). `UserStatus` semantics unknown — send 0 (G).

### 4.2 basic_utils/sync_chara_id (API 2) — C
* Request 0x1e8be50: `CharaIdNum`. Title task 0x1c43ee0, callback 0x1c440a0.
* Response: `PublishCharacterIdList` **array** (else bad shape → error);
  the first element object's `PublishCharaId` (int64) → result+0x10; callback stores it to
  GameData+0x690 and FrpgNetMan+0xa90 (the CharaId sent in summon/message requests).
  Empty array → CharaId stays 0x8000000000000000 (avoid).

### 4.3 Notices / agreement / penalty — C
* get_normal_notice (3, builder 0x1ea3cd0: `Language`, `Region`): `NoticeList` array of
  objects (`Title`, `Notice`, … — empty is fine).
* get_emergency_notice (4, builder 0x1ea45a0: `Language`, `Region`, `CheckTime`):
  `CheckTime` (optional string), `NoticeList` array.
* get_user_agreement (5): `UserAgreementText`, `UserAgreementID` — no request builder
  found (L: not sent in 1.09).
* penalty/check_user_priority_move_count (24, `Count`, title task 0x1c43940, polled by
  0x1c436a0): only `(ResKind & 0xffff0000)==0`.
* penalty/notify_user_properties_move_count (23, `Count`), penalty/notify_multi_play_error
  (22, `HostUserId`, `TargetUserId`): generic, `{"ResKind":0}`.

### 4.4 Other APIs: the list keys that must be arrays (0x1ea9e20 call sites) — C
| API | array key(s) |
|---|---|
| BloodMessCreate (6) | BloodMessIdList |
| BloodMessGetList (7) | BloodMessList |
| BloodMessGetEvaluate (9) | BloodMessEvaluationList |
| BloodMessSearchAdd (11) | BloodMessEvaluationList (+LostBloodMessIdList read later) |
| ChannelSearch/WordSearch (14/15) | ChannelList |
| ChannelGetDetailsInfo (16) | ChannelList |
| ChannelGetInfo (17) | ChannelInfoList (LostChannelIdList when ResKind 0x1040a) |
| SummonDataGetList (26) | SummonDataList |
| ChairMessGetList (29) | ChairMessList (+ChairMessCount) |
| TombMessGetList (32) | TombMessList |
| WanderingGhostGet (36) | WanderingGhostList |
| SyncCharaId (2) | PublishCharacterIdList |
| NoticeNormalGet / NoticeEmergencyGet (3/4) | NoticeList |

Generic-only (ResKind): ServerTimeGet, BloodMessEvaluate, BloodMessRemove, ChannelShare,
ChannelAddMaterialCompleteNotify, MultiPlayNetError, UserPropertiesMoveCount,
SummonDataRemove, ChairMessRespawnPointNotice, TombMessRemove. Others read single ids
(BloodMessCreate `BloodMessId`, TombMessCreate `TombMessId`, WanderingGhostCreate
`WanderingGhostId`, DeathVisionGet `DeathVisionDataVersion`/`DeathVisionData`) that may be
omitted. WanderingGhostGet failing (HTTP error) 3 times → offline 0xfa1, so answer it.

### 4.5 summon_messenger — C (blob layout: §5)
* **create** (25, builder 0x1e90e30, callers 0x14b6b8c / 0x1ea669e). Request keys:
  `SessionId, UserId, ClientSummonTypeList, SummonMethod, SummonType, CharaId, NatType,
  MatchingLevel, Region, RegionFlag, AreaId, AreaRegionId, ChannelId, PosX, PosY, PosZ,
  SummonDataVersion, SummonData (base64), ClientVersion, SummonWord,
  NaturalEnemy_Vileblood, NaturalEnemy_VilebloodHunter, NaturalEnemy_BloodHunter,
  NaturalEnemy_HunterOfHunter`. Response handler 0x1e80a68: only the code
  (`SprjServerResKind` is a play-log field name, not a response key).
* **get** (26, builder 0x1e946e0, callers 0x1e60320 / 0x1e9862a). Request keys:
  `SummonTypeList:[{SummonType, GetLimitCount}], SummonWordMatchingType, AreaId, ChannelId,
  ClientSummonTypeList, GetMaxCount, CharaId, SummonDataVersion, NatType, ClientVersion,
  MatchingLevel, PosX, PosY, PosZ, Region, SummonMethod, AreaRegionId, DistanceThreshold,
  RegionFlag, SummonWord, UnlockFlagList, CoopOrNaturalEnemyRecruitNum,
  IsInvationMultiPlayRequesting, SessionId, UserId`.
  Response handler 0x1e80ab4: `SummonDataList` array (required); for each element (object):
  `SummonDataVersion` int64 value must **== 3** (`cmp dword [node+0x68],3` at 0x1e85085; missing
key reads 0) else the entry is skipped;
  `SummonData` string → base64 decode (0x1ea92e0) → must be **exactly 0xE0 bytes** else skipped
  (0x1e852c4); the game then writes blob[0x79] = 0x63 (0x1e85306); `SummonType`, `CharaId`,
  `UserId` (integers) read; entries go to the summon list manager (0x5540428, 0xfc03e0).
  Other keys are ignored.
* **request** (28, builder 0x1e98650, caller 0x14b4847): `CharaId, TargetUserId, TargetCharaId,
  SessionId, UserId`. Handler 0x1e80b57: only the code.
* **delete** (27, 0x1e942e0, caller 0x14b5bb0): `CharaId` (+SessionId/UserId). Generic.

---------------------------------------------------------------------------------------------------

## 5. SummonData blob (0xE0 bytes) — agent A0-3 + own checks

* **Built** inline in the SummonStepManager tick 0x14b5e10 (walks passive list SSM+0x158; copy
  0x14b67b7–0x14b6948 into a stack buffer) → create builder 0x1e90e30 (call 0x14b6b8c), which
  base64-encodes 0xE0 bytes (0x1ea87e0 @0x1e91078) into `"SummonData"`. Local sign source:
  CreateSign 0x14b9180 (from SendSign 0x1901320); intermediate SignData object 0xF8 bytes,
  vtable 0x5324d40.
* **Parsed** by 0x14bfe20 / inlined in SSM add-received-sign 0x14ba980. GetList handler per
  entry: version==3, decoded size==0xE0, writes blob[0x79]=0x63, `CharaId` → blob+0xD8
  (0x1e854bb), `UserId` (int32) → blob+0xD0 (0x1e855db), `SummonType` only echoed to the play
  log; wraps with 0x1486a10(obj, 0, 0xE0, blob) and calls 0x14ba980(FrpgNetMan+0xc50, wrapper)
  (0x1e856df). **So the server only has to echo the client's blob; the JSON UserId/CharaId
  override the blob's 0xD0/0xD8.**

| Off | Size | Meaning | Conf |
|---|---|---|---|
| 0x00 | 0x34 | 13× int32 phantom appearance/equipment ids (0x1900eb0) | M |
| 0x34 | 4 | packed phantom colour RGBA | M |
| 0x38 | 1 | ChrIns vcall +0x190 byte | G |
| 0x39 | 5 | ChrIns+0x4e8..0x4f8 floats ×100 as bytes | M |
| 0x40 | 16 | sender online ID, ASCII, NUL-padded (FrpgNetMan+0xa68) — dedupe key | C |
| 0x50 | 8 | creation timestamp/sequence (0x1490ce0), newer wins | C |
| 0x58 | 4 | AreaId (= JSON AreaId) | C |
| 0x5C | 12 | pos x,y,z floats (JSON PosX/Y/Z truncated) | C |
| 0x68 | 4 | yaw float | M |
| 0x6C | 4 | play-region id (JSON AreaRegionId derived) | L |
| 0x70 | 2 | int16 MatchingLevel (soul level, PlayerGameData+0x90) | C |
| 0x72 | 2 | int16 from player vcall +0x598 | G |
| 0x74 | 2 | u16 min(PlayerGameData+0xd4, 50000) | G |
| 0x76 | 1 | sign type (2 ForceJoin, 7 NormalCoop, 8 NormalInvade, 0xA InvadeBounty, …) | C |
| 0x77 | 1 | resend/refresh counter | M |
| 0x78 | 1 | (int) sign lifetime (SSM+0x194 = 30.0 initially) | M |
| 0x79 | 1 | 0 on create; receiver forces 99 (remaining count) — **not** a version byte | C |
| 0x7A | 2 | u16 length of next field (0x25) | C |
| 0x7C | 0x50 | OnlineID serialization: byte 2 + 36-byte SceNpId, rest 0 (0xca2c90) | C |
| 0xCC | 1 | NAT type (sceNetCtlGetNatInfo, FrpgNetMan+0x9ec) | C |
| 0xD0 | 4 | int32 server UserId (create sends 0xFFFFFFFF; GetList overwrites) | C |
| 0xD8 | 8 | u64 CharaId (create: FrpgNetMan+0xa90; GetList overwrites) | C |

Receiver filtering: version/size above; host-state gate in 0x14ba980 (player SpEffect
stateInfo 0xBD → any sign type, 0xBF → only type 8; M/G semantics); dedupe 0x14b8d10 (same
type + 16 bytes at 0x40; equal timestamps merge, older expires). **No client-side area, level
or password check on intake** — level/area matching is server-side (the client sends its own
`MatchingLevel`, `AreaId`, `SummonTypeList`), so our party server decides who sees which sign.
(The ss.info level window §2.4 is used by a separate check, 0x15091c0 → 0x1582a70.)

**Party host intake (bbport security pass).** Because the receiving game checks nothing, the host's
board (`FromApi::sign_create`, `summon_data_problem` in gpu/shim/net/from_api.h) refuses a member's
sign before any other member's game can list it unless: the blob is exactly 0xE0 bytes of strict
base64; 0x40 is the creating member's own online id (1-16 of `[A-Za-z0-9_-]`, NUL padded — the
dedupe key, so nobody can replace or expire another member's sign); 0x58 AreaId is a map of the game
(`coop::ids::MapKnown`, generated from the map folders / MSBs by tools/party/ids_tool.py) and equals
the request's `AreaId`; position and yaw are finite with |v| < 100000; 0x70 MatchingLevel is 1..544
and equals the request's `MatchingLevel` when sent; 0x76 is a known sign type (1, 2, 5, 7, 8, 0xA,
19 — the descriptor table 0x553d750 has 0x22 rows); 0x7A == 0x25, 0x7C == 2 and the SceNpId handle at
0x7D is the member's own id (terminator 0x8D == 0); 0xCC NAT type is 0..3. The 13 equipment ids at
0x00 are not checked against the EquipParam tables.

Host side after picking a sign: 0x14baec0 (caller 0x1872360) sets the messenger object
SSM+0x58 (vtable 0x53249e0): +0x120 TargetUserId = blob 0xD0, +0x128 TargetCharaId = blob 0xD8;
0x14b47c0 → 0x1e98650 sends summon_messenger/request; callback 0x14be080 stores the code at
+0xEC; 0x14b48c0 continues only if code ≠ 0x10301 and `(code>>16)==0`. When FrpgNetMan+0xb == 0
the message is queued locally (SSM+0x1c0). The actual join goes over NP Matching2/signaling
(guest identified by the SceNpId at blob 0x7C); where the host consumes 0x7C/0xCC was not found.

---------------------------------------------------------------------------------------------------

## 6. Corrections for gpu/shim/net/from_api_formats.inc

1. `kSsStatus = "0"` — CONFIRMED (anything else → offline msg 0x1131/0x1132).
2. `kSsInfoFallbackIndices` — CONFIRMED-safe; real N = 0x4736550[region] ∈ {1,2,3,4}.
3. `kApiBase` — CONFIRMED shape (`scheme://host[:port]`, no trailing slash; paths appended).
4. `kSsValues`: **SummonDataCoopMatchingLevelLowerAbs → "-1000", LowerRel → "0",
   UpperAbs → "1000", UpperRel → "0"** (current 1000s for Lower* make every sign out of range).
   `PlaylogServerURL` empty is fine (string tag). Values must be bare digits (no spaces).
5. Login reply: `{"ResKind":0,"SessionId":"${SessionId as JSON string}","UserId":${UserId},
   "UserStatus":0,"LanguageId":1,"ServerVersion":109}` — drop `ServerTime`/`MessageId`
   (ignored). SessionId must be a non-empty JSON **string**; UserId a non-negative integer.
6. sync_chara_id reply: `{"ResKind":0,"PublishCharacterIdList":[{"PublishCharaId":${CharaId}}]}`
   (current `"CharaId"` key is not read and the missing array makes the answer invalid).
7. get_datetime: never requested; any `{"ResKind":0}` is fine.
8. notices: `{"ResKind":0,"NoticeList":[]}` — CONFIRMED (emergency may add
   `"CheckTime":"YYYY-MM-DDTHH:MM:SS"`).
9. get_user_agreement: keys are `UserAgreementText` (string) and `UserAgreementID` (number);
   not requested in 1.09 (L).
10. summon create/delete/request: `{"ResKind":0}` — extra keys harmless.
11. summon get: `{"ResKind":0,"SummonDataList":[…]}`; entry =
    `{"SummonDataVersion":3,"SummonData":"<b64 of 0xE0 bytes>","SummonType":N,"CharaId":N,"UserId":N}`
    (SummonDataId/AreaId/AreaRegionId/OnlineId are ignored by the game). The SummonData to echo is
    exactly what the guest posted in summon_messenger/create; SummonType should be the
    create request's `SummonType` (only logged).
12. kDefaultReply `{"ResKind":0}` is fine for generic APIs, but list APIs need their array
    (§4.4) — e.g. `/blood_messenger/message_area` → `{"ResKind":0,"BloodMessList":[]}`,
    `/tomb_messenger/message_area` → `TombMessList`, `/wandering_ghost/get` →
    `WanderingGhostList`, `/chair_messenger/get` → `ChairMessList`,
    `/blood_messenger/evaluation` and `/exist_messages` → `BloodMessEvaluationList`,
    `/blood_messenger/create` → `BloodMessIdList`, `/channel/*search*`,`get_details_info` →
    `ChannelList`, `/channel/get_info` → `ChannelInfoList`.
13. HTTP status must be exactly 200; Content-Type is not checked.
14. Numbers: integer literals recommended; `SummonDataVersion` must be 3; UserId ≤ 0x7fffffff.

---------------------------------------------------------------------------------------------------

## 7. Title "Play Online / Play Offline" and forcing online (agent A0-5, static analysis)

Dialog builder **0x1b39030** (called unconditionally from title ctor 0x1b384f0 @0x1b3871c and
re-entry functor 0x1b4bc30 @0x1b4bce8 — the normal boot prompt, L): CommandList with
| order | text id | text | functor vtable | operator() |
|---|---|---|---|---|
| 1 | 0x61e73 (401011) | Play Online | 0x533be80 | 0x1b49eb0 |
| 2 | 0x61e72 (401010) | Play Offline | 0x533be30 | 0x1b49cd0 |

Default cursor = byte 0x558037d (global 0x55801c8+0x1b5, loaded by 0x1c35b50 from saved
preference bit). Both operator()s are identical except one byte — the flag of the inner functor
(vtable 0x533bd90, op 0x1b48d40): **online 0x1b49ee1 `C6 45 C0 01`, offline 0x1b49d01
`C6 45 C0 00`**. 0x1b48d40 → step chain 0x1c36a80; first step op 0x1c3feb0 writes
obj(0x55801c8)+0x1b4=1, +0x1b5=+0x1b6=!flag (0x558037e = "offline" indicator) and
**FrpgNetMan+0xa = flag** (0x1c3ff2b). Branch 0x1c36d88 `cmp byte [rax],0; je` → flag 1 runs the
online chain **0x1c37b30**: sceNetCtlGetState (0x1c413c0) → sceNpGetState (0x1c41250) →
sceNpGetGamePresenceStatus (0x1c40f50) → sceNpCheckNpAvailability (0x1e76140) →
sceNpGetParentalControlInfo (0x1e763e0) → PS Plus check → NpCommerce dialog; then the FROM tasks
(ss.info 0x1c492c0, login 0x1c48aa0, sync_chara_id 0x1c43ee0, move-count check 0x1c43940,
normal notice 0x1c485d0).

Fallbacks to offline: step error handler 0x1c3f330 (vtable 0x5347790) → functor 0x1c3f930
(+0x1b6=1, FrpgNetMan+0xa=0, msg 0x61e6a "Starting game in offline mode"), or msg 0x61e69
"Returning to title menu" when error counters 0x5578fd8/0x5578a38/0x5578d08 > 0; 0x1c42cb0
(0x1c42d65/0x1c42d9f, "not fully installed"); 0x1c3f1d0.

**Force online (C/high):** patch 0x1b49d04 `00`→`01` (offline item behaves as online).
Weaker: NOP `je` 0x1c36d8b (`74 26`→`90 90`) — forces the sign-in branch but leaves
FrpgNetMan+0xa = 0. Either way the NP/NetCtl shim must report connected / signed in /
available / no parental restriction / Plus OK, and the FROM server must pass ss.info + login.

## 8. Bells and Insight (agent A0-5)

Insight = PlayerGameData ([[0x553b130]+8]) +0x84 (Lua GetHeroPoint 0x1338670).
EquipParamGoods: 200 Beckoning Bell (+0x38 Insight cost = **1**), 205 Small Resonant Bell (0),
225 Sinister Resonant Bell (0); bell SpEffects 9000/9005/9025 do not change Insight.

* `OnEvent_Call_SOS` (ASCII 0x492eca5) dispatched at 0x130ca87 in 0x130c590 (host activates a
  sign; type 1, 3 BlackSOS, 9 DragonewtSOS). No Insight check.
* `OnEvent_SendSoulSign_NormalCoop` (0x4931557) dispatched at 0x1901794 in SendSign 0x1901320
  (types 7, 0x15–0x18, 0x1a, 0x1d–0x20). Gates 0x1901566 (flow+0x16f8 table [+0x14] ≤ 1),
  0x1901585 (session member list empty); sign created by 0x14b9180. Chain 0x1900500 → task op
  0x130cde0 → 0x1901010 → 0x1901320.
* **Insight requirement** in goods-usability check 0x157f200:
  `0157fa51 movzx ecx, byte [r13+0x38]; 0157fa56 cmp ecx, [rax+0x84]; 0157fa5c setg al`
  → patch 0x157fa5c `0F 9F C0` → `30 C0 90`. Other gates there: 0x157f8c2/0x157f8c8 (region ≤
  999999), area validator 0x131d7b0 @0x157f90e (result 0x157f960), 205/225 via 0x191a750 @0x157f6cc
  and 0x18c96d0, 0x157f58e (online mode flow+0x1590 == 0 && goods+0x42 bit21 "online only" —
  set for 205/225), 0x157f578, 0x157f9a9.
* **Insight consumption** in UseGoods 0x18c9160: `018c92bb movzx eax, byte [r15+0x38]; neg eax;
  mov [rbp-0x44], eax` (delta applied by 0x18b61c0, clamp 0..99 at 0x18b64cd–0x18b650c)
  → patch 0x18c92bb `41 0F B6 47 38 F7 D8` → `31 C0 0F 1F 44 00 00` (affects goods 200/201 only).
