/* Save policy (party plan C5): which folder holds the saves, rotating backups and the start check.
 *
 *   BB_PARTY=host|join with BB_PARTY_SAVE=separate   saves live in <user>/savedata_party instead of
 *                                                    <user>/savedata (single-player saves untouched)
 *   BB_SAVE_BACKUP=1|0         backups on/off (default: on in a party session, off otherwise)
 *   BB_SAVE_BACKUP_MINUTES=N   a backup every N minutes while the game runs (default 10; 0: only at start)
 *   BB_SAVE_BACKUP_KEEP=N      backups kept per save folder (default 10)
 *
 * Backups: <user>/save_backups/<save folder>/<YYYYMMDD-HHMMSS>-<reason>[-suspect]/ holds a copy of the
 * save folder's contents (the numbered user folders) and BACKUP_INFO.txt. A copy that fails the check
 * (wrong file sizes, an emptied game-data file) is kept with -suspect and never pushes good backups out:
 * good and suspect backups have separate quotas. A copy identical to the newest good backup is dropped.
 * Restore: see <user>/save_backups/README.txt (written with the first backup). */
#ifndef BB_SAVE_POLICY_H
#define BB_SAVE_POLICY_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int files;      /* save files looked at */
    int errors;     /* wrong size, empty game-data file: the folder is damaged */
    int warnings;   /* differences from the newest good backup worth a look */
    int slots_used; /* character slots (userdata0000..0009) with data */
    uint64_t hash;  /* of every file's name and contents (equal folders, equal hashes) */
} SavePolicyCheck;

/* "savedata" or "savedata_party" (from the environment; read once). */
const char *runtime_savepolicy_dir(void);
/* At start (runtime_savedata_configure): reports the folder, checks it, makes the start backup and
 * starts the periodic backups. */
void runtime_savepolicy_start(void);
/* Checks a save folder; ref (may be NULL) is a good backup to compare with. log: print findings. */
int runtime_savepolicy_check(const char *folder, const char *ref, int log, SavePolicyCheck *out);
/* One backup of <user>/<dir> now. Returns 1 made, 0 unchanged or nothing to back up, -1 failed,
 * -2 the game wrote saves during the copy (nothing kept; try again later). made (may be NULL)
 * receives the new backup's folder. */
int runtime_savepolicy_backup(const char *user, const char *dir, const char *reason, int keep,
                              char *made, size_t made_size);
/* The newest backup without -suspect of <user>/save_backups/<dir> (0 found, -1 none). */
int runtime_savepolicy_newest_good(const char *user, const char *dir, char *out, size_t size);
/* Before a party session: <user>/savedata and <user>/savedata_party copied to
 * <user>/save_backups/party_sessions/<YYYYMMDD-HHMMSS>-<role>/ with SESSION.txt (role, time, files per
 * folder); newest `keep` kept. The launcher's "Undo last party session" restores it
 * (scripts/party_session.py). 1 made, -1 failed. */
int runtime_savepolicy_session_snapshot(const char *user, const char *role, int keep, char *made, size_t made_size);
/* Tests: forget the cached environment. */
void runtime_savepolicy_reset(void);

/* runtime_file.c: save activity. generation changes with every write, rename or removal under
 * /savedataN; open_writes counts save files open for writing. */
void runtime_file_save_activity(uint64_t *generation, int *open_writes);
#endif
