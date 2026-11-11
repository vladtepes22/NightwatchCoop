// Zstd behind the engine's inflate (gpu/shim/bbport_zstd_inflate.h): zstd frames through the hooked z_stream contract.
// Streams are made here at test time (no game data). The call pattern is the engine reader's: inflate(z, 3) on input
// chunks into an output buffer of a given size. Covered: chunked input and output, truncated streams, corrupt data,
// zlib and zstd streams mixed on one z_stream across resets (the router decides on each stream's first input).
// ninja -C out/gpu zstd-inflate-test
#include "bbport_zstd_inflate.h"

#include <zlib-ng.h>
#include <zlib.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using BbFastInflate::GuestZ;
using Bytes = std::vector<std::uint8_t>;

namespace {

Bytes MakeData(std::size_t n, std::uint32_t seed) {
    Bytes d(n);
    std::uint32_t x = seed * 2654435761u + 1;
    auto rnd = [&] {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return x;
    };
    std::size_t i = 0;
    while (i < n) {
        const std::uint32_t kind = rnd() % 4;
        const std::size_t run = 1 + rnd() % 300;
        for (std::size_t k = 0; k < run && i < n; ++k, ++i) {
            if (kind == 0) d[i] = std::uint8_t("the quick brown fox jumps"[(i + seed) % 25]);
            else if (kind == 1 && i > 40000) d[i] = d[i - 1 - (rnd() % 32768)];
            else if (kind == 2 && i > 3) d[i] = d[i - 1 - (rnd() % 3)];
            else d[i] = std::uint8_t(rnd());
        }
    }
    return d;
}

Bytes Zstd(const Bytes& in, int level, bool checksum) {
    ZSTD_CCtx* c = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, checksum ? 1 : 0);
    ZSTD_CCtx_setPledgedSrcSize(c, in.size());
    Bytes out(ZSTD_compressBound(in.size()));
    const std::size_t n = ZSTD_compress2(c, out.data(), out.size(), in.data(), in.size());
    assert(!ZSTD_isError(n));
    out.resize(n);
    ZSTD_freeCCtx(c);
    return out;
}

Bytes Zlib(const Bytes& in, int level) {  // windowBits 15: the header and adler32 trailer the engine's streams have
    uLongf n = compressBound(uLong(in.size()));
    Bytes out(n);
    assert(compress2(out.data(), &n, in.data(), uLong(in.size()), level) == Z_OK);
    out.resize(n);
    return out;
}

struct Call {
    int rc;
    std::uint32_t consumed, produced;
};
struct Result {
    Bytes out;
    std::vector<Call> calls;
    std::uint64_t total_in = 0, total_out = 0;
};

// One stream through the hook's routing: reserved starts as a zlib-ng stream (what the engine's inflateInit2_ hook
// gives it); the router moves it to zstd on a zstd frame.
struct Hooked {
    BbFastInflate::Pool zng{BbFastInflate::kZlibWindowBits};
    BbZstdInflate::Pool zpool;
    GuestZ z;
    Hooked() {
        std::memset(&z, 0, sizeof(z));
        z.reserved = reinterpret_cast<std::uint64_t>(zng.Acquire());
    }
    zng_stream* Zng() const {
        return reinterpret_cast<zng_stream*>((z.reserved & BbZstdInflate::kTagZstd)
                                                  ? reinterpret_cast<std::uint64_t>(BbZstdInflate::StateOf(z.reserved)->zng)
                                                  : (z.reserved & ~BbZstdInflate::kTagMask));
    }
    bool IsZstd() const { return (z.reserved & BbZstdInflate::kTagZstd) != 0; }
    int Inflate(int flush) {
        std::int64_t rc;
        if (BbZstdInflate::Route(&z, flush, zpool, &rc)) return int(rc);
        return BbFastInflate::Step(&z, reinterpret_cast<zng_stream*>(z.reserved & ~BbZstdInflate::kTagMask), flush);
    }
    // inflateReset: the engine's own reset clears the totals; ours resets the streams.
    void Reset() {
        z.total_in = z.total_out = 0;
        z.avail_in = z.avail_out = 0;
        if (IsZstd()) BbZstdInflate::Reset(BbZstdInflate::StateOf(z.reserved));
        zng_inflateReset(Zng());
    }
    ~Hooked() {
        BbZstdInflate::Unroute(&z, zpool);
        zng.Release(reinterpret_cast<zng_stream*>(z.reserved & ~BbZstdInflate::kTagMask));
    }
};

// The engine reader's loop (see test_fast_inflate.cpp).
Result Run(Hooked& h, const Bytes& stream, std::size_t chunk, std::size_t out_size) {
    Result r;
    Bytes buf(out_size);
    std::size_t fed = 0;
    GuestZ* z = &h.z;
    z->next_in = stream.data();
    z->avail_in = 0;
    for (int guard = 0; guard < 20000000; ++guard) {
        if (z->avail_in == 0 && fed < stream.size()) {
            const std::size_t n = std::min(chunk, stream.size() - fed);
            z->next_in = stream.data() + fed;
            z->avail_in = std::uint32_t(n);
            fed += n;
        }
        z->next_out = buf.data();
        z->avail_out = std::uint32_t(buf.size());
        const std::uint32_t in0 = z->avail_in;
        const int rc = h.Inflate(3);
        const std::uint32_t produced = std::uint32_t(buf.size()) - z->avail_out;
        r.out.insert(r.out.end(), buf.begin(), buf.begin() + produced);
        r.calls.push_back({rc, in0 - z->avail_in, produced});
        if (rc != 0) break;
    }
    r.total_in = z->total_in;
    r.total_out = z->total_out;
    return r;
}

int g_failures = 0;
void Check(bool ok, const char* what, std::size_t size, std::size_t chunk, std::size_t out_size) {
    if (!ok) {
        std::printf("FAIL: %s (data %zu, chunk %zu, out %zu)\n", what, size, chunk, out_size);
        ++g_failures;
    }
}

}  // namespace

int main() {
    static_assert(sizeof(GuestZ) == 0x70, "layout");
    const std::size_t sizes[] = {0, 1, 100, 5000, 70000, 1500000};
    const std::size_t chunks[] = {1, 7, 4096, 65536, std::size_t(1) << 30};
    const std::size_t outs[] = {1, 100, 4096, 1 << 17, 1 << 21};
    std::uint32_t seed = 1;

    // 1. Whole streams, every input / output granularity: the bytes, Z_STREAM_END, totals, nothing left over.
    for (const std::size_t size : sizes) {
        const Bytes data = MakeData(size, seed++);
        for (const int level : {1, 19}) {
            const Bytes stream = Zstd(data, level, false);
            for (const std::size_t chunk : chunks) {
                for (const std::size_t out : outs) {
                    if (size > 100000 && (chunk < 4096 || out < 4096)) continue;
                    if (size > 5000 && chunk == 1 && out < 100) continue;
                    Hooked h;
                    const Result r = Run(h, stream, chunk, out);
                    Check(h.IsZstd(), "stream was not routed to zstd", size, chunk, out);
                    Check(r.out == data, "output differs", size, chunk, out);
                    Check(!r.calls.empty() && r.calls.back().rc == Z_STREAM_END, "no Z_STREAM_END", size, chunk, out);
                    Check(r.total_in == stream.size() && r.total_out == data.size(), "totals", size, chunk, out);
                    for (std::size_t i = 0; i + 1 < r.calls.size(); ++i)
                        Check(r.calls[i].rc == Z_OK, "a call before the end did not return Z_OK", size, chunk, out);
                }
            }
        }
    }

    // 2. Bytes after the frame stay in avail_in (the reader hands more than the stream).
    {
        const Bytes data = MakeData(30000, 99);
        Bytes stream = Zstd(data, 3, false);
        const std::size_t clean = stream.size();
        stream.insert(stream.end(), {1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
        Hooked h;
        const Result r = Run(h, stream, std::size_t(1) << 30, 1 << 20);
        Check(r.out == data && r.calls.back().rc == Z_STREAM_END, "trailing bytes: output or end", 0, 0, 0);
        Check(h.z.avail_in == 10 && h.z.total_in == clean, "trailing bytes were consumed", 0, 0, 0);
        // and a call after the end keeps answering Z_STREAM_END without producing
        h.z.next_out = reinterpret_cast<std::uint8_t*>(&h);
        h.z.avail_out = 16;
        Check(h.Inflate(3) == Z_STREAM_END && h.z.avail_out == 16, "call after the end", 0, 0, 0);
    }

    // 3. Truncated: every cut point gives the prefix of the output, never Z_STREAM_END, then Z_BUF_ERROR (no more input).
    {
        const Bytes data = MakeData(200000, 5);
        const Bytes stream = Zstd(data, 3, false);
        for (const std::size_t cut : {std::size_t(0), std::size_t(3), std::size_t(4), std::size_t(10), stream.size() / 3,
                                      stream.size() / 2, stream.size() - 4, stream.size() - 1}) {
            Bytes part(stream.begin(), stream.begin() + cut);
            Hooked h;
            const Result r = Run(h, part, 4096, 1 << 16);
            Check(!r.calls.empty() && r.calls.back().rc == Z_BUF_ERROR, "truncated: last code is not Z_BUF_ERROR", cut, 0, 0);
            for (const Call& c : r.calls) Check(c.rc != Z_STREAM_END, "truncated: Z_STREAM_END", cut, 0, 0);
            Check(r.out.size() <= data.size() && std::equal(r.out.begin(), r.out.end(), data.begin()),
                  "truncated: output is not a prefix", cut, 0, 0);
        }
        // Z_FINISH on a truncated stream cannot finish: Z_BUF_ERROR, as zlib does
        Bytes part(stream.begin(), stream.begin() + stream.size() / 2);
        Hooked h;
        Bytes buf(data.size());
        h.z.next_in = part.data();
        h.z.avail_in = std::uint32_t(part.size());
        h.z.next_out = buf.data();
        h.z.avail_out = std::uint32_t(buf.size());
        Check(h.Inflate(4) == Z_BUF_ERROR, "Z_FINISH on a truncated stream", 0, 0, 0);
    }

    // 4. Corrupt: damaged streams end in Z_DATA_ERROR (never a crash, never Z_STREAM_END with other bytes), and stay failed.
    {
        const Bytes data = MakeData(300000, 7);
        const Bytes stream = Zstd(data, 3, true);  // checksum: even damage inside a literal block is caught
        for (const std::size_t at : {std::size_t(0), std::size_t(2), std::size_t(30), stream.size() / 2, stream.size() - 2}) {
            Bytes bad = stream;
            for (std::size_t k = 0; k < 8 && at + k < bad.size(); ++k) bad[at + k] ^= 0xA5;
            Hooked h;
            const Result r = Run(h, bad, 65536, 1 << 17);
            const int last = r.calls.back().rc;
            const bool ended_ok = last == Z_STREAM_END && r.out == data;
            Check(last == Z_DATA_ERROR || ended_ok || (at == 0 && last == Z_OK) || last == Z_BUF_ERROR,
                  "corrupt: unexpected code", at, 0, 0);
            Check(last != Z_STREAM_END || r.out == data, "corrupt: Z_STREAM_END with wrong bytes", at, 0, 0);
            if (last == Z_DATA_ERROR) {
                Check(h.z.msg != nullptr, "corrupt: no msg", at, 0, 0);
                Check(h.Inflate(3) == Z_DATA_ERROR, "corrupt: not sticky", at, 0, 0);
            }
        }
        // a stream of noise that starts like a frame
        Bytes noise = MakeData(5000, 11);
        noise[0] = 0x28, noise[1] = 0xB5, noise[2] = 0x2F, noise[3] = 0xFD;
        Hooked h;
        const Result r = Run(h, noise, 4096, 1 << 17);
        Check(r.calls.back().rc == Z_DATA_ERROR || r.calls.back().rc == Z_BUF_ERROR, "noise with a frame magic", 0, 0, 0);
    }

    // 5. Mixed zlib and zstd streams on one z_stream, across resets: the router re-decides on every first input.
    {
        Hooked h;
        const Bytes d1 = MakeData(80000, 21), d2 = MakeData(120000, 22), d3 = MakeData(3000, 23), d4 = MakeData(500000, 24);
        const Bytes s1 = Zlib(d1, 6), s2 = Zstd(d2, 3, false), s3 = Zlib(d3, 9), s4 = Zstd(d4, 19, false);
        struct Item {
            const Bytes *data, *stream;
            bool zstd;
        } items[] = {{&d1, &s1, false}, {&d2, &s2, true}, {&d3, &s3, false}, {&d4, &s4, true},
                     {&d2, &s2, true},  {&d1, &s1, false}, {&d1, &s1, false}, {&d4, &s4, true}};
        for (const Item& it : items) {
            const Result r = Run(h, *it.stream, 8192, 1 << 16);
            Check(h.IsZstd() == it.zstd, "mixed: wrong decoder", it.data->size(), 0, 0);
            Check(r.out == *it.data && r.calls.back().rc == Z_STREAM_END, "mixed: output or end", it.data->size(), 0, 0);
            Check(r.total_in == it.stream->size(), "mixed: total_in", it.data->size(), 0, 0);
            h.Reset();
        }
        // a zstd stream that was never finished, then reset and a zlib stream
        const Bytes half(s4.begin(), s4.begin() + s4.size() / 2);
        Run(h, half, 8192, 1 << 16);
        h.Reset();
        const Result r = Run(h, s1, 8192, 1 << 16);
        Check(!h.IsZstd() && r.out == d1 && r.calls.back().rc == Z_STREAM_END, "mixed: zlib after an abandoned zstd", 0, 0, 0);
    }

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("zstd inflate: ok\n");
    return 0;
}
