/* AIOS v2 — Lua C Library Shim
 * Provides all C library functions that Lua source files expect.
 * Compiled with kernel CFLAGS (no SSE). */

#include "../../include/types.h"
#include "../../include/string.h"
#include "../heap.h"
#include "../panic.h"
#include "../scheduler.h"
#include "../../drivers/serial.h"
#include "../../drivers/timer.h"

/* Include libc shim headers for types we implement */
#include <stdarg.h>
#include <locale.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>

/* ── errno ─────────────────────────────────────────────── */

int errno = 0;

static FILE stdin_impl, stdout_impl, stderr_impl;
FILE *stdin  = &stdin_impl;
FILE *stdout = &stdout_impl;
FILE *stderr = &stderr_impl;

/* ── Lua memory allocator ─────────────────────────────── */

void *lua_aios_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    (void)osize;
    if (nsize == 0) {
        kfree(ptr);
        return NULL;
    }
    if (ptr == NULL) {
        return kmalloc(nsize);
    }
    return krealloc(ptr, nsize);
}

/* ── Per-state tracked allocator ──────────────────────── */

struct lua_mem_stats {
    size_t current_bytes;
    size_t peak_bytes;
    size_t total_allocs;
    size_t total_frees;
    size_t limit_bytes;   /* 0 = unlimited */
};

void *lua_aios_alloc_tracked(void *ud, void *ptr, size_t osize, size_t nsize) {
    struct lua_mem_stats *stats = (struct lua_mem_stats *)ud;

    if (nsize == 0) {
        if (ptr) {
            stats->current_bytes -= osize;
            stats->total_frees++;
            kfree(ptr);
        }
        return NULL;
    }

    /* Check limit */
    if (stats->limit_bytes > 0) {
        size_t delta = nsize > osize ? nsize - osize : 0;
        if (stats->current_bytes + delta > stats->limit_bytes) {
            return NULL;  /* triggers Lua OOM */
        }
    }

    void *result;
    if (ptr == NULL) {
        result = kmalloc(nsize);
        stats->total_allocs++;
    } else {
        result = krealloc(ptr, nsize);
    }

    if (result) {
        stats->current_bytes += nsize - osize;
        if (stats->current_bytes > stats->peak_bytes)
            stats->peak_bytes = stats->current_bytes;
    }
    return result;
}

/* ── String functions not in kernel string.c ──────────── */

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c)
            return (void *)(p + i);
    }
    return NULL;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c)
            last = s;
        s++;
    }
    if (c == '\0') return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return (char *)haystack;
    while (*haystack) {
        if (*haystack == *needle && strncmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
        haystack++;
    }
    return NULL;
}

char *strcat(char *dest, const char *src) {
    char *d = dest + strlen(dest);
    while ((*d++ = *src++))
        ;
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest + strlen(dest);
    size_t i;
    for (i = 0; i < n && src[i]; i++)
        d[i] = src[i];
    d[i] = '\0';
    return dest;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        const char *a = accept;
        while (*a) {
            if (*s == *a)
                return (char *)s;
            a++;
        }
        s++;
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept) {
    size_t count = 0;
    while (*s) {
        const char *a = accept;
        bool found = false;
        while (*a) {
            if (*s == *a) { found = true; break; }
            a++;
        }
        if (!found) break;
        count++;
        s++;
    }
    return count;
}

size_t strcspn(const char *s, const char *reject) {
    size_t count = 0;
    while (*s) {
        const char *r = reject;
        while (*r) {
            if (*s == *r) return count;
            r++;
        }
        count++;
        s++;
    }
    return count;
}

char *strerror(int errnum) {
    (void)errnum;
    return "error";
}

int strcoll(const char *s1, const char *s2) {
    return strcmp(s1, s2);  /* No locale support, just strcmp */
}

size_t strlcpy(char *dest, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size > 0) {
        size_t copy = len < size - 1 ? len : size - 1;
        memcpy(dest, src, copy);
        dest[copy] = '\0';
    }
    return len;
}

/* ── ctype functions ──────────────────────────────────── */

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isdigit(c) || isalpha(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int iscntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7F; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isprint(int c) { return c >= 0x20 && c < 0x7F; }
int ispunct(int c) { return isprint(c) && !isalnum(c) && !isspace(c); }
int isgraph(int c) { return c > 0x20 && c < 0x7F; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

/* ── locale stubs ─────────────────────────────────────── */

static char decimal_point_str[] = ".";
static char empty_str[] = "";
static struct lconv default_lconv = {
    .decimal_point      = decimal_point_str,
    .thousands_sep      = empty_str,
    .grouping           = empty_str,
    .int_curr_symbol    = empty_str,
    .currency_symbol    = empty_str,
    .mon_decimal_point  = empty_str,
    .mon_thousands_sep  = empty_str,
    .mon_grouping       = empty_str,
    .positive_sign      = empty_str,
    .negative_sign      = empty_str,
    .int_frac_digits    = 127,
    .frac_digits        = 127,
    .p_cs_precedes      = 127,
    .p_sep_by_space     = 127,
    .n_cs_precedes      = 127,
    .n_sep_by_space     = 127,
    .p_sign_posn        = 127,
    .n_sign_posn        = 127,
};

struct lconv *localeconv(void) {
    return &default_lconv;
}

static char c_locale[] = "C";
char *setlocale(int category, const char *locale) {
    (void)category;
    (void)locale;
    return c_locale;
}

/* ── signal stub ──────────────────────────────────────── */

sighandler_t signal(int sig, sighandler_t handler) {
    (void)sig;
    (void)handler;
    return (sighandler_t)0;  /* SIG_DFL */
}

/* ── time stubs ───────────────────────────────────────── */

time_t time(time_t *t) {
    time_t val = (time_t)(timer_get_ticks() / timer_get_frequency());
    if (t) *t = val;
    return val;
}

clock_t clock(void) {
    return (clock_t)timer_get_ticks();
}

double difftime(time_t t1, time_t t0) {
    return (double)(t1 - t0);
}

static struct tm static_tm;

time_t mktime(struct tm *tp) {
    (void)tp;
    return 0;
}

struct tm *gmtime(const time_t *t) {
    (void)t;
    memset(&static_tm, 0, sizeof(static_tm));
    return (struct tm *)&static_tm;
}

struct tm *localtime(const time_t *t) {
    return gmtime(t);
}

unsigned int strftime(char *s, unsigned int max, const char *fmt, const struct tm *tp) {
    (void)fmt;
    (void)tp;
    if (max > 0) s[0] = '\0';
    return 0;
}

/* ── stdlib stubs ─────────────────────────────────────── */

void abort(void) {
    kernel_panic("Lua abort() called");
    __builtin_unreachable();
}

void exit(int status) {
    serial_printf("[lua] exit(%d)\n", status);
    task_exit();
    __builtin_unreachable();
}

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }

void *malloc(size_t size) { return kmalloc(size); }
void *calloc(size_t nmemb, size_t size) { return kzmalloc(nmemb * size); }
void *realloc(void *ptr, size_t size) { return krealloc(ptr, size); }
void  free(void *ptr) { kfree(ptr); }

static unsigned int rand_state = 1;

int rand(void) {
    rand_state = rand_state * 1103515245 + 12345;
    return (int)((rand_state >> 16) & 0x7FFF);
}

void srand(unsigned int seed) {
    rand_state = seed;
}

int system(const char *cmd) {
    (void)cmd;
    return -1;
}

char *getenv(const char *name) {
    (void)name;
    return NULL;
}

/* ── qsort (used by Lua table.sort) ──────────────────── */

static void swap_bytes(char *a, char *b, size_t size) {
    while (size--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (nmemb < 2) return;
    char *arr = (char *)base;
    /* Simple insertion sort for small arrays, quicksort for larger */
    if (nmemb <= 16) {
        for (size_t i = 1; i < nmemb; i++) {
            size_t j = i;
            while (j > 0 && compar(arr + j * size, arr + (j - 1) * size) < 0) {
                swap_bytes(arr + j * size, arr + (j - 1) * size, size);
                j--;
            }
        }
        return;
    }
    /* Quicksort with median-of-three pivot */
    size_t mid = nmemb / 2;
    if (compar(arr, arr + mid * size) > 0)
        swap_bytes(arr, arr + mid * size, size);
    if (compar(arr, arr + (nmemb - 1) * size) > 0)
        swap_bytes(arr, arr + (nmemb - 1) * size, size);
    if (compar(arr + mid * size, arr + (nmemb - 1) * size) > 0)
        swap_bytes(arr + mid * size, arr + (nmemb - 1) * size, size);
    /* Use middle as pivot, move to second-to-last */
    swap_bytes(arr + mid * size, arr + (nmemb - 2) * size, size);
    char *pivot = arr + (nmemb - 2) * size;
    size_t lo = 0, hi = nmemb - 2;
    for (;;) {
        while (compar(arr + (++lo) * size, pivot) < 0) {}
        while (compar(arr + (--hi) * size, pivot) > 0) {}
        if (lo >= hi) break;
        swap_bytes(arr + lo * size, arr + hi * size, size);
    }
    swap_bytes(arr + lo * size, pivot, size);
    qsort(arr, lo, size, compar);
    qsort(arr + (lo + 1) * size, nmemb - lo - 1, size, compar);
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    const char *arr = (const char *)base;
    while (nmemb > 0) {
        size_t mid = nmemb / 2;
        const void *p = arr + mid * size;
        int cmp = compar(key, p);
        if (cmp == 0) return (void *)p;
        if (cmp > 0) {
            arr = (const char *)p + size;
            nmemb -= mid + 1;
        } else {
            nmemb = mid;
        }
    }
    return NULL;
}

/* ── strtod ───────────────────────────────────────────── */

static int is_hex_digit(int c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

double strtod(const char *nptr, char **endptr) {
    const char *s = nptr;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;

    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }

    /* Handle hex floats: 0x... */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        double result = 0.0;
        bool has_digits = false;

        while (is_hex_digit(*s)) {
            result = result * 16.0 + hex_val(*s);
            s++;
            has_digits = true;
        }
        if (*s == '.') {
            s++;
            double place = 1.0 / 16.0;
            while (is_hex_digit(*s)) {
                result += hex_val(*s) * place;
                place /= 16.0;
                s++;
                has_digits = true;
            }
        }
        if (!has_digits) {
            if (endptr) *endptr = (char *)nptr;
            return 0.0;
        }
        /* Binary exponent: p+N */
        if (*s == 'p' || *s == 'P') {
            s++;
            int exp_sign = 1;
            if (*s == '-') { exp_sign = -1; s++; }
            else if (*s == '+') { s++; }
            int exp = 0;
            while (*s >= '0' && *s <= '9') {
                exp = exp * 10 + (*s - '0');
                s++;
            }
            exp *= exp_sign;
            /* result *= 2^exp using ldexp-like bit manipulation */
            if (exp > 0) {
                while (exp >= 30) { result *= (double)(1 << 30); exp -= 30; }
                if (exp > 0) result *= (double)(1 << exp);
            } else {
                while (exp <= -30) { result /= (double)(1 << 30); exp += 30; }
                if (exp < 0) result /= (double)(1 << (-exp));
            }
        }
        if (endptr) *endptr = (char *)s;
        return sign * result;
    }

    /* Handle special values */
    if ((s[0] == 'i' || s[0] == 'I') && (s[1] == 'n' || s[1] == 'N') && (s[2] == 'f' || s[2] == 'F')) {
        s += 3;
        if ((s[0] == 'i' || s[0] == 'I') && (s[1] == 'n' || s[1] == 'N') &&
            (s[2] == 'i' || s[2] == 'I') && (s[3] == 't' || s[3] == 'T') &&
            (s[4] == 'y' || s[4] == 'Y')) s += 5;
        if (endptr) *endptr = (char *)s;
        return sign * __builtin_huge_val();
    }
    if ((s[0] == 'n' || s[0] == 'N') && (s[1] == 'a' || s[1] == 'A') && (s[2] == 'n' || s[2] == 'N')) {
        s += 3;
        if (*s == '(') { while (*s && *s != ')') s++; if (*s == ')') s++; }
        if (endptr) *endptr = (char *)s;
        return __builtin_nan("");
    }

    /* Decimal float */
    double result = 0.0;
    bool has_digits = false;

    while (*s >= '0' && *s <= '9') {
        result = result * 10.0 + (*s - '0');
        s++;
        has_digits = true;
    }

    if (*s == '.') {
        s++;
        double place = 0.1;
        while (*s >= '0' && *s <= '9') {
            result += (*s - '0') * place;
            place *= 0.1;
            s++;
            has_digits = true;
        }
    }

    if (!has_digits) {
        if (endptr) *endptr = (char *)nptr;
        return 0.0;
    }

    /* Exponent */
    if (*s == 'e' || *s == 'E') {
        s++;
        int exp_sign = 1;
        if (*s == '-') { exp_sign = -1; s++; }
        else if (*s == '+') { s++; }
        int exp = 0;
        while (*s >= '0' && *s <= '9') {
            exp = exp * 10 + (*s - '0');
            s++;
        }
        exp *= exp_sign;
        /* Apply power of 10 */
        double p10 = 10.0;
        int absexp = exp < 0 ? -exp : exp;
        double factor = 1.0;
        while (absexp > 0) {
            if (absexp & 1) factor *= p10;
            p10 *= p10;
            absexp >>= 1;
        }
        if (exp < 0) result /= factor;
        else result *= factor;
    }

    if (endptr) *endptr = (char *)s;
    return sign * result;
}

float strtof(const char *nptr, char **endptr) {
    return (float)strtod(nptr, endptr);
}

/* ── strtol / strtoll / strtoul / strtoull ────────────── */

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;

    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }

    /* Auto-detect base */
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; }
        else { base = 10; }
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    unsigned long long result = 0;
    bool has_digits = false;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * (unsigned long long)base + (unsigned long long)digit;
        has_digits = true;
        s++;
    }

    if (!has_digits) {
        if (endptr) *endptr = (char *)nptr;
        return 0;
    }
    if (endptr) *endptr = (char *)s;
    return sign == -1 ? (unsigned long long)(-(long long)result) : result;
}

long long strtoll(const char *nptr, char **endptr, int base) {
    return (long long)strtoull(nptr, endptr, base);
}

long strtol(const char *nptr, char **endptr, int base) {
    return (long)strtoll(nptr, endptr, base);
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    return (unsigned long)strtoull(nptr, endptr, base);
}

long double strtold(const char *nptr, char **endptr) {
    return (long double)strtod(nptr, endptr);
}

/* ── snprintf / sprintf / fprintf ─────────────────────── */

/* Helper: write a character to buffer if space available */
typedef struct {
    char *buf;
    size_t pos;
    size_t size;
} fmt_state_t;

static void fmt_putc(fmt_state_t *st, char c) {
    if (st->pos + 1 < st->size)
        st->buf[st->pos] = c;
    st->pos++;
}

static void fmt_puts(fmt_state_t *st, const char *s) {
    while (*s) fmt_putc(st, *s++);
}

/* Write unsigned integer in given base */
static void fmt_uint(fmt_state_t *st, unsigned long long val, int base,
                     int upper, int width, char pad, bool left_align) {
    char digits[24];
    int len = 0;
    const char *alpha = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (val == 0) {
        digits[len++] = '0';
    } else {
        while (val > 0) {
            digits[len++] = alpha[val % (unsigned long long)base];
            val /= (unsigned long long)base;
        }
    }

    if (!left_align) {
        for (int i = len; i < width; i++) fmt_putc(st, pad);
    }
    for (int i = len - 1; i >= 0; i--) fmt_putc(st, digits[i]);
    if (left_align) {
        for (int i = len; i < width; i++) fmt_putc(st, ' ');
    }
}

static void fmt_int(fmt_state_t *st, long long val, int width, char pad,
                    bool left_align, bool force_sign) {
    if (val < 0) {
        fmt_putc(st, '-');
        if (width > 0) width--;
        fmt_uint(st, (unsigned long long)(-val), 10, 0, width, pad, left_align);
    } else {
        if (force_sign) { fmt_putc(st, '+'); if (width > 0) width--; }
        fmt_uint(st, (unsigned long long)val, 10, 0, width, pad, left_align);
    }
}

/* Double to string with precision digits after decimal point (%f style) */
static void fmt_double_f(fmt_state_t *st, double val, int prec, bool force_sign) {
    /* Handle special values */
    if (__builtin_isnan(val)) { fmt_puts(st, val < 0 ? "-nan" : "nan"); return; }
    if (__builtin_isinf(val)) { fmt_puts(st, val < 0 ? "-inf" : "inf"); return; }

    if (val < 0) { fmt_putc(st, '-'); val = -val; }
    else if (force_sign) { fmt_putc(st, '+'); }

    if (prec < 0) prec = 6;

    /* Integer part */
    unsigned long long ipart = (unsigned long long)val;
    double frac = val - (double)ipart;

    /* Round the fractional part */
    double round_add = 0.5;
    for (int i = 0; i < prec; i++) round_add *= 0.1;
    frac += round_add;
    if (frac >= 1.0) { ipart++; frac -= 1.0; }

    /* Print integer part */
    char ibuf[24];
    int ilen = 0;
    if (ipart == 0) { ibuf[ilen++] = '0'; }
    else {
        unsigned long long tmp = ipart;
        while (tmp > 0) { ibuf[ilen++] = '0' + (char)(tmp % 10); tmp /= 10; }
    }
    for (int i = ilen - 1; i >= 0; i--) fmt_putc(st, ibuf[i]);

    if (prec > 0) {
        fmt_putc(st, '.');
        for (int i = 0; i < prec; i++) {
            frac *= 10.0;
            int d = (int)frac;
            if (d > 9) d = 9;
            fmt_putc(st, '0' + (char)d);
            frac -= (double)d;
        }
    }
}

/* Double to string %e style (scientific notation) */
static void fmt_double_e(fmt_state_t *st, double val, int prec, bool upper, bool force_sign) {
    if (__builtin_isnan(val)) { fmt_puts(st, upper ? "NAN" : "nan"); return; }
    if (__builtin_isinf(val)) { fmt_puts(st, val < 0 ? (upper ? "-INF" : "-inf") : (upper ? "INF" : "inf")); return; }

    if (val < 0) { fmt_putc(st, '-'); val = -val; }
    else if (force_sign) { fmt_putc(st, '+'); }

    if (prec < 0) prec = 6;

    int exp10 = 0;
    if (val != 0.0) {
        while (val >= 10.0) { val /= 10.0; exp10++; }
        while (val < 1.0)   { val *= 10.0; exp10--; }
    }

    /* Now val is in [1.0, 10.0) */
    fmt_double_f(st, val, prec, false);
    fmt_putc(st, upper ? 'E' : 'e');
    if (exp10 < 0) { fmt_putc(st, '-'); exp10 = -exp10; }
    else { fmt_putc(st, '+'); }
    if (exp10 < 10) fmt_putc(st, '0');
    if (exp10 < 100) {
        fmt_putc(st, '0' + (char)(exp10 / 10));
        fmt_putc(st, '0' + (char)(exp10 % 10));
    } else {
        fmt_putc(st, '0' + (char)(exp10 / 100));
        fmt_putc(st, '0' + (char)((exp10 / 10) % 10));
        fmt_putc(st, '0' + (char)(exp10 % 10));
    }
}

/* %g format: use %e if exponent < -4 or >= prec, else %f. Strip trailing zeros. */
static void fmt_double_g(fmt_state_t *st, double val, int prec, bool upper, bool force_sign) {
    if (__builtin_isnan(val)) { fmt_puts(st, upper ? "NAN" : "nan"); return; }
    if (__builtin_isinf(val)) { fmt_puts(st, val < 0 ? (upper ? "-INF" : "-inf") : (upper ? "INF" : "inf")); return; }

    if (prec <= 0) prec = 1;

    double aval = val < 0 ? -val : val;
    int exp10 = 0;
    if (aval != 0.0) {
        double tmp = aval;
        while (tmp >= 10.0) { tmp /= 10.0; exp10++; }
        while (tmp < 1.0)   { tmp *= 10.0; exp10--; }
    }

    /* Render to temporary buffer to strip trailing zeros */
    char tmp_buf[64];
    fmt_state_t tmp = { tmp_buf, 0, sizeof(tmp_buf) };

    if (exp10 < -4 || exp10 >= prec) {
        fmt_double_e(&tmp, val, prec - 1, upper, force_sign);
    } else {
        int fprec = prec - exp10 - 1;
        if (fprec < 0) fprec = 0;
        fmt_double_f(&tmp, val, fprec, force_sign);
    }

    /* Null-terminate */
    size_t tlen = tmp.pos < sizeof(tmp_buf) - 1 ? tmp.pos : sizeof(tmp_buf) - 1;
    tmp_buf[tlen] = '\0';

    /* Strip trailing zeros after decimal point (but keep at least one digit after .) */
    char *dot = NULL;
    char *e_pos = NULL;
    for (size_t i = 0; i < tlen; i++) {
        if (tmp_buf[i] == '.') dot = &tmp_buf[i];
        if (tmp_buf[i] == 'e' || tmp_buf[i] == 'E') { e_pos = &tmp_buf[i]; break; }
    }

    if (dot && !e_pos) {
        char *end = tmp_buf + tlen - 1;
        while (end > dot && *end == '0') end--;
        if (end == dot) end--;  /* remove dot too if no fractional digits */
        *(end + 1) = '\0';
        tlen = (size_t)(end + 1 - tmp_buf);
    } else if (dot && e_pos) {
        /* Strip zeros between dot and 'e' */
        char *end = e_pos - 1;
        while (end > dot && *end == '0') end--;
        if (end == dot) end--;
        /* Shift exponent part */
        size_t elen = tlen - (size_t)(e_pos - tmp_buf);
        memmove(end + 1, e_pos, elen + 1);
        tlen = (size_t)(end + 1 - tmp_buf) + elen;
    }

    for (size_t i = 0; i < tlen; i++) fmt_putc(st, tmp_buf[i]);
}

/* %a format: hex float */
static void fmt_double_a(fmt_state_t *st, double val, int prec, bool upper) {
    if (__builtin_isnan(val)) { fmt_puts(st, upper ? "NAN" : "nan"); return; }
    if (__builtin_isinf(val)) { fmt_puts(st, val < 0 ? (upper ? "-INF" : "-inf") : (upper ? "INF" : "inf")); return; }

    if (val < 0) { fmt_putc(st, '-'); val = -val; }
    fmt_puts(st, upper ? "0X" : "0x");

    if (val == 0.0) {
        fmt_putc(st, '0');
        if (prec > 0) {
            fmt_putc(st, '.');
            for (int i = 0; i < prec; i++) fmt_putc(st, '0');
        }
        fmt_puts(st, upper ? "P+0" : "p+0");
        return;
    }

    /* Decompose into mantissa * 2^exp */
    union { double d; unsigned long long u; } u;
    u.d = val;
    int bexp = (int)((u.u >> 52) & 0x7FF) - 1023;
    unsigned long long mant = u.u & 0x000FFFFFFFFFFFFFULL;

    if (bexp == -1023) {
        /* Denormalized */
        bexp = -1022;
        fmt_putc(st, '0');
    } else {
        fmt_putc(st, '1');
    }

    if (prec < 0) prec = 13; /* full precision: 52 bits / 4 = 13 hex digits */
    if (prec > 0 || mant != 0) {
        fmt_putc(st, '.');
        const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        for (int i = 0; i < prec; i++) {
            int nibble = (int)((mant >> (48 - i * 4)) & 0xF);
            fmt_putc(st, hex[nibble]);
        }
    }

    fmt_putc(st, upper ? 'P' : 'p');
    if (bexp < 0) { fmt_putc(st, '-'); bexp = -bexp; }
    else { fmt_putc(st, '+'); }
    fmt_uint(st, (unsigned long long)bexp, 10, 0, 0, '0', false);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    fmt_state_t st = { buf, 0, size };

    while (*fmt) {
        if (*fmt != '%') {
            fmt_putc(&st, *fmt++);
            continue;
        }
        fmt++;  /* skip '%' */

        /* Flags */
        bool left_align = false;
        bool force_sign = false;
        bool space_sign = false;
        bool alt_form = false;
        bool zero_pad = false;
        while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#' || *fmt == '0') {
            if (*fmt == '-') left_align = true;
            if (*fmt == '+') force_sign = true;
            if (*fmt == ' ') space_sign = true;
            if (*fmt == '#') alt_form = true;
            if (*fmt == '0') zero_pad = true;
            fmt++;
        }
        (void)space_sign;
        (void)alt_form;

        /* Width */
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left_align = true; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        /* Precision */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    prec = prec * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        /* Length modifier */
        int length = 0; /* 0=int, 1=long, 2=long long */
        if (*fmt == 'l') { length = 1; fmt++; if (*fmt == 'l') { length = 2; fmt++; } }
        else if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        else if (*fmt == 'z' || *fmt == 't') { fmt++; } /* size_t / ptrdiff_t = int on i686 */

        char pad = (zero_pad && !left_align) ? '0' : ' ';

        switch (*fmt) {
        case 'd': case 'i': {
            long long val;
            if (length == 2) val = va_arg(ap, long long);
            else if (length == 1) val = va_arg(ap, long);
            else val = va_arg(ap, int);
            fmt_int(&st, val, width, pad, left_align, force_sign);
            break;
        }
        case 'u': {
            unsigned long long val;
            if (length == 2) val = va_arg(ap, unsigned long long);
            else if (length == 1) val = va_arg(ap, unsigned long);
            else val = va_arg(ap, unsigned int);
            fmt_uint(&st, val, 10, 0, width, pad, left_align);
            break;
        }
        case 'x': case 'X': {
            unsigned long long val;
            if (length == 2) val = va_arg(ap, unsigned long long);
            else if (length == 1) val = va_arg(ap, unsigned long);
            else val = va_arg(ap, unsigned int);
            if (alt_form && val != 0) fmt_puts(&st, *fmt == 'X' ? "0X" : "0x");
            fmt_uint(&st, val, 16, *fmt == 'X', width, pad, left_align);
            break;
        }
        case 'o': {
            unsigned long long val;
            if (length == 2) val = va_arg(ap, unsigned long long);
            else if (length == 1) val = va_arg(ap, unsigned long);
            else val = va_arg(ap, unsigned int);
            fmt_uint(&st, val, 8, 0, width, pad, left_align);
            break;
        }
        case 'f': case 'F': {
            double val = va_arg(ap, double);
            fmt_double_f(&st, val, prec, force_sign);
            break;
        }
        case 'e': case 'E': {
            double val = va_arg(ap, double);
            fmt_double_e(&st, val, prec, *fmt == 'E', force_sign);
            break;
        }
        case 'g': case 'G': {
            double val = va_arg(ap, double);
            fmt_double_g(&st, val, prec < 0 ? 6 : prec, *fmt == 'G', force_sign);
            break;
        }
        case 'a': case 'A': {
            double val = va_arg(ap, double);
            fmt_double_a(&st, val, prec, *fmt == 'A');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            size_t slen = strlen(s);
            if (prec >= 0 && (size_t)prec < slen) slen = (size_t)prec;
            if (!left_align) { for (size_t i = slen; i < (size_t)width; i++) fmt_putc(&st, ' '); }
            for (size_t i = 0; i < slen; i++) fmt_putc(&st, s[i]);
            if (left_align) { for (size_t i = slen; i < (size_t)width; i++) fmt_putc(&st, ' '); }
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!left_align) { for (int i = 1; i < width; i++) fmt_putc(&st, ' '); }
            fmt_putc(&st, c);
            if (left_align) { for (int i = 1; i < width; i++) fmt_putc(&st, ' '); }
            break;
        }
        case 'p': {
            void *ptr = va_arg(ap, void *);
            fmt_puts(&st, "0x");
            fmt_uint(&st, (unsigned long long)(uint32_t)ptr, 16, 0, 8, '0', false);
            break;
        }
        case '%':
            fmt_putc(&st, '%');
            break;
        case 'n':
            /* ignore %n for safety */
            break;
        default:
            fmt_putc(&st, '%');
            fmt_putc(&st, *fmt);
            break;
        }
        if (*fmt) fmt++;
    }

    /* Null-terminate */
    if (size > 0)
        st.buf[st.pos < size ? st.pos : size - 1] = '\0';

    return (int)st.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, 0x7FFFFFFF, fmt, ap);
    va_end(ap);
    return ret;
}

int fprintf(FILE *stream, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_print(buf);
    (void)stream;
    return ret;
}

int printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_print(buf);
    return ret;
}

int fputs(const char *s, FILE *stream) {
    (void)stream;
    serial_print(s);
    return 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    (void)stream;
    const char *p = (const char *)ptr;
    for (size_t i = 0; i < size * nmemb; i++)
        serial_putchar(p[i]);
    return nmemb;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    (void)ptr; (void)size; (void)nmemb; (void)stream;
    return 0;
}

int fclose(FILE *stream) { (void)stream; return 0; }
FILE *fopen(const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
int feof(FILE *stream) { (void)stream; return 1; }
int ferror(FILE *stream) { (void)stream; return 0; }
int fflush(FILE *stream) { (void)stream; return 0; }
int fseek(FILE *stream, long offset, int whence) { (void)stream; (void)offset; (void)whence; return -1; }
long ftell(FILE *stream) { (void)stream; return -1; }
void clearerr(FILE *stream) { (void)stream; }
int ungetc(int c, FILE *stream) { (void)c; (void)stream; return -1; }
int fgetc(FILE *stream) { (void)stream; return -1; }
char *fgets(char *s, int size, FILE *stream) { (void)s; (void)size; (void)stream; return NULL; }
int setvbuf(FILE *stream, char *buf, int mode, size_t size) { (void)stream; (void)buf; (void)mode; (void)size; return 0; }
FILE *tmpfile(void) { return NULL; }
char *tmpnam(char *s) { (void)s; return NULL; }
int remove(const char *path) { (void)path; return -1; }
int rename(const char *oldpath, const char *newpath) { (void)oldpath; (void)newpath; return -1; }
FILE *freopen(const char *path, const char *mode, FILE *stream) { (void)path; (void)mode; (void)stream; return NULL; }
int getc(FILE *stream) { return fgetc(stream); }
