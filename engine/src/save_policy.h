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

/* ---- saves from other builds (docs/SAVES.md "Saves from other builds") ----
 * Our layout keeps a save's description BESIDE its folder: <title id>/<dir>.sce_sys/param.bin (the
 * raw OrbisSaveDataParam, 1328 bytes). shadPS4 keeps it INSIDE: <title id>/<dir>/sce_sys/param.sfo
 * (a PSF file: MAINTITLE, SUBTITLE, DETAIL, SAVEDATA_LIST_PARAM, ...). The game's own files are the
 * same in both. We read either and write both, so a save moves between the two builds as it is. */
typedef struct { char title[128], subtitle[128], detail[1024]; uint32_t user_param; } SaveSfo;
/* A param.sfo's save description. 0 read, -1 not a PSF file. */
int runtime_savepolicy_sfo_parse(const void *data, size_t size, SaveSfo *out);
/* A save param.sfo as shadPS4 writes it, into out (cap bytes, 4096 is plenty). Its size, 0 if too small. */
size_t runtime_savepolicy_sfo_build(const SaveSfo *in, const char *dir_name, const char *title_id, void *out, size_t cap);
/* One of Bloodborne's title ids (CUSA00207 US, CUSA03173 EU, CUSA00900 JP, ...). */
int runtime_savepolicy_bloodborne_title(const char *title_id);
/* At start, before the check: makes <user>/<dir>/<user id>/<title id> (the folder this game reads)
 * hold the saves a player brought from another build. A save folder in shadPS4's layout gets our
 * <dir>.sce_sys/param.bin from its sce_sys/param.sfo (both kept). Without any save there, the newest
 * save under another user id or another Bloodborne title id (another region's game, another build's
 * user folder) is copied in (never moved; the original stays). Saves found anywhere else under
 * <user>/<dir> are named in the log with where they belong. Returns the number of saves taken in. */
int runtime_savepolicy_adopt(const char *user, const char *dir, int user_id, const char *title_id);
/* Party side files of a character that is no longer in its save slot. The party keeps per character
 * state next to the saves, keyed by save slot: party_rot.txt, party_stain.txt, party_prologue.txt,
 * party_gestures.txt, party_summoned_gives.txt, party_owed_lots.txt (and the unkeyed story replay
 * queue party_story.json). A save copied over the party save (or a new game in a used slot) put
 * another character in that slot, which then got the old one's rot, bloodstain, prologue reading and
 * owed items. <user>/party_characters.txt remembers each slot's character (its name and play time);
 * a slot whose name changed, whose play time went back, or that was emptied has its entries dropped.
 * save_folder: the folder with userdata0000..0009. Returns a bit per slot that changed. */
int runtime_savepolicy_side_files(const char *user, const char *save_folder);

/* runtime_file.c: save activity. generation changes with every write, rename or removal under
 * /savedataN; open_writes counts save files open for writing. */
void runtime_file_save_activity(uint64_t *generation, int *open_writes);
#endif
