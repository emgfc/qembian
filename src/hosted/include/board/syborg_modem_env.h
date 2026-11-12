/* Hosted EKA2: what the shared modem core (src/qemu-syborg/hw/arm/syborg_modem_core.c) needs, on the
 * freestanding hosted runtime (src/hosted/rt). Original work for qembian. */
#ifndef SYBORG_MODEM_ENV_H
#define SYBORG_MODEM_ENV_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int hvsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
void *sybmodem_host_alloc_zero(size_t n);       /* hmodem.cpp */
#ifdef __cplusplus
}
#endif
#define vsnprintf hvsnprintf
#define SYBMODEM_ALLOC_ZERO(n) sybmodem_host_alloc_zero(n)

static inline char *sybmodem_strchr(const char *s, int c)
{
    for (; *s; s++) {
        if (*s == (char)c) {
            return (char *)s;
        }
    }
    return c ? NULL : (char *)s;
}

static inline int sybmodem_strncmp(const char *a, const char *b, size_t n)
{
    for (; n && *a && *a == *b; n--, a++, b++) {
    }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

static inline char *sybmodem_strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) {
    }
    return r;
}
#define strchr sybmodem_strchr
#define strncmp sybmodem_strncmp
#define strcpy sybmodem_strcpy
#endif
