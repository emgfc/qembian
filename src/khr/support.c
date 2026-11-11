/* C runtime bits for src/khr's DLLs (freestanding build): DLL entry body (static constructors), the
 * memory/string functions the compiler and the RI use. SPDX-License-Identifier: MIT */
typedef unsigned int size_t;
typedef void (*ctor)(void);
extern ctor __init_array_start[] __attribute__((weak));
extern ctor __init_array_end[] __attribute__((weak));

int khr_dll_body(int reason)
{
	if (reason == 0 && __init_array_start)      /* KModuleEntryReasonProcessAttach */
		for (ctor *c = __init_array_start; c < __init_array_end; c++)
			(*c)();
	return 0;
}

void *memcpy(void *d, const void *s, size_t n)
{
	unsigned char *dp = d; const unsigned char *sp = s;
	if ((((unsigned)dp | (unsigned)sp | n) & 3) == 0) {
		unsigned *dw = d; const unsigned *sw = s;
		for (n >>= 2; n; n--) *dw++ = *sw++;
		return d;
	}
	while (n--) *dp++ = *sp++;
	return d;
}

void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *dp = d; const unsigned char *sp = s;
	if (dp <= sp || dp >= sp + n)
		return memcpy(d, s, n);
	dp += n; sp += n;
	while (n--) *--dp = *--sp;
	return d;
}

void *memset(void *d, int c, size_t n)
{
	unsigned char *dp = d;
	while (n--) *dp++ = (unsigned char)c;
	return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;
	for (; n; n--, x++, y++)
		if (*x != *y) return *x - *y;
	return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return (unsigned char)*a - (unsigned char)*b; }

/* EABI aliases GCC may emit */
void __aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memmove(void *d, const void *s, size_t n) { memmove(d, s, n); }
void __aeabi_memmove4(void *d, const void *s, size_t n) { memmove(d, s, n); }
void __aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }
void __aeabi_memset4(void *d, size_t n, int c) { memset(d, c, n); }
void __aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr8(void *d, size_t n) { memset(d, 0, n); }
