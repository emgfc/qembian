// Stand-in settings from the board (syborg-platform CONFIG window, "key=value;..." at +0x400), set
// from the device profile by tools/device.py qemu-args (-global syborg-platform.config=...), so one
// stand-in build serves every device of a family (docs/devices.md, step 11).
#ifndef BOARDCONFIG_H
#define BOARDCONFIG_H
#include "syborg.h"

// copies the value of aKey into aBuf (NUL-terminated, at most aMax-1 chars); returns its length,
// or -1 when the board has no such key (an older board reads zeros)
static TInt BoardConfig(const char* aKey, char* aBuf, TInt aMax)
	{
	// scanned in place (no copy: these run on small supervisor stacks)
	const volatile TUint8* p = (const volatile TUint8*)(KHwSVPPlatformDevice + 0x400);
	const TInt KSize = 0x400;
	TInt klen = 0;
	while (aKey[klen])
		++klen;
	TInt i = 0;
	while (i < KSize && p[i])
		{
		TBool same = ETrue;
		for (TInt k = 0; k < klen && same; ++k)
			same = i + k < KSize && p[i + k] == (TUint8)aKey[k];
		if (same && i + klen < KSize && p[i + klen] == '=')
			{
			TInt len = 0;
			for (TInt k = i + klen + 1; k < KSize && p[k] && p[k] != ';' && len < aMax - 1; ++k)
				aBuf[len++] = (char)p[k];
			aBuf[len] = 0;
			return len;
			}
		while (i < KSize && p[i] && p[i] != ';')
			++i;
		if (i < KSize && p[i] == ';')
			++i;
		}
	return -1;
	}

static TInt BoardConfigInt(const char* aKey, TInt aDefault)
	{
	char b[16];
	if (BoardConfig(aKey, b, sizeof(b)) <= 0)
		return aDefault;
	TInt v = 0;
	for (TInt i = 0; b[i] >= '0' && b[i] <= '9'; ++i)
		v = v * 10 + (b[i] - '0');
	return v;
	}
#endif
