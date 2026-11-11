/* The log drain: see runtime_logdrain.h. */
#include "runtime_logdrain.h"
#include "runtime_ipmask.h"
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define PIPE_BYTES (4u << 20)
#define FULL_SLACK (16u << 10) /* a pipe this close to full cannot take a normal log line */

static HANDLE rd, real_out;
static volatile LONG idle;
static volatile LONG started;
static volatile LONG full_hits; /* drain reads that found the pipe (nearly) full: the writers dropped lines meanwhile */

static uint64_t now_ms(void) { return GetTickCount64(); }

/* A 0/1 setting from the environment (env_name) or bbport.ini (key); def when neither is set. */
static int ini_flag(const char *env_name, const char *key, int def) {
    const char *e = getenv(env_name);
    if (e && (e[0] == '0' || e[0] == '1')) return e[0] == '1';
    int v = def;
    const char *path = getenv("BB_CONFIG");
    FILE *f = path ? fopen(path, "r") : NULL;
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *k = line; while (*k == ' ' || *k == '\t') ++k;
        char *ke = k + strlen(k); while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        if (!strcmp(k, key)) { const char *x = eq + 1; while (*x == ' ') ++x; v = (*x != '0'); }
    }
    if (f) fclose(f);
    return v;
}

static int ini_log_async(void) { return ini_flag("BB_LOG_ASYNC", "log_async", 1); }

/* 0.8.1 (NW-019b146): network addresses in the log are masked (1.2.3.4 -> 1.2.x.x, see runtime_ipmask.h) before
 * the line leaves the process. BB_LOG_RAW_IPS=1 / bbport.ini log_raw_ips=1 keeps them (a network problem that
 * needs the real addresses). A console is not drained, so a run from a console prints them as they are. */
static int mask_ips_on;
#define CARRY_MAX (32u << 10)
static char carry[CARRY_MAX];
static size_t carry_len;
static char work[sizeof carry + 64 * 1024];
static char masked[2 * sizeof work + 16];

/* Writes b[0..n) to the real stdout, masked when asked. Complete lines only (a partial last line waits in the
 * carry until its newline arrives, or until the drain is idle: flush_tail). */
static void emit(const char *b, size_t n, int flush_tail, int *dead) {
    const char *src = b;
    size_t len = n;
    if (mask_ips_on) {
        memcpy(work, carry, carry_len);
        memcpy(work + carry_len, b, n);
        len = carry_len + n;
        size_t cut = len;
        if (!flush_tail) {
            cut = len;
            while (cut && work[cut - 1] != '\n') --cut;
            if (!cut && len > CARRY_MAX / 2) cut = len; /* no newline in a long run: do not hold it back */
        }
        carry_len = len - cut;
        memcpy(carry, work + cut, carry_len);
        len = runtime_ipmask(work, cut, masked);
        src = masked;
    }
    size_t off = 0;
    while (!*dead && off < len) {
        DWORD w = 0;
        if (!WriteFile(real_out, src + off, (DWORD)(len - off), &w, NULL) || !w) *dead = 1; /* reader gone: drop, never block */
        off += w;
    }
}

static DWORD WINAPI drain_thread(LPVOID arg) {
    (void)arg;
    static char buf[64 * 1024];
    /* BB_LOG_STALL_MS=N (diagnostics, off): a write to the real stdout that took N ms or more is logged. */
    const char *s = getenv("BB_LOG_STALL_MS");
    unsigned stall_ms = s && *s ? (unsigned)atoi(s) : 0;
    int dead = 0;
    uint64_t last_data = now_ms();
    /* Idle poll: 2 ms while lines come, 10 ms after 0.1 s without any (500 wake-ups a second at above-normal
     * priority all game otherwise). Idea from Nightwatch 5c29916. */
    for (;;) {
        /* Peek, then read what is there: a ReadFile that blocks on the pipe would also block every
         * PeekNamedPipe on it (the flush below) behind the handle's lock. */
        DWORD avail = 0, n = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;
        if (!avail) {
            /* Idle again after a flood: say once that lines were dropped (written straight to the real stdout, the
             * pipe may be full of the next flood by now). */
            LONG hits = InterlockedExchange(&full_hits, 0);
            if (hits && !dead) {
                char msg[200];
                int len = snprintf(msg, sizeof msg, "Log output: the log pipe was full %ld time(s) (the reader of stdout was too slow for %u MiB of log): "
                                   "log lines were dropped, the game was not held up\n", (long)hits, PIPE_BYTES >> 20);
                DWORD w = 0;
                if (len > 0) WriteFile(real_out, msg, (DWORD)len, &w, NULL);
            }
            if (carry_len) emit("", 0, 1, &dead); /* a last line without its newline */
            InterlockedExchange(&idle, 1);
            Sleep(now_ms() - last_data >= 100 ? 10 : 2);
            continue;
        }
        InterlockedExchange(&idle, 0);
        last_data = now_ms();
        if (avail + FULL_SLACK >= PIPE_BYTES) InterlockedIncrement(&full_hits);
        if (!ReadFile(rd, buf, avail < sizeof buf ? avail : (DWORD)sizeof buf, &n, NULL) || !n) break;
        uint64_t t0 = now_ms();
        emit(buf, n, 0, &dead);
        uint64_t dt = now_ms() - t0;
        if (stall_ms && dt >= stall_ms)
            printf("Log output: the reader of stdout took %llu ms to accept %lu bytes (the game's log lines waited in the log pipe meanwhile)\n",
                   (unsigned long long)dt, (unsigned long)n);
    }
    return 0;
}

void runtime_logdrain_flush(unsigned timeout_ms) {
    if (!started) return;
    uint64_t end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) return;
        if (!avail && idle) return; /* the drain clears idle before it reads, so this order cannot miss data */
        Sleep(1);
    }
}

static void at_exit_flush(void) { runtime_logdrain_flush(10000); }

void runtime_logdrain_start(void) {
    mask_ips_on = !ini_flag("BB_LOG_RAW_IPS", "log_raw_ips", 0);
    int async = ini_log_async();
    if (started || (!async && !mask_ips_on)) return;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    DWORD t = GetFileType(out);
    if (t != FILE_TYPE_PIPE && t != FILE_TYPE_DISK) return; /* a console is not buffered here */
    HANDLE r, w, own;
    /* _dup2 onto fd 1 closes the handle fd 1 had: the drain keeps its own copy of it. */
    if (!DuplicateHandle(GetCurrentProcess(), out, GetCurrentProcess(), &own, 0, FALSE, DUPLICATE_SAME_ACCESS)) return;
    if (!CreatePipe(&r, &w, NULL, PIPE_BYTES)) { CloseHandle(own); return; }
    /* A full pipe must drop the line, not hold the thread that printed it (the game's main or render thread):
     * the write end never waits. (A write that finds no room returns 0 bytes written; the C runtime counts it as
     * a failed write and goes on.) BB_LOG_BLOCK=1 keeps the old waiting writes. */
    { DWORD nowait = PIPE_NOWAIT; const char *b = getenv("BB_LOG_BLOCK");
      /* BB_LOG_ASYNC=0 with the mask on: the pipe still waits for room, as the direct writes did */
      if (async && !(b && b[0] == '1')) SetNamedPipeHandleState(w, &nowait, NULL, NULL); }
    rd = r; real_out = own;
    HANDLE th = CreateThread(NULL, 0, drain_thread, NULL, 0, NULL);
    int wfd = th ? _open_osfhandle((intptr_t)w, _O_WRONLY | _O_BINARY) : -1;
    if (wfd < 0) { /* nothing changed yet except the thread: let it end */
        if (th) { CloseHandle(w); CloseHandle(th); } else { CloseHandle(r); CloseHandle(w); }
        CloseHandle(own); return;
    }
    fflush(stdout); fflush(stderr);
    _dup2(wfd, 1);
    if (err == out) _dup2(wfd, 2);
    _close(wfd);
    SetThreadPriority(th, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(th);
    started = 1;
    atexit(at_exit_flush);
}
#else
void runtime_logdrain_start(void) {}
void runtime_logdrain_flush(unsigned timeout_ms) { (void)timeout_ms; }
#endif
