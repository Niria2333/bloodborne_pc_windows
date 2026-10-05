// Windows port modifications by yaonikaixin999999, 2026-10-05.
// SPDX-License-Identifier: GPL-2.0-or-later
/* Exercise the loader's actual VEH handler with a recoverable speculative read. */
#define main native_probe_main
#include "../src/probe.c"
#undef main
#include <assert.h>
int main(void) {
    assert(AddVectoredExceptionHandler(1,windows_fault)!=NULL);
    jmp_buf recover;
    if (!_setjmp(recover,NULL)) {
        runtime_fault_recover=&recover;
        volatile uintptr_t invalid_address=1;
        volatile unsigned char *invalid=(volatile unsigned char *)invalid_address;
        (void)*invalid;
        assert(0 && "speculative access should fault");
    }
    assert(runtime_fault_recover==NULL);
    puts("Windows speculative memory read recovery passed");
    return 0;
}
