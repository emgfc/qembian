// N95 LightPdd.pdd stand-in: empty physical-device factory "LightDrv.pdd" so that
// User::LoadPhysicalDevice(_L("LightPdd")) succeeds (hack H-lightdrv, docs/hacks.md). The
// LightDrv stand-in LDD does not ask for a physical device, so Create() is never called.
#include <kernel/kernel.h>

class DLightPdd : public DPhysicalDevice
	{
public:
	DLightPdd() { iVersion = TVersion(255, 255, 0xffff); }
	virtual TInt Install() { _LIT(KName, "LightDrv.pdd"); Kern::Printf("SYBSTUB: install LightDrv.pdd (PDD, load only)"); return SetName(&KName); }
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DBase*& aChannel, TInt, const TDesC8*, const TVersion&) { aChannel = NULL; return KErrNotSupported; }
	virtual TInt Validate(TInt, const TDesC8*, const TVersion&) { return KErrNone; }
	};

DECLARE_STANDARD_PDD()
	{
	return new DLightPdd;
	}
