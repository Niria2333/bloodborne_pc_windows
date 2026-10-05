// Windows port modifications by yaonikaixin999999, 2026-10-05.
// SPDX-License-Identifier: GPL-2.0-or-later
/* Native Windows guest-thread integration: TLS isolation, Win32 calls, low
 * stacks, return values and pthread_exit without foreign SEH unwind metadata. */
#include "runtime.h"
#include "native_stack.h"
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef void *(ABI *Entry)(void *);
typedef int32_t (ABI *Create)(void **,void **,Entry,void *,const char *);
typedef int32_t (ABI *Join)(void *,void **);
typedef void (ABI *Exit)(void *);
typedef void *(ABI *Self)(void);

/* This fixture isolates thread behavior from the independently tested memory
 * backend. Real process memory and native Win32 thread APIs are still used. */
void *runtime_low_map(size_t size,int prot) {
    static volatile LONG64 next=0x1000000000LL;
    (void)prot;
    size_t bytes=(size+65535)&~(size_t)65535;
    uintptr_t address=(uintptr_t)InterlockedExchangeAdd64(&next,(LONG64)bytes);
    return VirtualAlloc((void *)address,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
}
void runtime_thread_keys_cleanup(void) {
    uintptr_t base,limit;
    __asm__ volatile("movq %%gs:0x8,%0\n movq %%gs:0x10,%1" : "=r"(base),"=r"(limit));
    /* Cleanup runs back on the host stack after both return and guest exit. */
    assert(limit<=(uintptr_t)&base && (uintptr_t)&base<base);
}

static uintptr_t teb(void) {
    uintptr_t p;
    __asm__ volatile("movq %%gs:0x30,%0" : "=r"(p));
    return p;
}
static unsigned char *tcb(void) {
    unsigned char *p;
    __asm__ volatile("movq %%gs:0x28,%0" : "=r"(p));
    return p;
}
static void *ABI worker(void *argument) {
    uintptr_t before=teb();
    unsigned char *thread_pointer=tcb();
    uintptr_t base,limit;
    __asm__ volatile("movq %%gs:0x8,%0\n movq %%gs:0x10,%1" : "=r"(base),"=r"(limit));
    assert(thread_pointer && *(void **)thread_pointer==thread_pointer);
    assert((uintptr_t)thread_pointer<(UINT64_C(1)<<40));
    assert(thread_pointer[-16]==0x5a && thread_pointer[-15]==0);
    assert((uintptr_t)&before<(UINT64_C(1)<<40));
    assert(limit<=(uintptr_t)&before && (uintptr_t)&before<base);
    thread_pointer[-16]=(unsigned char)(uintptr_t)argument;
    assert(GetCurrentThreadId()!=0 && teb()==before);
    assert(((Self)runtime_thread_resolve("aI+OeCz8xrQ#p#J"))()!=NULL);
    if ((uintptr_t)argument==2) {
        ((Exit)runtime_thread_resolve("3kg7rT0NQIs#p#J"))(argument);
        assert(0 && "guest pthread_exit returned");
    }
    return argument;
}
int main(void) {
    const unsigned char initial[2]={0x5a,0};
    uintptr_t before=teb();
    runtime_set_main_tls(initial,sizeof(initial),16,16);
    runtime_thread_attach_main();
    assert(teb()==before && tcb()[-16]==0x5a);
    Create create=(Create)runtime_thread_resolve("6UgtwV+0zb4#p#J");
    Join join=(Join)runtime_thread_resolve("onNY9Byn-W8#p#J");
    assert(create && join);
    void *threads[2],*result=NULL;
    for (uintptr_t i=0;i<2;++i)
        assert(create(&threads[i],NULL,worker,(void *)(i+1),"windows-tls")==0);
    for (uintptr_t i=0;i<2;++i) {
        assert(join(threads[i],&result)==0);
        assert((uintptr_t)result==i+1);
    }
    assert(teb()==before && tcb()[-16]==0x5a);
    puts("Windows guest TLS, low stack, Win32 callbacks and thread exit passed");
    return 0;
}
