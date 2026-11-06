/* The few libc/libm pieces the blob needs (freestanding: no C library is linked). */
#include "sygl_int.h"

void* memcpy(void* aDst, const void* aSrc, unsigned aLen)
	{
	u8* d = (u8*)aDst;
	const u8* s = (const u8*)aSrc;
	if ((((unsigned)d | (unsigned)s | aLen) & 3) == 0)
		{
		u32* dw = (u32*)d;
		const u32* sw = (const u32*)s;
		for (aLen >>= 2; aLen; --aLen)
			*dw++ = *sw++;
		return aDst;
		}
	while (aLen--)
		*d++ = *s++;
	return aDst;
	}

void* memset(void* aDst, int aVal, unsigned aLen)
	{
	u8* d = (u8*)aDst;
	if ((((unsigned)d | aLen) & 3) == 0)
		{
		u32 v = (u8)aVal;
		v |= v << 8; v |= v << 16;
		u32* dw = (u32*)d;
		for (aLen >>= 2; aLen; --aLen)
			*dw++ = v;
		return aDst;
		}
	while (aLen--)
		*d++ = (u8)aVal;
	return aDst;
	}

#define SY_PI 3.14159265f

/* sin on [-pi, pi] after reduction, folded to [-pi/2, pi/2], odd Taylor series to x^11 (|err| < 1e-7) */
float sy_sinf(float x)
	{
	const float twopi = 6.28318531f;
	int k = (int)(x / twopi);
	x -= (float)k * twopi;
	if (x > SY_PI) x -= twopi;
	if (x < -SY_PI) x += twopi;
	if (x > SY_PI / 2) x = SY_PI - x;
	if (x < -SY_PI / 2) x = -SY_PI - x;
	float x2 = x * x;
	return x * (1.0f + x2 * (-1.0f / 6 + x2 * (1.0f / 120 + x2 * (-1.0f / 5040 +
		x2 * (1.0f / 362880 + x2 * (-1.0f / 39916800))))));
	}

float sy_cosf(float x)
	{
	return sy_sinf(x + SY_PI / 2);
	}

float sy_sqrtf(float x)
	{
	if (x <= 0.0f)
		return 0.0f;
	union { float f; u32 i; } u;
	u.f = x;
	u.i = 0x1fbd1df5u + (u.i >> 1);		/* initial guess */
	float y = u.f;
	y = 0.5f * (y + x / y);
	y = 0.5f * (y + x / y);
	y = 0.5f * (y + x / y);
	return y;
	}
