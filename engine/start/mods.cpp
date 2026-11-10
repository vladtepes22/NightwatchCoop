// SPDX-License-Identifier: GPL-3.0-or-later
#include "mods.h"
#include "elf.h"
#include "host_only.h"
#include "features.h"

#include <windows.h>
#include <winioctl.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <random>
#include <set>

namespace start {
namespace {

const std::set<std::wstring> kGameFolders{L"action", L"chr", L"event", L"facegen", L"font", L"map", L"menu",
                                          L"movie", L"msg", L"mtd", L"obj", L"other", L"param", L"paramdef",
                                          L"parts", L"remo", L"script", L"sfx", L"shader", L"sound"};
const wchar_t* const kSerials[] = {L"CUSA00207", L"CUSA00208", L"CUSA00299", L"CUSA00900", L"CUSA01363",
                                   L"CUSA03014", L"CUSA03023", L"CUSA03173", L"CUSA03179"};

std::wstring Fold(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(text.data(), DWORD(text.size()));
    return text;
}

bool IsDir(const fs::path& p) {
    std::error_code e;
    return fs::is_directory(p, e);
}
bool IsFile(const fs::path& p) {
    std::error_code e;
    return fs::is_regular_file(p, e);
}
bool Exists(const fs::path& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsLink(const fs::path& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

std::vector<fs::path> Entries(const fs::path& folder) {
    std::vector<fs::path> out;
    std::error_code e;
    if (!IsDir(folder)) return out;
    for (const auto& entry : fs::directory_iterator(folder, e)) out.push_back(entry.path());
    return out;
}

fs::path Child(const fs::path& folder, const std::wstring& name) {
    const std::wstring want = Fold(name);
    for (const fs::path& entry : Entries(folder))
        if (Fold(entry.filename().native()) == want) return entry;
    return folder / name;
}

std::optional<std::pair<fs::path, fs::path>> ContentRoot(const fs::path& folder) {
    if (!IsDir(folder)) return std::nullopt;
    std::vector<std::wstring> wrappers{L"", L"app0"};
    for (const wchar_t* s : kSerials) wrappers.push_back(s);
    for (const std::wstring& wrapper : wrappers) {
        const fs::path base = wrapper.empty() ? folder : Child(folder, wrapper);
        if (IsDir(Child(base, L"dvdroot_ps4"))) return std::pair{base, fs::path()};
    }
    std::vector<fs::path> entries, folders;
    for (const fs::path& e : Entries(folder))
        if (!StartsWith(Utf8(e.filename()), ".")) entries.push_back(e);
    for (const fs::path& e : entries)
        if (IsDir(e)) folders.push_back(e);
    if (!folders.empty() && std::all_of(folders.begin(), folders.end(), [](const fs::path& f) {
            return kGameFolders.count(Fold(f.filename().native()));
        }))
        return std::pair{folder, fs::path(L"dvdroot_ps4")};
    if (folders.size() == 1 && std::none_of(entries.begin(), entries.end(), [](const fs::path& e) {
            if (!IsFile(e)) return false;
            const std::wstring ext = Fold(e.extension().native());
            return ext != L".txt" && ext != L".md" && ext != L".jpg" && ext != L".png" && ext != L".ini";
        }))
        return ContentRoot(folders[0]);
    return std::nullopt;
}

std::vector<std::wstring> Discover(const fs::path& root) {
    std::vector<std::wstring> names;
    for (const fs::path& p : Entries(root))
        if (IsDir(p) && ContentRoot(p)) names.push_back(p.filename().native());
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
        const std::wstring fa = Fold(a), fb = Fold(b);
        return fa != fb ? fa < fb : a < b;
    });
    return names;
}

std::vector<std::wstring> Selected(const fs::path& root, const fs::path& config) {
    const std::vector<std::wstring> available = Discover(root);
    if (config.empty() || !IsFile(config)) return available;
    const auto settings = nlohmann::json::parse(ReadText(config));
    std::set<std::wstring> disabled;
    std::vector<std::wstring> order;
    if (settings.contains("disabled")) {
        if (!settings["disabled"].is_array()) throw Error("Disabled mods must be a list of folder names");
        for (const auto& n : settings["disabled"]) {
            if (!n.is_string()) throw Error("Disabled mods must be a list of folder names");
            disabled.insert(Wide(n.get<std::string>()));
        }
    }
    if (settings.contains("order")) {
        if (!settings["order"].is_array()) throw Error("Mod order must be a list of folder names");
        for (const auto& n : settings["order"]) {
            if (!n.is_string()) throw Error("Mod order must be a list of folder names");
            order.push_back(Wide(n.get<std::string>()));
        }
    }
    std::vector<std::wstring> out;
    order.insert(order.end(), available.begin(), available.end());
    for (const std::wstring& n : order)
        if (std::find(available.begin(), available.end(), n) != available.end() && !disabled.count(n) &&
            std::find(out.begin(), out.end(), n) == out.end())
            out.push_back(n);
    return out;
}

void Walk(const fs::path& directory, const fs::path& root, std::vector<std::pair<fs::path, fs::path>>& files,
          bool skip_links) {
    std::vector<fs::path> folders, names;
    for (const fs::path& entry : Entries(directory)) {
        if (IsDir(entry)) {
            if (IsLink(entry)) {
                if (skip_links) Log("Mods: skipped the link %s (links are not followed)", Utf8(entry).c_str());
                continue;
            }
            folders.push_back(entry);
        } else {
            names.push_back(entry);
        }
    }
    std::sort(names.begin(), names.end());
    for (const fs::path& source : names) {
        if (IsLink(source)) {
            if (skip_links) Log("Mods: skipped the link %s (links are not followed)", Utf8(source).c_str());
            continue;
        }
        files.emplace_back(source.lexically_relative(root), source);
    }
    for (const fs::path& folder : folders) Walk(folder, root, files, skip_links);
}

std::vector<std::pair<fs::path, fs::path>> ModFiles(const fs::path& folder) {
    const auto layout = ContentRoot(folder);
    if (!layout) throw Error(Utf8(folder) + ": expected dvdroot_ps4 (or chr/, parts/, ...) inside the mod folder");
    const fs::path root = fs::canonical(layout->first);
    std::vector<std::pair<fs::path, fs::path>> found, out;
    Walk(root, root, found, true);
    for (auto& [relative, source] : found) {
        const fs::path game_path = layout->second.empty() ? relative : layout->second / relative;
        const std::wstring first = Fold(game_path.begin()->native());
        if (first != L"dvdroot_ps4") {
            if (first == L"eboot.bin" || first == L"sce_module" || first == L"sce_sys")
                throw Error("Executable/system replacement is unsupported: " + Utf8(source));
            continue;
        }
        if (!IsFile(source)) throw Error("Not a regular mod file: " + Utf8(source));
        out.emplace_back(game_path, source);
    }
    return out;
}

std::optional<std::string> AppVersion(const fs::path& folder) {
    try {
        return SfoText(ParseSfo(ReadFile(folder / "sce_sys" / "param.sfo")), "APP_VER", "");
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<fs::path> UpdateFolder(const fs::path& game) {
    const std::wstring update = Fold(game.filename().native() + L"-update"),
                       patch = Fold(game.filename().native() + L"-patch");
    for (const fs::path& entry : Entries(game.parent_path())) {
        const std::wstring name = Fold(entry.filename().native());
        if (!IsDir(entry) || (name != update && name != patch) || !IsFile(Child(entry, L"eboot.bin"))) continue;
        const auto version = AppVersion(entry);
        if (version && !version->empty() && AppVersion(game) == version) {
            Log("Game update: %s is already in the game (%s)", Utf8(entry.filename()).c_str(), version->c_str());
            return std::nullopt;
        }
        return entry;
    }
    return std::nullopt;
}

void CreateJunction(const fs::path& target, const fs::path& link) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) throw Error("cannot create " + Utf8(link));
    const std::wstring substitute = L"\\??\\" + fs::absolute(target).native();
    const std::wstring print = fs::absolute(target).native();
    const std::size_t path_bytes = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
    std::vector<std::uint8_t> buffer(8 + 8 + path_bytes, 0);
    Store<std::uint32_t>(buffer.data(), IO_REPARSE_TAG_MOUNT_POINT);
    Store<std::uint16_t>(buffer.data() + 4, std::uint16_t(8 + path_bytes));
    Store<std::uint16_t>(buffer.data() + 8, 0);
    Store<std::uint16_t>(buffer.data() + 10, std::uint16_t(substitute.size() * sizeof(wchar_t)));
    Store<std::uint16_t>(buffer.data() + 12, std::uint16_t((substitute.size() + 1) * sizeof(wchar_t)));
    Store<std::uint16_t>(buffer.data() + 14, std::uint16_t(print.size() * sizeof(wchar_t)));
    std::memcpy(buffer.data() + 16, substitute.c_str(), substitute.size() * sizeof(wchar_t));
    std::memcpy(buffer.data() + 16 + (substitute.size() + 1) * sizeof(wchar_t), print.c_str(), print.size() * sizeof(wchar_t));
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    DWORD returned = 0;
    const bool ok = handle != INVALID_HANDLE_VALUE &&
                    DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), DWORD(buffer.size()), nullptr, 0,
                                    &returned, nullptr);
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    if (!ok) {
        RemoveDirectoryW(link.c_str());
        throw Error("cannot link " + Utf8(link) + " to " + Utf8(target));
    }
}

bool JunctionsWork(const fs::path& folder) {
    const fs::path probe = folder / L"mod-junction-test";
    RemoveDirectoryW(probe.c_str());
    try {
        CreateJunction(folder, probe);
    } catch (const Error&) {
        return false;
    }
    RemoveDirectoryW(probe.c_str());
    return true;
}

void Link(const fs::path& path, const fs::path& target) {
    if (IsDir(target)) {
        CreateJunction(target, path);
    } else if (!CreateHardLinkW(path.c_str(), target.c_str(), nullptr)) {
        if (!CopyFileW(target.c_str(), path.c_str(), TRUE)) throw Error("cannot link " + Utf8(path));
    }
}

void Unlink(const fs::path& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (!RemoveDirectoryW(path.c_str())) throw Error("cannot remove " + Utf8(path));
        return;
    }
    if (!DeleteFileW(path.c_str())) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileW(path.c_str())) throw Error("cannot remove " + Utf8(path));
    }
}

fs::path Resolved(const fs::path& path) {
    HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw Error("cannot resolve " + Utf8(path));
    wchar_t buffer[32768];
    const DWORD n = GetFinalPathNameByHandleW(handle, buffer, DWORD(std::size(buffer)), FILE_NAME_NORMALIZED);
    CloseHandle(handle);
    std::wstring text(buffer, n);
    if (text.rfind(L"\\\\?\\UNC\\", 0) == 0) text = L"\\\\" + text.substr(8);
    else if (text.rfind(L"\\\\?\\", 0) == 0) text = text.substr(4);
    return fs::path(text);
}

void Expand(const fs::path& directory) {
    if (IsLink(directory)) {
        const fs::path target = Resolved(directory);
        if (!IsDir(target)) throw Error("File/directory conflict at " + Utf8(directory.filename()));
        Unlink(directory);
        fs::create_directory(directory);
        for (const fs::path& entry : Entries(target)) Link(directory / entry.filename(), entry);
    } else if (Exists(directory) && !IsDir(directory)) {
        throw Error("File/directory conflict at " + Utf8(directory.filename()));
    } else {
        std::error_code e;
        fs::create_directory(directory, e);
    }
}

fs::path OverlayBase(const fs::path& game, const fs::path& out) {
    const std::wstring out_drive = Fold(fs::absolute(out).root_name().native());
    const std::wstring game_drive = Fold(game.root_name().native());
    if (out_drive != game_drive) return game.parent_path() / L".bbport-overlays";
    return out;
}

void RemoveStaleOverlays(const fs::path& out) {
    for (const fs::path& entry : Entries(out)) {
        if (!StartsWith(Utf8(entry.filename()), "mod-game-") || !IsDir(entry) || IsLink(entry)) continue;
        try {
            RemoveOverlay(entry);
        } catch (const Error&) {
        }
    }
}

fs::path MakeTemporary(const fs::path& out) {
    std::random_device random;
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz0123456789_";
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::string name = "mod-game-";
        for (int i = 0; i < 8; ++i) name += letters[random() % (sizeof letters - 1)];
        const fs::path path = out / Path(name);
        if (CreateDirectoryW(path.c_str(), nullptr)) return path;
    }
    throw Error("cannot create a mod overlay in " + Utf8(out));
}

bool ChangesGameplay(const std::wstring& key) {
    for (const wchar_t* folder : {L"dvdroot_ps4/param/", L"dvdroot_ps4/event/", L"dvdroot_ps4/script/",
                                  L"dvdroot_ps4/map/mapstudio/", L"dvdroot_ps4/action/"})
        if (key.rfind(folder, 0) == 0) return true;
    return key.rfind(L"dvdroot_ps4/chr/", 0) == 0 &&
           (key.find(L".anibnd") != std::wstring::npos || key.find(L".behbnd") != std::wstring::npos);
}

// A randomized world is a mod folder with randomizer_manifest.json. Its [rando] header names the world, so a log tells
// engine, co-op and randomizer bugs apart; BB_RANDO=1 turns on the game side's [rando] lines (runtime_file.c).
// Runs once at start; nothing here touches a frame.
bool NoteRandomizerWorld(const std::wstring& name, const fs::path& root) {
    const fs::path path = root / L"randomizer_manifest.json";
    if (!IsFile(path)) return false;
    try {
        const auto m = nlohmann::json::parse(ReadText(path));
        std::string mods;
        for (const auto& mod : m.value("mods", nlohmann::json::array()))
            mods += (mods.empty() ? "" : ", ") + mod.get<std::string>();
        Log("[rando] world %s, generator %d, built on %s, input w:%.8s",
            m.value("code", std::string("?")).c_str(), m.value("generator", 0), mods.empty() ? "the game alone" : mods.c_str(),
            m.value("input_hash", std::string("?")).c_str());
    } catch (const std::exception&) {
        Log("[rando] world %s: manifest unreadable", Utf8(name).c_str());
    }
    return true;
}

fs::path BuildOverlay(fs::path game, const fs::path& out_base, const std::vector<std::pair<std::wstring, fs::path>>& mods,
                      const std::optional<fs::path>& update) {
    game = fs::canonical(game);
    fs::path out = OverlayBase(game, out_base);
    if (IsDir(out)) RemoveStaleOverlays(out);
    std::vector<std::pair<fs::path, fs::path>> replacements;
    std::map<std::wstring, std::wstring> owners;
    if (update) {
        std::vector<std::pair<fs::path, fs::path>> files;
        const fs::path root = fs::canonical(*update);
        Walk(root, root, files, false);
        for (const auto& [relative, source] : files) {
            owners[Fold(relative.generic_wstring())] = update->filename().native();
            replacements.emplace_back(relative, source);
        }
        Log("Game update: %s: %zu files over the game", Utf8(update->filename()).c_str(), files.size());
    }
    std::size_t gameplay = 0;
    UnsetEnv("BB_RANDO");
    // Files two enabled mods both ship, per (later mod, earlier mod) in load order: the later one wins.
    std::map<std::pair<std::wstring, std::wstring>, std::vector<std::wstring>> overrides;
    std::set<std::wstring> mod_names;
    for (const auto& [name, root] : mods) {
        if (NoteRandomizerWorld(name, root)) SetEnv("BB_RANDO", "1");
        std::size_t count = 0;
        for (const auto& [relative, source] : ModFiles(root)) {
            const std::wstring key = Fold(relative.generic_wstring());
            gameplay += ChangesGameplay(key);
            if (const auto it = owners.find(key); it != owners.end() && mod_names.count(it->second) && it->second != name)
                overrides[{name, it->second}].push_back(relative.generic_wstring());
            owners[key] = name;
            replacements.emplace_back(relative, source);
            ++count;
        }
        Log("Mods: %s: %zu files", Utf8(name).c_str(), count);
        mod_names.insert(name);
    }
    for (const auto& [pair, files] : overrides) {
        std::string list;
        for (std::size_t i = 0; i < files.size() && i < 5; ++i) list += (i ? ", " : "") + Utf8(files[i]);
        if (files.size() > 5) list += " (+" + std::to_string(files.size() - 5) + " more)";
        Log("Mods: WARNING: %s overrides %s files (%zu): %s", Utf8(pair.first).c_str(), Utf8(pair.second).c_str(),
            files.size(), list.c_str());
    }
    if (gameplay)
        Log("Mods: %zu of these files change gameplay (parameters, map layouts, events, AI, animations): "
            "co-op partners need the same files, or enemies and items desync between you",
            gameplay);
    UnsetEnv("BB_MOD_FILES");
    if (replacements.empty()) return game;
    fs::create_directories(out);
    out = fs::canonical(out);
    if (!JunctionsWork(out)) {
        const fs::path list = out / L"mod-files.txt";
        std::string text;
        std::set<std::wstring> seen;
        for (auto it = replacements.rbegin(); it != replacements.rend(); ++it) {
            if (!seen.insert(Fold(it->first.generic_wstring())).second) continue;
            text += Utf8(it->first.generic_wstring()) + "\t" + Utf8(fs::absolute(it->second)) + "\n";
        }
        WriteText(list, text);
        SetEnv("BB_MOD_FILES", Utf8(list));
        Log("Mods: %s cannot hold links (an exFAT or FAT32 drive?): the engine reads the %zu mod files in place",
            Utf8(out.root_path()).c_str(), seen.size());
        return game;
    }
    const fs::path overlay = MakeTemporary(out);
    try {
        for (const fs::path& entry : Entries(game)) Link(overlay / entry.filename(), entry);
        std::size_t replaced = 0, added = 0;
        for (const auto& [relative, source] : replacements) {
            fs::path parent = overlay;
            std::vector<fs::path> parts(relative.begin(), relative.end());
            for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
                parent = Child(parent, parts[i].native());
                Expand(parent);
            }
            const fs::path destination = Child(parent, parts.back().native());
            if (IsFile(destination) && !IsLink(destination)) {
                ++replaced;
                Unlink(destination);
            } else if (IsLink(destination) && !IsDir(destination)) {
                ++replaced;
                Unlink(destination);
            } else if (Exists(destination)) {
                throw Error("File/directory conflict: " + Utf8(relative));
            } else {
                ++added;
            }
            Link(destination, source);
        }
        Log("Mods: %zu game files replaced, %zu added", replaced, added);
        return overlay;
    } catch (...) {
        try {
            RemoveOverlay(overlay);
        } catch (...) {
        }
        throw;
    }
}

// ---- Party version check (the seamless fork's scripts/mods.py party_mod_set) ----
// out/party_mods.txt: the hash of the enabled mods' winning files that change gameplay, read by
// party_runtime.cpp; texture, shader, sound, font ... files do not count.
const std::set<std::wstring> kCosmeticFolders{L"adhoc", L"facegen", L"font", L"menu", L"movie", L"msg",
                                              L"mtd",   L"parts",   L"sfx",  L"shader", L"sound"};
const wchar_t* const kCosmeticSuffixes[] = {L".tpf", L".tpf.dcx", L".tpfbhd", L".tpfbdt", L".texbnd", L".texbnd.dcx", L".dds"};

bool ModFileIsCosmetic(const fs::path& relative) {
    std::vector<std::wstring> parts;
    for (const fs::path& part : relative) parts.push_back(Fold(part.native()));
    if (parts.size() > 2 && kCosmeticFolders.count(parts[1])) return true;
    const std::wstring& last = parts.back();
    for (const wchar_t* suffix : kCosmeticSuffixes) {
        const std::wstring s(suffix);
        if (last.size() >= s.size() && last.compare(last.size() - s.size(), s.size(), s) == 0) return true;
    }
    return false;
}

// SHA-256 of a mod file, from out/party_mods_cache.json while its size and time are unchanged.
std::string CachedFileSha256(const fs::path& path, nlohmann::json& cache) {
    const std::string key = Utf8(path);
    const auto size = std::uint64_t(fs::file_size(path));
    const auto time = std::int64_t(fs::last_write_time(path).time_since_epoch().count());
    if (const auto it = cache.find(key); it != cache.end() && it->is_array() && it->size() == 3 &&
                                         (*it)[0] == size && (*it)[1] == time && (*it)[2].is_string())
        return (*it)[2].get<std::string>();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw Error("cannot read " + key);
    Sha256Stream hash;
    std::vector<std::uint8_t> block(1 << 20);
    DWORD got = 0;
    while (::ReadFile(file, block.data(), DWORD(block.size()), &got, nullptr) && got) hash.Update(block.data(), got);
    CloseHandle(file);
    const std::string digest = hash.HexDigest();
    cache[key] = nlohmann::json::array({size, time, digest});
    return digest;
}

// `features`: the gameplay feature tables' rows (weapon_clip_groups), hashed after the files; "" adds nothing (so a
// set of mods without them keeps its hash).
void WritePartyModsFile(const fs::path& out, const std::vector<std::pair<std::wstring, fs::path>>& layers,
                        std::string_view features) {
    std::map<std::string, std::pair<std::wstring, fs::path>> owners;  // folded game path: (mod, file)
    for (const auto& [name, root] : layers)
        for (const auto& [relative, source] : ModFiles(root)) owners[Utf8(Fold(relative.generic_wstring()))] = {name, source};
    const fs::path cache_file = out / "party_mods_cache.json";
    nlohmann::json cache = nlohmann::json::object();
    try {
        cache = nlohmann::json::parse(ReadText(cache_file));
        if (!cache.is_object()) cache = nlohmann::json::object();
    } catch (const std::exception&) {
    }
    Sha256Stream hash;
    hash.Update("bbparty-mods-1");
    std::map<std::wstring, std::size_t> counts;
    std::set<std::wstring> cosmetic;
    for (const auto& [key, owner] : owners) {
        const auto& [name, source] = owner;
        if (ModFileIsCosmetic(Path(key))) {
            cosmetic.insert(name);
            continue;
        }
        ++counts[name];
        hash.Update(std::string_view("\0file\0", 6));
        hash.Update(key);
        hash.Update(std::string_view("\0", 1));
        hash.Update(CachedFileSha256(source, cache));
    }
    if (!features.empty()) {
        hash.Update(std::string_view("\0features\0", 10));
        hash.Update(features);
    }
    try {
        WriteText(cache_file, cache.dump());
    } catch (const std::exception&) {
    }
    std::string text = "# Party version check (bb-start): the enabled mods that change gameplay files. Players\n"
                       "# with another hash are refused by the party host; texture/shader/sound-only mods do not count.\n";
    text += "hash " + (counts.empty() && features.empty() ? std::string(64, '0') : hash.HexDigest()) + "\n";
    for (const auto& [name, root] : layers)
        if (const auto it = counts.find(name); it != counts.end())
            text += "mod " + Utf8(name) + " (" + std::to_string(it->second) + " files)\n";
    for (const std::wstring& name : cosmetic)
        if (!counts.count(name)) text += "# cosmetic " + Utf8(name) + "\n";
    WriteText(out / "party_mods.txt", text);
}

// ---- Host-only boss events of the mods (scripts/mods.py write_party_host_only_file; host_only.h) ----
void RemoveIfThere(const fs::path& path) {
    if (Exists(path)) Unlink(path);
}

// out/party_host_only.txt from the layers' own party_host_only.txt (beside dvdroot_ps4: no game file, in no hash). A mod
// with a bad list is left out with why; no ids or more than hostonly::kMax: no file.
void WritePartyHostOnlyFile(const fs::path& out, const std::vector<std::pair<std::wstring, fs::path>>& layers) {
    hostonly::Merged merged;
    for (const auto& [name, root] : layers) {
        const fs::path list = root / hostonly::kFile;
        if (!IsFile(list)) continue;
        std::vector<std::uint32_t> own;
        std::string error;
        try {
            const Bytes bytes = ReadFile(list);
            if (!hostonly::ParseModList(Utf8(name), std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                                        &own, &error)) {
                Log("Mods: %s: its host-only events are not used", error.c_str());
                continue;
            }
        } catch (const std::exception& e) {
            Log("Mods: %s: %s: its host-only events are not used", Utf8(list).c_str(), e.what());
            continue;
        }
        hostonly::Add(&merged, Utf8(name), own);
    }
    const fs::path path = out / hostonly::kFile;
    if (merged.ids.size() > hostonly::kMax)
        Log("Mods: %zu host-only events, more than %zu: none are used", merged.ids.size(), hostonly::kMax);
    const std::string text = hostonly::FileText(merged);
    if (text.empty()) {
        RemoveIfThere(path);
        return;
    }
    WriteText(path, text);
    Log("Mods: %zu host-only boss events", merged.ids.size());
}

// ---- Mod manifests and feature tables (features.h; docs/modding/manifest.md) ----
#ifndef NW_VERSION
#define NW_VERSION "0.0.0-unknown"
#endif

std::string ReadSmall(const fs::path& path) {
    const Bytes bytes = ReadFile(path);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// Drops the layers whose nightwatch.json is bad, needs a newer Nightwatch or names an unknown feature (one log line
// each); `manifests` gets the kept layers' manifests (none for a mod without one).
std::vector<std::pair<std::wstring, fs::path>> CheckManifests(const std::vector<std::pair<std::wstring, fs::path>>& layers,
                                                              std::map<std::wstring, nwfeatures::Manifest>* manifests) {
    std::vector<std::pair<std::wstring, fs::path>> kept;
    for (const auto& [name, root] : layers) {
        const fs::path file = root / nwfeatures::kManifest;
        if (!IsFile(file)) {
            kept.emplace_back(name, root);
            continue;
        }
        nwfeatures::Manifest m;
        std::string why;
        try {
            if (!nwfeatures::ParseManifest(ReadSmall(file), &m, &why)) why = "bad manifest: " + why;
        } catch (const std::exception& e) {
            why = std::string("cannot read ") + nwfeatures::kManifest + ": " + e.what();
        }
        if (why.empty()) why = nwfeatures::Refusal(m, NW_VERSION);
        if (!why.empty()) {
            Log("Mods: %s: %s: not loaded", Utf8(name).c_str(), why.c_str());
            continue;
        }
        std::string features;
        for (const std::string& f : m.features) features += (features.empty() ? "" : ", ") + f;
        Log("Mods: %s: manifest %s %s, requires %s, features %s", Utf8(name).c_str(), m.id.c_str(),
            m.version.empty() ? "-" : m.version.c_str(), m.min_version.c_str(), features.empty() ? "none" : features.c_str());
        (*manifests)[name] = m;
        kept.emplace_back(name, root);
    }
    return kept;
}

// out/nightwatch_hit_effects.txt from the layers' nightwatch/hit_effects.json (only for a manifest listing the
// feature). A visual feature: not in the party hash (each client draws its own; vanilla look without the mod).
void WriteHitEffectsFile(const fs::path& out, const std::vector<std::pair<std::wstring, fs::path>>& layers,
                         const std::map<std::wstring, nwfeatures::Manifest>& manifests) {
    nwfeatures::HitMerged merged;
    for (const auto& [name, root] : layers) {
        const fs::path table = root / nwfeatures::kTablesDir / (std::string(nwfeatures::kHitEffects) + ".json");
        if (!IsFile(table)) continue;
        const auto m = manifests.find(name);
        if (m == manifests.end() || !m->second.Uses(nwfeatures::kHitEffects)) {
            Log("Mods: %s: has nightwatch/hit_effects.json but its nightwatch.json does not list hit_effects: not used",
                Utf8(name).c_str());
            continue;
        }
        std::vector<nwfeatures::HitRow> rows;
        std::string error;
        try {
            if (!nwfeatures::ParseHitEffects(ReadSmall(table), &rows, &error)) {
                Log("Mods: %s: %s: its hit_effects table is not used", Utf8(name).c_str(), error.c_str());
                continue;
            }
        } catch (const std::exception& e) {
            Log("Mods: %s: %s: its hit_effects table is not used", Utf8(table).c_str(), e.what());
            continue;
        }
        nwfeatures::AddHitEffects(&merged, Utf8(name), rows);
    }
    const fs::path path = out / nwfeatures::kHitEffectsOut;
    const std::string text = nwfeatures::HitEffectsFileText(merged);
    if (text.empty()) {
        RemoveIfThere(path);
        return;
    }
    WriteText(path, text);
    Log("Mods: hit_effects: %zu rows from %zu mods", merged.rows.size(), merged.names.size());
}

// out/nightwatch_weapon_clip_groups.txt from the layers' nightwatch/weapon_clip_groups.json (only for a manifest
// listing the feature). Gameplay (which clips a weapon plays): returns the rows ("weapon group" lines, no comment)
// for the party hash, "" when there are none.
std::string WriteWeaponClipGroupsFile(const fs::path& out, const std::vector<std::pair<std::wstring, fs::path>>& layers,
                                      const std::map<std::wstring, nwfeatures::Manifest>& manifests) {
    const std::string feature = nwfeatures::kWeaponClipGroups;
    nwfeatures::ClipMerged merged;
    for (const auto& [name, root] : layers) {
        const fs::path table = root / nwfeatures::kTablesDir / (feature + ".json");
        if (!IsFile(table)) continue;
        const auto m = manifests.find(name);
        if (m == manifests.end() || !m->second.Uses(feature)) {
            Log("Mods: %s: has nightwatch/%s.json but its nightwatch.json does not list %s: not used", Utf8(name).c_str(),
                feature.c_str(), feature.c_str());
            continue;
        }
        std::vector<nwfeatures::ClipRow> rows;
        std::string error;
        try {
            if (!nwfeatures::ParseWeaponClipGroups(ReadSmall(table), &rows, &error)) {
                Log("Mods: %s: %s: its %s table is not used", Utf8(name).c_str(), error.c_str(), feature.c_str());
                continue;
            }
        } catch (const std::exception& e) {
            Log("Mods: %s: %s: its %s table is not used", Utf8(table).c_str(), e.what(), feature.c_str());
            continue;
        }
        nwfeatures::AddWeaponClipGroups(&merged, Utf8(name), rows);
    }
    const fs::path path = out / nwfeatures::kWeaponClipGroupsOut;
    const std::string text = nwfeatures::WeaponClipGroupsFileText(merged);
    if (text.empty()) {
        RemoveIfThere(path);
        return "";
    }
    WriteText(path, text);
    Log("Mods: %s: %zu rows from %zu mods", feature.c_str(), merged.rows.size(), merged.names.size());
    return text.substr(text.find('\n') + 1);
}

// out/nightwatch_sounds.txt from the layers' nightwatch/sounds.json (only for a manifest listing the feature). Each
// row's file must be a file of at most 4 MiB inside that mod; a row whose file is missing is dropped (one log line),
// the rest of the table is used. Cosmetic (each client plays its own): not in the party hash.
void WriteSoundsFile(const fs::path& out, const std::vector<std::pair<std::wstring, fs::path>>& layers,
                     const std::map<std::wstring, nwfeatures::Manifest>& manifests) {
    const std::string feature = nwfeatures::kSounds;
    nwfeatures::SoundMerged merged;
    for (const auto& [name, root] : layers) {
        const fs::path table = root / nwfeatures::kTablesDir / (feature + ".json");
        if (!IsFile(table)) continue;
        const auto m = manifests.find(name);
        if (m == manifests.end() || !m->second.Uses(feature)) {
            Log("Mods: %s: has nightwatch/%s.json but its nightwatch.json does not list %s: not used", Utf8(name).c_str(),
                feature.c_str(), feature.c_str());
            continue;
        }
        std::vector<nwfeatures::SoundRow> rows;
        std::string error;
        try {
            if (!nwfeatures::ParseSounds(ReadSmall(table), &rows, &error)) {
                Log("Mods: %s: %s: its %s table is not used", Utf8(name).c_str(), error.c_str(), feature.c_str());
                continue;
            }
        } catch (const std::exception& e) {
            Log("Mods: %s: %s: its %s table is not used", Utf8(table).c_str(), e.what(), feature.c_str());
            continue;
        }
        std::vector<nwfeatures::SoundRow> found;
        for (nwfeatures::SoundRow row : rows) {
            const fs::path file = fs::absolute(root / Wide(row.file));
            std::error_code ec;
            const auto size = IsFile(file) ? fs::file_size(file, ec) : 0;
            if (!IsFile(file) || ec || size == 0 || size > (4u << 20)) {
                Log("Mods: %s: sounds: %s: %s is missing or not a file of 1 byte..4 MiB: row not used", Utf8(name).c_str(),
                    row.event.c_str(), row.file.c_str());
                continue;
            }
            row.file = Utf8(file);
            found.push_back(std::move(row));
        }
        nwfeatures::AddSounds(&merged, Utf8(name), found);
    }
    const fs::path path = out / nwfeatures::kSoundsOut;
    const std::string text = nwfeatures::SoundsFileText(merged);
    if (text.empty()) {
        RemoveIfThere(path);
        return;
    }
    WriteText(path, text);
    Log("Mods: %s: %zu rows from %zu mods", feature.c_str(), merged.rows.size(), merged.names.size());
}

} // namespace

void RemoveOverlay(const fs::path& path) {
    if (IsLink(path) || !IsDir(path)) {
        if (IsLink(path) || Exists(path)) Unlink(path);
        return;
    }
    for (const fs::path& entry : Entries(path)) RemoveOverlay(entry);
    if (!RemoveDirectoryW(path.c_str())) throw Error("cannot remove " + Utf8(path));
}

fs::path BuildModsOverlay(const fs::path& game, const fs::path& out, const fs::path& mods_dir, const fs::path& config,
                          bool enabled) {
    RemoveIfThere(out / hostonly::kFile);  // a failed start leaves none: the host then has the fixed list only
    RemoveIfThere(out / nwfeatures::kHitEffectsOut);  // likewise: a failed start leaves the vanilla look
    RemoveIfThere(out / nwfeatures::kWeaponClipGroupsOut);  // and the vanilla clips
    RemoveIfThere(out / nwfeatures::kSoundsOut);  // and the game's own sounds
    std::vector<std::pair<std::wstring, fs::path>> layers;
    if (enabled) {
        fs::path legacy = fs::canonical(game);
        legacy += L"-mods";
        if (IsDir(legacy)) layers.emplace_back(legacy.filename().native(), legacy);
        if (IsDir(Child(mods_dir, L"dvdroot_ps4"))) {
            layers.emplace_back(mods_dir.filename().native(), mods_dir);
        } else {
            for (const std::wstring& name : Selected(mods_dir, config)) layers.emplace_back(name, mods_dir / name);
        }
    }
    std::map<std::wstring, nwfeatures::Manifest> manifests;
    layers = CheckManifests(layers, &manifests);
    std::string known;
    for (const std::string& f : nwfeatures::Known()) known += (known.empty() ? "" : ", ") + f;
    Log("Mods: Nightwatch %s, features: %s", NW_VERSION, known.c_str());
    fs::create_directories(out);
    WritePartyModsFile(out, layers, WriteWeaponClipGroupsFile(out, layers, manifests));
    const std::optional<fs::path> update = UpdateFolder(fs::canonical(game));
    // The party version check hashes the dump's own gameparam: with a separate update folder that is the update's
    // file (the base folder's is the pre-update one the game never loads).
    if (update) SetEnv("BB_GAME_UPDATE_DIR", Utf8(fs::canonical(*update)));
    else UnsetEnv("BB_GAME_UPDATE_DIR");
    const fs::path view = BuildOverlay(game, out, layers, update);
    WritePartyHostOnlyFile(out, layers);  // after the view, as scripts/mods.py main
    WriteHitEffectsFile(out, layers, manifests);
    WriteSoundsFile(out, layers, manifests);
    return view;
}

}
