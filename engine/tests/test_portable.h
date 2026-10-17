/* SPDX-License-Identifier: GPL-2.0-or-later
 * Portable helpers for the C/C++ test programs: environment variables and temporary files work
 * the same on Linux (setenv, $TMPDIR or /tmp) and Windows (_putenv_s, GetTempPath). */
#ifndef BB_TEST_PORTABLE_H
#define BB_TEST_PORTABLE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* setenv(name, value, 1); a null value removes the variable. Returns 0 on success. */
static inline int bb_test_setenv(const char* name, const char* value) {
#ifdef _WIN32
    /* _putenv_s with "" removes the variable. */
    return _putenv_s(name, value ? value : "") == 0 ? 0 : -1;
#else
    return value ? setenv(name, value, 1) : unsetenv(name);
#endif
}

/* Creates a new empty file "<temp dir>/<prefix>XXXXXX" and writes its path into path[size].
 * Returns 0 on success. The caller removes the file (remove(path)). */
static inline int bb_test_temp_file(char* path, size_t size, const char* prefix) {
#ifdef _WIN32
    char dir[MAX_PATH + 1];
    const DWORD n = GetTempPathA(sizeof(dir), dir);
    if (n == 0 || n > MAX_PATH) {
        return -1;
    }
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        const unsigned id = (unsigned)GetCurrentProcessId() * 7919u + GetTickCount() + attempt;
        if (snprintf(path, size, "%s%s%06x", dir, prefix, id & 0xffffffu) >= (int)size) {
            return -1;
        }
        const HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            return 0;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) {
            return -1;
        }
    }
    return -1;
#else
    const char* dir = getenv("TMPDIR");
    if (!dir || !*dir) {
        dir = "/tmp";
    }
    if (snprintf(path, size, "%s/%sXXXXXX", dir, prefix) >= (int)size) {
        return -1;
    }
    const int fd = mkstemp(path);
    if (fd < 0) {
        return -1;
    }
    close(fd);
    return 0;
#endif
}

/* Creates a new empty folder "<temp dir>/<prefix>XXXXXX" and writes its path into path[size].
 * Returns 0 on success. The caller removes it (rmdir). */
static inline int bb_test_temp_dir(char* path, size_t size, const char* prefix) {
#ifdef _WIN32
    char dir[MAX_PATH + 1];
    const DWORD n = GetTempPathA(sizeof(dir), dir);
    if (n == 0 || n > MAX_PATH) {
        return -1;
    }
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        const unsigned id = (unsigned)GetCurrentProcessId() * 7919u + GetTickCount() + attempt;
        if (snprintf(path, size, "%s%s%06x", dir, prefix, id & 0xffffffu) >= (int)size) {
            return -1;
        }
        if (CreateDirectoryA(path, NULL)) {
            return 0;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            return -1;
        }
    }
    return -1;
#else
    const char* dir = getenv("TMPDIR");
    if (!dir || !*dir) {
        dir = "/tmp";
    }
    if (snprintf(path, size, "%s/%sXXXXXX", dir, prefix) >= (int)size) {
        return -1;
    }
    return mkdtemp(path) ? 0 : -1;
#endif
}

/* Replaces the file's contents with data (binary: no newline translation). Returns 0 on success. */
static inline int bb_test_write_file(const char* path, const void* data, size_t size) {
    FILE* file = fopen(path, "wb");
    if (!file) {
        return -1;
    }
    const int ok = fwrite(data, 1, size, file) == size;
    return (fclose(file) == 0 && ok) ? 0 : -1;
}

#endif
