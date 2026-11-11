// Fast inflate (gpu/shim/bbport_fast_inflate.h): the engine's z_stream contract stepped by zlib-ng against zlib.
// Streams are made here at test time (no game data): raw deflate (windowBits -15, what the engine's readers use) of
// generated data at several levels and sizes. Each stream is inflated both ways with the engine reader's call
// pattern (inflate(z, 3) on input chunks into an output buffer of a given size) and compared: the bytes, the return
// code of every call, the bytes consumed and produced by every call, and the z_stream fields the engine reads.
// ninja -C out/gpu fast-inflate-test
#include "bbport_fast_inflate.h"

#include <zlib.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using BbFastInflate::GuestZ;
using Bytes = std::vector<std::uint8_t>;

namespace {

// A deterministic mix: text-like runs, repeats at all distances, and noise.
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

Bytes Deflate(const Bytes& in, int level, int bits = -15) {
    z_stream z;
    std::memset(&z, 0, sizeof(z));
    assert(deflateInit2(&z, level, Z_DEFLATED, bits, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    Bytes out(deflateBound(&z, uLong(in.size())) + 64);
    z.next_in = const_cast<Bytef*>(in.data());
    z.avail_in = uInt(in.size());
    z.next_out = out.data();
    z.avail_out = uInt(out.size());
    assert(deflate(&z, Z_FINISH) == Z_STREAM_END);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

struct Call {
    int rc;
    std::uint32_t consumed, produced;
    bool operator==(const Call& o) const { return rc == o.rc && consumed == o.consumed && produced == o.produced; }
};
int g_bits = -15;  // windowBits of the streams under test: -15 raw, 15 zlib header + adler32 (the engine's readers)
struct Result {
    std::uint64_t adler = 0;
    Bytes out;
    std::vector<Call> calls;
    std::uint64_t total_in = 0, total_out = 0;
};

// The engine reader's loop: feed `chunk` bytes of input, call inflate(z, 3) into `out_size` bytes of output until
// the input chunk is used up and the output is not full, or the stream ends.
template <class Inflate>
Result Run(const Bytes& stream, std::size_t chunk, std::size_t out_size, Inflate step, GuestZ* z) {
    Result r;
    Bytes buf(out_size);
    std::size_t fed = 0;  // bytes of `stream` handed to the reader so far
    z->next_in = stream.data();
    z->avail_in = 0;
    for (int guard = 0; guard < 20000000; ++guard) {
        if (z->avail_in == 0 && fed < stream.size()) {  // the reader's next input chunk
            const std::size_t n = std::min(chunk, stream.size() - fed);
            z->next_in = stream.data() + fed;
            z->avail_in = std::uint32_t(n);
            fed += n;
        }
        z->next_out = buf.data();
        z->avail_out = std::uint32_t(buf.size());
        const std::uint32_t in0 = z->avail_in;
        const int rc = step(z, 3);
        const std::uint32_t produced = std::uint32_t(buf.size()) - z->avail_out;
        r.out.insert(r.out.end(), buf.begin(), buf.begin() + produced);
        r.calls.push_back({rc, in0 - z->avail_in, produced});
        if (rc != Z_OK) break;  // Z_STREAM_END, or an error / no progress (Z_BUF_ERROR)
    }
    r.total_in = z->total_in;
    r.total_out = z->total_out;
    r.adler = z->adler;
    return r;
}

Result ViaZlib(const Bytes& stream, std::size_t chunk, std::size_t out_size) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    assert(inflateInit2(&zs, g_bits) == Z_OK);
    GuestZ g;
    std::memset(&g, 0, sizeof(g));
    // The same struct shape for both: zlib's z_stream is stepped through the guest-shaped fields.
    auto step = [&](GuestZ* z, int flush) {
        zs.next_in = const_cast<Bytef*>(z->next_in);
        zs.avail_in = z->avail_in;
        zs.next_out = z->next_out;
        zs.avail_out = z->avail_out;
        const int rc = inflate(&zs, flush);
        z->total_in += z->avail_in - zs.avail_in;
        z->total_out += z->avail_out - zs.avail_out;
        z->next_in = zs.next_in;
        z->avail_in = zs.avail_in;
        z->next_out = zs.next_out;
        z->avail_out = zs.avail_out;
        z->adler = zs.adler;
        return rc;
    };
    Result r = Run(stream, chunk, out_size, step, &g);
    inflateEnd(&zs);
    return r;
}

Result ViaZlibNg(BbFastInflate::Pool& pool, const Bytes& stream, std::size_t chunk, std::size_t out_size) {
    zng_stream* s = pool.Acquire();
    assert(s);
    GuestZ g;
    std::memset(&g, 0, sizeof(g));
    Result r = Run(stream, chunk, out_size, [&](GuestZ* z, int flush) { return BbFastInflate::Step(z, s, flush); }, &g);
    pool.Release(s);  // back to the pool reset, as the engine's inflateEnd does
    return r;
}

int g_failures = 0;
void Check(bool ok, const char* what, std::size_t size, int level, std::size_t chunk, std::size_t out_size) {
    if (!ok) {
        std::printf("FAIL: %s (data %zu, level %d, chunk %zu, out %zu)\n", what, size, level, chunk, out_size);
        ++g_failures;
    }
}

void Compare(BbFastInflate::Pool& pool, const Bytes& original, const Bytes& stream, int level, bool expect_end,
             std::size_t chunk, std::size_t out_size) {
    const Result a = ViaZlib(stream, chunk, out_size);
    const Result b = ViaZlibNg(pool, stream, chunk, out_size);
    Check(a.out == b.out, "output bytes differ between zlib and zlib-ng", original.size(), level, chunk, out_size);
    Check(a.calls.size() == b.calls.size() && a.calls == b.calls, "per-call (code, consumed, produced) differ",
          original.size(), level, chunk, out_size);
    Check(a.total_in == b.total_in && a.total_out == b.total_out && a.adler == b.adler, "total_in / total_out / adler differ", original.size(), level,
          chunk, out_size);
    if (expect_end) {
        Check(b.out == original, "zlib-ng output is not the original data", original.size(), level, chunk, out_size);
        Check(!b.calls.empty() && b.calls.back().rc == Z_STREAM_END, "no Z_STREAM_END", original.size(), level, chunk,
              out_size);
        Check(b.total_in == stream.size() && b.total_out == original.size(), "totals are not the stream's sizes",
              original.size(), level, chunk, out_size);
    }
}

}  // namespace

int main() {
    static_assert(sizeof(GuestZ) == 0x70, "layout");
  for (const int bits : {-15, 15}) {
    g_bits = bits;
    BbFastInflate::Pool pool(bits);
    std::printf("windowBits %d\n", bits);
    const std::size_t sizes[] = {0, 1, 100, 5000, 70000, 1500000};
    const int levels[] = {0, 1, 6, 9};
    const std::size_t chunks[] = {1, 7, 4096, 65536, std::size_t(1) << 30};
    const std::size_t outs[] = {1, 100, 4096, 1 << 17, 1 << 21};
    std::uint32_t seed = 1;
    for (const std::size_t size : sizes) {
        const Bytes data = MakeData(size, seed++);
        for (const int level : levels) {
            const Bytes stream = Deflate(data, level, g_bits);
            for (const std::size_t chunk : chunks) {
                for (const std::size_t out : outs) {
                    // one-byte chunks / outputs of a megabyte-sized stream would run for minutes: sample them
                    if (size > 100000 && (chunk < 4096 || out < 4096)) continue;
                    if (size > 5000 && chunk == 1 && out < 100) continue;
                    Compare(pool, data, stream, level, true, chunk, out);
                }
            }
        }
    }
    // A stream with bytes after its end: the engine's reader hands inflate more than the stream; the unused tail must
    // be left in avail_in the same way.
    {
        const Bytes data = MakeData(30000, 99);
        Bytes stream = Deflate(data, 6, g_bits);
        stream.insert(stream.end(), {1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
        const Result a = ViaZlib(stream, 1 << 30, 1 << 20);
        const Result b = ViaZlibNg(pool, stream, 1 << 30, 1 << 20);
        Check(a.out == data && b.out == data, "tail: output", data.size(), 6, 0, 0);
        Check(a.calls == b.calls && a.total_in == b.total_in, "tail: consumed input", data.size(), 6, 0, 0);
    }
    // A truncated stream and a corrupt one: the same codes, no output beyond what zlib makes.
    {
        const Bytes data = MakeData(40000, 5);
        const Bytes stream = Deflate(data, 6, g_bits);
        const Bytes cut(stream.begin(), stream.begin() + stream.size() / 2);
        for (const std::size_t chunk : {std::size_t(4096), std::size_t(1) << 30}) {
            const Result a = ViaZlib(cut, chunk, 1 << 16);
            const Result b = ViaZlibNg(pool, cut, chunk, 1 << 16);
            Check(a.out == b.out && a.calls == b.calls, "truncated stream differs", data.size(), 6, chunk, 1 << 16);
            Check(!b.calls.empty() && b.calls.back().rc != Z_STREAM_END, "truncated stream ended", data.size(), 6, chunk, 0);
        }
        Bytes bad = stream;
        bad[0] = 0xff;  // block type 3: invalid
        const Result a = ViaZlib(bad, 4096, 1 << 16);
        const Result b = ViaZlibNg(pool, bad, 4096, 1 << 16);
        Check(a.calls.back().rc == Z_DATA_ERROR && b.calls.back().rc == Z_DATA_ERROR, "corrupt stream: Z_DATA_ERROR",
              data.size(), 6, 4096, 1 << 16);
        Check(a.out == b.out, "corrupt stream output", data.size(), 6, 4096, 1 << 16);
    }
    // A pooled stream is reused: a second stream through the same pool entry is still right.
    for (int i = 0; i < 3; ++i) {
        const Bytes data = MakeData(20000 + i, 200 + i);
        Compare(pool, data, Deflate(data, 6, g_bits), 6, true, 4096, 1 << 16);
    }
  }
    if (g_failures) {
        std::printf("fast-inflate-test: %d failures\n", g_failures);
        return 1;
    }
    std::printf("fast-inflate-test: ok (zlib-ng %s against zlib %s)\n", zlibng_version(), zlibVersion());
    return 0;
}
