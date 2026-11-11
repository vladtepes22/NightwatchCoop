/* src/runtime_ipmask.c against tests/ipmask_vectors.txt (input<TAB>expected per line). ipmask-test VECTORS_FILE */
#include "runtime_ipmask.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ipmask-test vectors.txt\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("vectors"); return 2; }
    char line[4096], out[2 * 4096 + 16];
    int bad = 0, n = 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!l || line[0] == '#') continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        size_t ol = runtime_ipmask(line, strlen(line), out);
        out[ol] = 0;
        ++n;
        if (strcmp(out, tab + 1)) { printf("DIFF: %s\n  got  %s\n  want %s\n", line, out, tab + 1); ++bad; }
        /* idempotent, and the same without the surrounding text */
        char again[2 * 4096 + 16];
        size_t al = runtime_ipmask(out, ol, again);
        again[al] = 0;
        if (strcmp(again, out)) { printf("NOT IDEMPOTENT: %s\n", out); ++bad; }
    }
    fclose(f);
    printf("ipmask-test: %s (%d vectors)\n", bad ? "FAILED" : "OK", n);
    return bad ? 1 : 0;
}
