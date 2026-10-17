/* Windows implementations of the POSIX pieces declared in win32_compat.h. */
#ifdef _WIN32
#define _CRT_RAND_S
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <io.h>
#include <direct.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include "win32_compat.h"

int runtime_win_mkdir(const char *path, int mode) { (void)mode; return _mkdir(path); }
/* POSIX rename replaces an existing target; MoveFileEx does so only when asked. */
int runtime_win_rename(const char *from, const char *to) {
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) return 0;
    DWORD e = GetLastError();
    if (e==ERROR_ACCESS_DENIED || e==ERROR_SHARING_VIOLATION) {
        /* Windows 10's POSIX rename keeps existing readers on the old save's file object. */
        DWORD count=GetFullPathNameA(to,0,NULL,NULL);
        char *absolute=count ? malloc(count) : NULL;
        if (absolute && GetFullPathNameA(to,count,absolute,NULL)) {
            int chars=MultiByteToWideChar(CP_ACP,0,absolute,-1,NULL,0);
            size_t size=offsetof(FILE_RENAME_INFO,FileName)+(size_t)chars*sizeof(wchar_t);
            FILE_RENAME_INFO *info=chars ? calloc(1,size) : NULL;
            HANDLE file=CreateFileA(from,DELETE|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                    NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
            if (info && file!=INVALID_HANDLE_VALUE) {
                info->Flags=FILE_RENAME_FLAG_REPLACE_IF_EXISTS|FILE_RENAME_FLAG_POSIX_SEMANTICS;
                info->FileNameLength=(DWORD)(chars-1)*sizeof(wchar_t);
                MultiByteToWideChar(CP_ACP,0,absolute,-1,info->FileName,chars);
                BOOL renamed=SetFileInformationByHandle(file,FileRenameInfoEx,info,(DWORD)size);
                if (!renamed) e=GetLastError();
                CloseHandle(file); free(info); free(absolute);
                if (renamed) return 0;
                absolute=NULL;
            } else { if (file!=INVALID_HANDLE_VALUE) CloseHandle(file); free(info); }
        }
        free(absolute);
    }
    errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT
          : e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION ? EACCES
          : e == ERROR_DIR_NOT_EMPTY ? ENOTEMPTY : EIO;
    return -1;
}
/* Positional I/O keeps the descriptor's file position, as pread/pwrite do. */
static int64_t positional(int fd, void *buffer, size_t size, int64_t offset, int write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (offset < 0) { errno = EINVAL; return -1; }
    LARGE_INTEGER zero = {0}, position;
    if (!SetFilePointerEx(h, zero, &position, FILE_CURRENT)) { errno = ESPIPE; return -1; }
    int64_t done = 0;
    while ((size_t)done < size) {
        DWORD chunk = size - (size_t)done > 0x40000000u ? 0x40000000u : (DWORD)(size - (size_t)done), n = 0;
        OVERLAPPED at = {0};
        at.Offset = (DWORD)(uint64_t)(offset + done);
        at.OffsetHigh = (DWORD)((uint64_t)(offset + done) >> 32);
        BOOL ok = write ? WriteFile(h, (const char *)buffer + done, chunk, &n, &at)
                        : ReadFile(h, (char *)buffer + done, chunk, &n, &at);
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF) break;
            SetFilePointerEx(h, position, NULL, FILE_BEGIN);
            if (done) return done;
            errno = e == ERROR_NOACCESS ? EFAULT : e == ERROR_ACCESS_DENIED ? EBADF : EIO;
            return -1;
        }
        done += n;
        if (n < chunk) break;
    }
    SetFilePointerEx(h, position, NULL, FILE_BEGIN);
    return done;
}
int64_t runtime_win_pread(int fd, void *buffer, size_t size, int64_t offset) {
    return positional(fd, buffer, size, offset, 0);
}
int64_t runtime_win_pwrite(int fd, const void *buffer, size_t size, int64_t offset) {
    return positional(fd, (void *)buffer, size, offset, 1);
}
/* Depth-first removal of a directory tree (nftw FTW_DEPTH|FTW_PHYS on Linux). */
int runtime_win_remove_tree(const char *path) {
    struct _stat64 s;
    if (_stat64(path, &s)) return -1;
    if (!(s.st_mode & _S_IFDIR)) return remove(path);
    DIR *d = opendir(path);
    if (d) {
        for (struct dirent *e; (e = readdir(d));) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[2048];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            runtime_win_remove_tree(child);
        }
        closedir(d);
    }
    return _rmdir(path);
}

int64_t runtime_utc_offset(time_t when) {
    struct tm local;
    if (localtime_s(&local, &when)) return 0;
    return (int64_t)(_mkgmtime(&local) - when);
}
static int64_t filetime_us(FILETIME t) {
    return (int64_t)((((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime) / 10);
}
void runtime_cpu_times(int thread, int64_t *user_us, int64_t *system_us) {
    FILETIME created, exited, kernel, user;
    BOOL ok = thread ? GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)
                     : GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    *user_us = ok ? filetime_us(user) : 0;
    *system_us = ok ? filetime_us(kernel) : 0;
}
int runtime_random(void *buffer, size_t size) {
    unsigned char *out = buffer;
    for (size_t i = 0; i < size; i += 4) {
        unsigned int value;
        if (rand_s(&value)) { errno = EIO; return -1; }
        memcpy(out + i, &value, size - i < 4 ? size - i : 4);
    }
    return 0;
}

static DWORD tls_slot = TLS_OUT_OF_INDEXES;
static INIT_ONCE tls_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK allocate_slot(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once; (void)parameter; (void)context;
    tls_slot = TlsAlloc();
    return TRUE;
}
/* Slots below 64 are TEB.TlsSlots (gs:[0x1480 + slot*8]), later ones TEB.TlsExpansionSlots
 * (gs:[0x1780] points to them); probe.c rewrites the guest's reads for either. */
uint32_t runtime_win_tls_slot(void) {
    InitOnceExecuteOnce(&tls_once, allocate_slot, NULL, NULL);
    if (tls_slot == TLS_OUT_OF_INDEXES) {
        fputs("STOP: no TLS slot for the guest thread pointer\n", stderr);
        exit(21);
    }
    return tls_slot;
}
void runtime_win_set_tcb(void *tcb) { TlsSetValue(runtime_win_tls_slot(), tcb); }
void runtime_win_set_thread_name(const char *name) {
    wchar_t wide[64];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 64)) SetThreadDescription(GetCurrentThread(), wide);
}

int runtime_win_inet_pton4(const char *src, void *dst) { return inet_pton(AF_INET, src, dst); }
const char *runtime_win_inet_ntop4(const void *src, char *dst, uint32_t size) {
    return inet_ntop(AF_INET, src, dst, size);
}
int runtime_win_open_shared(const char *path, int flags, int mode) {
    DWORD access = (flags & 3) == O_WRONLY ? GENERIC_WRITE : (flags & 3) == O_RDWR ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    DWORD disposition = (flags & O_CREAT) ? ((flags & O_EXCL) ? CREATE_NEW : (flags & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS)
                                          : (flags & O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;
    DWORD attributes = (flags & O_CREAT) && !(mode & _S_IWRITE) ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL;
    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           disposition, attributes, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT
              : e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS ? EEXIST
              : e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION ? EACCES
              : e == ERROR_DISK_FULL ? ENOSPC : EIO;
        return -1;
    }
    int crt = _O_BINARY | ((flags & 3) == O_WRONLY ? _O_WRONLY : (flags & 3) == O_RDWR ? _O_RDWR : _O_RDONLY);
    if (flags & O_APPEND) crt |= _O_APPEND;
    int fd = _open_osfhandle((intptr_t)h, crt); /* not inherited: NULL security attributes */
    if (fd < 0) { CloseHandle(h); errno = EMFILE; return -1; }
    return fd;
}


int runtime_win_thread_name(uint32_t thread_id, char *out, size_t size) {
    typedef HRESULT (WINAPI *GetThreadDescFn)(HANDLE, PWSTR *);
    static GetThreadDescFn get_desc = NULL;
    static int checked = 0;
    if (!size) return 0;
    out[0] = 0;
    if (!checked) {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        if (kb) get_desc = (GetThreadDescFn)(void *)GetProcAddress(kb, "GetThreadDescription");
        checked = 1;
    }
    if (!get_desc) return 0;
    HANDLE thread = thread_id ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread_id) : GetCurrentThread();
    if (!thread) return 0;
    PWSTR wide = NULL;
    int ok = SUCCEEDED(get_desc(thread, &wide)) && wide && wide[0] &&
             WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)size, NULL, NULL) > 0;
    if (!ok) out[0] = 0;
    out[size - 1] = 0;
    if (wide) LocalFree(wide);
    if (thread_id) CloseHandle(thread);
    return ok;
}


#endif
