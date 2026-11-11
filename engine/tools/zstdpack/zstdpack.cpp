// SPDX-License-Identifier: GPL-3.0-or-later
// bb-zstdpack (zstd081): recompress the zlib streams inside the game's DCX files with zstd, once, on the player's PC.
// The engine's inflate hook (gpu/shim/bbport_zstd_inflate.h) recognises the zstd frame and decodes it faster than
// zlib-ng decodes the original, which shortens map loads.
//
//   bb-zstdpack convert --game <game folder> --out <output folder> [--level 9] [--window-log 0] [--threads N]
//                       [--filter TEXT] [--limit N] [--force]
//   bb-zstdpack verify  --game <game folder> --out <output folder> [--threads N]
//
// Reads <game>/dvdroot_ps4/**/*.dcx; never writes there. Each file that is a plain DCX "DFLT" container (header of
// 0x4c bytes, one zlib stream behind it) is written to <out>/dvdroot_ps4/<same path> with the same header, the
// compressed size (DCS, at +0x20) rewritten and a zstd frame in place of the zlib stream. Every other file stays
// where it is (the game folder serves it). <out>/bbzstd.manifest lists the converted files (relative path, size and
// modification time of the original, converted size): the game uses a converted file only while the original still
// has that size and time, so a mod that replaces a file wins. Resumable: a rerun skips what the manifest and the
// output already hold. Delete <out> to go back.
#include <zlib-ng.h>
#include <zstd.h>

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace {

constexpr std::size_t kHeader = 0x4c;
constexpr const char* kManifest = "bbzstd.manifest";

// The DCX layout this tool converts (every .dcx of the PS4 Bloodborne dump has it): a 0x4c byte header, the zlib stream
// behind it. Bytes +0x10 / +0x14 (the DCP / DCA offsets of the 0x44 / 0x4c headers, 0x24 / 0x2c in some files) are
// kept as they are, whatever they hold; the sizes at +0x1c (uncompressed) and +0x20 (compressed) are per file.
bool HeaderOk(const std::uint8_t* h, std::uint32_t* usize, std::uint32_t* csize) {
    static const std::uint8_t head[0x10] = {'D', 'C', 'X', 0, 0, 1, 0, 0, 0, 0, 0, 0x18, 0, 0, 0, 0x24};
    if (std::memcmp(h, head, sizeof(head)) != 0 || std::memcmp(h + 0x18, "DCS", 3) != 0 || h[0x1b] != 0) return false;
    // DCP block at +0x24: "DCP\0" "DFLT" 00 00 00 20 09 00 00 00 ..., then DCA at +0x44 "DCA\0" 00 00 00 08
    if (std::memcmp(h + 0x24, "DCP\0DFLT", 8) != 0 || std::memcmp(h + 0x44, "DCA\0\0\0\0\x08", 8) != 0) return false;
    auto be = [&](int o) {
        return (std::uint32_t(h[o]) << 24) | (std::uint32_t(h[o + 1]) << 16) | (std::uint32_t(h[o + 2]) << 8) | h[o + 3];
    };
    *usize = be(0x1c);
    *csize = be(0x20);
    return true;
}
void PutBe(std::uint8_t* p, std::uint32_t v) {
    p[0] = std::uint8_t(v >> 24), p[1] = std::uint8_t(v >> 16), p[2] = std::uint8_t(v >> 8), p[3] = std::uint8_t(v);
}

bool ReadFile(const fs::path& p, Bytes* out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    out->resize(std::size_t(n));
    f.seekg(0);
    return n == 0 || bool(f.read(reinterpret_cast<char*>(out->data()), n));
}
bool WriteFile(const fs::path& p, const Bytes& a, const Bytes* b = nullptr) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(a.data()), std::streamsize(a.size()));
    if (b) f.write(reinterpret_cast<const char*>(b->data()), std::streamsize(b->size()));
    f.flush();
    return bool(f);
}

struct Stat {
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    bool ok = false;
};
Stat StatOf(const fs::path& p) {
    Stat s;
#ifdef _WIN32
    struct _stat64 st;
    if (_wstat64(p.c_str(), &st) == 0) {
#else
    struct stat st;
    if (stat(p.c_str(), &st) == 0) {
#endif
        s.size = std::uint64_t(st.st_size);
        s.mtime = std::int64_t(st.st_mtime);
        s.ok = true;
    }
    return s;
}

// The original's zlib stream -> the uncompressed bytes (exactly usize of them, stream ended, nothing left over).
bool Inflate(const std::uint8_t* in, std::size_t n, std::size_t usize, Bytes* out) {
    out->resize(usize);
    zng_stream z{};
    if (zng_inflateInit(&z) != Z_OK) return false;
    z.next_in = in;
    z.avail_in = std::uint32_t(n);
    z.next_out = out->data();
    z.avail_out = std::uint32_t(usize);
    const int rc = zng_inflate(&z, Z_FINISH);
    const bool ok = rc == Z_STREAM_END && z.avail_in == 0 && z.avail_out == 0;
    zng_inflateEnd(&z);
    return ok;
}

struct Entry {
    std::string rel;  // path under dvdroot_ps4, '/' separators
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::uint64_t new_size = 0;
};

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

std::string Gb(double bytes) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.1f GB", bytes / 1073741824.0);
    return b;
}

struct Options {
    fs::path game, out;
    int level = 9, window_log = 0, threads = 0;
    std::string filter;
    std::size_t limit = 0;
    bool force = false;
    double max_ratio = 1.3;  // keep the original when the zstd file is larger than this times the original
};

bool ParseArgs(int argc, char** argv, Options* o, std::string* cmd) {
    if (argc < 2) return false;
    *cmd = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--game") o->game = val();
        else if (a == "--out") o->out = val();
        else if (a == "--level") o->level = std::atoi(val());
        else if (a == "--window-log") o->window_log = std::atoi(val());
        else if (a == "--threads") o->threads = std::atoi(val());
        else if (a == "--filter") o->filter = Lower(val());
        else if (a == "--limit") o->limit = std::size_t(std::atoll(val()));
        else if (a == "--force") o->force = true;
        else if (a == "--max-ratio") o->max_ratio = std::atof(val());
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return false;
        }
    }
    return !o->game.empty() && !o->out.empty() && (*cmd == "convert" || *cmd == "verify" || *cmd == "bench");
}

int ThreadCount(int asked) {
    if (asked > 0) return asked;
    const unsigned n = std::thread::hardware_concurrency();
    return int(n > 2 ? n - 1 : 1);  // leave one logical CPU for the player
}

// ---- manifest -------------------------------------------------------------------------------------------------
std::map<std::string, Entry> ReadManifest(const fs::path& out, bool* done) {
    std::map<std::string, Entry> m;
    *done = false;
    std::ifstream f(out / kManifest);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("F\t", 0) == 0) {
            Entry e;
            unsigned long long sz, nsz;
            long long mt;
            const std::size_t tab = line.find('\t', 2);
            if (tab == std::string::npos) continue;
            e.rel = line.substr(2, tab - 2);
            if (std::sscanf(line.c_str() + tab + 1, "%llu\t%lld\t%llu", &sz, &mt, &nsz) != 3) continue;
            e.size = sz, e.mtime = mt, e.new_size = nsz;
            m[e.rel] = e;
        } else if (line.rfind("DONE", 0) == 0) {
            *done = true;
        } else if (line.rfind("bbzstd", 0) == 0) {
            *done = false;
        }
    }
    return m;
}

// ---- convert --------------------------------------------------------------------------------------------------
struct Progress {
    std::atomic<std::uint64_t> files{0}, in_bytes{0}, out_bytes{0}, skipped{0}, failed{0}, kept{0}, unsupported{0};
};

int Convert(const Options& o) {
    const fs::path root = o.game / "dvdroot_ps4";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        std::fprintf(stderr, "%s is not a game folder (no dvdroot_ps4)\n", o.game.string().c_str());
        return 2;
    }
    if (fs::equivalent(o.out, o.game, ec) || fs::equivalent(o.out, root, ec)) {
        std::fprintf(stderr, "the output folder must not be the game folder\n");
        return 2;
    }
    std::printf("Listing %s ...\n", root.string().c_str());
    std::fflush(stdout);
    std::vector<Entry> todo;
    std::uint64_t total = 0;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        if (Lower(p.extension().string()) != ".dcx" || !it->is_regular_file(ec)) continue;
        Entry e;
        e.rel = p.lexically_relative(root).generic_string();
        if (!o.filter.empty() && Lower(e.rel).find(o.filter) == std::string::npos) continue;
        const Stat st = StatOf(p);
        if (!st.ok) continue;
        e.size = st.size, e.mtime = st.mtime;
        todo.push_back(e);
        total += e.size;
    }
    std::sort(todo.begin(), todo.end(), [](const Entry& a, const Entry& b) { return a.size > b.size; });
    if (o.limit && todo.size() > o.limit) {
        todo.resize(o.limit);
        total = 0;
        for (const Entry& e : todo) total += e.size;
    }
    fs::create_directories(o.out, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create %s: %s\n", o.out.string().c_str(), ec.message().c_str());
        return 2;
    }
    bool was_done = false;
    std::map<std::string, Entry> have = ReadManifest(o.out, &was_done);
    std::uint64_t remaining = 0;
    for (const Entry& e : todo) {
        auto h = have.find(e.rel);
        const bool kept = h != have.end() && h->second.size == e.size && h->second.mtime == e.mtime &&
                          StatOf(o.out / "dvdroot_ps4" / e.rel).size == h->second.new_size;
        if (!kept) remaining += e.size;
    }
    // Free space: the converted files are about 0.8-0.9 of the originals; plan for 1.0 and a margin.
    const fs::space_info sp = fs::space(o.out, ec);
    const double need = double(remaining) * 1.0 + 1073741824.0;
    std::printf("%zu DCX files, %s in the game; %s to convert (%.1f%% already done). Needs up to about %s of free space "
                "in %s, %s free.\n",
                todo.size(), Gb(double(total)).c_str(), Gb(double(remaining)).c_str(),
                total ? 100.0 * double(total - remaining) / double(total) : 100.0, Gb(need).c_str(),
                o.out.string().c_str(), ec ? "unknown" : Gb(double(sp.available)).c_str());
    std::fflush(stdout);
    if (!ec && double(sp.available) < need && !o.force) {
        std::fprintf(stderr, "not enough free space (use --force to try anyway)\n");
        return 3;
    }

    std::mutex mu;
    std::ofstream man(o.out / kManifest, std::ios::trunc);
    {
        std::lock_guard<std::mutex> lk(mu);
        man << "bbzstd 1 level=" << o.level << " windowlog=" << o.window_log << "\n";
        for (const auto& kv : have) {  // keep the earlier entries that are still valid; a changed one is redone
            const Entry& e = kv.second;
            man << "F\t" << e.rel << "\t" << e.size << "\t" << e.mtime << "\t" << e.new_size << "\n";
        }
        man.flush();
    }

    Progress pr;
    std::atomic<std::size_t> next{0};
    const auto t0 = std::chrono::steady_clock::now();
    const int nthreads = ThreadCount(o.threads);
    std::printf("Converting with %d threads, zstd level %d ...\n", nthreads, o.level);
    std::fflush(stdout);

    auto worker = [&] {
        ZSTD_CCtx* cctx = ZSTD_createCCtx();
        Bytes src, raw, frame;
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= todo.size()) break;
            Entry e = todo[i];
            const fs::path in_path = root / e.rel;
            const fs::path out_path = o.out / "dvdroot_ps4" / e.rel;
            auto h = have.find(e.rel);
            if (h != have.end() && h->second.size == e.size && h->second.mtime == e.mtime &&
                StatOf(out_path).size == h->second.new_size) {
                pr.skipped++;
                pr.files++;
                pr.in_bytes += e.size;
                pr.out_bytes += h->second.new_size;
                continue;
            }
            std::uint32_t usize = 0, csize = 0;
            if (!ReadFile(in_path, &src) || src.size() < kHeader || !HeaderOk(src.data(), &usize, &csize) ||
                kHeader + std::uint64_t(csize) != src.size()) {
                pr.unsupported++;  // another layout: the game folder serves it as it is
                pr.files++;
                continue;
            }
            if (!Inflate(src.data() + kHeader, csize, usize, &raw)) {
                std::fprintf(stderr, "FAILED %s: the original does not inflate to its stated size\n", e.rel.c_str());
                pr.failed++;
                pr.files++;
                continue;
            }
            ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
            ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, o.level);
            ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 0);
            ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 1);
            if (o.window_log) ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, o.window_log);
            ZSTD_CCtx_setPledgedSrcSize(cctx, raw.size());
            frame.resize(ZSTD_compressBound(raw.size()));
            const std::size_t zn = ZSTD_compress2(cctx, frame.data(), frame.size(), raw.data(), raw.size());
            if (ZSTD_isError(zn)) {
                std::fprintf(stderr, "FAILED %s: %s\n", e.rel.c_str(), ZSTD_getErrorName(zn));
                pr.failed++;
                pr.files++;
                continue;
            }
            frame.resize(zn);
            if (double(zn) > double(csize) * o.max_ratio) {  // too large: leave the original
                pr.kept++;
                pr.files++;
                pr.in_bytes += e.size;
                pr.out_bytes += e.size;
                continue;
            }
            // Verify before it is written: the frame must decode to the same bytes.
            Bytes check(raw.size());
            const std::size_t dn = ZSTD_decompress(check.data(), check.size(), frame.data(), frame.size());
            if (dn != raw.size() || (!raw.empty() && std::memcmp(check.data(), raw.data(), raw.size()) != 0)) {
                std::fprintf(stderr, "FAILED %s: the zstd frame does not decode to the original bytes\n", e.rel.c_str());
                pr.failed++;
                pr.files++;
                continue;
            }
            Bytes head(src.begin(), src.begin() + kHeader);
            PutBe(head.data() + 0x20, std::uint32_t(zn));
            std::error_code ec2;
            fs::create_directories(out_path.parent_path(), ec2);
            const fs::path tmp = out_path.string() + ".tmp";
            if (!WriteFile(tmp, head, &frame)) {
                std::fprintf(stderr, "FAILED %s: cannot write %s (disk full?)\n", e.rel.c_str(), tmp.string().c_str());
                fs::remove(tmp, ec2);
                pr.failed++;
                pr.files++;
                continue;
            }
            fs::rename(tmp, out_path, ec2);
            if (ec2) {
                fs::remove(out_path, ec2);
                fs::rename(tmp, out_path, ec2);
            }
            if (ec2) {
                std::fprintf(stderr, "FAILED %s: cannot move into place: %s\n", e.rel.c_str(), ec2.message().c_str());
                pr.failed++;
                pr.files++;
                continue;
            }
            const std::uint64_t new_size = kHeader + zn;
            {
                std::lock_guard<std::mutex> lk(mu);
                man << "F\t" << e.rel << "\t" << e.size << "\t" << e.mtime << "\t" << new_size << "\n";
                man.flush();
            }
            pr.files++;
            pr.in_bytes += e.size;
            pr.out_bytes += new_size;
        }
        ZSTD_freeCCtx(cctx);
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < nthreads; ++t) pool.emplace_back(worker);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const std::uint64_t f = pr.files.load();
        const double done_in = double(pr.in_bytes.load());
        const double rate = el > 0 ? done_in / el : 0;
        const double eta = rate > 0 ? std::max(0.0, double(total) - done_in) / rate : 0;
        std::printf("\rProgress: %llu of %zu files, %s of %s read, %s written, about %d min %02d s left   ",
                    (unsigned long long)f, todo.size(), Gb(done_in).c_str(), Gb(double(total)).c_str(),
                    Gb(double(pr.out_bytes.load())).c_str(), int(eta / 60), int(eta) % 60);
        std::fflush(stdout);
        if (f >= todo.size()) break;
    }
    for (auto& t : pool) t.join();
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\nDone in %.0f s: %llu files (%llu already done, %llu kept as they are, %llu other layout), %s -> %s, "
                "%llu failed.\n",
                el, (unsigned long long)pr.files.load(), (unsigned long long)pr.skipped.load(),
                (unsigned long long)pr.kept.load(), (unsigned long long)pr.unsupported.load(),
                Gb(double(pr.in_bytes.load())).c_str(), Gb(double(pr.out_bytes.load())).c_str(),
                (unsigned long long)pr.failed.load());
    if (pr.failed.load() == 0) {
        std::lock_guard<std::mutex> lk(mu);
        man << "DONE\t" << todo.size() << "\n";
        man.flush();
    }
    return pr.failed.load() ? 1 : 0;
}

// ---- verify ---------------------------------------------------------------------------------------------------
// Every manifest entry: the original and the converted file read back from disk; headers equal but the compressed
// size; the frame decodes to exactly the bytes the original's zlib stream inflates to.
int Verify(const Options& o) {
    const fs::path root = o.game / "dvdroot_ps4";
    bool done = false;
    const std::map<std::string, Entry> m = ReadManifest(o.out, &done);
    std::vector<Entry> list;
    for (const auto& kv : m) list.push_back(kv.second);
    std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) { return a.size > b.size; });
    std::printf("Verifying %zu converted files%s ...\n", list.size(), done ? "" : " (the conversion did not finish)");
    std::atomic<std::size_t> next{0}, finished{0}, bad{0};
    std::atomic<std::uint64_t> bytes{0};
    auto worker = [&] {
        Bytes a, b, raw, back;
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= list.size()) break;
            const Entry& e = list[i];
            const char* why = nullptr;
            std::uint32_t usize = 0, csize = 0, usize2 = 0, csize2 = 0;
            if (!ReadFile(root / e.rel, &a) || !ReadFile(o.out / "dvdroot_ps4" / e.rel, &b)) why = "cannot read";
            else if (a.size() < kHeader || b.size() < kHeader) why = "short";
            else if (!HeaderOk(a.data(), &usize, &csize) || !HeaderOk(b.data(), &usize2, &csize2)) why = "header";
            else if (kHeader + std::uint64_t(csize) != a.size() || kHeader + std::uint64_t(csize2) != b.size()) why = "size field";
            else if (usize != usize2) why = "uncompressed size";
            else {
                Bytes ha(a.begin(), a.begin() + kHeader), hb(b.begin(), b.begin() + kHeader);
                PutBe(ha.data() + 0x20, csize2);
                if (ha != hb) why = "header bytes differ";
                else if (!Inflate(a.data() + kHeader, csize, usize, &raw)) why = "original does not inflate";
                else {
                    back.resize(raw.size());
                    const std::size_t dn = ZSTD_decompress(back.data(), back.size(), b.data() + kHeader, csize2);
                    if (ZSTD_isError(dn) || dn != raw.size() || (!raw.empty() && std::memcmp(back.data(), raw.data(), raw.size()) != 0))
                        why = "content differs";
                }
            }
            if (why) {
                std::fprintf(stderr, "VERIFY FAILED %s: %s\n", e.rel.c_str(), why);
                bad++;
            }
            bytes += e.size;
            finished++;
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < ThreadCount(o.threads); ++t) pool.emplace_back(worker);
    while (finished.load() < list.size()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::printf("\rVerified %zu of %zu files, %s read   ", finished.load(), list.size(), Gb(double(bytes.load())).c_str());
        std::fflush(stdout);
    }
    for (auto& t : pool) t.join();
    std::printf("\n%zu files verified, %zu bad.\n", list.size(), bad.load());
    return bad.load() ? 1 : 0;
}

// Decode speed of the original (zlib-ng) against the converted file, per file, from the manifest; for choosing a level.
int Bench(const Options& o) {
    const fs::path root = o.game / "dvdroot_ps4";
    bool done = false;
    const std::map<std::string, Entry> m = ReadManifest(o.out, &done);
    double tz = 0, ts = 0, ub = 0, zb = 0, sb = 0, tz_s = 0, ts_s = 0, ub_s = 0;  // *_s: files whose zstd is not smaller
    double tzs = 0, tss = 0;  // streaming: the engine reader's pattern (input in 16 KB pieces, 40 KB of output per call)
    std::size_t n = 0, ns = 0;
    Bytes a, b, raw, back;
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    for (const auto& kv : m) {
        const Entry& e = kv.second;
        if (!o.filter.empty() && Lower(e.rel).find(o.filter) == std::string::npos) continue;
        if (o.limit && n >= o.limit) break;
        if (!ReadFile(root / e.rel, &a) || !ReadFile(o.out / "dvdroot_ps4" / e.rel, &b)) continue;
        std::uint32_t us = 0, cs = 0, us2 = 0, cs2 = 0;
        if (!HeaderOk(a.data(), &us, &cs) || !HeaderOk(b.data(), &us2, &cs2)) continue;
        raw.resize(us);
        back.resize(us);
        double bz = 1e9, bs = 1e9;
        for (int r = 0; r < 3; ++r) {
            auto t = std::chrono::steady_clock::now();
            Inflate(a.data() + kHeader, cs, us, &raw);
            bz = std::min(bz, std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count());
            t = std::chrono::steady_clock::now();
            ZSTD_decompressDCtx(dctx, back.data(), us, b.data() + kHeader, cs2);
            bs = std::min(bs, std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count());
        }
        {   // streaming decode of both, like the engine's reader
            const std::size_t in_piece = 16384, out_piece = 40960;
            auto t = std::chrono::steady_clock::now();
            zng_stream zs{};
            zng_inflateInit(&zs);
            const std::uint8_t* src_z = a.data() + kHeader;
            std::size_t left = cs, outpos = 0;
            zs.avail_in = 0;
            while (true) {
                if (zs.avail_in == 0 && left) {
                    const std::size_t k = std::min(in_piece, left);
                    zs.next_in = src_z, zs.avail_in = std::uint32_t(k), src_z += k, left -= k;
                }
                zs.next_out = raw.data() + outpos;
                zs.avail_out = std::uint32_t(std::min(out_piece, raw.size() - outpos));
                const std::size_t before = zs.avail_out;
                const int rc = zng_inflate(&zs, 3);
                outpos += before - zs.avail_out;
                if (rc != Z_OK) break;
            }
            zng_inflateEnd(&zs);
            tzs += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
            t = std::chrono::steady_clock::now();
            ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
            const std::uint8_t* src_s = b.data() + kHeader;
            left = cs2, outpos = 0;
            std::size_t avail_in = 0;
            const std::uint8_t* next_in = src_s;
            while (true) {
                if (avail_in == 0 && left) {
                    const std::size_t k = std::min(in_piece, left);
                    next_in = src_s, avail_in = k, src_s += k, left -= k;
                }
                ZSTD_inBuffer in{next_in, avail_in, 0};
                ZSTD_outBuffer out{back.data() + outpos, std::min(out_piece, back.size() - outpos), 0};
                const std::size_t r = ZSTD_decompressStream(dctx, &out, &in);
                next_in += in.pos, avail_in -= in.pos, outpos += out.pos;
                if (ZSTD_isError(r) || r == 0) break;
            }
            tss += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
        }
        tz += bz, ts += bs, ub += us, zb += cs, sb += cs2, ++n;
        if (cs2 >= cs) tz_s += bz, ts_s += bs, ub_s += us, ++ns;
    }
    std::printf("%zu files: %.1f MB out, zlib %.1f MB in %.2f s (%.0f MB/s), zstd %.1f MB in %.2f s (%.0f MB/s): zstd time %.0f%%\n",
                n, ub / 1e6, zb / 1e6, tz, ub / 1e6 / tz, sb / 1e6, ts, ub / 1e6 / ts, 100 * ts / tz);
    std::printf("  streaming (16 KB in, 40 KB out per call): zlib-ng %.2f s (%.0f MB/s), zstd %.2f s (%.0f MB/s): zstd time %.0f%%\n", tzs,
                ub / 1e6 / tzs, tss, ub / 1e6 / tss, 100 * tss / tzs);
    if (ns) std::printf("  of them %zu files where zstd is not smaller: %.1f MB out, zlib %.2f s, zstd %.2f s (%.0f%%)\n", ns,
                        ub_s / 1e6, tz_s, ts_s, 100 * ts_s / tz_s);
    ZSTD_freeDCtx(dctx);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    std::string cmd;
    if (!ParseArgs(argc, argv, &o, &cmd)) {
        std::fprintf(stderr,
                     "usage: bb-zstdpack convert --game DIR --out DIR [--level 9] [--window-log N] [--threads N] [--force]\n"
                     "       bb-zstdpack verify  --game DIR --out DIR [--threads N]\n");
        return 2;
    }
    return cmd == "convert" ? Convert(o) : cmd == "verify" ? Verify(o) : Bench(o);
}
