#ifndef _AIOS_TIME_H
#define _AIOS_TIME_H

typedef long long time_t;
typedef long clock_t;

#define CLOCKS_PER_SEC 250

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

time_t time(time_t *t);
clock_t clock(void);
double difftime(time_t t1, time_t t0);
time_t mktime(struct tm *tp);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);
unsigned int strftime(char *s, unsigned int max, const char *fmt, const struct tm *tp);

#endif
