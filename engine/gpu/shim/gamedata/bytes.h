// SPDX-License-Identifier: GPL-3.0-or-later
// Bounds-checked little / big endian reads over a byte range, for the readers of the player's own game
// files (gamedata/*.h). Every out-of-range read throws FormatError: a damaged or unexpected file makes the
// table build fail with a message, never a crash. Header only, no game dependency.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace bbdata {

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

using Bytes = std::vector<std::uint8_t>;

/// Non-owning view of bytes.
struct Span {
    const std::uint8_t* p = nullptr;
    std::size_t n = 0;
    Span() = default;
    Span(const std::uint8_t* data, std::size_t size) : p(data), n(size) {}
    Span(const Bytes& b) : p(b.data()), n(b.size()) {}  // NOLINT: implicit on purpose

    bool Has(std::int64_t off, std::uint64_t len) const {
        return off >= 0 && static_cast<std::uint64_t>(off) <= n && len <= n - static_cast<std::uint64_t>(off);
    }
    void Need(std::int64_t off, std::uint64_t len) const {
        if (!Has(off, len)) {
            throw FormatError("read out of range (offset " + std::to_string(off) + ", " + std::to_string(len) +
                              " bytes of " + std::to_string(n) + ")");
        }
    }
    /// Little endian value at `off`.
    template <class T>
    T Le(std::int64_t off) const {
        Need(off, sizeof(T));
        T v;
        std::memcpy(&v, p + off, sizeof(T));
        return v;  // the targets are little endian (x86-64)
    }
    /// Big endian u32 at `off`.
    std::uint32_t Be32(std::int64_t off) const {
        Need(off, 4);
        return static_cast<std::uint32_t>(p[off]) << 24 | static_cast<std::uint32_t>(p[off + 1]) << 16 |
               static_cast<std::uint32_t>(p[off + 2]) << 8 | p[off + 3];
    }
    bool Tag(std::int64_t off, const char* tag4) const { return Has(off, 4) && std::memcmp(p + off, tag4, 4) == 0; }
    /// The `len` bytes at `off` (throws when out of range).
    Span Sub(std::int64_t off, std::uint64_t len) const {
        Need(off, len);
        return Span(p + off, static_cast<std::size_t>(len));
    }
    /// Like Python's b[a:b]: clamped, never throws.
    Span Slice(std::int64_t a, std::int64_t b) const {
        const std::int64_t sz = static_cast<std::int64_t>(n);
        a = a < 0 ? 0 : (a > sz ? sz : a);
        b = b < a ? a : (b > sz ? sz : b);
        return Span(p + a, static_cast<std::size_t>(b - a));
    }
};

/// UTF-16LE (NUL terminated at an even step from `at`, or the end of the data) as UTF-8. An unpaired
/// surrogate becomes U+FFFD (Python's errors="replace").
inline std::string Utf16z(Span b, std::int64_t at) {
    if (at < 0 || static_cast<std::uint64_t>(at) > b.n) {
        throw FormatError("string offset out of range");
    }
    std::size_t e = static_cast<std::size_t>(at);
    while (e + 1 < b.n && !(b.p[e] == 0 && b.p[e + 1] == 0)) {
        e += 2;
    }
    std::string out;
    auto put = [&](std::uint32_t c) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | c >> 6);
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | c >> 12);
            out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | c >> 18);
            out += static_cast<char>(0x80 | (c >> 12 & 0x3F));
            out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    };
    const std::size_t end = e;
    std::size_t i = static_cast<std::size_t>(at);
    while (i + 1 < end) {
        std::uint32_t u = b.p[i] | static_cast<std::uint32_t>(b.p[i + 1]) << 8;
        i += 2;
        if (u >= 0xD800 && u < 0xDC00) {
            if (i + 1 < end) {
                const std::uint32_t lo = b.p[i] | static_cast<std::uint32_t>(b.p[i + 1]) << 8;
                if (lo >= 0xDC00 && lo < 0xE000) {
                    i += 2;
                    put(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                    continue;
                }
            }
            put(0xFFFD);
        } else if (u >= 0xDC00 && u < 0xE000) {
            put(0xFFFD);
        } else {
            put(u);
        }
    }
    return out;
}

}  // namespace bbdata
