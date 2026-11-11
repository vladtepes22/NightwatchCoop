// SPDX-License-Identifier: GPL-3.0-or-later
// Fast inflate (BB_FAST_INFLATE, bbport_load_speed.cpp): the engine's own zlib 1.2.3 inflate (guest +0x2067AC0,
// with inflate_fast at +0x26219D0) is 60-85 % of a map load, run on the resource post-processor threads. The
// streams it reads are raw deflate (windowBits -15) with the engine's 0x70-byte LP64 z_stream. This header holds
// the part that needs no game: the same stream stepped by zlib-ng (https://github.com/zlib-ng/zlib-ng, zlib
// licence, licenses/zlib-ng.txt). The inflated bytes of a deflate stream are fixed by the format, so the output
// is identical to the engine's; tests/test_fast_inflate.cpp checks the whole contract (output, return codes,
// the z_stream fields the engine reads) against zlib on streams made at test time.
//
// Per guest stream: the engine's own state is still made by its inflateInit2_ (so its inflateEnd / reset keep
// working on it); a zlib-ng stream is made next to it and its pointer kept in the z_stream's `reserved` field
// (+0x68, which zlib 1.2.3 never touches). inflate() steps the zlib-ng stream through the guest's fields.
#pragma once

#include <zlib-ng.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace BbFastInflate {

/// The engine's z_stream (PS4 / LP64 layout, 0x70 bytes): pointers and size_t are 8 bytes, uInt 4.
struct GuestZ {
    const std::uint8_t* next_in;
    std::uint32_t avail_in;
    std::uint32_t pad0;
    std::uint64_t total_in;
    std::uint8_t* next_out;
    std::uint32_t avail_out;
    std::uint32_t pad1;
    std::uint64_t total_out;
    const char* msg;
    void* state;
    void* zalloc;
    void* zfree;
    void* opaque;
    std::int32_t data_type;
    std::int32_t pad2;
    std::uint64_t adler;
    std::uint64_t reserved;
};
static_assert(sizeof(GuestZ) == 0x70, "the engine's z_stream is 0x70 bytes");
static_assert(offsetof(GuestZ, total_in) == 0x10 && offsetof(GuestZ, next_out) == 0x18 &&
                  offsetof(GuestZ, avail_out) == 0x20 && offsetof(GuestZ, total_out) == 0x28 &&
                  offsetof(GuestZ, msg) == 0x30 && offsetof(GuestZ, state) == 0x38 &&
                  offsetof(GuestZ, reserved) == 0x68,
              "z_stream field offsets");

constexpr int kRawWindowBits = -15;
constexpr int kZlibWindowBits = 15;  // what the engine's readers use: a zlib header and an adler32 trailer

struct Stats {
    std::uint64_t streams = 0, in_bytes = 0, out_bytes = 0;
};

/// Streams are pooled: a map load makes thousands of short ones.
class Pool {
public:
    explicit Pool(int window_bits) : bits_(window_bits) {}
    zng_stream* Acquire() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!free_.empty()) {
                zng_stream* s = free_.back();
                free_.pop_back();
                return s;
            }
        }
        zng_stream* s = new zng_stream;
        std::memset(s, 0, sizeof(*s));
        if (zng_inflateInit2(s, bits_) != Z_OK) {
            delete s;
            return nullptr;
        }
        return s;
    }
    void Release(zng_stream* s) {
        if (!s) return;
        if (zng_inflateReset(s) == Z_OK) {
            std::lock_guard<std::mutex> lk(mu_);
            if (free_.size() < 64) {
                free_.push_back(s);
                return;
            }
        }
        zng_inflateEnd(s);
        delete s;
    }

private:
    int bits_;
    std::mutex mu_;
    std::vector<zng_stream*> free_;
};

/// One inflate() call of the engine's contract on a zlib-ng stream: the guest's buffers go in, what was consumed
/// and produced comes back through the guest's fields (totals added, as zlib keeps them). Returns zlib's code.
inline int Step(GuestZ* z, zng_stream* s, int flush) {
    s->next_in = z->next_in;
    s->avail_in = z->avail_in;
    s->next_out = z->next_out;
    s->avail_out = z->avail_out;
    const int rc = zng_inflate(s, flush);
    z->total_in += z->avail_in - s->avail_in;
    z->total_out += z->avail_out - s->avail_out;
    z->next_in = s->next_in;
    z->avail_in = s->avail_in;
    z->next_out = s->next_out;
    z->avail_out = s->avail_out;
    z->msg = s->msg;
    z->adler = s->adler;
    return rc;
}

}  // namespace BbFastInflate
