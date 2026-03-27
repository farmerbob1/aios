/* AIOS System Clock — NTP-synced wall-clock time
 *
 * Uses lwIP's built-in SNTP client to sync with pool.ntp.org.
 * Provides Unix timestamp + date/time breakdown for the OS. */

#include "sysclock.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"
#include "../../drivers/timer.h"

#include "lwip/apps/sntp.h"
#include "lwip/dns.h"

/* ── State ────────────────────────────────────────────── */

static volatile uint32_t ntp_epoch_sec = 0;  /* Unix timestamp at sync */
static volatile uint32_t sync_ticks = 0;     /* timer ticks at sync */
static volatile bool synced = false;

/* ── SNTP callback ────────────────────────────────────── */

void aios_set_system_time(uint32_t sec) {
    ntp_epoch_sec = sec;
    sync_ticks = timer_get_ticks();
    synced = true;
    serial_printf("[clock] NTP sync: epoch=%u\n", sec);
}

/* ── Public API ───────────────────────────────────────── */

void sysclock_init(void) {
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_setservername(1, "time.google.com");
    sntp_init();
    serial_printf("[clock] SNTP started (pool.ntp.org, time.google.com)\n");
}

uint32_t sysclock_unix(void) {
    if (!synced) return 0;
    uint32_t elapsed_ticks = timer_get_ticks() - sync_ticks;
    uint32_t freq = timer_get_frequency();
    uint32_t elapsed_sec = (freq > 0) ? elapsed_ticks / freq : 0;
    return ntp_epoch_sec + elapsed_sec;
}

bool sysclock_synced(void) {
    return synced;
}

/* Days in each month (non-leap) */
static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};

static bool is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

void sysclock_datetime(int *year, int *month, int *day,
                       int *hour, int *min, int *sec) {
    uint32_t t = sysclock_unix();
    if (t == 0) {
        *year = 0; *month = 0; *day = 0;
        *hour = 0; *min = 0; *sec = 0;
        return;
    }

    /* Seconds within today */
    uint32_t daytime = t % 86400;
    *hour = (int)(daytime / 3600);
    *min = (int)((daytime % 3600) / 60);
    *sec = (int)(daytime % 60);

    /* Days since 1970-01-01 */
    int days = (int)(t / 86400);
    int y = 1970;
    while (1) {
        int yd = is_leap(y) ? 366 : 365;
        if (days < yd) break;
        days -= yd;
        y++;
    }
    *year = y;

    int m = 0;
    while (m < 12) {
        int md = mdays[m];
        if (m == 1 && is_leap(y)) md = 29;
        if (days < md) break;
        days -= md;
        m++;
    }
    *month = m + 1;
    *day = days + 1;
}

void sysclock_bearssl_time(uint32_t *days, uint32_t *seconds) {
    uint32_t t = sysclock_unix();
    if (t == 0) {
        /* Not synced — return zeros. BearSSL will fail with TIME_UNKNOWN
         * which is correct: we genuinely don't know the time yet. */
        *days = 0;
        *seconds = 0;
        return;
    }
    /* BearSSL's own formula (from x509_minimal.c line 1406):
     * days = unix_days + 719528  (days since Jan 1, 0 AD)
     * seconds = unix_seconds_within_day */
    *days = (t / 86400) + 719528;
    *seconds = t % 86400;
}
