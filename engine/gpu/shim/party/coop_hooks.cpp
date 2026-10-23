// SPDX-License-Identifier: GPL-3.0-or-later
// Byte-verified detours for the party co-op layer (see coop_hooks.h). The stub and branch
// writing follows gpu/shim/bbport_stability.cpp.
#include "coop_hooks.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

extern "C" void* runtime_low_map(size_t size, int prot); // src/runtime_memory.c

namespace coop {
namespace {

using u32 = std::uint32_t;
using ull = unsigned long long;

unsigned char* g_image = nullptr;
u64 g_image_size = 0;
std::mutex g_mu; // installs

constexpr std::size_t kStubPageSize = 16384;
u8* g_stub_page = nullptr;
std::size_t g_stub_used = 0;

u8* MapNear(std::size_t size) {
    const u64 base = reinterpret_cast<u64>(g_image);
#ifdef _WIN32
    // The image is the first block of the runtime's low guest area: the next low block lies right
    // after it, within rel32 reach (as bbport_stability.cpp / bbport_game_menu.cpp do).
    if (void* p = runtime_low_map(size, 3 /* read | write; VirtualProtect later */)) {
        const u64 at = reinterpret_cast<u64>(p);
        if (at >= base && at + size <= base + (2ull << 30) - (1ull << 20)) {
            return static_cast<u8*>(p);
        }
    }
#endif
    for (u64 k = 1; k <= 64; ++k) {
        for (const u64 hint : {base - k * (16ull << 20), base + g_image_size + k * (16ull << 20)}) {
#ifdef _WIN32
            void* p = VirtualAlloc(reinterpret_cast<void*>(hint & ~u64(0xffff)), size, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_READWRITE);
            if (p) {
                return static_cast<u8*>(p);
            }
#else
            void* p = mmap(reinterpret_cast<void*>(hint), size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED) {
                return static_cast<u8*>(p);
            }
#endif
        }
    }
    return nullptr;
}

// Never without execute: another thread may be running an earlier stub while one is added.
void StubWritable(bool writable) {
#ifdef _WIN32
    DWORD unused = 0;
    VirtualProtect(g_stub_page, kStubPageSize, writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ, &unused);
    if (!writable) {
        FlushInstructionCache(GetCurrentProcess(), g_stub_page, kStubPageSize);
    }
#else
    mprotect(g_stub_page, kStubPageSize, writable ? PROT_READ | PROT_WRITE | PROT_EXEC : PROT_READ | PROT_EXEC);
#endif
}

bool Rel32(u64 from_next, u64 to, std::int32_t& rel) {
    const std::int64_t d = std::int64_t(to) - std::int64_t(from_next);
    if (d < INT32_MIN || d > INT32_MAX) {
        return false;
    }
    rel = std::int32_t(d);
    return true;
}

struct Emitter {
    std::array<u8, 512> code{};
    std::size_t size = 0;
    u64 at = 0; ///< where the stub will be placed
    bool ok = true;
    void Bytes(std::initializer_list<u8> bytes) {
        for (const u8 b : bytes) {
            Byte(b);
        }
    }
    void Byte(u8 b) {
        if (size < code.size()) {
            code[size++] = b;
        } else {
            ok = false;
        }
    }
    void Raw(const u8* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            Byte(p[i]);
        }
    }
    void U32(u32 v) {
        u8 b[4];
        std::memcpy(b, &v, 4);
        Raw(b, 4);
    }
    void U64(u64 v) {
        u8 b[8];
        std::memcpy(b, &v, 8);
        Raw(b, 8);
    }
    void JmpTo(u64 target) { // jmp rel32
        Byte(0xe9);
        std::int32_t rel = 0;
        ok &= Rel32(at + size + 4, target, rel);
        U32(u32(rel));
    }
    void JmpAbs(const void* fn) { // jmp [rip]; dq fn
        Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00});
        U64(reinterpret_cast<u64>(fn));
    }
    void CallAbs(const void* fn) {
        Bytes({0x48, 0xb8}); // movabs rax, fn
        U64(reinterpret_cast<u64>(fn));
        Bytes({0xff, 0xd0}); // call rax
    }
    void SkipRedZone() { Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff}); } // lea rsp, [rsp - 128]
    void BackRedZone() { Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); } // lea rsp, [rsp + 128]
    /// Saves rax..r11 and xmm0-7 (the handler may change them; the arguments stay in place),
    /// aligns the stack to 16.
    void SaveVolatile() {
        Bytes({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
        Bytes({0x41, 0x54});                   // push r12 (holds the unaligned stack pointer)
        Bytes({0x49, 0x89, 0xe4});             // mov r12, rsp
        Bytes({0x48, 0x83, 0xe4, 0xf0});       // and rsp, -16
        Bytes({0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00}); // sub rsp, 0x80
        for (u8 i = 0; i < 8; ++i) {
            Bytes({0xf3, 0x0f, 0x7f, u8(0x44 | (i << 3)), 0x24, u8(i * 16)}); // movdqu [rsp + 16i], xmm_i
        }
    }
    void RestoreVolatile() {
        for (u8 i = 0; i < 8; ++i) {
            Bytes({0xf3, 0x0f, 0x6f, u8(0x44 | (i << 3)), 0x24, u8(i * 16)}); // movdqu xmm_i, [rsp + 16i]
        }
        Bytes({0x4c, 0x89, 0xe4}); // mov rsp, r12
        Bytes({0x41, 0x5c});       // pop r12
        Bytes({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});
    }
};

Emitter NewStub() {
    Emitter e;
    e.at = reinterpret_cast<u64>(g_stub_page + g_stub_used);
    e.ok = g_stub_page != nullptr;
    return e;
}

/// Copies `e` into the stub page; returns its address, or 0.
u64 PlaceStub(const Emitter& e) {
    if (!e.ok || !g_stub_page || g_stub_used + e.size > kStubPageSize ||
        e.at != reinterpret_cast<u64>(g_stub_page + g_stub_used)) {
        return 0;
    }
    StubWritable(true);
    u8* at = g_stub_page + g_stub_used;
    std::memcpy(at, e.code.data(), e.size);
    g_stub_used += (e.size + 15) & ~std::size_t(15);
    StubWritable(false);
    return reinterpret_cast<u64>(at);
}

/// Writes `jmp/call stub` (opcode) at `off`, `pad` NOPs after it.
bool WriteBranch(u64 off, u8 opcode, u64 stub, std::size_t pad) {
    std::int32_t rel = 0;
    const u64 address = reinterpret_cast<u64>(g_image + off);
    if (!stub || !Rel32(address + 5, stub, rel)) {
        return false;
    }
    u8 code[32];
    code[0] = opcode;
    std::memcpy(code + 1, &rel, 4);
    for (std::size_t i = 0; i < pad && 5 + i < sizeof code; ++i) {
        code[5 + i] = 0x90;
    }
    const std::size_t n = 5 + (pad < sizeof code - 5 ? pad : sizeof code - 5);
#ifdef _WIN32
    DWORD old = 0;
    if (!VirtualProtect(g_image + off, n, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(g_image + off, code, n);
    DWORD unused = 0;
    VirtualProtect(g_image + off, n, old, &unused);
    FlushInstructionCache(GetCurrentProcess(), g_image + off, n);
#else
    std::memcpy(g_image + off, code, n);
#endif
    return true;
}

bool Ready(const char* name) {
    if (!g_image) {
        std::printf("[coop] Coop hooks: %s: no image; not installed\n", name);
        return false;
    }
    if (!g_stub_page) {
        g_stub_page = MapNear(kStubPageSize);
        if (!g_stub_page) {
            std::printf("[coop] Coop hooks: %s: no memory near the image for stubs; not installed\n", name);
            return false;
        }
        StubWritable(false);
    }
    return true;
}

bool PrologueOk(u64 off, std::initializer_list<u8> prologue, const char* name) {
    if (prologue.size() < 5 || prologue.size() > 32) {
        std::printf("[coop] Coop hooks: %s: a %zu-byte prologue cannot be relocated; not installed\n", name,
                    prologue.size());
        return false;
    }
    if (!Matches(off, prologue)) {
        std::printf("[coop] Coop hooks: %s: guest +0x%llx is not the expected code (another game version or a patch); "
                    "not installed\n",
                    name, ull(off));
        return false;
    }
    return true;
}

} // namespace

namespace {
std::mutex g_done_mu;
std::condition_variable g_done_cv;
bool g_done = false;
std::thread::id g_patch_thread;
bool g_patch_thread_set = false;
} // namespace

bool HooksInit(unsigned char* image, u64 size) {
    {
        std::lock_guard<std::mutex> dl(g_done_mu);
        if (!g_patch_thread_set) {
            g_patch_thread = std::this_thread::get_id();  // the image patching thread (WaitImageHooksDone)
            g_patch_thread_set = true;
        }
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (!image || !size) {
        return false;
    }
    g_image = image;
    g_image_size = size;
    return true;
}

unsigned char* Image() {
    return g_image;
}
u64 ImageSize() {
    return g_image_size;
}

u64 Guest(u64 off) {
    return g_image && off < (1ull << 32) ? reinterpret_cast<u64>(g_image) + off : 0;
}

bool Matches(u64 off, const u8* bytes, std::size_t n) {
    if (!g_image || off + n > g_image_size) {
        return false;
    }
    return std::memcmp(g_image + off, bytes, n) == 0;
}

namespace {
std::map<u64, std::vector<u8>> g_replaced; // under g_mu: prologue bytes our jumps replaced
void NoteReplaced(u64 off, std::initializer_list<u8> prologue) {
    g_replaced[off] = std::vector<u8>(prologue.begin(), prologue.end());
}
std::vector<std::string> g_installed; // under g_mu: "<name> @+<off>" of every install (A9)
void NoteInstalled(u64 off, const char* name) {
    char at[32];
    std::snprintf(at, sizeof at, " @+0x%llx", ull(off));
    g_installed.push_back(std::string(name ? name : "?") + at);
}
} // namespace

std::vector<std::string> InstalledHooks() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_installed;
}

void MarkImageHooksDone() {
    {
        std::lock_guard<std::mutex> lk(g_done_mu);
        g_done = true;
    }
    g_done_cv.notify_all();
}

bool WaitImageHooksDone(int ms) {
    std::unique_lock<std::mutex> lk(g_done_mu);
    if (g_done) return true;
    if (g_patch_thread_set && g_patch_thread == std::this_thread::get_id()) return false;
    return g_done_cv.wait_for(lk, std::chrono::milliseconds(ms), [] { return g_done; });
}

bool MatchesOriginal(u64 off, const u8* bytes, std::size_t n) {
    if (!g_image || off + n > g_image_size) {
        return false;
    }
    std::vector<u8> view(g_image + off, g_image + off + n);
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& [at, orig] : g_replaced) {
        for (std::size_t i = 0; i < orig.size(); ++i) {
            if (at + i >= off && at + i < off + n) {
                view[at + i - off] = orig[i];
            }
        }
    }
    return std::memcmp(view.data(), bytes, n) == 0;
}

bool CallsTo(u64 off, u64 target) {
    if (!g_image || off + 5 > g_image_size || g_image[off] != 0xe8) {
        return false;
    }
    std::int32_t rel;
    std::memcpy(&rel, g_image + off + 1, 4);
    return off + 5 + std::int64_t(rel) == target;
}

bool HookPrologue(u64 off, std::initializer_list<u8> prologue, PreHandler handler, const char* name) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!Ready(name) || !PrologueOk(off, prologue, name)) {
        return false;
    }
    Emitter e = NewStub();
    e.SkipRedZone();
    e.SaveVolatile();
    e.CallAbs(reinterpret_cast<const void*>(handler)); // rdi..r9 still hold the arguments
    e.RestoreVolatile();
    e.BackRedZone();
    e.Raw(prologue.begin(), prologue.size());
    e.JmpTo(reinterpret_cast<u64>(g_image) + off + prologue.size());
    const u64 stub = PlaceStub(e);
    if (!stub || !WriteBranch(off, 0xe9, stub, prologue.size() - 5)) {
        std::printf("[coop] Coop hooks: %s: the stub at guest +0x%llx could not be placed; not installed\n", name, ull(off));
        return false;
    }
    NoteReplaced(off, prologue);
    NoteInstalled(off, name);
    std::printf("[coop] Coop hooks: %s: entry hook at guest +0x%llx\n", name, ull(off));
    return true;
}

bool ReplacePrologue(u64 off, std::initializer_list<u8> prologue, const void* handler, void** original,
                     const char* name) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!Ready(name) || !PrologueOk(off, prologue, name)) {
        return false;
    }
    Emitter t = NewStub();
    t.Raw(prologue.begin(), prologue.size());
    t.JmpTo(reinterpret_cast<u64>(g_image) + off + prologue.size());
    const u64 trampoline = PlaceStub(t);
    Emitter j = NewStub();
    j.JmpAbs(handler);
    const u64 jump = PlaceStub(j);
    if (!trampoline || !jump) {
        std::printf("[coop] Coop hooks: %s: stub memory is full; not installed\n", name);
        return false;
    }
    if (original) {
        *original = reinterpret_cast<void*>(trampoline);
    }
    if (!WriteBranch(off, 0xe9, jump, prologue.size() - 5)) {
        std::printf("[coop] Coop hooks: %s: guest +0x%llx out of reach; not installed\n", name, ull(off));
        return false;
    }
    NoteReplaced(off, prologue);
    NoteInstalled(off, name);
    std::printf("[coop] Coop hooks: %s: replaced guest +0x%llx\n", name, ull(off));
    return true;
}

bool HookCallSite(u64 off, u64 target, const void* handler, const char* name) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!Ready(name)) {
        return false;
    }
    if (!CallsTo(off, target)) {
        std::printf("[coop] Coop hooks: %s: guest +0x%llx is not a call to +0x%llx; not installed\n", name, ull(off),
                    ull(target));
        return false;
    }
    Emitter e = NewStub();
    e.JmpAbs(handler);
    if (!WriteBranch(off, 0xe8, PlaceStub(e), 0)) {
        std::printf("[coop] Coop hooks: %s: the stub for guest +0x%llx could not be placed; not installed\n", name, ull(off));
        return false;
    }
    NoteInstalled(off, name);
    std::printf("[coop] Coop hooks: %s: call site guest +0x%llx -> handler\n", name, ull(off));
    return true;
}

bool HookTailJump(u64 off, u64 target, const void* handler, const char* name) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!Ready(name)) {
        return false;
    }
    bool jumps = g_image && off + 5 <= g_image_size && g_image[off] == 0xe9;
    if (jumps) {
        std::int32_t rel;
        std::memcpy(&rel, g_image + off + 1, 4);
        jumps = off + 5 + std::int64_t(rel) == target;
    }
    if (!jumps) {
        std::printf("[coop] Coop hooks: %s: guest +0x%llx is not a jmp to +0x%llx; not installed\n", name, ull(off),
                    ull(target));
        return false;
    }
    Emitter e = NewStub();
    e.JmpAbs(handler);
    if (!WriteBranch(off, 0xe9, PlaceStub(e), 0)) {
        std::printf("[coop] Coop hooks: %s: the stub for guest +0x%llx could not be placed; not installed\n", name, ull(off));
        return false;
    }
    NoteInstalled(off, name);
    std::printf("[coop] Coop hooks: %s: tail jump guest +0x%llx -> handler\n", name, ull(off));
    return true;
}

bool HookValueFilter(u64 off, std::initializer_list<u8> original, std::initializer_list<u8> load_args, ValueFilter fn,
                     u64 resume, const char* name) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!Ready(name) || !PrologueOk(off, original, name)) {
        return false;
    }
    if (!fn || resume >= g_image_size) {
        std::printf("[coop] Coop hooks: %s: no filter or a bad resume offset; not installed\n", name);
        return false;
    }
    Emitter e = NewStub();
    e.Raw(original.begin(), original.size());
    e.SkipRedZone();
    e.SaveVolatile();                 // rax is the first push: [r12 + 0x48] after it
    e.Raw(load_args.begin(), load_args.size());
    e.CallAbs(reinterpret_cast<const void*>(fn));
    e.Bytes({0x49, 0x89, 0x44, 0x24, 0x48}); // mov [r12 + 0x48], rax (the saved rax = the new value)
    e.RestoreVolatile();
    e.BackRedZone();
    e.JmpTo(reinterpret_cast<u64>(g_image + resume));
    if (!WriteBranch(off, 0xe9, PlaceStub(e), original.size() - 5)) {
        std::printf("[coop] Coop hooks: %s: the stub for guest +0x%llx could not be placed; not installed\n", name, ull(off));
        return false;
    }
    NoteInstalled(off, name);
    std::printf("[coop] Coop hooks: %s: value filter at guest +0x%llx (resume +0x%llx)\n", name, ull(off), ull(resume));
    return true;
}

bool PatchBytes(u64 off, std::initializer_list<u8> original, std::initializer_list<u8> patched, const char* name) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_image || original.size() != patched.size() || !patched.size()) {
        std::printf("[coop] Coop hooks: %s: no image or bad patch; not written\n", name);
        return false;
    }
    const std::size_t n = patched.size();
    if (Matches(off, patched.begin(), n)) {
        NoteInstalled(off, name);
        std::printf("[coop] Coop hooks: %s: guest +0x%llx already patched\n", name, ull(off));
        return true;
    }
    if (!Matches(off, original.begin(), n)) {
        std::printf("[coop] Coop hooks: %s: guest +0x%llx is not the expected code (another game version or a patch); "
                    "not written\n",
                    name, ull(off));
        return false;
    }
#ifdef _WIN32
    DWORD old = 0;
    if (!VirtualProtect(g_image + off, n, PAGE_EXECUTE_READWRITE, &old)) {
        std::printf("[coop] Coop hooks: %s: guest +0x%llx not writable; not written\n", name, ull(off));
        return false;
    }
    std::memcpy(g_image + off, patched.begin(), n);
    DWORD unused = 0;
    VirtualProtect(g_image + off, n, old, &unused);
    FlushInstructionCache(GetCurrentProcess(), g_image + off, n);
#else
    std::memcpy(g_image + off, patched.begin(), n);
#endif
    NoteInstalled(off, name);
    std::printf("[coop] Coop hooks: %s: patched %zu bytes at guest +0x%llx\n", name, n, ull(off));
    return true;
}

} // namespace coop
