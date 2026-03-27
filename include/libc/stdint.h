/* AIOS libc shim — stdint.h
 * Must match include/types.h exactly to prevent conflicts. */
#ifndef _AIOS_STDINT_H
#define _AIOS_STDINT_H

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;

typedef uint32_t  uintptr_t;
typedef int32_t   intptr_t;
typedef int64_t   intmax_t;
typedef uint64_t  uintmax_t;

#define INT8_MIN   (-128)
#define INT8_MAX   127
#define UINT8_MAX  255
#define INT16_MIN  (-32768)
#define INT16_MAX  32767
#define UINT16_MAX 65535
#define INT32_MIN  (-2147483647 - 1)
#define INT32_MAX  2147483647
#define UINT32_MAX 4294967295U
#define INT64_MIN  (-9223372036854775807LL - 1)
#define INT64_MAX  9223372036854775807LL
#define UINT64_MAX 18446744073709551615ULL

#define INTPTR_MIN  INT32_MIN
#define INTPTR_MAX  INT32_MAX
#define UINTPTR_MAX UINT32_MAX
#define INTMAX_MIN  INT64_MIN
#define INTMAX_MAX  INT64_MAX
#define UINTMAX_MAX UINT64_MAX

#define SIZE_MAX    UINT32_MAX
#define PTRDIFF_MIN INT32_MIN
#define PTRDIFF_MAX INT32_MAX

/* C99 constant macros */
#define INT8_C(c)    c
#define INT16_C(c)   c
#define INT32_C(c)   c
#define INT64_C(c)   c ## LL
#define UINT8_C(c)   c
#define UINT16_C(c)  c
#define UINT32_C(c)  c ## U
#define UINT64_C(c)  c ## ULL
#define INTMAX_C(c)  c ## LL
#define UINTMAX_C(c) c ## ULL

#endif
