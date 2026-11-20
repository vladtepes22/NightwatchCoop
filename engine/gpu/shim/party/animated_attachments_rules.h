// SPDX-License-Identifier: GPL-3.0-or-later
// animated_attachments (docs/modding/animated_attachments.md): the pure half of animated_attachments.cpp.
// The table is out/nightwatch_animated_attachments.txt, written by bb-start (start/features.h
// AnimatedAttachmentsFileText) from the mods' nightwatch/animated_attachments.json: "<speffect> <PART>" per line,
// '#' comments, in merge order.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace coop::attachments {

constexpr std::size_t kMaxRows = 64;      // start/features.h kMaxAttachmentRows
constexpr int kSlotsPerChr = 2;           // the asm model's slots 15 and 16 (animated_attachments.cpp)
constexpr std::size_t kMaxPartName = 32;  // start/features.h AttachmentPartName

struct Row {
    std::int32_t speffect = 0;
    std::string part;  // upper case, e.g. "HD_A_9761"
};

/// A part file name: 1..32 of A-Z, 0-9, '_' (bb-start wrote it upper-cased).
inline bool ValidPart(std::string_view p) {
    if (p.empty() || p.size() > kMaxPartName) return false;
    for (char c : p)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

/// The whole file, or false (no rows used) at the first line that is not "<speffect> <PART>" with speffect
/// 1..2147483647 and PART as ValidPart, or past kMaxRows rows.
inline bool ParseTable(std::string_view text, std::vector<Row>* rows, std::size_t* bad_line = nullptr) {
    rows->clear();
    std::size_t number = 0;
    const auto fail = [&]() {
        rows->clear();
        if (bad_line) *bad_line = number;
        return false;
    };
    while (!text.empty()) {
        ++number;
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        std::vector<std::string_view> words;
        std::size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
            const std::size_t start = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
            if (i > start) words.push_back(line.substr(start, i - start));
        }
        if (words.empty()) continue;  // blank or comment
        if (words.size() != 2 || rows->size() >= kMaxRows) return fail();
        std::int64_t sp = 0;
        for (char c : words[0]) {
            if (c < '0' || c > '9') return fail();
            sp = sp * 10 + (c - '0');
            if (sp > 0x7FFFFFFF) return fail();
        }
        if (sp < 1 || !ValidPart(words[1])) return fail();
        rows->push_back({std::int32_t(sp), std::string(words[1])});
    }
    return true;
}

/// The rows a character wears: the first kSlotsPerChr rows (table order) whose SpEffect `has(speffect)` says is on.
/// Indices into `rows`; `dropped` (optional) receives the count of further rows that were on but found no slot.
template <class Has>
inline std::vector<int> Wanted(const std::vector<Row>& rows, Has&& has, int* dropped = nullptr) {
    std::vector<int> out;
    int extra = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!has(rows[i].speffect)) continue;
        if (int(out.size()) < kSlotsPerChr) out.push_back(int(i));
        else ++extra;
    }
    if (dropped) *dropped = extra;
    return out;
}

/// The game's resource path of a part, as its equipment does (0x1CA9790 -> 0x1F29D30, "parts:/%s.partsbnd"): the
/// name, "_L" when the parts manager is told to use the low set (its 6th argument), then ".partsbnd" (the file on
/// disk is .partsbnd.dcx). UTF-16 with a terminating 0.
inline std::u16string ResourcePath(std::string_view part, bool low) {
    std::u16string s = u"parts:/";
    for (char c : part) s += char16_t(static_cast<unsigned char>(c));
    if (low) s += u"_L";
    s += u".partsbnd";
    return s;
}

}  // namespace coop::attachments
