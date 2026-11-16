/* A thread reading with lseek + read and others reading the same descriptor with pread: on
 * Windows pread moves and restores the file offset, so without the per-descriptor lock the first
 * thread reads from the wrong place. Credit: Jgamrot/yharnam-pc 1e6fb99 (GPL-2.0-or-later). */
#include "../src/runtime_file.c"
#include <assert.h>
#include <time.h>
#include <pthread.h>
static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
void runtime_memory_note_write(uintptr_t address, uint64_t size) { (void)address; (void)size; }
uint64_t host_monotonic_ns(void) { return 0; }
/* Nightwatch: the rest of our runtime_file.c's links. */
void bbnet_configure(const char *a, const char *b) { (void)a; (void)b; }
int runtime_memory_guest_writable(uintptr_t a, uint64_t s) { (void)a; (void)s; return 1; }
int runtime_thread_is_loader(void) { return 0; }
int runtime_savepolicy_world_fix_file(const char *t, const char *g) { (void)t; (void)g; return 0; }
int host_thread_start(HostThread *t, size_t s, void *(*e)(void *), void *a) { (void)t; (void)s; (void)e; (void)a; return -1; }
void host_thread_detach(HostThread t) { (void)t; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
#define WORDS (1u<<18) /* 1 MiB: word i holds i */
#define CHUNK 1024
static int fd;
static volatile int stop;
static unsigned bad_seek, bad_pread;
static unsigned next(unsigned *s) { *s=*s*1103515245u+12345u; return (*s>>8)%(WORDS-CHUNK); }
static int check(const uint32_t *b,unsigned at) {
    for (unsigned i=0;i<CHUNK;++i) if (b[i]!=at+i) return 0;
    return 1;
}
static void *seek_reader(void *arg) {
    unsigned s=(unsigned)(uintptr_t)arg; uint32_t b[CHUNK];
    while (!stop) {
        unsigned at=next(&s);
        int64_t r=do_lseek(fd,(int64_t)at*4,0);
        assert(r==(int64_t)at*4);
        /* the offset must still be ours when we read: only pread of others may sit in between */
        if (do_read(fd,b,sizeof(b))!=(int64_t)sizeof(b) || !check(b,at)) ++bad_seek;
    }
    return NULL;
}
static void *pread_reader(void *arg) {
    unsigned s=(unsigned)(uintptr_t)arg; uint32_t b[CHUNK];
    while (!stop) {
        unsigned at=next(&s);
        if (do_pread(fd,b,sizeof(b),(int64_t)at*4)!=(int64_t)sizeof(b) || !check(b,at)) __atomic_add_fetch(&bad_pread,1,__ATOMIC_RELAXED);
    }
    return NULL;
}
int main(void) {
    char temp_dir[MAX_PATH], root[512], game[600], user[600], data[700];
    GetTempPathA(sizeof(temp_dir), temp_dir);
    snprintf(root,sizeof(root),"%sbbport-offsets-%lu",temp_dir,(unsigned long)GetCurrentProcessId());
    snprintf(game,sizeof(game),"%s/game",root); snprintf(user,sizeof(user),"%s/user",root);
    snprintf(data,sizeof(data),"%s/data.bin",game);
    mkdir(root,0755); mkdir(game,0755);
    FILE *f=fopen(data,"wb"); assert(f);
    for (uint32_t i=0;i<WORDS;++i) assert(fwrite(&i,4,1,f)==1);
    assert(!fclose(f));
    runtime_file_configure(game,user);
    fd=(int)do_open("/app0/data.bin",0,0); assert(fd>=3);
    pthread_t a,b,c;
    pthread_create(&a,NULL,seek_reader,(void *)1);
    pthread_create(&b,NULL,pread_reader,(void *)2);
    pthread_create(&c,NULL,pread_reader,(void *)3);
    Sleep(2000);
    stop=1;
    pthread_join(a,NULL); pthread_join(b,NULL); pthread_join(c,NULL);
    assert(!do_close(fd));
    printf("lseek+read wrong data: %u, pread wrong data: %u\n",bad_seek,bad_pread);
    remove(data); rmdir(game);
    assert(!bad_seek && !bad_pread);
    puts("file offset test passed");
    return 0;
}
