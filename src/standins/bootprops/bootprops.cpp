// bootprops: Publish & Subscribe values a phone's own hardware components publish at boot, for the
// components we do not run. The profile lists them: [[standins]] name = "bootprops",
// config = { props = "CAT/KEY=VALUE,..." } (hex category and key, decimal or 0x value), read here as the
// board config key "bootprops.props" (src/standins/_shared/boardconfig.h).
// First use: N8 Belle's GLES forwarders (libGLESv1_CM.dll ...) read the VideoCore chip type from
// 0x2726BEEF/0 and panic "LibraryLoader::L -1" when it is not defined; the VideoCore boot side
// publishes it on the phone (0 = VideoCore III, the N8's BCM2727 -> khclient_vc3.dll).
#include <kernel/kernel.h>
#include <kernel/sproperty.h>
#include "boardconfig.h"

static TUint ParseHex(const char*& p)
	{
	TUint v = 0;
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		p += 2;
	for (;; ++p)
		{
		TUint d;
		if (*p >= '0' && *p <= '9') d = *p - '0';
		else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
		else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
		else return v;
		v = (v << 4) | d;
		}
	}

static TInt ParseValue(const char*& p)
	{
	TBool neg = *p == '-';
	if (neg)
		++p;
	TInt v;
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		v = (TInt)ParseHex(p);
	else
		for (v = 0; *p >= '0' && *p <= '9'; ++p)
			v = v * 10 + (*p - '0');
	return neg ? -v : v;
	}

DECLARE_STANDARD_EXTENSION()
	{
	char spec[256];
	if (BoardConfig("bootprops.props", spec, sizeof(spec)) <= 0)
		return KErrNone;
	_LIT_SECURITY_POLICY_PASS(KPass);
	const char* p = spec;
	while (*p)
		{
		TUint cat = ParseHex(p);
		TUint key = 0;
		TInt value = 0;
		if (*p == '/')
			key = ParseHex(++p);
		if (*p == '=')
			value = ParseValue(++p);
		RPropertyRef prop;
		TInt r = prop.Attach(TUid::Uid(cat), key);
		if (r == KErrNone)
			{
			r = prop.Define(RProperty::EInt, KPass, KPass);
			if (r == KErrNone || r == KErrAlreadyExists)
				r = prop.Set(value);
			prop.Close();
			}
		Kern::Printf("BOOTPROPS: %08x/%x = %d -> %d", cat, key, value, r);
		while (*p && *p != ',')
			++p;
		if (*p == ',')
			++p;
		}
	return KErrNone;
	}
