#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *dest, const void *src, size_t n);
size_t strlen(const char *s);  // provided by nugget cxxglue.c

#ifdef __cplusplus
}
#endif
