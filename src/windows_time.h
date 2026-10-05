// Windows port modifications by yaonikaixin999999, 2026-10-05.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BB_WINDOWS_TIME_H
#define BB_WINDOWS_TIME_H
#ifdef _WIN32
#include <time.h>
static inline struct tm *bb_localtime_r(const time_t *t,struct tm *out) {
    return localtime_s(out,t) ? NULL : out;
}
static inline struct tm *bb_gmtime_r(const time_t *t,struct tm *out) {
    return gmtime_s(out,t) ? NULL : out;
}
#define localtime_r bb_localtime_r
#define gmtime_r bb_gmtime_r
#endif
#endif
