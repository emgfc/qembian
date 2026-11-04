// N95 custom.ldd stand-in (see nokiastub.h). EStart: DoControl 32 must give 0/1, 2 a flag, 1 the startup mode (4/5 = reset path).
#include "nokiastub.h"
static const SStubCtl KCtl[] =
	{
	{ 32, EWriteInt, 0, 0, 0 },	// EStart: value must be 0 or 1 to continue normally
	{ 2, EWriteInt, 0, 0, 0 },	// EStart: flag; non-zero triggers extra handling
	{ 1, EWriteInt, 0, 0, 0 },	// EStart: startup mode; 4 or 5 would take the reset path
	};
NOKIA_STUB_LDD("Custom", KCtl, 1)
