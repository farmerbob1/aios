/* AIOS QuickJS C Library Shim
 * Provides functions QuickJS needs that aren't in the Lua shim layer. */

#include "../../include/types.h"
#include "../../include/string.h"
#include "../../drivers/timer.h"
#include "../net/sysclock.h"

#include <sys/time.h>
#include <time.h>
#include <stdio.h>

/* ── gettimeofday ──────────────────────────────────────── */

int gettimeofday(struct timeval *tv, struct timezone *tz) {
    (void)tz;
    if (tv) {
        uint32_t unix_sec = sysclock_unix();
        if (unix_sec) {
            tv->tv_sec = (long)unix_sec;
            /* Approximate sub-second precision from timer ticks */
            uint32_t freq = timer_get_frequency();
            uint32_t ticks = timer_get_ticks();
            if (freq > 0) {
                tv->tv_usec = (long)((ticks % freq) * 1000000UL / freq);
            } else {
                tv->tv_usec = 0;
            }
        } else {
            /* NTP not synced — use ticks as rough time source */
            uint32_t freq = timer_get_frequency();
            uint32_t ticks = timer_get_ticks();
            if (freq > 0) {
                tv->tv_sec = (long)(ticks / freq);
                tv->tv_usec = (long)((ticks % freq) * 1000000UL / freq);
            } else {
                tv->tv_sec = 0;
                tv->tv_usec = 0;
            }
        }
    }
    return 0;
}

/* ── atof / atoi ──────────────────────────────────────── */

double strtod(const char *nptr, char **endptr);

double atof(const char *s) {
    return strtod(s, (char **)0);
}

long strtol(const char *nptr, char **endptr, int base);

int atoi(const char *s) {
    return (int)strtol(s, (char **)0, 10);
}

long atol(const char *s) {
    return strtol(s, (char **)0, 10);
}

/* ── memmem (non-standard, used by cutils.c) ─────────── */

void *memmem(const void *haystack, size_t haystacklen,
             const void *needle, size_t needlelen) {
    if (needlelen == 0) return (void *)haystack;
    if (haystacklen < needlelen) return (void *)0;

    const uint8_t *h = (const uint8_t *)haystack;
    const uint8_t *n = (const uint8_t *)needle;
    size_t limit = haystacklen - needlelen;

    for (size_t i = 0; i <= limit; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, needlelen) == 0) {
            return (void *)(h + i);
        }
    }
    return (void *)0;
}

/* mktime is already provided by lua_shim.c */
