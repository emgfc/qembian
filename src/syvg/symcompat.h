/* The few Symbian names the v1 host-drawing code (now src/syvg/vghost.cpp, from src/khr/hostgl.cpp and
   hostraster.cpp) used, for the host build. SPDX-License-Identifier: MIT */
#pragma once
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>
typedef int TInt;
typedef unsigned char TUint8;
typedef unsigned int TUint32;
typedef int TInt32;
enum { KErrNone = 0, KErrNotSupported = -5, KErrGeneral = -2, KErrBadDescriptor = -38 };
template <class T> static inline T Min(T a, T b) { return a < b ? a : b; }
template <class T> static inline T Max(T a, T b) { return a > b ? a : b; }
namespace Mem { static inline void Copy(void *d, const void *s, int n) { memcpy(d, s, n); } }
namespace RDebug {
static inline void Printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static inline void Printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("syvg: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}
}
