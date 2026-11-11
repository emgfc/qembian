/* <string.h> for src/khr: memory functions from src/khr/support.c */
#pragma once
typedef unsigned int size_t;
#ifdef __cplusplus
extern "C" {
#endif
void *memcpy(void *, const void *, size_t); void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t); int memcmp(const void *, const void *, size_t);
size_t strlen(const char *); int strcmp(const char *, const char *);
#ifdef __cplusplus
}
#endif
