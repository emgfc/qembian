// N95 d_display.ldd stand-in (see nokiastub.h): channel "DisplayDriver" used by the N95's
// r_display.dll (CDisplayDriver): scdv.dll (WSERV's screen driver) and, through
// power_save_display_mode.dll, the screensaver. The real driver is documented in
// docs/nokia-d_display.md (section 4 = what this stand-in follows). Geometry and the frame buffer
// come from HAL (our lcd_n95 driver); the Syborg frame buffer is always live, so screen updates
// (ctl 8/9) need no copy.
//
//   ctl 3 {start,end,fullColour}  partial display (screensaver): only panel rows start..end lit.
//                                 Panel rows = UI X columns here (portrait picture, 270 rotation);
//                                 programmed into the Syborg fb (PARTIAL_* registers)
//   ctl 4                         cancel partial display
//   ctl 5 -> TInt                 partial display active
//   ctl 10 (screen by value)      select screen: only 0
//   ctl 11 -> 26 words            capabilities (table below)
//   ctl 31 -> TInt                scan-line bytes
//   req 0x8001 (sub)              panel self-test: sub 0 only
//   req 0x8002 (mode index)       colour mode: 1 = EColor16MU only (our fb is 32 bpp)
//   req 0x8003 (rotation index)   0, 2, 3 (90 degrees is not supported by the panel)
//   req 0x23                      display event notification: stays pending (as on the device)
#include "nokiastub.h"
#include "syborg.h"

static const TUint32 KCaps[26] =
	{
	1,				// +0  display present
	0,				// +4  caller is the window server (ScDv ignores it)
	0,				// +8  DSB buffers supported: real 1; 0 keeps CDirectScreenBitmap on heap memory
	0,				// +12
	0, 0,			// +16 partial-width updates, +20 update retry strategy (0 on the N95)
	0, 0,			// +24/+28 current height/width (real 240/320, swapped after rotation; ScDv
					//         does not need them, left 0 as before)
	240,			// +32 max partial-display lines (power_save_display_mode reports it)
	0, 1, 1,		// +36/+40/+44 rotation 90/180/270 (the panel cannot do 90)
	0,				// +48 EColor4K
	0,				// +52 EColor64K: real 1; off because our frame buffer is 32 bpp only
	0,				// +56 EColor16M
	1,				// +60 EColor16MU (N95 wsini.ini asks for it)
	0,				// +64 EColor16MA
	0, 0, 0, 0, 0,	// +68..+84
	1, 1,			// +88/+92 constants on the device (meaning unknown)
	0, 0			// +96/+100
	};

// partial-display state (one panel)
static TInt PartialActive;

// Syborg fb partial-display registers (src/qemu-syborg/hw/arm/syborg_fb.c)
enum { KFbPartialStart = 14, KFbPartialEnd = 15, KFbPartialMode = 16 };

static void SetPartial(TBool aOn, TUint aStart, TUint aEnd, TBool aFullColour)
	{
	WriteReg(KHwBaseClcd, KFbPartialStart, aStart);
	WriteReg(KHwBaseClcd, KFbPartialEnd, aEnd);
	WriteReg(KHwBaseClcd, KFbPartialMode, (aOn ? 1 : 0) | (aFullColour ? 2 : 0));
	PartialActive = aOn;
	}

static TInt DisplayCtl(DThread* aClient, TInt aFn, TAny* a1, TAny*, const TUint32* aWords)
	{
	switch (aFn)
		{
	case 3:		// {startRow, endRow, fullColour}: start < end < 240
		if (aWords[0] >= aWords[1] || aWords[1] >= 240)
			return KErrArgument;
		SetPartial(ETrue, aWords[0], aWords[1], aWords[2] != 0);
		Kern::Printf("N95STUB: DisplayDriver partial display rows %d..%d %s", aWords[0], aWords[1],
					 aWords[2] ? "full colour" : "8 colours");
		return KErrNone;
	case 4:
		SetPartial(EFalse, 0, 0, EFalse);
		return KErrNone;
	case 5:
		return Kern::ThreadRawWrite(aClient, a1, &PartialActive, sizeof(TInt));
	case 10:	// screen index by value
		return (TInt)a1 == 0 ? KErrNone : KErrNotSupported;
		}
	return KStubNotHandled;
	}

static TInt DisplayReq(DThread*, TInt aReq, const TUint32* aArgs)
	{
	switch (aReq)
		{
	case 0x8001: return aArgs[0] == 0 ? KErrNone : KErrNotSupported;
	case 0x8002: return aArgs[0] == 1 ? KErrNone : KErrNotSupported;
	case 0x8003: return aArgs[0] == 1 || aArgs[0] > 3 ? KErrNotSupported : KErrNone;
		}
	return KStubNotHandled;
	}

static const SStubCtl KCtl[] =
	{
	{ 11, EWriteBlock, 0, KCaps, sizeof(KCaps) },
	// ctl 31 = scan-line bytes (r_display export 77). The device answers 320 x bytes/pixel; ScDv
	// derives its stride from it and draws portrait into our 240-pixel-wide frame buffer, so it
	// must match that: 240 px * 4 bytes (hack H-lcd-stride, docs/hacks.md).
	{ 31, EWriteInt, 960, 0, 0 },
	{ KStubRequest | 0x23, EPendRequest, 0, 0, 0 },
	};
// other r_display requests (synchronous DoRequest + WaitForRequest) complete at once
NOKIA_STUB_LDD_HOOKS("DisplayDriver", KCtl, ETrue, DisplayReq, DisplayCtl)
