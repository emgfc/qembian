// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// EXYIN.DLL stand-in: the touch screen of single-touch phones (5800 XpressMusic: Nokia's exyin.dll) as
// a Symbian digitiser (EHalGroupDigitiser, TRawEvent button-1 down / move / up) on the Syborg board's
// absolute pointer device (syborg-pointer, the same registers the baseport's EPOINTER mouse driver
// reads; a touch ROM leaves EPOINTER out: kernel.omit_baseport). The generic part is the kernel's own
// DDigitiser (kernel/eka/drivers/xyin/d_xyin.cpp); this file is the hardware part, shaped after the
// EPL bsptemplate xyin.cpp.
//
// v2 (docs/research/touch-driver-design.md): forwarding only. The device (syborg-pointer v2) queues one
// typed entry per host sample {x, y, DOWN/UP/MOVE, time}; the interrupt drains them into a ring in order
// and a DFC issues each as a raw event (EButton1Down / EPointerMove / EButton1Up) with Kern::AddEvent:
// no 10 ms resampling, no delay line, the release at the release point. The kernel's raw-event queue
// holds 40 events and AddEvent answers KErrOverflow when it is full (the generic DDigitiser ignored that,
// so long swipes lost their pen-up: a "pressed" state that stuck, a next swipe that joined the old
// drag); here a down or an up that does not fit waits and is retried every 2 ms, in order, and only a
// move that a newer move follows may be dropped. DDigitiser stays the base for the HAL (calibration,
// power, info); its sampling state machine is never fed.
// Raw digitiser units are 12 bit (0..4095); the default calibration maps them onto the display size
// the LCD HAL reports, so no calibration is needed. Board config (docs/devices.md [[standins]] config):
//   exyin.rotate = 0|90|180|270   turn host coordinates before calibration (panel vs UI orientation)
//   exyin.trace = 1               every issued event: SYBSTUB: exyin ev D|M|U x,y ts

#include <drivers/xyin.h>
#include <kernel/kern_priv.h>
#include <videodriver.h>
#include "syborg.h"
#include "boardconfig.h"

enum { KRegId = 0, KRegLatch = 1, KRegFifoCount = 2, KRegX = 3, KRegY = 4, KRegZ = 5, KRegButtons = 6,
	   KRegIntEnable = 7, KRegFifoSize = 8, KRegVersion = 9, KRegCaps = 10, KRegSlot = 11, KRegTsLo = 12,
	   KRegTsHi = 13, KRegFlags = 14, KRegStats = 15 };
enum { KFlagDown = 1, KFlagUp = 2, KFlagMove = 4, KFlagCoalesced = 8, KFlagCancel = 16 };
const TInt KRawBits = 12;
const TInt KRawMax = (1 << KRawBits) - 1;
const TInt KRing = 256;
const TInt KRetryMs = 2;

static inline TUint32 Reg(TInt aReg) { return ReadReg(KHwBaseKmiPointer, aReg); }

struct TSample { TInt iX, iY; TUint32 iTs; TUint8 iSlot, iFlags; };

NONSHARABLE_CLASS(DSybDigitiser) : public DDigitiser
	{
public:
	DSybDigitiser();
	virtual TInt DoCreate();
	virtual void WaitForPenDown() {}
	virtual void WaitForPenUp() {}
	virtual void WaitForPenUpDebounce() {}
	virtual void DigitiserOn();
	virtual void DigitiserOff();
	virtual void FilterPenMove(const TPoint&) {}
	virtual void ResetPenMoveFilter() {}
	virtual TInt DigitiserToScreen(const TPoint& aDigitiserPoint, TPoint& aScreenPoint);
	virtual void ScreenToDigitiser(TInt& aX, TInt& aY);
	virtual TInt SetXYInputCalibration(const TDigitizerCalibration& aCalibration);
	virtual TInt CalibrationPoints(TDigitizerCalibration& aCalibration);
	virtual TInt SaveXYInputCalibration();
	virtual TInt RestoreXYInputCalibration(TDigitizerCalibrationType aType);
	virtual void DigitiserInfo(TDigitiserInfoV01& aInfo);
	virtual void PowerDown(TPowerState);
	virtual void PowerUp();
	static void Isr(TAny* aPtr);
	static void DeliverDfcFn(TAny* aPtr);
	static void RetryExpired(TAny* aPtr);
	void Push(const TSample& aS);
	TBool Pop(TSample& aS);
	void Deliver();
	TInt Issue(const TSample& aS);
	TSize ScreenSize();
	void DefaultCalibration(TDigitizerCalibration& aCal, TSize aSize);
	struct TCal { TInt iR11, iR12, iR21, iR22, iTx, iTy; };
public:
	TDfc iDeliverDfc;
	NTimer iRetry;
	TSample iRing[KRing];
	TInt iHead, iCount;						// ring: the ISR appends, the DFC pops under NKern::Lock
	TSample iParked;						// a down/up (or the last move) the window server had no room for
	TBool iHaveParked;
	TPoint iLast;							// the last issued screen point
	TBool iDown;							// a press issued and not yet released
	TInt iRotate, iTrace;
	TInt iOverflows, iPressEvents, iPressStart, iDropped;
	TCal iCal, iCalSaved, iCalFactory;
	};

DDigitiser* DDigitiser::New()
	{
	return new DSybDigitiser;
	}

DSybDigitiser::DSybDigitiser()
	: iDeliverDfc(DeliverDfcFn, this, 5), iRetry(RetryExpired, this)
	{
	}

TSize DSybDigitiser::ScreenSize()
	{
	TVideoInfoV01Buf buf;
	if (Kern::HalFunction(EHalGroupDisplay, EDisplayHalCurrentModeInfo, (TAny*)&buf, NULL) == KErrNone)
		return buf().iSizeInPixels;
	TSize s;
	s.iWidth = 360;
	s.iHeight = 640;
	return s;
	}

TInt DSybDigitiser::DoCreate()
	{
	// own DFC thread: on the shared DfcQue0 other drivers' work delayed touch events
	TDynamicDfcQue* q;
	TInt e = Kern::DynamicDfcQCreate(q, 27, _L8("ExyinDfc"));
	if (e != KErrNone)
		return e;
	iDfcQ = q;
	iDeliverDfc.SetDfcQ(iDfcQ);
	iRotate = BoardConfigInt("exyin.rotate", 0);
	iTrace = BoardConfigInt("exyin.trace", 0);
	TSize s = ScreenSize();
	TInt w = s.iWidth, h = s.iHeight;
	iCal.iR11 = (w << 16) / (KRawMax + 1); iCal.iR12 = 0;
	iCal.iR21 = 0; iCal.iR22 = (h << 16) / (KRawMax + 1);
	iCal.iTx = 0; iCal.iTy = 0;
	iCalSaved = iCalFactory = iCal;
	iCfg.iPenDownDiscard = 0;
	iCfg.iPenUpDiscard = 1;			// >= 1: DDigitiser::Create allocates its (unused) delay line
	iCfg.iDriveXRise = 0;
	iCfg.iDriveYRise = 0;
	iCfg.iMinX = 0;
	iCfg.iMaxX = KRawMax;
	iCfg.iSpreadX = KRawMax + 1;
	iCfg.iMinY = 0;
	iCfg.iMaxY = KRawMax;
	iCfg.iSpreadY = KRawMax + 1;
	iCfg.iMaxJumpX = 0;
	iCfg.iMaxJumpY = 0;
	iCfg.iAccThresholdX = 1;
	iCfg.iAccThresholdY = 1;
	iCfg.iNumXYSamples = 1;
	iCfg.iDisregardMinMax = EFalse;
	Add();								// power handler
	TInt r = Interrupt::Bind(EIntPointer, Isr, this);
	if (r != KErrNone)
		return r;
	Interrupt::Enable(EIntPointer);
	WriteReg(KHwBaseKmiPointer, KRegIntEnable, 1);
	Kern::Printf("SYBSTUB: exyin digitiser %dx%d, raw %d bit, rotate %d, pointer id %08x v%d", w, h, KRawBits, iRotate,
				 Reg(KRegId), Reg(KRegVersion));
	return KErrNone;
	}

// interrupt: every queued entry into the ring, in order
void DSybDigitiser::Isr(TAny* aPtr)
	{
	DSybDigitiser& d = *(DSybDigitiser*)aPtr;
	TBool v2 = Reg(KRegVersion) >= 2;
	while (Reg(KRegFifoCount))
		{
		WriteReg(KHwBaseKmiPointer, KRegLatch, 1);
		TSample s;
		s.iX = (TInt)Reg(KRegX) >> (15 - KRawBits);
		s.iY = (TInt)Reg(KRegY) >> (15 - KRawBits);
		s.iSlot = v2 ? (TUint8)Reg(KRegSlot) : 0;
		s.iTs = v2 ? Reg(KRegTsLo) : 0;
		if (v2)
			s.iFlags = (TUint8)Reg(KRegFlags);
		else
			{							// a v1 device: flags from the button edges
			TBool down = Reg(KRegButtons) & 1;
			s.iFlags = down && !d.iDown ? KFlagDown : !down && d.iDown ? KFlagUp : KFlagMove;
			}
		d.Push(s);
		}
	Interrupt::Clear(EIntPointer);
	d.iDeliverDfc.Add();
	}

// ISR side. A full ring: a move replaces the newest queued move; a down/up replaces the newest move
// anywhere behind it (the finger's path loses a point, never a press or a release)
void DSybDigitiser::Push(const TSample& aS)
	{
	if (iCount == KRing)
		{
		TSample& last = iRing[(iHead + iCount - 1) % KRing];
		if ((aS.iFlags & KFlagMove) && (last.iFlags & KFlagMove))
			{
			last = aS;
			return;
			}
		for (TInt k = iCount - 1; k >= 0; --k)
			{
			TSample& t = iRing[(iHead + k) % KRing];
			if (t.iFlags & KFlagMove)
				{						// close the gap: the entries after it move down one
				for (TInt j = k; j < iCount - 1; ++j)
					iRing[(iHead + j) % KRing] = iRing[(iHead + j + 1) % KRing];
				--iCount;
				break;
				}
			}
		if (iCount == KRing)
			{
			++iDropped;
			return;
			}
		}
	iRing[(iHead + iCount) % KRing] = aS;
	++iCount;
	}

TBool DSybDigitiser::Pop(TSample& aS)
	{
	NKern::Lock();
	TInt irq = NKern::DisableAllInterrupts();
	TBool r = iCount > 0;
	if (r)
		{
		aS = iRing[iHead];
		iHead = (iHead + 1) % KRing;
		--iCount;
		}
	NKern::RestoreInterrupts(irq);
	NKern::Unlock();
	return r;
	}

void DSybDigitiser::DeliverDfcFn(TAny* aPtr)
	{
	((DSybDigitiser*)aPtr)->Deliver();
	}

void DSybDigitiser::RetryExpired(TAny* aPtr)
	{
	((DSybDigitiser*)aPtr)->iDeliverDfc.Add();
	}

// DFC: issue in order; a down or an up that does not fit waits (nothing behind it goes first)
void DSybDigitiser::Deliver()
	{
	if (iHaveParked)
		{
		if (Issue(iParked) != KErrNone)
			{
			iRetry.OneShot(NKern::TimerTicks(KRetryMs));
			return;
			}
		iHaveParked = EFalse;
		}
	TSample s;
	while (Pop(s))
		{
		if (s.iSlot != 0)
			continue;					// one pointer (multitouch: later)
		if (Issue(s) == KErrNone)
			continue;
		++iOverflows;
		if ((s.iFlags & KFlagMove) && iCount)
			continue;					// a newer move follows: this one is expendable
		iParked = s;
		iHaveParked = ETrue;
		iRetry.OneShot(NKern::TimerTicks(KRetryMs));
		return;
		}
	}

TInt DSybDigitiser::Issue(const TSample& aS)
	{
	TInt x = aS.iX, y = aS.iY, t;
	switch (iRotate)
		{
	case 90:  t = x; x = KRawMax - y; y = t; break;
	case 180: x = KRawMax - x; y = KRawMax - y; break;
	case 270: t = x; x = y; y = KRawMax - t; break;
		}
	TPoint raw, p;
	raw.iX = Min(Max(x, 0), KRawMax);
	raw.iY = Min(Max(y, 0), KRawMax);
	DigitiserToScreen(raw, p);
	TRawEvent e;
	char kind;
	if (aS.iFlags & KFlagDown)
		{
		if (iDown)						// a release lost upstream: close the old press first
			{
			e.Set(TRawEvent::EButton1Up, iLast.iX, iLast.iY);
			if (Kern::AddEvent(e) != KErrNone)
				return KErrOverflow;
			iDown = EFalse;
			}
		e.Set(TRawEvent::EButton1Down, p.iX, p.iY);
		kind = 'D';
		}
	else if (aS.iFlags & (KFlagUp | KFlagCancel))
		{
		if (!iDown)
			return KErrNone;
		e.Set(TRawEvent::EButton1Up, p.iX, p.iY);
		kind = 'U';
		}
	else
		{
		if (!iDown || (p.iX == iLast.iX && p.iY == iLast.iY))
			return KErrNone;			// hover, or no pixel change
		e.Set(TRawEvent::EPointerMove, p.iX, p.iY);
		kind = 'M';
		}
	TInt r = Kern::AddEvent(e);
	if (r != KErrNone)
		return r;
	iLast = p;
	if (kind == 'D')
		{
		iDown = ETrue;
		iPressEvents = 0;
		iOverflows = 0;
		iPressStart = NKern::TickCount();
		}
	++iPressEvents;
	if (iTrace)
		Kern::Printf("SYBSTUB: exyin ev %c %d,%d ts %u", kind, p.iX, p.iY, aS.iTs);
	if (kind == 'U')
		{
		iDown = EFalse;
		Kern::Printf("SYBSTUB: exyin press %d events %d ms, %d overflows, at %d,%d", iPressEvents,
			(NKern::TickCount() - iPressStart) * NKern::TickPeriod() / 1000, iOverflows, p.iX, p.iY);
		}
	return KErrNone;
	}

void DSybDigitiser::DigitiserOn() {}
void DSybDigitiser::DigitiserOff() {}
void DSybDigitiser::PowerUp() { PowerUpDone(); }
void DSybDigitiser::PowerDown(TPowerState) { PowerDownDone(); }

TInt DSybDigitiser::DigitiserToScreen(const TPoint& aD, TPoint& aS)
	{
	NKern::LockSystem();
	TCal c = iCal;
	NKern::UnlockSystem();
	aS.iX = (aD.iX * c.iR11 + aD.iY * c.iR21 + c.iTx) >> 16;
	aS.iY = (aD.iX * c.iR12 + aD.iY * c.iR22 + c.iTy) >> 16;
	return KErrNone;
	}

void DSybDigitiser::ScreenToDigitiser(TInt& aX, TInt& aY)
	{
	NKern::LockSystem();
	TCal c = iCal;
	NKern::UnlockSystem();
	Int64 X = Int64(aX) << 16, Y = Int64(aY) << 16;
	Int64 d = Int64(c.iR21) * c.iR12 - Int64(c.iR22) * c.iR11;
	if (!d)
		return;
	Int64 r = X * c.iR12 - Y * c.iR11 - Int64(c.iTx) * c.iR12 + Int64(c.iTy) * c.iR11;
	aY = (TInt)(r / d);
	r = X * c.iR22 - Y * c.iR21 - Int64(c.iTx) * c.iR22 + Int64(c.iTy) * c.iR21;
	aX = (TInt)(r / -d);
	}

void DSybDigitiser::DefaultCalibration(TDigitizerCalibration& aCal, TSize aSize)
	{
	aCal.iBl.iX = aCal.iTl.iX = aSize.iWidth / 10;
	aCal.iTr.iY = aCal.iTl.iY = aSize.iHeight / 10;
	aCal.iBr.iY = aCal.iBl.iY = aSize.iHeight - aSize.iHeight / 10;
	aCal.iTr.iX = aCal.iBr.iX = aSize.iWidth - aSize.iWidth / 10;
	}

TInt DSybDigitiser::CalibrationPoints(TDigitizerCalibration& aCal)
	{
	DefaultCalibration(aCal, ScreenSize());
	return KErrNone;
	}

// the user-side calibration application touched the CalibrationPoints() crosses at aCal (screen
// coordinates, through the current calibration): solve the new matrix (bsptemplate xyin.cpp method)
TInt DSybDigitiser::SetXYInputCalibration(const TDigitizerCalibration& aCal)
	{
	TDigitizerCalibration cal;
	CalibrationPoints(cal);
	TInt Xp1 = cal.iTl.iX, Yp1 = cal.iTl.iY, Xp2 = cal.iBl.iX, Yp2 = cal.iBl.iY, Xp3 = cal.iBr.iX, Yp3 = cal.iBr.iY;
	TInt X1 = aCal.iTl.iX, Y1 = aCal.iTl.iY, X2 = aCal.iBl.iX, Y2 = aCal.iBl.iY, X3 = aCal.iBr.iX, Y3 = aCal.iBr.iY;
	ScreenToDigitiser(X1, Y1);
	ScreenToDigitiser(X2, Y2);
	ScreenToDigitiser(X3, Y3);
	Int64 d1 = Int64(Y1 - Y2) * (X1 - X3) - Int64(Y1 - Y3) * (X1 - X2);
	Int64 d2 = Int64(Y2 - Y3) * (X1 - X3) - Int64(Y1 - Y3) * (X2 - X3);
	if (!d1 || !d2)
		return KErrArgument;
	TCal c;
	c.iR11 = (TInt)(((Int64(Y1 - Y2) * (Xp1 - Xp3) - Int64(Y1 - Y3) * (Xp1 - Xp2)) << 16) / d1);
	c.iR12 = (TInt)(((Int64(Y1 - Y2) * (Yp1 - Yp3) - Int64(Y1 - Y3) * (Yp1 - Yp2)) << 16) / d1);
	c.iR21 = (TInt)(((Int64(X1 - X3) * (Xp2 - Xp3) - Int64(X2 - X3) * (Xp1 - Xp3)) << 16) / d2);
	c.iR22 = (TInt)(((Int64(X1 - X3) * (Yp2 - Yp3) - Int64(X2 - X3) * (Yp1 - Yp3)) << 16) / d2);
	c.iTx = (Xp1 << 16) - X1 * c.iR11 - Y1 * c.iR21;
	c.iTy = (Yp1 << 16) - X1 * c.iR12 - Y1 * c.iR22;
	NKern::LockSystem();
	iCal = c;
	NKern::UnlockSystem();
	return KErrNone;
	}

TInt DSybDigitiser::SaveXYInputCalibration()
	{
	NKern::LockSystem();
	iCalSaved = iCal;
	NKern::UnlockSystem();
	return KErrNone;
	}

TInt DSybDigitiser::RestoreXYInputCalibration(TDigitizerCalibrationType aType)
	{
	TInt r = KErrNone;
	NKern::LockSystem();
	if (aType == EFactory)
		iCal = iCalFactory;
	else if (aType == ESaved)
		iCal = iCalSaved;
	else
		r = KErrNotSupported;
	NKern::UnlockSystem();
	return r;
	}

void DSybDigitiser::DigitiserInfo(TDigitiserInfoV01& aInfo)
	{
	TSize s = ScreenSize();
	aInfo.iDigitiserSize = s;
	aInfo.iOffsetToDisplay.iX = 0;
	aInfo.iOffsetToDisplay.iY = 0;
	}
