/* include/kaos/kernel.h — Convenience includes for KAOS module authors
 *
 * Modules include this to access common kernel APIs. */

#ifndef KAOS_KERNEL_H
#define KAOS_KERNEL_H

#include "../types.h"

/* These are resolved at load time via KAOS symbol table.
 * Module authors declare externs for the kernel functions they need. */

/* Serial output */
extern void serial_print(const char* str);
extern void serial_printf(const char* fmt, ...);
extern void serial_putchar(char c);

/* Heap */
extern void* kmalloc(unsigned int size);
extern void  kfree(void* ptr);
extern void* kzmalloc(unsigned int size);

/* String */
extern void* memcpy(void* dest, const void* src, unsigned int n);
extern void* memset(void* dest, int val, unsigned int n);
extern int   strcmp(const char* a, const char* b);
extern unsigned int strlen(const char* s);

#endif /* KAOS_KERNEL_H */
