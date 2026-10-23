// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdio>
#include <exception>
#include <thread>
#include <typeinfo>
#include "bbport_toggles.h"
#ifndef _WIN32
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace BbThreads {

#ifdef _WIN32
unsigned Available();
void MakeBackground();
void MakeCritical(bool bulk = false);
unsigned CapSpinUs(unsigned wanted);
#else
inline void MakeCritical(bool = false) {}
inline unsigned CapSpinUs(unsigned wanted) { return wanted; }
inline unsigned Available() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

inline void MakeBackground() {
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
}
#endif

/// Runs a thread's body; an exception that leaves it is logged (its type and message) and goes on
/// as before, so the process still stops with the same crash report and dump. Without this the log
/// had only "Host fault 0x20474343" (an uncaught C++ exception), not which one (std::bad_alloc
/// once the PC's commit ran out, 8 GB laptops of 2026-10-22).
template <typename F>
void LogEscaping(const char* thread, F&& body) {
    try {
        body();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: uncaught C++ exception %s: %s\n", thread, typeid(e).name(), e.what());
        std::fflush(stderr);
        throw;
    } catch (...) {
        std::fprintf(stderr, "%s: uncaught C++ exception (not a std::exception)\n", thread);
        std::fflush(stderr);
        throw;
    }
}

} // namespace BbThreads
