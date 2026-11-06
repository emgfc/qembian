// sygl platform layer: the only code that talks to Symbian (SDK headers, N95 exports reached
// through generated veneers, gen/imports.S). Everything else in the blob is portable C.
// No writable static data: per-thread state lives behind UserSvr::DllTls(KSyglTlsKey).
#include <e32std.h>
#include <e32svr.h>
#include <hal.h>
#include <w32std.h>
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

extern "C" {

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
