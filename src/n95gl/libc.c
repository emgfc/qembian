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

/* x^e for x > 0: integer exponents by squaring (exact), others through log2/exp2 approximations
   (relative error ~1e-4, enough for specular and spot falloff) */
static float sy_log2f(float x)
	{
	union { float f; u32 i; } u;
	u.f = x;
	float e = (float)(int)((u.i >> 23) & 0xff) - 127.0f;
	u.i = (u.i & 0x007fffffu) | 0x3f800000u;	/* mantissa in [1, 2) */
	float m = u.f;
	float t = (m - 1.0f) / (m + 1.0f), t2 = t * t;	/* ln(m) = 2 atanh(t) */
	float ln = 2.0f * t * (1.0f + t2 * (1.0f / 3 + t2 * (1.0f / 5 + t2 * (1.0f / 7 + t2 * (1.0f / 9)))));
	return e + ln * 1.44269504f;
	}

static float sy_exp2f(float x)
	{
	if (x < -126.0f) return 0.0f;
	if (x > 127.0f) x = 127.0f;
	int i = (int)x;
	if ((float)i > x) --i;
	float f = x - (float)i;						/* [0, 1) */
	float y = f * 0.69314718f;					/* e^y, y in [0, ln2) */
	float p = 1.0f + y * (1.0f + y * (0.5f + y * (1.0f / 6 + y * (1.0f / 24 + y * (1.0f / 120)))));
	union { float f; u32 i; } u;
	u.i = (u32)(i + 127) << 23;
	return p * u.f;
	}

float sy_powf(float x, float e)
	{
	if (e == 0.0f) return 1.0f;
	if (x <= 0.0f) return 0.0f;
	int ie = (int)e;
	if ((float)ie == e && ie > 0 && ie <= 256)
		{
		float r = 1.0f, b = x;
		while (ie) { if (ie & 1) r *= b; b *= b; ie >>= 1; }
		return r;
		}
	return sy_exp2f(e * sy_log2f(x));
	}
