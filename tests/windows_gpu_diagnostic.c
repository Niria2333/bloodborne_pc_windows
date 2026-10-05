// Windows port modifications by yaonikaixin999999, 2026-10-05.
// SPDX-License-Identifier: GPL-2.0-or-later
/* Explicit diagnostic build: CPU contracts and Vulkan smoke, no game renderer. */
#include "gpu/bbgpu.h"
#include <stdio.h>
void bbgpu_register_kernel(void) {}
int bbgpu_init(const BbGpuConfig *config) {
    (void)config;
    fputs("ERROR: diagnostic build has no game renderer. Build with BB_DIAGNOSTIC_ONLY=OFF.\n",stderr);
    return -1;
}
uintptr_t bbgpu_resolve(const char *name) { (void)name; return 0; }
int bbgpu_handle_fault(void *context,void *address) { (void)context; (void)address; return 0; }
void bbgpu_dump_guest_writes(void *context) { (void)context; }
int bbgpu_text_input_begin(const char *text,const char *prompt,uint32_t max_length) { (void)text; (void)prompt; (void)max_length; return 0; }
int bbgpu_text_input_poll(char *out,uint64_t size) { (void)out; (void)size; return 2; }
void bbgpu_text_input_end(void) {}
int bbgpu_overlay_captures_input(void) { return 0; }
unsigned bbgpu_symbol_count(void) { return 0; }
