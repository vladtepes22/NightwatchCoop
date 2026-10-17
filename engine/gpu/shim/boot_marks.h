/* Boot timeline (docs/STARTUP.md): "Boot: <phase> at <s> s" lines, seconds since the process was
 * created (the same clock in every translation unit, no shared state). Header-only so the GPU
 * library, the runtime and the tests link without anything extra. BB_BOOT_TIMES=0 silences them. */
#ifndef BB_BOOT_MARKS_H
#define BB_BOOT_MARKS_H

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Seconds since this process was created (Windows), else since the first call. */
static inline double bb_boot_seconds(void) {
#ifdef _WIN32
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return 0.0;
    }
    GetSystemTimePreciseAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = created.dwLowDateTime;
    a.HighPart = created.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    return (double)(b.QuadPart - a.QuadPart) / 1e7;
#else
    static struct timespec t0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!t0.tv_sec && !t0.tv_nsec) {
        t0 = t;
    }
    return (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9;
#endif
}

static inline void bb_boot_mark(const char* what) {
    const char* e = getenv("BB_BOOT_TIMES");
    if (e && e[0] == '0') {
        return;
    }
    printf("Boot: %s at %.3f s\n", what, bb_boot_seconds());
    fflush(stdout);
}

#ifdef __cplusplus
}
#endif

#endif
