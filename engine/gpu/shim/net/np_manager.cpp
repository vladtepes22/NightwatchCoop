// SPDX-License-Identifier: GPL-3.0-or-later
// derived from droogie/bbhost src/hle/system.cpp @8f2746c (NP state, auth, Plus, WebApi, Lookup)
// and src/hle/np_matching2.cpp @8f2746c (hle_np_fill_npid)
//
// libSceNp for party play (BB_PARTY): the user is signed in to a network with no PSN behind it.
// The online id is the party name (BB_PARTY_NAME, else BB_USER_NAME); NpAuth hands out a dummy
// authorization code, Plus is authorized, WebApi answers empty friend/block lists, Lookup maps
// an online id to its NpId. Score, Trophy, Commerce and ProfileDialog stay on the runtime's
// offline stubs (runtime_services.c); Matching2 and Signaling come with the host service (A3).
#include "bbnet_internal.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace bbnet {

void fill_npid(void* out, const char* online) {
    if (!out) return;
    std::memset(out, 0, 36);  // SceNpId {SceNpOnlineId handle {char data[16], term, dummy[3]}, opt[8], reserved[8]}
    if (!online || !online[0]) online = "Player";
    std::snprintf(static_cast<char*>(out), 17, "%s", online);
}

namespace {

constexpr int kNpInvalidArgument = static_cast<int>(0x80550003);

// ---- State --------------------------------------------------------------------------------------

// SceNpStateCallback(userId, state, const SceNpId*, userdata).
using NpStateCb = void(BBNET_ABI*)(int, int, void*, void*);
using NpPresenceCb = void(BBNET_ABI*)(void*, int, void*);
std::mutex g_np_mu;
NpStateCb g_np_state_cb;
void* g_np_state_ud;
NpPresenceCb g_np_pres_cb;
void* g_np_pres_ud;
bool g_np_state_fired;
bool g_np_pres_fired;
std::uint8_t g_np_id[36];

BBNET_ABI int np_reg_state(NpStateCb cb, void* ud) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lk(g_np_mu);
    g_np_state_cb = cb;
    g_np_state_ud = ud;
    g_np_state_fired = false;
    log("sceNpRegisterStateCallback");
    return 0;
}
BBNET_ABI int np_unreg_state(void*) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lk(g_np_mu);
    g_np_state_cb = nullptr;
    g_np_state_ud = nullptr;
    return 0;
}
BBNET_ABI int np_reg_presence(NpPresenceCb cb, void* ud) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lk(g_np_mu);
    g_np_pres_cb = cb;
    g_np_pres_ud = ud;
    g_np_pres_fired = false;
    log("sceNpRegisterGamePresenceCallback");
    return 0;
}
BBNET_ABI int np_get_state(int, int* state) {
    BBNET_GUEST_RETURN();
    if (!state) return kNpInvalidArgument;
    *state = 2;  // SCE_NP_STATE_SIGNED_IN
    return 0;
}
// sceNpCheckCallback: the state callback fires once (SIGNED_IN) on the calling thread, as the
// console fires it from inside this call. bbhost keeps this a no-op (sceNpGetState already says
// signed in); BB_NP_STATE_CB=0 does the same here.
BBNET_ABI int np_check_cb() {
    BBNET_GUEST_RETURN();
    NpStateCb state_cb = nullptr;
    void* state_ud = nullptr;
    NpPresenceCb pres_cb = nullptr;
    void* pres_ud = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_np_mu);
        if (!settings().np_state_callback) return 0;
        if (!g_np_state_fired && g_np_state_cb) {
            g_np_state_fired = true;
            fill_npid(g_np_id, settings().online_id.c_str());
            state_cb = g_np_state_cb;
            state_ud = g_np_state_ud;
        }
        if (!g_np_pres_fired && g_np_pres_cb) {
            g_np_pres_fired = true;
            pres_cb = g_np_pres_cb;
            pres_ud = g_np_pres_ud;
        }
    }
    if (state_cb) log("sceNpCheckCallback -> SignedIn (%s)", settings().online_id.c_str());
    restore_guest_fs();  // before guest code: the lock and the log above may have waited
    if (state_cb) {
        state_cb(1, 2, g_np_id, state_ud);  // (userId, SCE_NP_STATE_SIGNED_IN, npId, userdata)
    }
    if (pres_cb) pres_cb(g_np_id, 1, pres_ud);
    return 0;
}
BBNET_ABI int np_online_id(int, void* id) {
    BBNET_GUEST_RETURN();
    if (!id) return kNpInvalidArgument;
    const std::string& cur = settings().online_id;
    std::memset(id, 0, 20);  // SceNpOnlineId {char data[16], term, dummy[3]}
    std::memcpy(id, cur.c_str(), cur.size() < 16 ? cur.size() : 16);
    return 0;
}
BBNET_ABI int np_get_npid(int, void* id) {
    BBNET_GUEST_RETURN();
    if (!id) return kNpInvalidArgument;
    // The same handle as sceNpGetOnlineId: the session service matches peers by it.
    fill_npid(id, settings().online_id.c_str());
    return 0;
}
BBNET_ABI int np_presence_status(int, int* st) {
    BBNET_GUEST_RETURN();
    if (st) *st = 1;  // online
    return 0;
}

// ---- Async requests, availability, Plus, parental control ----------------------------------------

std::atomic<int> g_np_req{1};
BBNET_ABI int np_create_async_request(const void*) { return g_np_req.fetch_add(1); }
BBNET_ABI int np_ok_int(int) { return 0; }
BBNET_ABI int np_poll_async(int, int* result) {
    BBNET_GUEST_RETURN();
    if (result) *result = 0;
    return 0;  // finished
}
BBNET_ABI int np_check_availability(int, const void*, void*) { return 0; }
// sceNpGetParentalControlInfo(reqId, const SceNpOnlineId*, int8* age, SceNpParentalControlInfo* {bool content, chat, ugc})
BBNET_ABI int np_parental(int, const void*, std::int8_t* age, std::uint8_t* info) {
    BBNET_GUEST_RETURN();
    if (age) *age = 25;
    if (info) std::memset(info, 0, 3);
    return 0;
}
// sceNpCheckPlus(reqId, const SceNpCheckPlusParameter*, SceNpCheckPlusResult* {bool authorized})
BBNET_ABI int np_check_plus(int, const void*, std::uint8_t* result) {
    BBNET_GUEST_RETURN();
    if (result) result[0] = 1;
    return 0;
}
BBNET_ABI int np_plus_cb(const void*, void*) { return 0; }
BBNET_ABI int np_plus_notify(int, std::uint64_t) { return 0; }

// ---- NpAuth ---------------------------------------------------------------------------------------

BBNET_ABI int npauth_create_async_request(const void*) { return g_np_req.fetch_add(1); }
// sceNpAuthGetAuthorizationCode(reqId, const param*, SceNpAuthorizationCode* {char code[128]}, int* issuerId)
BBNET_ABI int npauth_get_code(int, const void*, char* code, int* issuer) {
    BBNET_GUEST_RETURN();
    if (code) {
        std::memset(code, 0, 128);
        std::strncpy(code, "DUMMY", 127);
    }
    if (issuer) *issuer = 10;
    return 0;
}

// ---- Score: the comment filter only ---------------------------------------------------------------
// The game runs the character name through the Score comment filter (sceNpScoreSanitizeComment in
// 0xcd09a0, sceNpScoreCensorComment in 0xcd0720) when a save loads and keeps the verdict at [PlayerGameData+0x5d0]->+0xc (2 = accepted). Entering the world online,
// 0x193a3e0 (0x193aa94..0x193abd7) logs out of the FROM server - clears UserId / SessionId (0x1e8bb10),
// queues msg 0x125d and sets FrpgNetMan+0xa50 (server offline) - unless that verdict is 2. The
// runtime's offline Score stub answered SIGNED_OUT (verdict 3), so every party member dropped
// offline on its first world load and no summon_messenger request was ever sent. There is no
// word filter behind the party: every comment is accepted as is. The rest of Score (ranking,
// game data) stays on the offline stubs.

// sceNpScoreCensorComment(reqId, const char* comment, void* option): 0 = acceptable.
BBNET_ABI int score_censor(int, const char* comment, void*) {
    BBNET_GUEST_RETURN();
    if (!comment) return kNpInvalidArgument;
    if (party_trace()) log("sceNpScoreCensorComment '%s' -> accepted", comment);
    return 0;
}
// sceNpScoreSanitizeComment(reqId, const char* comment, char* sanitized, void* option): the comment
// unchanged (the game's buffer is 256 bytes; it only passes comments shorter than 255).
BBNET_ABI int score_sanitize(int, const char* comment, char* sanitized, void*) {
    BBNET_GUEST_RETURN();
    if (!comment || !sanitized) return kNpInvalidArgument;
    const std::size_t n = strnlen(comment, 255);
    std::memcpy(sanitized, comment, n);
    sanitized[n] = 0;
    if (party_trace()) log("sceNpScoreSanitizeComment '%s' -> unchanged", sanitized);
    return 0;
}

// ---- WebApi: empty friend and block lists ----------------------------------------------------------

constexpr int kWebApiInvalid = static_cast<int>(0x80552902);
constexpr int kWebApiNoReq = static_cast<int>(0x80552906);
constexpr char kWebApiJsonType[] = "application/json; charset=utf-8";

struct WebApiReq {
    int ctx = 0;
    int status = 200;
    std::string body;
    std::size_t read_off = 0;
};
std::mutex g_web_mu;
int g_web_lib = 1;
int g_web_ctx = 1;
int g_web_filter = 1;
int g_web_cb = 1;
std::int64_t g_web_req = 1;
std::unordered_map<std::int64_t, WebApiReq> g_web_reqs;

const char* webapi_body_for_path(const char* path) {
    if (path && std::strstr(path, "friendList")) return "{\"totalResults\":0,\"friendList\":[]}";
    if (path && std::strstr(path, "blockList")) return "{\"totalResults\":0,\"blockList\":[]}";
    return "{\"totalResults\":0}";
}

BBNET_ABI int webapi_init(int http_ctx, std::uint64_t pool) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_web_mu);
    const int id = g_web_lib++;
    log("sceNpWebApiInitialize http=%d pool=%llu -> %d", http_ctx, static_cast<unsigned long long>(pool), id);
    return id;
}
BBNET_ABI int webapi_create_ctx(int lib, const void*) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_web_mu);
    const int id = g_web_ctx++;
    log("sceNpWebApiCreateContext lib=%d -> %d", lib, id);
    return id;
}
BBNET_ABI int webapi_create_filter(int lib, const void*, std::uint64_t n) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_web_mu);
    const int id = g_web_filter++;
    log("sceNpWebApiCreatePushEventFilter lib=%d n=%llu -> %d", lib, static_cast<unsigned long long>(n), id);
    return id;
}
BBNET_ABI int webapi_delete_filter(int, int) { return 0; }
BBNET_ABI int webapi_reg_push(int ctx, int filter, void*, void*) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_web_mu);
    const int id = g_web_cb++;
    log("sceNpWebApiRegisterPushEventCallback ctx=%d filter=%d -> %d", ctx, filter, id);
    return id;
}
BBNET_ABI int webapi_unreg_push(int, int) { return 0; }
BBNET_ABI int webapi_create_req(int ctx, const char* group, const char* path, int method, const void*,
                                std::int64_t* req_id) {
    BBNET_GUEST_RETURN();
    if (!path || !req_id) return kWebApiInvalid;
    std::lock_guard<std::mutex> lock(g_web_mu);
    const std::int64_t id = g_web_req++;
    WebApiReq r{};
    r.ctx = ctx;
    r.body = webapi_body_for_path(path);
    g_web_reqs[id] = std::move(r);
    *req_id = id;
    static int logs;
    if (logs < 8) {
        log("sceNpWebApiCreateRequest ctx=%d group=%s path=%s method=%d -> %lld", ctx, group ? group : "", path, method,
            static_cast<long long>(id));
        ++logs;
    }
    return 0;
}
BBNET_ABI int webapi_delete_req(std::int64_t req) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_web_mu);
    g_web_reqs.erase(req);
    return 0;
}
BBNET_ABI int webapi_send(std::int64_t req, const void*, std::uint64_t) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_web_mu);
    return g_web_reqs.count(req) ? 0 : kWebApiNoReq;
}
BBNET_ABI int webapi_status(std::int64_t req, int* code) {
    BBNET_GUEST_RETURN();
    if (!code) return kWebApiInvalid;
    std::lock_guard<std::mutex> lock(g_web_mu);
    auto it = g_web_reqs.find(req);
    if (it == g_web_reqs.end()) return kWebApiNoReq;
    *code = it->second.status;
    return 0;
}
BBNET_ABI int webapi_read(std::int64_t req, void* data, std::uint64_t size) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_web_mu);
    auto it = g_web_reqs.find(req);
    if (it == g_web_reqs.end()) return kWebApiNoReq;
    WebApiReq& r = it->second;
    if (!data || size == 0 || r.read_off >= r.body.size()) return 0;
    std::size_t n = static_cast<std::size_t>(size);
    const std::size_t left = r.body.size() - r.read_off;
    if (n > left) n = left;
    std::memcpy(data, r.body.data() + r.read_off, n);
    r.read_off += n;
    return static_cast<int>(n);
}
BBNET_ABI int webapi_hdr_len(std::int64_t req, const char* name, std::uint64_t* len) {
    BBNET_GUEST_RETURN();
    if (!name || !len) return kWebApiInvalid;
    std::lock_guard<std::mutex> lock(g_web_mu);
    if (!g_web_reqs.count(req)) return kWebApiNoReq;
    if (std::strcmp(name, "Content-Type") == 0 || std::strcmp(name, "content-type") == 0) {
        *len = sizeof(kWebApiJsonType);
        return 0;
    }
    return kWebApiInvalid;
}
BBNET_ABI int webapi_hdr_val(std::int64_t req, const char* name, char* buf, std::uint64_t n) {
    BBNET_GUEST_RETURN();
    if (!name || !buf || n == 0) return kWebApiInvalid;
    std::lock_guard<std::mutex> lock(g_web_mu);
    if (!g_web_reqs.count(req)) return kWebApiNoReq;
    if (std::strcmp(name, "Content-Type") != 0 && std::strcmp(name, "content-type") != 0) return kWebApiInvalid;
    std::snprintf(buf, static_cast<std::size_t>(n), "%s", kWebApiJsonType);
    return 0;
}
// sceNpWebApiUtilityParseNpId(const char* json, SceNpId*): a JSON object with onlineId, a
// quoted string or a bare id.
BBNET_ABI int webapi_parse_npid(const char* json, void* npid) {
    BBNET_GUEST_RETURN();
    if (!json || !npid) return kWebApiInvalid;
    const char* s = json;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
    char online[17]{};
    auto take_quoted = [&](const char* p) -> bool {
        const char* q = std::strchr(p, '"');
        if (!q) return false;
        ++q;
        if (p != s) {  // after a key: skip to the value's opening quote
            q = std::strchr(q, '"');
            if (!q) return false;
            q = std::strchr(q + 1, '"');
            if (!q) return false;
            ++q;
        }
        const char* e = std::strchr(q, '"');
        if (!e) return false;
        const std::size_t n = static_cast<std::size_t>(e - q);
        if (n == 0 || n >= sizeof(online)) return false;
        std::memcpy(online, q, n);
        online[n] = 0;
        return true;
    };
    if (*s == '{') {
        const char* key = std::strstr(s, "\"onlineId\"");
        if (!key || !take_quoted(key)) return kWebApiInvalid;
    } else if (*s == '"') {
        if (!take_quoted(s)) return kWebApiInvalid;
    } else {
        std::snprintf(online, sizeof(online), "%s", s);
    }
    fill_npid(npid, online);
    return 0;
}

// ---- Lookup -----------------------------------------------------------------------------------------

std::mutex g_lookup_mu;
int g_lookup_ctx = 1;
int g_lookup_req = 1;
BBNET_ABI int lookup_title_ctx(const void*, const void*) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_lookup_mu);
    const int id = g_lookup_ctx++;
    log("sceNpLookupCreateTitleCtx -> %d", id);
    return id;
}
BBNET_ABI int lookup_async(int, const void*) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_lookup_mu);
    return g_lookup_req++;
}
BBNET_ABI int lookup_npid(int, const void* online, void* npid, void*) {
    BBNET_GUEST_RETURN();
    if (!online || !npid) return kNpInvalidArgument;
    char id[17]{};
    std::memcpy(id, online, 16);
    fill_npid(npid, id);
    return 0;
}

}  // namespace

// Names not listed here (sceNpCmp*, sceNpSetNpTitleId, sceNpSetContentRestriction, the rest of Score,
// Trophy, Commerce, ProfileDialog, Matching2, Signaling) stay on the runtime's stubs.
const Export kNpExports[] = {
    {"sceNpRegisterStateCallback", reinterpret_cast<void*>(np_reg_state)},
    {"sceNpUnregisterStateCallback", reinterpret_cast<void*>(np_unreg_state)},
    {"sceNpRegisterGamePresenceCallback", reinterpret_cast<void*>(np_reg_presence)},
    {"sceNpGetState", reinterpret_cast<void*>(np_get_state)},
    {"sceNpCheckCallback", reinterpret_cast<void*>(np_check_cb)},
    {"sceNpGetOnlineId", reinterpret_cast<void*>(np_online_id)},
    {"sceNpGetNpId", reinterpret_cast<void*>(np_get_npid)},
    {"sceNpGetGamePresenceStatus", reinterpret_cast<void*>(np_presence_status)},
    {"sceNpCreateAsyncRequest", reinterpret_cast<void*>(np_create_async_request)},
    {"sceNpDeleteRequest", reinterpret_cast<void*>(np_ok_int)},
    {"sceNpAbortRequest", reinterpret_cast<void*>(np_ok_int)},
    {"sceNpPollAsync", reinterpret_cast<void*>(np_poll_async)},
    {"sceNpCheckNpAvailability", reinterpret_cast<void*>(np_check_availability)},
    {"sceNpGetParentalControlInfo", reinterpret_cast<void*>(np_parental)},
    {"sceNpCheckPlus", reinterpret_cast<void*>(np_check_plus)},
    {"sceNpRegisterPlusEventCallback", reinterpret_cast<void*>(np_plus_cb)},
    {"sceNpUnregisterPlusEventCallback", reinterpret_cast<void*>(np_plus_cb)},
    {"sceNpNotifyPlusFeature", reinterpret_cast<void*>(np_plus_notify)},
    {"sceNpAuthCreateAsyncRequest", reinterpret_cast<void*>(npauth_create_async_request)},
    {"sceNpAuthDeleteRequest", reinterpret_cast<void*>(np_ok_int)},
    {"sceNpAuthPollAsync", reinterpret_cast<void*>(np_poll_async)},
    {"sceNpAuthGetAuthorizationCode", reinterpret_cast<void*>(npauth_get_code)},
    {"sceNpScoreCensorComment", reinterpret_cast<void*>(score_censor)},
    {"sceNpScoreSanitizeComment", reinterpret_cast<void*>(score_sanitize)},
    {"sceNpWebApiInitialize", reinterpret_cast<void*>(webapi_init)},
    {"sceNpWebApiTerminate", reinterpret_cast<void*>(np_ok_int)},
    {"sceNpWebApiCreateContext", reinterpret_cast<void*>(webapi_create_ctx)},
    {"sceNpWebApiDeleteContext", reinterpret_cast<void*>(np_ok_int)},
    {"sceNpWebApiCreatePushEventFilter", reinterpret_cast<void*>(webapi_create_filter)},
    {"sceNpWebApiDeletePushEventFilter", reinterpret_cast<void*>(webapi_delete_filter)},
    {"sceNpWebApiRegisterPushEventCallback", reinterpret_cast<void*>(webapi_reg_push)},
    {"sceNpWebApiUnregisterPushEventCallback", reinterpret_cast<void*>(webapi_unreg_push)},
    {"sceNpWebApiCreateRequest", reinterpret_cast<void*>(webapi_create_req)},
    {"sceNpWebApiDeleteRequest", reinterpret_cast<void*>(webapi_delete_req)},
    {"sceNpWebApiAbortRequest", reinterpret_cast<void*>(webapi_delete_req)},
    {"sceNpWebApiSendRequest", reinterpret_cast<void*>(webapi_send)},
    {"sceNpWebApiGetHttpStatusCode", reinterpret_cast<void*>(webapi_status)},
    {"sceNpWebApiReadData", reinterpret_cast<void*>(webapi_read)},
    {"sceNpWebApiGetHttpResponseHeaderValueLength", reinterpret_cast<void*>(webapi_hdr_len)},
    {"sceNpWebApiGetHttpResponseHeaderValue", reinterpret_cast<void*>(webapi_hdr_val)},
    {"sceNpWebApiUtilityParseNpId", reinterpret_cast<void*>(webapi_parse_npid)},
    {"sceNpLookupCreateTitleCtx", reinterpret_cast<void*>(lookup_title_ctx)},
    {"sceNpLookupDeleteTitleCtx", reinterpret_cast<void*>(np_ok_int)},
    {"sceNpLookupCreateAsyncRequest", reinterpret_cast<void*>(lookup_async)},
    {"sceNpLookupDeleteRequest", reinterpret_cast<void*>(np_ok_int)},
    {"sceNpLookupAbortRequest", reinterpret_cast<void*>(np_ok_int)},
    {"sceNpLookupPollAsync", reinterpret_cast<void*>(np_poll_async)},
    {"sceNpLookupNpId", reinterpret_cast<void*>(lookup_npid)},
    {nullptr, nullptr},
};

}  // namespace bbnet
