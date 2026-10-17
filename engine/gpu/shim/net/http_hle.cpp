// SPDX-License-Identifier: GPL-3.0-or-later
// derived from droogie/bbhost src/hle/http.cpp @8f2746c
//
// sceHttp / sceSsl for party play. Objects are integer handles like the SDK's: context ->
// template (UA, headers) -> connection (base URL) -> request. Nonblocking requests (the game's
// mode) run on a worker and report completion through sceHttpWaitRequest events; blocking ones
// run inline. bbport: no libcurl - perform() routes: the FROM game server's hosts, the play-log
// buckets and our gameurl go to FromApi (in-process on the host, the "http" PartyTransport
// call on a guest); anything else fails as a network error, as the offline stubs do. sceSsl
// hands out ids only.
#include "bbnet_internal.h"
#include "from_api.h"
#include "party_transport.h"
#include "party_util.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bbnet::http {

namespace {

constexpr int kHttpInvalidId = static_cast<int>(0x80431016u);  // SCE_HTTP_ERROR_INVALID_ID
constexpr int kHttpInvalidValue = static_cast<int>(0x80431019u);
constexpr int kHttpBeforeSend = static_cast<int>(0x80431002u);
constexpr int kHttpEagain = static_cast<int>(0x80431082u);   // SCE_HTTP_ERROR_EAGAIN
constexpr int kHttpNetwork = static_cast<int>(0x80431063u);  // SCE_HTTP_ERROR_NETWORK
constexpr int kHttpBusy = static_cast<int>(0x80431021u);
constexpr std::uint32_t kEvIn = 0x1, kEvOut = 0x2, kEvSockErr = 0x8, kEvHup = 0x10;

// ---- sceSsl: contexts are just ids.
std::mutex g_ssl_mu;
int g_ssl_next = 1;
std::unordered_set<int> g_ssl_ctx;

BBNET_ABI int ssl_init(std::uint64_t pool) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_ssl_mu);
    const int id = g_ssl_next++;
    g_ssl_ctx.insert(id);
    log("sceSslInit pool=%llu -> %d", static_cast<unsigned long long>(pool), id);
    return id;
}
BBNET_ABI int ssl_term() {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_ssl_mu);
    g_ssl_ctx.clear();
    return 0;
}

// ---- objects
struct NbEvent {
    std::uint32_t events;
    std::uint32_t detail;
    int id;
    void* user;
};
static_assert(sizeof(NbEvent) == 24);

struct HttpEpoll {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<NbEvent> events;
    bool aborting = false;
};
enum class Kind : int { Ctx = 1, Tmpl, Conn, Req };
struct Response {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    int error = 0;  // SCE error or 0
    long status = 0;
    std::string body;
    std::size_t read_off = 0;
};
struct Obj {
    Kind kind = Kind::Ctx;
    int parent = 0;
    int nonblock = -1;  // -1 inherit
    HttpEpoll* ep = nullptr;
    void* user = nullptr;
    std::string ua;
    std::string url;
    int method = 0;
    std::vector<std::string> headers;
    std::uint64_t content_len = 0;
    unsigned connect_timeout_us = 0;
    bool sent = false;
    std::shared_ptr<Response> resp;
};
std::mutex g_mu;
int g_next = 1;
std::unordered_map<int, Obj> g_objs;
std::unordered_set<HttpEpoll*> g_eps;

int alloc(Kind kind, int parent) {
    const int id = g_next++;
    Obj o{};
    o.kind = kind;
    o.parent = parent;
    g_objs[id] = o;
    return id;
}
Obj* get(int id, Kind kind) {
    auto it = g_objs.find(id);
    return it == g_objs.end() || it->second.kind != kind ? nullptr : &it->second;
}
Obj* any(int id) {
    auto it = g_objs.find(id);
    return it == g_objs.end() ? nullptr : &it->second;
}

// "Name: value" -> whether its name is `name` (case-insensitive).
bool header_named(const std::string& line, const std::string& name) {
    const auto colon = line.find(':');
    if (colon == std::string::npos || colon != name.size()) return false;
    for (std::size_t i = 0; i < colon; i++) {
        if (std::tolower(static_cast<unsigned char>(line[i])) != std::tolower(static_cast<unsigned char>(name[i])))
            return false;
    }
    return true;
}
std::string header_name(const std::string& line) { return line.substr(0, line.find(':')); }

// Request -> connection -> template -> context, collecting inherited settings.
struct Effective {
    std::string ua;
    std::vector<std::string> headers;
    bool nonblock = false;
    HttpEpoll* ep = nullptr;
    void* user = nullptr;
};
Effective effective_locked(int req_id) {
    Effective e;
    int nb = -1;
    for (int id = req_id; id;) {
        Obj* o = any(id);
        if (!o) break;
        // A nearer object's header wins over a parent's of the same name.
        const std::vector<std::string> nearer = e.headers;
        for (auto it = o->headers.rbegin(); it != o->headers.rend(); ++it) {
            const std::string name = header_name(*it);
            if (std::any_of(nearer.begin(), nearer.end(), [&](const std::string& h) { return header_named(h, name); }))
                continue;
            e.headers.insert(e.headers.begin(), *it);
        }
        if (e.ua.empty()) e.ua = o->ua;
        if (nb < 0 && o->nonblock >= 0) nb = o->nonblock;
        if (!e.ep && o->ep) {
            e.ep = o->ep;
            e.user = o->user;
        }
        id = o->parent;
    }
    e.nonblock = nb > 0;
    return e;
}

// Methods as the game numbers them: 0 GET, 1 POST, 2 HEAD, 4 PUT (the play-log upload).
const char* method_name(int m) {
    switch (m) {
    case 1: return "POST";
    case 2: return "HEAD";
    case 4: return "PUT";
    default: return "GET";
    }
}

std::atomic<int> g_ss_info{0}, g_login{0}, g_chara{0}, g_notice{0}, g_fail{0};
std::atomic<long long> g_last_ms{-1};

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void note_progress(const std::string& url, bool ok) {
    g_last_ms = now_ms();
    if (!ok) {
        ++g_fail;
        return;
    }
    auto has = [&](const char* s) { return url.find(s) != std::string::npos; };
    if (has("/ss.info")) ++g_ss_info;
    else if (has("/basic_utils/login")) ++g_login;
    else if (has("/basic_utils/sync_chara_id")) ++g_chara;
    else if (has("/basic_utils/get_normal_notice")) ++g_notice;
}

// The route: FromApi for the FROM hosts, the play-log buckets and our gameurl (in-process on
// the host, over the party link on a guest); a network error for anything else.
void perform(const std::string& url, int method, const Effective& eff, std::string post,
             std::shared_ptr<Response> resp) {
    const auto t0 = std::chrono::steady_clock::now();
    long status = 0;
    int err = 0;
    std::string body;
    if (!party::FromApi::routes(url)) {
        err = kHttpNetwork;
    } else {
        party::HttpRequest rq;
        rq.method = method_name(method);
        rq.url = url;
        rq.headers = eff.headers;
        if (!eff.ua.empty()) rq.headers.push_back("User-Agent: " + eff.ua);
        rq.body = std::move(post);
        if (settings().host) {
            party::HttpResponse out;
            party::FromApi::instance().handle(party::Caller{settings().online_id, 0}, rq, out);
            status = out.status;
            body = std::move(out.body);
            if (!status) err = kHttpNetwork;
        } else {
            // TODO(A4): RemoteGuest carries this to the host's FromApi (party_transport.h "http").
            json::Value j = json::Value::make_object();
            j.set("Method", rq.method);
            j.set("Url", rq.url);
            json::Value hs = json::Value::make_array();
            for (const std::string& h : rq.headers) hs.push(h);
            j.set("Headers", std::move(hs));
            j.set("Body", party::b64_encode(rq.body));
            json::Value reply;
            std::string e;
            if (party::transport().ok_call(party::call::kHttp, j, reply, e, 15000)) {
                status = static_cast<long>(party::int_of(reply, "Status", 0));
                const std::vector<std::uint8_t> b = party::b64_decode(party::str_of(reply, "Body"));
                body.assign(b.begin(), b.end());
                if (!status) err = kHttpNetwork;
            } else {
                err = kHttpNetwork;
                static std::atomic<int> logs{0};
                if (logs.fetch_add(1) < 8) log("http: %s -> the party host: %s", url.c_str(), e.c_str());
            }
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    note_progress(url, status == 200 && !err);
    static std::atomic<int> logs{0};
    if (party_trace() || url.find("/summon_messenger/") != std::string::npos || logs.fetch_add(1) < 24) {
        log("http: %s %s -> %ld (%zu bytes, %.0f ms)%s", method_name(method), url.c_str(), status, body.size(), ms,
            err ? " error" : "");
    }
    std::lock_guard<std::mutex> lk(resp->mu);
    resp->status = status;
    resp->error = err;
    resp->body = std::move(body);
    resp->done = true;
    resp->cv.notify_all();
}

void post_event(HttpEpoll* ep, int id, void* user, std::uint32_t events, std::uint32_t detail) {
    if (!ep) return;
    std::lock_guard<std::mutex> lk(ep->mu);
    ep->events.push_back(NbEvent{events, detail, id, user});
    ep->cv.notify_all();
}

// ---- entry points
BBNET_ABI int http_init(int net_mem, int ssl_ctx, std::uint64_t pool) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_mu);
    const int id = alloc(Kind::Ctx, 0);
    online_watch_start();
    log("sceHttpInit net=%d ssl=%d pool=%llu -> %d", net_mem, ssl_ctx, static_cast<unsigned long long>(pool), id);
    return id;
}
BBNET_ABI int http_term(int) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_mu);
    g_objs.clear();
    return 0;
}
BBNET_ABI int http_create_template(int ctx, const char* ua, int ver, int proxy) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_mu);
    if (!get(ctx, Kind::Ctx)) return kHttpInvalidId;
    const int id = alloc(Kind::Tmpl, ctx);
    g_objs[id].ua = ua ? ua : "";
    log("sceHttpCreateTemplate ctx=%d ua=%s ver=%d proxy=%d -> %d", ctx, ua ? ua : "", ver, proxy, id);
    return id;
}
BBNET_ABI int http_delete_object(int id) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_mu);
    g_objs.erase(id);
    return 0;
}
BBNET_ABI int http_set_nonblock(int id, int enable) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_mu);
    Obj* o = any(id);
    if (!o) return kHttpInvalidId;
    o->nonblock = enable ? 1 : 0;
    return 0;
}
BBNET_ABI int http_create_epoll(int ctx, HttpEpoll** out) {
    BBNET_GUEST_RETURN();
    if (!out) return kHttpInvalidValue;
    std::lock_guard<std::mutex> lock(g_mu);
    if (!get(ctx, Kind::Ctx)) return kHttpInvalidId;
    auto* ep = new HttpEpoll();
    g_eps.insert(ep);
    *out = ep;
    log("sceHttpCreateEpoll ctx=%d -> %p", ctx, static_cast<void*>(ep));
    return 0;
}
BBNET_ABI int http_destroy_epoll(int, HttpEpoll* ep) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_eps.erase(ep)) return kHttpInvalidValue;
    for (auto& kv : g_objs) {
        if (kv.second.ep == ep) kv.second.ep = nullptr;
    }
    // Waiters may still hold it: mark and leak rather than free under them.
    std::lock_guard<std::mutex> lk(ep->mu);
    ep->aborting = true;
    ep->cv.notify_all();
    return 0;
}
// sceHttpAddRequestHeader(id, name, value, mode): mode 0 (OVERWRITE) replaces a header of that
// name, 1 (ADD) adds another (the play-log uploader sets Authorization again before every PUT).
BBNET_ABI int http_add_header(int id, const char* name, const char* value, unsigned mode) {
    BBNET_GUEST_RETURN();
    if (!name) return kHttpInvalidValue;
    std::lock_guard<std::mutex> lock(g_mu);
    Obj* o = any(id);
    if (!o) return kHttpInvalidId;
    if (mode != 1) {
        const std::string n(name);
        o->headers.erase(std::remove_if(o->headers.begin(), o->headers.end(),
                                        [&](const std::string& h) { return header_named(h, n); }),
                         o->headers.end());
    }
    o->headers.push_back(std::string(name) + ": " + (value ? value : ""));
    return 0;
}
// sceHttpWaitRequest(SceHttpEpollHandle, SceHttpNBEvent* out, int maxevents, int timeout_us)
BBNET_ABI int http_wait_request(HttpEpoll* ep, NbEvent* out, int max_events, int timeout) {
    BBNET_GUEST_RETURN();
    if (!ep || !out || max_events <= 0) return kHttpInvalidValue;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_eps.count(ep)) return kHttpInvalidValue;
    }
    std::unique_lock<std::mutex> lk(ep->mu);
    auto ready = [&] { return !ep->events.empty() || ep->aborting; };
    if (timeout < 0) {
        ep->cv.wait(lk, ready);
    } else if (timeout > 0) {
        // Whole milliseconds, rounded up: a sub-millisecond wait would return at once and the
        // game's loop would spin out the rest (bbhost: winpthreads did that).
        ep->cv.wait_for(lk, std::chrono::milliseconds((timeout + 999) / 1000), ready);
    }
    if (ep->aborting) {
        ep->aborting = false;
        return 0;
    }
    int n = 0;
    while (n < max_events && !ep->events.empty()) {
        out[n++] = ep->events.front();
        ep->events.pop_front();
    }
    return n;
}
BBNET_ABI int http_abort_wait(HttpEpoll* ep) {
    BBNET_GUEST_RETURN();
    if (!ep) return kHttpInvalidValue;
    std::lock_guard<std::mutex> lk(ep->mu);
    ep->aborting = true;
    ep->cv.notify_all();
    return 0;
}
BBNET_ABI int https_option(int, unsigned) {
    BBNET_GUEST_RETURN();
    return 0;
}

BBNET_ABI int http_connect_url(int tmpl, const char* url, int) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_mu);
    if (!get(tmpl, Kind::Tmpl)) return kHttpInvalidId;
    const int id = alloc(Kind::Conn, tmpl);
    g_objs[id].url = url ? url : "";
    return id;
}
BBNET_ABI int http_request_url(int conn, int method, const char* url, std::uint64_t len) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_mu);
    if (!get(conn, Kind::Conn)) return kHttpInvalidId;
    const int id = alloc(Kind::Req, conn);
    Obj& r = g_objs[id];
    r.method = method;
    r.url = url ? url : "";
    r.content_len = len;
    r.resp = std::make_shared<Response>();
    return id;
}
BBNET_ABI int http_set_content_len(int id, std::uint64_t len) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_mu);
    Obj* r = get(id, Kind::Req);
    if (!r) return kHttpInvalidId;
    r->content_len = len;
    return 0;
}
BBNET_ABI int http_set_connect_timeout(int id, unsigned usec) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_mu);
    Obj* o = any(id);
    if (!o) return kHttpInvalidId;
    o->connect_timeout_us = usec;
    return 0;
}
BBNET_ABI int http_set_epoll(int id, HttpEpoll* ep, void* user) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_mu);
    Obj* o = any(id);
    if (!o) return kHttpInvalidId;
    o->ep = ep;
    o->user = user;
    return 0;
}
BBNET_ABI int http_unset_epoll(int id) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_mu);
    Obj* o = any(id);
    if (!o) return kHttpInvalidId;
    o->ep = nullptr;
    return 0;
}
BBNET_ABI int http_delete_request(int id) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_mu);
    if (!get(id, Kind::Req)) return kHttpInvalidId;
    g_objs.erase(id);
    return 0;
}

// sceHttpSendRequest(reqId, const void* postData, size_t size)
// C6 RE aid (BB_HTTP_TRACE_CALLERS=1): the guest call chain of each request (frame pointers:
// the game's functions keep rbp), as image offsets, to find what paces the title's sign-in.
__attribute__((noinline)) void trace_callers(const std::string& url, void* frame) {
    static const bool on = [] {
        const char* e = std::getenv("BB_HTTP_TRACE_CALLERS");
        return e && e[0] == '1';
    }();
    if (!on) return;
    const std::uintptr_t base = guest_image_base();
    const std::uint64_t size = guest_image_size();
    std::uintptr_t fp = 0;
    std::string chain;
    if (frame) guest_read(reinterpret_cast<std::uintptr_t>(frame), &fp, sizeof fp);  // the caller's rbp
    for (int i = 0; i < 12 && fp; ++i) {
        std::uintptr_t ret = 0, next = 0;
        if (!guest_read(fp + 8, &ret, sizeof ret) || !guest_read(fp, &next, sizeof next)) break;
        char b[24];
        if (base && ret >= base && ret < base + size) {
            std::snprintf(b, sizeof b, " %llx", static_cast<unsigned long long>(ret - base));
            chain += b;
        }
        if (next <= fp) break;
        fp = next;
    }
    log("http trace: %s from%s", url.c_str(), chain.empty() ? " ?" : chain.c_str());
}

BBNET_ABI int http_send(int id, const void* post, std::uint64_t size) {
    BBNET_GUEST_RETURN();
    std::string url;
    int method;
    Effective eff;
    std::shared_ptr<Response> resp;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        Obj* r = get(id, Kind::Req);
        if (!r) return kHttpInvalidId;
        if (r->sent) return kHttpBusy;
        r->sent = true;
        url = r->url;
        method = r->method;
        eff = effective_locked(id);
        resp = r->resp;
    }
    {
        char b[24];
        std::snprintf(b, sizeof b, " ret %llx",
                      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) -
                                                      guest_image_base()));
        trace_callers(url + b, __builtin_frame_address(0));
    }
    std::string body;
    if (post && size) body.assign(static_cast<const char*>(post), static_cast<std::size_t>(size));
    if (eff.nonblock) {
        HttpEpoll* ep = eff.ep;
        void* user = eff.user;
        std::thread([=] {
            perform(url, method, eff, body, resp);
            std::uint32_t ev = kEvOut | kEvIn;
            std::uint32_t detail = 0;
            {
                std::lock_guard<std::mutex> lk(resp->mu);
                if (resp->error) {
                    ev = kEvSockErr | kEvHup;
                    detail = static_cast<std::uint32_t>(resp->error);
                }
            }
            std::lock_guard<std::mutex> lock(g_mu);  // the epoll may have been destroyed meanwhile
            if (g_eps.count(ep)) post_event(ep, id, user, ev, detail);
        }).detach();
        return 0;
    }
    perform(url, method, eff, body, resp);
    std::lock_guard<std::mutex> lk(resp->mu);
    return resp->error;
}

std::shared_ptr<Response> resp_of(int id, bool* nonblock) {
    std::lock_guard<std::mutex> lock(g_mu);
    Obj* r = get(id, Kind::Req);
    if (nonblock) *nonblock = r && effective_locked(id).nonblock;
    return r && r->sent ? r->resp : nullptr;
}

// A nonblocking request answers EAGAIN until its response is in (the game asks once a frame);
// a blocking one waits (up to 35 s).
void wait_done(std::unique_lock<std::mutex>& lk, const std::shared_ptr<Response>& resp, bool nonblock) {
    if (resp->done || nonblock) return;
    resp->cv.wait_for(lk, std::chrono::seconds(35), [&] { return resp->done; });
}

BBNET_ABI int http_status(int id, int* code) {
    BBNET_GUEST_RETURN();
    if (!code) return kHttpInvalidValue;
    bool nonblock = false;
    auto resp = resp_of(id, &nonblock);
    if (!resp) return kHttpBeforeSend;
    std::unique_lock<std::mutex> lk(resp->mu);
    wait_done(lk, resp, nonblock);
    if (!resp->done) return kHttpEagain;
    if (resp->error) return resp->error;
    *code = static_cast<int>(resp->status);
    return 0;
}
// sceHttpGetResponseContentLength(reqId, int* result, uint64_t* contentLength): the SDK's
// three-argument form; result 0 = SCE_HTTP_CONTENTLEN_EXIST. (bbhost binds a two-argument
// form that writes the length through the second pointer - see docs/party/from_api_schema.md
// if A0 finds the game's call site disagrees.)
BBNET_ABI int http_resp_len(int id, int* result, std::uint64_t* len) {
    BBNET_GUEST_RETURN();
    if (!result && !len) return kHttpInvalidValue;
    bool nonblock = false;
    auto resp = resp_of(id, &nonblock);
    if (!resp) return kHttpBeforeSend;
    std::unique_lock<std::mutex> lk(resp->mu);
    wait_done(lk, resp, nonblock);
    if (!resp->done) return kHttpEagain;
    if (resp->error) return resp->error;
    if (result) *result = 0;
    if (len) *len = resp->body.size();
    return 0;
}
BBNET_ABI int http_read(int id, void* data, std::uint64_t size) {
    BBNET_GUEST_RETURN();
    if (!data) return kHttpInvalidValue;
    bool nonblock = false;
    auto resp = resp_of(id, &nonblock);
    if (!resp) return kHttpBeforeSend;
    std::unique_lock<std::mutex> lk(resp->mu);
    wait_done(lk, resp, nonblock);
    if (!resp->done) return kHttpEagain;
    if (resp->error) return resp->error;
    const std::size_t avail = resp->body.size() - resp->read_off;
    const std::size_t n = static_cast<std::size_t>(size < avail ? size : avail);
    std::memcpy(data, resp->body.data() + resp->read_off, n);
    resp->read_off += n;
    return static_cast<int>(n);
}

}  // namespace

}  // namespace bbnet::http

namespace bbnet {
OnlineProgress online_progress() {
    OnlineProgress p;
    p.ss_info = http::g_ss_info;
    p.login = http::g_login;
    p.chara_id = http::g_chara;
    p.notice = http::g_notice;
    p.failures = http::g_fail;
    const long long last = http::g_last_ms;
    p.since_last = last < 0 ? -1 : double(http::now_ms() - last) / 1000.0;
    return p;
}
}  // namespace bbnet

namespace bbnet {

#define H(name, fn) {name, reinterpret_cast<void*>(http::fn)}
const Export kHttpExports[] = {
    H("sceSslInit", ssl_init),
    H("sceSslTerm", ssl_term),
    H("sceHttpInit", http_init),
    H("sceHttpTerm", http_term),
    H("sceHttpCreateTemplate", http_create_template),
    H("sceHttpDeleteTemplate", http_delete_object),
    H("sceHttpSetNonblock", http_set_nonblock),
    H("sceHttpCreateEpoll", http_create_epoll),
    H("sceHttpDestroyEpoll", http_destroy_epoll),
    H("sceHttpAddRequestHeader", http_add_header),
    H("sceHttpWaitRequest", http_wait_request),
    H("sceHttpAbortWaitRequest", http_abort_wait),
    H("sceHttpsEnableOption", https_option),
    H("sceHttpsDisableOption", https_option),
    H("sceHttpCreateConnectionWithURL", http_connect_url),
    H("sceHttpCreateRequestWithURL", http_request_url),
    H("sceHttpSetRequestContentLength", http_set_content_len),
    H("sceHttpSetConnectTimeOut", http_set_connect_timeout),
    H("sceHttpSetEpoll", http_set_epoll),
    H("sceHttpUnsetEpoll", http_unset_epoll),
    H("sceHttpDeleteRequest", http_delete_request),
    H("sceHttpDeleteConnection", http_delete_object),
    H("sceHttpSendRequest", http_send),
    H("sceHttpGetStatusCode", http_status),
    H("sceHttpGetResponseContentLength", http_resp_len),
    H("sceHttpReadData", http_read),
    {nullptr, nullptr},
};
#undef H

}  // namespace bbnet
