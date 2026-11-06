// SPDX-License-Identifier: GPL-3.0-or-later
#include "msb.h"

namespace bbdata {

std::vector<MsbList> ParseMsbLists(Span data) {
    std::vector<MsbList> out;
    std::int64_t off = 0x10;
    while (off && off < static_cast<std::int64_t>(data.n)) {
        const std::int32_t cnt = data.Le<std::int32_t>(off + 4);
        const std::int64_t name_off = data.Le<std::int64_t>(off + 8);
        if (cnt <= 0 || cnt > (1 << 24)) {
            throw FormatError("MSB list count implausible");
        }
        MsbList l;
        if (name_off) l.name = Utf16z(data, name_off);
        l.entries.reserve(static_cast<std::size_t>(cnt) - 1);
        for (std::int32_t i = 0; i < cnt - 1; ++i) {
            l.entries.push_back(data.Le<std::int64_t>(off + 16 + static_cast<std::int64_t>(i) * 8));
        }
        const std::int64_t next = data.Le<std::int64_t>(off + 16 + static_cast<std::int64_t>(cnt - 1) * 8);
        out.push_back(std::move(l));
        off = next;
    }
    return out;
}

const std::vector<std::int64_t>* FindMsbList(const std::vector<MsbList>& lists, const std::string& name) {
    const std::vector<std::int64_t>* found = nullptr;
    for (const MsbList& l : lists) {
        if (l.name == name) found = &l.entries;
    }
    return found;
}

std::vector<MsbPoint> ReadMsbPoints(Span data, const std::vector<std::int64_t>& entries, bool strict_entity) {
    std::vector<MsbPoint> out;
    out.reserve(entries.size());
    for (const std::int64_t r : entries) {
        MsbPoint p;
        p.x = data.Le<float>(r + 0x14);
        p.y = data.Le<float>(r + 0x18);
        p.z = data.Le<float>(r + 0x1c);
        p.yaw_deg = data.Le<float>(r + 0x24);
        const std::int64_t ent = data.Le<std::int64_t>(r + 0x48);
        if (strict_entity) {
            p.entity = data.Le<std::int32_t>(r + ent);
            p.has_entity = true;
        } else if (0 < ent && data.Has(r + ent, 4)) {
            p.entity = data.Le<std::int32_t>(r + ent);
            p.has_entity = true;
        }
        out.push_back(p);
    }
    return out;
}

std::vector<MsbPart> ReadMsbParts(Span data, const std::vector<std::int64_t>& entries) {
    std::vector<MsbPart> out;
    out.reserve(entries.size());
    for (const std::int64_t r : entries) {
        MsbPart p;
        if (data.Has(r, 0xB8)) {
            p.type = data.Le<std::int32_t>(r + 0x14);
            p.x = data.Le<float>(r + 0x28);
            p.y = data.Le<float>(r + 0x2c);
            p.z = data.Le<float>(r + 0x30);
            const std::int64_t ent = data.Le<std::int64_t>(r + 0xB0);
            if (0 < ent && data.Has(r + ent, 4)) {
                p.entity = data.Le<std::int32_t>(r + ent);
                p.has_entity = true;
            }
        }
        out.push_back(p);
    }
    return out;
}

std::vector<std::string> ReadMsbNames(Span data, const std::vector<std::int64_t>& entries) {
    std::vector<std::string> out;
    out.reserve(entries.size());
    for (const std::int64_t r : entries) {
        const std::int64_t off = data.Has(r, 8) ? data.Le<std::int64_t>(r) : 0;
        out.push_back(off > 0 && data.Has(r + off, 2) ? Utf16z(data, r + off) : std::string());
    }
    return out;
}

void ScanMsbEntities(Span data, std::int64_t lo, std::vector<std::int32_t>* out) {
    if (!data.Tag(0, "MSB ")) {
        throw FormatError("not an MSB");
    }
    const std::size_t words = data.n / 4;
    const std::int64_t hi = lo + 10000;
    for (std::size_t i = 0; i < words; ++i) {
        std::int32_t v;
        std::memcpy(&v, data.p + i * 4, 4);
        if (v >= lo && v < hi) out->push_back(v);
    }
}

}  // namespace bbdata
