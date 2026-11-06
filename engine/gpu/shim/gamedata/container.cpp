// SPDX-License-Identifier: GPL-3.0-or-later
#include "container.h"

#include <miniz.h>

#include <cstdio>

namespace bbdata {

Bytes ReadFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        throw FormatError("cannot open " + path);
    }
    Bytes out;
    std::uint8_t buf[1 << 16];
    if (std::fseek(f, 0, SEEK_END) == 0) {
        const long sz = std::ftell(f);
        if (sz > 0) {
            out.reserve(static_cast<std::size_t>(sz));
        }
        std::fseek(f, 0, SEEK_SET);
    }
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) {
        out.insert(out.end(), buf, buf + n);
    }
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) {
        throw FormatError("read error in " + path);
    }
    return out;
}

Bytes DecodeMaybeDcx(Bytes raw) {
    const Span b(raw);
    if (!b.Tag(0, "DCX\0")) {
        return raw;
    }
    if (!b.Tag(0x24, "DCP\0") || !b.Tag(0x28, "DFLT")) {
        throw FormatError("unsupported DCX compression");
    }
    const std::uint32_t usz = b.Be32(0x1c);
    const std::uint32_t csz = b.Be32(0x20);
    const Span z = b.Sub(0x4c, csz);
    if (usz > (1u << 30)) {
        throw FormatError("DCX size implausible");
    }
    Bytes out(usz);
    mz_ulong dl = usz;
    const int rc = mz_uncompress(out.data(), &dl, z.p, static_cast<mz_ulong>(z.n));
    if (rc != MZ_OK) {
        throw FormatError("DCX stream damaged (zlib " + std::to_string(rc) + ")");
    }
    if (dl != usz) {
        throw FormatError("DCX size mismatch");
    }
    return out;
}

Bytes ReadMaybeDcx(const std::string& path) {
    try {
        return DecodeMaybeDcx(ReadFile(path));
    } catch (const FormatError& e) {
        const std::string m = e.what();
        if (m.find(path) != std::string::npos) {
            throw;
        }
        throw FormatError(path + ": " + m);
    }
}

Bytes MakeDcx(Span data) {
    mz_ulong cl = mz_compressBound(static_cast<mz_ulong>(data.n));
    Bytes z(cl);
    if (mz_compress(z.data(), &cl, data.p, static_cast<mz_ulong>(data.n)) != MZ_OK) {
        throw FormatError("compress failed");
    }
    z.resize(cl);
    Bytes out(0x4c, 0);
    std::memcpy(out.data(), "DCX\0", 4);
    std::memcpy(out.data() + 0x24, "DCP\0", 4);
    std::memcpy(out.data() + 0x28, "DFLT", 4);
    auto be = [&](std::size_t at, std::uint32_t v) {
        out[at] = static_cast<std::uint8_t>(v >> 24);
        out[at + 1] = static_cast<std::uint8_t>(v >> 16);
        out[at + 2] = static_cast<std::uint8_t>(v >> 8);
        out[at + 3] = static_cast<std::uint8_t>(v);
    };
    be(0x1c, static_cast<std::uint32_t>(data.n));
    be(0x20, static_cast<std::uint32_t>(z.size()));
    out.insert(out.end(), z.begin(), z.end());
    return out;
}

std::vector<BndEntry> ParseBnd4(Span b) {
    if (!b.Tag(0, "BND4")) {
        throw FormatError("not BND4");
    }
    const std::uint32_t count = b.Le<std::uint32_t>(0x0c);
    const std::uint64_t esz = b.Le<std::uint64_t>(0x20);
    const bool unicode = b.Le<std::uint8_t>(0x30) != 0;
    std::vector<BndEntry> out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::int64_t o = 0x40 + static_cast<std::int64_t>(i) * static_cast<std::int64_t>(esz);
        const std::int64_t csz = b.Le<std::int64_t>(o + 8);
        const std::uint32_t off = b.Le<std::uint32_t>(o + 0x18);
        const std::uint32_t noff = b.Le<std::uint32_t>(o + 0x20);
        BndEntry e;
        if (unicode) {
            e.name = Utf16z(b, noff);
        } else {
            if (noff > b.n) {
                throw FormatError("BND4 name out of range");
            }
            std::size_t k = noff;
            while (k < b.n && b.p[k]) {
                ++k;
            }
            if (k >= b.n) {
                throw FormatError("BND4 name unterminated");
            }
            e.name.assign(reinterpret_cast<const char*>(b.p) + noff, k - noff);
        }
        if (csz < 0 || static_cast<std::uint64_t>(off) + static_cast<std::uint64_t>(csz) > b.n) {
            throw FormatError("BND4 entry out of range");
        }
        e.offset = off;
        e.size = static_cast<std::uint64_t>(csz);
        out.push_back(std::move(e));
    }
    return out;
}

const BndEntry* FindBndEntry(const std::vector<BndEntry>& entries, const std::string& name_with_ext) {
    const std::string tail = "\\" + name_with_ext;
    for (const BndEntry& e : entries) {
        std::string n = e.name;
        for (char& c : n) {
            if (c == '/') {
                c = '\\';
            }
        }
        if (n.size() >= tail.size() && n.compare(n.size() - tail.size(), tail.size(), tail) == 0) {
            return &e;
        }
    }
    return nullptr;
}

}  // namespace bbdata
