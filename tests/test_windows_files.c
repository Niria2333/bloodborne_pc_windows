// Windows port modifications by yaonikaixin999999, 2026-10-05.
// SPDX-License-Identifier: GPL-2.0-or-later
/* Exercise native Windows guest file reads, offsets, directories and writable saves. */
#include "../src/runtime_file.c"
#include <assert.h>
static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
int main(void) {
    char temp[MAX_PATH],root[MAX_PATH];
    assert(GetTempPathA(MAX_PATH,temp));
    assert(GetTempFileNameA(temp,"bbp",0,root));
    assert(DeleteFileA(root) && CreateDirectoryA(root,NULL));
    char game[512],user[512],asset[512];
    snprintf(game,sizeof(game),"%s/game",root); assert(!mkdir(game,0755));
    snprintf(user,sizeof(user),"%s/user",root);
    snprintf(asset,sizeof(asset),"%s/asset.dcx",game);
    const unsigned char bytes[]={0,10,13,26,255,42};
    FILE *f=fopen(asset,"wb"); assert(f && fwrite(bytes,1,sizeof(bytes),f)==sizeof(bytes)); assert(!fclose(f));
    runtime_file_configure(game,user);
    int fd=(int)do_open("/app0/asset.dcx",0,0); assert(fd>=3);
    unsigned char output[8]={0};
    assert(do_read(fd,output,sizeof(bytes))==sizeof(bytes) && !memcmp(output,bytes,sizeof(bytes)));
    assert(do_lseek(fd,2,SEEK_SET)==2);
    assert(do_pread(fd,output,2,0)==2 && !memcmp(output,bytes,2));
    assert(do_lseek(fd,0,SEEK_CUR)==2);
    assert(do_read(fd,output,2)==2 && !memcmp(output,bytes+2,2));
    GuestStat info; assert(!do_fstat(fd,&info) && info.size==sizeof(bytes));
    assert(!do_close(fd));
    assert(do_open("/app0/asset.dcx",2,0)==-EROFS);
    char translated[512];
    assert(translate("/app0/../asset.dcx",translated,sizeof(translated))==EACCES);
    assert(translate("/app0/..\\asset.dcx",translated,sizeof(translated))==EACCES);
    int dir=(int)do_open("/app0",0x20000,0); assert(dir>=3);
    assert(!do_fstat(dir,&info) && S_ISDIR(info.mode));
    char entries[2048]; int64_t n=do_getdents(dir,entries,sizeof(entries),NULL); assert(n>0);
    int found=0;
    for(int64_t p=0;p<n;) {
        uint16_t length; memcpy(&length,entries+p+4,2); assert(length);
        if(!strcmp(entries+p+8,"asset.dcx")){assert(entries[p+6]==8);found=1;}
        p+=length;
    }
    assert(found && !do_close(dir));
    int save=(int)do_open("/data/save",0x202,0644); assert(save>=3);
    assert(do_write(save,bytes,sizeof(bytes))==sizeof(bytes));
    assert(!do_ftruncate(save,2) && !do_fstat(save,&info) && info.size==2);
    assert(!do_fsync(save) && !do_close(save));
    assert(!path_op("/data/save",2,0));
    assert(!unlink(asset) && !rmdir(game));
    const char *dirs[]={"temp0","download0","data"};
    for(int i=0;i<3;++i){char p[1024];snprintf(p,sizeof(p),"%s/%s",user,dirs[i]);assert(!rmdir(p));}
    assert(!rmdir(user) && !rmdir(root));
    puts("Windows guest binary files, offsets, directory listing and saves PASS");
    return 0;
}
