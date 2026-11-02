/* Save commit (src/runtime_savecommit.c): the steps run in a background thread; ordering and
 * what a kill leaves behind. Temporary folders only.
 *   savecommit-test DIR                   in-process cases
 *   savecommit-test --child STAGE DIR     commits DIR/file.dat over its old content and stops itself at
 *                                         STAGE (before-rename | after-rename) for the test to kill it
 *   savecommit-test --check FILE          prints the byte value of a whole file, 0 missing, -1 torn */
#include "runtime_savecommit.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#define MSLEEP(ms) Sleep(ms)
#define MKDIR(p) _mkdir(p)
#define SETENV(v) _putenv("BB_SAVE_COMMIT_ASYNC=" v)
#define OPEN_FLAGS (O_RDWR|O_BINARY)
#else
#include <unistd.h>
#define MSLEEP(ms) usleep((ms)*1000)
#define MKDIR(p) mkdir(p,0755)
#define SETENV(v) ((v)[0] ? setenv("BB_SAVE_COMMIT_ASYNC",v,1) : unsetenv("BB_SAVE_COMMIT_ASYNC"))
#define OPEN_FLAGS O_RDWR
#endif

#define SIZE (1<<20)
static char dir[512];
static int fix_delay_ms, fixes;
static const char *stop_stage;
static void hold(const char *stage) {
    if (stop_stage && !strcmp(stop_stage,stage)) {
        char marker[600]; snprintf(marker,sizeof(marker),"%s/reached",dir);
        FILE *f=fopen(marker,"wb"); if (f) { fputs(stage,f); fclose(f); }
        MSLEEP(60000); /* killed meanwhile */
    }
}
static int fix(const char *temp,const char *commit) {
    (void)temp; (void)commit;
    ++fixes;
    if (fix_delay_ms) MSLEEP(fix_delay_ms);
    hold("before-rename");
    return 0;
}
static void note(const char *what,const char *path,uint64_t t0,uint64_t t1) {
    (void)path; (void)t0; (void)t1;
    if (!strcmp(what,"commit: rename")) hold("after-rename");
}
static uint64_t now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}
static void fill(const char *path,unsigned char v) {
    unsigned char *b=malloc(SIZE); memset(b,v,SIZE);
    FILE *f=fopen(path,"wb"); assert(f); assert(fwrite(b,1,SIZE,f)==SIZE); fclose(f); free(b);
}
static int content(const char *path) {
    FILE *f=fopen(path,"rb"); if (!f) return 0;
    unsigned char *b=malloc(SIZE+1); size_t n=fread(b,1,SIZE+1,f); fclose(f);
    int r=n==SIZE ? b[0] : -1;
    for (size_t i=0;i<n && r>0;++i) if (b[i]!=b[0]) r=-1;
    free(b); return r;
}
static void setup(const char *where) {
    snprintf(dir,sizeof(dir),"%s",where);
    MKDIR(dir);
    runtime_savecommit_configure(&(SaveCommitOps){.fix=fix,.note=note,.now=now});
}
static int submit(const char *name,unsigned char v) {
    char temp[600],commit[600];
    snprintf(temp,sizeof(temp),"%s/%s.%u.bbtmp",dir,name,(unsigned)v);
    snprintf(commit,sizeof(commit),"%s/%s",dir,name);
    fill(temp,v);
    int fd=open(temp,OPEN_FLAGS);
    assert(fd>=0);
    return runtime_savecommit_submit(fd,strdup(temp),strdup(commit));
}
static int child(const char *stage,const char *where) {
    stop_stage=stage; setup(where);
    char path[600]; snprintf(path,sizeof(path),"%s/file.dat",dir);
    fill(path,0xA1);                         /* the old save */
    assert(submit("file.dat",0xB2)==0);
    runtime_savecommit_flush();              /* does not return while stopped */
    return 0;
}
static void inprocess(const char *where) {
    setup(where);
    char path[600]; snprintf(path,sizeof(path),"%s/file.dat",dir);
    char other[600]; snprintf(other,sizeof(other),"%s/other.dat",dir);
    fill(path,0xA1);
    /* 1: submit returns before the slow steps are done; a reader of that path waits and sees the new file whole. */
    fix_delay_ms=400;
    uint64_t t0=now();
    assert(submit("file.dat",0xB2)==0);
    uint64_t submitted=now()-t0;
    assert(submitted<200000000ull);          /* the caller is not held for the 400 ms */
    assert(runtime_savecommit_pending()==1);
    assert(content(path)==0xA1);             /* still the old file, whole, until the rename */
    runtime_savecommit_wait_file(path);
    assert(now()-t0>=380000000ull);
    assert(content(path)==0xB2 && runtime_savecommit_pending()==0);
    /* 2: a path that is not being committed does not wait; a directory holding the file does. */
    assert(submit("file.dat",0xC3)==0);
    t0=now(); runtime_savecommit_wait_file(other);
    assert(now()-t0<100000000ull);
    runtime_savecommit_wait_dir(dir);
    assert(content(path)==0xC3);
    /* 3: commits of one file apply in the order they were closed. */
    fix_delay_ms=30;
    for (int v=1;v<=8;++v) assert(submit("file.dat",(unsigned char)(0x10+v))==0);
    runtime_savecommit_wait_file(path);
    assert(content(path)==0x18);
    /* 4: nothing is left behind by finished commits. */
    runtime_savecommit_flush();
    assert(runtime_savecommit_pending()==0);
    char temp[600]; snprintf(temp,sizeof(temp),"%s/file.dat.%u.bbtmp",dir,0x18u);
    assert(content(temp)==0);
    printf("savecommit-test ok (fix steps %d; the close returned in %.2f ms while its steps took 400 ms)\n",fixes,(double)submitted/1e6);
}
/* Default (BB_SAVE_COMMIT_ASYNC unset): submit does the steps inline, nothing is queued. */
static void inline_default(const char *where) {
    setup(where);
    char path[600]; snprintf(path,sizeof(path),"%s/file.dat",dir);
    fill(path,0xA1);
    assert(!runtime_savecommit_enabled());
    assert(submit("file.dat",0xB2)==0);
    assert(runtime_savecommit_pending()==0 && content(path)==0xB2);
    printf("savecommit-test inline ok\n");
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--ini-on")) { SETENV(""); return runtime_savecommit_enabled() ? 0 : 1; } /* BB_CONFIG from the caller */
    if (argc==3 && !strcmp(argv[1],"--inline")) { SETENV(""); inline_default(argv[2]); return 0; }
    if (!getenv("BB_SAVE_COMMIT_ASYNC")) SETENV("1"); /* the cases below are the background path */
    if (argc==4 && !strcmp(argv[1],"--child")) return child(argv[2],argv[3]);
    if (argc==3 && !strcmp(argv[1],"--check")) { printf("%d\n",content(argv[2])); return 0; }
    if (argc!=2) { fprintf(stderr,"usage: savecommit-test DIR | --child STAGE DIR | --check FILE\n"); return 2; }
    inprocess(argv[1]);
    return 0;
}
