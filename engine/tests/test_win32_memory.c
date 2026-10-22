// SPDX-License-Identifier: GPL-2.0-or-later
// win_mem_private (src/win32_memory.c) with Windows refusing the commit, as on a PC whose page
// file is full (BB_TEST_LOW_COMMIT_FAILURES): a refusal is retried and the memory given, and one
// that lasts BB_POOL_COMMIT_WAIT seconds exits 76 with the reason. Before, a refusal read as "range
// taken" and every later low allocation failed (the "Mutex is not initialized" restart crash).
// Each case runs in a child process: the refusal count is read once per process.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32_memory.h"

#define START UINT64_C(0x0800000000)
#define SIZE UINT64_C(0x10000000)

static int child(void) {
    if (win_mem_space(START, START + SIZE)) return 2;
    unsigned char *p = win_mem_private(START, START + 0x10000);
    if (p != (unsigned char *)START) return 3;
    p[0] = 1;
    p[0xffff] = 2;
    /* A later range is not refused for good. */
    if (win_mem_private(START + 0x20000, START + 0x30000) != (void *)(START + 0x20000)) return 4;
    return 0;
}

static DWORD run(const char *self, const char *failures, const char *wait) {
    SetEnvironmentVariableA("BB_TEST_LOW_COMMIT_FAILURES", failures);
    SetEnvironmentVariableA("BB_POOL_COMMIT_WAIT", wait);
    char line[MAX_PATH + 16];
    snprintf(line, sizeof line, "\"%s\" --child", self);
    STARTUPINFOA startup = {.cb = sizeof startup};
    PROCESS_INFORMATION process;
    if (!CreateProcessA(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) return 1000;
    WaitForSingleObject(process.hProcess, 60000);
    DWORD code = 1001;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--child")) return child();
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof self);
    DWORD code;
    if ((code = run(self, "0", "120")) != 0) { fprintf(stderr, "no refusal: exit %lu\n", code); return 1; }
    if ((code = run(self, "3", "120")) != 0) { fprintf(stderr, "3 refusals: exit %lu, want 0\n", code); return 1; }
    if ((code = run(self, "1000000", "1")) != 76) { fprintf(stderr, "lasting refusal: exit %lu, want 76\n", code); return 1; }
    puts("PASS: refused commits retried; a lasting refusal exits 76");
    return 0;
}
