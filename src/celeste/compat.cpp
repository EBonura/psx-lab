// Minimal C library glue for celeste on bare-metal PS1.
// memcpy, snprintf (via PSYQo's vsnprintf), and no-op printf/fprintf.

#include <stddef.h>
#include <stdarg.h>

extern "C" {

// PSYQo provides vsnprintf in xprintf.c
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = static_cast<unsigned char *>(dest);
    const unsigned char *s = static_cast<const unsigned char *>(src);
    while (n--) *d++ = *s++;
    return dest;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return ret;
}

// Debug output — no-op on PS1
struct _FILE {};
static struct _FILE fake_stderr;
struct _FILE *stderr = &fake_stderr;

int printf(const char *fmt, ...) {
    (void)fmt;
    return 0;
}

int fprintf(struct _FILE *stream, const char *fmt, ...) {
    (void)stream;
    (void)fmt;
    return 0;
}

} // extern "C"
