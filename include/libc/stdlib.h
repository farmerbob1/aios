#ifndef _AIOS_STDLIB_H
#define _AIOS_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     2147483647

double strtod(const char *nptr, char **endptr);
float  strtof(const char *nptr, char **endptr);
long   strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);

int    abs(int x);
long   labs(long x);

void   exit(int status);
void   abort(void);

void  *malloc(size_t size);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *ptr, size_t size);
void   free(void *ptr);

int    rand(void);
void   srand(unsigned int seed);

int    system(const char *cmd);
char  *getenv(const char *name);

void   qsort(void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));
void  *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
               int (*compar)(const void *, const void *));

long long llabs(long long x);

double atof(const char *s);
int    atoi(const char *s);
long   atol(const char *s);

/* alloca — stack allocation via GCC builtin */
#define alloca __builtin_alloca

#endif
