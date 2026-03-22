/* AIOS v2 — lwIP compiler/CPU abstraction (arch/cc.h)
 * Defines types, byte order, and platform diagnostics for lwIP. */

#ifndef AIOS_LWIP_CC_H
#define AIOS_LWIP_CC_H

/* Use our kernel types */
typedef unsigned char      u8_t;
typedef signed char        s8_t;
typedef unsigned short     u16_t;
typedef signed short       s16_t;
typedef unsigned int       u32_t;
typedef signed int         s32_t;
typedef unsigned int       mem_ptr_t;
typedef unsigned int       size_t;
typedef int                ptrdiff_t;

/* Protection type for SYS_LIGHTWEIGHT_PROT */
typedef unsigned int       sys_prot_t;

/* NULL — guard against redefinition from types.h */
#ifndef NULL
#define NULL ((void*)0)
#endif

/* Byte order */
#define BYTE_ORDER LITTLE_ENDIAN

/* Packing macros for protocol headers */
#define PACK_STRUCT_FIELD(x)  x
#define PACK_STRUCT_STRUCT    __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

/* Platform diagnostics — route to serial */
extern void serial_printf(const char *fmt, ...);
extern void kernel_panic(const char *msg);

#define LWIP_PLATFORM_DIAG(x)   do { serial_printf x; } while(0)
#define LWIP_PLATFORM_ASSERT(x) kernel_panic(x)

/* Random number — simple LCG for now */
extern unsigned int lwip_rand_seed;
static inline u32_t lwip_rand_impl(void) {
    lwip_rand_seed = lwip_rand_seed * 1103515245 + 12345;
    return lwip_rand_seed;
}
#define LWIP_RAND() lwip_rand_impl()

/* Disable system headers we don't have */
#define LWIP_NO_STDINT_H    1
#define LWIP_NO_INTTYPES_H  1
#define LWIP_NO_LIMITS_H    1
#define LWIP_NO_CTYPE_H     1
#define LWIP_NO_STDDEF_H    1

/* Printf format specifiers for lwIP types */
#define X8_F   "02x"
#define U16_F  "u"
#define S16_F  "d"
#define X16_F  "x"
#define U32_F  "u"
#define S32_F  "d"
#define X32_F  "x"
#define SZT_F  "u"

/* String functions — provided by our kernel */
extern void *memset(void *, int, unsigned int);
extern void *memcpy(void *, const void *, unsigned int);
extern int   memcmp(const void *, const void *, unsigned int);
extern void *memmove(void *, const void *, unsigned int);
extern unsigned int strlen(const char *);
extern int   strcmp(const char *, const char *);
extern int   strncmp(const char *, const char *, unsigned int);
extern char *strcpy(char *, const char *);
extern char *strncpy(char *, const char *, unsigned int);

/* atoi — needed by lwIP netif.c */
static inline int lwip_atoi(const char *s) {
    int n = 0, neg = 0;
    if (!s) return 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return neg ? -n : n;
}
#define atoi(s) lwip_atoi(s)

/* isdigit/isxdigit/isalpha/tolower — used by various lwIP files */
static inline int lwip_isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int lwip_isxdigit(int c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static inline int lwip_isalpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static inline int lwip_tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}
#define isdigit(c)  lwip_isdigit(c)
#define isxdigit(c) lwip_isxdigit(c)
#define isalpha(c)  lwip_isalpha(c)
#define tolower(c)  lwip_tolower(c)

#endif /* AIOS_LWIP_CC_H */
