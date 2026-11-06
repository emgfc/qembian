// N95 d_display.ldd stand-in (see nokiastub.h): channel "DisplayDriver" used by the N95's
// r_display.dll (CDisplayDriver): scdv.dll (WSERV's screen driver) and, through
// power_save_display_mode.dll, the screensaver. The real driver is documented in
// docs/nokia-d_display.md (section 4 = what this stand-in follows). Geometry and the frame buffer
// come from HAL (our lcd_n95 driver); the Syborg frame buffer is always live, so screen updates
// (ctl 8/9) need no copy.
//
//   ctl 3 {start,end,fullColour}  partial display (screensaver): only panel rows start..end lit.
//                                 Panel rows are rows of the native landscape panel, so in the
//                                 picture they are rows at rotation index 0/2 and columns at 1/3
//                                 (WSERV switches rotation with req 0x8003; the screensaver passes
//                                 its UI Y range at 0/2 and X range at 1/3 accordingly).
//                                 Programmed into the Syborg fb (PARTIAL_* registers)
//   ctl 4                         cancel partial display
//   ctl 5 -> TInt                 partial display active
//   ctl 10 (screen by value)      select screen: only 0
//   ctl 11 -> 26 words            capabilities (table below)
//   ctl 26 -> TInt                current rotation index
//   ctl 31 -> TInt                scan-line bytes
//   req 0x8001 (sub)              panel self-test: sub 0 only
//   req 0x8002 (mode index)       colour mode: 1 = EColor16MU only (our fb is 32 bpp)
//   req 0x8003 (rotation index)   0, 2, 3 (90 degrees is not supported by the panel). ScDv then
//                                 draws the picture in that orientation (240x320 at 3, 320x240 at
//                                 0/2) into the frame buffer, line pitch always 1280 bytes (ctl 31,
//                                 as on the device); the display controller (the Syborg fb,
//                                 ORIENTATION register) rotates it onto the portrait screen
//   req 0x23                      display event notification: stays pending (as on the device)
#include "nokiastub.h"
#include "syborg.h"

static const TUint32 KCaps[26] =
	{
	1,				// +0  display present
	0,				// +4  caller is the window server (ScDv ignores it)
	1,				// +8  DSB buffers supported (as on the device): CDirectScreenBitmap uses ctl 23/24/25 +
					//     req 0x2003/0x2004, paced by vsync (with 0 ScDv draws on heap memory and its
					//     EndUpdate completes at once: no frame pacing)
	0,				// +12
	0, 0,			// +16 partial-width updates, +20 update retry strategy (0 on the N95)
	0, 0,			// +24/+28 current height/width (real 240/320, swapped after rotation; ScDv
					//         does not need them, left 0 as before)
	240,			// +32 max partial-display lines (power_save_display_mode reports it)
	0, 1, 1,		// +36/+40/+44 rotation 90/180/270 (the panel cannot do 90)
	0,				// +48 EColor4K
	0,				// +52 EColor64K: real 1; off because our frame buffer is 32 bpp only
	0,				// +56 EColor16M
	1,				// +60 EColor16MU (N95 wsini.ini asks for it)
	0,				// +64 EColor16MA
	0, 0, 0, 0, 0,	// +68..+84
	1, 1,			// +88/+92 constants on the device (meaning unknown)
	0, 0			// +96/+100
	};

// partial-display state (one panel) and the current rotation index (req 0x8003)
static TInt PartialActive;
static TUint PartialStart, PartialEnd;
static TBool PartialFullColour;
static TUint Rotation = 0;

// Syborg fb registers (src/qemu-syborg/hw/arm/syborg_fb.c)
enum { KFbOrientation = 4, KFbPartialStart = 14, KFbPartialEnd = 15, KFbPartialMode = 16 };
const TUint KPanelRows = 240;		// native landscape panel: 320 columns x 240 rows

// The phone is held portrait: the portrait UI (rotation index 3) is upright on the screen. The
// controller turns the picture by (3 - index) quarter turns, so at the native index 0 (the
// screensaver) the landscape picture's top is at the screen's left edge [inferred: at index 3 the
// screensaver passes its portrait X range as panel rows unreversed, so panel row r is screen
// column r; if the real phone shows it the other way round, use (Rotation + 1) & 3 here].
static void SetFbOrientation()
	{
	WriteReg(KHwBaseClcd, KFbOrientation, (3 - Rotation) & 3);
	}

static void SetPartial(TBool aOn, TUint aStart, TUint aEnd, TBool aFullColour)
	{
	PartialActive = aOn;
	PartialStart = aStart;
	PartialEnd = aEnd;
	PartialFullColour = aFullColour;
	TBool rows = (Rotation & 1) == 0;	// panel rows are picture rows unless rotated by 90/270
	if (Rotation == 2)					// upside-down landscape: panel row r = picture row 239-r
		{
		TUint s = aStart;
		aStart = KPanelRows - 1 - aEnd;
		aEnd = KPanelRows - 1 - s;
		}
	WriteReg(KHwBaseClcd, KFbPartialStart, aStart);
	WriteReg(KHwBaseClcd, KFbPartialEnd, aEnd);
	WriteReg(KHwBaseClcd, KFbPartialMode, (aOn ? 1 : 0) | (aFullColour ? 2 : 0) | (rows ? 4 : 0));
	}

static TInt DisplayCtl(DThread* aClient, TInt aFn, TAny* a1, TAny*, const TUint32* aWords)
	{
	switch (aFn)
		{
	case 3:		// {startRow, endRow, fullColour}: start < end < 240
		if (aWords[0] >= aWords[1] || aWords[1] >= 240)
			return KErrArgument;
		SetPartial(ETrue, aWords[0], aWords[1], aWords[2] != 0);
		Kern::Printf("N95STUB: DisplayDriver partial display panel rows %d..%d %s, rotation %d -> picture %s",
					 aWords[0], aWords[1], aWords[2] ? "full colour" : "8 colours", Rotation,
					 (Rotation & 1) ? "columns" : "rows");
		return KErrNone;
	case 4:
		SetPartial(EFalse, 0, 0, EFalse);
		return KErrNone;
	case 5:
		return Kern::ThreadRawWrite(aClient, a1, &PartialActive, sizeof(TInt));
	case 10:	// screen index by value
		return (TInt)a1 == 0 ? KErrNone : KErrNotSupported;
	case 26:	// current rotation index (last req 0x8003). r_display ord59 swaps HAL's native
				// 320x240 to the current 240x320 with it (CDirectScreenBitmap checks its rect so)
		return Kern::ThreadRawWrite(aClient, a1, &Rotation, sizeof(TInt));
		}
	return KStubNotHandled;
	}

static TInt DisplayReq(DThread*, TInt aReq, const TUint32* aArgs)
	{
	switch (aReq)
		{
	case 0x8001: return aArgs[0] == 0 ? KErrNone : KErrNotSupported;
	case 0x8002: return aArgs[0] == 1 ? KErrNone : KErrNotSupported;
	case 0x8003:
		if (aArgs[0] == 1 || aArgs[0] > 3)
			return KErrNotSupported;
		Rotation = aArgs[0];
		SetFbOrientation();
		if (PartialActive)		// the panel band stays, its place in the picture moves
			SetPartial(ETrue, PartialStart, PartialEnd, PartialFullColour);
		return KErrNone;
		}
	return KStubNotHandled;
	}

static const SStubCtl KCtl[] =
	{
	{ 11, EWriteBlock, 0, KCaps, sizeof(KCaps) },
	// ctl 31 = scan-line bytes (r_display export 77): 320 x bytes/pixel at every rotation, as on
	// the device. ScDv draws with this stride: 240 used pixels per line at rotation 3, 320 at 0/2.
	// lcd_n95 programs the same pitch into the fb and reports it through HAL.
	{ 31, EWriteInt, 1280, 0, 0 },
	{ KStubRequest | 0x23, EPendRequest, 0, 0, 0 },
	};

// ---------------------------------------------------------------------------------------------
// Direct screen bitmap (DSB): CDirectScreenBitmap in ScDv, through r_display (docs/nokia-d_display.md
// section 2, caps word 2 = 1). Games draw into a driver buffer and post it with EndUpdate, which
// completes only after the frame went to the panel: that is the frame pacing games rely on.
//   ctl 23 a1 -> {x | w<<16, y | h<<16}, a2 = 1 double buffer / 2 incremental (single buffer)
//          create the buffer(s): physical RAM mapped user-readable/writable at one global address
//          (like the frame buffer itself, which ScDv also writes from user side)
//   ctl 24 a1 <- {TDisplayMode, address, w, h, line bytes, pixel shift, 0}
//          = BeginUpdate. r_display would treat word 1 as a chunk handle and add word 6 only when
//          caps word 3 != 0; the N95 reports 0 there, so the address is used as it is.
//          -18 no buffer, -39 a buffer is already being drawn (begun, not posted)
//   ctl 25 release (pending EndUpdate completes with KErrCancel)
//   req 0x2003        EndUpdate, whole buffer
//   req 0x2004 a1 ->  EndUpdate for the rect {x | w<<16, y | h<<16} (screen coordinates)
// A posted buffer is copied into the Syborg frame buffer at the next vsync (fb INT_CAUSE bit 0,
// fixed rate, QEMU property syborg-fb.refresh-rate, default 60 Hz) and the request completes then,
// so EndUpdate-paced loops run at the panel rate. WSERV's own updates (ctl 8/9) are not paced:
// the frame buffer is always live. Coordinates are portrait (our frame buffer memory layout).
// ---------------------------------------------------------------------------------------------
const TInt KFbW = 240, KFbH = 320, KFbPitch = 1280, KBpp = 4;	// pitch: ctl 31
enum { KFbBase = 1, KFbIntMask = 6, KFbIntCause = 7, KFbFrameCount = 17, KFbRefreshRate = 18 };
const TInt KErrDsbBusy = -39;			// value the real driver returns (FUN_8012aa20)
const TInt KDisplayModeColor16MU = 11;	// TDisplayMode EColor16MU

struct SDsb
	{
	DPlatChunkHw* iChunk;	// global mapping of the buffers (user accessible)
	TPhysAddr iPhys;
	TLinAddr iKern;			// address of buffer 0 (same for kernel and user)
	TAny* iOwner;			// channel that created it
	TInt iX, iY, iW, iH;	// screen rect (portrait)
	TInt iBufBytes, iBuffers, iBack;	// buffer size, 1 or 2 buffers, buffer handed out next
	TBool iBegun;			// ctl 24 done, EndUpdate not yet
	// posted frame waiting for the vsync
	TBool iPending;
	DThread* iReqThread;
	TRequestStatus* iReqStatus;
	TAny* iReqChannel;
	TInt iReqBuf, iRx, iRy, iRw, iRh;
	TUint iFrames;			// frames presented
	};
static SDsb Dsb;
static NFastMutex DsbLock;
static DPlatChunkHw* FbChunk;
static TUint8* FbKern;

static void VsyncDfcFn(TAny*);
static TDfc VsyncDfc(VsyncDfcFn, NULL, 1);

static void SetVsyncIrq(TBool aOn)
	{
	TInt irq = NKern::DisableAllInterrupts();
	TUint m = ReadReg(KHwBaseClcd, KFbIntMask);
	WriteReg(KHwBaseClcd, KFbIntMask, aOn ? (m | 1) : (m & ~1u));
	NKern::RestoreInterrupts(irq);
	}

static void VsyncIsr(TAny*)
	{
	WriteReg(KHwBaseClcd, KFbIntCause, 1);
	VsyncDfc.Add();
	}

// complete a request of another thread and drop our reference on it (DFC or client context)
static void CompleteAndRelease(DThread* aThread, TRequestStatus* aStatus, TInt aReason)
	{
	Kern::RequestComplete(aThread, aStatus, aReason);
	NKern::ThreadEnterCS();
	aThread->Close(NULL);
	NKern::ThreadLeaveCS();
	}

static void VsyncDfcFn(TAny*)
	{
	NKern::FMWait(&DsbLock);
	if (!Dsb.iPending)
		{
		NKern::FMSignal(&DsbLock);
		SetVsyncIrq(EFalse);
		return;
		}
	const TUint8* src = (const TUint8*)Dsb.iKern + Dsb.iReqBuf * Dsb.iBufBytes
						+ (Dsb.iRy - Dsb.iY) * Dsb.iW * KBpp + (Dsb.iRx - Dsb.iX) * KBpp;
	TInt rw = Dsb.iRw, rh = Dsb.iRh, rx = Dsb.iRx, ry = Dsb.iRy, pitch = Dsb.iW * KBpp;
	DThread* t = Dsb.iReqThread;
	TRequestStatus* s = Dsb.iReqStatus;
	Dsb.iPending = EFalse;
	Dsb.iReqThread = NULL;
	TUint frames = ++Dsb.iFrames;
	NKern::FMSignal(&DsbLock);
	// the frame "goes to the panel": copy into the live frame buffer
	TUint8* dst = FbKern + ry * KFbPitch + rx * KBpp;
	for (TInt y = 0; y < rh; ++y, src += pitch, dst += KFbPitch)
		memcpy(dst, src, rw * KBpp);
	if (frames == 1 || (frames % 600) == 0)
		Kern::Printf("N95DSB: frame %d presented at vsync %d (%d Hz)", frames,
					 ReadReg(KHwBaseClcd, KFbFrameCount), ReadReg(KHwBaseClcd, KFbRefreshRate));
	CompleteAndRelease(t, s, KErrNone);
	}

static TBool ClipRect(TInt& aX, TInt& aY, TInt& aW, TInt& aH, TInt aX0, TInt aY0, TInt aW0, TInt aH0)
	{
	TInt x1 = Max(aX, aX0), y1 = Max(aY, aY0);
	TInt x2 = Min(aX + aW, aX0 + aW0), y2 = Min(aY + aH, aY0 + aH0);
	if (x2 <= x1 || y2 <= y1)
		return EFalse;
	aX = x1; aY = y1; aW = x2 - x1; aH = y2 - y1;
	return ETrue;
	}

// cancel a pending EndUpdate (aChannel NULL = any); called in a client thread
static void DsbCancelPending(TAny* aChannel, TInt aReason)
	{
	NKern::FMWait(&DsbLock);
	DThread* t = NULL;
	TRequestStatus* s = NULL;
	if (Dsb.iPending && (!aChannel || Dsb.iReqChannel == aChannel))
		{
		t = Dsb.iReqThread;
		s = Dsb.iReqStatus;
		Dsb.iPending = EFalse;
		Dsb.iReqThread = NULL;
		}
	NKern::FMSignal(&DsbLock);
	if (t)
		CompleteAndRelease(t, s, aReason);
	}

static TInt DsbRelease(TAny* aChannel)
	{
	if (!Dsb.iChunk)
		return KErrNotReady;
	if (aChannel && Dsb.iOwner != aChannel)
		return KErrAccessDenied;
	DsbCancelPending(NULL, KErrCancel);
	NKern::ThreadEnterCS();
	Dsb.iChunk->Close(NULL);
	Epoc::FreePhysicalRam(Dsb.iPhys, Dsb.iBufBytes * Dsb.iBuffers);
	NKern::ThreadLeaveCS();
	Kern::Printf("N95DSB: released after %d frames", Dsb.iFrames);
	memclr(&Dsb, sizeof(Dsb));
	return KErrNone;
	}

static TInt DsbCreate(DThread* aClient, TAny* aChannel, TAny* a1, TInt aFlags)
	{
	TUint32 r[2];
	TInt e = Kern::ThreadRawRead(aClient, a1, r, sizeof(r));
	if (e != KErrNone)
		return e;
	TInt x = r[0] & 0xffff, w = r[0] >> 16, y = r[1] & 0xffff, h = r[1] >> 16;
	Kern::Printf("N95DSB: create rect %d,%d %dx%d flags %d by %O", x, y, w, h, aFlags, aClient);
	if ((aFlags != 1 && aFlags != 2) || w <= 0 || h <= 0 || x + w > KFbW || y + h > KFbH)
		return KErrArgument;
	if (Dsb.iChunk)
		return KErrInUse;
	NKern::ThreadEnterCS();
	if (!FbChunk)
		{
		e = DPlatChunkHw::New(FbChunk, ReadReg(KHwBaseClcd, KFbBase), Kern::RoundToPageSize(KFbPitch * KFbH),
							  EMapAttrSupRw | EMapAttrBufferedC);
		if (e != KErrNone)
			{
			NKern::ThreadLeaveCS();
			return e;
			}
		FbKern = (TUint8*)FbChunk->LinearAddress();
		}
	SDsb d;
	memclr(&d, sizeof(d));
	d.iX = x; d.iY = y; d.iW = w; d.iH = h;
	d.iBuffers = aFlags == 1 ? 2 : 1;
	d.iBufBytes = Kern::RoundToPageSize(w * h * KBpp);
	TInt total = d.iBufBytes * d.iBuffers;
	e = Epoc::AllocPhysicalRam(total, d.iPhys);
	if (e == KErrNone)
		{
		e = DPlatChunkHw::New(d.iChunk, d.iPhys, total, EMapAttrUserRw | EMapAttrBufferedC);
		if (e != KErrNone)
			Epoc::FreePhysicalRam(d.iPhys, total);
		}
	NKern::ThreadLeaveCS();
	if (e != KErrNone)
		return e;
	d.iKern = d.iChunk->LinearAddress();
	memclr((TAny*)d.iKern, total);
	d.iOwner = aChannel;
	Dsb = d;
	return KErrNone;
	}

static TInt DsbBegin(DThread* aClient, TAny* a1)
	{
	TUint32 info[7] = {0, 0, 0, 0, 0, 0, 0};
	TInt e = KErrNone;
	NKern::FMWait(&DsbLock);
	if (!Dsb.iChunk)
		e = KErrNotReady;
	else if (Dsb.iBegun)
		e = KErrDsbBusy;
	else
		{
		info[0] = KDisplayModeColor16MU;
		info[1] = Dsb.iKern + Dsb.iBack * Dsb.iBufBytes;
		info[2] = Dsb.iW;
		info[3] = Dsb.iH;
		info[4] = Dsb.iW * KBpp;
		info[5] = 5;			// log2(bits per pixel)
		info[6] = 0;
		Dsb.iBegun = ETrue;
		}
	NKern::FMSignal(&DsbLock);
	if (e == KErrDsbBusy)
		return e;			// the real driver leaves the block alone here
	TInt w = Kern::ThreadRawWrite(aClient, a1, info, sizeof(info));
	return e != KErrNone ? e : w;
	}

static void DsbEnd(DThread* aClient, TAny* aChannel, TRequestStatus* aStatus, TInt aReq, TUint32 aRectPtr)
	{
	TInt e = KErrNone;
	TInt x = Dsb.iX, y = Dsb.iY, w = Dsb.iW, h = Dsb.iH;
	if (aReq == 0x2004)
		{
		TUint32 r[2];
		e = Kern::ThreadRawRead(aClient, (TAny*)aRectPtr, r, sizeof(r));
		if (e == KErrNone)
			{
			x = r[0] & 0xffff; w = r[0] >> 16; y = r[1] & 0xffff; h = r[1] >> 16;
			if (!ClipRect(x, y, w, h, Dsb.iX, Dsb.iY, Dsb.iW, Dsb.iH))
				e = KErrArgument;
			}
		}
	NKern::FMWait(&DsbLock);
	if (e == KErrNone && !Dsb.iChunk)
		e = KErrNotReady;
	else if (e == KErrNone && Dsb.iPending)
		e = KErrInUse;		// one posted frame at a time
	if (e == KErrNone)
		{
		Dsb.iPending = ETrue;
		Dsb.iReqThread = aClient;
		Dsb.iReqStatus = aStatus;
		Dsb.iReqChannel = aChannel;
		Dsb.iReqBuf = Dsb.iBack;
		Dsb.iRx = x; Dsb.iRy = y; Dsb.iRw = w; Dsb.iRh = h;
		Dsb.iBegun = EFalse;
		if (Dsb.iBuffers == 2)
			Dsb.iBack ^= 1;
		aClient->Open();	// (fast mutex held: Open is a simple atomic increment)
		}
	NKern::FMSignal(&DsbLock);
	if (e != KErrNone)
		Kern::RequestComplete(aStatus, e);
	else
		SetVsyncIrq(ETrue);
	}

class DDisplayChannel : public DNokiaStubChannel
	{
public:
	DDisplayChannel(DNokiaStubFactory* aF) : DNokiaStubChannel(aF) {}
	virtual ~DDisplayChannel()
		{
		DsbCancelPending(this, KErrCancel);
		if (Dsb.iChunk && Dsb.iOwner == this)
			DsbRelease(this);
		}
	virtual TInt Request(TInt aFn, TAny* a1, TAny* a2)
		{
		DThread* client = &Kern::CurrentThread();
		switch (aFn)
			{
		case 23: return DsbCreate(client, this, a1, (TInt)a2);
		case 24: return DsbBegin(client, a1);			// every frame: not logged
		case 25: return DsbRelease(this);
		case ~0x2003:
		case ~0x2004:
			{
			TUint32 args[2] = {0, 0};
			if (a2)
				Kern::ThreadRawRead(client, a2, args, sizeof(args));
			DsbEnd(client, this, (TRequestStatus*)a1, ~aFn, args[0]);
			return KErrNone;
			}
		case KMaxTInt:
			DsbCancelPending(this, KErrCancel);
			break;
			}
		return DNokiaStubChannel::Request(aFn, a1, a2);
		}
	};

class DDisplayFactory : public DNokiaStubFactory
	{
public:
	DDisplayFactory(const TDesC& aName)
		: DNokiaStubFactory(aName, KCtl, sizeof(KCtl) / sizeof(KCtl[0]), ETrue, ETrue, DisplayReq, DisplayCtl) {}
	virtual TInt Install()
		{
		static TBool bound = EFalse;	// later loads of the LDD create (and drop) more factories
		if (!bound)
			{
			VsyncDfc.SetDfcQ(Kern::DfcQue0());
			TInt r = Interrupt::Bind(EIntFb, VsyncIsr, NULL);
			if (r == KErrNone)
				r = Interrupt::Enable(EIntFb);
			bound = r == KErrNone;
			Kern::Printf("N95DSB: vsync interrupt %d, refresh %d Hz", r, ReadReg(KHwBaseClcd, KFbRefreshRate));
			}
		return DNokiaStubFactory::Install();
		}
	virtual TInt Create(DLogicalChannelBase*& aChannel)
		{
		aChannel = new DDisplayChannel(this);
		return aChannel ? KErrNone : KErrNoMemory;
		}
	};

// other r_display requests (synchronous DoRequest + WaitForRequest) complete at once
_LIT(KStubName, "DisplayDriver");
DECLARE_STANDARD_LDD()
	{
	return new DDisplayFactory(KStubName);
	}
