// SPDX-License-Identifier: GPL-3.0-or-later
// out/party_host_only.txt: boss phase events only the party host decides, which the enabled mods add under ids of
// their own (a mod lists them in party_host_only.txt in its folder, beside dvdroot_ps4; the engine's host reads the
// merged file, party_bossfog_rules.h ParseModHostOnlyEvents). Same rules as scripts/mods.py mod_host_only_events and
// write_party_host_only_file: the mod's file is read as UTF-8 (a leading byte order mark dropped) and split into lines
// as Python's str.splitlines does after universal newlines; `#` starts a comment; spaces, tabs and CRs around an id are
// dropped; any other line (not ASCII digits, 0, above 4294967295) leaves the whole mod's list out. The lists are merged
// sorted and unique; no ids: no file; more than kMax: none are used and no file.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace hostonly {

constexpr const char* kFile = "party_host_only.txt";
constexpr std::size_t kMax = 4096;  // party_bossfog_rules.h kMaxModHostOnlyEvents, scripts/mods.py PARTY_HOST_ONLY_MAX

/// Strict UTF-8 as Python's codec decodes it (no overlong forms, no surrogates, at most U+10FFFF). False at the first
/// bad byte (`where`: its offset).
inline bool DecodeUtf8(std::string_view bytes, std::u32string* out, std::size_t* where) {
    out->clear();
    for (std::size_t i = 0; i < bytes.size();) {
        const auto b = std::uint8_t(bytes[i]);
        std::size_t n = 0;
        char32_t c = 0, low = 0;
        if (b < 0x80) {
            out->push_back(b);
            ++i;
            continue;
        } else if (b >= 0xC2 && b <= 0xDF) {
            n = 1, c = b & 0x1F, low = 0x80;
        } else if (b >= 0xE0 && b <= 0xEF) {
            n = 2, c = b & 0x0F, low = 0x800;
        } else if (b >= 0xF0 && b <= 0xF4) {
            n = 3, c = b & 0x07, low = 0x10000;
        } else {
            *where = i;
            return false;
        }
        for (std::size_t k = 1; k <= n; ++k) {
            if (i + k >= bytes.size() || (std::uint8_t(bytes[i + k]) & 0xC0) != 0x80) {
                *where = i;
                return false;
            }
            c = (c << 6) | (std::uint8_t(bytes[i + k]) & 0x3F);
        }
        if (c < low || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
            *where = i;
            return false;
        }
        out->push_back(c);
        i += n + 1;
    }
    return true;
}

/// Python's str.splitlines boundaries (after universal newlines, "\r\n" is one).
inline bool LineBreak(char32_t c) {
    return c == U'\n' || c == U'\r' || c == U'\v' || c == U'\f' || c == 0x1C || c == 0x1D || c == 0x1E || c == 0x85 ||
           c == 0x2028 || c == 0x2029;
}

inline std::string Utf8(std::u32string_view text) {
    std::string out;
    for (const char32_t c : text) {
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

/// One mod's party_host_only.txt (`bytes`): true with its ids in file order (repeats kept, as mods.py), or false with
/// `error` ("<name>: party_host_only.txt line N is not an event id: <line>", as mods.py's ValueError).
inline bool ParseModList(std::string_view name, std::string_view bytes, std::vector<std::uint32_t>* ids, std::string* error) {
    ids->clear();
    if (bytes.substr(0, 3) == "\xEF\xBB\xBF") bytes.remove_prefix(3);  // utf-8-sig
    std::u32string text;
    std::size_t where = 0;
    if (!DecodeUtf8(bytes, &text, &where)) {
        *error = std::string(name) + ": " + kFile + " is not UTF-8 (byte " + std::to_string(where) + ")";
        return false;
    }
    std::vector<std::u32string_view> lines;
    const std::u32string_view all(text);
    for (std::size_t start = 0, i = 0; i < all.size();) {
        if (!LineBreak(all[i])) {
            if (++i == all.size()) lines.push_back(all.substr(start));
            continue;
        }
        lines.push_back(all.substr(start, i - start));
        i += (all[i] == U'\r' && i + 1 < all.size() && all[i + 1] == U'\n') ? 2 : 1;
        start = i;
    }
    for (std::size_t number = 1; number <= lines.size(); ++number) {
        const std::u32string_view line = lines[number - 1];
        std::u32string_view t = line.substr(0, line.find(U'#'));
        const auto blank = [](char32_t c) { return c == U' ' || c == U'\t' || c == U'\r'; };
        while (!t.empty() && blank(t.front())) t.remove_prefix(1);
        while (!t.empty() && blank(t.back())) t.remove_suffix(1);
        if (t.empty()) continue;
        std::uint64_t v = 0;
        bool ok = true;
        for (const char32_t c : t) {
            if (c < U'0' || c > U'9') {
                ok = false;
                break;
            }
            if (v <= 0xFFFFFFFFu) v = v * 10 + std::uint64_t(c - U'0');  // past the limit: stays past it
        }
        if (!ok || v == 0 || v > 0xFFFFFFFFu) {
            // mods.py names the line with str.strip(): Python's whitespace on both ends
            const auto space = [](char32_t c) {
                return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
                       (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
                       c == 0x3000;
            };
            std::u32string_view shown = line;
            while (!shown.empty() && space(shown.front())) shown.remove_prefix(1);
            while (!shown.empty() && space(shown.back())) shown.remove_suffix(1);
            *error = std::string(name) + ": " + kFile + " line " + std::to_string(number) + " is not an event id: " + Utf8(shown);
            ids->clear();
            return false;
        }
        ids->push_back(std::uint32_t(v));
    }
    return true;
}

/// The merge of the layers' lists (in load order): `names` of the mods that gave ids, `ids` sorted and unique.
struct Merged {
    std::vector<std::string> names;
    std::set<std::uint32_t> ids;
};

inline void Add(Merged* merged, std::string_view name, const std::vector<std::uint32_t>& own) {
    if (own.empty()) return;
    merged->ids.insert(own.begin(), own.end());
    merged->names.emplace_back(name);
}

/// out/party_host_only.txt's text, or "" for no file (no ids, or more than kMax).
inline std::string FileText(const Merged& merged) {
    if (merged.ids.empty() || merged.ids.size() > kMax) return "";
    std::string text = "# Boss phase events only the party host decides (bb-start), from: ";
    for (std::size_t i = 0; i < merged.names.size(); ++i) text += (i ? ", " : "") + merged.names[i];
    text += "\n";
    for (const std::uint32_t id : merged.ids) text += std::to_string(id) + "\n";
    return text;
}

}  // namespace hostonly
