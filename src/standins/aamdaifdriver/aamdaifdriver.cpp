// AAMdaifDriver.ldd stand-in (5730, RAPIDO audio): AAMdaifApi.dll opens "MdaifDriver" (1.0.1) and
// uses only DoControl. Audio is silent: no DSP memory, the mapping controls are accepted and dropped.
// Protocol: docs/devices/nokia-5730-rm465/rapido-audio.md.
#include "nokiastub.h"

// ctl 2 table read by user code (AAMdaifApi ord 1/5): +4 u16 count = 0 -> every id looks up as 0.
// Kept in ROM (XIP LDD) so any client can read it; a non-zero marker keeps it out of .bss.
static const TUint32 KDspInfo[0x50 / 4] = { 0x4644444d /* "MDDF" marker, not read */ };

static TInt MdaifCtl(DThread* aClient, TInt aFn, TAny* a1, TAny*, const TUint32* w)
	{
	switch (aFn)
		{
	case 5:		// "DSP already set up?" out word: 0 -> client sends ISI F3 0x30 and waits for 0x31
		{
		TInt v = 0;	// 1 would skip the ISI handshake entirely (fallback if no F3 reply)
		return Kern::ThreadRawWrite(aClient, a1, &v, sizeof(v));
		}
	case 2:		// {dspAddr, size, TAny** out, dir}: return a readable (here: ROM) user pointer
		{
		const TAny* p = KDspInfo;
		return Kern::ThreadRawWrite(aClient, (TAny*)w[2], &p, sizeof(p));
		}
	case 10:	// TInt out: 0 (ord 5 then skips ctl 12)
		{
		TInt v = 0;
		return Kern::ThreadRawWrite(aClient, a1, &v, sizeof(v));
		}
	case 6: case 7: case 8:	// map entry {i, size, addr, 0} / map done / DSP request: accepted
	default:
		return KErrNone;
		}
	}

NO_CONTROLS
NOKIA_STUB_LDD_CTLHOOK("MdaifDriver", KCtl, ETrue, MdaifCtl)
