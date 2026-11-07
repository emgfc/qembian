// AAMdaifDriver.ldd stand-in (5730, RAPIDO audio): AAMdaifApi.dll loads it and opens "MdaifDriver".
// PROBE ONLY (2026-11-07): every control returns 0 and every request completes with KErrNone, and
// all of it is logged ("N95STUB: MdaifDriver ..."), to learn what the client asks before a real
// mock is written. Hack H-mdaif-probe (docs/hacks.md).
#include "nokiastub.h"
NO_CONTROLS
NOKIA_STUB_LDD2("MdaifDriver", KCtl, 1, ETrue)
