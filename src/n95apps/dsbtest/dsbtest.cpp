// dsbtest.exe: measures CDirectScreenBitmap frame pacing on the N95 user side.
// Plain EXE, no app framework. All output via RDebug::Printf, prefixed "DSBTEST:".
// Built against the S60 3rd FP1 (Symbian 9.2) SDK; see build.sh.

#include <e32base.h>
#include <e32debug.h>
#include <w32std.h>
#include <cdsb.h>
#include <graphicsaccelerator.h>
#include <hal.h>

#define P(fmt, ...) RDebug::Printf("DSBTEST: " fmt, ##__VA_ARGS__)

static const TInt KFrames = 300;
static const TInt KBlock = 60;
static const TInt KBurst = 60;

static TUint32 gFastFreq = 0;     // Hz, 0 if unknown
static TBool gFastUp = ETrue;
static TInt gNanoTickUs = 0;      // NTickCount period in us

static TUint32 FastDiff(TUint32 aStart, TUint32 aEnd)
	{
	return gFastUp ? aEnd - aStart : aStart - aEnd;
	}

// fast counter ticks -> microseconds (64-bit to avoid overflow)
static TUint32 FastToUs(TUint32 aTicks)
	{
	if (!gFastFreq)
		return 0;
	TUint64 us = (TUint64)aTicks * 1000000u / gFastFreq;
	return (TUint32)us;
	}

static TInt BytesPerPixel(TDisplayMode aMode)
	{
	switch (aMode)
		{
	case EColor4K:
	case EColor64K:
		return 2;
	case EColor16M:
		return 3;
	case EColor16MU:
	case EColor16MA:
		return 4;
	case EGray256:
	case EColor256:
		return 1;
	default:
		return 0;
		}
	}

static void Fill(const TAcceleratedBitmapInfo& aInfo, TInt aFrame)
	{
	TInt bpp = BytesPerPixel(aInfo.iDisplayMode);
	if (!aInfo.iAddress || bpp == 0 || aInfo.iLinePitch <= 0)
		return;
	// cycle red -> green -> blue ramps
	TUint8 r = 0, g = 0, b = 0;
	TUint8 v = (TUint8)((aFrame * 8) & 0xff);
	switch ((aFrame / 32) % 3)
		{
	case 0: r = v; break;
	case 1: g = v; break;
	default: b = v; break;
		}
	TInt w = aInfo.iSize.iWidth;
	TInt h = aInfo.iSize.iHeight;
	if (w * bpp > aInfo.iLinePitch)
		w = aInfo.iLinePitch / bpp;
	for (TInt y = 0; y < h; y++)
		{
		TUint8* row = aInfo.iAddress + y * aInfo.iLinePitch;
		if (bpp == 2)
			{
			TUint16 px = (aInfo.iDisplayMode == EColor4K)
				? (TUint16)(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4))
				: (TUint16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
			TUint16* p = (TUint16*)row;
			for (TInt x = 0; x < w; x++)
				p[x] = px;
			}
		else if (bpp == 4)
			{
			TUint32 px = 0xff000000u | (r << 16) | (g << 8) | b;
			TUint32* p = (TUint32*)row;
			for (TInt x = 0; x < w; x++)
				p[x] = px;
			}
		else if (bpp == 3)
			{
			for (TInt x = 0; x < w; x++)
				{
				row[3 * x] = b; row[3 * x + 1] = g; row[3 * x + 2] = r;
				}
			}
		else
			{
			for (TInt x = 0; x < w; x++)
				row[x] = v;
			}
		}
	}

static TUint32 gFrameFast[KFrames];   // per-frame duration, fast counter ticks
static TUint32 gFrameTick[KFrames];   // per-frame duration, nanokernel ticks

// 9.x TRAP = try/catch around User::MarkCleanupStack()/UnMarkCleanupStack(); these open a trap
// level so CleanupStack::PushL is legal (without it NewL panics E32USER-CBase 66). We have no C++
// EH runtime, so a real leave still panics, but pushes/pops work. Not declared in the SDK headers.
class TTrapHandler;
extern "C" TTrapHandler* _ZN4User16MarkCleanupStackEv();
extern "C" void _ZN4User18UnMarkCleanupStackEP12TTrapHandler(TTrapHandler*);

static void RunTestL();
static void RunTest()
	{
	TTrapHandler* h = _ZN4User16MarkCleanupStackEv();
	RunTestL();
	_ZN4User18UnMarkCleanupStackEP12TTrapHandler(h);
	}

static void RunTestL()
	{
	CDirectScreenBitmap* dsb = NULL;
	// NewL leaves only on OOM (would panic: no EH runtime); RunTest opened a trap level.
	dsb = CDirectScreenBitmap::NewL();
	P("NewL -> %08x", (TUint)dsb);
	if (!dsb)
		return;

	TInt r = dsb->Create(TRect(0, 0, 240, 320), CDirectScreenBitmap::EDoubleBuffer);
	P("Create(0,0,240,320, EDoubleBuffer) -> %d", r);
	if (r != KErrNone)
		{
		dsb->Close();
		delete dsb;
		return;
		}

	TAcceleratedBitmapInfo info;
	TRequestStatus st;
	TInt lastStatus = KErrNone;
	TInt errors = 0;
	TUint32 blockFast0 = User::FastCounter();
	TUint32 blockTick0 = User::NTickCount();
	TUint32 minUs = 0xffffffffu, maxUs = 0;

	for (TInt f = 0; f < KFrames; f++)
		{
		TUint32 f0 = User::FastCounter();
		TUint32 t0 = User::NTickCount();
		Mem::FillZ(&info, sizeof(info));
		r = dsb->BeginUpdate(info);
		if (f == 0 || r != KErrNone)
			P("BeginUpdate frame %d -> %d: mode %d addr %08x size %dx%d pitch %d shift %d phys %08x",
				f, r, (TInt)info.iDisplayMode, (TUint)info.iAddress, info.iSize.iWidth,
				info.iSize.iHeight, info.iLinePitch, info.iPixelShift, (TUint)info.iPhysicalAddress);
		if (r != KErrNone)
			{
			if (++errors > 5)
				{
				P("too many BeginUpdate errors, stopping");
				break;
				}
			User::After(100000);
			continue;
			}
		Fill(info, f);
		st = KRequestPending;
		dsb->EndUpdate(st);
		User::WaitForRequest(st);
		lastStatus = st.Int();
		if (lastStatus != KErrNone && errors++ < 5)
			P("EndUpdate frame %d status %d", f, lastStatus);
		TUint32 f1 = User::FastCounter();
		TUint32 t1 = User::NTickCount();
		gFrameFast[f] = FastDiff(f0, f1);
		gFrameTick[f] = t1 - t0;
		TUint32 us = FastToUs(gFrameFast[f]);
		if (us < minUs) minUs = us;
		if (us > maxUs) maxUs = us;

		if ((f + 1) % KBlock == 0)
			{
			TUint32 bf = User::FastCounter();
			TUint32 bt = User::NTickCount();
			TUint32 usFast = FastToUs(FastDiff(blockFast0, bf));
			TUint32 usTick = (bt - blockTick0) * (TUint32)gNanoTickUs;
			TUint32 us60 = gFastFreq ? usFast : usTick;
			TUint32 fps10 = us60 ? (TUint32)((TUint64)KBlock * 10000000u / us60) : 0;
			P("frames %d, last60 ms %d (ntick ms %d), fps %d.%d, status %d, frame us min %d max %d",
				f + 1, us60 / 1000, usTick / 1000, fps10 / 10, fps10 % 10, lastStatus,
				minUs, maxUs);
			blockFast0 = bf;
			blockTick0 = bt;
			minUs = 0xffffffffu;
			maxUs = 0;
			}
		}

	// burst: BeginUpdate/EndUpdate back to back, no fill, only the request wait
	TUint32 b0 = User::FastCounter();
	TUint32 bt0 = User::NTickCount();
	TInt done = 0;
	for (TInt i = 0; i < KBurst; i++)
		{
		r = dsb->BeginUpdate(info);
		if (r != KErrNone)
			{
			P("burst BeginUpdate %d -> %d", i, r);
			break;
			}
		st = KRequestPending;
		dsb->EndUpdate(st);
		User::WaitForRequest(st);
		lastStatus = st.Int();
		done++;
		}
	TUint32 b1 = User::FastCounter();
	TUint32 bt1 = User::NTickCount();
	TUint32 bus = gFastFreq ? FastToUs(FastDiff(b0, b1)) : (bt1 - bt0) * (TUint32)gNanoTickUs;
	TUint32 bfps10 = bus ? (TUint32)((TUint64)done * 10000000u / bus) : 0;
	P("burst %d pairs, ms %d (ntick ms %d), fps %d.%d, status %d",
		done, bus / 1000, (bt1 - bt0) * gNanoTickUs / 1000, bfps10 / 10, bfps10 % 10, lastStatus);

	// per-frame dump, compact: 10 frames per line, microseconds
	for (TInt f = 0; f < KFrames; f += 10)
		{
		P("us[%d..]: %d %d %d %d %d %d %d %d %d %d", f,
			FastToUs(gFrameFast[f]), FastToUs(gFrameFast[f + 1]), FastToUs(gFrameFast[f + 2]),
			FastToUs(gFrameFast[f + 3]), FastToUs(gFrameFast[f + 4]), FastToUs(gFrameFast[f + 5]),
			FastToUs(gFrameFast[f + 6]), FastToUs(gFrameFast[f + 7]), FastToUs(gFrameFast[f + 8]),
			FastToUs(gFrameFast[f + 9]));
		}

	dsb->Close();
	delete dsb;
	}

GLDEF_C TInt E32Main()
	{
	P("start");
	CTrapCleanup* cleanup = CTrapCleanup::New();   // in case scdv uses the cleanup stack

	TInt v = 0;
	if (HAL::Get(HALData::EFastCounterFrequency, v) == KErrNone)
		gFastFreq = (TUint32)v;
	TInt up = 1;
	if (HAL::Get(HALData::EFastCounterCountsUp, up) == KErrNone)
		gFastUp = up != 0;
	TInt np = 0;
	if (HAL::Get(HALData::ENanoTickPeriod, np) == KErrNone)
		gNanoTickUs = np;
	if (gNanoTickUs <= 0)
		gNanoTickUs = 1000;
	P("fast counter %d Hz, counts up %d, nanotick %d us", (TInt)gFastFreq, (TInt)gFastUp, gNanoTickUs);

	RWsSession ws;
	TInt r;
	TInt tries = 0;
	while ((r = ws.Connect()) != KErrNone)
		{
		if (tries++ % 10 == 0)
			P("RWsSession::Connect -> %d, retrying", r);
		User::After(1000000);
		}
	P("RWsSession connected after %d retries; waiting 15 s", tries);
	User::After(15000000);
	{
	TInt ram = 0, ramFree = 0, rom = 0;
	HAL::Get(HALData::EMemoryRAM, ram);
	HAL::Get(HALData::EMemoryRAMFree, ramFree);
	HAL::Get(HALData::EMemoryROM, rom);
	P("memory: RAM %d KB, free %d KB, ROM %d KB", ram >> 10, ramFree >> 10, rom >> 10);
	}

	RunTest();

	ws.Close();
	delete cleanup;
	P("done");
	return KErrNone;
	}

// Startup glue called from startup.s (EABI path of the 9.x uc_exe.cpp, minus static ctors
// and the global-destructor TLS hook: this program has no static objects with ctors/dtors).
struct SDsbThreadCreateInfo   // leading part of SStdEpocThreadCreateInfo (u32std.h)
	{
	TAny* iHandle;
	TInt iType;
	TThreadFunction iFunction;
	TAny* iPtr;
	};
struct SStdEpocThreadCreateInfo;
class UserHeap;
IMPORT_C TInt DsbSetupThreadHeap(TBool, SStdEpocThreadCreateInfo&) asm("_ZN8UserHeap15SetupThreadHeapEiR24SStdEpocThreadCreateInfo");

extern "C" void DsbRunThread(TBool aNotFirst, SDsbThreadCreateInfo& aInfo)
	{
	TInt r = DsbSetupThreadHeap(aNotFirst, (SStdEpocThreadCreateInfo&)aInfo);
	if (r == KErrNone)
		{
		if (aNotFirst)
			r = (*aInfo.iFunction)(aInfo.iPtr);
		else
			{
			User::InitProcess();
			r = E32Main();
			}
		}
	User::Exit(r);
	}

extern "C" void DsbInvariant() { User::Invariant(); }
extern "C" void DsbHandleException(TAny* aInfo) { User::HandleException(aInfo); }
