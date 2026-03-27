#ifndef _AIOS_STDIO_H
#define _AIOS_STDIO_H

#include <stddef.h>
#include <stdarg.h>

/* FILE — minimal concrete struct so we can allocate static instances.
 * Lua core never dereferences internals, only liolib.c does. */
typedef struct _FILE { int _dummy; } FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF (-1)
#define BUFSIZ 1024
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#define FILENAME_MAX 256
#define FOPEN_MAX    16
#define TMP_MAX      256
#define L_tmpnam     20

int snprintf(char *buf, size_t size, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int printf(const char *fmt, ...);
int fputs(const char *s, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
int fclose(FILE *stream);
FILE *fopen(const char *path, const char *mode);
int feof(FILE *stream);
int ferror(FILE *stream);
int fflush(FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void clearerr(FILE *stream);
int ungetc(int c, FILE *stream);
int fgetc(FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int setvbuf(FILE *stream, char *buf, int mode, size_t size);
FILE *tmpfile(void);
char *tmpnam(char *s);
int remove(const char *path);
int rename(const char *oldpath, const char *newpath);
FILE *freopen(const char *path, const char *mode, FILE *stream);
int getc(FILE *stream);
int putc(int c, FILE *stream);
int fputc(int c, FILE *stream);
int putchar(int c);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int vfprintf(FILE *stream, const char *fmt, va_list ap);
int puts(const char *s);

#endif
