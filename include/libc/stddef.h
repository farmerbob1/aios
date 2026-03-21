/* AIOS libc shim — stddef.h
 * Must match include/types.h exactly to prevent conflicts. */
#ifndef _AIOS_STDDEF_H
#define _AIOS_STDDEF_H

typedef unsigned int size_t;
typedef int          ptrdiff_t;

#ifndef NULL
#define NULL ((void*)0)
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

/* GCC's internal __WCHAR_TYPE__ */
#ifndef __cplusplus
typedef __WCHAR_TYPE__ wchar_t;
#endif

#endif
