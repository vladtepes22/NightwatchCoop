// SPDX-License-Identifier: GPL-2.0-or-later
// bb-start: the native start of Bloodborne PC (what scripts/run_windows.py and the scripts it ran
// did): the game's boot image, mods, patches, the engine's own assets, then bb-probe. Shared
// helpers: files, hashes, the environment the engine gets, the session log.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace start {

/// A failure that ends the start with a message (the scripts' ValueError/OSError).
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Text is UTF-8 everywhere; paths convert at the Windows API.
fs::path Path(std::string_view utf8);
std::string Utf8(const fs::path& path);
std::string Utf8(std::wstring_view text);
inline std::string Utf8(const std::wstring& text) { return Utf8(std::wstring_view(text)); }
inline std::string Utf8(const wchar_t* text) { return Utf8(std::wstring_view(text)); }
std::wstring Wide(std::string_view utf8);

Bytes ReadFile(const fs::path& path);
std::string ReadText(const fs::path& path);
/// Written whole to a temporary beside it, then moved over it.
void WriteFile(const fs::path& path, const void* data, std::size_t size);
inline void WriteFile(const fs::path& path, const Bytes& data) { WriteFile(path, data.data(), data.size()); }
inline void WriteText(const fs::path& path, std::string_view text) { WriteFile(path, text.data(), text.size()); }

std::string Sha256Hex(const void* data, std::size_t size);
inline std::string Sha256Hex(const Bytes& data) { return Sha256Hex(data.data(), data.size()); }
Bytes Sha1(const void* data, std::size_t size);
/// SHA-256 over several files' names and contents, as run_windows.py prepare_key.
class Sha256Stream {
public:
    Sha256Stream();
    ~Sha256Stream();
    void Update(const void* data, std::size_t size);
    void Update(std::string_view text) { Update(text.data(), text.size()); }
    std::string HexDigest();
private:
    void* hash_ = nullptr;
    void* algorithm_ = nullptr;
};
std::uint32_t Crc32(const void* data, std::size_t size);

// The environment bb-probe gets (and this process reads).
std::optional<std::string> Env(const char* name);
std::string Env(const char* name, std::string_view fallback);
void SetEnv(const char* name, std::string_view value);
void UnsetEnv(const char* name);

/// The system commit limit (RAM plus page file) in MiB.
std::uint64_t CommitLimitMb();
/// 0 when Windows made the engine's memory pool of `mib` MiB (kept for bb-probe, see PoolHandOff),
/// else the Windows error it refused it with (1450, 1455: a laptop's or a small page file's limits).
unsigned long PoolRefused(std::uint64_t mib);
/// The pool section made for `mib` MiB goes to bb-probe (BB_POOL_SECTION, an inherited handle); the
/// others are closed.
void PoolHandOff(std::uint64_t mib);

/// bbport.ini as the scripts read it: key=value, # and ; comments.
std::map<std::string, std::string> ReadSettings(const fs::path& path);
std::optional<std::string> Setting(const std::map<std::string, std::string>& settings, const std::string& key);

/// Everything printed goes to BB_LOG_FILE when the launcher gives one (it shows that file).
void Log(const char* format, ...)
#if defined(__clang__) || defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void InitLog();
/// The log's file handle for bb-probe's output (null: inherit this process's).
void* LogHandle();

std::string Hex(std::uint64_t value);
std::string Lower(std::string text);
std::string Trim(std::string_view text);
std::vector<std::string> Split(std::string_view text, char separator);
bool StartsWith(std::string_view text, std::string_view prefix);
Bytes FromHex(std::string_view hex);
std::string ToHex(const std::uint8_t* data, std::size_t size);

template <typename T> T Load(const std::uint8_t* data) {
    T value;
    std::memcpy(&value, data, sizeof value);
    return value;
}
template <typename T> void Store(std::uint8_t* data, T value) { std::memcpy(data, &value, sizeof value); }
template <typename T> void Append(Bytes& out, T value) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof value);
}
inline void Append(Bytes& out, const void* data, std::size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    out.insert(out.end(), p, p + size);
}

} // namespace start
