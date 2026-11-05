// N95 d_display.ldd stand-in (see nokiastub.h): channel "DisplayDriver" used by the N95's
// r_display.dll (CDisplayDriver), which scdv.dll (the screen driver) opens to learn the
// supported colour modes. Geometry and the frame buffer come from HAL (our Syborg LCD driver),
// so only open, select-screen, capabilities and completing requests are needed.
//   DoControl(10, &screen)  select screen                -> KErrNone
//   DoControl(11, caps)     104-byte capabilities block  -> only the colour-mode flags are known:
//       +56 EColor16M (8), +60 EColor16MU (11), +64 EColor16MA (12) supported
//       (scdv tries +64, then +60, then +56; the N95 wsini.ini asks for COLOR16MU)
//       +8: scdv stores (+8 == 0) as a flag; left 0
//       +36/+40/+44: rotations 90/180/270 supported
#include "nokiastub.h"
static const TUint32 KCaps[26] =
	{
	1, 0, 0, 0, 1, 1, 0, 0,		// +0..+31: +0, +16, +20 = 1 are the defaults scdv pre-fills before
								// the call (+0 is kept by scdv per screen device; meaning unknown)
	0,							// +32 unknown (read by r_display export 62)
	1, 1, 1,					// +36/+40/+44 rotation 90/180/270 supported (r_display export 63;
								// rotation 0 is implicit). The N95 wsini.ini uses 270 for 240x320.
	0, 0,						// +48..+55 unknown
	0,							// +56 EColor16M
	1,							// +60 EColor16MU
	0,							// +64 EColor16MA
	0, 0, 0, 0, 0, 0, 0, 0, 0	// +68..+103 unknown
	};
static const SStubCtl KCtl[] =
	{
	{ 10, EReturn, KErrNone, 0, 0 },
	{ 11, EWriteBlock, 0, KCaps, sizeof(KCaps) },
	};
// r_display issues synchronous requests (DoRequest 0x8001.. + WaitForRequest), e.g. screen
// updates; the Syborg frame buffer is always live, so complete them at once.
// Diagnostic: dump the structures behind ctl 31 (pointer in word 1) and ctl 9 (rect).
static TInt DisplayCtlHook(DThread* aClient, TInt aFn, TAny* a1, TAny*, const TUint32* aW)
	{
	if (aFn == 31 && aW[1])
		{
		TUint32 b[12];
		if (Kern::ThreadRawRead(aClient, (TAny*)aW[1], b, sizeof(b)) == KErrNone)
			Kern::Printf("N95STUB: DisplayDriver ctl 31 @%08x: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
				aW[1], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]);
		}
	(void)a1;
	return KStubNotHandled;
	}
NOKIA_STUB_LDD_CTLHOOK("DisplayDriver", KCtl, ETrue, DisplayCtlHook)
