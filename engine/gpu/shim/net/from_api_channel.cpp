// SPDX-License-Identifier: GPL-3.0-or-later
// The Chalice Dungeon channel board (from_api_channel.h).
#include "from_api_channel.h"

#include "bbnet_internal.h"
#include "party_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace bbnet::party {

namespace {

std::string jstr(const std::string& s) { return json::dump(json::Value(s), 0); }
std::string jnum(long long v) { return std::to_string(v); }

std::uint64_t fnv1a64(const std::string& s, std::uint64_t h = 0xcbf29ce484222325ull) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

// A number of a peer's body as a small integer (the game sends ids and levels as integers).
long long small(const json::Value& body, const char* key, long long def = 0) {
    const long long v = int_of(body, key, def);
    return v < -0x7fffffffLL || v > 0x7fffffffLL ? def : v;
}

bool has_number(const json::Value& body, const char* key) {
    const json::Value* m = body.type == json::Value::Type::Object ? body.find(key) : nullptr;
    return m && m->type == json::Value::Type::Number;
}

// A base64 text the game's decoder takes (standard alphabet, '=' padding), within the limit.
bool form_data_ok(const std::string& b64) {
    if (b64.empty() || b64.size() > kMaxFormDataB64) return false;
    std::vector<std::uint8_t> raw;
    return b64_decode_strict(b64, &raw) && !raw.empty();
}

// WishMaterialList as the game sends it, re-built from its four integers per entry.
json::Value wish_list(const json::Value& body) {
    json::Value out = json::Value::make_array();
    const json::Value* l = body.find("WishMaterialList");
    if (!l || l->type != json::Value::Type::Array) return out;
    for (const json::Value& e : l->array) {
        if (out.array.size() >= kMaxWishMaterials) break;
        if (e.type != json::Value::Type::Object) continue;
        json::Value w = json::Value::make_object();
        w.set("WishMaterialMainTypeId", small(e, "WishMaterialMainTypeId"));
        w.set("WishMaterialSubTypeId", small(e, "WishMaterialSubTypeId"));
        w.set("WishMaterialCount", small(e, "WishMaterialCount"));
        w.set("SpendMaterialCount", small(e, "SpendMaterialCount"));
        out.push(std::move(w));
    }
    return out;
}

// The ids of a ChannelIdList: [{"ChannelId": n}, ...] (builders 0x1e9cf60 / 0x1e9d5b0); plain
// numbers are taken too.
std::vector<std::uint64_t> id_list(const json::Value& body) {
    std::vector<std::uint64_t> ids;
    const json::Value* l = body.find("ChannelIdList");
    if (!l || l->type != json::Value::Type::Array) return ids;
    for (const json::Value& e : l->array) {
        if (ids.size() >= kMaxChannelQuery) break;
        long long v = -1;
        if (e.type == json::Value::Type::Object) v = int_of(e, "ChannelId", -1);
        else if (e.type == json::Value::Type::Number && e.number >= 0 && e.number < 9.0e15) v = static_cast<long long>(e.number);
        if (v > 0) ids.push_back(static_cast<std::uint64_t>(v));
    }
    return ids;
}

}  // namespace

std::string ChannelBoard::FixedKey(long long grail, long long level, long long rites) {
    return "fixed:" + std::to_string(grail) + ":" + std::to_string(level) + ":" + std::to_string(rites);
}

std::uint64_t ChannelBoard::IdOf(const std::string& form_data) {
    // 52 bits: exact as a JSON double; never 0 (the game's "none" is 0x8000000000000000 / 0).
    const std::uint64_t id = fnv1a64(form_data) & ((1ull << 52) - 1);
    return id ? id : 1;
}

std::string ChannelBoard::GlyphOf(const std::string& form_data) {
    static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::uint64_t h = fnv1a64(form_data, 0x84222325cbf29ce4ull);
    std::string g;
    for (int i = 0; i < 8; ++i) {
        g += kAlphabet[h % 36];
        h /= 36;
        if (i == 4) h ^= fnv1a64(form_data);  // 36^8 > 2^41: refill
    }
    return g;
}

std::string ChannelBoard::handle(const std::string& api, const std::string& caller, std::uint64_t caller_uid,
                                 const json::Value& body, const std::string& now) {
    if (api == "upload") return upload(caller, caller_uid, body, now);
    if (api == "share") return share(caller, body);
    if (api == "search") return search(caller, body);
    if (api == "word_search") return word_search(caller, body, now);
    if (api == "get_details_info") return details(caller, body);
    if (api == "get_info") return info(body);
    if (api == "random_join") return random_join(caller, body, now);
    if (api == "add_material") return R"({"ResKind":0,"CompleteFlag":0})";
    return R"({"ResKind":0})";
}

ChannelBoard::Channel* ChannelBoard::find_id(std::uint64_t id) {
    for (Channel& c : channels_) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

void ChannelBoard::drop_excess() {
    while (channels_.size() > kMaxChannels) {
        auto oldest = std::min_element(channels_.begin(), channels_.end(),
                                       [](const Channel& a, const Channel& b) { return a.touched < b.touched; });
        channels_.erase(oldest);
    }
}

std::string ChannelBoard::upload(const std::string& caller, std::uint64_t uid, const json::Value& body,
                                 const std::string& now) {
    const std::string form = str_of(body, "FormData");
    if (!form_data_ok(form) || int_of(body, "FormDataVersion", 0) != 0) {
        log("from api: %s: refused a chalice dungeon upload (%zu FormData chars)", caller.c_str(), form.size());
        return "{\"ResKind\":" + jnum(kChannelLost) + "}";
    }
    std::lock_guard<std::mutex> lk(mu_);
    // A shared fixed dungeon: one channel per (chalice, depth, rites), whatever its FormData.
    const bool fixed = small(body, "FixedOrGeneral") == 2;
    const std::string fixed_key =
        fixed ? FixedKey(small(body, "HolyGrailTypeId"), small(body, "RitualLevel"), small(body, "SubFeatureFlag")) : "";
    const std::uint64_t id = fixed ? IdOf(fixed_key) : IdOf(form);
    Channel* c = find_id(id);
    if (c && fixed) {
        c->form_data = form;
    } else if (c && c->form_data != form) {
        // A hash collision (or a forged FormData aimed at someone's id): never replace another dungeon.
        log("from api: %s: chalice dungeon id %llu collides; refused", caller.c_str(), static_cast<unsigned long long>(id));
        return "{\"ResKind\":" + jnum(kChannelLost) + "}";
    }
    if (!c) {
        std::size_t mine = 0;
        for (const Channel& o : channels_) mine += o.owner == caller ? 1 : 0;
        if (mine >= kMaxChannelsPerUser) {
            // The member's least recently used dungeon makes room.
            auto it = channels_.end();
            for (auto i = channels_.begin(); i != channels_.end(); ++i) {
                if (i->owner == caller && (it == channels_.end() || i->touched < it->touched)) it = i;
            }
            if (it != channels_.end()) channels_.erase(it);
        }
        Channel n;
        n.id = id;
        n.glyph = fixed ? GlyphOf(fixed_key) : GlyphOf(form);
        n.owner = caller;
        n.owner_uid = uid;
        n.form_data = form;
        n.created = now;
        channels_.push_back(std::move(n));
        c = &channels_.back();
        log("from api: %s uploaded chalice dungeon %llu, glyph %s (chalice %lld, depth %lld, %s)", caller.c_str(),
            static_cast<unsigned long long>(id), c->glyph.c_str(), small(body, "HolyGrailTypeId"),
            small(body, "RitualLevel"), small(body, "FixedOrGeneral") ? "fixed" : "root");
    }
    // The dungeon's description follows its latest upload (the same FormData: the same dungeon).
    c->chara_id = int_of(body, "CharaId", c->chara_id);
    c->holy_grail = small(body, "HolyGrailTypeId", c->holy_grail);
    c->ritual_level = small(body, "RitualLevel", c->ritual_level);
    c->fixed_or_general = small(body, "FixedOrGeneral", c->fixed_or_general);
    c->sub_feature = small(body, "SubFeatureFlag", c->sub_feature);
    c->status = small(body, "Status", c->status);
    c->share_level = small(body, "ShareLevel", c->share_level);
    c->wish_materials = wish_list(body);
    c->last_play = now;
    c->touched = ++clock_;
    made_[caller] = {c->id, std::chrono::steady_clock::now()};
    const std::string reply =
        "{\"ResKind\":0,\"ChannelId\":" + jnum(static_cast<long long>(c->id)) + ",\"DiscernmentWord\":" + jstr(c->glyph) + "}";
    drop_excess();
    return reply;
}

bool ChannelBoard::latest_ritual(const std::string& except, std::chrono::seconds within, Channel* out,
                                 std::string* member) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = std::chrono::steady_clock::now();
    const std::pair<const std::string, std::pair<std::uint64_t, std::chrono::steady_clock::time_point>>* best = nullptr;
    for (const auto& m : made_) {
        if (m.first == except || now - m.second.second > within) continue;
        if (!best || m.second.second > best->second.second) best = &m;
    }
    if (!best) return false;
    for (const Channel& c : channels_) {
        if (c.id == best->second.first) {
            if (out) *out = c;
            if (member) *member = best->first;
            return true;
        }
    }
    return false;
}

std::string ChannelBoard::share(const std::string& caller, const json::Value& body) {
    std::lock_guard<std::mutex> lk(mu_);
    const long long id = int_of(body, "ChannelId", 0);
    Channel* c = id > 0 ? find_id(static_cast<std::uint64_t>(id)) : nullptr;
    if (!c) return "{\"ResKind\":" + jnum(kChannelLost) + "}";
    // The share level is the uploader's setting; another member's game cannot change it.
    if (c->owner == caller) c->share_level = small(body, "ShareLevel", c->share_level);
    return R"({"ResKind":0})";
}

std::string ChannelBoard::entry(const Channel& c, bool root, long long share_level) const {
    std::string e = root ? "{\"ResKind\":0," : "{";
    e += "\"ChannelId\":" + jnum(static_cast<long long>(c.id));
    e += ",\"ShareLevel\":" + jnum(share_level >= 0 ? share_level : c.share_level);
    e += ",\"Status\":" + jnum(c.status);
    e += ",\"FormDataVersion\":0";
    e += ",\"FormData\":" + jstr(c.form_data);
    e += ",\"DiscernmentWord\":" + jstr(c.glyph);
    e += ",\"RitualLevel\":" + jnum(c.ritual_level);
    e += ",\"HolyGrailTypeId\":" + jnum(c.holy_grail);
    e += ",\"FixedOrGeneral\":" + jnum(c.fixed_or_general);
    e += ",\"SubFeatureFlag\":" + jnum(c.sub_feature);
    e += ",\"CreateUserId\":" + jnum(static_cast<long long>(c.owner_uid));
    e += ",\"CreateCharaId\":" + jnum(c.chara_id);
    e += ",\"CreateDate\":" + jstr(c.created);
    e += ",\"LastPlayDate\":" + jstr(c.last_play);
    e += ",\"TurnoutLevel\":" + jnum(c.turnout);
    e += ",\"WishMaterialList\":" + json::dump(c.wish_materials, 0);
    e += "}";
    return e;
}

std::string ChannelBoard::search(const std::string& caller, const json::Value& body) {
    // Search criteria (builder 0x1e9ee50): HolyGrailTypeId / RitualLevel / SubFeatureFlag are
    // null for "any"; FixedOrGeneralList [{FixedOrGeneral: n}] lists the kinds asked for.
    std::vector<long long> kinds;
    if (const json::Value* l = body.find("FixedOrGeneralList"); l && l->type == json::Value::Type::Array) {
        for (const json::Value& e : l->array) {
            if (kinds.size() >= kMaxChannelQuery) break;
            if (e.type == json::Value::Type::Object && has_number(e, "FixedOrGeneral")) kinds.push_back(small(e, "FixedOrGeneral"));
        }
    }
    const bool by_grail = has_number(body, "HolyGrailTypeId");
    const bool by_level = has_number(body, "RitualLevel");
    const bool by_rites = has_number(body, "SubFeatureFlag");
    long long max = small(body, "GetCount", 20);
    if (max <= 0 || max > static_cast<long long>(kMaxChannelQuery)) max = kMaxChannelQuery;
    // A Shared Fixed Dungeon ritual (FixedOrGeneralList [{2}], GetCount 1, every criterion set):
    // the game joins the one shared channel of that fixed dungeon and treats an empty answer as a
    // server failure (reply handler 0x1e8711c: result 0x80000005 -> drop offline, msg 0x11f9 "Failed
    // to connect to Bloodborne server", seen live). The fixed layout comes from the chalice's params,
    // so the board makes that channel on first use, keyed by (chalice, depth, rites), the caller's too.
    const bool shared_fixed = std::find(kinds.begin(), kinds.end(), 2) != kinds.end() && by_grail && by_level &&
                              by_rites;
    std::lock_guard<std::mutex> lk(mu_);
    if (shared_fixed) {
        const std::string key = FixedKey(small(body, "HolyGrailTypeId"), small(body, "RitualLevel"),
                                         small(body, "SubFeatureFlag"));
        Channel* c = find_id(IdOf(key));
        if (!c) {
            Channel n;
            n.id = IdOf(key);
            n.glyph = GlyphOf(key);
            n.owner = caller;
            n.holy_grail = small(body, "HolyGrailTypeId");
            n.ritual_level = small(body, "RitualLevel");
            n.sub_feature = small(body, "SubFeatureFlag");
            n.fixed_or_general = 2;
            n.status = small(body, "Status", 1);
            n.touched = ++clock_;
            channels_.push_back(std::move(n));
            c = &channels_.back();
            log("from api: %s: shared fixed chalice dungeon %llu (glyph %s, chalice %lld, depth %lld, rites %lld) "
                "made on first use", caller.c_str(), static_cast<unsigned long long>(c->id), c->glyph.c_str(),
                c->holy_grail, c->ritual_level, c->sub_feature);
            drop_excess();
            c = find_id(IdOf(key));
        }
        if (c) {
            c->touched = ++clock_;
            ritual_[caller] = {c->id, std::chrono::steady_clock::now()};
            made_[caller] = {c->id, std::chrono::steady_clock::now()};
            return "{\"ResKind\":0,\"ChannelList\":[" + entry(*c, false) + "]}";
        }
    }
    std::vector<const Channel*> hits;
    for (const Channel& c : channels_) {
        if (c.owner == caller) continue;
        if (by_grail && c.holy_grail != small(body, "HolyGrailTypeId")) continue;
        if (by_level && c.ritual_level != small(body, "RitualLevel")) continue;
        if (by_rites && c.sub_feature != small(body, "SubFeatureFlag")) continue;
        if (!kinds.empty() && std::find(kinds.begin(), kinds.end(), c.fixed_or_general) == kinds.end()) continue;
        hits.push_back(&c);
    }
    std::sort(hits.begin(), hits.end(), [](const Channel* a, const Channel* b) { return a->touched > b->touched; });
    std::string list = "[";
    for (std::size_t i = 0; i < hits.size() && static_cast<long long>(i) < max; ++i) {
        if (i) list += ",";
        list += entry(*hits[i], false);
    }
    return "{\"ResKind\":0,\"ChannelList\":" + list + "]}";
}

std::string ChannelBoard::word_search(const std::string& caller, const json::Value& body, const std::string& now) {
    std::string word = str_of(body, "SearchWord");
    if (word.size() > 32) word.clear();
    for (char& ch : word) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::lock_guard<std::mutex> lk(mu_);
    for (Channel& c : channels_) {
        if (!word.empty() && c.glyph == word) {
            c.touched = ++clock_;
            c.last_play = now;
            log("from api: %s found chalice dungeon %llu by its glyph %s", caller.c_str(),
                static_cast<unsigned long long>(c.id), word.c_str());
            // As in details(): a ShareLevel makes the reader's game take the Status, which the join
            // needs (+0x54 == 1). With the stored 0 of a shared fixed dungeon made by a member's
            // ritual search, "Join the selected Chalice Dungeon?" -> YES did nothing (run c19).
            return entry(c, true, c.share_level ? c.share_level : 1);
        }
    }
    log("from api: %s: no chalice dungeon with the glyph '%s'", caller.c_str(), word.c_str());
    return "{\"ResKind\":" + jnum(kChannelLost) + "}";
}

std::string ChannelBoard::details(const std::string& caller, const json::Value& body) {
    const std::vector<std::uint64_t> ids = id_list(body);
    std::lock_guard<std::mutex> lk(mu_);
    // A reader's game copies an entry's Status into its channel object (+0x54) only with a
    // ShareLevel (0x1e6dea2); the ritual helper's StateSwitch (0x1aaa3d0) needs +0x54 == 1 for the
    // joined state 5, else a summoned member's quick join ends in state 3 and 0x1ac3ef0 cancels it
    // (live, run #11). The member performing the ritual of that dungeon (its fixed search within
    // kRitualWindow) gets the stored ShareLevel (0): with 1 its ritual took the local path (run #12).
    const auto now = std::chrono::steady_clock::now();
    auto in_ritual = [&](std::uint64_t id) {
        auto it = ritual_.find(caller);
        return it != ritual_.end() && it->second.first == id && now - it->second.second < kRitualWindow;
    };
    std::string list = "[";
    bool first = true;
    for (std::uint64_t id : ids) {
        if (const Channel* c = find_id(id)) {
            if (!first) list += ",";
            first = false;
            list += entry(*c, false, in_ritual(id) || c->share_level ? c->share_level : 1);
        }
    }
    return "{\"ResKind\":0,\"ChannelList\":" + list + "]}";
}

std::string ChannelBoard::info(const json::Value& body) {
    // Unknown ids are left out, not reported lost: the board lives in the host's process, and a
    // "lost" answer would make a member's game drop a dungeon it uploaded in an earlier session.
    const std::vector<std::uint64_t> ids = id_list(body);
    std::lock_guard<std::mutex> lk(mu_);
    std::string list = "[";
    bool first = true;
    for (std::uint64_t id : ids) {
        if (const Channel* c = find_id(id)) {
            if (!first) list += ",";
            first = false;
            list += "{\"ChannelId\":" + jnum(static_cast<long long>(c->id)) + ",\"Status\":" + jnum(c->status) +
                    ",\"ShareLevel\":" + jnum(c->share_level) + ",\"TurnoutLevel\":" + jnum(c->turnout) + "}";
        }
    }
    return "{\"ResKind\":0,\"ChannelInfoList\":" + list + "]}";
}

std::string ChannelBoard::random_join(const std::string& caller, const json::Value& body, const std::string& now) {
    (void)body;  // RandomJoinTargetList (chalice family / depth limits): the party joins its own dungeons
    std::lock_guard<std::mutex> lk(mu_);
    // The dungeon another member plays now (newest report first), else the newest upload.
    Channel* best = nullptr;
    std::uint64_t best_clock = 0;
    for (const auto& [member, mc] : member_channel_) {
        if (member == caller) continue;
        Channel* c = find_id(mc.first);
        if (c && mc.second >= best_clock) {
            best = c;
            best_clock = mc.second;
        }
    }
    if (!best) {
        for (Channel& c : channels_) {
            if (c.owner != caller && (!best || c.touched > best->touched)) best = &c;
        }
    }
    if (!best) {
        log("from api: %s: random join found no party chalice dungeon", caller.c_str());
        return "{\"ResKind\":" + jnum(kChannelLost) + "}";
    }
    best->touched = ++clock_;
    best->last_play = now;
    log("from api: %s joins chalice dungeon %llu (glyph %s)", caller.c_str(), static_cast<unsigned long long>(best->id),
        best->glyph.c_str());
    return entry(*best, true);
}

void ChannelBoard::note_member_channel(const std::string& member, std::uint64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [m, mc] : member_channel_) {
        if (m == member) {
            if (mc.first != id) mc = {id, ++clock_};
            return;
        }
    }
    if (member_channel_.size() < 64) member_channel_.push_back({member, {id, ++clock_}});
}

std::uint64_t ChannelBoard::member_channel(const std::string& member) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [m, mc] : member_channel_) {
        if (m == member) return mc.first;
    }
    return 0;
}

bool ChannelBoard::set_form_data(std::uint64_t id, const std::string& b64) {
    if (!form_data_ok(b64)) return false;
    std::lock_guard<std::mutex> lk(mu_);
    Channel* c = find_id(id);
    if (!c) return false;
    c->form_data = b64;
    return true;
}

bool ChannelBoard::adopt(std::uint64_t id, const std::string& b64, const std::string& owner) {
    if (!id || !form_data_ok(b64)) return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (find_id(id)) return false;
    Channel n;
    n.id = id;
    n.glyph = GlyphOf(b64);
    n.owner = owner;
    n.form_data = b64;
    n.status = 1;
    n.touched = ++clock_;
    channels_.push_back(std::move(n));
    log("from api: %s's game is in chalice dungeon %llu (not on the board yet): added, glyph %s", owner.c_str(),
        static_cast<unsigned long long>(id), channels_.back().glyph.c_str());
    drop_excess();
    return true;
}

void ChannelBoard::note_made(const std::string& member, std::uint64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = made_.find(member);
    if (it != made_.end() && it->second.first == id) return;  // keeps the time of the ritual
    made_[member] = {id, std::chrono::steady_clock::now()};
}

std::string ChannelBoard::form_data(std::uint64_t id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const Channel& c : channels_) {
        if (c.id == id) return c.form_data;
    }
    return {};
}

bool ChannelBoard::known(std::uint64_t id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const Channel& c : channels_) {
        if (c.id == id) return true;
    }
    return false;
}

std::vector<ChannelBoard::Channel> ChannelBoard::channels() const {
    std::lock_guard<std::mutex> lk(mu_);
    return channels_;
}

}  // namespace bbnet::party
