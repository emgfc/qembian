// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// EXYIN.DLL stand-in: the touch screen of single-touch phones (5800 XpressMusic: Nokia's exyin.dll) as
// a Symbian digitiser (EHalGroupDigitiser, TRawEvent button-1 down / move / up) on the Syborg board's
// absolute pointer device (syborg-pointer, the same registers the baseport's EPOINTER mouse driver
// reads; a touch ROM leaves EPOINTER out: kernel.omit_baseport). The generic part is the kernel's own
// DDigitiser (kernel/eka/drivers/xyin/d_xyin.cpp); this file is the hardware part, shaped after the
// EPL bsptemplate xyin.cpp.
//
// The device reports absolute positions 0..0x7fff and the button state on every host mouse event.
// The interrupt only latches them; while the button is down a 10 ms timer feeds samples to DDigitiser
// (it issues pen-down only after a short delay line, so a click without movement still needs samples).
// Raw digitiser units are 12 bit (0..4095); the default calibration maps them onto the display size
// the LCD HAL reports, so no calibration is needed. Board config (docs/devices.md [[standins]] config):
//   exyin.rotate = 0|90|180|270   turn host coordinates before calibration (panel vs UI orientation)

#include <drivers/xyin.h>
#include <kernel/kern_priv.h>
#include <videodriver.h>
#include "syborg.h"
#include "boardconfig.h"

enum { KRegId = 0, KRegLatch = 1, KRegFifoCount = 2, KRegX = 3, KRegY = 4, KRegZ = 5, KRegButtons = 6, KRegIntEnable = 7 };
const TInt KRawBits = 12;
const TInt KRawMax = (1 << KRawBits) - 1;
const TInt KSampleMs = 10;

static inline TUint32 Reg(TInt aReg) { return ReadReg(KHwBaseKmiPointer, aReg); }

NONSHARABLE_CLASS(DSybDigitiser) : public DDigitiser
	{
public:
	DSybDigitiser();
	virtual TInt DoCreate();
	virtual void WaitForPenDown();
	virtual void WaitForPenUp();
	virtual void WaitForPenUpDebounce();
	virtual void DigitiserOn();
	virtual void DigitiserOff();
	virtual void FilterPenMove(const TPoint& aPoint);
	virtual void ResetPenMoveFilter();
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
	static void TimerExpired(TAny* aPtr);
	static void SampleDfcFn(TAny* aPtr);
	void Sample();
	TSize ScreenSize();
	void DefaultCalibration(TDigitizerCalibration& aCal, TSize aSize);
	struct TCal { TInt iR11, iR12, iR21, iR22, iTx, iTy; };
public:
	NTimer iTimer;
	TDfc iSampleTimerDfc;
	TInt iCurX, iCurY;			// latched raw position (KRawBits)
	TBool iDown;				// button 1 held
	TBool iSampling;			// timer running while the pen is down
	TInt iPressSamples;			// samples still owed to a press (a click shorter than the delay line)
	TInt iPressStart;			// NKern::TickCount() at the first sample of this press
	TInt iPressCount;			// samples delivered in this press
	TInt iRotate;
	TCal iCal, iCalSaved, iCalFactory;
	};

DDigitiser* DDigitiser::New()
	{
	return new DSybDigitiser;
	}

DSybDigitiser::DSybDigitiser()
	: iTimer(TimerExpired, this), iSampleTimerDfc(SampleDfcFn, this, 5)
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
	// own DFC thread: on the shared DfcQue0 other drivers' work delayed the 10 ms samples so much that
	// clicks shorter than ~150 ms reached the UI as nothing (5800, sydb phone)
	TDynamicDfcQue* q;
	TInt e = Kern::DynamicDfcQCreate(q, 27, _L8("ExyinDfc"));
	if (e != KErrNone)
		return e;
	iDfcQ = q;
	iSampleTimerDfc.SetDfcQ(iDfcQ);
	iRotate = BoardConfigInt("exyin.rotate", 0);
	TSize s = ScreenSize();
	TInt w = s.iWidth, h = s.iHeight;
	iCal.iR11 = (w << 16) / (KRawMax + 1); iCal.iR12 = 0;
	iCal.iR21 = 0; iCal.iR22 = (h << 16) / (KRawMax + 1);
	iCal.iTx = 0; iCal.iTy = 0;
	iCalSaved = iCalFactory = iCal;
	iCfg.iPenDownDiscard = 0;
	iCfg.iPenUpDiscard = 1;			// >= 1: DDigitiser::Create allocates the delay line with it
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
	iCfg.iAccThresholdX = 1;			// every pixel of movement is a move event (finger scrolling)
	iCfg.iAccThresholdY = 1;
	iCfg.iNumXYSamples = 1;
	iCfg.iDisregardMinMax = EFalse;
	Add();								// power handler
	TInt r = Interrupt::Bind(EIntPointer, Isr, this);
	if (r != KErrNone)
		return r;
	Interrupt::Enable(EIntPointer);
	WriteReg(KHwBaseKmiPointer, KRegIntEnable, 1);
	Kern::Printf("SYBSTUB: exyin digitiser %dx%d, raw %d bit, rotate %d, pointer id %08x", w, h, KRawBits, iRotate, Reg(KRegId));
	return KErrNone;
	}

// interrupt: drain the device FIFO, keep the last position and button state
void DSybDigitiser::Isr(TAny* aPtr)
	{
	DSybDigitiser& d = *(DSybDigitiser*)aPtr;
	while (Reg(KRegFifoCount))
		{
		WriteReg(KHwBaseKmiPointer, KRegLatch, 1);
		TInt x = (TInt)Reg(KRegX) >> (15 - KRawBits), y = (TInt)Reg(KRegY) >> (15 - KRawBits);
		TInt t;
		switch (d.iRotate)
			{
		case 90:  t = x; x = KRawMax - y; y = t; break;
		case 180: x = KRawMax - x; y = KRawMax - y; break;
		case 270: t = x; x = y; y = KRawMax - t; break;
			}
		d.iCurX = x < 0 ? 0 : x > KRawMax ? KRawMax : x;
		d.iCurY = y < 0 ? 0 : y > KRawMax ? KRawMax : y;
		TBool down = Reg(KRegButtons) & 1;
		if (down && !d.iDown)
			d.iPressSamples = 2;		// DDigitiser issues pen-down on the 2nd sample (iPenUpDiscard 1)
		d.iDown = down;
		}
	Interrupt::Clear(EIntPointer);
	if ((d.iDown || d.iPressSamples) && !d.iSampling)
		{
		d.iSampling = ETrue;
		d.iSampleTimerDfc.Add();
		}
	}

void DSybDigitiser::TimerExpired(TAny* aPtr)
	{
	((DSybDigitiser*)aPtr)->iSampleTimerDfc.Add();
	}

void DSybDigitiser::SampleDfcFn(TAny* aPtr)
	{
	((DSybDigitiser*)aPtr)->Sample();
	}

void DSybDigitiser::Sample()
	{
	if (iDown || iPressSamples)
		{
		if (iPressSamples)
			--iPressSamples;			// a quick click still reaches pen-down before its pen-up
		if (!iPressCount++)
			iPressStart = NKern::TickCount();
		iX[0] = iCurX;
		iY[0] = iCurY;
		// DDigitiser processes it and calls WaitForPenUp() for the next one. RawSampleValid/PenUp
		// queue a DFC (TDfc::Add): legal from an ISR, an IDFC or a thread with the kernel locked;
		// Sample() runs in our DFC thread (the udeb kernel's precondition check faulted on the N8)
		NKern::Lock();
		RawSampleValid();
		NKern::Unlock();
		}
	else
		{
		iSampling = EFalse;
		__KTRACE_OPT(KEXTENSION, Kern::Printf("SYBSTUB: exyin press %d samples, %d ms", iPressCount,
			(NKern::TickCount() - iPressStart) * NKern::TickPeriod() / 1000));
		Kern::Printf("SYBSTUB: exyin press %d samples %d ms at %d,%d", iPressCount,
			(NKern::TickCount() - iPressStart) * NKern::TickPeriod() / 1000, iCurX, iCurY);
		iPressCount = 0;
		NKern::Lock();
		PenUp();
		NKern::Unlock();
		}
	}

void DSybDigitiser::WaitForPenDown()
	{
	iSampling = EFalse;					// the interrupt starts sampling on the next press
	}

void DSybDigitiser::WaitForPenUp()
	{
	iTimer.OneShot(NKern::TimerTicks(KSampleMs));
	}

void DSybDigitiser::WaitForPenUpDebounce()
	{
	iTimer.OneShot(NKern::TimerTicks(KSampleMs));
	}

void DSybDigitiser::DigitiserOn() {}
void DSybDigitiser::DigitiserOff() {}
void DSybDigitiser::PowerUp() { PowerUpDone(); }
void DSybDigitiser::PowerDown(TPowerState) { PowerDownDone(); }

void DSybDigitiser::FilterPenMove(const TPoint& aPoint)
	{
	if (Abs(aPoint.iX - iLastPos.iX) >= iCfg.iAccThresholdX || Abs(aPoint.iY - iLastPos.iY) >= iCfg.iAccThresholdY)
		{
		iLastPos = aPoint;
		IssuePenMoveEvent(aPoint);
		}
	}

void DSybDigitiser::ResetPenMoveFilter() {}

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
