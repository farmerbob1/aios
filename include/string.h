/* AIOS v2 — Minimal string/memory functions (freestanding)
 * The compiler with -ffreestanding may emit calls to memcpy/memset
 * for struct copies and array initialization, so these must be linked. */

#pragma once

#include "types.h"

void*  memcpy(void* dest, const void* src, size_t n);
void*  memset(void* dest, int val, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
void*  memmove(void* dest, const void* src, size_t n);

size_t strlen(const char* s);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
char*  strcpy(char* dest, const char* src);
char*  strncpy(char* dest, const char* src, size_t n);
