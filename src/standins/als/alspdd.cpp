// als.pdd stand-in: empty physical device "ALS.pdd" so that User::LoadPhysicalDevice(_L("ALS.pdd"))
// succeeds (H-als, docs/hacks.md); the ALS stand-in LDD asks for no physical device.
#include <kernel/kernel.h>

class DAlsPdd : public DPhysicalDevice
	{
public:
	DAlsPdd() { iVersion = TVersion(255, 255, 0xffff); }
	virtual TInt Install() { _LIT(KName, "ALS.pdd"); Kern::Printf("SYBSTUB: install ALS.pdd (PDD, load only)"); return SetName(&KName); }
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DBase*& aChannel, TInt, const TDesC8*, const TVersion&) { aChannel = NULL; return KErrNotSupported; }
	virtual TInt Validate(TInt, const TDesC8*, const TVersion&) { return KErrNone; }
	};

DECLARE_STANDARD_PDD()
	{
	return new DAlsPdd;
	}
