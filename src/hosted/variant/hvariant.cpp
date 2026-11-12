// Hosted EKA2: a minimal variant (the board layer: class Asic of kernel/arm/assp.h) for kernel tests on
// the hosted platform: 1 ms tick from the nanokernel's interrupt thread, debug output to fd 1, host
// clock as the real-time clock. The phone's own variant (our syborg baseport + stand-ins) replaces it
// later. Original work for qembian.
#include <kernel/kern_priv.h>
#include <assp.h>
#include <u32hal.h>
#include <hal_data.h>

extern void HostedStartSystemTimer(TInt aPeriodUs);
extern void HostedSetVariant(TInt (*aEntry)(TInt), Asic* (*aInitialise)(TBool));

class HostedAsic : public Asic
	{
public:
	virtual TMachineStartupType StartupReason() { return EStartupColdReset; }
	virtual void Init1() {}
	virtual void Init3() { HostedStartSystemTimer(1000); }
	virtual void DebugOutput(TUint aChar)
		{
		iLine[iLen++] = (char)aChar;
		if (aChar == '\n' || iLen == sizeof(iLine))
			{
			hwrite(1, iLine, iLen);
			iLen = 0;
			}
		}
	virtual void Idle() {}
	virtual TInt MsTickPeriod() { return 1000; }
	virtual TInt SystemTimeInSecondsFrom2000(TInt& aTime)
		{
		struct { TInt32 s, ns; } ts;
		hsys(SYS_clock_gettime, 0 /* CLOCK_REALTIME */, &ts);
		aTime = ts.s - 946684800;		// 1970 -> 2000
		return KErrNone;
		}
	virtual TInt SetSystemTimeInSecondsFrom2000(TInt) { return KErrNotSupported; }
	virtual TUint32 NanoWaitCalibration() { return 1; }
	virtual TInt VariantHal(TInt aFunction, TAny* a1, TAny* a2);
	virtual TPtr8 MachineConfiguration() { return TPtr8(iMachineConfig, sizeof(iMachineConfig), sizeof(iMachineConfig)); }
private:
	char iLine[256];
	TInt iLen;
	TUint8 iMachineConfig[256];
	};

static HostedAsic TheHostedAsic;

static TInt VariantEntry(TInt) { return KErrNone; }
static Asic* VariantInitialise(TBool) { return &TheHostedAsic; }

void HostedUseTestVariant()
	{
	HostedSetVariant(&VariantEntry, &VariantInitialise);
	}

// The variant HAL as our Syborg variant answers it (Syborg::VariantHal, baseport syborg/specific/assp.cpp,
// EPL v1.0, Nokia): the phone's servers ask it at start-up. Bug 2026-11-12: KErrNotSupported for
// EVariantHalVariantInfo made FbServ leave (-5) and panic (FBSERV -15), and the window server with it
// (kn_efile).
TInt HostedAsic::VariantHal(TInt aFunction, TAny* a1, TAny*)
	{
	switch (aFunction)
		{
	case EVariantHalVariantInfo:
		{
		TVariantInfoV01Buf infoBuf;
		TVariantInfoV01& info = infoBuf();
		info.iRomVersion = Epoc::RomHeader().iVersion;
		info.iMachineUniqueId = (TInt64(HALData::EMachineUid_OmapH4) << 32);
		info.iLedCapabilities = (8 << 16) + 1;		// KLedMaskGreen1
		info.iProcessorClockInKHz = 0;
		info.iSpeedFactor = 0;
		Kern::InfoCopy(*(TDes8*)a1, infoBuf);
		return KErrNone;
		}
	case EVariantHalDebugPortSet:
		{
		TUint32 port = (TUint32)a1;
		if (port > 3 && port != (TUint32)KNullDebugPort)
			return KErrNotSupported;
		Kern::SuperPage().iDebugPort = port;		// output stays on the host console
		return KErrNone;
		}
	case EVariantHalDebugPortGet:
		{
		TUint32 port = Kern::SuperPage().iDebugPort;
		kumemput32(a1, &port, sizeof(port));
		return KErrNone;
		}
	case EVariantHalSwitches:
	case EVariantHalLedMaskGet:
		{
		TUint32 x = 0;
		kumemput32(a1, &x, sizeof(x));
		return KErrNone;
		}
	case EVariantHalLedMaskSet:
		return KErrNone;
	case EVariantHalCustomRestartReason:
		{
		TInt x = Kern::SuperPage().iHwStartupReason;
		kumemput32(a1, &x, sizeof(x));
		return KErrNone;
		}
	case EVariantHalCustomRestart:
		if (!Kern::CurrentThreadHasCapability(ECapabilityPowerMgmt,
				__PLATSEC_DIAGNOSTIC_STRING("Checked by Hal function EVariantHalCustomRestart")))
			return KErrPermissionDenied;
		Kern::Restart(0);
		return KErrNone;
	default:
		return KErrNotSupported;
		}
	}
