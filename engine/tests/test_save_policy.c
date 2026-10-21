/* Save policy (src/runtime_savepolicy.c): folder choice, save check, rotating backups. Temporary
 * folders only. ninja -C out/gpu save-policy-test && out/gpu/save-policy-test */
#include "save_policy.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
static void set_env(const char *k,const char *v) { _putenv_s(k,v ? v : ""); }
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p,0755)
static void set_env(const char *k,const char *v) { if (v) setenv(k,v,1); else unsetenv(k); }
#endif

static char user[512];
const char *runtime_file_user_dir(void) { return user; }
/* busy: the game writes saves during a backup's copy (every call moves the generation on). */
static uint64_t generation; static int busy, open_writes;
void runtime_file_save_activity(uint64_t *g,int *o) { if (busy) ++generation; *g=generation; *o=open_writes; }

#define SLOT 0x140000
#define SYSTEM 0x40000
static void mkdirs(const char *path) {
    char b[1024]; snprintf(b,sizeof(b),"%s",path);
    for (char *p=b+1;*p;++p) if (*p=='/' && p[-1]!=':') { *p=0; MKDIR(b); *p='/'; }
    MKDIR(b);
}
static void put(const char *rel,size_t size,int fill) {
    char path[1024]; snprintf(path,sizeof(path),"%s/%s",user,rel);
    char dir[1024]; snprintf(dir,sizeof(dir),"%s",path); *strrchr(dir,'/')=0; mkdirs(dir);
    unsigned char *b=calloc(1,size ? size : 1);
    if (fill) { memset(b,fill,size); b[0]=0x41; }
    FILE *f=fopen(path,"wb"); assert(f); fwrite(b,1,size,f); fclose(f); free(b);
}
#define SAVE "savedata_party/1/CUSA03173/SPRJ0005"
static void good_save(int fill) {
    put(SAVE "/userdata0000",SLOT,fill);
    put(SAVE "/userdata0001",SLOT,0);
    put(SAVE "/userdata0010",SYSTEM,3);
    put(SAVE "/backup0000",SLOT,fill);
    put("savedata_party/1/CUSA03173/SPRJ0005.sce_sys/param.bin",1328,1);
}
/* Entries of dir whose name contains needle (with=1) or not (with=0); .txt files ignored. */
static int count(const char *dir,const char *needle,int with) {
    int n=0; DIR *d=opendir(dir); if (!d) return 0;
    for (struct dirent *e; (e=readdir(d));) {
        if (e->d_name[0]=='.' || strstr(e->d_name,".txt")) continue;
        if ((strstr(e->d_name,needle)!=NULL)==with) ++n;
    }
    closedir(d); return n;
}
static void remove_tree(const char *path) {
    struct stat st;
    if (stat(path,&st)) return;
    if (!S_ISDIR(st.st_mode)) { remove(path); return; }
    DIR *d=opendir(path);
    for (struct dirent *e; d && (e=readdir(d));) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char child[1024]; snprintf(child,sizeof(child),"%s/%s",path,e->d_name); remove_tree(child);
    }
    if (d) closedir(d);
    rmdir(path);
}
/* The oldest suspect backup must be the one removed: same-second names sort by their number. */
static int has(const char *dir,const char *name) { char p[1024]; struct stat st; snprintf(p,sizeof(p),"%s/%s",dir,name); return !stat(p,&st); }
static void sleep_1s(void) {
#ifdef _WIN32
    Sleep(1100);
#else
    sleep(1);
#endif
}

int main(void) {
#ifdef _WIN32
    char tmp[MAX_PATH]; GetTempPathA(sizeof(tmp),tmp);
    snprintf(user,sizeof(user),"%sbbport-save-policy-%lu",tmp,(unsigned long)GetCurrentProcessId());
    for (char *p=user;*p;++p) if (*p=='\\') *p='/';
#else
    snprintf(user,sizeof(user),"/tmp/bbport-save-policy-%d",(int)getpid());
#endif
    mkdirs(user);
    printf("test folder: %s\n",user);

    /* Folder choice: every party session plays from the party saves (shared is ignored). */
    set_env("BB_PARTY",NULL); set_env("BB_PARTY_SAVE","separate"); runtime_savepolicy_reset();
    assert(!strcmp(runtime_savepolicy_dir(),"savedata"));
    set_env("BB_PARTY","host"); set_env("BB_PARTY_SAVE","shared"); runtime_savepolicy_reset();
    assert(!strcmp(runtime_savepolicy_dir(),"savedata_party"));
    set_env("BB_PARTY_SAVE",NULL); runtime_savepolicy_reset();
    assert(!strcmp(runtime_savepolicy_dir(),"savedata_party"));
    /* BB_PARTY off values (gpu/party_env.h): no party save folder, whatever BB_PARTY_SAVE says. */
    { const char *off[]={"0","off","OFF","false","no"," "};
      for (unsigned i=0;i<sizeof(off)/sizeof(off[0]);++i) {
          set_env("BB_PARTY",off[i]); set_env("BB_PARTY_SAVE","separate"); runtime_savepolicy_reset();
          assert(!strcmp(runtime_savepolicy_dir(),"savedata"));
      } }
    /* Any on value (a party code too) is a party run. */
    set_env("BB_PARTY","BBP1-ABCDEF"); set_env("BB_PARTY_SAVE","separate"); runtime_savepolicy_reset();
    assert(!strcmp(runtime_savepolicy_dir(),"savedata_party"));
    set_env("BB_PARTY","join"); set_env("BB_PARTY_SAVE","separate"); runtime_savepolicy_reset();
    assert(!strcmp(runtime_savepolicy_dir(),"savedata_party"));
    puts("PASS: save folder choice");

    /* The check. */
    char folder[700]; snprintf(folder,sizeof(folder),"%s/savedata_party",user);
    SavePolicyCheck c;
    good_save(7);
    assert(runtime_savepolicy_check(folder,NULL,2,&c)==0 && c.files==5 && c.slots_used==1 && !c.warnings);
    uint64_t h=c.hash;
    put(SAVE "/userdata0000",SLOT/2,7); /* truncated */
    assert(runtime_savepolicy_check(folder,NULL,2,&c)==1);
    put(SAVE "/userdata0000",SLOT,7);
    put(SAVE "/userdata0010",SYSTEM,0); /* zeroed game data */
    assert(runtime_savepolicy_check(folder,NULL,2,&c)==1);
    put(SAVE "/userdata0010",SYSTEM,3);
    put("savedata_party/1/CUSA03173/SPRJ0005.sce_sys/param.bin",10,1);
    assert(runtime_savepolicy_check(folder,NULL,2,&c)==1);
    good_save(7);
    assert(runtime_savepolicy_check(folder,NULL,0,&c)==0 && c.hash==h);
    puts("PASS: save check (sizes, zeroed game data, param.bin)");

    /* Backups: made, unchanged, changed; rotation keeps 3 good ones. */
    char base[800], made[900], good[900];
    snprintf(base,sizeof(base),"%s/save_backups/savedata_party",user);
    assert(runtime_savepolicy_backup(user,"savedata_party","start",3,made,sizeof(made))==1);
    assert(strstr(made,"-start") && !strstr(made,"suspect"));
    assert(runtime_savepolicy_check(made,NULL,0,&c)==0 && c.hash==h); /* the copy is the save */
    char readme[900]; snprintf(readme,sizeof(readme),"%s/save_backups/README.txt",user);
    struct stat st; assert(!stat(readme,&st) && st.st_size>200);
    assert(runtime_savepolicy_backup(user,"savedata_party","timer",3,NULL,0)==0);
    for (int i=0;i<4;++i) {
        good_save(8+i);
        assert(runtime_savepolicy_backup(user,"savedata_party","timer",3,NULL,0)==1);
    }
    assert(count(base,"suspect",1)==0 && count(base,"suspect",0)==3);
    puts("PASS: backups (unchanged skipped, newest 3 kept)");

    /* Compared with the newest good backup: an emptied slot, a lost file. */
    assert(!runtime_savepolicy_newest_good(user,"savedata_party",good,sizeof(good)));
    put(SAVE "/userdata0000",SLOT,0);
    assert(runtime_savepolicy_check(folder,good,2,&c)==0 && c.warnings==1 && c.slots_used==0);
    char lost[900]; snprintf(lost,sizeof(lost),"%s/" SAVE "/userdata0001",user); remove(lost);
    assert(runtime_savepolicy_check(folder,good,2,&c)==0 && c.warnings==2);
    good_save(11);
    puts("PASS: comparison with the newest good backup");

    /* A damaged save (a crash): kept as -suspect, never pushes the good ones out. */
    sleep_1s();
    put(SAVE "/userdata0000",1000,7);
    for (int i=0;i<5;++i) {
        assert(runtime_savepolicy_backup(user,"savedata_party","restart",3,made,sizeof(made))==1);
        assert(strstr(made,"-suspect"));
    }
    assert(count(base,"suspect",1)==3 && count(base,"suspect",0)==3);
    { char *slash=strrchr(made,'/'); assert(has(base,slash+1)); } /* the newest stays */
    char still[900];
    assert(!runtime_savepolicy_newest_good(user,"savedata_party",still,sizeof(still)) && !strcmp(still,good));
    puts("PASS: damaged saves kept as -suspect, good backups untouched");

    /* The game saves during the copy: nothing kept. A crash-interrupted copy is cleaned up. */
    good_save(20);
    busy=1;
    assert(runtime_savepolicy_backup(user,"savedata_party","timer",3,NULL,0)==-2);
    busy=0; open_writes=1;
    assert(runtime_savepolicy_backup(user,"savedata_party","timer",3,NULL,0)==-2);
    open_writes=0;
    assert(count(base,".partial",1)==0);
    char partial[900]; snprintf(partial,sizeof(partial),"%s/20000101-000000-timer.partial",base); mkdirs(partial);
    assert(runtime_savepolicy_backup(user,"savedata_party","timer",3,NULL,0)==1);
    assert(count(base,".partial",1)==0 && count(base,"suspect",0)==3);
    puts("PASS: busy saves skipped, unfinished copies removed");

    /* Party session snapshots: both folders, byte-identical, labelled with the role; newest N kept;
     * made at a party start, not at a crash restart (the same session). */
    {
        char sess[800], made2[900], a[1200], b[1200];
        snprintf(sess,sizeof(sess),"%s/save_backups/party_sessions",user);
        put("savedata/1/CUSA03173/SPRJ0005/userdata0000",SLOT,5);
        assert(runtime_savepolicy_session_snapshot(user,"host",2,made2,sizeof(made2))==1);
        assert(strstr(made2,"-host"));
        snprintf(a,sizeof(a),"%s/savedata_party",made2); snprintf(b,sizeof(b),"%s/savedata_party",user);
        SavePolicyCheck x, y;
        runtime_savepolicy_check(a,NULL,0,&x); runtime_savepolicy_check(b,NULL,0,&y);
        assert(x.hash==y.hash && x.files==y.files);
        snprintf(a,sizeof(a),"%s/savedata",made2); snprintf(b,sizeof(b),"%s/savedata",user);
        runtime_savepolicy_check(a,NULL,0,&x); runtime_savepolicy_check(b,NULL,0,&y);
        assert(x.hash==y.hash && x.files==1);
        snprintf(a,sizeof(a),"%s/SESSION.txt",made2);
        FILE *f=fopen(a,"rb"); assert(f); char txt[1024]={0}; fread(txt,1,sizeof(txt)-1,f); fclose(f);
        assert(strstr(txt,"role=host") && strstr(txt,"savedata_party_files=5") && strstr(txt,"savedata_files=1"));
        assert(runtime_savepolicy_session_snapshot(user,"guest",2,NULL,0)==1);
        assert(runtime_savepolicy_session_snapshot(user,"guest",2,NULL,0)==1);
        assert(count(sess,"-",1)==2 && count(sess,"-host",1)==0); /* the oldest went */
        set_env("BB_SAVE_BACKUP_MINUTES","0"); set_env("BB_SAVE_SESSION_KEEP","9");
        set_env("BB_PARTY","join"); set_env("BB_PARTY_RESTARTED","1"); runtime_savepolicy_reset();
        runtime_savepolicy_start();
        assert(count(sess,"-",1)==2); /* crash restart: no new snapshot */
        set_env("BB_PARTY_RESTARTED",NULL); runtime_savepolicy_reset();
        runtime_savepolicy_start();
        assert(count(sess,"-",1)==3 && count(sess,".partial",1)==0);
        puts("PASS: party session snapshots");
    }

    /* Saves from other builds: param.sfo both ways. */
    {
        SaveSfo in={0}, back;
        snprintf(in.title,sizeof(in.title),"Bloodborne SaveData"); snprintf(in.detail,sizeof(in.detail),"Hunter's Dream");
        in.user_param=7;
        unsigned char sfo[4096];
        size_t n=runtime_savepolicy_sfo_build(&in,"SPRJ0005","CUSA00207",sfo,sizeof(sfo));
        assert(n>20 && !memcmp(sfo,"\0PSF",4) && sfo[4]==1 && sfo[5]==1);
        assert(!runtime_savepolicy_sfo_parse(sfo,n,&back));
        assert(!strcmp(back.title,"Bloodborne SaveData") && !strcmp(back.detail,"Hunter's Dream") && !back.subtitle[0] && back.user_param==7);
        assert(runtime_savepolicy_sfo_build(&in,"SPRJ0005","CUSA00207",sfo,64)==0);
        assert(runtime_savepolicy_sfo_parse(sfo,10,&back)==-1 && runtime_savepolicy_sfo_parse("PSF\0xxxxxxxxxxxxxxxxxxxxx",24,&back)==-1);
        /* A truncated file: entries pointing past the end are skipped, never read. */
        assert(!runtime_savepolicy_sfo_parse(sfo,n-1100,&back));
        assert(runtime_savepolicy_bloodborne_title("CUSA03173") && runtime_savepolicy_bloodborne_title("CUSA00900") &&
               !runtime_savepolicy_bloodborne_title("CUSA12345") && !runtime_savepolicy_bloodborne_title(NULL));

        /* shadPS4's layout (sce_sys/param.sfo inside the folder, no .sce_sys beside it): param.bin made from it. */
        remove_tree(user); mkdirs(user);
        put("savedata/1/CUSA00207/SPRJ0005/userdata0000",SLOT,5);
        put("savedata/1/CUSA00207/SPRJ0005/userdata0010",SYSTEM,3);
        { char p[1024]; snprintf(p,sizeof(p),"%s/savedata/1/CUSA00207/SPRJ0005/sce_sys",user); mkdirs(p);
          strcat(p,"/param.sfo"); FILE *f=fopen(p,"wb"); fwrite(sfo,1,n,f); fclose(f); }
        put("savedata/1/CUSA00207/SPRJ0005/sce_sys/icon0.png",100,9);
        assert(runtime_savepolicy_adopt(user,"savedata",1,"CUSA00207")==1);
        char bin[1024]; snprintf(bin,sizeof(bin),"%s/savedata/1/CUSA00207/SPRJ0005.sce_sys",user);
        assert(has(bin,"param.bin") && has(bin,"icon0.png"));
        { char p[1100]; snprintf(p,sizeof(p),"%s/param.bin",bin); FILE *f=fopen(p,"rb"); unsigned char b[1328];
          assert(f && fread(b,1,sizeof(b),f)==1328); fclose(f);
          assert(!strcmp((char *)b,"Bloodborne SaveData") && !strcmp((char *)b+256,"Hunter's Dream") && b[1280]==7); }
        assert(runtime_savepolicy_adopt(user,"savedata",1,"CUSA00207")==0); /* once */
        SavePolicyCheck c;
        snprintf(bin,sizeof(bin),"%s/savedata",user);
        assert(runtime_savepolicy_check(bin,NULL,2,&c)==0 && c.slots_used==1); /* sce_sys inside: no error */

        /* Another region's title id: copied (kept) into the folder this game reads; the newest wins. */
        remove_tree(user); mkdirs(user);
        put("savedata/1/CUSA00207/SPRJ0005/userdata0000",SLOT,5);
        put("savedata/1/CUSA00207/SPRJ0005/userdata0010",SYSTEM,3);
        sleep_1s();
        put("savedata/1/CUSA00900/SPRJ0005/userdata0000",SLOT,6);
        put("savedata/1/CUSA00900/SPRJ0005/userdata0010",SYSTEM,3);
        put("savedata/1/CUSA00900/SPRJ0005.sce_sys/param.bin",1328,1);
        put("savedata/1/CUSA99999/OTHER/userdata0000",SLOT,7); /* another game: never taken */
        assert(runtime_savepolicy_adopt(user,"savedata",1,"CUSA03173")==1);
        char got[1024]; snprintf(got,sizeof(got),"%s/savedata/1/CUSA03173",user);
        assert(has(got,"SPRJ0005/userdata0000") && has(got,"SPRJ0005.sce_sys/param.bin"));
        { char p[1100]; snprintf(p,sizeof(p),"%s/SPRJ0005/userdata0000",got); FILE *f=fopen(p,"rb"); unsigned char b[2];
          assert(f && fread(b,1,2,f)==2 && b[1]==6); fclose(f); } /* CUSA00900's: the newer one */
        snprintf(got,sizeof(got),"%s/savedata/1/CUSA00207",user);
        assert(has(got,"SPRJ0005/userdata0000")); /* the original stays */
        assert(runtime_savepolicy_adopt(user,"savedata",1,"CUSA03173")==0); /* it has a save now: nothing more */

        /* Another user id (another build's user folder), in the party folder. */
        remove_tree(user); mkdirs(user);
        put("savedata_party/1000/CUSA00207/SPRJ0005/userdata0000",SLOT,5);
        assert(runtime_savepolicy_adopt(user,"savedata_party",1,"CUSA00207")==1);
        snprintf(got,sizeof(got),"%s/savedata_party/1/CUSA00207",user);
        assert(has(got,"SPRJ0005/userdata0000"));

        /* Our own save: left alone; nothing found anywhere: nothing done. */
        assert(runtime_savepolicy_adopt(user,"savedata_party",1,"CUSA00207")==0);
        assert(runtime_savepolicy_adopt(user,"savedata_none",1,"CUSA00207")==0);
        remove_tree(user); mkdirs(user);
        puts("PASS: saves from other builds");
    }

    /* Party side files: a slot that holds another character than last time drops the old one's entries. */
    {
        remove_tree(user); mkdirs(user);
        char folder[1024]; snprintf(folder,sizeof(folder),"%s/savedata_party/1/CUSA00207/SPRJ0005",user);
        mkdirs(folder);
        /* a character: play time at +8, a UTF-16 name after the header */
        #define CHARACTER(slot,name,play) do { unsigned char *b=calloc(1,SLOT); b[0]=0x41; unsigned p=(play); memcpy(b+8,&p,4); \
            const char *nm=(name); for (int i=0;nm[i];++i) b[0x100+2*i]=(unsigned char)nm[i]; \
            char pth[1100]; snprintf(pth,sizeof(pth),"%s/userdata%04d",folder,(slot)); FILE *f=fopen(pth,"wb"); \
            fwrite(b,1,SLOT,f); fclose(f); free(b); } while (0)
        CHARACTER(0,"Alfred",3600000); CHARACTER(3,"Eileen",7200000);
        assert(runtime_savepolicy_side_files(user,folder)==0); /* first start: only remembered */
        assert(has(user,"party_characters.txt"));
        put("party_rot.txt",0,0);
        { char p[1100]; snprintf(p,sizeof(p),"%s/party_rot.txt",user); FILE *f=fopen(p,"wb");
          fputs("slot 0 kinds 1 0 0 0 0\nslot 3 kinds 2 0 0 0 0\n",f); fclose(f);
          snprintf(p,sizeof(p),"%s/party_stain.txt",user); f=fopen(p,"wb"); fputs("0 385875968 1.0 2.0 3.0 500 abcd\n",f); fclose(f);
          snprintf(p,sizeof(p),"%s/party_prologue.txt",user); f=fopen(p,"wb"); fputs("last 0\nslot 0 1\nslot 3 0\n",f); fclose(f);
          snprintf(p,sizeof(p),"%s/party_gestures.txt",user); f=fopen(p,"wb"); fputs("owed 0 805 1\nowed 3 808 0\n",f); fclose(f);
          snprintf(p,sizeof(p),"%s/party_owed_lots.txt",user); f=fopen(p,"wb"); fputs("12345 0\n555 3\n",f); fclose(f);
          snprintf(p,sizeof(p),"%s/party_summoned_gives.txt",user); f=fopen(p,"wb"); fputs("1 4001 1 1 0\n",f); fclose(f);
          snprintf(p,sizeof(p),"%s/party_story.json",user); f=fopen(p,"wb"); fputs("{}",f); fclose(f); }
        /* the same characters, played on: nothing dropped */
        CHARACTER(0,"Alfred",3700000);
        assert(runtime_savepolicy_side_files(user,folder)==0 && has(user,"party_story.json"));
        /* another save copied over the party save: slot 0 holds another character, slot 3 is empty */
        CHARACTER(0,"Djura",9000000);
        { char p[1100]; snprintf(p,sizeof(p),"%s/userdata0003",folder); remove(p); }
        assert(runtime_savepolicy_side_files(user,folder)==((1<<0)|(1<<3)));
        char text[2048]; FILE *f; char p[1100];
        snprintf(p,sizeof(p),"%s/party_prologue.txt",user); f=fopen(p,"rb"); size_t n=fread(text,1,sizeof(text)-1,f); text[n]=0; fclose(f);
        assert(!strcmp(text,"last 0\n"));
        assert(!has(user,"party_rot.txt") && !has(user,"party_stain.txt") && !has(user,"party_gestures.txt") &&
               !has(user,"party_owed_lots.txt") && !has(user,"party_summoned_gives.txt") && !has(user,"party_story.json"));
        /* an older save of the same character (play time back): dropped too; then remembered as it is */
        CHARACTER(0,"Djura",9500000); assert(runtime_savepolicy_side_files(user,folder)==0);
        snprintf(p,sizeof(p),"%s/party_rot.txt",user); f=fopen(p,"wb"); fputs("slot 0 kinds 1 0 0 0 0\nslot 5 kinds 1 0 0 0 0\n",f); fclose(f);
        CHARACTER(0,"Djura",1000000);
        assert(runtime_savepolicy_side_files(user,folder)==1);
        f=fopen(p,"rb"); n=fread(text,1,sizeof(text)-1,f); text[n]=0; fclose(f);
        assert(!strcmp(text,"slot 5 kinds 1 0 0 0 0\n"));
        assert(runtime_savepolicy_side_files(user,folder)==0);
        #undef CHARACTER
        remove_tree(user); mkdirs(user);
        puts("PASS: party side files of a replaced character");
    }

    /* Nothing to back up. */
    assert(runtime_savepolicy_backup(user,"savedata_none","start",3,NULL,0)==0);
    remove_tree(user);
    puts("PASS: all save policy tests");
    return 0;
}
