// ssi.pdd stand-in: the N73's ISC data transmission PDD (resource/isc/isc_config.ini
// "DataTransmissionPdd=ssi.pdd"; IscApi loads it before xbus.ldd). Our modem link is the fake peer
// in iscdriver.ldd, so this is an empty physical-device factory that only has to load.
#include <kernel/kernel.h>

class DSsiPdd : public DPhysicalDevice
	{
public:
	DSsiPdd() { iVersion = TVersion(255, 255, 0xffff); }
	virtual TInt Install() { _LIT(KName, "Ssi.pdd"); Kern::Printf("N95STUB: install Ssi.pdd (PDD, load only)"); return SetName(&KName); }
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DBase*& aChannel, TInt, const TDesC8*, const TVersion&) { aChannel = NULL; return KErrNotSupported; }
	virtual TInt Validate(TInt, const TDesC8*, const TVersion&) { return KErrNone; }
	};

DECLARE_STANDARD_PDD()
	{
	return new DSsiPdd;
	}
