// custom.ldd stand-in (see nokiastub.h): EStart's start-up handshake with Nokia's Custom driver.
// Two interfaces, told apart by the version the client opens with:
//   2.0 (N95, 9.2): controls. DoControl 32 must give 0/1, 2 a flag, 1 the startup mode
//                   (4/5 = reset path).
//   1.0 (N73, 9.1): requests (the driver is an ISA client and gets the answers from the modem):
//                   request 0 {}            start-up sync: complete KErrNone
//                   control 1 -> TInt       flag (0)
//                   request 1 {TInt* mode}  start-up mode, written then completed; EStart waits 5 s,
//                                           then takes mode 10; 4/5 = reset path, 0 = normal
//                   (EStart.exe 0xf825a642.. in RM-132 v3.0717; docs/devices/nokia-n73-rm132)
#include "nokiastub.h"
static const SStubCtl KCtl[] =
	{
	{ 32, EWriteInt, 0, 0, 0 },	// EStart: value must be 0 or 1 to continue normally
	{ 2, EWriteInt, 0, 0, 0 },	// EStart: flag; non-zero triggers extra handling
	{ 1, EWriteInt, 0, 0, 0 },	// EStart: startup mode (2.0) / flag (1.0); 4 or 5 would take the reset path
	};

static TInt CustomReq(DThread* aClient, TInt aReq, const TUint32* aArgs, TInt aMajor)
	{
	if (aMajor != 1)
		return KStubNotHandled;			// 2.0: requests stay pending as before
	if (aReq == 0)
		return KErrNone;
	if (aReq == 1)
		{
		const TInt KStartupModeNormal = 0;
		return Kern::ThreadRawWrite(aClient, (TAny*)aArgs[0], &KStartupModeNormal, sizeof(TInt));
		}
	return KStubNotHandled;
	}

NOKIA_STUB_LDD_HOOKS("Custom", KCtl, EFalse, CustomReq, NULL)
