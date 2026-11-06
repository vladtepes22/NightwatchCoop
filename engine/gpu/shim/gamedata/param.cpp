// SPDX-License-Identifier: GPL-3.0-or-later
#include "param.h"

#include <cctype>
#include <optional>

namespace bbdata {
namespace {

struct Field {
    std::string name;
    std::string type;  // "s8".."f32", or "raw"
    std::int64_t off = 0;
    int bit = -1;  // -1: whole value
    int width = 0;
};

int TypeSize(const std::string& t) {
    if (t == "s8" || t == "u8") return 1;
    if (t == "s16" || t == "u16") return 2;
    if (t == "s32" || t == "u32" || t == "f32") return 4;
    return 0;
}

bool IsValueType(const std::string& t) { return TypeSize(t) != 0; }

bool WordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// "name : 3" (spaces allowed, a SpEffect field is "hasTarget : 1") -> (name, 3)
std::optional<std::pair<std::string, int>> ParseBitfield(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size() && WordChar(s[i])) ++i;
    if (i == 0) return std::nullopt;
    const std::string name = s.substr(0, i);
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size() || s[i] != ':') return std::nullopt;
    ++i;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size()) return std::nullopt;
    int w = 0;
    for (; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return std::nullopt;
        w = w * 10 + (s[i] - '0');
        if (w > 1000) return std::nullopt;
    }
    return std::make_pair(name, w);
}

// "name[4]" -> name
std::optional<std::string> ParseArray(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size() && WordChar(s[i])) ++i;
    if (i == 0 || i >= s.size() || s[i] != '[') return std::nullopt;
    std::size_t j = i + 1;
    if (j >= s.size() || !std::isdigit(static_cast<unsigned char>(s[j]))) return std::nullopt;
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
    if (j + 1 != s.size() || s[j] != ']') return std::nullopt;
    return s.substr(0, i);
}

std::string CStr(Span b, std::int64_t at, std::size_t max) {
    b.Need(at, max);
    std::size_t n = 0;
    while (n < max && b.p[at + static_cast<std::int64_t>(n)]) ++n;
    return std::string(reinterpret_cast<const char*>(b.p) + at, n);
}

std::vector<Field> Layout(Span b) {
    const std::uint16_t n = b.Le<std::uint16_t>(0x08);
    std::vector<Field> lay;
    std::int64_t off = 0;
    int bit = 0;
    std::string bt;  // current bitfield's base type, empty: none
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::int64_t o = 0x38 + static_cast<std::int64_t>(i) * 0xd0;
        const std::string t = CStr(b, o + 0x70, 0x20);
        const std::string nm = CStr(b, o + 0x90, 0x20);
        const std::uint32_t cnt = b.Le<std::uint32_t>(o + 0x64);
        std::string base;  // empty: none
        if (IsValueType(t)) {
            base = t;
        } else if (t.rfind("dummy", 0) == 0 || t.rfind("fixstr", 0) == 0) {
            base.clear();
        } else {
            base = cnt == 1 ? "u8" : cnt == 2 ? "u16" : cnt == 4 ? "u32" : "";
        }
        const auto bf = ParseBitfield(nm);
        if (bf && !base.empty()) {
            const int w = bf->second;
            if (bt != base || bit + w > TypeSize(base) * 8) {
                if (!bt.empty()) off += TypeSize(bt);
                bt = base;
                bit = 0;
            }
            lay.push_back({bf->first, base, off, bit, w});
            bit += w;
            continue;
        }
        if (!bt.empty()) {
            off += TypeSize(bt);
            bt.clear();
            bit = 0;
        }
        const auto arr = ParseArray(nm);
        lay.push_back({arr ? *arr : nm, base.empty() ? "raw" : base, off, -1, static_cast<int>(cnt)});
        off += cnt;
    }
    return lay;
}

}  // namespace

ParamTable ParseParam(Span param, Span paramdef) {
    const std::vector<Field> lay = Layout(paramdef);
    ParamTable t;
    std::vector<const Field*> cols;
    for (const Field& f : lay) {
        if (!IsValueType(f.type)) continue;
        t.index_[f.name] = static_cast<int>(t.names_.size());
        t.names_.push_back(f.name);
        cols.push_back(&f);
    }
    std::vector<int> kinds;
    for (const Field* f : cols) {
        const std::string& ty = f->type;
        kinds.push_back(ty == "u8" ? 0 : ty == "s8" ? 1 : ty == "u16" ? 2 : ty == "s16" ? 3 : ty == "u32" ? 4 : ty == "s32" ? 5 : 6);
    }
    const std::uint16_t cnt = param.Le<std::uint16_t>(0x0a);
    t.ids.reserve(cnt);
    t.cells_.assign(static_cast<std::size_t>(cnt) * cols.size(), 0);
    for (std::uint32_t i = 0; i < cnt; ++i) {
        const std::int64_t r = 0x40 + static_cast<std::int64_t>(i) * 0x18;
        t.ids.push_back(param.Le<std::int32_t>(r));
        const std::int64_t doff = param.Le<std::int64_t>(r + 8);
        for (std::size_t c = 0; c < cols.size(); ++c) {
            const Field& f = *cols[c];
            const std::int64_t at = doff + f.off;
            std::int64_t v = 0;
            std::uint64_t raw = 0;
            switch (kinds[c]) {
                case 0: raw = param.Le<std::uint8_t>(at); v = static_cast<std::int64_t>(raw); break;
                case 1: v = param.Le<std::int8_t>(at); raw = static_cast<std::uint8_t>(v); break;
                case 2: raw = param.Le<std::uint16_t>(at); v = static_cast<std::int64_t>(raw); break;
                case 3: v = param.Le<std::int16_t>(at); raw = static_cast<std::uint16_t>(v); break;
                case 4: raw = param.Le<std::uint32_t>(at); v = static_cast<std::int64_t>(raw); break;
                case 5: v = param.Le<std::int32_t>(at); raw = static_cast<std::uint32_t>(v); break;
                default: raw = param.Le<std::uint32_t>(at); v = static_cast<std::int64_t>(raw); break;  // f32: bits
            }
            if (f.bit >= 0) {
                v = static_cast<std::int64_t>((raw >> f.bit) & ((std::uint64_t{1} << f.width) - 1));
            }
            t.cells_[i * cols.size() + c] = v;
        }
    }
    return t;
}

}  // namespace bbdata
