/* AIOS v2 — Freestanding type definitions
 * Used instead of stdint.h/stdbool.h for explicit control over dependencies. */

#pragma once

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;

typedef uint32_t  size_t;
typedef int32_t   ssize_t;
typedef int32_t   ptrdiff_t;
typedef _Bool     bool;

#define true  1
#define false 0
#ifndef NULL
#define NULL  ((void*)0)
#endif
