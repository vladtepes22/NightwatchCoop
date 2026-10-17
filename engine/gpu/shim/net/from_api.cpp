// SPDX-License-Identifier: GPL-3.0-or-later
// FromApi (from_api.h): the FROM game server's API answered by the party host. Formats are in
// from_api_formats.inc.
#include "from_api.h"

#include "../../party_env.h"

#include "bbnet_internal.h"
#include "party_util.h"
#include "../party/party_ids.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>

namespace bbnet::party {

#include "from_api_formats.inc"

namespace {

// BB_PARTY_FAST_JOIN (default on): the quicker sign polling of kSsFastJoinValues.
bool fast_join() { return bb_party_switch_on("BB_PARTY_FAST_JOIN") != 0; }

std::atomic<std::uint64_t> g_sign_changes{0};
std::atomic<std::uint64_t> g_summons{0};

std::int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Url {
    std::string scheme, host, path, query;
    int port = 0;
};

Url parse_url(const std::string& url) {
    Url u;
    std::size_t p = url.find("://");
    std::size_t h = 0;
    if (p != std::string::npos) {
        u.scheme = url.substr(0, p);
        h = p + 3;
    }
    std::size_t end = url.find_first_of("/?", h);
    std::string hostport = url.substr(h, end == std::string::npos ? std::string::npos : end - h);
    const std::size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        u.port = std::atoi(hostport.c_str() + colon + 1);
        hostport = hostport.substr(0, colon);
    }
    for (char& c : hostport) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    u.host = hostport;
    if (end == std::string::npos) {
        u.path = "/";
        return u;
    }
    const std::size_t q = url.find('?', end);
    u.path = url.substr(end, q == std::string::npos ? std::string::npos : q - end);
    if (u.path.empty()) u.path = "/";
    if (q != std::string::npos) u.query = url.substr(q + 1);
    return u;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_playlog_host(const std::string& host) {
    return host == "bb-playlog-test.s3.amazonaws.com" || host == "bb-playlog-prod.s3.amazonaws.com";
}

// ${Name} -> vars[Name] (raw JSON text), "null" when unknown.
std::string render(const char* tmpl, const std::map<std::string, std::string>& vars) {
    std::string out;
    for (const char* p = tmpl; *p;) {
        if (p[0] == '$' && p[1] == '{') {
            const char* e = std::strchr(p + 2, '}');
            if (e) {
                const std::string key(p + 2, static_cast<std::size_t>(e - p - 2));
                auto it = vars.find(key);
                out += it != vars.end() ? it->second : "null";
                p = e + 1;
                continue;
            }
        }
        out += *p++;
    }
    return out;
}

std::string jstr(const std::string& s) { return json::dump(json::Value(s), 0); }
std::string jnum(long long v) { return std::to_string(v); }

std::string preview(const std::string& body) {
    bool text = true;
    for (unsigned char c : body) {
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
            text = false;
            break;
        }
    }
    if (text) return body.size() > 600 ? body.substr(0, 600) + "..." : body;
    std::string hex;
    char b[4];
    for (std::size_t i = 0; i < body.size() && i < 96; ++i) {
        std::snprintf(b, sizeof(b), "%02x", static_cast<unsigned char>(body[i]));
        hex += b;
    }
    return "<" + std::to_string(body.size()) + " bytes: " + hex + (body.size() > 96 ? "...>" : ">");
}

// N for the ss.info element names: FrpgNetMan+0x9e8, when the game has it.
bool ss_info_index(unsigned* out) {
    void* slot = guest_image_at(formats::kFrpgNetManSlot, 8);
    if (!slot) return false;
    std::uint64_t man = 0;
    if (!guest_read(reinterpret_cast<std::uintptr_t>(slot), &man, 8) || man < 0x10000) return false;
    std::uint32_t n = 0;
    if (!guest_read(static_cast<std::uintptr_t>(man) + formats::kFrpgNetManServerIndexOff, &n, 4)) return false;
    *out = n;
    return true;
}

}  // namespace

std::string from_api_ss_info() {
    std::vector<unsigned> indices;
    unsigned n = 0;
    if (ss_info_index(&n)) {
        indices.push_back(n);
    } else {
        indices.assign(std::begin(formats::kSsInfoFallbackIndices), std::end(formats::kSsInfoFallbackIndices));
    }
    std::string x;
    x += "<ss>";
    x += formats::kSsStatus;
    x += "</ss>\n";
    for (unsigned i : indices) {
        const std::string sfx = std::to_string(i);
        for (const formats::SsValue& v : formats::kSsValues) {
            const char* value = v.value;
            if (fast_join()) {
                for (const formats::SsValue& f : formats::kSsFastJoinValues) {
                    if (std::strcmp(f.name, v.name) == 0) value = f.value;
                }
            }
            x += "<" + std::string(v.name) + sfx + ">" + value + "</" + v.name + sfx + ">\n";
        }
        x += "<gameurl" + sfx + ">\n";
        for (const char* api : formats::kApiNames) {
            x += std::string("<") + api + ">" + formats::kApiBase + "</" + api + ">\n";
        }
        x += "</gameurl" + sfx + ">\n";
    }
    return x;
}

FromApi::FromApi(PartyHostService& service) : service_(service) {}

FromApi& FromApi::instance() {
    static FromApi* api = new FromApi(PartyHostService::instance());
    return *api;
}

bool FromApi::routes(const std::string& url) {
    const Url u = parse_url(url);
    if (u.host == kGameHost) return true;
    if (is_playlog_host(u.host)) return true;
    return ends_with(u.host, "scej-network.jp");
}

std::uint64_t FromApi::user_id_of(const std::string& online_id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [id, uid] : users_) {
        if (id == online_id) return uid;
    }
    users_.emplace_back(online_id, next_user_);
    return next_user_++;
}

std::vector<FromApi::Sign> FromApi::signs() const {
    std::lock_guard<std::mutex> lk(mu_);
    return signs_;
}

void FromApi::handle(const Caller& caller, const HttpRequest& rq, HttpResponse& out) {
    const Url u = parse_url(rq.url);
    out = HttpResponse{};
    if (is_playlog_host(u.host)) {
        out.status = formats::kPlaylogReply.status;
        out.content_type = formats::kPlaylogReply.content_type;
        out.body = formats::kPlaylogReply.body;
    } else if (ends_with(u.path, "ss.info")) {
        out.status = 200;
        out.content_type = "text/plain";
        // The game base64-decodes the ss.info body before parsing it (the completion 0x1e7f240
        // streams [response +0x58] through the base64 decoder 0x1ea92e0 -> 0xfd06f0 at 0x1e8169e,
        // then calls 0x1e89850 -> parser 0x1eb65e0); plain XML decodes to garbage and the parse
        // fails (seen with BB_PARTY_TRACE_SSINFO=1), so no login ever follows.
        out.body = b64_encode(from_api_ss_info());
    } else {
        json::Value body;
        std::string err;
        const bool is_json = !rq.body.empty() && rq.body.find('{') != std::string::npos &&
                             json::parse(rq.body.substr(rq.body.find('{')), body, err);
        if (!is_json) body = json::Value::make_object();
        // ?user_id= on summon_messenger calls (simclient.py): the body's UserId wins.
        if (!body.find("UserId") && u.query.rfind("user_id=", 0) == 0) {
            body.set("UserId", static_cast<long long>(std::strtoll(u.query.c_str() + 8, nullptr, 10)));
        }
        out.body = dispatch(caller, u.path, body, &out.status, &out.content_type);
    }
    if (party_trace()) {
        log("from api: %s %s %s (%s) -> %d %s", caller.online_id.c_str(), rq.method.c_str(), rq.url.c_str(),
            preview(rq.body).c_str(), out.status, preview(out.body).c_str());
    }
}

void FromApi::handle_json(const Caller& caller, const json::Value& rq, json::Value& reply) {
    HttpRequest h;
    h.method = str_of(rq, "Method");
    if (h.method.empty() || h.method.size() > 16) h.method = "GET";
    h.url = str_of(rq, "Url");
    if (h.url.size() > kMaxUrl) {  // a peer's request: nothing the game sends is this long
        reply = json::Value::make_object();
        reply.set("ResKind", 1);
        reply.set("Error", "url too long");
        return;
    }
    if (const json::Value* hs = rq.find("Headers"); hs && hs->type == json::Value::Type::Array) {
        for (const json::Value& v : hs->array) {
            if (h.headers.size() >= kMaxHeaders) break;
            if (v.type == json::Value::Type::String && v.string.size() <= 1024) h.headers.push_back(v.string);
        }
    }
    const std::vector<std::uint8_t> b = b64_decode(str_of(rq, "Body"));
    h.body.assign(b.begin(), b.end());
    HttpResponse r;
    handle(caller, h, r);
    reply = json::Value::make_object();
    reply.set("ResKind", 0);
    reply.set("Status", r.status);
    reply.set("ContentType", r.content_type);
    reply.set("Body", b64_encode(r.body));
}

std::string FromApi::dispatch(const Caller& caller, const std::string& path, const json::Value& body, int* status,
                              std::string* content_type) {
    const formats::Reply* reply = &formats::kDefaultReply;
    for (const formats::Reply& r : formats::kReplies) {
        if (ends_with(path, r.path)) {
            reply = &r;
            break;
        }
    }
    *status = reply->status;
    *content_type = reply->content_type;
    const std::uint64_t uid = user_id_of(caller.online_id);
    std::map<std::string, std::string> vars;
    char sid[32];
    std::snprintf(sid, sizeof(sid), "bbp-%llu", static_cast<unsigned long long>(uid));
    vars["SessionId"] = jstr(sid);
    vars["UserId"] = jnum(static_cast<long long>(uid));
    const std::time_t now = std::time(nullptr);
    vars["Now"] = jnum(static_cast<long long>(now));
    char text[32];
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &tm);  // the game's CheckTime format
    vars["NowText"] = jstr(text);
    // sync_chara_id publishes the CharaId every later request carries; one per member.
    vars["CharaId"] = jnum(int_of(body, "CharaId", static_cast<long long>(uid)));
    if (ends_with(path, "/summon_messenger/create")) vars["SummonDataId"] = sign_create(caller, body);
    else if (ends_with(path, "/summon_messenger/get")) vars["SummonDataList"] = sign_get(caller, body);
    else if (ends_with(path, "/summon_messenger/delete")) sign_delete(caller, body);
    else if (ends_with(path, "/summon_messenger/request")) vars["Result"] = sign_request(caller, body);
    else if (const std::size_t at = path.rfind("/channel/"); at != std::string::npos) {
        *status = 200;
        *content_type = "application/json";
        return channels_.handle(path.substr(at + 9), caller.online_id, uid, body, text);
    }
    return render(reply->body, vars);
}

const char* summon_data_problem(const std::uint8_t* d, std::size_t size, const std::string& online_id,
                                const json::Value* body) {
    if (size != kSummonDataSize) return "not 0xE0 bytes";
    auto u16 = [&](std::size_t o) { return static_cast<unsigned>(d[o] | d[o + 1] << 8); };
    auto u32 = [&](std::size_t o) {
        return static_cast<std::uint32_t>(d[o]) | static_cast<std::uint32_t>(d[o + 1]) << 8 |
               static_cast<std::uint32_t>(d[o + 2]) << 16 | static_cast<std::uint32_t>(d[o + 3]) << 24;
    };
    auto f32 = [&](std::size_t o) {
        const std::uint32_t bits = u32(o);
        float f;
        std::memcpy(&f, &bits, sizeof f);
        return f;
    };
    // An online id field of 16 bytes: the id, then NULs only.
    auto id_field = [&](std::size_t o) {
        std::size_t n = 0;
        while (n < 16 && d[o + n]) ++n;
        for (std::size_t i = n; i < 16; ++i)
            if (d[o + i]) return false;
        if (n == 0 || n != online_id.size() || std::memcmp(d + o, online_id.data(), n) != 0) return false;
        for (std::size_t i = 0; i < n; ++i) {
            const unsigned char c = d[o + i];
            if (!std::isalnum(c) && c != '_' && c != '-') return false;
        }
        return true;
    };
    if (!id_field(0x40)) return "online id at 0x40 is not the sender's";
    const std::uint32_t area = u32(0x58);
    if (!coop::ids::MapKnown(area)) return "AreaId at 0x58 is not a map of the game";
    if (body && body->find("AreaId") && static_cast<long long>(area) != int_of(*body, "AreaId", -1))
        return "AreaId at 0x58 differs from the request's";
    for (std::size_t o : {0x5Cu, 0x60u, 0x64u, 0x68u}) {
        const float v = f32(o);
        if (!std::isfinite(v) || std::fabs(v) >= 100000.0f) return "position / yaw not finite or out of range";
    }
    const int level = static_cast<std::int16_t>(u16(0x70));
    if (level < 1 || level > kMaxMatchingLevel) return "MatchingLevel at 0x70 out of range";
    if (body && body->find("MatchingLevel") && level != int_of(*body, "MatchingLevel", -1))
        return "MatchingLevel at 0x70 differs from the request's";
    switch (d[0x76]) {
    case 1: case 2: case 5: case 7: case 8: case 0xA: case 19: break;
    default: return "sign type at 0x76 unknown";
    }
    if (u16(0x7A) != 0x25 || d[0x7C] != 2) return "OnlineID serialization header at 0x7A wrong";
    if (!id_field(0x7D) || d[0x8D] != 0) return "SceNpId at 0x7D is not the sender's";
    if (d[0xCC] > 3) return "NAT type at 0xCC out of range";
    return nullptr;
}

std::string FromApi::sign_create(const Caller& caller, const json::Value& body) {
    // The sign goes into other members' games as it is: their native code reads SummonData
    // (exactly 0xE0 bytes, from_api_formats.inc) and the numbers. Anything else is refused.
    std::vector<std::uint8_t> data;
    const long long type = int_of(body, "SummonType", 0);
    const long long version = int_of(body, "SummonDataVersion", 3);
    const long long area = int_of(body, "AreaId", 0);
    const long long region = int_of(body, "AreaRegionId", 0);
    if (!b64_decode_strict(str_of(body, "SummonData"), &data) || data.size() != kSummonDataSize || type < 0 ||
        type > 255 || version < 0 || version > 0xffff || area < -0x80000000LL || area > 0xffffffffLL ||
        region < -0x80000000LL || region > 0x7fffffffLL) {
        // AreaId and AreaRegionId are signed: the Hunter's Dream and "Bells anywhere" areas are
        // negative (Dream gate 0x47304B0 = -1, seamless_rules.md), and a live guest's sign there was refused.
        log("from api: %s: refused a malformed sign (type %lld, version %lld, %zu data bytes, area %lld, region %lld)",
            caller.online_id.c_str(), type, version, data.size(), area, region);
        return "0";
    }
    if (const char* bad = summon_data_problem(data.data(), data.size(), caller.online_id, &body)) {
        log("from api: %s: refused a sign: %s", caller.online_id.c_str(), bad);
        return "0";
    }
    Sign s;
    s.user_id = user_id_of(caller.online_id);
    s.online_id = caller.online_id;
    s.area = static_cast<std::uint32_t>(int_of(body, "AreaId", 0));
    s.region = static_cast<int>(int_of(body, "AreaRegionId", 0));
    s.summon_type = static_cast<int>(int_of(body, "SummonType", 0));
    s.data_b64 = str_of(body, "SummonData");
    s.chara_id = int_of(body, "CharaId", static_cast<long long>(s.user_id));
    s.version = version;
    s.channel = int_of(body, "ChannelId", 0);
    s.request = body;
    s.posted_ms = steady_now_ms();
    if (ChaliceArea(area)) channels_.note_member_channel(caller.online_id, static_cast<std::uint64_t>(s.channel));
    std::lock_guard<std::mutex> lk(mu_);
    // One sign per member and type: a new one replaces the old.
    signs_.erase(std::remove_if(signs_.begin(), signs_.end(),
                                [&](const Sign& o) { return o.user_id == s.user_id && o.summon_type == s.summon_type; }),
                 signs_.end());
    std::size_t mine = 0;
    for (const Sign& o : signs_) mine += o.user_id == s.user_id ? 1 : 0;
    if (mine >= kMaxSignsPerUser) {
        log("from api: %s has %zu signs up; refused another", caller.online_id.c_str(), mine);
        return "0";
    }
    s.id = next_sign_++;
    signs_.push_back(s);
    g_sign_changes.fetch_add(1, std::memory_order_relaxed);
    log("from api: %s put up a sign (type %d, area 0x%x, channel %lld) -> %llu", caller.online_id.c_str(), s.summon_type,
        s.area, s.channel, static_cast<unsigned long long>(s.id));
    return jnum(static_cast<long long>(s.id));
}

std::string FromApi::sign_get(const Caller& caller, const json::Value& body) {
    std::vector<int> types;
    if (const json::Value* tl = body.find("SummonTypeList"); tl && tl->type == json::Value::Type::Array) {
        for (const json::Value& t : tl->array) types.push_back(static_cast<int>(int_of(t, "SummonType", 0)));
    }
    long long max = int_of(body, "GetMaxCount", 32);
    if (max <= 0) max = 32;
    // Chalice Dungeons (from_api.h): a host inside one summons only when its dungeon is on the
    // channel board - the summoned game fetches it by ChannelId (get_details_info) and quick-joins
    // it; without it that game would build its own slot's dungeon instead.
    const long long my_area = int_of(body, "AreaId", 0);
    const long long my_channel = int_of(body, "ChannelId", 0);
    const bool in_chalice = ChaliceArea(my_area);
    bool dungeon_unknown = false;
    if (in_chalice) {
        channels_.note_member_channel(caller.online_id, static_cast<std::uint64_t>(my_channel));
        dungeon_unknown = my_channel <= 0 || !channels_.known(static_cast<std::uint64_t>(my_channel));
    }
    int held = 0, prologue_held = 0, young_held = 0;
    std::int64_t min_age = 0;
    std::string list = "[";
    int n = 0;
    std::vector<Sign> board;
    bool caller_prologue = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        board = signs_;
        // Playtest 1 #19: no summon into or out of a prologue world (whoever finishes first waits).
        caller_prologue = prologue_.count(caller.online_id) != 0;
        const std::size_t before = board.size();
        board.erase(std::remove_if(board.begin(), board.end(),
                                   [&](const Sign& s) { return caller_prologue || prologue_.count(s.online_id) != 0; }),
                    board.end());
        prologue_held = static_cast<int>(before - board.size());
        const std::int64_t now = steady_now_ms();
        holds_.erase(std::remove_if(holds_.begin(), holds_.end(), [&](const auto& h) { return h.second <= now; }),
                     holds_.end());
        board.erase(std::remove_if(board.begin(), board.end(),
                                   [&](const Sign& s) {
                                       for (const auto& h : holds_)
                                           if (h.first == s.online_id) return true;
                                       return false;
                                   }),
                    board.end());
        // A4: a sign younger than kMinSignAgeMs is not offered yet (its bell is barely up).
        min_age = min_sign_age_ms_;
        const std::size_t before_young = board.size();
        board.erase(std::remove_if(board.begin(), board.end(),
                                   [&](const Sign& s) { return now - s.posted_ms < min_sign_age_ms_; }),
                    board.end());
        young_held = static_cast<int>(before_young - board.size());
    }
    // Party-only by construction; every other member's signs, wherever they stand (the party
    // plays together - the game's own area check still applies on its side). Oldest first: the
    // member who waited longest is offered first.
    for (const Sign& s : board) {
        if (s.online_id == caller.online_id) continue;
        if (!types.empty() && std::find(types.begin(), types.end(), s.summon_type) == types.end()) continue;
        if (dungeon_unknown) {
            ++held;
            continue;
        }
        if (n >= max) break;
        std::map<std::string, std::string> v;
        v["SummonDataId"] = jnum(static_cast<long long>(s.id));
        v["UserId"] = jnum(static_cast<long long>(s.user_id));
        v["SummonType"] = jnum(s.summon_type);
        v["AreaId"] = jnum(s.area);
        v["AreaRegionId"] = jnum(s.region);
        v["SummonData"] = jstr(s.data_b64);
        v["OnlineId"] = jstr(s.online_id);
        v["CharaId"] = jnum(s.chara_id);
        v["SummonDataVersion"] = jnum(s.version);
        if (n++) list += ",";
        list += render(formats::kSignTemplate, v);
    }
    list += "]";
    {
        // One line whenever what a member is shown changes (sign debugging).
        static std::mutex last_mu;
        static std::map<std::string, std::string> last;
        std::lock_guard<std::mutex> llk(last_mu);
        std::string key = std::to_string(n) + " sign(s) for types [";
        for (std::size_t i = 0; i < types.size(); ++i) key += (i ? "," : "") + std::to_string(types[i]);
        key += "] of " + std::to_string(board.size()) + " on the board";
        if (in_chalice) key += ", in chalice dungeon " + std::to_string(my_channel);
        if (held) key += " (" + std::to_string(held) + " held back: that dungeon is not on the channel board)";
        if (young_held) key += " (" + std::to_string(young_held) + " too new: shown " + std::to_string(min_age) + " ms after it went up)";
        if (prologue_held)
            key += " (" + std::to_string(prologue_held) +
                   (caller_prologue ? " held back: this member is in its prologue)" : " held back: its owner is in the prologue)");
        if (last[caller.online_id] != key) {
            last[caller.online_id] = key;
            log("from api: %s asks for signs: %s", caller.online_id.c_str(), key.c_str());
        }
    }
    return list;
}

void FromApi::set_min_sign_age_ms(std::int64_t ms) {
    std::lock_guard<std::mutex> lk(mu_);
    min_sign_age_ms_ = ms;
}

std::size_t FromApi::shown_signs() {
    std::lock_guard<std::mutex> lk(mu_);
    const std::int64_t now = steady_now_ms();
    std::size_t n = 0;
    for (const Sign& s : signs_) n += now - s.posted_ms >= min_sign_age_ms_ ? 1 : 0;
    return n;
}

std::int64_t FromApi::ms_since_request() {
    std::lock_guard<std::mutex> lk(mu_);
    return steady_now_ms() - last_request_ms_;
}

std::int64_t FromApi::hold_left_ms() {
    std::lock_guard<std::mutex> lk(mu_);
    const std::int64_t now = steady_now_ms();
    std::int64_t left = 0;
    for (const auto& h : holds_) left = std::max(left, h.second - now);
    return left;
}

void FromApi::set_prologue_members(const std::set<std::string>& ids) {
    std::lock_guard<std::mutex> lk(mu_);
    if (ids == prologue_) return;
    prologue_ = ids;
    std::string names;
    for (const std::string& id : ids) names += (names.empty() ? "" : ", ") + id;
    log("from api: members in their prologue (no signs shown to or from them): %s", names.empty() ? "none" : names.c_str());
}

void FromApi::hold_signs(const std::string& online_id, int ms) {
    std::lock_guard<std::mutex> lk(mu_);
    const std::int64_t until = steady_now_ms() + ms;
    for (auto& h : holds_) {
        if (h.first == online_id) {
            h.second = until;
            return;
        }
    }
    holds_.emplace_back(online_id, until);
    log("from api: %s's signs are held for %d s (the host's old connection to it must time out first)",
        online_id.c_str(), ms / 1000);
}

std::size_t FromApi::drop_signs(const std::string& online_id) {
    std::lock_guard<std::mutex> lk(mu_);
    const std::size_t before = signs_.size();
    signs_.erase(std::remove_if(signs_.begin(), signs_.end(), [&](const Sign& s) { return s.online_id == online_id; }),
                 signs_.end());
    const std::size_t n = before - signs_.size();
    if (n) log("from api: %s's link is gone: %zu sign(s) taken off the board", online_id.c_str(), n);
    return n;
}

std::size_t FromApi::renew_signs(const std::string& online_id) {
    std::lock_guard<std::mutex> lk(mu_);
    std::size_t n = 0;
    for (Sign& s : signs_) {
        if (s.online_id != online_id) continue;
        const std::uint64_t old = s.id;
        s.id = next_sign_++;
        ++n;
        log("from api: the summon of %s failed before it joined: sign %llu is now %llu (summoned again)",
            online_id.c_str(), static_cast<unsigned long long>(old), static_cast<unsigned long long>(s.id));
    }
    if (n) g_sign_changes.fetch_add(1, std::memory_order_relaxed);
    return n;
}

std::string FromApi::sign_delete(const Caller& caller, const json::Value& body) {
    const auto id = static_cast<std::uint64_t>(int_of(body, "SummonDataId", 0));
    std::lock_guard<std::mutex> lk(mu_);
    signs_.erase(std::remove_if(signs_.begin(), signs_.end(),
                                [&](const Sign& s) { return s.online_id == caller.online_id && (!id || s.id == id); }),
                 signs_.end());
    return "0";
}

std::string FromApi::sign_request(const Caller& caller, const json::Value& body) {
    const auto target_uid = static_cast<std::uint64_t>(int_of(body, "TargetUserId", 0));
    const auto sign_id = static_cast<std::uint64_t>(int_of(body, "SummonDataId", 0));
    std::string target;
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_request_ms_ = steady_now_ms();
        for (const Sign& s : signs_) {
            if ((target_uid && s.user_id == target_uid) || (sign_id && s.id == sign_id)) {
                target = s.online_id;
                break;
            }
        }
        if (target.empty() && target_uid) {
            for (const auto& [id, uid] : users_) {
                if (uid == target_uid) target = id;
            }
        }
    }
    if (target.empty() || target == caller.online_id) {
        log("from api: %s asked to summon user %llu: no such sign", caller.online_id.c_str(),
            static_cast<unsigned long long>(target_uid));
        return "0";
    }
    PartyHostService::RoomView room;
    if (!service_.room_of(caller.online_id, &room, true)) {
        log("from api: %s asked to summon %s without a room of its own", caller.online_id.c_str(), target.c_str());
        return "0";
    }
    json::Value host = json::Value::make_object();
    service_.resolve(caller.online_id, &host);
    json::Value ev = json::Value::make_object();
    ev.set("Name", "guest_invite");
    ev.set("RoomId", static_cast<long long>(room.room_id));
    ev.set("HostOnlineId", caller.online_id);
    ev.set("HostAddr", str_of(host, "Addr"));
    ev.set("HostPort", int_of(host, "Port", 0));
    ev.set("HostLocalAddr", str_of(host, "LocalAddr"));
    ev.set("HostLocalPort", int_of(host, "LocalPort", 0));
    ev.set("HostMappedAddr", str_of(host, "MappedAddr"));
    ev.set("HostMappedPort", int_of(host, "MappedPort", 0));
    ev.set("MemberTag", int_of(room.extra, "MemberTag", 1));
    ev.set("HostArea", int_of(room.extra, "HostArea", 0));
    ev.set("HostLevel", int_of(room.extra, "HostLevel", 0));
    json::Value pos = json::Value::make_array();
    const json::Value* hp = room.extra.find("HostPos");
    for (int i = 0; i < 3; ++i) {
        double v = 0;
        if (hp && hp->type == json::Value::Type::Array && static_cast<std::size_t>(i) < hp->array.size())
            v = hp->array[static_cast<std::size_t>(i)].number;
        pos.push(v);
    }
    ev.set("HostPos", std::move(pos));
    service_.push_event(target, std::move(ev));
    g_summons.fetch_add(1, std::memory_order_relaxed);
    log("from api: %s summons %s into room %llu", caller.online_id.c_str(), target.c_str(),
        static_cast<unsigned long long>(room.room_id));
    return "1";
}

}  // namespace bbnet::party

namespace bbnet {
// Changes of the board, or of how many signs on it are old enough to be shown (A4): either lets the
// host's game poll at once.
std::uint64_t sign_board_changes() {
    return party::g_sign_changes.load(std::memory_order_relaxed) * 64 + party::FromApi::instance().shown_signs();
}
std::uint64_t summon_requests() { return party::g_summons.load(std::memory_order_relaxed); }
}  // namespace bbnet
