// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// sybeth.pdd: the Ethernet physical device for QEMU's syborg-net (src/qemu-syborg/hw/arm/syborg_net.c), a
// mailbox NIC: frames are copied through a TX and an RX window, no DMA. Under the EPL enet.ldd (DEthernet
// interface, drivers/ethernet.h), which etherpkt.drv opens (CommsDat LANService: LDD "enet"/"Ethernet",
// PDD "sybeth"/"Ethernet.Syborg").
//
// Receive: the device interrupts (level) while a frame waits. The ISR masks the device and queues a DFC;
// the DFC hands every waiting frame to the LDD (ReceiveIsr -> ReceiveFrame copies it, RX_NEXT) and unmasks.
// Send: the LDD's DFC thread calls Send; the frame goes into the TX window and TX_LEN sends it at once.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <drivers/ethernet.h>
#include "syborg.h"

namespace
	{
	enum { RId = 0x000, RStatus = 0x004, RIntEnable = 0x008, RTxLen = 0x00c, RRxLen = 0x010, RRxNext = 0x014,
	       RRxDropped = 0x018, RMac = 0x100, RTxWin = 0x200, RRxWin = 0x800 };
	const TUint32 KIdNet = 0xc51d0040;
	const TInt KWin = 1536;
	const TInt KFrameMax = 1514;
	_LIT(KPddName, "Ethernet.Syborg");

	inline volatile TUint32& Reg(TUint aOff) { return *(volatile TUint32*)(KHwSVPNetDevice + aOff); }
	inline volatile TUint8& Byte(TUint aOff) { return *(volatile TUint8*)(KHwSVPNetDevice + aOff); }
	}

class DSybEthernet : public DEthernet
	{
public:
	DSybEthernet();
	~DSybEthernet();
	TInt DoCreate();
	virtual TInt Start();
	virtual void Stop(TStopMode aMode);
	virtual TInt ValidateConfig(const TEthernetConfigV01& aConfig) const;
	virtual TInt Configure(TEthernetConfigV01& aConfig);
	virtual void MacConfigure(TEthernetConfigV01& aConfig);
	virtual void GetConfig(TEthernetConfigV01& aConfig) const;
	virtual void CheckConfig(TEthernetConfigV01& aConfig);
	virtual void Caps(TDes8& aCaps) const;
	virtual TInt Send(TBuf8<KMaxEthernetPacket+32>& aBuffer);
	virtual TInt ReceiveFrame(TBuf8<KMaxEthernetPacket+32>& aBuffer, TBool aOkToUse);
	virtual TInt DisableIrqs();
	virtual void RestoreIrqs(TInt aLevel);
	virtual TDfcQue* DfcQ(TInt aUnit);
#ifdef ETH_CHIP_IO_ENABLED
	virtual TInt BgeChipIOCtrl(TPckgBuf<TChipIOInfo>&) { return KErrNotSupported; }
#endif
private:
	static void Isr(TAny* aPtr);
	static void RxDfcFn(TAny* aPtr);
	void RxDfc();
	TEthernetConfigV01 iConfig;
	TDynamicDfcQue* iDfcQ;
	TDfc iRxDfc;
	TBool iBound, iStarted;
	};

DSybEthernet::DSybEthernet()
	: iRxDfc(RxDfcFn, this, 1)
	{
	}

DSybEthernet::~DSybEthernet()
	{
	Reg(RIntEnable) = 0;
	if (iBound)
		Interrupt::Unbind(EIntNet0);
	iRxDfc.Cancel();
	if (iDfcQ)
		iDfcQ->Destroy();
	}

TInt DSybEthernet::DoCreate()
	{
	if (Reg(RId) != KIdNet)
		{
		Kern::Printf("SYBSTUB: sybeth no syborg-net (id %08x)", (TUint32)Reg(RId));
		return KErrHardwareNotAvailable;
		}
	TInt r = Kern::DynamicDfcQCreate(iDfcQ, 27, _L("SybEthDfc"));
	if (r != KErrNone)
		return r;
	iRxDfc.SetDfcQ(iDfcQ);
	iConfig.iEthSpeed = KEthSpeed100BaseTX;
	iConfig.iEthDuplex = KEthDuplexFull;
	for (TInt i = 0; i < 6; i++)
		iConfig.iEthAddress[i] = Byte(RMac + i);
	r = Interrupt::Bind(EIntNet0, Isr, this);
	if (r < 0)
		return r;
	iBound = ETrue;
	Kern::Printf("SYBSTUB: sybeth MAC %02x:%02x:%02x:%02x:%02x:%02x, link %d", iConfig.iEthAddress[0],
		iConfig.iEthAddress[1], iConfig.iEthAddress[2], iConfig.iEthAddress[3], iConfig.iEthAddress[4],
		iConfig.iEthAddress[5], (Reg(RStatus) >> 1) & 1);
	return KErrNone;
	}

TInt DSybEthernet::Start()
	{
	if (!iStarted)
		{
		iStarted = ETrue;
		Reg(RIntEnable) = 1;
		Interrupt::Enable(EIntNet0);
		}
	return KErrNone;
	}

void DSybEthernet::Stop(TStopMode)
	{
	iStarted = EFalse;
	Reg(RIntEnable) = 0;
	Interrupt::Disable(EIntNet0);
	iRxDfc.Cancel();
	}

TInt DSybEthernet::ValidateConfig(const TEthernetConfigV01& aConfig) const
	{
	return aConfig.iEthSpeed == KEthSpeedUnknown || aConfig.iEthDuplex == KEthDuplexUnknown ? KErrNotSupported : KErrNone;
	}

TInt DSybEthernet::Configure(TEthernetConfigV01&) { return KErrNone; }

void DSybEthernet::MacConfigure(TEthernetConfigV01& aConfig)
	{
	for (TInt i = 0; i < 6; i++)		// the host NIC takes any MAC: slirp answers whatever the guest uses
		iConfig.iEthAddress[i] = aConfig.iEthAddress[i];
	}

void DSybEthernet::GetConfig(TEthernetConfigV01& aConfig) const { aConfig = iConfig; }
void DSybEthernet::CheckConfig(TEthernetConfigV01&) {}

void DSybEthernet::Caps(TDes8& aCaps) const
	{
	TEthernetCaps caps;
	aCaps.FillZ(aCaps.MaxLength());
	aCaps = caps.Left(Min(caps.Length(), aCaps.MaxLength()));
	}

TInt DSybEthernet::Send(TBuf8<KMaxEthernetPacket+32>& aBuffer)
	{
	TInt n = aBuffer.Length();
	if (n < 14 || n > KFrameMax)
		return KErrArgument;
	const TUint8* p = aBuffer.Ptr();
	TInt i = 0;
	for (; i + 4 <= n; i += 4)
		Reg(RTxWin + i) = p[i] | p[i + 1] << 8 | p[i + 2] << 16 | (TUint32)p[i + 3] << 24;
	for (; i < n; i++)
		Byte(RTxWin + i) = p[i];
	Reg(RTxLen) = n;
	return KErrNone;
	}

// from ReceiveIsr (the LDD), in our DFC: copy the waiting frame (or drop it when the LDD has no buffer)
TInt DSybEthernet::ReceiveFrame(TBuf8<KMaxEthernetPacket+32>& aBuffer, TBool aOkToUse)
	{
	TInt n = Reg(RRxLen);
	if (aOkToUse && n > 0 && n <= KWin)
		{
		aBuffer.SetLength(n);
		TUint8* p = (TUint8*)aBuffer.Ptr();
		TInt i = 0;
		for (; i + 4 <= n; i += 4)
			{
			TUint32 w = Reg(RRxWin + i);
			p[i] = (TUint8)w; p[i + 1] = (TUint8)(w >> 8); p[i + 2] = (TUint8)(w >> 16); p[i + 3] = (TUint8)(w >> 24);
			}
		for (; i < n; i++)
			p[i] = Byte(RRxWin + i);
		}
	Reg(RRxNext) = 1;
	return aOkToUse && n > 0 ? KErrNone : KErrGeneral;
	}

TInt DSybEthernet::DisableIrqs() { return NKern::DisableInterrupts(1); }
void DSybEthernet::RestoreIrqs(TInt aLevel) { NKern::RestoreInterrupts(aLevel); }
TDfcQue* DSybEthernet::DfcQ(TInt) { return iDfcQ; }

void DSybEthernet::Isr(TAny* aPtr)
	{
	DSybEthernet* d = (DSybEthernet*)aPtr;
	Reg(RIntEnable) = 0;				// level interrupt: masked until the DFC drained the queue
	Interrupt::Clear(EIntNet0);
	d->iRxDfc.Add();
	}

void DSybEthernet::RxDfcFn(TAny* aPtr)
	{
	((DSybEthernet*)aPtr)->RxDfc();
	}

void DSybEthernet::RxDfc()
	{
	for (TInt k = 0; k < 64 && (Reg(RStatus) & 1); k++)
		ReceiveIsr();
	if (iStarted)
		Reg(RIntEnable) = 1;			// still a frame waiting (more than 64): interrupts again at once
	}

class DSybEthernetPdd : public DPhysicalDevice
	{
public:
	DSybEthernetPdd()
		{
		iUnitsMask = 1;
		iVersion = TVersion(KEthernetMajorVersionNumber, KEthernetMinorVersionNumber, KEthernetBuildVersionNumber);
		}
	virtual TInt Install() { return SetName(&KPddName); }
	virtual void GetCaps(TDes8& aDes) const
		{
		TEthernetCaps caps;
		aDes.FillZ(aDes.MaxLength());
		aDes = caps.Left(Min(caps.Length(), aDes.MaxLength()));
		}
	virtual TInt Create(DBase*& aChannel, TInt, const TDesC8*, const TVersion&)
		{
		DSybEthernet* d = new DSybEthernet;
		aChannel = d;
		return d ? d->DoCreate() : KErrNoMemory;
		}
	virtual TInt Validate(TInt aUnit, const TDesC8*, const TVersion& aVer)
		{
		if (aUnit != 0 || !Kern::QueryVersionSupported(iVersion, aVer))
			return KErrNotSupported;
		return KErrNone;
		}
	};

DECLARE_STANDARD_PDD()
	{
	return new DSybEthernetPdd;
	}
