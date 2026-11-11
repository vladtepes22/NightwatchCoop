/* Log drain (src/runtime_logdrain.c): printing 6 MiB of numbered lines while the reader of stdout does not read.
 *   logdrain-test N   starts the drain (unless BB_LOG_ASYNC=0), prints N lines, prints the time the printing took
 *                     (ms, to stderr) and exits through the normal exit path.
 *   logdrain-test N idle  as kill, after a 1 s pause (the drain's idle backoff must still deliver and flush).
 *   logdrain-test N kill  then prints a last line and ends with BB_TERMINATE_PROCESS(23) (flushes the drain first). */
#include "runtime_logdrain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1000;
    if (argc > 2 && !strcmp(argv[2], "ips")) { /* the address mask: whole lines, a line printed in pieces, a last line without its newline */
        setvbuf(stdout, NULL, _IONBF, 0);
        runtime_logdrain_start();
        printf("send 4 -> 192.168.1.20:9307 vport 1->2\n");
        printf("peer 8.8.4.4");
        Sleep(300);
        printf(":9307 and [2001:db8:1:2::5]:9307 lo 127.0.0.1\n");
        printf("tail 10.8.0.2");
        return 0;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    runtime_logdrain_start();
    unsigned long long t0 = GetTickCount64();
    if (argc > 2 && !strcmp(argv[2], "idle")) Sleep(1000); /* the drain's poll has backed off to 10 ms by now */
    int width = argc > 3 ? atoi(argv[3]) : 100; /* bytes of x after the line number */
    char *pad = malloc((size_t)width + 1);
    memset(pad, 'x', (size_t)width); pad[width] = 0;
    for (int i = 0; i < n; ++i) printf("line %06d %s\n", i, pad);
    fprintf(stderr, "printed in %llu ms\n", GetTickCount64() - t0);
    if (argc > 2 && (!strcmp(argv[2], "kill") || !strcmp(argv[2], "idle"))) { /* a fatal stop: no atexit handlers (as the crash handler) */
        puts("LAST LINE before the fatal stop");
        BB_TERMINATE_PROCESS(23);
    }
    return 0;
}
