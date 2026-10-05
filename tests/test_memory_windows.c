// Windows port modifications by yaonikaixin999999, 2026-10-05.
// SPDX-License-Identifier: GPL-2.0-or-later
/* Standalone native-memory tests; no game data or GPU driver is required.
 * gcc -std=gnu11 -O2 -Isrc tests/test_memory_windows.c src/runtime_memory.c
 *     -lwinpthread -lpsapi -o test_memory_windows.exe */
#include "runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>

uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
typedef int32_t (ABI *Allocate)(int64_t,int64_t,uint64_t,uint64_t,int,int64_t *);
typedef int32_t (ABI *Map)(void **,uint64_t,int,int,int64_t,uint64_t);
typedef int32_t (ABI *Unmap)(void *,uint64_t);
typedef int32_t (ABI *Release)(uint64_t,uint64_t);
typedef int32_t (ABI *Reserve)(void **,uint64_t,int,uint64_t);
typedef int32_t (ABI *Protect)(const void *,uint64_t,int);
typedef int32_t (ABI *Flexible)(void **,uint64_t,int,int);
typedef int32_t (ABI *Query)(void *,void **,void **,uint32_t *);
#define GET(type,name) ((type)runtime_memory_resolve(name))
#define PAGE_BYTES UINT64_C(16384)
extern int runtime_memory_write_backing(uintptr_t,const void *,uint64_t);
extern uint64_t runtime_memory_clamp(uintptr_t,uint64_t);
extern int runtime_memory_region(uintptr_t,uintptr_t *,uintptr_t *,int *);
extern void runtime_memory_gpu_protect(uintptr_t,uint64_t,int,int);
static DWORD protection(void *address) {
    MEMORY_BASIC_INFORMATION info;
    assert(VirtualQuery(address,&info,sizeof(info)));
    return info.Protect;
}
int main(void) {
    Allocate alloc=GET(Allocate,"sceKernelAllocateDirectMemory");
    Map map=GET(Map,"sceKernelMapDirectMemory");
    Unmap unmap=GET(Unmap,"sceKernelMunmap");
    Release release=GET(Release,"sceKernelReleaseDirectMemory");
    Reserve reserve=GET(Reserve,"sceKernelReserveVirtualRange");
    Protect protect=GET(Protect,"sceKernelMprotect");
    Flexible flexible=GET(Flexible,"sceKernelMapFlexibleMemory");
    Query query=GET(Query,"sceKernelQueryMemoryProtection");
    assert(alloc && map && unmap && release && reserve && protect && flexible && query);

    /* A multi-GiB physical allocation must leave its unused backing sparse. */
    PERFORMANCE_INFORMATION before={.cb=sizeof(before)},after={.cb=sizeof(after)};
    assert(GetPerformanceInfo(&before,sizeof(before)));
    int64_t large=-1;
    assert(!alloc(0,UINT64_C(5056)*1024*1024,UINT64_C(4)*1024*1024*1024,PAGE_BYTES,0,&large));
    assert(GetPerformanceInfo(&after,sizeof(after)));
    uint64_t committed=(after.CommitTotal>before.CommitTotal ? after.CommitTotal-before.CommitTotal : 0)*after.PageSize;
    assert(committed<UINT64_C(256)*1024*1024);
    assert(!release(large,UINT64_C(4)*1024*1024*1024));

    int64_t pad=-1,physical=-1,replacement=-1;
    assert(!alloc(0,1024*1024,PAGE_BYTES,0,0,&pad) && pad==0);
    assert(!alloc(0,1024*1024,4*PAGE_BYTES,0,0,&physical) && physical==PAGE_BYTES);
    assert(!alloc(0,1024*1024,2*PAGE_BYTES,0,0,&replacement));
    void *x=NULL,*y=NULL;
    assert(!map(&x,4*PAGE_BYTES,3,0,physical,0));
    assert(!map(&y,4*PAGE_BYTES,3,0,physical,0));
    assert((uintptr_t)x<UINT64_C(1)<<40 && (uintptr_t)y<UINT64_C(1)<<40);
    /* Both address and physical offset deliberately violate 64 KiB alignment. */
    void *unaligned=(unsigned char *)y+PAGE_BYTES;
    assert(!unmap(unaligned,PAGE_BYTES));
    assert(!map(&unaligned,PAGE_BYTES,3,0x10,physical+PAGE_BYTES,0));
    for (unsigned i=0;i<4;++i) *(uint64_t *)((unsigned char *)x+i*PAGE_BYTES)=0xabc000+i;
    for (unsigned i=0;i<4;++i) assert(*(uint64_t *)((unsigned char *)y+i*PAGE_BYTES)==0xabc000+i);
    assert((uint32_t)map(&x,PAGE_BYTES,3,0x90,physical,0)==0x8002000c);

    /* Unmapping one page must retain alias identity and GPU protections on
     * the surviving views, even after a Windows whole-view reconstruction. */
    runtime_memory_gpu_protect((uintptr_t)x+PAGE_BYTES,PAGE_BYTES,0,0);
    assert(protection((unsigned char *)x+PAGE_BYTES)==PAGE_NOACCESS);
    assert(!unmap((unsigned char *)x+2*PAGE_BYTES,PAGE_BYTES));
    assert(protection((unsigned char *)x+PAGE_BYTES)==PAGE_NOACCESS);
    assert(*(uint64_t *)((unsigned char *)x+3*PAGE_BYTES)==0xabc003);
    assert(!runtime_memory_is_mapped((uintptr_t)x+2*PAGE_BYTES,PAGE_BYTES));
    assert(runtime_memory_clamp((uintptr_t)x,4*PAGE_BYTES)==2*PAGE_BYTES);
    uint64_t marker=UINT64_C(0x123456789abcdef0);
    assert(runtime_memory_write_backing((uintptr_t)x+PAGE_BYTES,&marker,sizeof(marker)));
    assert(*(uint64_t *)((unsigned char *)y+PAGE_BYTES)==marker);
    runtime_memory_gpu_protect((uintptr_t)x+PAGE_BYTES,PAGE_BYTES,1,1);
    assert(*(uint64_t *)((unsigned char *)x+PAGE_BYTES)==marker);

    void *middle=(unsigned char *)x+PAGE_BYTES;
    assert(!map(&middle,2*PAGE_BYTES,3,0x10,replacement,0));
    assert(*(uint64_t *)middle==0);
    *(uint64_t *)middle=0x555;
    assert(*(uint64_t *)((unsigned char *)y+PAGE_BYTES)==marker);
    assert(*(uint64_t *)((unsigned char *)x+3*PAGE_BYTES)==0xabc003);
    assert(!protect(x,4*PAGE_BYTES,1));
    for (unsigned i=0;i<4;++i) assert(protection((unsigned char *)x+i*PAGE_BYTES)==PAGE_READONLY);
    void *queried_start=NULL,*queried_end=NULL; uint32_t queried_prot=0;
    assert(!query(middle,&queried_start,&queried_end,&queried_prot));
    assert(queried_start==x && queried_end==(unsigned char *)x+4*PAGE_BYTES && queried_prot==1);
    assert(!protect(x,4*PAGE_BYTES,3));
    assert(!release(physical,4*PAGE_BYTES));
    assert(!runtime_memory_is_mapped((uintptr_t)x,PAGE_BYTES));
    assert(!runtime_memory_is_mapped((uintptr_t)y,4*PAGE_BYTES));
    assert(runtime_memory_is_mapped((uintptr_t)middle,2*PAGE_BYTES));
    assert(*(uint64_t *)middle==0x555);
    assert(!release(replacement,2*PAGE_BYTES));
    assert(!release(pad,PAGE_BYTES));

    void *reserved=NULL;
    assert(!reserve(&reserved,4*PAGE_BYTES,0,PAGE_BYTES));
    assert(!runtime_memory_is_mapped((uintptr_t)reserved,PAGE_BYTES));
    int64_t fresh=-1;
    assert(!alloc(0,1024*1024,2*PAGE_BYTES,0,0,&fresh));
    void *no_access=NULL;
    assert(!map(&no_access,PAGE_BYTES,0,0,fresh,0));
    assert(protection(no_access)==PAGE_NOACCESS);
    assert(!protect(no_access,PAGE_BYTES,3));
    assert(!unmap(no_access,PAGE_BYTES));
    void *inside=(unsigned char *)reserved+PAGE_BYTES;
    assert(!map(&inside,2*PAGE_BYTES,3,0x10,fresh,0));
    assert(*(uint64_t *)inside==0); /* zero after release/reuse */
    assert(!release(fresh,2*PAGE_BYTES));
    assert(!unmap(reserved,4*PAGE_BYTES));

    void *flex=NULL;
    assert(!flexible(&flex,4*PAGE_BYTES,3,0));
    for (unsigned i=0;i<4;++i) *(uint64_t *)((unsigned char *)flex+i*PAGE_BYTES)=0xf000+i;
    assert(!unmap((unsigned char *)flex+PAGE_BYTES,PAGE_BYTES));
    assert(*(uint64_t *)((unsigned char *)flex+3*PAGE_BYTES)==0xf003);
    assert(runtime_memory_write_backing((uintptr_t)flex+3*PAGE_BYTES,&marker,sizeof(marker)));
    assert(*(uint64_t *)((unsigned char *)flex+3*PAGE_BYTES)==marker);
    void *flex2=NULL;
    assert(!flexible(&flex2,PAGE_BYTES,3,0));
    assert(*(uint64_t *)flex2==0);
    assert(*(uint64_t *)((unsigned char *)flex+3*PAGE_BYTES)==marker);
    assert(!unmap(flex,4*PAGE_BYTES));
    assert(!unmap(flex2,PAGE_BYTES));

    void *low=runtime_low_map(PAGE_BYTES,3);
    assert(low && (uintptr_t)low>=UINT64_C(0x0800000000) && (uintptr_t)low<UINT64_C(0x1000000000));
    *(uint64_t *)low=marker;
    assert(*(uint64_t *)low==marker && protection(low)==PAGE_READWRITE);
    assert(VirtualFree(low,0,MEM_RELEASE));
    void *bad=(void *)(UINT64_C(1)<<40);
    assert((uint32_t)reserve(&bad,PAGE_BYTES,0x10,PAGE_BYTES)==0x80020016);
    puts("PASS: sparse multi-GiB backing, 16 KiB aliases, partial remap/unmap, GPU protection/backing writes, flexible reuse, low memory");
    runtime_memory_report();
    return 0;
}
