// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

#include "video_core/renderer_vulkan/vk_optiscaler.h"

namespace Vulkan::OptiScaler {

namespace {

bool EnvOn(const char* name) {
    const char* env = std::getenv(name);
    return !env || env[0] != '0';
}

#ifdef _WIN32
std::string Narrow(const std::wstring& text) {
    std::string out;
    for (const wchar_t c : text) {
        out.push_back(c < 128 ? char(c) : '?');
    }
    return out;
}

/// "x.y.z.w" from the file's version resource (version.dll is loaded here, not linked).
std::string FileVersion(const wchar_t* path) {
    const HMODULE dll = LoadLibraryW(L"version.dll");
    if (!dll) {
        return {};
    }
    using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using InfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    const auto size_fn = reinterpret_cast<SizeFn>(GetProcAddress(dll, "GetFileVersionInfoSizeW"));
    const auto info_fn = reinterpret_cast<InfoFn>(GetProcAddress(dll, "GetFileVersionInfoW"));
    const auto query_fn = reinterpret_cast<QueryFn>(GetProcAddress(dll, "VerQueryValueW"));
    std::string version;
    DWORD handle = 0;
    const DWORD size = size_fn && info_fn && query_fn ? size_fn(path, &handle) : 0;
    if (size) {
        std::vector<char> data(size);
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT length = 0;
        if (info_fn(path, 0, size, data.data()) &&
            query_fn(data.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed), &length) && fixed &&
            fixed->dwFileVersionMS) {
            char text[64];
            std::snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(fixed->dwFileVersionMS),
                          LOWORD(fixed->dwFileVersionMS), HIWORD(fixed->dwFileVersionLS),
                          LOWORD(fixed->dwFileVersionLS));
            version = text;
        }
    }
    FreeLibrary(dll);
    return version;
}

/// OptiScaler exports the NGX entry points under a proxy name (winmm.dll, dxgi.dll, version.dll,
/// ...) and keeps OptiScaler.ini beside itself. The driver's own NGX core (_nvngx.dll / nvngx.dll)
/// exports them too and counts only with that ini.
Info Find() {
    Info info;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        return info;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Module32FirstW(snapshot, &entry); more && !info.loaded;
         more = Module32NextW(snapshot, &entry)) {
        std::wstring name = entry.szModule;
        for (auto& c : name) {
            c = wchar_t(std::towlower(c));
        }
        if (!GetProcAddress(entry.hModule, "NVSDK_NGX_VULKAN_Init_Ext2")) {
            continue;
        }
        std::error_code ec;
        const bool ini = std::filesystem::exists(
            std::filesystem::path(entry.szExePath).parent_path() / L"OptiScaler.ini", ec);
        const bool ngx_core = name == L"_nvngx.dll" || name == L"nvngx.dll";
        if (ngx_core && !ini) {
            continue;
        }
        info.loaded = true;
        info.module = Narrow(entry.szModule);
        info.version = FileVersion(entry.szExePath);
    }
    CloseHandle(snapshot);
    return info;
}
#else
Info Find() {
    return {};
}
#endif

} // namespace

const Info& Detect() {
    static const Info info = [] {
        Info found = Find();
        if (found.loaded) {
            std::printf("OptiScaler: %s%s loaded as %s; compatibility mode: %s\n",
                        found.version.empty() ? "" : "v",
                        found.version.empty() ? "(version unknown)" : found.version.c_str(),
                        found.module.c_str(),
                        EnvOn("BB_OPTISCALER_COMPAT")
                            ? "on - submissions from one thread (BB_OPTISCALER_SYNC_SUBMIT), "
                              "NGX calls under the queue lock (BB_OPTISCALER_NGX_LOCK); "
                              "BB_OPTISCALER_COMPAT=0 turns it off"
                            : "off (BB_OPTISCALER_COMPAT=0)");
        }
        return found;
    }();
    return info;
}

bool Compat() {
    static const bool on = Detect().loaded && EnvOn("BB_OPTISCALER_COMPAT");
    return on;
}

bool Part(const char* name) {
    return Compat() && EnvOn(name);
}

} // namespace Vulkan::OptiScaler
