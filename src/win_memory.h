// Windows port modifications by yaonikaixin999999, 2026-10-05.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BB_WIN_MEMORY_H
#define BB_WIN_MEMORY_H
/* Native Windows backing for runtime_memory.c. The guest arena is a sparse
 * placeholder reservation, split only at actual view boundaries. Replacing
 * placeholders allows 16 KiB file offsets and addresses: ordinary Windows
 * MapViewOfFile requires 64 KiB alignment and cannot implement PS4 aliases.
 * All calls except page protection run under the runtime's exclusive lock. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef PVOID (WINAPI *WinAlloc2)(HANDLE,PVOID,SIZE_T,ULONG,ULONG,void *,ULONG);
typedef PVOID (WINAPI *WinMap3)(HANDLE,HANDLE,PVOID,ULONG64,SIZE_T,ULONG,ULONG,void *,ULONG);
typedef BOOL (WINAPI *WinUnmap2)(HANDLE,PVOID,ULONG);
/* mapped: 0 placeholder, 1 section view, -1 pre-existing host allocation. */
typedef struct { uintptr_t start,end; uint64_t phys; int mapped; } WinSpan;
typedef struct { uintptr_t start,end; DWORD protection; } WinProtection;
static WinAlloc2 win_alloc2;
static WinMap3 win_map3;
static WinUnmap2 win_unmap2;
static HANDLE win_section;
static unsigned char *win_backing;
static uint64_t win_backing_size;
static WinSpan *win_spans;
static size_t win_span_count,win_span_capacity;

static DWORD win_memory_page_prot(int prot) {
    if (prot & 4) return (prot & 2) ? PAGE_EXECUTE_READWRITE : (prot & 1) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    return (prot & 2) ? PAGE_READWRITE : (prot & 1) ? PAGE_READONLY : PAGE_NOACCESS;
}
static void win_memory_error(const char *operation) {
    fprintf(stderr,"Runtime: Windows memory %s failed, error=%lu\n",operation,(unsigned long)GetLastError());
}
/* After removing a mapped view, its placeholder must remain reconstructible.
 * An OS failure here must stop execution rather than leave stale guest VMAs. */
static void win_memory_require(int ok,const char *operation) {
    if (!ok) { win_memory_error(operation); abort(); }
}
static int win_span_capacity_for(size_t count) {
    if (count<=win_span_capacity) return 0;
    size_t capacity=win_span_capacity ? win_span_capacity*2 : 256;
    if (capacity<count) capacity=count;
    WinSpan *next=realloc(win_spans,capacity*sizeof(*next));
    if (!next) return -1;
    win_spans=next; win_span_capacity=capacity; return 0;
}
static size_t win_span_index(uintptr_t address) {
    size_t low=0,high=win_span_count;
    while (low<high) { size_t mid=(low+high)/2; if (win_spans[mid].end<=address) low=mid+1; else high=mid; }
    return low;
}
static int win_split_placeholder(uintptr_t address) {
    size_t i=win_span_index(address);
    if (i==win_span_count || win_spans[i].start>=address) return 0;
    WinSpan old=win_spans[i];
    if (old.mapped || win_span_capacity_for(win_span_count+1)) return -1;
    if (!VirtualFree((void *)old.start,address-old.start,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) return -1;
    memmove(win_spans+i+2,win_spans+i+1,(win_span_count-i-1)*sizeof(*win_spans));
    win_spans[i].end=address;
    win_spans[i+1]=(WinSpan){address,old.end,0,0}; ++win_span_count;
    return 0;
}
static int win_coalesce_placeholders(void) {
    for (size_t i=0;i+1<win_span_count;) {
        if (win_spans[i].mapped || win_spans[i+1].mapped) { ++i; continue; }
        if (!VirtualFree((void *)win_spans[i].start,win_spans[i+1].end-win_spans[i].start,
                         MEM_RELEASE|MEM_COALESCE_PLACEHOLDERS)) return -1;
        win_spans[i].end=win_spans[i+1].end;
        memmove(win_spans+i+1,win_spans+i+2,(--win_span_count-i-1)*sizeof(*win_spans));
    }
    return 0;
}
static int win_map_span(size_t i,uint64_t phys,int prot) {
    WinSpan *span=&win_spans[i];
    void *view=win_map3(win_section,GetCurrentProcess(),(void *)span->start,phys,span->end-span->start,
                        MEM_REPLACE_PLACEHOLDER,PAGE_EXECUTE_READWRITE,NULL,0);
    if (!view) return -1;
    span->phys=phys; span->mapped=1;
    /* A view created read-only/no-access cannot later gain write permission
     * with VirtualProtect. Request the section's full access rights and then
     * apply the guest restriction, allowing later sceKernelMprotect changes. */
    if (prot!=7) {
        DWORD old;
        win_memory_require(VirtualProtect(view,span->end-span->start,win_memory_page_prot(prot),&old),"set mapped view protection");
    }
    return 0;
}
static int win_snapshot_protection(uintptr_t start,uintptr_t end,WinProtection **out,size_t *count) {
    WinProtection *saved=NULL; size_t used=0,capacity=0;
    for (uintptr_t address=start;address<end;) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void *)address,&info,sizeof(info))) { free(saved); return -1; }
        uintptr_t next=(uintptr_t)info.BaseAddress+info.RegionSize;
        if (next>end) next=end;
        if (next<=address) { free(saved); return -1; }
        if (used==capacity) {
            capacity=capacity ? capacity*2 : 8;
            WinProtection *grown=realloc(saved,capacity*sizeof(*saved));
            if (!grown) { free(saved); return -1; } saved=grown;
        }
        saved[used++]=(WinProtection){address,next,info.Protect}; address=next;
    }
    *out=saved; *count=used; return 0;
}
static void win_restore_protection(const WinProtection *saved,size_t count,uintptr_t start,uintptr_t end) {
    for (size_t i=0;i<count;++i) {
        uintptr_t a=saved[i].start>start ? saved[i].start : start;
        uintptr_t b=saved[i].end<end ? saved[i].end : end;
        if (a>=b) continue;
        DWORD old;
        win_memory_require(VirtualProtect((void *)a,b-a,saved[i].protection,&old),"restore view protection");
    }
}
/* Windows unmaps whole views. For a partial unmap, reconstruct surviving
 * portions against the same section offsets and restore their actual host
 * protections, including temporary GPU read/write tracking restrictions. */
static int win_clear_range(uintptr_t start,uintptr_t end) {
    if (!win_span_count || start<win_spans[0].start || end>win_spans[win_span_count-1].end || end<start) return -1;
    for (size_t i=win_span_index(start);i<win_span_count && win_spans[i].start<end;++i)
        if (win_spans[i].mapped<0) return -1;
    for (size_t i=win_span_index(start);i<win_span_count && win_spans[i].start<end;) {
        WinSpan old=win_spans[i];
        if (!old.mapped) { ++i; continue; }
        uintptr_t a=start>old.start ? start : old.start,b=end<old.end ? end : old.end;
        WinProtection *saved=NULL; size_t saved_count=0;
        if (win_span_capacity_for(win_span_count+2)) return -1;
        if ((a>old.start || b<old.end) && win_snapshot_protection(old.start,old.end,&saved,&saved_count)) return -1;
        if (!win_unmap2(GetCurrentProcess(),(void *)old.start,MEM_PRESERVE_PLACEHOLDER)) {
            free(saved); return -1;
        }
        win_spans[i].mapped=0; win_spans[i].phys=0;
        win_memory_require(!win_split_placeholder(a),"split left placeholder");
        win_memory_require(!win_split_placeholder(b),"split right placeholder");
        if (old.start<a) {
            win_memory_require(!win_map_span(win_span_index(old.start),old.phys,7),"remap surviving left view");
            win_restore_protection(saved,saved_count,old.start,a);
        }
        if (b<old.end) {
            win_memory_require(!win_map_span(win_span_index(b),old.phys+b-old.start,7),"remap surviving right view");
            win_restore_protection(saved,saved_count,b,old.end);
        }
        free(saved); i=win_span_index(b);
    }
    win_memory_require(!win_coalesce_placeholders(),"coalesce placeholders");
    return 0;
}
static int win_memory_pool(uint64_t size,uintptr_t minimum,uintptr_t maximum,unsigned char **backing) {
    if (win_section) { *backing=win_backing; return 0; }
    HMODULE kernel=GetModuleHandleW(L"kernelbase.dll");
    if (!kernel) kernel=GetModuleHandleW(L"kernel32.dll");
    win_alloc2=(WinAlloc2)(void *)GetProcAddress(kernel,"VirtualAlloc2");
    win_map3=(WinMap3)(void *)GetProcAddress(kernel,"MapViewOfFile3");
    win_unmap2=(WinUnmap2)(void *)GetProcAddress(kernel,"UnmapViewOfFile2");
    if (!win_alloc2 || !win_map3 || !win_unmap2) {
        fputs("Runtime: guest memory needs Windows 10 version 1803 or newer (placeholder APIs).\n",stderr); return -1;
    }
    if (win_span_capacity_for(1)) return -1;
    HANDLE section=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_EXECUTE_READWRITE|SEC_RESERVE,
                                      (DWORD)(size>>32),(DWORD)size,NULL);
    if (!section) { win_memory_error("create sparse shared section"); return -1; }
    /* Windows places native thread stacks below 1 TiB. Reserve the free
     * intervals and leave those existing host allocations untouched. */
    win_span_count=0;
    for (uintptr_t at=minimum;at<maximum;) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void *)at,&info,sizeof(info))) goto reserve_failed;
        uintptr_t next=(uintptr_t)info.BaseAddress+info.RegionSize;
        if (next>maximum) next=maximum;
        if (next<=at || win_span_capacity_for(win_span_count+1)) goto reserve_failed;
        int occupied=info.State!=MEM_FREE;
        if (!occupied && !win_alloc2(GetCurrentProcess(),(void *)at,next-at,
                                     MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0)) goto reserve_failed;
        win_spans[win_span_count++]=(WinSpan){at,next,0,occupied ? -1 : 0};
        at=next;
    }
    unsigned char *view=MapViewOfFile(section,FILE_MAP_ALL_ACCESS|FILE_MAP_EXECUTE,0,0,(SIZE_T)size);
    if (!view) { win_memory_error("map backing view"); goto reserve_failed; }
    win_section=section; win_backing=view; win_backing_size=size; *backing=view;
    return 0;
reserve_failed:
    win_memory_error("reserve guest address arena");
    for (size_t i=0;i<win_span_count;++i) if (!win_spans[i].mapped) VirtualFree((void *)win_spans[i].start,0,MEM_RELEASE);
    win_span_count=0; CloseHandle(section); return -1;
}
/* The guest VMA table knows guest mappings; native stacks that were already
 * in the 40-bit address range must also be excluded by its first-fit search. */
static uintptr_t win_memory_blocked_end(uintptr_t address,uint64_t size) {
    for (size_t i=win_span_index(address);i<win_span_count && win_spans[i].start<address+size;++i)
        if (win_spans[i].mapped<0) return win_spans[i].end;
    return 0;
}
static int win_memory_place(uintptr_t address,uint64_t size,int prot,int mapped,uint64_t phys) {
    if (!size || address+size<address || !win_span_count || address<win_spans[0].start ||
        address+size>win_spans[win_span_count-1].end) return -1;
    if (mapped) {
        if (phys>win_backing_size || size>win_backing_size-phys) return -1;
        /* SEC_RESERVE consumes pagefile/RAM only for physical pages used by
         * a mapping, rather than committing the entire 6-10 GiB section. */
        if (!VirtualAlloc(win_backing+phys,(SIZE_T)size,MEM_COMMIT,PAGE_EXECUTE_READWRITE)) {
            win_memory_error("commit shared pages"); return -1;
        }
    }
    if (win_span_capacity_for(win_span_count+2) || win_clear_range(address,address+size)) return -1;
    if (!mapped) return 0;
    win_memory_require(!win_split_placeholder(address),"split mapping start");
    win_memory_require(!win_split_placeholder(address+size),"split mapping end");
    win_memory_require(!win_map_span(win_span_index(address),phys,prot),"replace placeholder with shared view");
    return 0;
}
static int win_memory_unmap(uintptr_t address,uint64_t size) {
    if (!size || address+size<address) return -1;
    return win_clear_range(address,address+size);
}
static int win_memory_protect(uintptr_t address,uint64_t size,int prot) {
    uintptr_t end=address+size;
    if (!size || end<address) return -1;
    for (size_t i=win_span_index(address);address<end;++i) {
        if (i==win_span_count || win_spans[i].start>address || win_spans[i].mapped!=1) return -1;
        uintptr_t next=win_spans[i].end<end ? win_spans[i].end : end;
        DWORD old;
        if (!VirtualProtect((void *)address,next-address,win_memory_page_prot(prot),&old)) return -1;
        address=next;
    }
    return 0;
}
static void win_memory_zero(uint64_t phys,uint64_t size) {
    if (!win_backing || phys>win_backing_size || size>win_backing_size-phys) return;
    uintptr_t address=(uintptr_t)win_backing+phys,end=address+size;
    while (address<end) {
        MEMORY_BASIC_INFORMATION info;
        win_memory_require(VirtualQuery((void *)address,&info,sizeof(info))!=0,"query backing commitment");
        uintptr_t next=(uintptr_t)info.BaseAddress+info.RegionSize;
        if (next>end) next=end;
        if (info.State==MEM_COMMIT) memset((void *)address,0,next-address);
        address=next;
    }
    /* Windows cannot decommit SEC_RESERVE section pages. Reuse is zeroed;
     * committed storage stays at its high-water mark until process exit. */
}
#endif
