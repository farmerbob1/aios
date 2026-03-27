/* AIOS System Clock — NTP-synced wall-clock time */

#pragma once

#include "../../include/types.h"

/* Initialize SNTP client (call after lwIP stack + DHCP) */
void sysclock_init(void);

/* Get Unix timestamp (seconds since 1970-01-01 00:00:00 UTC) */
uint32_t sysclock_unix(void);

/* Is the clock synced (NTP received)? */
bool sysclock_synced(void);

/* Get date/time components */
void sysclock_datetime(int *year, int *month, int *day,
                       int *hour, int *min, int *sec);

/* Get BearSSL-format time (days since year 0, seconds since midnight) */
void sysclock_bearssl_time(uint32_t *days, uint32_t *seconds);

/* Called by SNTP when time is received */
void aios_set_system_time(uint32_t sec);
