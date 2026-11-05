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
	{ KStubRequest | 7,  EComplete, KErrNotSupported, 0, 0 },
	{ KStubRequest | 16, EComplete, KErrNotSupported, 0, 0 },
	};
// HACK (docs/hacks.md): request 6 = SecEnv.dll export 17, "process data in a secure-environment
// application" (DrmBb5KeyStorage: mode 3 wrap / 2 unwrap of DRM keys). args[0] -> struct
// { TDesC8* app, TUint8* in, TInt inLen, TUint8* out, TInt outLen (in/out), TUint8 mode }.
// Identity transform: copy input to output, outLen = inLen. Failing it made RightsServer exit -5,
// which killed DRMHelperServer, which blocked AknSkinSrv -> EikSrvs -> AknCapServer -> Starter.
static TInt SecEnvHook(DThread* aClient, TInt aReq, const TUint32* aArgs)
	{
	if (aReq != 6 || !aArgs[0])
		return KStubNotHandled;
	TUint32 st[6];
	TInt r = Kern::ThreadRawRead(aClient, (TAny*)aArgs[0], st, sizeof(st));
	if (r != KErrNone)
		return r;
	TInt inLen = (TInt)st[2], outMax = (TInt)st[4];
	TInt n = inLen < outMax ? inLen : outMax;
	TUint8 buf[256];
	for (TInt done = 0; done < n; )
		{
		TInt chunk = n - done > (TInt)sizeof(buf) ? (TInt)sizeof(buf) : n - done;
		r = Kern::ThreadRawRead(aClient, (TAny*)(st[1] + done), buf, chunk);
		if (r == KErrNone)
			r = Kern::ThreadRawWrite(aClient, (TAny*)(st[3] + done), buf, chunk);
		if (r != KErrNone)
			return r;
		done += chunk;
		}
	st[4] = n;
	Kern::ThreadRawWrite(aClient, (TAny*)(aArgs[0] + 16), &st[4], 4);
	Kern::Printf("N95STUB: SecEnv process-data mode %d: %d bytes passed through (identity)", st[5] & 0xff, n);
	return KErrNone;
	}
NOKIA_STUB_LDD_HOOK("SecEnv", KCtl, SecEnvHook)
