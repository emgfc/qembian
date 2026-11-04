// N95 nokiaiscdriver.ldd stand-in (see nokiastub.h). IscApi channel: modem present but silent; control 0 = async modem init.
#include "nokiastub.h"
static const SStubCtl KCtl[] =
	{
	{ 0, EComplete, KErrNone, 0, 0 },		// IscApi InitializeModem(TRequestStatus&): a1 -> { TRequestStatus*, ... }
	};
NOKIA_STUB_LDD("NokiaISCDriver", KCtl, 1)
