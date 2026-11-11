// SPDX-License-Identifier: GPL-3.0-or-later
// Converted game files: see bbport_zstd_files.h.
#include "bbport_zstd_files.h"

#include <strings.h>
#include <sys/stat.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>

extern "C" {
extern int (*runtime_file_redirect)(const char* rest, char* host, size_t size);
}

namespace BbZstdFiles {
namespace {

struct Item {
    std::string rel;  // as written by the converter (the case of the game folder)
    std::uint64_t size = 0, new_size = 0;
    std::int64_t mtime = 0;
};

std::string g_dir;                              // converted folder
std::unordered_map<std::string, Item> g_items;  // lower-case "chr/c0000.anibnd.dcx" -> item
std::atomic<std::uint64_t> g_redirected{0}, g_stale{0};

std::string Lower(const char* s, std::size_t n) {
    std::string r(s, n);
    for (char& c : r) c = char(std::tolower((unsigned char)c));
    return r;
}

int Redirect(const char* rest, char* host, size_t size) {
    static const char prefix[] = "/dvdroot_ps4/";
    if (strncasecmp(rest, prefix, sizeof(prefix) - 1) != 0) return 0;
    const char* rel = rest + sizeof(prefix) - 1;
    const std::size_t n = std::strlen(rel);
    if (n < 5 || strncasecmp(rel + n - 4, ".dcx", 4) != 0) return 0;
    const auto it = g_items.find(Lower(rel, n));
    if (it == g_items.end()) return 0;
    const Item& item = it->second;
#ifdef _WIN32
    struct _stat64 st;
    const int stat_rc = _stat64(host, &st);
#else
    struct stat st;
    const int stat_rc = stat(host, &st);
#endif
    if (stat_rc != 0 || std::uint64_t(st.st_size) != item.size || std::int64_t(st.st_mtime) != item.mtime) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 3) std::printf("Zstd files: %s changed since it was converted: the original is used\n", rel);
        g_stale.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    char path[1024];
    if (std::snprintf(path, sizeof(path), "%s/dvdroot_ps4/%s", g_dir.c_str(), item.rel.c_str()) >= int(sizeof(path)) ||
        std::strlen(path) >= size)
        return 0;
    std::memcpy(host, path, std::strlen(path) + 1);
    g_redirected.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

}  // namespace

bool Enable(const std::string& dir) {
    std::string d = dir;
    while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    std::ifstream f(d + "/bbzstd.manifest");
    if (!f) {
        std::printf("Zstd files: %s has no bbzstd.manifest: not used\n", d.c_str());
        return false;
    }
    std::string line;
    bool header = false, done = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("bbzstd 1", 0) == 0) {
            header = true;
        } else if (line.rfind("F\t", 0) == 0) {
            const std::size_t tab = line.find('\t', 2);
            if (tab == std::string::npos) continue;
            Item it;
            it.rel = line.substr(2, tab - 2);
            unsigned long long sz = 0, nsz = 0;
            long long mt = 0;
            if (std::sscanf(line.c_str() + tab + 1, "%llu\t%lld\t%llu", &sz, &mt, &nsz) != 3) continue;
            it.size = sz, it.mtime = mt, it.new_size = nsz;
            g_items[Lower(it.rel.data(), it.rel.size())] = it;
        } else if (line.rfind("DONE", 0) == 0) {
            done = true;
        }
    }
    if (!header || g_items.empty()) {
        std::printf("Zstd files: %s: manifest is empty or from another version: not used\n", d.c_str());
        g_items.clear();
        return false;
    }
    g_dir = d;
    runtime_file_redirect = &Redirect;
    std::printf("Zstd files: %zu converted files in %s%s\n", g_items.size(), d.c_str(),
                done ? "" : " (the conversion was not finished)");
    return true;
}

}  // namespace BbZstdFiles
