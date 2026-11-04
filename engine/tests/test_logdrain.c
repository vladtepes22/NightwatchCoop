/* Log drain (src/runtime_logdrain.c): printing 6 MiB of numbered lines while the reader of stdout does not read.
 *   logdrain-test N   starts the drain (unless BB_LOG_ASYNC=0), prints N lines, prints the time the printing took
 *                     (ms, to stderr) and exits through the normal exit path. */
#include "runtime_logdrain.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif
int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1000;
    setvbuf(stdout, NULL, _IONBF, 0);
    runtime_logdrain_start();
    unsigned long long t0 = GetTickCount64();
    for (int i = 0; i < n; ++i) printf("line %06d xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n", i);
    fprintf(stderr, "printed in %llu ms\n", GetTickCount64() - t0);
    return 0;
}
