// SPDX-License-Identifier: GPL-3.0-or-later
// liftre077 RE aid (BB_TEST_LIFT_WATCH=1): hardware data breakpoints (debug registers, set on every
// thread of the process) on the local chr's carry state and the lift car's hit object, to find which
// game code writes them and in which order inside a frame. Each hit is queued with the instruction
// after the write, the value written, the thread and the return addresses found on the stack; the
// lift probe (party_worldact.cpp LiftProbeTick) drains the queue into the log. Off by default; test aid.
#include "party_lift_watch.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace coop::liftwatch {
namespace {
constexpr int kRing = 4096;
Hit g_ring[kRing];
std::atomic<std::uint32_t> g_head{0};
std::uint32_t g_tail = 0;
std::uint64_t g_addr[4] = {};
int g_len[4] = {};
std::atomic<bool> g_veh_added{false};
std::atomic<std::uint64_t> g_image{0};
std::atomic<std::uint64_t> g_step{0};
std::atomic<bool> g_on{false};
std::atomic<std::uint32_t> g_dropped{0};

bool Readable(std::uint64_t a, std::size_t n) {
    MEMORY_BASIC_INFORMATION mi{};
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof mi)) return false;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return a + n <= reinterpret_cast<std::uint64_t>(mi.BaseAddress) + mi.RegionSize;
}

LONG CALLBACK Veh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!(c->Dr6 & 0xf)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_on.load(std::memory_order_relaxed)) {
        const std::uint32_t slot = g_head.fetch_add(1) % kRing;
        Hit& h = g_ring[slot];
        std::memset(&h, 0, sizeof h);
        const std::uint64_t base = g_image.load(std::memory_order_relaxed);
        h.which = static_cast<std::uint8_t>(c->Dr6 & 0xf);
        h.rip = c->Rip - base;
        h.tid = GetCurrentThreadId();
        h.step = g_step.load(std::memory_order_relaxed);
        for (int i = 0; i < 4; ++i) {
            if (c->Dr6 & (1u << i) && g_addr[i] && Readable(g_addr[i], static_cast<std::size_t>(g_len[i]))) {
                std::memcpy(&h.value, reinterpret_cast<void*>(g_addr[i]), static_cast<std::size_t>(g_len[i]));
                h.which = static_cast<std::uint8_t>(i);
                break;
            }
        }
        // return addresses: the frame chain of rbp (the game's functions keep frame pointers), and the first
        // stack words that point into the image's code
        std::uint64_t bp = c->Rbp;
        int n = 0;
        for (int d = 0; d < 6 && bp && !(bp & 7) && Readable(bp, 16); ++d) {
            std::uint64_t next = 0, ret = 0;
            std::memcpy(&next, reinterpret_cast<void*>(bp), 8);
            std::memcpy(&ret, reinterpret_cast<void*>(bp + 8), 8);
            if (ret < base || ret >= base + 0x6000000ull) break;
            if (n < 6) h.ret[n++] = ret - base;
            if (next <= bp) break;
            bp = next;
        }
        h.rsp0 = 0;
        if (Readable(c->Rsp, 8)) {
            std::uint64_t r = 0;
            std::memcpy(&r, reinterpret_cast<void*>(c->Rsp), 8);
            if (r >= base && r < base + 0x6000000ull) h.rsp0 = r - base;
        }
    } else {
        g_dropped.fetch_add(1);
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD DrBits(int len) { return len == 1 ? 0 : len == 2 ? 1 : len == 8 ? 2 : 3; }

void ApplyToThread(HANDLE th) {
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, &c)) return;
    DWORD64 dr7 = 0;
    DWORD64* regs[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
    for (int i = 0; i < 4; ++i) {
        *regs[i] = g_addr[i];
        if (g_addr[i]) {
            // L(i), RW = 01 (write), LEN
            dr7 |= (1ull << (2 * i)) | (1ull << (16 + 4 * i)) | (static_cast<DWORD64>(DrBits(g_len[i])) << (18 + 4 * i));
        }
    }
    c.Dr7 = dr7;
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    SetThreadContext(th, &c);
}

void ArmAll() {
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != static_cast<DWORD>(-1)) {
            ApplyToThread(th);
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
}
}  // namespace

void Arm(std::uint64_t image, const std::uint64_t addr[4], const int len[4]) {
    g_image = image;
    bool same = true;
    for (int i = 0; i < 4; ++i) {
        if (g_addr[i] != addr[i] || g_len[i] != len[i]) same = false;
    }
    if (same && g_on) return;
    for (int i = 0; i < 4; ++i) {
        g_addr[i] = addr[i];
        g_len[i] = len[i];
    }
    if (!g_veh_added.exchange(true)) AddVectoredExceptionHandler(1, Veh);
    g_on = true;
    // the calling thread is armed from a helper (it must not suspend itself); the helper also arms the others
    std::atomic<bool> done{false};
    std::thread([&done] {
        ArmAll();
        // the thread that called Arm: found by id below
        done = true;
    }).detach();
    // arm this thread: ask a helper to suspend it while it spins
    const DWORD self = GetCurrentThreadId();
    std::atomic<bool> self_done{false};
    std::thread([self, &self_done] {
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, self);
        if (th) {
            if (SuspendThread(th) != static_cast<DWORD>(-1)) {
                ApplyToThread(th);
                ResumeThread(th);
            }
            CloseHandle(th);
        }
        self_done = true;
    }).detach();
    const auto t0 = std::chrono::steady_clock::now();
    while ((!self_done || !done) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) {
        std::this_thread::yield();
    }
}

void SetStep(std::uint64_t step) { g_step = step; }

bool Pop(Hit* out) {
    const std::uint32_t head = g_head.load();
    if (g_tail == head) return false;
    if (head - g_tail > kRing) g_tail = head - kRing;
    *out = g_ring[g_tail % kRing];
    ++g_tail;
    return true;
}

std::uint32_t Dropped() { return g_dropped.load(); }
}  // namespace coop::liftwatch
#else
namespace coop::liftwatch {
void Arm(std::uint64_t, const std::uint64_t[4], const int[4]) {}
void SetStep(std::uint64_t) {}
bool Pop(Hit*) { return false; }
std::uint32_t Dropped() { return 0; }
}  // namespace coop::liftwatch
#endif
