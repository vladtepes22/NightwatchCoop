// SPDX-License-Identifier: GPL-3.0-or-later
// Zstd behind the engine's inflate (zstd081). The converter (tools/zstdpack) recompresses the zlib stream inside each
// DCX file with zstd; the file keeps its DCX header ("DFLT", sizes rewritten). The engine's stream readers still call
// inflateInit2_ / inflate(z, 3) / inflateReset / inflateEnd on their 0x70-byte z_stream, so the hooks in
// bbport_load_speed.cpp watch the first input of every stream: a zstd frame (magic 0x28B52FFD, which a zlib header can
// never start with: zlib's first byte is 0x78 and (b0*256+b1) % 31 == 0) is decoded here, anything else keeps going
// to zlib-ng / the game's inflate, so mods and loose files in the original format still work.
//
// This header is the part that needs no game: the z_stream contract on ZSTD_decompressStream (BSD licence, zstd,
// licenses/zstd.txt). tests/test_zstd_inflate.cpp checks it against the same call patterns as the engine's readers:
// chunked input and output, a truncated stream, corrupt data, mixed zlib and zstd streams.
#pragma once

#include <zstd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "bbport_fast_inflate.h"

namespace BbZstdInflate {

constexpr std::uint32_t kMagic = 0xFD2FB528u;  // 28 B5 2F FD, little endian

/// A stream's first input starts a zstd frame. zlib streams never do (first byte 0x78-ish, never 0x28 0xB5). A first
/// chunk shorter than 4 bytes counts when it is a prefix of the magic (zstd takes the header a byte at a time).
inline bool IsZstd(const std::uint8_t* p, std::uint32_t n) {
    static const std::uint8_t magic[4] = {0x28, 0xB5, 0x2F, 0xFD};
    if (n == 0) return false;
    return std::memcmp(p, magic, n < 4 ? n : 4) == 0;
}

// zlib return codes (zlib.h), spelled here so the header does not need it.
constexpr int kOk = 0, kStreamEnd = 1, kDataError = -3, kBufError = -5;

struct Stats {
    std::uint64_t streams = 0, in_bytes = 0, out_bytes = 0;
};

/// One zstd stream: a decompression context and the state zlib keeps in its z_stream's `state`.
struct State {
    ZSTD_DCtx* ctx = nullptr;
    bool done = false;        // the frame ended (Z_STREAM_END is returned from now on)
    bool failed = false;
    const char* msg = nullptr;
    // The zlib-ng stream the engine's z_stream had before this stream turned out to be zstd (kept: a reset may bring
    // a zlib stream next). Owned by the caller's pool; opaque here.
    void* zng = nullptr;
    bool raw = false;
};

/// Contexts are pooled: a map load opens thousands of streams, and a context keeps its window buffer.
class Pool {
public:
    ZSTD_DCtx* Acquire() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!free_.empty()) {
                ZSTD_DCtx* c = free_.back();
                free_.pop_back();
                return c;
            }
        }
        return ZSTD_createDCtx();
    }
    void Release(ZSTD_DCtx* c) {
        if (!c) return;
        ZSTD_DCtx_reset(c, ZSTD_reset_session_only);
        std::lock_guard<std::mutex> lk(mu_);
        if (free_.size() < 16) {
            free_.push_back(c);
            return;
        }
        ZSTD_freeDCtx(c);
    }

private:
    std::mutex mu_;
    std::vector<ZSTD_DCtx*> free_;
};

/// inflateReset on a zstd stream: the next stream starts clean (it may be zlib: the caller decides on its first input).
inline void Reset(State* s) {
    ZSTD_DCtx_reset(s->ctx, ZSTD_reset_session_only);
    s->done = false;
    s->failed = false;
    s->msg = nullptr;
}

/// One inflate() call of the engine's contract on a zstd stream: the guest's buffers go in, what was consumed and
/// produced comes back through the guest's fields (totals added). Returns zlib's code:
///   Z_STREAM_END  the frame ended and everything it produced was delivered,
///   Z_OK          progress was made,
///   Z_BUF_ERROR   no progress possible (no input left or no room), or Z_FINISH could not finish,
///   Z_DATA_ERROR  corrupt data (msg set).
inline int Step(BbFastInflate::GuestZ* z, State* s, int flush) {
    if (s->failed) return kDataError;
    if (s->done) return kStreamEnd;
    ZSTD_inBuffer in{z->next_in, z->avail_in, 0};
    ZSTD_outBuffer out{z->next_out, z->avail_out, 0};
    const std::size_t r = ZSTD_decompressStream(s->ctx, &out, &in);
    z->next_in += in.pos;
    z->avail_in -= static_cast<std::uint32_t>(in.pos);
    z->total_in += in.pos;
    z->next_out += out.pos;
    z->avail_out -= static_cast<std::uint32_t>(out.pos);
    z->total_out += out.pos;
    if (ZSTD_isError(r)) {
        s->failed = true;
        s->msg = ZSTD_getErrorName(r);
        z->msg = s->msg;
        return kDataError;
    }
    if (r == 0) {
        s->done = true;
        return kStreamEnd;
    }
    if (in.pos == 0 && out.pos == 0) return kBufError;
    return flush == 4 /* Z_FINISH */ ? kBufError : kOk;
}

// ---- the engine's z_stream->reserved tagging ----------------------------------------------------------------------
// reserved = a zlib-ng stream's address, bit 0 set for a raw (windowBits -15) one; with bit 1 set it is the address of a
// State (a zstd stream), which keeps the zlib-ng stream (and its bit 0) it replaced.
constexpr std::uint64_t kTagZstd = 2, kTagMask = 3;
inline State* StateOf(std::uint64_t tagged) { return reinterpret_cast<State*>(tagged & ~kTagMask); }

/// The inflate hook's first step: a stream whose first input is a zstd frame is decoded by zstd; a zlib one is left to
/// zlib-ng / the game's inflate (false returned). A stream that was reset decides again on its next first input.
/// `reserved` must be non-zero (a zlib-ng stream was made for the engine's stream).
inline bool Route(BbFastInflate::GuestZ* z, int flush, Pool& pool, std::int64_t* rc, Stats* stats = nullptr) {
    bool zs = (z->reserved & kTagZstd) != 0;
    if (z->total_in == 0 && z->avail_in > 0) {
        const bool magic = IsZstd(z->next_in, z->avail_in);
        if (magic && !zs) {
            auto* st = new State;
            st->ctx = pool.Acquire();
            st->raw = (z->reserved & 1) != 0;
            st->zng = reinterpret_cast<void*>(z->reserved & ~kTagMask);
            z->reserved = reinterpret_cast<std::uint64_t>(st) | kTagZstd;
            if (stats) stats->streams++;
            zs = true;
        } else if (!magic && zs) {
            State* st = StateOf(z->reserved);
            z->reserved = reinterpret_cast<std::uint64_t>(st->zng) | (st->raw ? 1 : 0);
            pool.Release(st->ctx);
            delete st;
            zs = false;
        }
    }
    if (!zs) return false;
    const std::uint64_t in0 = z->total_in, out0 = z->total_out;
    *rc = Step(z, StateOf(z->reserved), flush);
    if (stats) stats->in_bytes += z->total_in - in0, stats->out_bytes += z->total_out - out0;
    return true;
}

/// inflateEnd: a zstd stream goes back to what it was (its zlib-ng stream, tagged as before) for the caller to release.
inline void Unroute(BbFastInflate::GuestZ* z, Pool& pool) {
    if (!(z->reserved & kTagZstd)) return;
    State* st = StateOf(z->reserved);
    pool.Release(st->ctx);
    z->reserved = reinterpret_cast<std::uint64_t>(st->zng) | (st->raw ? 1 : 0);
    delete st;
}

}  // namespace BbZstdInflate
