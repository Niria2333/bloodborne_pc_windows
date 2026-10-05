// Windows port modifications by yaonikaixin999999, 2026-10-05.
// SPDX-License-Identifier: GPL-2.0-or-later
/* GCC knows the SysV declaration when a Win64 caller enters guest code and
 * preserves the additional Win64 nonvolatile registers at that call site. */
#ifndef BB_NATIVE_STACK_H
#define BB_NATIVE_STACK_H
#include "runtime.h"
void ABI enter_on_stack(void *entry, void *arg0, void *arg1, void *top, void *bottom);
#endif
