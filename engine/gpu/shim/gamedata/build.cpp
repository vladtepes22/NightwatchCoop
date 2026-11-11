// SPDX-License-Identifier: GPL-3.0-or-later
#include "build.h"

#include "container.h"
#include "derive_ids.h"
#include "fmg.h"
#include "party/party_gamedata.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace coop::gamedata {
namespace fs = std::filesystem;
namespace {

// Bump when the derivation changes what the tables hold (a cache of another version is rebuilt).
constexpr const char* kDerivationVersion = "cpp-4";
constexpr const char* kCacheMagic = "BBPARTYTABLES 1";

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct FileInfo {
    fs::path path;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
};

#ifdef _WIN32
// 0.8.1 tools081: the key check took 2.7 s per start because std::filesystem asks the OS three times per file (type, size,
// time: ~25k files under event/ and map/mapstudio/). FindFirstFileEx returns name, size and time of a whole folder in one
// call (about 30 ms for the same files). The time is converted to the value std::filesystem's last_write_time gave before
// (nanoseconds from 2174-01-01 at one second resolution), so the cache key stays what it was and no cache is rebuilt.
void WalkFind(const fs::path& dir, const std::function<bool(const std::string&)>& match, std::vector<FileInfo>& out) {
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileExW((dir / L"*").c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t* n = d.cFileName;
        if (n[0] == L'.' && (!n[1] || (n[1] == L'.' && !n[2]))) continue;
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) WalkFind(dir / n, match, out);  // like the iterator: no links
            continue;
        }
        if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        FileInfo f;
        f.path = dir / n;
        if (!match(f.path.filename().string())) continue;
        f.size = (static_cast<std::uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
        const std::uint64_t ft = (static_cast<std::uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
        f.mtime = (static_cast<std::int64_t>(ft / 10000000ull) - 11644473600ll - 6437667600ll) * 1000000000ll;
        out.push_back(std::move(f));
    } while (FindNextFileW(h, &d));
    FindClose(h);
}
#endif

// Files under `dir` (recursive) whose name matches, sorted like the Python tools (case folded path parts).
std::vector<FileInfo> ListFiles(const fs::path& dir, const std::function<bool(const std::string&)>& match) {
    std::vector<FileInfo> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
#ifdef _WIN32
    WalkFind(dir, match, out);
#else
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code e2;
        if (it->is_regular_file(e2) && match(it->path().filename().string())) {
            FileInfo f;
            f.path = it->path();
            f.size = it->file_size(e2);
            const auto t = it->last_write_time(e2);
            f.mtime = e2 ? 0 : static_cast<std::int64_t>(t.time_since_epoch().count());
            out.push_back(std::move(f));
        }
    }
#endif
    // Python's sort of Windows paths: tuple of case folded parts (not the joined string)
    std::vector<std::pair<std::vector<std::string>, FileInfo>> keyed;
    keyed.reserve(out.size());
    for (FileInfo& f : out) {
        std::vector<std::string> parts;
        for (const fs::path& part : f.path.lexically_relative(dir)) parts.push_back(Lower(part.string()));
        keyed.emplace_back(std::move(parts), std::move(f));
    }
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = std::move(keyed[i].second);
    return out;
}

bool Contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
bool EndsWith(const std::string& s, const char* suffix) {
    const std::size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// m12_34_56_78 [.msb [.dcx]]
bool MapName(const std::string& name, int* a, int* b, int* c, int* d, bool* has_ext) {
    static const std::regex re(R"(^m(\d\d)_(\d\d)_(\d\d)_(\d\d)(\.msb(\.dcx)?)?$)");
    std::smatch m;
    if (!std::regex_match(name, m, re)) return false;
    *a = std::stoi(m[1]);
    *b = std::stoi(m[2]);
    *c = std::stoi(m[3]);
    *d = std::stoi(m[4]);
    *has_ext = m[5].matched;
    return true;
}

std::int64_t Mtime(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec ? 0 : static_cast<std::int64_t>(t.time_since_epoch().count());
}

struct Fnv {
    std::uint64_t h1 = 0xcbf29ce484222325ull, h2 = 0x84222325cbf29ce4ull;
    void Add(const std::string& s) {
        for (const char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            h1 = (h1 ^ c) * 0x100000001b3ull;
            h2 = (h2 ^ (c + 31u)) * 0x1000193ull + 0x9e3779b97f4a7c15ull;
        }
        h1 = (h1 ^ 0xff) * 0x100000001b3ull;
        h2 = (h2 ^ 0xff) * 0x1000193ull;
    }
    std::string Hex() const {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(h1), static_cast<unsigned long long>(h2));
        return buf;
    }
};

struct EventFile {
    std::string stem;
    fs::path path, eld;
    std::uint64_t size = 0;
};

struct Parsed {
    bbdata::Emevd evd;
    std::unordered_map<std::int64_t, std::string> names;
};

bbdata::Emedf LoadEmedf(const fs::path& event_dir) {
    for (const char* n : {"common.emedf.dcx", "common.emedf"}) {
        const fs::path p = event_dir / n;
        std::error_code ec;
        if (fs::exists(p, ec)) return bbdata::ParseEmedf(bbdata::Bytes(bbdata::ReadMaybeDcx(p.string())));
    }
    throw bbdata::FormatError("common.emedf(.dcx) not found in the event folder");
}

bbdata::ParamTable LoadParam(const bbdata::Bytes& pb, const std::vector<bbdata::BndEntry>& pe, const bbdata::Bytes& db,
                             const std::vector<bbdata::BndEntry>& de, const std::string& name) {
    const bbdata::BndEntry* p = bbdata::FindBndEntry(pe, name + ".param");
    const bbdata::BndEntry* d = bbdata::FindBndEntry(de, name + ".paramdef");
    if (!p || !d) throw bbdata::FormatError("param " + name + " not found");
    return bbdata::ParseParam(bbdata::Span(pb).Sub(static_cast<std::int64_t>(p->offset), p->size),
                              bbdata::Span(db).Sub(static_cast<std::int64_t>(d->offset), d->size));
}

}  // namespace

std::string GameRoot(const std::string& path) {
    std::error_code ec;
    const fs::path p = fs::path(path) / "dvdroot_ps4";
    return fs::is_directory(p, ec) ? p.string() : path;
}

namespace {

/// One pass over the game folder, shared by the cache key and the derivation.
struct Scan {
    fs::path root;
    bool has_param = false;
    std::vector<FileInfo> fixed;    // the parambnd and paramdefbnd
    std::vector<FileInfo> events;   // every file under event/
    std::vector<FileInfo> msbs;     // map/mapstudio files whose name holds ".msb"
    std::vector<std::pair<std::string, bool>> map_entries;  // map/ entries (name, is a folder), sorted by name
    std::vector<std::string> remo_names;                    // remo/ entry names, sorted
};

Scan ScanGame(const std::string& game_root) {
    Scan sc;
    sc.root = game_root;
    std::error_code ec;
    const fs::path pb = sc.root / "param/gameparam/gameparam.parambnd.dcx", db = sc.root / "paramdef/paramdef.paramdefbnd.dcx";
    if (!fs::is_regular_file(pb, ec)) return sc;
    sc.has_param = true;
    sc.fixed.push_back({pb, fs::file_size(pb, ec), Mtime(pb)});
    if (fs::is_regular_file(db, ec)) sc.fixed.push_back({db, fs::file_size(db, ec), Mtime(db)});
    if (fs::is_directory(sc.root / "msg", ec)) {  // the item names (gesture names) of every language
        std::vector<fs::path> langs;
        for (const auto& e : fs::directory_iterator(sc.root / "msg", ec)) langs.push_back(e.path());
        std::sort(langs.begin(), langs.end());
        for (const fs::path& l : langs) {
            const fs::path m = l / "item.msgbnd.dcx";
            if (fs::is_regular_file(m, ec)) sc.fixed.push_back({m, fs::file_size(m, ec), Mtime(m)});
        }
    }
    sc.events = ListFiles(sc.root / "event", [](const std::string&) { return true; });
    sc.msbs = ListFiles(sc.root / "map/mapstudio", [](const std::string& n) { return Contains(n, ".msb"); });
    if (fs::is_directory(sc.root / "map", ec)) {
        for (const auto& e : fs::directory_iterator(sc.root / "map", ec)) {
            sc.map_entries.emplace_back(e.path().filename().string(), e.is_directory(ec));
        }
        std::sort(sc.map_entries.begin(), sc.map_entries.end());
    }
    if (fs::is_directory(sc.root / "remo", ec)) {
        for (const auto& e : fs::directory_iterator(sc.root / "remo", ec)) sc.remo_names.push_back(e.path().filename().string());
        std::sort(sc.remo_names.begin(), sc.remo_names.end());
    }
    return sc;
}

std::string KeyOf(const Scan& sc) {
    if (!sc.has_param) return std::string();
    Fnv h;
    h.Add(std::string("derivation ") + kDerivationVersion);
    auto file = [&](const FileInfo& f) {
        h.Add(f.path.lexically_relative(sc.root).generic_string() + "|" + std::to_string(f.size) + "|" + std::to_string(f.mtime));
    };
    for (const FileInfo& f : sc.fixed) file(f);
    for (const FileInfo& f : sc.events) file(f);
    for (const FileInfo& f : sc.msbs) file(f);
    h.Add("map:");
    for (const auto& e : sc.map_entries) h.Add(e.first);
    h.Add("remo:");
    for (const std::string& n : sc.remo_names) h.Add(n);
    return h.Hex();
}

Sections DeriveFrom(const Scan& sc, unsigned threads, Progress* progress, Timings* timings);

}  // namespace

std::string ComputeKey(const std::string& game_root) { return KeyOf(ScanGame(game_root)); }

Sections DeriveAll(const std::string& game_root, unsigned threads, Progress* progress, Timings* timings) {
    return DeriveFrom(ScanGame(game_root), threads, progress, timings);
}

namespace {

Sections DeriveFrom(const Scan& sc, unsigned threads, Progress* progress, Timings* timings) {
    const fs::path& root = sc.root;
    Progress local;
    Progress& prog = progress ? *progress : local;
    std::error_code ec;

    // ---- listing ----
    std::vector<EventFile> files;
    std::vector<MsbFile> msbs;
    std::set<std::int64_t> maps, remos;
    {
        StepTimer t(timings, "list");
        prog.SetLabel("Reading the game's file list");
        for (const FileInfo& fi : sc.events) {
            const fs::path& p = fi.path;
            if (!Contains(p.filename().string(), ".emevd")) continue;
            EventFile f;
            const std::string name = p.filename().string();
            f.stem = name.substr(0, name.find('.'));
            f.path = p;
            f.eld = p.parent_path() / (f.stem + ".emeld" + (EndsWith(name, ".dcx") ? ".dcx" : ""));
            f.size = fi.size;
            files.push_back(std::move(f));
        }
        for (const auto& [name, is_dir] : sc.map_entries) {
            int a, b, c, d;
            bool ext;
            if (is_dir && MapName(name, &a, &b, &c, &d, &ext)) maps.insert(a << 24 | b << 16 | c << 8 | d);
        }
        for (const FileInfo& fi : sc.msbs) {
            const fs::path& p = fi.path;
            int a, b, c, d;
            bool ext;
            if (!MapName(p.filename().string(), &a, &b, &c, &d, &ext)) continue;
            maps.insert(a << 24 | b << 16 | c << 8 | d);
            MsbFile m;
            m.a = a;
            m.b = b;
            m.c = c;
            m.d = d;
            m.path = p.string();
            m.size = fi.size;
            msbs.push_back(std::move(m));
        }
        static const std::regex remo_re(R"(^s(\d\d)_(\d\d)_(\d{4})\.remobnd(\.dcx)?$)");
        for (const std::string& n : sc.remo_names) {
            std::smatch m;
            if (std::regex_match(n, m, remo_re)) remos.insert(std::stoll(m[1]) * 1000000 + std::stoll(m[2]) * 10000 + std::stoll(m[3]));
        }
    }
    const fs::path pb_path = root / "param/gameparam/gameparam.parambnd.dcx", db_path = root / "paramdef/paramdef.paramdefbnd.dcx";
    std::uint64_t param_bytes = 0;
    for (const FileInfo& f : sc.fixed) param_bytes += f.size;
    std::uint64_t total = param_bytes + 4096;
    for (const EventFile& f : files) total += 2 * f.size;
    for (const MsbFile& m : msbs) total += m.size;
    prog.SetTotal(total);

    // ---- phase 1: params, event scripts, maps, all in parallel (heaviest first) ----
    prog.SetLabel("Reading params, events and maps");
    ParamSet params;
    std::vector<Parsed> parsed(files.size());
    std::vector<MsbInfo> infos(msbs.size());
    bbdata::Emedf emedf;
    {
        StepTimer t(timings, "read");
        emedf = LoadEmedf(root / "event");
        struct Task {
            std::uint64_t weight;
            std::function<void()> run;
        };
        std::vector<Task> tasks;
        tasks.push_back({param_bytes, [&] {
                             StepTimer pt(timings, "  params");
                             const bbdata::Bytes pb = bbdata::ReadMaybeDcx(pb_path.string()), db = bbdata::ReadMaybeDcx(db_path.string());
                             const auto pe = bbdata::ParseBnd4(pb), de = bbdata::ParseBnd4(db);
                             params.item_lot = LoadParam(pb, pe, db, de, "ItemLotParam");
                             params.goods = LoadParam(pb, pe, db, de, "EquipParamGoods");
                             params.weapon = LoadParam(pb, pe, db, de, "EquipParamWeapon");
                             params.protector = LoadParam(pb, pe, db, de, "EquipParamProtector");
                             params.return_point = LoadParam(pb, pe, db, de, "ReturnPointParam");
                             params.shop = LoadParam(pb, pe, db, de, "ShopLineupParam");
                         }});
        for (std::size_t i = 0; i < files.size(); ++i) {
            tasks.push_back({files[i].size, [&, i] {
                                 parsed[i].evd = bbdata::ParseEmevd(bbdata::ReadMaybeDcx(files[i].path.string()));
                                 std::error_code e;
                                 if (fs::exists(files[i].eld, e)) {
                                     parsed[i].names = bbdata::ParseEmeld(bbdata::Span(bbdata::ReadMaybeDcx(files[i].eld.string())));
                                 }
                             }});
        }
        for (std::size_t i = 0; i < msbs.size(); ++i) {
            tasks.push_back({msbs[i].size, [&, i] { infos[i] = ReadMsb(msbs[i]); }});
        }
        std::vector<std::size_t> order(tasks.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return tasks[a].weight > tasks[b].weight; });
        ParallelFor(tasks.size(), threads, [&](std::size_t k) {
            Task& t2 = tasks[order[k]];
            t2.run();
            prog.Add(t2.weight);
        });
    }

    // ---- phase 2: expand the events (map scripts in parallel, then common with their calls into it) ----
    prog.SetLabel("Expanding event scripts");
    std::vector<FileEvents> allrec;
    {
        StepTimer t(timings, "expand");
        std::size_t common = files.size();
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (files[i].stem == "common") {
                common = i;
                break;
            }
        }
        std::set<std::int64_t> common_ids;
        if (common < files.size()) {
            for (const bbdata::EvEvent& e : parsed[common].evd.events) common_ids.insert(e.id);
        }
        std::vector<std::vector<bbdata::EventRec>> recs(files.size());
        const bbdata::ExternalCalls none;
        ParallelFor(files.size(), threads, [&](std::size_t i) {
            if (files[i].stem == "common") return;
            recs[i] = bbdata::ExpandFile(parsed[i].evd, parsed[i].names, emedf, none);
            prog.Add(files[i].size);
        });
        bbdata::ExternalCalls external;
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (files[i].stem == "common") continue;
            std::set<std::int64_t> own;
            for (const bbdata::EventRec& r : recs[i]) own.insert(r.event);
            for (const bbdata::EventRec& r : recs[i]) {
                for (const bbdata::EventCall& c : r.calls) {
                    if (common_ids.count(c.target) && !own.count(c.target)) external[c.target].push_back({c.slot, c.params});
                }
            }
        }
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (files[i].stem != "common") allrec.push_back({files[i].stem, std::move(recs[i])});
        }
        if (common < files.size()) {
            allrec.push_back({"common", bbdata::ExpandFile(parsed[common].evd, parsed[common].names, emedf, external)});
            prog.Add(files[common].size);
        }
    }

    // ---- phase 3: the tables, independent of one another ----
    prog.SetLabel("Deriving the tables");
    std::vector<FogCalls> fog_calls;
    {
        std::map<std::string, FogCalls> by_stem;
        for (std::size_t i = 0; i < files.size(); ++i) {
            for (FogCalls& fc : CollectFogCalls(files[i].stem, parsed[i].evd)) {
                FogCalls& dst = by_stem[fc.stem];
                dst.stem = fc.stem;
                for (auto& c : fc.calls) dst.calls.push_back(std::move(c));
            }
        }
        for (auto& [k, v] : by_stem) fog_calls.push_back(std::move(v));
    }
    // the chalice dungeons' parameterised events (m29.emevd) are started from the dungeon maps' own scripts: expand
    // them once more with those calls (the boss entities are their arguments)
    std::vector<bbdata::EventRec> chalice_rec;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (files[i].stem != "m29") continue;
        std::set<std::int64_t> own;  // the events that hold a SpEffect instruction (2004[8])
        for (const bbdata::EvEvent& e : parsed[i].evd.events) {
            for (const bbdata::EvInstr& in : e.ins) {
                if (in.bank == 2004 && in.id == 8 && !e.params.empty()) own.insert(e.id);
            }
        }
        bbdata::ExternalCalls ext;
        for (const FileEvents& fe : allrec) {
            if (fe.stem == "m29") continue;
            for (const bbdata::EventRec& r : fe.recs) {
                for (const bbdata::EventCall& c : r.calls) {
                    if (own.count(c.target)) ext[c.target].push_back({c.slot, c.params});
                }
            }
        }
        chalice_rec = bbdata::ExpandFile(parsed[i].evd, parsed[i].names, emedf, ext);
    }
    parsed.clear();
    parsed.shrink_to_fit();
    Sections out[5];
    {
        StepTimer t(timings, "derive");
        IdsInput in;
        in.maps = maps;
        in.remo_files = remos;
        in.msbs = &msbs;
        in.infos = &infos;
        in.params = &params;
        in.events = &allrec;
        ParallelFor(5, threads, [&](std::size_t k) {
            switch (k) {
                case 0: {
                    StepTimer s(timings, "  flags+items");
                    const FlagCategories cats(allrec, params);
                    out[0] = DeriveItems(params, allrec, cats);
                    out[4] = DeriveFlagRows(cats);
                    for (auto& [name, rows] : DeriveLamps(cats)) out[4][name] = std::move(rows);
                    for (auto& [name, rows] : DeriveHostOnlyEvents(allrec)) out[4][name] = std::move(rows);
                    for (auto& [name, rows] : DeriveShopGates(params, allrec)) out[4][name] = std::move(rows);
                    for (auto& [name, rows] : DeriveStartWeapons(params)) out[4][name] = std::move(rows);
                    for (auto& [name, rows] : DeriveProgressIds(cats, allrec)) out[4][name] = std::move(rows);
                    const Sections worldact = DeriveWorldAct(allrec);
                    for (const auto& [name, rows] : worldact) out[4][name] = rows;
                    for (auto& [name, rows] : DeriveSpots(allrec, worldact, (root / "map/mapstudio").string(), threads)) out[4][name] = std::move(rows);
                    for (auto& [name, rows] : DeriveBossEntities(allrec, chalice_rec)) out[4][name] = std::move(rows);
                    break;
                }
                case 4: break;
                case 1: { StepTimer s(timings, "  ids"); out[1] = DeriveIds(in); break; }
                case 2: { StepTimer s(timings, "  warper"); out[2] = DeriveWarper(params); break; }
                default: { StepTimer s(timings, "  npcfog"); out[3] = DeriveNpcFog(fog_calls, (root / "map/mapstudio").string(), threads); break; }
            }
        });
    }
    prog.Add(4096);
    {
        // gestureNames: the game's own names of the 23 gestures (goods 800 .. 822)
        StepTimer st(timings, "  gesture names");
        const auto names = bbdata::ReadGoodsNames(root.string());
        auto& rows = out[0]["gestureNames"];
        for (std::int32_t id = 800; id <= 822; ++id) {
            const auto it = names.find(id);
            if (it != names.end() && !it->second.empty() && it->second.find_first_of("\t\n") == std::string::npos) {
                rows.push_back({std::to_string(id), it->second});
            }
        }
    }
    Sections all;
    for (Sections& s : out) {
        for (auto& [name, rows] : s) all[name] = std::move(rows);
    }
    return all;
}

}  // namespace

std::string RenderCache(const std::string& key, const Sections& sections) {
    std::string t = std::string(kCacheMagic) + "\nkey " + key + "\n";
    for (const auto& [name, rows] : sections) {
        t += "[" + name + "] " + std::to_string(rows.size()) + "\n";
        for (const Row& r : rows) {
            for (std::size_t i = 0; i < r.size(); ++i) {
                if (r[i].find_first_of("\t\n") != std::string::npos) throw std::runtime_error(name + ": tab / newline in a field");
                if (i) t += '\t';
                t += r[i];
            }
            t += '\n';
        }
    }
    return t;
}

std::string CacheTextKey(const std::string& text) {
    std::istringstream in(text);
    std::string l1, l2;
    std::getline(in, l1);
    std::getline(in, l2);
    while (!l1.empty() && (l1.back() == '\r' || l1.back() == ' ')) l1.pop_back();
    while (!l2.empty() && (l2.back() == '\r' || l2.back() == ' ')) l2.pop_back();
    if (l1 != kCacheMagic || l2.rfind("key ", 0) != 0) return std::string();
    return l2.substr(4);
}

namespace {

bool ReadText(const std::string& path, std::string* out) {
    try {
        const bbdata::Bytes b = bbdata::ReadFile(path);
        out->assign(b.begin(), b.end());
        return true;
    } catch (const bbdata::FormatError&) {
        return false;
    }
}

bool WriteAtomic(const std::string& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
    }
    return !ec;
}

std::string TimingLine(Timings& t) {
    std::string s;
    std::lock_guard<std::mutex> lk(t.mu);
    for (const auto& [n, ms] : t.steps) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s %.0f ms; ", n.c_str(), ms);
        s += buf;
    }
    return s;
}

/// Cache hit or derive + write. `text` receives the cache text.
bool CachedText(const std::string& root, const std::string& cache_path, bool force, unsigned threads, Progress* prog,
                std::string* text, std::string* err, std::string* log, bool* rebuilt) {
    const auto k0 = std::chrono::steady_clock::now();
    const Scan sc = ScanGame(root);
    const std::string key = KeyOf(sc);
    const double key_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - k0).count();
    if (key.empty()) {
        *err = "no game data under " + root;
        return false;
    }
    if (!force && ReadText(cache_path, text) && CacheTextKey(*text) == key) {
        *rebuilt = false;
        if (log) *log += "key check " + std::to_string(static_cast<int>(key_ms)) + " ms; ";
        return true;
    }
    *rebuilt = true;
    MarkRebuild();  // only now is there something to wait for (the in-game notice keys on this, not on the key check)
    Timings tm;
    tm.Note("key", key_ms);
    const auto t0 = std::chrono::steady_clock::now();
    try {
        const Sections s = DeriveFrom(sc, threads, prog, &tm);
        *text = RenderCache(key, s);
    } catch (const std::exception& e) {
        *err = e.what();
        return false;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!WriteAtomic(cache_path, *text) && log) *log += "could not write " + cache_path + "\n";
    char buf[64];
    std::snprintf(buf, sizeof buf, "built in %.1f s with %u threads: ", secs, threads);
    if (log) *log += std::string(buf) + TimingLine(tm) + "\n";
    return true;
}

}  // namespace

bool BuildToFile(const std::string& game, const std::string& cache_path, bool force, unsigned threads, std::string* err,
                 std::string* log) {
    std::string text, e, l;
    bool rebuilt = false;
    Progress prog;
    const bool ok = CachedText(GameRoot(game), cache_path, force, threads ? threads : DefaultThreads(), &prog, &text, &e, &l, &rebuilt);
    if (log) *log = l + (ok && !rebuilt ? "cache current\n" : "");
    if (!ok && err) *err = e;
    return ok;
}

unsigned DefaultThreads() {
    if (const char* e = std::getenv("BB_PARTY_TABLES_THREADS")) {
        const int n = std::atoi(e);
        if (n > 0) return static_cast<unsigned>(n);
    }
    const unsigned hc = std::thread::hardware_concurrency();
    return hc > 2 ? hc - 1 : 1;
}

void StartBackground(const std::string& app0, const std::string& user_dir) {
    const char* tables = std::getenv("BB_PARTY_TABLES");
    const char* off = std::getenv("BB_PARTY_TABLES_BUILD");
    if ((tables && *tables) || (off && std::string(off) == "0")) return;  // a named cache file / switched off: Get() reads the file
    std::string game = app0;
    if (game.empty()) {
        if (const char* g = std::getenv("BB_GAME_DIR")) game = g;
    }
    if (game.empty()) return;
    const std::string cache = (fs::path(user_dir.empty() ? "user" : user_dir) / "party_tables.txt").string();
    SetGameDir(GameRoot(game));
    BeginBuild();
    std::thread([game, cache] {
        const auto t0 = std::chrono::steady_clock::now();
        Tables tables_out;
        std::string err;
        try {
            const std::string root = GameRoot(game);
            const unsigned threads = DefaultThreads();
            Progress prog;
            std::atomic<bool> done{false};
            std::thread reporter([&] {
                std::string last;
                int last_pct = -1;
                while (!done.load()) {
                    const std::string label = prog.Label();
                    const int pct = prog.Percent();
                    SetBuildProgress(pct, label);
                    if (label != last && !label.empty()) {
                        std::printf("[party-data] %s: %d%%\n", label.c_str(), pct);
                        std::fflush(stdout);
                        last = label;
                        last_pct = pct;
                    }
                    (void)last_pct;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            });
            std::string text, log;
            bool rebuilt = false;
            const bool ok = CachedText(root, cache, false, threads, &prog, &text, &err, &log, &rebuilt);
            done.store(true);
            reporter.join();
            if (ok) {
                std::string perr;
                if (Parse(text, &tables_out, &perr)) {
                    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    if (rebuilt) {
                        std::printf("[party-data] done in %.1f s: %s", secs, log.c_str());
                    } else {
                        std::printf("[party-data] tables loaded from the cache (%.2f s; %s)\n", secs, log.c_str());
                    }
                } else {
                    err = perr;
                }
            }
        } catch (const std::exception& e) {
            err = e.what();
        }
        if (!tables_out.loaded) {
            std::printf("[party-data] could not be built (%s); the party features that need the tables are off\n", err.c_str());
            tables_out = Tables{};
            tables_out.status = "party tables not built (" + err + ")";
        }
        std::fflush(stdout);
        FinishBuild(std::move(tables_out));
    }).detach();
}

}  // namespace coop::gamedata
