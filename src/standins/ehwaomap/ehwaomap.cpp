// N95 ehwaomap.pdd stand-in: an empty physical-device factory named "Hwa.Omap" so that
// User::LoadPhysicalDevice(_L("EHWAOMAP")) succeeds (hack H-dspbridge, docs/hacks.md). The "Hwa"
// stand-in LDD does not ask for a physical device, so Create() is never called.
#include <kernel/kernel.h>

class DHwaOmapPdd : public DPhysicalDevice
	{
public:
	DHwaOmapPdd() { iVersion = TVersion(255, 255, 0xffff); }
	virtual TInt Install() { _LIT(KName, "Hwa.Omap"); Kern::Printf("SYBSTUB: install Hwa.Omap (PDD, load only)"); return SetName(&KName); }
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DBase*& aChannel, TInt, const TDesC8*, const TVersion&) { aChannel = NULL; return KErrNotSupported; }
	virtual TInt Validate(TInt, const TDesC8*, const TVersion&) { return KErrNone; }
	};

DECLARE_STANDARD_PDD()
	{
	return new DHwaOmapPdd;
	}
