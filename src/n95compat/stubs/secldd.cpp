// N95 secldd.ldd stand-in (see nokiastub.h): channel "SecEnv" used by SecEnv.dll (Nokia secure
// environment, e.g. DRM key storage in RightsServer). SecEnv.dll does LoadLogicalDevice("secldd"),
// DoCreate("SecEnv", TVersion(1,0,x), unit -1), DoControl(0, ...) and synchronous
// DoRequest(3/4/5/6/7/16, status, args) + WaitForRequest. Those complete with KErrNotSupported. SecEnvInit.exe (an active-object file service for the secure environment)
// keeps request 2 outstanding as "wait for the next job"; completing it made SecEnvInit spin
// (1.7 M requests in one boot), so it stays pending. Every call is logged.
#include "nokiastub.h"
static const SStubCtl KCtl[] =
	{
	{ KStubRequest | 2, EPendRequest, 0, 0, 0 },
	// secure-environment crypto/storage operations (SecEnv.dll exports 12..17): no secure
	// environment here; completing them with KErrNone and no output made RightsServer panic USER 130
	{ KStubRequest | 3,  EComplete, KErrNotSupported, 0, 0 },
	{ KStubRequest | 4,  EComplete, KErrNotSupported, 0, 0 },
	{ KStubRequest | 5,  EComplete, KErrNotSupported, 0, 0 },
	{ KStubRequest | 6,  EComplete, KErrNotSupported, 0, 0 },
	{ KStubRequest | 7,  EComplete, KErrNotSupported, 0, 0 },
	{ KStubRequest | 16, EComplete, KErrNotSupported, 0, 0 },
	};
NOKIA_STUB_LDD2("SecEnv", KCtl, 1, ETrue)
