// sygl platform layer: the only code that talks to Symbian (SDK headers, N95 exports reached
// through generated veneers, gen/imports.S). Everything else in the blob is portable C.
// No writable static data: per-thread state lives behind UserSvr::DllTls(KSyglTlsKey).
#include <e32std.h>
#include <e32svr.h>
#include <hal.h>
#include <w32std.h>
#include <coemain.h>
#include <fbs.h>
#include "sygl_int.h"

static const TInt KSyglTlsKey = 0x5359474c;		// 'SYGL': our TLS slot (any int key works on EKA2)
static const TInt KReqVsyncWait = 0x7f01;			// our d_display request (hack H-vsync-wait)
static const TInt KCtlRotation = 26;				// d_display: current WSERV rotation index

class RSyDisplay : public RBusLogicalChannel
	{
public:
	TInt Open()
		{
		_LIT(KDev, "DisplayDriver");
		return DoCreate(KDev, TVersion(1, 1, 1540), KNullUnit, NULL, NULL, EOwnerThread);
		}
	TInt Ctl(TInt aFn, TAny* a1) { return DoControl(aFn, a1); }
	void Req(TInt aReq, TRequestStatus& aStatus) { DoRequest(aReq, aStatus); }
	};

// Direct screen access for one window surface (what Nokia's libpvrTechviewWSEGL does through
// CDirectScreenAccess). We use the non-leaving RDirectScreenAccess (the blob has no exception
// unwinding) in a small active object. An abort from WSERV is acknowledged either by RunL (when the
// app's scheduler runs) or at the next swap, where Cancel() also consumes the completion signal,
// so a game loop that never yields to its scheduler cannot cause a stray-signal panic.
class CSyDsa : public CActive
	{
public:
	CSyDsa(RWsSession& aWs) : CActive(RDirectScreenAccess::EPriorityVeryHigh), iDsa(aWs) {}
	~CSyDsa()
		{
		Cancel();
		FreeRegion();
		if (iConstructed)
			iDsa.Close();
		}
	TInt Start(RWindowBase& aWin)
		{
		if (!iConstructed)
			{
			TInt r = iDsa.Construct();
			if (r != KErrNone)
				return r;
			iConstructed = ETrue;
			if (CActiveScheduler::Current())
				{
				CActiveScheduler::Add(this);
				iAdded = ETrue;
				}
			}
		FreeRegion();
		TInt r = iDsa.Request(iRegion, iStatus, aWin);
		if (r < KErrNone)
			return r;
		if (iAdded)
			SetActive();
		else
			iPolled = ETrue;		// no scheduler: we poll iStatus at each swap
		iRunning = ETrue;
		return KErrNone;
		}
	// at a swap: handle an abort that the scheduler has not delivered (yet). On the N95, WSERV's
	// abort reaches ws32's own active object (priority 2000, waiting on a message queue), which
	// then completes our iStatus; an app whose loop never yields to its scheduler (ZigGLTri drains
	// WSERV events itself) would never see it. So run only the ready active objects of DSA priority
	// (ws32's and ours), consuming one completion signal each, as the scheduler loop would.
	void Poll()
		{
		TInt err;
		for (TInt i = 0; i < 4 && CActiveScheduler::RunIfReady(err, RDirectScreenAccess::EPriorityVeryHigh); ++i)
			User::WaitForAnyRequest();
		if (!iRunning || iStatus == KRequestPending)
			return;
		if (IsActive())
			Cancel();				// DoCancel acknowledges; WaitForRequest consumes the signal
		else if (iPolled)
			{
			User::WaitForRequest(iStatus);
			Stop();
			}
		}
	TBool Running() const { return iRunning; }
	RRegion* Region() const { return iRegion; }
	TInt iAborts;
private:
	void Stop()
		{
		if (iRunning)
			{
			iDsa.Completed();		// tell WSERV we stopped drawing
			iRunning = EFalse;
			++iAborts;
			}
		iPolled = EFalse;
		FreeRegion();
		}
	void FreeRegion()
		{
		if (iRegion)
			{
			iRegion->Close();
			User::Free(iRegion);
			iRegion = NULL;
			}
		}
	void RunL() { Stop(); }
	void DoCancel()
		{
		if (iStatus == KRequestPending)
			{
			iDsa.Cancel();			// our own stop (surface destroyed)
			iRunning = EFalse;
			FreeRegion();
			}
		else
			Stop();					// an abort that arrived before the scheduler ran us
		}
	RDirectScreenAccess iDsa;
	RRegion* iRegion;
	TBool iConstructed, iAdded, iRunning, iPolled;
	};

extern "C" {

// visible region of the surface's window, in screen coordinates: fills up to aMax rects
// (x, y, w, h each) and returns their number, or -1 when DSA is not available (no CONE env)
int sy_dsa_region(SyThread* aT, SySurface* aS, int* aRects, int aMax)
	{
	(void)aT;
	CCoeEnv* env = CCoeEnv::Static();
	if (!env || !aS->window || !CActiveScheduler::Current())	// ws32's DSA needs a scheduler
		return -1;
	CSyDsa* d = (CSyDsa*)aS->dsa;
	if (!d)
		{
		d = new CSyDsa(env->WsSession());	// CBase::new: User::AllocZ, no leave
		if (!d)
			return -1;
		aS->dsa = d;
		}
	d->Poll();
	if (!d->Running())
		{
		TInt r = d->Start(*(RWindowBase*)aS->window);
		if (r != KErrNone)
			{
			if (aS->frames < 3)
				SYLOG("DSA request failed %d", r);
			return 0;
			}
		if (d->iAborts && d->iAborts <= 20)
			SYLOG("DSA restarted (%d aborts), %d rects", d->iAborts, d->Region() ? d->Region()->Count() : -1);
		}
	RRegion* reg = d->Region();
	if (!reg)
		return 0;
	TInt n = reg->Count();
	const TRect* r = reg->RectangleList();
	TInt k = 0;
	for (TInt i = 0; i < n && k < aMax; ++i, ++k)
		{
		aRects[k * 4] = r[i].iTl.iX; aRects[k * 4 + 1] = r[i].iTl.iY;
		aRects[k * 4 + 2] = r[i].Width(); aRects[k * 4 + 3] = r[i].Height();
		}
	return k;
	}

// CFbsBitmap pixels for pixmap surfaces and eglCopyBuffers. Our colour buffers are 0x00RRGGBB.
int sy_bitmap_info(void* aBmp, int* aW, int* aH, int* aMode)
	{
	CFbsBitmap* b = (CFbsBitmap*)aBmp;
	if (!b || !b->Handle())
		return -1;
	TSize s = b->SizeInPixels();
	*aW = s.iWidth; *aH = s.iHeight; *aMode = b->DisplayMode();
	return 0;
	}

static inline u32 from565(TUint16 v)
	{
	u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
	return (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
	}

// aToBitmap: copy aPix (w x h, top row first) into the bitmap, else read the bitmap into aPix.
// Copies the overlapping area; returns 0, or -1 for an unsupported display mode.
int sy_bitmap_copy(void* aBmp, u32* aPix, int aW, int aH, int aToBitmap)
	{
	CFbsBitmap* b = (CFbsBitmap*)aBmp;
	int bw, bh, mode;
	if (sy_bitmap_info(aBmp, &bw, &bh, &mode))
		return -1;
	int w = aW < bw ? aW : bw, h = aH < bh ? aH : bh;
	b->LockHeap(ETrue);
	TUint8* base = (TUint8*)b->DataAddress();
	TInt stride = b->DataStride();
	int r = 0;
	for (int y = 0; y < h && base; ++y)
		{
		TUint8* row = base + y * stride;
		u32* p = aPix + y * aW;
		switch (mode)
			{
		case EColor64K:
			for (int x = 0; x < w; ++x)
				{
				TUint16* q = (TUint16*)row + x;
				if (aToBitmap) { u32 c = p[x]; *q = (TUint16)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x1f)); }
				else p[x] = from565(*q);
				}
			break;
		case EColor4K:
			for (int x = 0; x < w; ++x)
				{
				TUint16* q = (TUint16*)row + x;
				if (aToBitmap) { u32 c = p[x]; *q = (TUint16)(((c >> 12) & 0xf00) | ((c >> 8) & 0xf0) | ((c >> 4) & 0xf)); }
				else { u32 v = *q; p[x] = (((v >> 8) & 15) * 17 << 16) | (((v >> 4) & 15) * 17 << 8) | ((v & 15) * 17); }
				}
			break;
		case EColor16M:		// B, G, R bytes
			for (int x = 0; x < w; ++x)
				{
				TUint8* q = row + x * 3;
				if (aToBitmap) { u32 c = p[x]; q[0] = (TUint8)c; q[1] = (TUint8)(c >> 8); q[2] = (TUint8)(c >> 16); }
				else p[x] = ((u32)q[2] << 16) | ((u32)q[1] << 8) | q[0];
				}
			break;
		case EColor16MU:
		case EColor16MA:
			for (int x = 0; x < w; ++x)
				{
				u32* q = (u32*)row + x;
				if (aToBitmap) *q = p[x] | 0xff000000u;
				else p[x] = *q & 0x00ffffffu;
				}
			break;
		case EGray256:
			for (int x = 0; x < w; ++x)
				{
				if (aToBitmap) { u32 c = p[x]; row[x] = (TUint8)((((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 150 + (c & 255) * 29) >> 8); }
				else p[x] = (u32)row[x] * 0x010101u;
				}
			break;
		default:
			r = -1;
			y = h;
			break;
			}
		}
	b->UnlockHeap(ETrue);
	return r;
	}

void sy_dsa_destroy(SySurface* aS)
	{
	CSyDsa* d = (CSyDsa*)aS->dsa;
	aS->dsa = NULL;
	delete d;
	}

// big buffers (surfaces, textures): our own chunk heap per thread, like Nokia's "PVRHeap", so the
// app's heap limit (often 1 MB, e.g. ZigGT2Car) does not apply
static const TInt KBigHeapMax = 32 << 20;
void* sy_big_alloc(int aBytes)
	{
	SyThread* t = sy_thread();
	if (!t)
		return NULL;
	if (!t->bigHeap)
		{
		t->bigHeap = UserHeap::ChunkHeap(NULL, 0x10000, KBigHeapMax, 0x10000);
		if (!t->bigHeap)
			{
			SYLOG("big heap: ChunkHeap(%d MB) failed", KBigHeapMax >> 20);
			return NULL;
			}
		}
	return ((RHeap*)t->bigHeap)->Alloc(aBytes);
	}

void sy_big_free(void* aPtr)
	{
	SyThread* t = sy_thread_peek();
	if (aPtr && t && t->bigHeap)
		((RHeap*)t->bigHeap)->Free(aPtr);
	}

void* sy_alloc(int aBytes) { return User::Alloc(aBytes); }
void* sy_allocz(int aBytes) { return User::AllocZ(aBytes); }
void sy_free(void* aPtr) { User::Free(aPtr); }
unsigned sy_ms(void) { return User::NTickCount(); }	// 1 ms nanokernel tick on our kernel

SyThread* sy_thread_peek(void)
	{
	return (SyThread*)UserSvr::DllTls(KSyglTlsKey);
	}

SyThread* sy_thread(void)
	{
	SyThread* t = sy_thread_peek();
	if (t)
		return t;
	t = (SyThread*)User::AllocZ(sizeof(SyThread));
	if (!t)
		return NULL;
	if (UserSvr::DllSetTls(KSyglTlsKey, t) != KErrNone)
		{
		User::Free(t);
		return NULL;
		}
	t->eglError = 0x3000;		// EGL_SUCCESS
	t->swapInterval = 1;
	return t;
	}

int sy_window_rect(void* aWindow, int* aX, int* aY, int* aW, int* aH)
	{
	RWindowBase* w = (RWindowBase*)aWindow;
	if (!w)
		return -1;
	TPoint p = w->AbsPosition();
	TSize s = w->Size();
	*aX = p.iX; *aY = p.iY; *aW = s.iWidth; *aH = s.iHeight;
	return 0;
	}

int sy_display_open(SyThread* aT)
	{
	if (aT->display)
		return 0;
	RSyDisplay d;
	TInt r = d.Open();
	if (r != KErrNone)
		{
		SYLOG("DisplayDriver open failed %d", r);
		return r;
		}
	aT->display = d.Handle();
	return 0;
	}

int sy_display_rotation(SyThread* aT)
	{
	TInt rot = 3;
	if (aT->display)
		{
		RSyDisplay d;
		d.SetHandle(aT->display);
		d.Ctl(KCtlRotation, &rot);
		}
	return rot;
	}

void sy_display_wait_vsync(SyThread* aT)
	{
	if (!aT->display)
		return;
	RSyDisplay d;
	d.SetHandle(aT->display);
	TRequestStatus s;
	d.Req(KReqVsyncWait, s);
	User::WaitForRequest(s);
	}

void sy_display_close(SyThread* aT)
	{
	if (!aT->display)
		return;
	RSyDisplay d;
	d.SetHandle(aT->display);
	d.Close();
	aT->display = 0;
	}

int sy_fb_info(SyThread* aT)
	{
	if (aT->fb)
		return 0;
	TInt addr = 0, mode = 0;
	if (HAL::Get(HALData::EDisplayMemoryAddress, addr) != KErrNone || !addr)
		return -1;
	HAL::Get(HALData::EDisplayMode, mode);
	TInt pitch = mode;		// in: display mode, out: bytes per line
	if (HAL::Get(HALData::EDisplayOffsetBetweenLines, pitch) != KErrNone || pitch <= 0)
		pitch = 1280;
	aT->fb = (u32*)addr;
	aT->fbPitch = pitch;
	SYLOG("frame buffer %08x, %d bytes per line (mode %d)", addr, pitch, mode);
	return 0;
	}

}	// extern "C"
