// SPDX-License-Identifier: GPL-3.0-or-later
// Party text chat, the pure part (party_chat.h): sanitising, EVENT party_chat JSON, the rate limits, word masking, the
// line log. No game dependency (tests/test_party_chat.cpp, tests/party_fuzz.cpp). The game side: party_chat_game.cpp.
#include "party_chat.h"

#include "net/json.h"
#include "party/name_filter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>

namespace party::chat {

namespace {

const char* const kPresets[kPresetCount] = {"Help!", "Follow me", "Wait", "Boss here", "Thanks", "Going to the Dream"};

/// Strict UTF-8: false on a malformed, overlong, surrogate or out-of-range sequence.
bool NextCp(const std::string& s, std::size_t* i, std::uint32_t* cp) {
    const std::size_t n = s.size();
    const std::size_t p = *i;
    const unsigned c = static_cast<unsigned char>(s[p]);
    if (c < 0x80) {
        *cp = c;
        *i = p + 1;
        return true;
    }
    std::size_t extra;
    std::uint32_t v, min;
    if (c >= 0xc2 && c <= 0xdf) extra = 1, v = c & 0x1f, min = 0x80;
    else if (c >= 0xe0 && c <= 0xef) extra = 2, v = c & 0x0f, min = 0x800;
    else if (c >= 0xf0 && c <= 0xf4) extra = 3, v = c & 0x07, min = 0x10000;
    else return false;
    if (p + extra >= n) return false;
    for (std::size_t k = 1; k <= extra; ++k) {
        const unsigned d = static_cast<unsigned char>(s[p + k]);
        if ((d & 0xc0) != 0x80) return false;
        v = (v << 6) | (d & 0x3f);
    }
    if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return false;
    *cp = v;
    *i = p + extra + 1;
    return true;
}

void Append(std::string* o, std::uint32_t cp) {
    if (cp < 0x80) {
        *o += static_cast<char>(cp);
    } else if (cp < 0x800) {
        *o += static_cast<char>(0xc0 | (cp >> 6));
        *o += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        *o += static_cast<char>(0xe0 | (cp >> 12));
        *o += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        *o += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        *o += static_cast<char>(0xf0 | (cp >> 18));
        *o += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        *o += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        *o += static_cast<char>(0x80 | (cp & 0x3f));
    }
}

/// Spaces of any kind (collapsed to one plain space).
bool IsSpace(std::uint32_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
           c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

/// Code points that are dropped: controls, soft hyphen, zero width / bidi / invisible formatting, combining marks
/// (zalgo), variation selectors, private use, tags, specials.
bool Dropped(std::uint32_t c) {
    if (c < 0x20 || (c >= 0x7f && c <= 0x9f)) return true;
    if (c == 0xad || c == 0x34f || c == 0x61c || c == 0x115f || c == 0x1160 || c == 0x17b4 || c == 0x17b5) return true;
    if ((c >= 0x200b && c <= 0x200f) || (c >= 0x202a && c <= 0x202e) || (c >= 0x2060 && c <= 0x206f)) return true;
    if (c == 0xfeff || c == 0x3164 || c == 0xffa0) return true;
    if ((c >= 0xe000 && c <= 0xf8ff) || (c >= 0xfe00 && c <= 0xfe0f) || (c >= 0xfff0 && c <= 0xffff)) return true;
    if (c >= 0xe0000) return true;
    if ((c >= 0x300 && c <= 0x36f) || (c >= 0x1ab0 && c <= 0x1aff) || (c >= 0x1dc0 && c <= 0x1dff) ||
        (c >= 0x20d0 && c <= 0x20ff) || (c >= 0xfe20 && c <= 0xfe2f))
        return true;
    return false;
}

bool ValidName(const std::string& s) {
    if (s.empty() || s.size() > 16) return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    }
    return true;
}

std::mutex g_mu;
std::vector<Line> g_log;
std::uint32_t g_seq = 0;

bool AsciiPunct(char c) {
    return c > ' ' && c < 0x7f && !std::isalnum(static_cast<unsigned char>(c));
}

struct Token {
    std::string text;        // the whole space-delimited token
    std::size_t core_begin;  // the part between leading / trailing ASCII punctuation
    std::size_t core_end;
    bool masked = false;
};

}  // namespace

const char* Preset(int i) {
    return i >= 0 && i < kPresetCount ? kPresets[i] : "";
}

std::vector<std::string> PresetList() {
    return std::vector<std::string>(kPresets, kPresets + kPresetCount);
}

std::size_t CharCount(const std::string& utf8) {
    std::size_t n = 0, i = 0;
    std::uint32_t cp;
    while (i < utf8.size()) {
        if (!NextCp(utf8, &i, &cp)) break;
        ++n;
    }
    return n;
}

std::string Sanitise(const std::string& utf8, int max_chars) {
    std::string out;
    std::size_t chars = 0, i = 0;
    bool space = false;
    if (max_chars < 0) max_chars = 0;
    while (i < utf8.size()) {
        std::uint32_t cp;
        if (!NextCp(utf8, &i, &cp)) return "";
        if (IsSpace(cp)) {
            space = !out.empty();
            continue;
        }
        if (Dropped(cp)) continue;
        std::string one;
        Append(&one, cp);
        const std::size_t extra = space ? 1 : 0;
        if (chars + 1 + extra > std::size_t(max_chars) || out.size() + one.size() + extra > kMaxBytes) break;
        if (space) out += ' ';
        space = false;
        out += one;
        chars += 1 + extra;
    }
    return out;
}

std::string ToJson(const ChatEvent& e) {
    json::Value v = json::Value::make_object();
    v.set("text", e.text);
    v.set("t", static_cast<long long>(e.t));
    if (e.slot >= 0) v.set("slot", e.slot);
    if (!e.who.empty()) v.set("who", e.who);
    return json::dump(v, 0);
}

bool FromJson(const std::string& text, ChatEvent* out, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = std::string("party_chat: ") + why;
        return false;
    };
    if (text.size() > 1024) return fail("too long");
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) return fail("not JSON");
    if (v.type != json::Value::Type::Object) return fail("not an object");
    ChatEvent e;
    const json::Value* t = v.find("text");
    if (!t || t->type != json::Value::Type::String) return fail("no text");
    e.text = Sanitise(t->string);
    if (e.text.empty()) return fail("empty text");
    if (const json::Value* x = v.find("t")) {
        if (x->type != json::Value::Type::Number || !std::isfinite(x->number) || x->number < 0 || x->number > 9.0e15)
            return fail("bad time");
        e.t = static_cast<std::int64_t>(x->number);
    }
    if (const json::Value* x = v.find("slot")) {
        if (x->type != json::Value::Type::Number || !std::isfinite(x->number) || x->number < -1 ||
            x->number > kMaxSlots - 1 || x->number != std::floor(x->number))
            return fail("bad slot");
        e.slot = static_cast<int>(x->number);
    }
    if (const json::Value* w = v.find("who")) {
        if (w->type != json::Value::Type::String || !ValidName(w->string)) return fail("bad name");
        e.who = w->string;
    }
    *out = e;
    return true;
}

Rate::Rate(int burst, double window_s) : burst_(burst), window_s_(window_s) {
    for (double& t : tokens_) t = burst;
}

bool Rate::Take(int slot, double now_s) {
    if (slot < 0 || slot >= kMaxSlots || !std::isfinite(now_s)) return false;
    const double rate = burst_ / window_s_;
    if (last_[slot] != 0.0 && now_s > last_[slot]) {
        tokens_[slot] = std::min<double>(burst_, tokens_[slot] + (now_s - last_[slot]) * rate);
    }
    last_[slot] = now_s;
    if (tokens_[slot] < 1.0) return false;
    tokens_[slot] -= 1.0;
    return true;
}

bool SendGap::Take(std::int64_t now_ms) {
    if (now_ms - last_ < kSendGapMs) return false;
    last_ = now_ms;
    return true;
}

std::string MaskWords(const std::string& text) {
    namespace nf = coop::namefilter;
    std::vector<Token> toks;
    {
        std::size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && text[i] == ' ') ++i;
            if (i >= text.size()) break;
            std::size_t j = text.find(' ', i);
            if (j == std::string::npos) j = text.size();
            Token t;
            t.text = text.substr(i, j - i);
            t.core_begin = 0;
            t.core_end = t.text.size();
            while (t.core_begin < t.core_end && AsciiPunct(t.text[t.core_begin])) ++t.core_begin;
            while (t.core_end > t.core_begin && AsciiPunct(t.text[t.core_end - 1])) --t.core_end;
            toks.push_back(std::move(t));
            i = j;
        }
    }
    if (toks.empty()) return text;
    const nf::NameFilter& f = nf::Default();
    bool any = false;
    const auto core = [&](const Token& t) { return t.text.substr(t.core_begin, t.core_end - t.core_begin); };
    for (Token& t : toks) {
        const std::string c = core(t);
        if (!c.empty() && f.Offensive(c)) t.masked = any = true;
    }
    for (std::size_t n = 2; n <= 4; ++n) {  // runs of words (phrases, letters spaced out)
        for (std::size_t s = 0; s + n <= toks.size(); ++s) {
            std::string joined;
            bool clean = true;  // a run holding a word already masked is no new find (its neighbours stay)
            for (std::size_t k = s; k < s + n; ++k) {
                if (toks[k].masked) clean = false;
                if (!joined.empty()) joined += ' ';
                joined += core(toks[k]);
            }
            if (!clean || !f.Offensive(joined)) continue;
            for (std::size_t k = s; k < s + n; ++k) toks[k].masked = true;
            any = true;
        }
    }
    if (!any && f.Offensive(text)) {  // only the whole line is caught (a code, a long join): all of it goes
        for (Token& t : toks) t.masked = true;
        any = true;
    }
    if (!any) return text;
    std::string out;
    for (std::size_t k = 0; k < toks.size(); ++k) {
        if (k) out += ' ';
        const Token& t = toks[k];
        if (!t.masked) {
            out += t.text;
            continue;
        }
        out += t.text.substr(0, t.core_begin);
        out += std::string(CharCount(core(t)), '*');
        out += t.text.substr(t.core_end);
    }
    return out;
}

void Push(const Line& l) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_log.push_back(l);
    g_log.back().name[kNameBytes - 1] = 0;
    g_log.back().seq = ++g_seq;
    while (g_log.size() > std::size_t(kLogLines)) g_log.erase(g_log.begin());
}

void ClearAll() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_log.clear();
}

std::vector<Line> Read(std::int64_t now_ms) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_log.erase(std::remove_if(g_log.begin(), g_log.end(), [&](const Line& l) { return l.until_ms <= now_ms; }),
                g_log.end());
    return g_log;
}

float Alpha(std::int64_t left_ms) {
    if (left_ms <= 0) return 0.0f;
    if (left_ms >= kFadeMs) return 1.0f;
    return float(left_ms) / float(kFadeMs);
}

std::int64_t LifeLeft(std::int64_t t, std::int64_t now) {
    if (t <= 0 || now <= 0) return kLifeMs;
    const std::int64_t age = now - t;
    if (age < -2000 || age >= kLifeMs) return 0;
    return std::min(kLifeMs, kLifeMs - std::max<std::int64_t>(age, 0));
}

}  // namespace party::chat
