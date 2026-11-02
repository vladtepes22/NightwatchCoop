/* Save commit: a save file written to a temporary copy replaces the real file in one rename.
 * Steps: flush the copy to disk, (guest in a party) keep the own world state, rename over the
 * file, flush the directory. A crash or kill at any point leaves the old file whole or the new
 * one whole (the leftover copy is removed at the next mount), never a mix.
 * The steps can take seconds on a slow or synced folder, so closing a save file hands them to
 * one background thread when BB_SAVE_COMMIT_ASYNC=1 (default: inline, the old code in runtime_file.c). Whatever looks at that file by path
 * meanwhile calls runtime_savecommit_wait_file first; the process end calls _flush. */
#ifndef RUNTIME_SAVECOMMIT_H
#define RUNTIME_SAVECOMMIT_H
#include <stdint.h>

typedef struct {
    /* Own world state kept in a party guest's save (runtime_savepolicy_world_fix_file); may be NULL. */
    int (*fix)(const char *temp, const char *commit);
    /* Called for every step (name, file, start/end ns); NULL = none. */
    void (*note)(const char *what, const char *path, uint64_t t0, uint64_t t1);
    uint64_t (*now)(void);
} SaveCommitOps;
void runtime_savecommit_configure(const SaveCommitOps *ops);

/* The steps, here and now. host: the open descriptor of the temp (closed here). 0 or -errno; on
 * an error the temp is removed and the old file kept. */
int runtime_savecommit_run(int host, const char *temp, const char *commit);
/* The steps in the background (or inline when async is off); takes the descriptor and the two
 * strings (malloc'ed: freed here). Returns 0, or the error when it ran inline. */
int runtime_savecommit_submit(int host, char *temp, char *commit);
/* Block while a commit of that file is queued or running / of a file in that directory (a listing). */
void runtime_savecommit_wait_file(const char *host_path);
void runtime_savecommit_wait_dir(const char *host_path);
/* Blocks until nothing is queued or running. */
void runtime_savecommit_flush(void);
/* Commits queued or running. */
int runtime_savecommit_pending(void);
/* True when this temp path belongs to a queued or running commit. */
int runtime_savecommit_owns_temp(const char *temp);
/* BB_SAVE_COMMIT_ASYNC=1 (default off). */
int runtime_savecommit_enabled(void);
#endif
