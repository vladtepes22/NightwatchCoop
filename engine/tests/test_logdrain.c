/* Log drain (src/runtime_logdrain.c): printing numbered lines while the reader of stdout does not read.
 *   logdrain-test N        starts the drain (unless BB_LOG_ASYNC=0), prints N lines, prints the time the printing
 *                          took (ms, to stderr) and exits through the normal exit path.
 *   logdrain-test N stop   ... then prints "last line before the stop" and stops the way a fatal stop does
 *                          (runtime_logdrain_flush, then _exit(23): no atexit handler runs).
 *   logdrain-test N tail   prints 1 KiB lines, then waits 4 s (the reader reads again), prints "tail line"
 *                          and exits normally: the log keeps working after a full pipe dropped lines. */
#include "runtime_logdrain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif
static char xs[1024];
int main(int argc, char **argv) {
    memset(xs, 'x', sizeof xs - 1);
    int n = argc > 1 ? atoi(argv[1]) : 1000;
    const char *mode = argc > 2 ? argv[2] : "";
    setvbuf(stdout, NULL, _IONBF, 0);
    runtime_logdrain_start();
    unsigned long long t0 = GetTickCount64();
    /* tail: 1 KiB lines, one write each (printf on an unbuffered stream writes each character on its
     * own: 1 KiB lines would measure that, not the drain) */
    const int big = !strcmp(mode, "tail");
    for (int i = 0; i < n; ++i) {
        char line[1100];
        const int len = snprintf(line, sizeof line, "line %06d %.*s\n", i, big ? 1012 : 100, xs);
        if (big) fwrite(line, 1, (size_t)len, stdout);
        else printf("%s", line);
    }
    fprintf(stderr, "printed in %llu ms\n", GetTickCount64() - t0);
    if (!strcmp(mode, "stop")) {
        printf("last line before the stop\n");
        runtime_logdrain_flush(3000);
        _exit(23);
    }
    if (!strcmp(mode, "tail")) {
        Sleep(4000);
        printf("tail line\n");
    }
    return 0;
}
