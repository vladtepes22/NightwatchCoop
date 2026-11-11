// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_handle_trace.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <winternl.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace BbHandleTrace {
namespace {
std::mutex g_mu;
std::map<std::string, unsigned long long> g_counts; // "kind module+rva" -> creations
std::atomic<bool> g_started{false};
thread_local bool t_busy = false;

std::string ModuleOf(void* addr) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &m) || !m) {
        return "?";
    }
    char path[MAX_PATH];
    GetModuleFileNameA(m, path, sizeof(path));
    const char* base = strrchr(path, '\\');
    char out[MAX_PATH + 24];
    snprintf(out, sizeof(out), "%s+0x%llx", base ? base + 1 : path,
             static_cast<unsigned long long>(reinterpret_cast<char*>(addr) - reinterpret_cast<char*>(m)));
    return out;
}

bool IsSystemCaller(const std::string& s) {
    static const char* skip[] = {"kernel32.dll", "KERNEL32.DLL", "KERNELBASE.dll", "ntdll.dll"};
    for (const char* k : skip) {
        if (s.compare(0, strlen(k), k) == 0) {
            return true;
        }
    }
    return false;
}

std::map<HANDLE, std::string> g_live; // handle value -> "kind caller" while open (stale entries are overwritten)

__attribute__((noinline)) std::string Who(const char* kind) {
    void* frames[16];
    const USHORT n = RtlCaptureStackBackTrace(3, 16, frames, nullptr);
    std::string who = "?";
    for (USHORT i = 0; i < n; ++i) {
        const std::string s = ModuleOf(frames[i]);
        if (!IsSystemCaller(s)) {
            who = s;
            // winpthreads is the pthread layer of std::mutex / std::thread: name its callers too
            USHORT j = i + 1;
            while (j < n && ModuleOf(frames[j]).compare(0, 12, "libwinpthrea") == 0) {
                ++j;
            }
            if (j < n) {
                who += " < " + ModuleOf(frames[j]);
                if (j + 1 < n) {
                    who += " < " + ModuleOf(frames[j + 1]);
                }
            }
            break;
        }
    }
    // The creating thread's name (SetThreadDescription: the guest threads and most host threads set one).
    using GetThreadDescription_t = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static auto get_desc = reinterpret_cast<GetThreadDescription_t>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetThreadDescription")));
    if (get_desc) {
        PWSTR desc = nullptr;
        if (SUCCEEDED(get_desc(GetCurrentThread(), &desc)) && desc) {
            char narrow[64] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
            LocalFree(desc);
            who += std::string(" [thread '") + (narrow[0] ? narrow : "?") + "']";
        }
    }
    return std::string(kind) + " " + who;
}

__attribute__((noinline)) void Note(const char* kind, HANDLE h) {
    if (t_busy) {
        return;
    }
    t_busy = true;
    const std::string key = Who(kind);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ++g_counts[key];
        if (h && h != INVALID_HANDLE_VALUE) {
            g_live[h] = key;
        }
    }
    t_busy = false;
}

void NoteClose(HANDLE h) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_live.erase(h);
}

using CreateEventW_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR);
using CreateEventExW_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
using CreateSemaphoreW_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LONG, LONG, LPCWSTR);
using CreateSemaphoreExW_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LONG, LONG, LPCWSTR, DWORD, DWORD);
using NtCreateEvent_t = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, PVOID, ULONG, BOOLEAN);
using NtCreateSemaphore_t = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, PVOID, LONG, LONG);
CreateEventW_t o_CreateEventW;
CreateEventExW_t o_CreateEventExW;
CreateSemaphoreW_t o_CreateSemaphoreW;
CreateSemaphoreExW_t o_CreateSemaphoreExW;
NtCreateEvent_t o_NtCreateEvent;
NtCreateSemaphore_t o_NtCreateSemaphore;

HANDLE WINAPI h_CreateEventW(LPSECURITY_ATTRIBUTES a, BOOL b, BOOL c, LPCWSTR d) {
    HANDLE r = o_CreateEventW(a, b, c, d);
    Note("CreateEventW", r);
    return r;
}
HANDLE WINAPI h_CreateEventExW(LPSECURITY_ATTRIBUTES a, LPCWSTR b, DWORD c, DWORD d) {
    HANDLE r = o_CreateEventExW(a, b, c, d);
    Note("CreateEventExW", r);
    return r;
}
HANDLE WINAPI h_CreateSemaphoreW(LPSECURITY_ATTRIBUTES a, LONG b, LONG c, LPCWSTR d) {
    HANDLE r = o_CreateSemaphoreW(a, b, c, d);
    Note("CreateSemaphoreW", r);
    return r;
}
HANDLE WINAPI h_CreateSemaphoreExW(LPSECURITY_ATTRIBUTES a, LONG b, LONG c, LPCWSTR d, DWORD e, DWORD f) {
    HANDLE r = o_CreateSemaphoreExW(a, b, c, d, e, f);
    Note("CreateSemaphoreExW", r);
    return r;
}
LONG NTAPI h_NtCreateEvent(PHANDLE a, ACCESS_MASK b, PVOID c, ULONG d, BOOLEAN e) {
    LONG r = o_NtCreateEvent(a, b, c, d, e);
    Note("NtCreateEvent", r >= 0 ? *a : nullptr);
    return r;
}
LONG NTAPI h_NtCreateSemaphore(PHANDLE a, ACCESS_MASK b, PVOID c, LONG d, LONG e) {
    LONG r = o_NtCreateSemaphore(a, b, c, d, e);
    Note("NtCreateSemaphore", r >= 0 ? *a : nullptr);
    return r;
}

using CloseHandle_t = BOOL(WINAPI*)(HANDLE);
using NtClose_t = LONG(NTAPI*)(HANDLE);
CloseHandle_t o_CloseHandle;
NtClose_t o_NtClose;
BOOL WINAPI h_CloseHandle(HANDLE h) {
    NoteClose(h);
    return o_CloseHandle(h);
}
LONG NTAPI h_NtClose(HANDLE h) {
    NoteClose(h);
    return o_NtClose(h);
}

using DuplicateHandle_t = BOOL(WINAPI*)(HANDLE, HANDLE, HANDLE, LPHANDLE, DWORD, BOOL, DWORD);
DuplicateHandle_t o_DuplicateHandle;
bool IsEventHandle(HANDLE h) {
    using NtQueryObject_t = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static auto q = reinterpret_cast<NtQueryObject_t>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject")));
    alignas(8) char buf[512];
    ULONG ret = 0;
    if (!q || q(h, 2, buf, sizeof(buf), &ret) < 0) {
        return false;
    }
    const auto* name = reinterpret_cast<const UNICODE_STRING*>(buf);
    return name->Length == 10 && !wcsncmp(name->Buffer, L"Event", 5);
}
BOOL WINAPI h_DuplicateHandle(HANDLE sp, HANDLE sh, HANDLE tp, LPHANDLE th, DWORD acc, BOOL inh, DWORD opt) {
    const BOOL r = o_DuplicateHandle(sp, sh, tp, th, acc, inh, opt);
    if (r && th && *th && !(opt & DUPLICATE_CLOSE_SOURCE) && tp == GetCurrentProcess() && IsEventHandle(*th)) {
        Note("DuplicateHandle(Event)", *th);
    }
    return r;
}

// Critical sections create their LockSemaphore Event inside ntdll when first contended and give it back only in
// DeleteCriticalSection: a section initialised and never deleted leaks an Event without any CreateEvent call.
using InitCS_t = VOID(WINAPI*)(LPCRITICAL_SECTION);
using InitCSEx_t = BOOL(WINAPI*)(LPCRITICAL_SECTION, DWORD, DWORD);
using InitCSSpin_t = BOOL(WINAPI*)(LPCRITICAL_SECTION, DWORD);
using DelCS_t = VOID(WINAPI*)(LPCRITICAL_SECTION);
InitCS_t o_InitCS;
InitCSEx_t o_InitCSEx;
InitCSSpin_t o_InitCSSpin;
DelCS_t o_DelCS;
std::map<void*, std::string> g_cs;
void NoteCS(void* cs, const char* kind) {
    if (t_busy) {
        return;
    }
    t_busy = true;
    const std::string key = Who(kind);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ++g_counts[key];
        g_cs[cs] = key;
    }
    t_busy = false;
}
VOID WINAPI h_InitCS(LPCRITICAL_SECTION cs) {
    o_InitCS(cs);
    NoteCS(cs, "InitializeCriticalSection");
}
BOOL WINAPI h_InitCSEx(LPCRITICAL_SECTION cs, DWORD a, DWORD b) {
    const BOOL r = o_InitCSEx(cs, a, b);
    NoteCS(cs, "InitializeCriticalSectionEx");
    return r;
}
BOOL WINAPI h_InitCSSpin(LPCRITICAL_SECTION cs, DWORD a) {
    const BOOL r = o_InitCSSpin(cs, a);
    NoteCS(cs, "InitializeCriticalSectionAndSpinCount");
    return r;
}
VOID WINAPI h_DelCS(LPCRITICAL_SECTION cs) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_cs.erase(cs);
    }
    o_DelCS(cs);
}

struct Target {
    void** original;
    void* hook;
};

void PatchModule(HMODULE mod, const Target* targets, int count) {
    auto* base = reinterpret_cast<char*>(mod);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) {
        return;
    }
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; thunk->u1.Function; ++thunk) {
            for (int i = 0; i < count; ++i) {
                if (*targets[i].original && reinterpret_cast<void*>(thunk->u1.Function) == *targets[i].original) {
                    DWORD old;
                    if (VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                        thunk->u1.Function = reinterpret_cast<ULONG_PTR>(targets[i].hook);
                        VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
                    }
                }
            }
        }
    }
}

void PatchAll() {
    const Target targets[] = {
        {reinterpret_cast<void**>(&o_CreateEventW), reinterpret_cast<void*>(h_CreateEventW)},
        {reinterpret_cast<void**>(&o_CreateEventExW), reinterpret_cast<void*>(h_CreateEventExW)},
        {reinterpret_cast<void**>(&o_CreateSemaphoreW), reinterpret_cast<void*>(h_CreateSemaphoreW)},
        {reinterpret_cast<void**>(&o_CreateSemaphoreExW), reinterpret_cast<void*>(h_CreateSemaphoreExW)},
        {reinterpret_cast<void**>(&o_NtCreateEvent), reinterpret_cast<void*>(h_NtCreateEvent)},
        {reinterpret_cast<void**>(&o_NtCreateSemaphore), reinterpret_cast<void*>(h_NtCreateSemaphore)},
        {reinterpret_cast<void**>(&o_DuplicateHandle), reinterpret_cast<void*>(h_DuplicateHandle)},
        {reinterpret_cast<void**>(&o_InitCS), reinterpret_cast<void*>(h_InitCS)},
        {reinterpret_cast<void**>(&o_InitCSEx), reinterpret_cast<void*>(h_InitCSEx)},
        {reinterpret_cast<void**>(&o_InitCSSpin), reinterpret_cast<void*>(h_InitCSSpin)},
        {reinterpret_cast<void**>(&o_DelCS), reinterpret_cast<void*>(h_DelCS)},
        {reinterpret_cast<void**>(&o_CloseHandle), reinterpret_cast<void*>(h_CloseHandle)},
        {reinterpret_cast<void**>(&o_NtClose), reinterpret_cast<void*>(h_NtClose)},
    };
    HMODULE mods[1024];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        return;
    }
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; ++i) {
        PatchModule(mods[i], targets, 13);
    }
}

void Start() {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    o_CreateEventW = reinterpret_cast<CreateEventW_t>(reinterpret_cast<void*>(GetProcAddress(k32, "CreateEventW")));
    o_CreateEventExW = reinterpret_cast<CreateEventExW_t>(reinterpret_cast<void*>(GetProcAddress(k32, "CreateEventExW")));
    o_CreateSemaphoreW = reinterpret_cast<CreateSemaphoreW_t>(reinterpret_cast<void*>(GetProcAddress(k32, "CreateSemaphoreW")));
    o_CreateSemaphoreExW = reinterpret_cast<CreateSemaphoreExW_t>(reinterpret_cast<void*>(GetProcAddress(k32, "CreateSemaphoreExW")));
    o_NtCreateEvent = reinterpret_cast<NtCreateEvent_t>(reinterpret_cast<void*>(GetProcAddress(nt, "NtCreateEvent")));
    o_NtCreateSemaphore = reinterpret_cast<NtCreateSemaphore_t>(reinterpret_cast<void*>(GetProcAddress(nt, "NtCreateSemaphore")));
    o_DuplicateHandle = reinterpret_cast<DuplicateHandle_t>(reinterpret_cast<void*>(GetProcAddress(k32, "DuplicateHandle")));
    o_InitCS = reinterpret_cast<InitCS_t>(reinterpret_cast<void*>(GetProcAddress(k32, "InitializeCriticalSection")));
    o_InitCSEx = reinterpret_cast<InitCSEx_t>(reinterpret_cast<void*>(GetProcAddress(k32, "InitializeCriticalSectionEx")));
    o_InitCSSpin = reinterpret_cast<InitCSSpin_t>(reinterpret_cast<void*>(GetProcAddress(k32, "InitializeCriticalSectionAndSpinCount")));
    o_DelCS = reinterpret_cast<DelCS_t>(reinterpret_cast<void*>(GetProcAddress(k32, "DeleteCriticalSection")));
    o_CloseHandle = reinterpret_cast<CloseHandle_t>(reinterpret_cast<void*>(GetProcAddress(k32, "CloseHandle")));
    o_NtClose = reinterpret_cast<NtClose_t>(reinterpret_cast<void*>(GetProcAddress(nt, "NtClose")));
    PatchAll();
    std::printf("Handle trace: BB_HANDLE_TRACE on (Event / Semaphore creations by caller, every 30 s)\n");
}
} // namespace

// BB_MEM_BREAKDOWN=1: every 60 s the committed address space by owner type (image / mapped section / private),
// private regions of 16 MiB and more one by one (a growing one is a heap segment or an allocator arena),
// the rest summed, and the heaps' sizes. Diagnostic only.
static void MemoryBreakdown() {
    static const bool on = [] {
        const char* e = std::getenv("BB_MEM_BREAKDOWN");
        return e && e[0] && e[0] != '0';
    }();
    static std::chrono::steady_clock::time_point last;
    if (!on) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (last.time_since_epoch().count() != 0 && now - last < std::chrono::seconds(60)) {
        return;
    }
    last = now;
    unsigned long long image = 0, mapped = 0, priv = 0, small_priv = 0, small_n = 0;
    std::string big;
    char* addr = nullptr;
    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT) {
            if (mbi.Type == MEM_IMAGE) {
                image += mbi.RegionSize;
            } else if (mbi.Type == MEM_MAPPED) {
                if (mbi.RegionSize >= (16u << 20)) {
                    char line[96];
                    snprintf(line, sizeof(line), " mapped@%p=%lluMiB", mbi.BaseAddress,
                             static_cast<unsigned long long>(mbi.RegionSize >> 20));
                    big += line;
                }
                mapped += mbi.RegionSize;
            } else {
                priv += mbi.RegionSize;
                if (mbi.RegionSize >= (16u << 20)) {
                    char line[96];
                    snprintf(line, sizeof(line), " priv@%p=%lluMiB", mbi.BaseAddress,
                             static_cast<unsigned long long>(mbi.RegionSize >> 20));
                    big += line;
                } else {
                    small_priv += mbi.RegionSize;
                    ++small_n;
                }
            }
        }
        addr = static_cast<char*>(mbi.BaseAddress) + mbi.RegionSize;
        if (!addr) {
            break;
        }
    }
    HANDLE heaps[256];
    const DWORD nheaps = GetProcessHeaps(256, heaps);
    unsigned long long heap_bytes = 0;
    for (DWORD i = 0; i < nheaps && i < 256; ++i) {
        PROCESS_HEAP_ENTRY e{};
        HeapLock(heaps[i]);
        while (HeapWalk(heaps[i], &e)) {
            if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
                heap_bytes += e.cbData;
            }
        }
        HeapUnlock(heaps[i]);
    }
    std::printf("Memory breakdown: committed image %llu MiB, mapped %llu MiB, private %llu MiB (%llu MiB in %llu regions under "
                "16 MiB); Windows heaps %lu, busy %llu MiB;%s\n",
                image >> 20, mapped >> 20, priv >> 20, small_priv >> 20, small_n, static_cast<unsigned long>(nheaps),
                heap_bytes >> 20, big.c_str());
    std::fflush(stdout);
}

void Tick() {
    MemoryBreakdown();
    static const bool enabled = [] {
        const char* e = std::getenv("BB_HANDLE_TRACE");
        return e && e[0] && e[0] != '0';
    }();
    if (!enabled) {
        return;
    }
    static std::chrono::steady_clock::time_point last;
    const auto now = std::chrono::steady_clock::now();
    if (!g_started.exchange(true)) {
        Start();
        last = now;
        return;
    }
    if (now - last < std::chrono::seconds(30)) {
        return;
    }
    last = now;
    PatchAll(); // modules loaded since
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    std::lock_guard<std::mutex> lk(g_mu);
    std::printf("Handle trace: %lu handles; creations since start by caller:\n", static_cast<unsigned long>(handles));
    std::map<std::string, unsigned long long> open;
    for (const auto& kv : g_live) {
        ++open[kv.second];
    }
    for (const auto& kv : g_cs) {
        ++open[kv.second];
    }
    for (const auto& kv : g_counts) {
        std::printf("Handle trace:   %8llu created, %6llu open  %s\n", kv.second, open[kv.first], kv.first.c_str());
    }
    std::fflush(stdout);
}
} // namespace BbHandleTrace
#else
namespace BbHandleTrace {
void Tick() {}
} // namespace BbHandleTrace
#endif
