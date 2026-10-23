#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32_memory.h"

typedef PVOID (WINAPI *VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef PVOID (WINAPI *MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef BOOL (WINAPI *UnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);
static VirtualAlloc2Fn virtual_alloc2;
static MapViewOfFile3Fn map_view3;
static UnmapViewOfFile2Fn unmap_view2;
static HANDLE section;
static uintptr_t space_start, space_end;
static int lazy;
static unsigned char *pool_view;
static uint64_t pool_bytes;
typedef struct { uintptr_t start, end; uint64_t phys; } View;
static View *views;
static size_t view_count, view_capacity;

static void report(const char *what, uintptr_t a, uintptr_t b) {
    fprintf(stderr, "Runtime: %s [%#llx, %#llx) failed: Windows error %lu\n", what,
            (unsigned long long)a, (unsigned long long)b, GetLastError());
}
/* Windows refused memory for want of commit (RAM plus page file), not for a taken range. */
static int commit_error(DWORD e) {
    return e == ERROR_COMMITMENT_LIMIT || e == ERROR_PAGEFILE_QUOTA || e == ERROR_NOT_ENOUGH_MEMORY ||
           e == ERROR_OUTOFMEMORY || e == ERROR_COMMITMENT_MINIMUM || e == ERROR_NO_SYSTEM_RESOURCES;
}
static unsigned long long env_number(const char *name, unsigned long long fallback) {
    const char *v = getenv(name);
    return v && *v ? strtoull(v, NULL, 10) : fallback;
}
static unsigned long long commit_free_mib(void) {
    MEMORYSTATUSEX st = {.dwLength = sizeof(st)};
    return GlobalMemoryStatusEx(&st) ? st.ullAvailPageFile >> 20 : 0;
}
int win_mem_space(uintptr_t start, uintptr_t end) {
    if (space_end) return 0;
    HMODULE kernel = GetModuleHandleW(L"kernelbase.dll");
    virtual_alloc2 = kernel ? (VirtualAlloc2Fn)(void *)GetProcAddress(kernel, "VirtualAlloc2") : NULL;
    map_view3 = kernel ? (MapViewOfFile3Fn)(void *)GetProcAddress(kernel, "MapViewOfFile3") : NULL;
    unmap_view2 = kernel ? (UnmapViewOfFile2Fn)(void *)GetProcAddress(kernel, "UnmapViewOfFile2") : NULL;
    if (!virtual_alloc2 || !map_view3 || !unmap_view2) {
        fputs("STOP: Windows 10 1803 or newer is required (placeholder memory API)\n", stderr);
        exit(21);
    }
    if (virtual_alloc2(GetCurrentProcess(), (void *)start, end - start, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                       PAGE_NOACCESS, NULL, 0) != (void *)start) {
        report("reserving the guest address space", start, end);
        return -1;
    }
    space_start = start; space_end = end;
    return 0;
}
int win_mem_section(uint64_t size, void **backing) {
    const char *lazy_env = getenv("BB_POOL_LAZY");
    lazy = lazy_env && lazy_env[0] == '1';
    pool_bytes = size;
    const char *given = getenv("BB_POOL_SECTION");
    if (given && *given) {
        char *end = NULL;
        const unsigned long long handle = strtoull(given, &end, 10);
        const unsigned long long bytes = end && *end == ':' ? strtoull(end + 1, NULL, 10) : 0;
        if (handle && bytes == size) {
            section = (HANDLE)(uintptr_t)handle;
            *backing = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, size);
            pool_view = *backing;
            if (*backing) return 0;
            fprintf(stderr, "Runtime: the pool bb-start made cannot be mapped (Windows error %lu); making one\n",
                    GetLastError());
        } else {
            fprintf(stderr, "Runtime: BB_POOL_SECTION %s is not a %llu byte pool; making one\n", given,
                    (unsigned long long)size);
        }
        section = NULL;
    }
    section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
                                 PAGE_EXECUTE_READWRITE | (lazy ? SEC_RESERVE : SEC_COMMIT),
                                 (DWORD)(size >> 32), (DWORD)size, NULL);
    if (!section) { report("creating the memory pool section", 0, size); return -1; }
    *backing = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!*backing) { report("mapping the memory pool", 0, size); return -1; }
    pool_view = *backing;
    if (lazy) {
        fprintf(stderr, "Runtime: the game's memory (%llu MiB) is committed as it is mapped "
                        "(BB_POOL_LAZY=1)\n", (unsigned long long)(size >> 20));
    }
    return 0;
}
int win_mem_commit(uint64_t phys, uint64_t size) {
    if (!lazy || !pool_view || !size || phys >= pool_bytes) return 0;
    if (size > pool_bytes - phys) size = pool_bytes - phys;
    if (VirtualAlloc(pool_view + phys, size, MEM_COMMIT, PAGE_READWRITE)) return 0;
    fprintf(stderr, "Runtime: Windows has no memory left to commit for the game's %llu MiB at %#llx "
                    "(error %lu): close other programs or let Windows grow the page file\n",
            (unsigned long long)(size >> 20), (unsigned long long)phys, GetLastError());
    return -1;
}
/* bbport (visy's win_mem_punch, TEXTURE_GC_BACKPORT.md Part 2): released memory is dropped, not
 * written. A memset faulted in every page of a range that was never touched (RAM went resident on
 * each release, e.g. during the Continue load). visy uses VirtualAlloc(MEM_RESET); measured on a
 * pagefile section view that is a no-op (pages stay resident and keep their data), so this uses
 * DiscardVirtualMemory: it frees the pages and they read as zero again through every view of the
 * section, which keeps the "zero on reuse" the guest gets on Linux (hole punch). memset remains the
 * fallback if Windows refuses. */
static void clear(uint64_t at, uint64_t size) {
    if (DiscardVirtualMemory(pool_view + at, size) != ERROR_SUCCESS) memset(pool_view + at, 0, size);
}
void win_mem_zero(uint64_t phys, uint64_t size) {
    if (!lazy) { clear(phys, size); return; }
    MEMORY_BASIC_INFORMATION info;
    for (uint64_t at = phys, end = phys + size; at < end;) {
        if (!VirtualQuery(pool_view + at, &info, sizeof(info))) return;
        uint64_t next = (uint64_t)((unsigned char *)info.BaseAddress + info.RegionSize - pool_view);
        if (next > end) next = end;
        if (info.State == MEM_COMMIT) clear(at, next - at);
        at = next;
    }
}
static DWORD page_mode(int prot) {
    switch (prot & 7) {
    case 0: return PAGE_NOACCESS;
    case 1: return PAGE_READONLY;
    case 2: case 3: return PAGE_READWRITE;
    case 4: return PAGE_EXECUTE;
    case 5: return PAGE_EXECUTE_READ;
    default: return PAGE_EXECUTE_READWRITE;
    }
}
static size_t view_index(uintptr_t a) {
    size_t lo = 0, hi = view_count;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (views[mid].end <= a) lo = mid + 1; else hi = mid; }
    return lo;
}
static int view_insert(View v) {
    if (view_count == view_capacity) {
        size_t capacity = view_capacity ? view_capacity * 2 : 256;
        View *next = realloc(views, capacity * sizeof(*views));
        if (!next) return -1;
        views = next; view_capacity = capacity;
    }
    size_t at = view_index(v.start);
    memmove(views + at + 1, views + at, (view_count - at) * sizeof(*views));
    views[at] = v; ++view_count;
    return 0;
}
static void view_erase(size_t at) {
    memmove(views + at, views + at + 1, (view_count - at - 1) * sizeof(*views));
    --view_count;
}
static int split_at(uintptr_t x) {
    if (x == space_start || x == space_end) return 0;
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((void *)x, &info, sizeof(info))) return -1;
    if ((uintptr_t)info.AllocationBase == x) return 0;
    if (info.State != MEM_RESERVE) { SetLastError(ERROR_INVALID_ADDRESS); return -1; }
    return VirtualFree((void *)x, info.RegionSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ? 0 : -1;
}
static int make_placeholder(uintptr_t a, uintptr_t b) {
    if (split_at(a) || split_at(b)) return -1;
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((void *)a, &info, sizeof(info))) return -1;
    if (info.RegionSize < b - a && !VirtualFree((void *)a, b - a, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) return -1;
    return 0;
}
static int map_view(uintptr_t a, uintptr_t b, uint64_t phys) {
    if (make_placeholder(a, b)) return -1;
    if (map_view3(section, GetCurrentProcess(), (void *)a, phys, b - a, MEM_REPLACE_PLACEHOLDER,
                  PAGE_READWRITE, NULL, 0) != (void *)a) return -1;
    return view_insert((View){a, b, phys});
}
static int inside(uintptr_t a, uintptr_t b) { return a < b && a >= space_start && b <= space_end; }
int win_mem_release(uintptr_t a, uintptr_t b, WinMemRestore restore) {
    if (!inside(a, b)) { SetLastError(ERROR_INVALID_ADDRESS); report("releasing", a, b); return -1; }
    for (size_t i = view_index(a); i < view_count && views[i].start < b;) {
        View v = views[i];
        if (!unmap_view2(GetCurrentProcess(), (void *)v.start, MEM_PRESERVE_PLACEHOLDER)) { report("unmapping", v.start, v.end); return -1; }
        view_erase(i);
        if (v.start < a) {
            if (map_view(v.start, a, v.phys)) { report("remapping", v.start, a); return -1; }
            if (restore) restore(v.start, a);
            ++i;
        }
        if (v.end > b) {
            if (map_view(b, v.end, v.phys + (b - v.start))) { report("remapping", b, v.end); return -1; }
            if (restore) restore(b, v.end);
        }
    }
    if (make_placeholder(a, b)) { report("reserving", a, b); return -1; }
    return 0;
}
int win_mem_map(uintptr_t a, uintptr_t b, uint64_t phys, int prot, WinMemRestore restore) {
    if (win_mem_commit(phys, b - a)) { SetLastError(ERROR_COMMITMENT_LIMIT); return -1; }
    if (win_mem_release(a, b, restore)) return -1;
    if (map_view(a, b, phys)) { report("mapping", a, b); return -1; }
    return (prot & 7) == 3 ? 0 : win_mem_protect(a, b, prot);
}
int win_mem_protect(uintptr_t a, uintptr_t b, int prot) {
    DWORD mode = page_mode(prot), old;
    for (size_t i = view_index(a); i < view_count && views[i].start < b; ++i) {
        uintptr_t start = views[i].start > a ? views[i].start : a, end = views[i].end < b ? views[i].end : b;
        if (!VirtualProtect((void *)start, end - start, mode, &old)) { report("protecting", start, end); return -1; }
    }
    return 0;
}
void *win_mem_private(uintptr_t a, uintptr_t b) {
    if (!inside(a, b)) return NULL;
    MEMORY_BASIC_INFORMATION info;
    for (uintptr_t at = a; at < b; at = (uintptr_t)info.BaseAddress + info.RegionSize)
        if (!VirtualQuery((void *)at, &info, sizeof(info)) || info.State != MEM_RESERVE) return NULL;
    if (make_placeholder(a, b)) return NULL;
    /* bbport (visy e4dbb97d): NULL means "this range is taken" to the caller (runtime_low_map),
     * which then tries the next range. A commit-limit refusal is not that: before, it walked the
     * whole low range (every try refused the same way) and every later low allocation failed - a
     * guest mutex's init then returned ENOMEM, which the game ignores, and its first lock was a
     * Dantelion "Mutex is not initialized" panic. It is retried for BB_POOL_COMMIT_WAIT seconds,
     * then the process exits (code 76) with the reason. BB_TEST_LOW_COMMIT_FAILURES=N simulates N
     * refusals (tests). */
    static unsigned long long test_failures = ~0ull;
    if (test_failures == ~0ull) test_failures = env_number("BB_TEST_LOW_COMMIT_FAILURES", 0);
    const unsigned long long wait_s = env_number("BB_POOL_COMMIT_WAIT", 120);
    const ULONGLONG t0 = GetTickCount64();
    for (unsigned attempt = 0;; ++attempt) {
        void *p = NULL;
        if (test_failures) { --test_failures; SetLastError(ERROR_COMMITMENT_LIMIT); }
        else p = virtual_alloc2(GetCurrentProcess(), (void *)a, b - a, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                                PAGE_READWRITE, NULL, 0);
        if (p) {
            if (attempt) fprintf(stderr, "Runtime: host memory for the game committed after %u retries (%llu s)\n",
                                 attempt, (unsigned long long)((GetTickCount64() - t0) / 1000));
            return p;
        }
        const DWORD error = GetLastError();
        if (!commit_error(error)) return NULL; /* the range is taken: the caller tries the next one */
        const unsigned long long waited = (GetTickCount64() - t0) / 1000;
        if (waited >= wait_s) {
            report("committing host memory for the game", a, b);
            fprintf(stderr, "Runtime: FATAL: Windows cannot commit %llu KiB of memory for the game (%llu MiB of "
                    "commit left; error %lu). The page file is too small: let Windows manage its size "
                    "(System > About > Advanced system settings > Performance > Advanced > Virtual "
                    "memory) or enlarge it, or close other programs. Exiting.\n",
                    (unsigned long long)((b - a) >> 10), commit_free_mib(), error);
            fflush(stdout);
            fflush(stderr);
            ExitProcess(76);
        }
        if (!attempt || attempt % 40 == 0)
            fprintf(stderr, "Runtime: committing %llu KiB of host memory for the game failed (Windows error %lu, "
                    "%llu MiB of commit left); retrying for up to %llu s\n", (unsigned long long)((b - a) >> 10),
                    error, commit_free_mib(), wait_s);
        Sleep(attempt < 8 ? 25 : 250);
    }
}
#endif
