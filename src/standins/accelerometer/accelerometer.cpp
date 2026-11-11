// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// accelerometer.ldd stand-in (N8 Symbian^3, 701/808 Belle: Nokia's accelerometer.ldd/.pdd are kernel side
// and not in our ROM). The phone's own sensor plug-in accssy.dll (sensorserver.exe) opens channel
// "accelerometer" (PDD "accelerometer.pdd", version 2.0.0x861) and streams XYZ samples from request 3;
// orientationssy turns them into DisplayUp / DisplayRightUp and the automatic rotation rules
// (autorotateui_*.rul) turn the UI. Protocol: docs/devices/nokia-n8-rm596/accelerometer.md.
//
// Forwarding only: the vector is the host's (QEMU syborg-sensor at KHwBaseSensor: the emulator window's
// Turn view, sydb accel); this driver reads it every sample and writes it to the client as the Nokia
// driver's record { TInt64 time; x; y; z; 0 }.
//   controls: 0 = the driver information block, copied from the host device (its INFO registers: the
//   profile's [board.accelerometer] info: chip id, data-rate and range tables, channel bits); 2 (config of
//   an id) answers zeros, 1 (enable/power) and 3 (set config) are accepted [V: the stream then runs];
//   requests: 1 = synchronous start, completed at once with KErrNone (accssy waits for it); 7 (Belle) =
//   accssy's own orientation channel: every KSampleMs {zenith, azimuth} in degrees from the host vector;
//   3 = the sample stream (completed every KSampleMs with the current vector); 2, 4..7 stay
//   pending until cancelled (DoCancel(n), n = request number, (index << 5) | 6 for request 6);
//   0 and > 7: KErrNotSupported.

#include <kernel/kern_priv.h>
#include "syborg.h"

enum { KRegId = 0, KRegX = 1, KRegY = 2, KRegZ = 3, KRegSeq = 4, KRegInfoLen = 5, KRegInfo = 0x40 };
const TUint32 KSensorId = 0xc51d0030;
const TInt KSampleMs = 40;
const TInt KRecordBytes = 24;
const TInt KMaxReq = 8;
const TInt KCtl0Bytes = 248;

static inline TUint32 Reg(TInt aReg) { return ReadReg(KHwBaseSensor, aReg); }

// angle of (aX, aY) from +x towards +y in whole degrees, 0..359 (no floating point in the kernel):
// atan(t) ~= 45 t + 15.5 t (1 - t) degrees for t in [0, 1] (error < 0.3 degrees)
static TInt Atan2Deg(TInt64 aY, TInt64 aX)
	{
	if (!aX && !aY)
		return 0;
	TInt64 ax = aX < 0 ? -aX : aX, ay = aY < 0 ? -aY : aY;
	TBool steep = ay > ax;
	TInt64 t = steep ? (ax << 10) / ay : (ay << 10) / ax;		// 0..1024
	TInt64 q = 45 * t * 1024 + (155 * t * (1024 - t)) / 10;	// degrees << 20
	TInt a = (TInt)((q + (1 << 19)) >> 20);
	if (steep)
		a = 90 - a;
	if (aX < 0)
		a = 180 - a;
	if (aY < 0)
		a = 360 - a;
	return a % 360;
	}

static TInt64 ISqrt64(TInt64 aV)
	{
	TInt64 r = 0, bit = (TInt64)1 << 62;
	while (bit > aV)
		bit >>= 2;
	while (bit)
		{
		if (aV >= r + bit)
			{
			aV -= r + bit;
			r = (r >> 1) + bit;
			}
		else
			r >>= 1;
		bit >>= 2;
		}
	return r;
	}

class DAccDevice : public DLogicalDevice
	{
public:
	DAccDevice();
	virtual TInt Install();
	virtual void GetCaps(TDes8& aDes) const;
	virtual TInt Create(DLogicalChannelBase*& aChannel);
	};

class DAccChannel : public DLogicalChannel
	{
public:
	DAccChannel();
	virtual ~DAccChannel();
protected:
	virtual TInt DoCreate(TInt aUnit, const TDesC8* aInfo, const TVersion& aVer);
	virtual void HandleMsg(TMessageBase* aMsg);
private:
	TInt DoControl(TInt aFn, TAny* a1, TAny* a2);
	TInt DoRequest(TInt aReq, TRequestStatus* aStatus, TAny* a1, TAny* a2);
	void DoCancel(TUint aReq);
	void Complete(TInt aReq, TInt aResult);
	TInt WriteZeros(TAny* aDes);
	TInt WriteInfo(TAny* aDes);
	static void TimerFn(TAny* aPtr);
	static void SampleDfcFn(TAny* aPtr);
	void Sample();
private:
	DThread* iClient;
	TRequestStatus* iStatus[KMaxReq];
	TAny* iDes;					// request 3's TDes8
	NTimer iTimer;
	TDfc iSampleDfc;
	TBool iHw;
	TAny* iZen;					// request 7's output words
	TAny* iAzi;
	};

DAccDevice::DAccDevice()
	{
	iVersion = TVersion(2, 9, 0xffff);			// accssy asks 2.0.0x861 (N8), major 2 (Belle)
	iParseMask = KDeviceAllowUnit | KDeviceAllowPhysicalDevice;
	iUnitsMask = 1;
	}

TInt DAccDevice::Install()
	{
	_LIT(KName, "accelerometer");
	Kern::Printf("SYBSTUB: install accelerometer (sensor %s)", Reg(KRegId) == KSensorId ? "present" : "ABSENT");
	return SetName(&KName);
	}

void DAccDevice::GetCaps(TDes8& aDes) const
	{
	TVersion v = iVersion;
	Kern::InfoCopy(aDes, (TUint8*)&v, sizeof(v));
	}

TInt DAccDevice::Create(DLogicalChannelBase*& aChannel)
	{
	aChannel = new DAccChannel;
	return aChannel ? KErrNone : KErrNoMemory;
	}

DAccChannel::DAccChannel()
	: iTimer(TimerFn, this), iSampleDfc(SampleDfcFn, this, 1)
	{
	iClient = &Kern::CurrentThread();
	iClient->Open();
	}

DAccChannel::~DAccChannel()
	{
	iTimer.Cancel();
	iSampleDfc.Cancel();
	for (TInt i = 0; i < KMaxReq; ++i)
		Complete(i, KErrCancel);
	Kern::SafeClose((DObject*&)iClient, NULL);
	}

TInt DAccChannel::DoCreate(TInt aUnit, const TDesC8*, const TVersion& aVer)
	{
	iHw = Reg(KRegId) == KSensorId;
	Kern::Printf("SYBSTUB: accelerometer open unit %d ver %d.%d.%d by %O%s", aUnit, aVer.iMajor, aVer.iMinor,
				 aVer.iBuild, iClient, iHw ? "" : " (no sensor device: portrait)");
	SetDfcQ(Kern::DfcQue0());
	iSampleDfc.SetDfcQ(iDfcQ);
	iMsgQ.Receive();
	return KErrNone;
	}

void DAccChannel::HandleMsg(TMessageBase* aMsg)
	{
	TThreadMessage& m = *(TThreadMessage*)aMsg;
	TInt id = m.iValue;
	if (id == (TInt)ECloseMsg)
		{
		iTimer.Cancel();
		iSampleDfc.Cancel();
		for (TInt i = 0; i < KMaxReq; ++i)
			Complete(i, KErrCancel);
		m.Complete(KErrNone, EFalse);
		return;
		}
	if (id == KMaxTInt)
		{
		DoCancel(m.Int0());
		m.Complete(KErrNone, ETrue);
		return;
		}
	if (id < 0)
		{
		TRequestStatus* s = (TRequestStatus*)m.Ptr0();
		TInt r = DoRequest(~id, s, m.Ptr1(), m.Ptr2());
		if (r != KErrNone)
			Kern::RequestComplete(iClient, s, r);
		m.Complete(KErrNone, ETrue);
		return;
		}
	m.Complete(DoControl(id, m.Ptr0(), m.Ptr1()), ETrue);
	}

TInt DAccChannel::WriteZeros(TAny* aDes)
	{
	if (!aDes)
		return KErrArgument;
	TInt max = Kern::ThreadGetDesMaxLength(iClient, aDes);
	if (max < 0)
		return max;
	TBuf8<KCtl0Bytes> z;
	z.FillZ(Min(max, KCtl0Bytes));
	return Kern::ThreadDesWrite(iClient, aDes, z, 0, KChunkShiftBy0, iClient);
	}

TInt DAccChannel::WriteInfo(TAny* aDes)
	{
	if (!iHw)
		return WriteZeros(aDes);
	TInt max = Kern::ThreadGetDesMaxLength(iClient, aDes);
	if (max < 0)
		return max;
	TUint32 w[KCtl0Bytes / 4];
	TInt len = Min(Min((TInt)Reg(KRegInfoLen), KCtl0Bytes), max) & ~3;
	for (TInt i = 0; i < len / 4; ++i)
		w[i] = Reg(KRegInfo + i);
	TPtrC8 p((const TUint8*)w, len);
	return Kern::ThreadDesWrite(iClient, aDes, p, 0, KChunkShiftBy0, iClient);
	}

TInt DAccChannel::DoControl(TInt aFn, TAny* a1, TAny* a2)
	{
	TInt r;
	switch (aFn)
		{
	case 0: r = WriteInfo(a1); break;				// driver information block (the host's)
	case 1: r = KErrNone; break;					// power / mode [inferred]
	case 2: r = WriteZeros(a2); break;				// get config of id *a1 [inferred]
	case 3: r = KErrNone; break;					// set config of id *a1 [inferred]
	default: r = KErrNotSupported; break;
		}
	TUint32 w0 = 0, w1 = 0;
	if (a1) Kern::ThreadRawRead(iClient, a1, &w0, 4);
	Kern::Printf("SYBSTUB: accelerometer ctl %d a1=%08x [%08x] a2=%08x -> %d", aFn, a1, w0, a2, r);
	(void)w1;
	return r;
	}

TInt DAccChannel::DoRequest(TInt aReq, TRequestStatus* aStatus, TAny* a1, TAny* a2)
	{
	if (aReq != 3 && aReq != 7)	// requests 3 and 7 are the 25 Hz sample streams: not logged
		Kern::Printf("SYBSTUB: accelerometer request %d args %08x %08x", aReq, a1, a2);
	if (aReq < 1 || aReq >= KMaxReq)
		return KErrNotSupported;
	if (aReq == 6)
		{
		TUint8 idx = 0;
		if (a2 && Kern::ThreadRawRead(iClient, a2, &idx, 1) == KErrNone && idx >= 4)
			return KErrArgument;
		}
	if (aReq == 1)		// synchronous start: accssy waits for it (User::WaitForRequest at 0x80b730c2) and treats
		{				// non-zero as failure [V N8 accssy]
		Kern::RequestComplete(iClient, aStatus, KErrNone);
		return KErrNone;
		}
	if (iStatus[aReq])
		return KErrInUse;
	iStatus[aReq] = aStatus;
	if (aReq == 3)
		iDes = a1;
	if (aReq == 7)		// Belle: accssy's own orientation channel, a stream of {TInt zenith, TInt azimuth} in degrees
		{				// (a1, a2), one per completion (RunL 0x80b1ed90) [V Delight, static]
		iZen = a1;
		iAzi = a2;
		}
	if ((aReq == 3 || aReq == 7) && !iTimer.IsPending())
		iTimer.OneShot(NKern::TimerTicks(KSampleMs));	// ISR callback: TimerFn adds the DFC
	return KErrNone;
	}

void DAccChannel::DoCancel(TUint aReq)
	{
	TInt n = aReq & 0x1f;
	Kern::Printf("SYBSTUB: accelerometer cancel %x", aReq);
	if (n > 0 && n < KMaxReq)
		Complete(n, KErrCancel);
	if ((n == 3 || n == 7) && !iStatus[3] && !iStatus[7])
		iTimer.Cancel();
	}

void DAccChannel::Complete(TInt aReq, TInt aResult)
	{
	if (iStatus[aReq])
		Kern::RequestComplete(iClient, iStatus[aReq], aResult);	// nulls iStatus[aReq]
	}

void DAccChannel::TimerFn(TAny* aPtr)
	{
	((DAccChannel*)aPtr)->iSampleDfc.Add();
	}

void DAccChannel::SampleDfcFn(TAny* aPtr)
	{
	((DAccChannel*)aPtr)->Sample();
	}

void DAccChannel::Sample()
	{
	if (!iStatus[3] && !iStatus[7])
		return;
	TInt32 v[3] = { 0, 1000, 0 };
	if (iHw)
		for (TInt tries = 0; tries < 4; ++tries)	// the host sets the vector at once; reread if it moved
			{
			TUint32 seq = Reg(KRegSeq);
			v[0] = (TInt32)Reg(KRegX);
			v[1] = (TInt32)Reg(KRegY);
			v[2] = (TInt32)Reg(KRegZ);
			if (Reg(KRegSeq) == seq)
				break;
			}
	if (iStatus[7])
		{
		TInt64 hz = (TInt64)v[0] * v[0] + (TInt64)v[1] * v[1];
		TInt zen = Atan2Deg(ISqrt64(hz), v[2]);		// 0..180, 0 = face up
		TInt azi = Atan2Deg(v[1], v[0]);			// angle of (x, y) from +x towards +y, 0..359
		TInt r7 = Kern::ThreadRawWrite(iClient, iZen, &zen, 4);
		if (r7 == KErrNone)
			r7 = Kern::ThreadRawWrite(iClient, iAzi, &azi, 4);
		Complete(7, r7);
		}
	if (!iStatus[3])
		{
		if (iStatus[7] == NULL)
			return;
		}
	TInt64 t = Kern::SystemTime();
	TUint32 rec[KRecordBytes / 4] = { I64LOW(t), I64HIGH(t), (TUint32)v[0], (TUint32)v[1], (TUint32)v[2], 0 };
	if (!iStatus[3])
		return;
	TInt max = Kern::ThreadGetDesMaxLength(iClient, iDes);
	TInt r;
	if (max < 0)
		r = max;
	else if (max < KRecordBytes)
		r = KErrNoMemory;							// as Nokia's driver: no room for one record
	else
		{
		TPtrC8 p((const TUint8*)rec, KRecordBytes);
		r = Kern::ThreadDesWrite(iClient, iDes, p, 0, KChunkShiftBy0, iClient);
		}
	Complete(3, r);
	}

DECLARE_STANDARD_LDD()
	{
	return new DAccDevice;
	}
