// See name_filter.h. Keep in step with scripts/namefilter.py (tests/name_filter_vectors.txt runs both).
#include "party/name_filter.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "party/name_filter_data.inc"

namespace coop::namefilter {
namespace {

constexpr size_t kMaxIn = 80;
constexpr size_t kMaxOut = 128;
constexpr size_t kMaxWord = 40;
enum Cls { kLink = 1, kPeople = 2, kViolA = 3, kViolH = 4, kIdW = 5, kIdS = 6 };

std::u32string Utf8ToU32(const std::string& s) {
    std::u32string out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || (n > 0 && i + static_cast<size_t>(n) >= s.size())) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        char32_t cp = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
        bool ok = true;
        for (int k = 1; k <= n; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok || (n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += n + 1;
    }
    return out;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && IsSpace(s[a])) ++a;
    while (b > a && IsSpace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Non-empty, non-comment lines, trimmed.
std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string ln;
    while (std::getline(in, ln)) {
        ln = Trim(ln);
        if (!ln.empty() && ln[0] != '#') out.push_back(ln);
    }
    return out;
}

std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && IsSpace(s[i])) ++i;
        size_t j = i;
        while (j < s.size() && !IsSpace(s[j])) ++j;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::u32string Collapse(const std::u32string& s) {
    std::u32string out;
    int run = 0;
    for (char32_t ch : s) {
        if (!out.empty() && out.back() == ch) {
            ++run;
            if (ch == 'a' || ch == 'e' || ch == 'i' || ch == 'o' || ch == 'u' || run >= 2) continue;
        } else {
            run = 0;
        }
        out.push_back(ch);
    }
    return out;
}

std::u32string Collapse1(const std::u32string& s) {
    std::u32string out;
    for (char32_t ch : s) {
        if (out.empty() || out.back() != ch) out.push_back(ch);
    }
    return out;
}

std::u32string Cap3(const std::u32string& s) {
    std::u32string out;
    int run = 0;
    for (char32_t ch : s) {
        if (!out.empty() && out.back() == ch) {
            ++run;
            if (run >= 3) continue;
        } else {
            run = 0;
        }
        out.push_back(ch);
    }
    return out;
}

bool Contains(const std::u32string& hay, const std::u32string& needle) { return hay.find(needle) != std::u32string::npos; }

std::vector<std::u32string> SplitOn(const std::u32string& s, char32_t sep) {
    std::vector<std::u32string> out;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(sep, a);
        if (b == std::u32string::npos) {
            out.push_back(s.substr(a));
            break;
        }
        out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

std::u32string ReplaceAll(std::u32string w, const std::u32string& f) {
    if (f.empty()) return w;
    std::u32string out;
    size_t i = 0;
    for (;;) {
        size_t p = w.find(f, i);
        if (p == std::u32string::npos) {
            out.append(w, i, std::u32string::npos);
            break;
        }
        out.append(w, i, p - i);
        out.push_back('#');
        i = p + f.size();
    }
    return out;
}

char32_t Leet(char32_t ch, int mode) {  // mode 0 none, 1 i-variant, 2 l-variant
    if (mode == 0) return ch;
    switch (ch) {
    case '0': return 'o';
    case '1': return mode == 1 ? 'i' : 'l';
    case '3': return 'e';
    case '4': return 'a';
    case '5': return 's';
    case '7': return 't';
    case '@': return 'a';
    case '$': return 's';
    case '!': return 'i';
    case '+': return 't';
    case '(': return 'c';
    case '|': return mode == 1 ? 'i' : 'l';
    default: return ch;
    }
}

std::vector<std::u32string> Words(const std::u32string& cps, int leet) {
    std::vector<std::u32string> words;
    std::u32string cur;
    for (char32_t cp : cps) {
        char32_t ch = Leet(cp, leet);
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || cp >= 0x80) {
            cur.push_back(ch);
        } else if (!cur.empty()) {
            words.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) words.push_back(cur);
    std::vector<std::u32string> merged;
    size_t i = 0;
    while (i < words.size()) {
        size_t j = i;
        while (j < words.size() && words[j].size() == 1 && words[j][0] < 0x80) ++j;
        if (j - i >= 3) {
            std::u32string m;
            for (size_t k = i; k < j; ++k) m += words[k];
            merged.push_back(m);
            i = j;
        } else {
            merged.push_back(words[i]);
            ++i;
        }
    }
    return merged;
}

bool ReadDirFiles(const std::filesystem::path& dir, std::map<std::string, std::string>* out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(dir / "strict.txt", ec) || !fs::exists(dir / "charmap.tsv", ec)) return false;
    std::map<std::string, std::string> tmp;
    for (const char* f : kFiles) {
        std::ifstream in(dir / f, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        tmp[f] = ss.str();
    }
    *out = std::move(tmp);
    return true;
}

std::map<std::string, std::string> LoadTexts() {
    namespace fs = std::filesystem;
    std::map<std::string, std::string> t;
    if (const char* e = std::getenv("BB_NAME_FILTER_DIR")) {
        if (*e && ReadDirFiles(fs::path(e), &t)) return t;
    }
    if (ReadDirFiles(fs::path("name_filter"), &t)) return t;
    if (ReadDirFiles(fs::path("data") / "name_filter", &t)) return t;
    return {{"charmap.tsv", kNfCharmap}, {"strict.txt", kNfStrict}, {"identity.txt", kNfIdentity},
            {"people.txt", kNfPeople},   {"violence.txt", kNfViolence}, {"link.txt", kNfLink},
            {"codes.txt", kNfCodes},     {"allow.txt", kNfAllow}};
}

}  // namespace

NameFilter::NameFilter(const std::map<std::string, std::string>& texts) {
    for (const std::string& ln : Lines(texts.at("charmap.tsv"))) {
        size_t tab = ln.find('\t');
        if (tab == std::string::npos) continue;
        std::string key = ln.substr(0, tab), val = ln.substr(tab + 1);
        size_t dash = key.find('-');
        char32_t lo = static_cast<char32_t>(std::stoul(key.substr(0, dash), nullptr, 16));
        char32_t hi = dash == std::string::npos ? lo : static_cast<char32_t>(std::stoul(key.substr(dash + 1), nullptr, 16));
        if (val == "drop" || val == "sep") {
            for (char32_t cp = lo; cp <= hi; ++cp) cm_[cp] = {val == "drop" ? 1 : 2, U""};
        } else {
            cm_[lo] = {val[0] == '^' ? 3 : 0, Utf8ToU32(val.substr(1))};
        }
    }
    std::set<std::u32string> ts[3];
    for (const std::string& ln : Lines(texts.at("strict.txt"))) {
        std::vector<std::string> p = Split(ln);
        const std::string flag = p.size() > 1 ? p[1] : "t";
        const int form = flag.find('x') != std::string::npos ? 2 : 1;
        auto fold = [form](const std::u32string& s) { return form == 2 ? Collapse(s) : Collapse1(s); };
        std::u32string t = fold(Term(Utf8ToU32(p[0]), true));
        if (t.empty()) continue;
        terms_[form].insert(t);
        if (t.back() != 's') terms_[form].insert(fold(t + U"s"));
        if (flag.find('s') != std::string::npos) ts[form].insert(t);
    }
    for (int form = 1; form <= 2; ++form) terms_s_[form].assign(ts[form].begin(), ts[form].end());
    auto put = [&](const std::u32string& w, int cls) {
        if (w.empty()) return;
        auto it = k_.find(w);
        if (it == k_.end() || it->second < cls) k_[w] = cls;
    };
    for (const std::string& ln : Lines(texts.at("identity.txt"))) {
        std::vector<std::string> p = Split(ln);
        std::u32string w = Term(Utf8ToU32(p[0]));
        put(w, p[1] == "S" ? kIdS : kIdW);
        put(Collapse(w + U"s"), kIdS);
    }
    for (const std::string& ln : Lines(texts.at("violence.txt"))) {
        std::vector<std::string> p = Split(ln);
        put(Term(Utf8ToU32(p[0])), p[1] == "H" ? kViolH : kViolA);
    }
    for (const std::string& ln : Lines(texts.at("people.txt"))) put(Term(Utf8ToU32(Split(ln)[0])), kPeople);
    for (const std::string& ln : Lines(texts.at("link.txt"))) put(Term(Utf8ToU32(Split(ln)[0])), kLink);
    for (const auto& kv : k_) k_max_ = std::max(k_max_, kv.first.size());
    for (const std::string& ln : Lines(texts.at("codes.txt"))) {
        std::vector<std::string> p = Split(ln);
        const std::string& kind = p[0];
        if (kind == "join") c_join_.push_back(Term(Utf8ToU32(p[1])));
        else if (kind == "raw") c_raw_.insert(Cap3(Term(Utf8ToU32(p[1]), true)));
        else if (kind == "rawjoin") c_rawjoin_.push_back(Cap3(Term(Utf8ToU32(p[1]), true)));
        else if (kind == "ctx") {
            size_t bar = std::find(p.begin(), p.end(), "|") - p.begin();
            std::vector<std::u32string> others;
            for (size_t i = bar + 1; i < p.size(); ++i) others.push_back(Cap3(Term(Utf8ToU32(p[i]), true)));
            c_ctx_.emplace_back(Cap3(Term(Utf8ToU32(p[1]), true)), others);
        } else if (kind == "sym") c_sym_.insert(static_cast<char32_t>(std::stoul(p[1], nullptr, 16)));
        else if (kind == "sym2")
            c_sym2_.emplace_back(static_cast<char32_t>(std::stoul(p[1], nullptr, 16)),
                                 static_cast<char32_t>(std::stoul(p[2], nullptr, 16)));
    }
    for (const std::string& ln : Lines(texts.at("allow.txt"))) {
        std::vector<std::string> p = Split(ln);
        for (int form = 1; form <= 2; ++form) {
            const std::u32string raw = Term(Utf8ToU32(p[0]), true);
            const std::u32string x = form == 2 ? Collapse(raw) : Collapse1(raw);
            if (p[1] == "w") allow_w_[form].insert(x);
            else allow_c_[form].push_back(x);
        }
    }
    for (int form = 1; form <= 2; ++form) {
        std::sort(allow_c_[form].begin(), allow_c_[form].end(), [](const std::u32string& a, const std::u32string& b) {
            return a.size() != b.size() ? a.size() > b.size() : a < b;
        });
    }
}

void NameFilter::Map(const std::u32string& in, std::u32string* out, int* up) const {
    out->clear();
    *up = 0;
    for (size_t i = 0; i < in.size() && i < kMaxIn; ++i) {
        char32_t cp = in[i];
        if (cp < 0x80) {
            if (cp >= 'A' && cp <= 'Z') cp += 32;
            out->push_back(cp);
        } else {
            auto it = cm_.find(cp);
            if (it == cm_.end()) out->push_back(cp);
            else if (it->second.kind == 2) out->push_back(' ');
            else if (it->second.kind != 1) {
                out->append(it->second.text);
                if (it->second.kind == 3) ++*up;
            }
        }
        if (out->size() >= kMaxOut) break;
    }
    if (out->size() > kMaxOut) out->resize(kMaxOut);
}

std::u32string NameFilter::Term(const std::u32string& text, bool digits) const {
    std::u32string mapped, s;
    int up;
    Map(text, &mapped, &up);
    for (char32_t c : mapped) {
        if (c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) s.push_back(c);
    }
    return digits ? s : Collapse(s);
}

std::u32string NameFilter::Blank(const std::u32string& w, int form) const {
    if (allow_w_[form].count(w)) return U"#";
    std::u32string r = w;
    for (const std::u32string& f : allow_c_[form]) {
        if (Contains(r, f)) r = ReplaceAll(r, f);
    }
    return r;
}

bool NameFilter::Dp(const std::u32string& tok, std::vector<int>* out) const {
    const size_t n = tok.size();
    const int inf = 1 << 30;
    std::vector<int> best(n + 1, inf);
    best[n] = 0;
    for (size_t i = n; i-- > 0;) {
        for (size_t len = 1; len <= std::min(k_max_, n - i); ++len) {
            if (best[i + len] + 1 < best[i] && k_.count(tok.substr(i, len))) best[i] = best[i + len] + 1;
        }
    }
    if (best[0] >= inf) return false;
    out->clear();
    size_t i = 0;
    while (i < n) {
        for (size_t len = std::min(k_max_, n - i); len > 0; --len) {
            auto it = k_.find(tok.substr(i, len));
            if (it != k_.end() && best[i + len] == best[i] - 1) {
                out->push_back(it->second);
                i += len;
                break;
            }
        }
    }
    return true;
}

std::vector<int> NameFilter::Elements(const std::u32string& tok) const {
    if (tok.size() > kMaxWord || tok.find('#') != std::u32string::npos) return {0};
    std::vector<int> r;
    if (Dp(tok, &r)) return r;
    size_t a = 0, b = tok.size();
    auto strip_ch = [](char32_t c) { return c == 'x' || (c >= '0' && c <= '9'); };
    while (a < b && strip_ch(tok[a])) ++a;
    while (b > a && strip_ch(tok[b - 1])) --b;
    if (b > a && (a != 0 || b != tok.size()) && Dp(tok.substr(a, b - a), &r)) return r;
    return {0};
}

bool NameFilter::TierA(const std::vector<std::u32string>& blanked, int form) const {
    const std::set<std::u32string>& terms = terms_[form];
    const std::vector<std::u32string>& terms_s = terms_s_[form];
    auto strip_ch = [](char32_t c) { return c == 'x' || (c >= '0' && c <= '9'); };
    for (const std::u32string& b : blanked) {
        for (const std::u32string& part : SplitOn(b, '#')) {
            if (part.empty()) continue;
            if (terms.count(part)) return true;
            size_t lo = 0, hi = part.size();
            while (lo < hi && strip_ch(part[lo])) ++lo;
            while (hi > lo && strip_ch(part[hi - 1])) --hi;
            if (terms.count(part.substr(lo, hi - lo))) return true;
            for (const std::u32string& t : terms_s) {
                if (Contains(part, t)) return true;
            }
        }
    }
    std::u32string joined;
    for (const std::u32string& b : blanked) joined += b;
    for (const std::u32string& t : terms_s) {
        if (Contains(joined, t)) return true;
    }
    return false;
}

bool NameFilter::TierB(const std::vector<std::u32string>& words) const {
    std::vector<int> elems;
    for (const std::u32string& w : words) {
        std::vector<int> e = Elements(w);
        elems.insert(elems.end(), e.begin(), e.end());
    }
    bool strong = false, weak = false, va = false, vh = false;
    for (size_t i = 0; i < elems.size(); ++i) {
        const int c = elems[i];
        if (c == kIdS || (c == kIdW && i + 1 < elems.size() && elems[i + 1] == kPeople)) strong = true;
        else if (c == kIdW) weak = true;
        else if (c == kViolA) va = true;
        else if (c == kViolH) vh = true;
    }
    return (strong && (va || vh)) || (weak && vh);
}

const char* NameFilter::Scan(const std::u32string& cps) const {
    for (int leet = 0; leet < 3; ++leet) {
        std::vector<std::u32string> words = Words(cps, leet), blanked1, blanked;
        for (const std::u32string& w : words) blanked1.push_back(Blank(Collapse1(w), 1));
        if (TierA(blanked1, 1)) return "A";
        for (std::u32string& w : words) {
            w = Collapse(w);
            blanked.push_back(Blank(w, 2));
        }
        if (TierA(blanked, 2)) return "A";
        if (TierB(words)) return "B";
        std::u32string joined;
        for (const std::u32string& b : blanked) joined += b;
        for (const std::u32string& j : c_join_) {
            if (Contains(joined, j)) return "C";
        }
    }
    return "";
}

bool NameFilter::Codes(const std::u32string& cps) const {
    std::set<std::u32string> rs;
    std::u32string joined;
    for (const std::u32string& w : Words(cps, 0)) {
        std::u32string c = Cap3(w);
        rs.insert(c);
        joined += c;
    }
    for (const std::u32string& r : rs) {
        if (c_raw_.count(r)) return true;
    }
    for (const std::u32string& j : c_rawjoin_) {
        if (Contains(joined, j)) return true;
    }
    for (const auto& ctx : c_ctx_) {
        if (!rs.count(ctx.first)) continue;
        for (const std::u32string& o : ctx.second) {
            if (rs.count(o)) return true;
        }
    }
    return false;
}

std::string NameFilter::Reason(const std::string& utf8) const {
    if (utf8.empty()) return "";
    std::u32string name = Utf8ToU32(utf8);
    if (name.size() > kMaxIn) name.resize(kMaxIn);
    for (char32_t c : name) {
        if (c_sym_.count(c)) return "C";
    }
    for (size_t i = 0; i + 1 < name.size(); ++i) {
        for (const auto& p : c_sym2_) {
            if (name[i] == p.first && name[i + 1] == p.second) return "C";
        }
    }
    std::u32string cps;
    int up = 0;
    Map(name, &cps, &up);
    if (const char* r = Scan(cps); *r) return r;
    if (Codes(cps)) return "C";
    bool bidi = false;
    for (char32_t c : name) bidi = bidi || c == 0x202B || c == 0x202E || c == 0x2067;
    if (bidi) {  // a right-to-left override shows the stored text backwards
        std::u32string rev(cps.rbegin(), cps.rend());
        if (const char* r = Scan(rev); *r) return r;
    }
    if (up >= 1) {
        std::u32string sw(cps.rbegin(), cps.rend());
        for (char32_t& c : sw) {
            switch (c) {
            case 'u': c = 'n'; break;
            case 'n': c = 'u'; break;
            case 'd': c = 'p'; break;
            case 'p': c = 'd'; break;
            case 'b': c = 'q'; break;
            case 'q': c = 'b'; break;
            case 'm': c = 'w'; break;
            case 'w': c = 'm'; break;
            default: break;
            }
        }
        if (const char* r = Scan(sw); *r) return r;
    }
    return "";
}

const NameFilter& Default() {
    static const NameFilter f(LoadTexts());
    return f;
}

bool Offensive(const std::string& name) { return Default().Offensive(name); }
std::string Reason(const std::string& name) { return Default().Reason(name); }

std::string MaskText(const std::string& name) {
    std::uint32_t h = 0x811C9DC5u;
    for (unsigned char b : name) h = (h ^ b) * 0x01000193u;
    char buf[32];
    std::snprintf(buf, sizeof buf, "Hunter %03X", static_cast<unsigned>((h ^ (h >> 16)) & 0xFFF));
    return buf;
}

std::string Masked(const std::string& name) { return Offensive(name) ? MaskText(name) : name; }

}  // namespace coop::namefilter
