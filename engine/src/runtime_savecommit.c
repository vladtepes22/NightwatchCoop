/* Save commit steps and their background thread: see runtime_savecommit.h. */
#include "runtime_savecommit.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "win32_compat.h"
#endif

typedef struct Job { struct Job *next; int host; char *temp, *commit; } Job;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake=PTHREAD_COND_INITIALIZER, idle=PTHREAD_COND_INITIALIZER;
static Job *head, *tail, *running;
static int worker_started, atexit_set;
static SaveCommitOps ops;

void runtime_savecommit_configure(const SaveCommitOps *o) { ops=*o; }
static uint64_t now_ns(void) {
    if (ops.now) return ops.now();
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}
static void note(const char *what,const char *path,uint64_t t0,uint64_t t1) { if (ops.note) ops.note(what,path,t0,t1); }
static void sync_directory(const char *file) {
#ifdef _WIN32
    (void)file; return; /* the rename itself is MOVEFILE_WRITE_THROUGH (runtime_win_rename) */
#else
    char dir[1024];
    snprintf(dir,sizeof(dir),"%s",file);
    char *slash=strrchr(dir,'/');
    if (!slash) return;
    *slash=0;
    int d=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (d>=0) { fsync(d); close(d); }
#endif
}
int runtime_savecommit_run(int host,const char *temp,const char *commit) {
    uint64_t t0=now_ns();
    int e=fsync(host) ? errno : 0;
    close(host);
    /* BB_SAVE_IO_DELAY_MS: test aid, a slow disk or synced folder (the flush takes this long more). */
    static int delay=-1;
    if (delay<0) { const char *d=getenv("BB_SAVE_IO_DELAY_MS"); delay=d && *d ? atoi(d) : 0; }
    if (delay>0) {
        struct timespec ts={delay/1000,(long)(delay%1000)*1000000L};
        nanosleep(&ts,NULL);
    }
    uint64_t t1=now_ns();
    note("commit: flush to disk",commit,t0,t1);
    /* A party guest's character save written in the host's world keeps the own world's state. */
    if (!e && ops.fix) ops.fix(temp,commit);
    uint64_t t2=now_ns();
    note("commit: party world fix",commit,t1,t2);
    if (!e && rename(temp,commit)) e=errno;
    uint64_t t3=now_ns();
    note("commit: rename",commit,t2,t3);
    if (e) {
        fprintf(stderr,"Runtime: save file %s not replaced (%s); the old one is kept\n",commit,strerror(e));
        unlink(temp);
        return -e;
    }
    sync_directory(commit);
    return 0;
}
int runtime_savecommit_enabled(void) {
    static int v=-1;
    if (v<0) {
        /* BB_SAVE_COMMIT_ASYNC=0/1 wins; else bbport.ini (BB_CONFIG) save_commit_async=1; default off. */
        const char *e=getenv("BB_SAVE_COMMIT_ASYNC");
        if (e && (e[0]=='0' || e[0]=='1')) v=e[0]=='1';
        else {
            v=0;
            const char *path=getenv("BB_CONFIG");
            FILE *f=path ? fopen(path,"r") : NULL;
            char line[256];
            while (f && fgets(line,sizeof line,f)) {
                char *eq=strchr(line,'=');
                if (!eq || line[0]=='#') continue;
                *eq=0;
                char *k=line; while (*k==' ' || *k=='\t') ++k;
                char *ke=k+strlen(k); while (ke>k && (ke[-1]==' ' || ke[-1]=='\t')) *--ke=0;
                if (!strcmp(k,"save_commit_async")) { const char *x=eq+1; while (*x==' ') ++x; v=(*x=='1'); }
            }
            if (f) fclose(f);
        }
    }
    return v;
}
static void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lock);
    for (;;) {
        while (!head) pthread_cond_wait(&wake,&lock);
        Job *j=head; head=j->next; if (!head) tail=NULL;
        running=j;
        pthread_mutex_unlock(&lock);
        runtime_savecommit_run(j->host,j->temp,j->commit);
        pthread_mutex_lock(&lock);
        running=NULL;
        pthread_cond_broadcast(&idle);
        free(j->temp); free(j->commit); free(j);
    }
    return NULL;
}
int runtime_savecommit_submit(int host,char *temp,char *commit) {
    Job *j=runtime_savecommit_enabled() ? malloc(sizeof(*j)) : NULL;
    if (j) {
        *j=(Job){.host=host,.temp=temp,.commit=commit};
        pthread_mutex_lock(&lock);
        if (!worker_started) {
            pthread_t t;
            if (!pthread_create(&t,NULL,worker,NULL)) { pthread_detach(t); worker_started=1; }
            if (!atexit_set) { atexit(runtime_savecommit_flush); atexit_set=1; }
        }
        if (worker_started) {
            if (tail) tail->next=j; else head=j;
            tail=j;
            pthread_cond_signal(&wake);
            pthread_mutex_unlock(&lock);
            return 0;
        }
        pthread_mutex_unlock(&lock);
        free(j);
    }
    int r=runtime_savecommit_run(host,temp,commit);
    free(temp); free(commit);
    return r;
}
static char fold(char c) {
    if (c==(char)92) return '/'; /* backslash */
#ifdef _WIN32
    if (c>='A' && c<='Z') return (char)(c+32);
#endif
    return c;
}
/* dir: path may also be a directory holding the file. */
static int covers(const char *path,const char *file,int dir) {
    size_t n=strlen(path);
    while (n>1 && fold(path[n-1])=='/') --n;
    if (!n) return 0;
    for (size_t i=0;i<n;++i) if (!file[i] || fold(file[i])!=fold(path[i])) return 0;
    if (!file[n]) return 1;
    return dir && fold(file[n])=='/';
}
static void wait_for(const char *host_path,int dir) {
    if (!host_path) return;
    pthread_mutex_lock(&lock);
    for (;;) {
        int busy=running && covers(host_path,running->commit,dir);
        for (Job *j=head; j && !busy; j=j->next) busy=covers(host_path,j->commit,dir);
        if (!busy) break;
        pthread_cond_wait(&idle,&lock);
    }
    pthread_mutex_unlock(&lock);
}
void runtime_savecommit_wait_file(const char *host_path) { wait_for(host_path,0); }
void runtime_savecommit_wait_dir(const char *host_path) { wait_for(host_path,1); }
void runtime_savecommit_flush(void) {
    pthread_mutex_lock(&lock);
    while (head || running) pthread_cond_wait(&idle,&lock);
    pthread_mutex_unlock(&lock);
}
int runtime_savecommit_pending(void) {
    pthread_mutex_lock(&lock);
    int n=running ? 1 : 0;
    for (Job *j=head; j; j=j->next) ++n;
    pthread_mutex_unlock(&lock);
    return n;
}
int runtime_savecommit_owns_temp(const char *temp) {
    pthread_mutex_lock(&lock);
    int own=running && !strcmp(running->temp,temp);
    for (Job *j=head; j && !own; j=j->next) own=!strcmp(j->temp,temp);
    pthread_mutex_unlock(&lock);
    return own;
}
