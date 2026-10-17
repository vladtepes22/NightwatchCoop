/* Real Windows save writes: commit, existing readers, crash remnants and backup activity. */
#include "../src/runtime_file.c"
#include <assert.h>

static int32_t guest_errno;
int runtime_memory_guest_writable(uintptr_t address,uint64_t size) { (void)address; (void)size; return 1; }
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int error) { return error; }
void runtime_memory_note_write(uintptr_t address,uint64_t size) { (void)address; (void)size; }
void bbnet_configure(const char *app0,const char *user) { (void)app0; (void)user; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t size,const char *name) {
    for (size_t i=0;i<size;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static void expect_file(const char *path,const char *expected) {
    char value[64]={0}; FILE *file=fopen(path,"rb"); assert(file);
    fread(value,1,sizeof(value)-1,file); fclose(file); assert(!strcmp(value,expected));
}
int main(int argc,char **argv) {
    char root[512],save[600];
    if (argc==2) {
        runtime_file_configure(argv[1],argv[1]);
        assert(!runtime_file_mount("/savedata0",argv[1]));
        int fd=(int)do_open("/savedata0/slot",2,0); assert(fd>=3);
        assert(do_pwrite(fd,"partial",7,0)==7);
        _exit(0); /* Skip the guest close and simulate process termination during a save. */
    }
    char temp[MAX_PATH]; assert(GetTempPathA(sizeof(temp),temp));
    snprintf(root,sizeof(root),"%sbb-atomic-save-%lu",temp,(unsigned long)GetCurrentProcessId());
    assert(!mkdir(root,0755));
    snprintf(save,sizeof(save),"%s/slot",root);
    FILE *file=fopen(save,"wb"); assert(file); fputs("original",file); fclose(file);
    runtime_file_configure(root,root);
    assert(!runtime_file_mount("/savedata0",root));
    int reader=(int)do_open("/savedata0/slot",0,0); assert(reader>=3);
    int writer=(int)do_open("/savedata0/slot",2,0); assert(writer>=3);
    uint64_t generation; int open_writes;
    runtime_file_save_activity(&generation,&open_writes); assert(open_writes==1);
    assert(do_pwrite(writer,"complete",8,0)==8);
    expect_file(save,"original");
    assert(!do_close(writer));
    expect_file(save,"complete");
    char old[16]={0}; assert(do_read(reader,old,8)==8 && !strcmp(old,"original"));
    assert(!do_close(reader));
    runtime_file_save_activity(&generation,&open_writes); assert(open_writes==0);
    writer=(int)do_open("/savedata0/slot",2,0); assert(writer>=3);
    assert(!do_close(writer)); expect_file(save,"complete");
    char exe[MAX_PATH],command[1300]; assert(GetModuleFileNameA(NULL,exe,sizeof(exe)));
    snprintf(command,sizeof(command),"\"%s\" \"%s\"",exe,root);
    STARTUPINFOA startup={.cb=sizeof(startup)}; PROCESS_INFORMATION child;
    assert(CreateProcessA(NULL,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startup,&child));
    assert(WaitForSingleObject(child.hProcess,10000)==WAIT_OBJECT_0);
    DWORD status; assert(GetExitCodeProcess(child.hProcess,&status) && !status);
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    expect_file(save,"complete");
    int remnants=0; DIR *dir=opendir(root); assert(dir);
    for (struct dirent *entry;(entry=readdir(dir));) remnants+=temp_name(entry->d_name);
    closedir(dir); assert(remnants==1);
    runtime_file_unmount("/savedata0");
    assert(!runtime_file_mount("/savedata0",root));
    dir=opendir(root); assert(dir);
    for (struct dirent *entry;(entry=readdir(dir));) assert(!temp_name(entry->d_name));
    closedir(dir); expect_file(save,"complete");
    writer=(int)do_open("/savedata0/slot",2|0x400,0); assert(writer>=3);
    assert(!do_close(writer)); expect_file(save,"");
    writer=(int)do_open("/savedata0/new-empty",2|0x200,0644); assert(writer>=3);
    assert(!do_close(writer));
    char created[600]; snprintf(created,sizeof(created),"%s/new-empty",root); expect_file(created,"");
    assert(!runtime_win_remove_tree(root));
    puts("PASS: atomic save commit, existing reader, interrupted write, stale cleanup and backup activity");
}
