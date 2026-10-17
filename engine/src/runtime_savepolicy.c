/* Save policy: separate party save folder, rotating backups, save check at start (save_policy.h).
 * Self-contained (C library + Win32 only) so tests/test_save_policy.c can build it alone. */
#define _GNU_SOURCE
#include "save_policy.h"
#include "../gpu/party_env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

const char *runtime_file_user_dir(void);

#define SLOT_SIZE 0x140000   /* userdata0000..0009, backup0000..0009: one character each */
#define SYSTEM_SIZE 0x40000  /* userdata0010, backup0010: game data (options, ...) */
#define PARAM_SIZE 1328      /* .sce_sys/param.bin (runtime_savedata.c Param) */
#define INFO_NAME "BACKUP_INFO.txt"

static char dir_name[32];

void runtime_savepolicy_reset(void) { dir_name[0]=0; }
/* A party run (gpu/party_env.h: unset, empty, 0, off, false, no = off). */
static int party_session(void) { return bb_party_enabled(); }
/* A party session always plays from the party saves (docs/party/ingame_flow.md §5): co-op progress
 * sync and items can never reach a single-player character. BB_PARTY_SAVE=shared is ignored. */
const char *runtime_savepolicy_dir(void) {
    if (!dir_name[0]) {
        const char *s=getenv("BB_PARTY_SAVE");
        if (party_session() && s && !strcmp(s,"shared"))
            printf("Saves: BB_PARTY_SAVE=shared is ignored: a party session always uses the party saves\n");
        snprintf(dir_name,sizeof(dir_name),"%s",party_session() ? "savedata_party" : "savedata");
    }
    return dir_name;
}
static int env_int(const char *name,int fallback,int lo,int hi) {
    const char *v=getenv(name);
    if (!v || !*v) return fallback;
    char *end; long n=strtol(v,&end,10);
    if (*end || n<lo || n>hi) return fallback;
    return (int)n;
}

/* ---- host file helpers ---- */
static int is_dir(const char *p) { struct stat s; return !stat(p,&s) && S_ISDIR(s.st_mode); }
static int make_dir(const char *p) {
#ifdef _WIN32
    return _mkdir(p);
#else
    return mkdir(p,0755);
#endif
}
static int make_dirs(const char *path) {
    char buf[1024]; snprintf(buf,sizeof(buf),"%s",path);
    for (char *p=buf+1;*p;++p) if ((*p=='/' || *p=='\\') && p[-1]!=':') { char c=*p; *p=0; make_dir(buf); *p=c; }
    return make_dir(buf) && errno!=EEXIST ? -1 : 0;
}
static void remove_tree(const char *path) {
    if (!is_dir(path)) { remove(path); return; }
    DIR *d=opendir(path);
    if (d) {
        for (struct dirent *e; (e=readdir(d));) {
            if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
            char child[1024]; snprintf(child,sizeof(child),"%s/%s",path,e->d_name);
            remove_tree(child);
        }
        closedir(d);
    }
    rmdir(path);
}
static int rename_dir(const char *from,const char *to) {
#ifdef _WIN32
    return MoveFileExA(from,to,MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(from,to);
#endif
}
/* The whole file. Opened shared for delete on Windows: the game may rename a save over it meanwhile. */
static int read_all(const char *path,unsigned char **data,size_t *size) {
    *data=NULL; *size=0;
#ifdef _WIN32
    HANDLE h=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if (h==INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER n;
    if (!GetFileSizeEx(h,&n) || n.QuadPart>(256<<20)) { CloseHandle(h); return -1; }
    unsigned char *buf=malloc((size_t)n.QuadPart+1);
    size_t done=0;
    while (buf && done<(size_t)n.QuadPart) {
        DWORD got=0;
        if (!ReadFile(h,buf+done,(DWORD)((size_t)n.QuadPart-done),&got,NULL) || !got) break;
        done+=got;
    }
    CloseHandle(h);
    if (!buf || done!=(size_t)n.QuadPart) { free(buf); return -1; }
#else
    FILE *f=fopen(path,"rb");
    if (!f) return -1;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    unsigned char *buf=n>=0 ? malloc((size_t)n+1) : NULL;
    size_t done=buf ? fread(buf,1,(size_t)n,f) : 0;
    fclose(f);
    if (!buf || done!=(size_t)n) { free(buf); return -1; }
#endif
    *data=buf; *size=done;
    return 0;
}
static int write_all(const char *path,const void *data,size_t size) {
    FILE *f=fopen(path,"wb");
    if (!f) return -1;
    int ok=fwrite(data,1,size,f)==size;
    ok=!fflush(f) && ok;
    return !fclose(f) && ok ? 0 : -1;
}

/* ---- the file list of a save folder (relative paths, sorted) ---- */
typedef struct { char rel[400]; long long size; } Entry;
typedef struct { Entry *items; int count, cap; } List;
static int skipped(const char *name,int depth) {
    size_t n=strlen(name);
    return name[0]=='.' || (n>6 && !strcmp(name+n-6,".bbtmp")) || (depth==0 && !strcmp(name,INFO_NAME));
}
static void walk(const char *root,const char *rel,int depth,List *l) {
    char path[1024]; snprintf(path,sizeof(path),"%s%s%s",root,*rel ? "/" : "",rel);
    DIR *d=opendir(path);
    if (!d) return;
    for (struct dirent *e; (e=readdir(d));) {
        if (skipped(e->d_name,depth)) continue;
        char child_rel[400], child[1100];
        if ((size_t)snprintf(child_rel,sizeof(child_rel),"%s%s%s",rel,*rel ? "/" : "",e->d_name)>=sizeof(child_rel)) continue;
        snprintf(child,sizeof(child),"%s/%s",root,child_rel);
        struct stat s;
        if (stat(child,&s)) continue;
        if (S_ISDIR(s.st_mode)) { if (depth<6) walk(root,child_rel,depth+1,l); continue; }
        if (l->count==l->cap) {
            int cap=l->cap ? l->cap*2 : 32;
            Entry *grown=realloc(l->items,(size_t)cap*sizeof(Entry));
            if (!grown) continue;
            l->items=grown; l->cap=cap;
        }
        snprintf(l->items[l->count].rel,sizeof(l->items[0].rel),"%s",child_rel);
        l->items[l->count++].size=(long long)s.st_size;
    }
    closedir(d);
}
static int by_rel(const void *a,const void *b) { return strcmp(((const Entry *)a)->rel,((const Entry *)b)->rel); }
static List list_files(const char *root) {
    List l={0};
    walk(root,"",0,&l);
    if (l.count) qsort(l.items,(size_t)l.count,sizeof(Entry),by_rel);
    return l;
}
static const char *base_name(const char *rel) { const char *s=strrchr(rel,'/'); return s ? s+1 : rel; }
/* userdata0000..0010 / backup0000..0010: the slot number, else -1. */
static int save_number(const char *name,int *is_userdata) {
    const char *digits=NULL;
    if (!strncmp(name,"userdata",8)) { digits=name+8; *is_userdata=1; }
    else if (!strncmp(name,"backup",6)) { digits=name+6; *is_userdata=0; }
    else return -1;
    if (strlen(digits)!=4) return -1;
    for (int i=0;i<4;++i) if (digits[i]<'0' || digits[i]>'9') return -1;
    int n=atoi(digits);
    return n<=10 ? n : -1;
}
static int all_zero(const unsigned char *p,size_t n) { for (size_t i=0;i<n;++i) if (p[i]) return 0; return 1; }
static uint64_t fnv(uint64_t h,const void *data,size_t n) {
    const unsigned char *p=data;
    for (size_t i=0;i<n;++i) { h^=p[i]; h*=1099511628211ULL; }
    return h;
}

#define NOTE(level,...) do { if (log>=(level)) { printf(__VA_ARGS__); fflush(stdout); } } while (0)
/* log: 0 quiet, 1 errors, 2 errors and warnings. */
int runtime_savepolicy_check(const char *folder,const char *ref,int log,SavePolicyCheck *out) {
    SavePolicyCheck c={0};
    c.hash=1469598103934665603ULL;
    List l=list_files(folder);
    for (int i=0;i<l.count;++i) {
        const Entry *e=&l.items[i];
        const char *name=base_name(e->rel);
        int userdata=0, n=save_number(name,&userdata);
        unsigned char *data=NULL; size_t size=0;
        c.hash=fnv(c.hash,e->rel,strlen(e->rel)+1);
        char path[1100]; snprintf(path,sizeof(path),"%s/%s",folder,e->rel);
        if (read_all(path,&data,&size)) {
            ++c.errors; NOTE(1,"Saves: ERROR %s: cannot be read\n",path);
            continue;
        }
        c.hash=fnv(c.hash,&size,sizeof(size));
        c.hash=fnv(c.hash,data,size);
        if (n>=0) {
            ++c.files;
            size_t want=n==10 ? SYSTEM_SIZE : SLOT_SIZE;
            int zero=all_zero(data,size);
            if (size!=want) {
                ++c.errors;
                NOTE(1,"Saves: ERROR %s: %zu bytes, expected %zu (truncated or damaged)\n",path,size,want);
            } else if (n==10 && zero) {
                ++c.errors;
                NOTE(1,"Saves: ERROR %s: the game data file is all zeros (damaged)\n",path);
            } else if (n<10 && !zero && all_zero(data,16)) {
                ++c.warnings;
                NOTE(2,"Saves: WARNING %s: the character data has no header (first 16 bytes zero)\n",path);
            }
            if (userdata && n<10 && !zero && size==want) ++c.slots_used;
            if (ref && size==want) {
                char other[1100]; snprintf(other,sizeof(other),"%s/%s",ref,e->rel);
                unsigned char *old=NULL; size_t old_size=0;
                if (!read_all(other,&old,&old_size) && old_size==want) {
                    if (userdata && n<10 && zero && !all_zero(old,old_size)) {
                        ++c.warnings;
                        NOTE(2,"Saves: WARNING %s: character slot %d is empty, but the backup %s has a character there\n",path,n,ref);
                    } else if (userdata && !zero && !all_zero(old,4) && memcmp(old,data,4)) {
                        ++c.warnings;
                        NOTE(2,"Saves: WARNING %s: header %02x%02x%02x%02x differs from the backup's %02x%02x%02x%02x\n",path,
                             data[0],data[1],data[2],data[3],old[0],old[1],old[2],old[3]);
                    }
                }
                free(old);
            }
        } else if (!strcmp(name,"param.bin")) {
            ++c.files;
            if (size!=PARAM_SIZE) {
                ++c.errors;
                NOTE(1,"Saves: ERROR %s: %zu bytes, expected %d (damaged save description)\n",path,size,PARAM_SIZE);
            }
        }
        free(data);
    }
    if (ref) { /* save files the backup has and the folder lost */
        List r=list_files(ref);
        for (int i=0;i<r.count;++i) {
            int userdata=0;
            if (save_number(base_name(r.items[i].rel),&userdata)<0 || !userdata) continue;
            Entry key; snprintf(key.rel,sizeof(key.rel),"%s",r.items[i].rel);
            if (!l.count || !bsearch(&key,l.items,(size_t)l.count,sizeof(Entry),by_rel)) {
                ++c.warnings;
                NOTE(2,"Saves: WARNING %s/%s is missing (the backup %s has it)\n",folder,r.items[i].rel,ref);
            }
        }
        free(r.items);
    }
    free(l.items);
    if (out) *out=c;
    return c.errors;
}

/* ---- backups ---- */
static void backups_dir(const char *user,const char *dir,char *out,size_t size) {
    snprintf(out,size,"%s/save_backups/%s",user,dir);
}
typedef struct { char name[128]; } Name;
/* <YYYYMMDD-HHMMSS>-<reason>[-NN][-suspect][.partial]: time, then NN (a second backup in the same second). */
static int same_second_number(const char *name) {
    char b[128]; snprintf(b,sizeof(b),"%s",name);
    size_t n=strlen(b);
    if (n>8 && !strcmp(b+n-8,".partial")) b[n-=8]=0;
    if (n>8 && !strcmp(b+n-8,"-suspect")) b[n-=8]=0;
    return n>3 && b[n-3]=='-' && b[n-2]>='0' && b[n-2]<='9' && b[n-1]>='0' && b[n-1]<='9' ? (b[n-2]-'0')*10+b[n-1]-'0' : 1;
}
static int by_name(const void *a,const void *b) {
    const char *x=((const Name *)a)->name, *y=((const Name *)b)->name;
    int t=strncmp(x,y,15);
    if (t) return t;
    int nx=same_second_number(x), ny=same_second_number(y);
    return nx!=ny ? (nx<ny ? -1 : 1) : strcmp(x,y);
}
/* Backup folders under base, oldest first (names start with the time). kind: 0 good, 1 suspect, 2 partial. */
static int backup_names(const char *base,int kind,Name *out,int max) {
    int n=0;
    DIR *d=opendir(base);
    if (!d) return 0;
    for (struct dirent *e; (e=readdir(d));) {
        if (e->d_name[0]=='.' || strlen(e->d_name)>=sizeof(out[0].name)) continue;
        char path[1100]; snprintf(path,sizeof(path),"%s/%s",base,e->d_name);
        if (!is_dir(path)) continue;
        size_t len=strlen(e->d_name);
        int partial=len>8 && !strcmp(e->d_name+len-8,".partial");
        int k=partial ? 2 : strstr(e->d_name,"-suspect") ? 1 : 0;
        if (k==kind && n<max) memcpy(out[n++].name,e->d_name,len+1);
    }
    closedir(d);
    qsort(out,(size_t)n,sizeof(Name),by_name);
    return n;
}
int runtime_savepolicy_newest_good(const char *user,const char *dir,char *out,size_t size) {
    char base[700]; backups_dir(user,dir,base,sizeof(base));
    static Name names[512];
    int n=backup_names(base,0,names,512);
    if (!n) return -1;
    snprintf(out,size,"%s/%s",base,names[n-1].name);
    return 0;
}
static void rotate(const char *base,int keep) {
    static Name names[512];
    for (int kind=0;kind<3;++kind) {
        int n=backup_names(base,kind,names,512);
        int limit=kind==2 ? 0 : keep; /* partial: a copy interrupted by a crash */
        for (int i=0;i<n-limit;++i) {
            char path[1100];
            if ((size_t)snprintf(path,sizeof(path),"%s/%s",base,names[i].name)>=sizeof(path)) continue;
            remove_tree(path);
            printf("Saves: removed %s backup %s\n",kind==0 ? "old" : kind==1 ? "old suspect" : "unfinished",path);
        }
    }
}
static void write_readme(const char *user) {
    char path[800]; snprintf(path,sizeof(path),"%s/save_backups/README.txt",user);
    struct stat s;
    if (!stat(path,&s)) return;
    static const char text[]=
        "Bloodborne port - save backups\r\n"
        "==============================\r\n\r\n"
        "The game copies its save folder here when a party session starts and every few minutes\r\n"
        "while it runs (BB_SAVE_BACKUP_MINUTES, default 10). The newest 10 backups are kept\r\n"
        "(BB_SAVE_BACKUP_KEEP). A copy identical to the newest backup is not kept again.\r\n\r\n"
        "  save_backups\\savedata\\...        backups of user\\savedata (single-player saves)\r\n"
        "  save_backups\\savedata_party\\...  backups of user\\savedata_party (party saves)\r\n\r\n"
        "Each backup folder is named <date>-<time>-<reason> (start, restart, timer, ...).\r\n"
        "Before every party session, save_backups\\party_sessions\\<date>-<time>-host (or -guest) gets\r\n"
        "both save folders as they were: the launcher's Party tab \"Undo last party session\" puts\r\n"
        "the newest one back (it first saves the current state there too, so an undo can be undone).\r\n"
        "A name ending in -suspect means the copy failed the save check (wrong file sizes, a damaged\r\n"
        "game data file): restore from it only if nothing else is left. Suspect backups never push\r\n"
        "good ones out. BACKUP_INFO.txt in each folder lists what the check found.\r\n\r\n"
        "To restore a backup:\r\n"
        "  1. Close the game (and the launcher's game window).\r\n"
        "  2. Move the current save folder away, e.g. rename user\\savedata_party to\r\n"
        "     user\\savedata_party.broken (keep it until the restored save works).\r\n"
        "  3. Create an empty user\\savedata_party (or user\\savedata) folder and copy everything from\r\n"
        "     the backup folder into it except BACKUP_INFO.txt - that is the numbered folder\r\n"
        "     (e.g. 1\\CUSA03173\\SPRJ0005\\...).\r\n"
        "  4. Start the game. Its log line \"Saves: check OK\" confirms the restored files.\r\n";
    if (!write_all(path,text,sizeof(text)-1)) printf("Saves: restore instructions in %s\n",path);
}
static void stamp(char *out,size_t size) {
    time_t now=time(NULL); struct tm t;
#ifdef _WIN32
    localtime_s(&t,&now);
#else
    localtime_r(&now,&t);
#endif
    strftime(out,size,"%Y%m%d-%H%M%S",&t);
}
int runtime_savepolicy_backup(const char *user,const char *dir,const char *reason,int keep,char *made,size_t made_size) {
    char src[700], base[700], partial[940], final[920];
    snprintf(src,sizeof(src),"%s/%s",user,dir);
    backups_dir(user,dir,base,sizeof(base));
    List l=list_files(src);
    if (!l.count) { free(l.items); return 0; }
    if (make_dirs(base)) { printf("Saves: cannot create %s; no backup\n",base); free(l.items); return -1; }
    char when[32]; stamp(when,sizeof(when));
    /* After every backup of this second (also removed ones' successors): names sort by time, then number. */
    int number=0;
    DIR *d=opendir(base);
    for (struct dirent *e; d && (e=readdir(d));)
        if (!strncmp(e->d_name,when,15) && same_second_number(e->d_name)>number) number=same_second_number(e->d_name);
    if (d) closedir(d);
    if (number>=99) { free(l.items); return -2; } /* 99 backups in one second: try later */
    if (!number) snprintf(final,sizeof(final),"%s/%s-%s",base,when,reason);
    else snprintf(final,sizeof(final),"%s/%s-%s-%02d",base,when,reason,number+1);
    snprintf(partial,sizeof(partial),"%s.partial",final);
    uint64_t gen0=0, gen1=0; int open0=0, open1=0;
    runtime_file_save_activity(&gen0,&open0);
    if (open0) { free(l.items); return -2; }
    int failed=make_dirs(partial);
    for (int i=0;i<l.count && !failed;++i) {
        char from[1100], to[1400];
        snprintf(from,sizeof(from),"%s/%s",src,l.items[i].rel);
        snprintf(to,sizeof(to),"%s/%s",partial,l.items[i].rel);
        char *slash=strrchr(to,'/'); *slash=0; make_dirs(to); *slash='/';
        unsigned char *data; size_t size;
        if (read_all(from,&data,&size)) { printf("Saves: cannot read %s for the backup\n",from); failed=1; break; }
        failed=write_all(to,data,size)!=0;
        free(data);
    }
    free(l.items);
    runtime_file_save_activity(&gen1,&open1);
    if (failed || gen1!=gen0 || open1) {
        remove_tree(partial);
        if (failed) { printf("Saves: backup to %s failed\n",partial); return -1; }
        return -2;
    }
    char ref[900]; int have_ref=!runtime_savepolicy_newest_good(user,dir,ref,sizeof(ref));
    SavePolicyCheck c, r;
    runtime_savepolicy_check(partial,have_ref ? ref : NULL,0,&c);
    if (!c.errors && have_ref) {
        runtime_savepolicy_check(ref,NULL,0,&r);
        if (r.hash==c.hash) {
            remove_tree(partial);
            printf("Saves: unchanged since the backup %s\n",ref);
            fflush(stdout);
            return 0;
        }
    }
    if (c.errors && strlen(final)+9<sizeof(final)) strcat(final,"-suspect");
    char info[1024], infopath[1000];
    int n=snprintf(info,sizeof(info),"source=%s\r\nreason=%s\r\ntime=%s\r\nfiles=%d\r\ncharacter_slots=%d\r\nerrors=%d\r\nwarnings=%d\r\nhash=%016llx\r\n",
                   src,reason,when,c.files,c.slots_used,c.errors,c.warnings,(unsigned long long)c.hash);
    snprintf(infopath,sizeof(infopath),"%s/%s",partial,INFO_NAME);
    write_all(infopath,info,(size_t)n);
    if (rename_dir(partial,final)) { printf("Saves: cannot finish the backup %s\n",final); remove_tree(partial); return -1; }
    if (c.errors) printf("Saves: WARNING the save folder failed the check; kept the copy as %s (good backups are kept)\n",final);
    else printf("Saves: backup %s (%d files, %d character slot%s)\n",final,c.files,c.slots_used,c.slots_used==1 ? "" : "s");
    rotate(base,keep);
    write_readme(user);
    fflush(stdout);
    if (made) snprintf(made,made_size,"%s",final);
    return 1;
}

/* ---- party session snapshots: "Undo last party session" (launcher, scripts/party_session.py) ---- */
/* Every file of src (relative paths kept) into dst. Files copied, or -1. */
static int copy_folder(const char *src,const char *dst) {
    List l=list_files(src);
    int n=0, failed=make_dirs(dst);
    for (int i=0;i<l.count && !failed;++i) {
        char from[1100], to[1400];
        snprintf(from,sizeof(from),"%s/%s",src,l.items[i].rel);
        snprintf(to,sizeof(to),"%s/%s",dst,l.items[i].rel);
        char *slash=strrchr(to,'/'); *slash=0; make_dirs(to); *slash='/';
        unsigned char *data; size_t size;
        if (read_all(from,&data,&size)) { printf("Saves: cannot read %s for the session snapshot\n",from); failed=1; break; }
        failed=write_all(to,data,size)!=0;
        free(data);
        ++n;
    }
    free(l.items);
    return failed ? -1 : n;
}
int runtime_savepolicy_session_snapshot(const char *user,const char *role,int keep,char *made,size_t made_size) {
    static const char *const folders[]={"savedata","savedata_party"};
    char base[700], final[900], partial[940], when[32];
    snprintf(base,sizeof(base),"%s/save_backups/party_sessions",user);
    if (make_dirs(base)) { printf("Saves: cannot create %s; no session snapshot\n",base); return -1; }
    stamp(when,sizeof(when));
    int number=0; /* after every snapshot of this second: names sort by time, then number */
    DIR *d=opendir(base);
    for (struct dirent *e; d && (e=readdir(d));)
        if (!strncmp(e->d_name,when,15) && same_second_number(e->d_name)>number) number=same_second_number(e->d_name);
    if (d) closedir(d);
    if (!number) snprintf(final,sizeof(final),"%s/%s-%s",base,when,role);
    else snprintf(final,sizeof(final),"%s/%s-%s-%02d",base,when,role,number+1);
    snprintf(partial,sizeof(partial),"%s.partial",final);
    remove_tree(partial);
    char info[1024];
    int len=snprintf(info,sizeof(info),"kind=session-start\r\nrole=%s\r\ntime=%s\r\n",role,when), failed=make_dirs(partial);
    for (int i=0;i<2 && !failed;++i) {
        char src[800], dst[1100];
        snprintf(src,sizeof(src),"%s/%s",user,folders[i]);
        snprintf(dst,sizeof(dst),"%s/%s",partial,folders[i]);
        int files=is_dir(src) ? copy_folder(src,dst) : 0;
        if (files<0) { failed=1; break; }
        SavePolicyCheck c={0};
        if (files) runtime_savepolicy_check(dst,NULL,0,&c);
        len+=snprintf(info+len,sizeof(info)-(size_t)len,"%s_files=%d\r\n%s_slots=%d\r\n%s_hash=%016llx\r\n",folders[i],files,
                      folders[i],c.slots_used,folders[i],(unsigned long long)(files ? c.hash : 0));
    }
    if (!failed) {
        char infopath[1000]; snprintf(infopath,sizeof(infopath),"%s/SESSION.txt",partial);
        failed=write_all(infopath,info,(size_t)len)!=0;
    }
    if (failed || rename_dir(partial,final)) {
        remove_tree(partial);
        printf("Saves: the party session snapshot %s failed\n",final);
        fflush(stdout);
        return -1;
    }
    printf("Saves: party session snapshot %s (role %s; the launcher's \"Undo last party session\" restores it)\n",final,role);
    rotate(base,keep);
    fflush(stdout);
    if (made) snprintf(made,made_size,"%s",final);
    return 1;
}

/* ---- the game's side ---- */
static int backup_minutes, backup_keep;
static void pause_seconds(int s) {
#ifdef _WIN32
    Sleep((DWORD)s*1000);
#else
    sleep((unsigned)s);
#endif
}
static void *backup_thread(void *arg) {
    (void)arg;
    for (;;) {
        pause_seconds(backup_minutes*60);
        for (int attempt=0;attempt<60;++attempt) { /* wait for 5 quiet seconds (no save writes) */
            uint64_t g0, g1; int o0, o1;
            runtime_file_save_activity(&g0,&o0);
            pause_seconds(5);
            runtime_file_save_activity(&g1,&o1);
            if (g0!=g1 || o0 || o1) continue;
            if (runtime_savepolicy_backup(runtime_file_user_dir(),runtime_savepolicy_dir(),"timer",backup_keep,NULL,0)!=-2) break;
        }
    }
    return NULL;
}
void runtime_savepolicy_start(void) {
    const char *user=runtime_file_user_dir(), *dir=runtime_savepolicy_dir();
    char folder[700]; snprintf(folder,sizeof(folder),"%s/%s",user,dir);
    int separate=!strcmp(dir,"savedata_party");
    if (separate)
        printf("Saves: party session with separate saves: using %s (the single-player saves in %s/savedata are not touched)\n",folder,user);
    List l=list_files(folder);
    int empty=!l.count;
    free(l.items);
    if (empty) {
        if (separate) printf("Saves: %s has no saves yet: the party game starts without characters (New Game "
                             "makes the party character; the launcher's Party tab can copy a single-player save).\n",folder);
    } else {
        char ref[900]; int have_ref=!runtime_savepolicy_newest_good(user,dir,ref,sizeof(ref));
        SavePolicyCheck c;
        runtime_savepolicy_check(folder,have_ref ? ref : NULL,2,&c);
        if (c.errors)
            printf("Saves: WARNING %s failed the save check (%d problem%s, see above). %s\n",folder,c.errors,c.errors==1 ? "" : "s",
                   have_ref ? "The newest good backup is listed below; user/save_backups/README.txt explains restoring it." :
                              "There is no good backup yet.");
        else
            printf("Saves: check OK: %s, %d files, %d character slot%s used%s\n",folder,c.files,c.slots_used,c.slots_used==1 ? "" : "s",
                   c.warnings ? " (warnings above)" : "");
        if (c.errors && have_ref) printf("Saves: newest good backup: %s\n",ref);
    }
    fflush(stdout);
    const char *b=getenv("BB_SAVE_BACKUP");
    int enabled=b && *b ? b[0]=='1' : party_session();
    if (!enabled) return;
    backup_keep=env_int("BB_SAVE_BACKUP_KEEP",10,1,500);
    backup_minutes=env_int("BB_SAVE_BACKUP_MINUTES",10,0,24*60);
    const char *restarted=getenv("BB_PARTY_RESTARTED");
    /* Before a party session (not a crash restart: the same session goes on): both save folders,
     * for the launcher's "Undo last party session". BB_SAVE_SESSION_KEEP (default 5, 0 = none). */
    int session_keep=env_int("BB_SAVE_SESSION_KEEP",5,0,100);
    if (party_session() && session_keep && !(restarted && restarted[0]=='1')) {
        const char *p=getenv("BB_PARTY");
        runtime_savepolicy_session_snapshot(user,p && !strcmp(p,"host") ? "host" : "guest",session_keep,NULL,0);
    }
    if (!empty) runtime_savepolicy_backup(user,dir,restarted && restarted[0]=='1' ? "restart" : "start",backup_keep,NULL,0);
    if (backup_minutes) {
        pthread_t t; pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr,256*1024); /* the copies are on the heap */
        pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t,&attr,backup_thread,NULL)) printf("Saves: no periodic backups (thread not created)\n");
        pthread_attr_destroy(&attr);
        printf("Saves: backups every %d min to %s/save_backups/%s (newest %d kept)\n",backup_minutes,user,dir,backup_keep);
        fflush(stdout);
    }
}
