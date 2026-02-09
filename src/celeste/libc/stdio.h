#pragma once
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _FILE FILE;
extern FILE *stderr;

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);  // from PSYQo xprintf.c
int snprintf(char *buf, size_t n, const char *fmt, ...);
int printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
