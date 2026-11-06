// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/logging/log.h"
#include "../../../src/runtime_logdrain.h"
#include <chrono>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <execinfo.h>
#include <unistd.h>
#endif

// bb-probe (probe.c): set while the port restarts itself through run.sh. The device fd is closed
// before exec; Vulkan calls failing then are not errors: this thread waits for the exec instead.
extern "C" __attribute__((weak)) volatile int runtime_restarting; // absent in the tests
extern "C" __attribute__((weak)) void runtime_logdrain_flush(unsigned timeout_ms); // src/runtime_logdrain.c, absent in the tests

void assert_fail_impl() {
    if (&runtime_restarting && runtime_restarting) {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
#ifndef _WIN32
    // bbport: where it failed (Windows writes a crash dump instead): module+offset per frame, for
    // addr2line against the package's libraries.
    void* frames[32];
    const int count = backtrace(frames, 32);
    std::fputs("  call stack:\n", stderr);
    std::fflush(stderr);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
#endif
    // bbport: the log drain (src/runtime_logdrain.c) still holds the assertion's text: _Exit runs no
    // atexit handler, so hand it to the log first (bounded).
    runtime_logdrain_flush(3000);
#ifdef _WIN32
    // Not _Exit: unloading the Vulkan driver while GPU threads are inside it can hang the process
    // (visy 497198d5).
    TerminateProcess(GetCurrentProcess(), 23);
#endif
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
