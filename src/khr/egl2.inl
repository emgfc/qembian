/*
 * The phone's EGL (libEGL.dll's egl* exports, the SDK's libegl13u ordinals): forwarding only. Every call goes
 * to the host's EGL (SYGL_EGL, src/syvg/egl2_fn.h, src/syvg/syvg_egl.cpp: configs, surfaces, contexts, the
 * bound API and current contexts, errors, strings for OpenVG and OpenGL ES). What stays here is the window
 * glue the host cannot do: a surface-manager surface for the app's RWindow (its id and size go to the host),
 * its re-creation when the window's size changes, and SubmitUpdate after a swap the host presented.
 * Included by eglos.cpp. SPDX-License-Identifier: MIT
 */
#include "../syvg/egl2_fn.h"
#include "../qemu-syborg/hw/arm/syborg_gl_proto.h"

/* one call, synchronous, under the process mutex (the command buffer is shared by the process's threads) */
static unsigned int EglCall(TInt aFn, const unsigned int *aArgs, TInt aCount, TInt aOutBytes = 4)
{
	unsigned int r = 0;
	if (!khrHostOn())
		return 0;
	OSAcquireMutex();
	unsigned int *p = khrHostEmit(SYGL_EGL, 2 + aCount);
	if (p) {
		p[0] = aFn;
		p[1] = (unsigned int)RThread().Id();
		for (TInt i = 0; i < aCount; i++)
			p[2 + i] = aArgs[i];
		if (khrHostFlush(aOutBytes) == KErrNone)
			r = khrHostOutArea()[0];
	}
	OSReleaseMutex();
	return r;
}

/* window surfaces: host surface id -> the guest's window glue */
struct Win2 { unsigned int iId; WindowContext *iCtx; };
static Win2 gWins[64];

static WindowContext *WinOf(unsigned int aId)
{
	for (TInt i = 0; i < 64; i++)
		if (gWins[i].iId == aId && aId)
			return gWins[i].iCtx;
	return NULL;
}

}	/* namespace OpenVGRI */

using namespace OpenVGRI;

#define A(...) const unsigned int a[] = { __VA_ARGS__ }
#define CALL(fn) EglCall(EGL2_FN_##fn, a, sizeof(a) / 4)
#define CALL0(fn) EglCall(EGL2_FN_##fn, NULL, 0)

/* the OpenGL ES stubs by name (gles.c): api 1 or 2 */
extern "C" void *khr_gl_proc(const char *aName, int aApi);

extern "C" {

EXPORT_C EGLint eglGetError(void) { return (EGLint)CALL0(GetError); }
EXPORT_C EGLDisplay eglGetDisplay(EGLNativeDisplayType d) { A((unsigned int)d); return (EGLDisplay)CALL(GetDisplay); }
EXPORT_C EGLBoolean eglInitialize(EGLDisplay d, EGLint *maj, EGLint *min) { A((unsigned int)d, (unsigned int)maj, (unsigned int)min); return CALL(Initialize); }
EXPORT_C EGLBoolean eglTerminate(EGLDisplay d) { A((unsigned int)d); return CALL(Terminate); }

/* strings stay valid for the process: the host's answer, one copy per name */
EXPORT_C const char *eglQueryString(EGLDisplay d, EGLint name)
{
	static char sStr[4][256];
	static EGLint sName[4];
	A((unsigned int)d, (unsigned int)name);
	const char *ret = NULL;
	if (!khrHostOn())
		return NULL;
	OSAcquireMutex();
	unsigned int *p = khrHostEmit(SYGL_EGL, 2 + 2);
	if (p) {
		p[0] = EGL2_FN_QueryString; p[1] = (unsigned int)RThread().Id(); p[2] = a[0]; p[3] = a[1];
		if (khrHostFlush(4 + 255) == KErrNone && khrHostOutArea()[0] != ~0u) {
			TInt slot = 0;
			while (slot < 3 && sName[slot] && sName[slot] != name)
				slot++;
			sName[slot] = name;
			TInt len = Min((TInt)khrHostOutArea()[0], 255);
			Mem::Copy(sStr[slot], khrHostOutArea() + 1, len);
			sStr[slot][len] = 0;
			ret = sStr[slot];
		}
	}
	OSReleaseMutex();
	return ret;
}

EXPORT_C EGLBoolean eglGetConfigs(EGLDisplay d, EGLConfig *c, EGLint size, EGLint *num) { A((unsigned int)d, (unsigned int)c, (unsigned int)size, (unsigned int)num); return CALL(GetConfigs); }
EXPORT_C EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *at, EGLConfig *c, EGLint size, EGLint *num) { A((unsigned int)d, (unsigned int)at, (unsigned int)c, (unsigned int)size, (unsigned int)num); return CALL(ChooseConfig); }
EXPORT_C EGLBoolean eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint attr, EGLint *v) { A((unsigned int)d, (unsigned int)c, (unsigned int)attr, (unsigned int)v); return CALL(GetConfigAttrib); }

EXPORT_C EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType win, const EGLint *at)
{
	/* the window glue first (guest OS work); a window the glue cannot use goes to the host as a null
	   surface id, and the host decides the error */
	WindowContext *w = (WindowContext *)OSCreateWindowContext(win);
	static const unsigned int KNoSid[4] = { 0, 0, 0, 0 };
	const unsigned int *sid = w ? (const unsigned int *)w->iSurface.iInternal : KNoSid;
	A((unsigned int)d, (unsigned int)c, sid[0], sid[1], sid[2], sid[3], w ? (unsigned int)w->iSize.iWidth : 0,
	  w ? (unsigned int)w->iSize.iHeight : 0, (unsigned int)at);
	unsigned int id = CALL(CreateWindowSurface);
	if (!id) {
		OSDestroyWindowContext(w);
		return EGL_NO_SURFACE;
	}
	for (TInt i = 0; i < 64; i++)
		if (!gWins[i].iId) {
			gWins[i].iId = id;
			gWins[i].iCtx = w;
			break;
		}
	return (EGLSurface)id;
}

EXPORT_C EGLSurface eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint *at) { A((unsigned int)d, (unsigned int)c, (unsigned int)at); return (EGLSurface)CALL(CreatePbufferSurface); }
EXPORT_C EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay d, EGLenum t, EGLClientBuffer b, EGLConfig c, const EGLint *at) { A((unsigned int)d, (unsigned int)t, (unsigned int)b, (unsigned int)c, (unsigned int)at); return (EGLSurface)CALL(CreatePbufferFromClientBuffer); }
EXPORT_C EGLSurface eglCreatePixmapSurface(EGLDisplay d, EGLConfig c, EGLNativePixmapType px, const EGLint *at) { A((unsigned int)d, (unsigned int)c, (unsigned int)px, (unsigned int)at); return (EGLSurface)CALL(CreatePixmapSurface); }

EXPORT_C EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface s)
{
	A((unsigned int)d, (unsigned int)s);
	EGLBoolean r = CALL(DestroySurface);
	if (r)
		for (TInt i = 0; i < 64; i++)
			if (gWins[i].iId == (unsigned int)s) {
				OSDestroyWindowContext(gWins[i].iCtx);
				gWins[i].iId = 0;
				gWins[i].iCtx = NULL;
			}
	return r;
}

EXPORT_C EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *v) { A((unsigned int)d, (unsigned int)s, (unsigned int)attr, (unsigned int)v); return CALL(QuerySurface); }
EXPORT_C EGLBoolean eglBindAPI(EGLenum api) { A((unsigned int)api); return CALL(BindAPI); }
EXPORT_C EGLenum eglQueryAPI(void) { return CALL0(QueryAPI); }
EXPORT_C EGLBoolean eglWaitClient(void) { return CALL0(WaitClient); }
EXPORT_C EGLBoolean eglReleaseThread(void) { return CALL0(ReleaseThread); }
EXPORT_C EGLBoolean eglSurfaceAttrib(EGLDisplay d, EGLSurface s, EGLint attr, EGLint v) { A((unsigned int)d, (unsigned int)s, (unsigned int)attr, (unsigned int)v); return CALL(SurfaceAttrib); }
EXPORT_C EGLBoolean eglBindTexImage(EGLDisplay d, EGLSurface s, EGLint b) { A((unsigned int)d, (unsigned int)s, (unsigned int)b); return CALL(BindTexImage); }
EXPORT_C EGLBoolean eglReleaseTexImage(EGLDisplay d, EGLSurface s, EGLint b) { A((unsigned int)d, (unsigned int)s, (unsigned int)b); return CALL(ReleaseTexImage); }
EXPORT_C EGLBoolean eglSwapInterval(EGLDisplay d, EGLint i) { A((unsigned int)d, (unsigned int)i); return CALL(SwapInterval); }
EXPORT_C EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *at) { A((unsigned int)d, (unsigned int)c, (unsigned int)share, (unsigned int)at); return (EGLContext)CALL(CreateContext); }
EXPORT_C EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext c) { A((unsigned int)d, (unsigned int)c); return CALL(DestroyContext); }
EXPORT_C EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c) { A((unsigned int)d, (unsigned int)dr, (unsigned int)rd, (unsigned int)c); return CALL(MakeCurrent); }
EXPORT_C EGLContext eglGetCurrentContext(void) { return (EGLContext)CALL0(GetCurrentContext); }
EXPORT_C EGLSurface eglGetCurrentSurface(EGLint rd) { A((unsigned int)rd); return (EGLSurface)CALL(GetCurrentSurface); }
EXPORT_C EGLDisplay eglGetCurrentDisplay(void) { return (EGLDisplay)CALL0(GetCurrentDisplay); }
EXPORT_C EGLBoolean eglQueryContext(EGLDisplay d, EGLContext c, EGLint attr, EGLint *v) { A((unsigned int)d, (unsigned int)c, (unsigned int)attr, (unsigned int)v); return CALL(QueryContext); }
EXPORT_C EGLBoolean eglWaitGL(void) { return CALL0(WaitGL); }
EXPORT_C EGLBoolean eglWaitNative(EGLint e) { A((unsigned int)e); return CALL(WaitNative); }

EXPORT_C EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s)
{
	WindowContext *w = WinOf((unsigned int)s);
	if (w && w->iSize != WindowSize(w) && MakeSurface(w) == KErrNone) {
		const unsigned int *sid = (const unsigned int *)w->iSurface.iInternal;
		A((unsigned int)s, sid[0], sid[1], sid[2], sid[3], (unsigned int)w->iSize.iWidth, (unsigned int)w->iSize.iHeight);
		CALL(WindowUpdate);
	}
	A((unsigned int)d, (unsigned int)s);
	unsigned int r = CALL(SwapBuffers);
	RSurfaceUpdateSession *sus = w && (r & 2) ? ThreadSus() : NULL;
	if (sus) {
		sus->SubmitUpdate(KAllScreens, w->iSurface, 0, NULL);
		if (w->iFrames++ < 3)
			RDebug::Printf("KHR: swap %d window %08x %dx%d (host EGL)", w->iFrames, w->iWindow, w->iSize.iWidth, w->iSize.iHeight);
	}
	return r & 1;
}

EXPORT_C EGLBoolean eglCopyBuffers(EGLDisplay d, EGLSurface s, EGLNativePixmapType px) { A((unsigned int)d, (unsigned int)s, (unsigned int)px); return CALL(CopyBuffers); }

/* EGL_NOK_resource_profiling(2) and EGL_KHR_reusable_sync: reached through eglGetProcAddress only (no
   libEGL ordinals). Forwarding; the host keeps the sync objects and the memory figures. */
static EGLBoolean eglQueryProfilingDataNOK_2(EGLDisplay d, EGLint bits, EGLint *data, EGLint size, EGLint *count)
{
	A((unsigned int)d, (unsigned int)bits, (unsigned int)data, (unsigned int)size, (unsigned int)count);
	return CALL(QueryProfilingDataNOK);
}

static void *eglCreateSyncKHR_2(EGLDisplay d, EGLenum type, const EGLint *attribs)
{
	A((unsigned int)d, (unsigned int)type, (unsigned int)attribs);
	return (void *)CALL(CreateSyncKHR);
}

static EGLBoolean eglDestroySyncKHR_2(EGLDisplay d, void *sync)
{
	A((unsigned int)d, (unsigned int)sync);
	return CALL(DestroySyncKHR);
}

/* the host never blocks the vCPU: while the sync is unsignaled it answers EGL_TIMEOUT_EXPIRED_KHR, and this
   thread sleeps and asks again until its timeout (nanoseconds; EGL_FOREVER_KHR: no end) ran out */
static EGLint eglClientWaitSyncKHR_2(EGLDisplay d, void *sync, EGLint flags, TUint64 timeout)
{
	const TInt KPollUs = 20000;
	TUint64 waited = 0;
	for (;;) {
		A((unsigned int)d, (unsigned int)sync, (unsigned int)flags);
		EGLint r = (EGLint)CALL(ClientWaitSyncKHR);
		if (r != 0x30F5 /* EGL_TIMEOUT_EXPIRED_KHR */ || timeout == 0 || waited >= timeout)
			return r;
		User::After(KPollUs);
		waited += (TUint64)KPollUs * 1000;
	}
}

static EGLBoolean eglSignalSyncKHR_2(EGLDisplay d, void *sync, EGLenum mode)
{
	A((unsigned int)d, (unsigned int)sync, (unsigned int)mode);
	return CALL(SignalSyncKHR);
}

static EGLBoolean eglGetSyncAttribKHR_2(EGLDisplay d, void *sync, EGLint attribute, EGLint *value)
{
	A((unsigned int)d, (unsigned int)sync, (unsigned int)attribute, (unsigned int)value);
	return CALL(GetSyncAttribKHR);
}

/* EGL_NOK_image_endpoint(2): the host keeps the endpoints, their frames and the EGLImages. What stays here:
   the surface opened and mapped in this process (its buffers, for producers that draw in guest memory) and the
   notification requests (a TRequestStatus of a thread of this process: a poller thread asks the host which
   are due and completes them). */
struct EpLocal { unsigned int iId; RChunk iChunk; TSurfaceId iSid; TBool iMapped; TRequestStatus *iStatus; TThreadId iThread; };
static EpLocal gEps[16];
static RSurfaceManager gEpSm;
static TBool gEpSmOpen;
static RMutex gEpLock;
static TBool gEpPoller;

static EpLocal *EpFind(unsigned int aId)
{
	for (TInt i = 0; i < 16; i++)
		if (gEps[i].iId && gEps[i].iId == aId)
			return &gEps[i];
	return NULL;
}

static void EpComplete(EpLocal *e, TInt aReason)
{
	if (!e->iStatus)
		return;
	RThread t;
	if (t.Open(e->iThread) == KErrNone) {
		t.RequestComplete(e->iStatus, aReason);
		t.Close();
	}
	e->iStatus = NULL;
}

static TInt EpPollerFn(TAny *)
{
	unsigned int dummy = 0;
	for (;;) {
		User::After(16000);
		TBool any = EFalse;
		gEpLock.Wait();
		for (TInt i = 0; i < 16 && !any; i++)
			any = gEps[i].iId && gEps[i].iStatus;
		gEpLock.Signal();
		if (!any)
			continue;
		if (EglCall(EGL2_FN_EndpointPollNOK, &dummy, 0, 4 * 17)) {
			unsigned int ids[16], n;
			OSAcquireMutex();
			const unsigned int *o = khrHostOutArea();
			n = o[0] > 16 ? 16 : o[0];
			for (unsigned int k = 0; k < n; k++)
				ids[k] = o[1 + k];
			OSReleaseMutex();
			gEpLock.Wait();
			for (unsigned int k = 0; k < n; k++) {
				EpLocal *e = EpFind(ids[k]);
				if (e)
					EpComplete(e, KErrNone);
			}
			gEpLock.Signal();
		}
	}
}

static void *eglCreateEndpointNOK_2(EGLDisplay d, EGLenum type, EGLenum stype, void *source, const EGLint *attribs)
{
	TInt w = 0, h = 0, stride = 0, nbuf = 0, off0 = 0, off1 = 0;
	TUint32 fmt = 0, base = 0;
	EpLocal *slot = NULL;
	if (!gEpLock.Handle() && gEpLock.CreateLocal() != KErrNone)
		return NULL;
	gEpLock.Wait();
	for (TInt i = 0; i < 16 && !slot; i++)
		if (!gEps[i].iId)
			slot = &gEps[i];
	if (slot && source && stype == 0x30D3) {
		if (!gEpSmOpen)
			gEpSmOpen = gEpSm.Open() == KErrNone;
		const TSurfaceId &sid = *(const TSurfaceId *)source;
		RSurfaceManager::TInfoBuf ib;
		if (gEpSmOpen && gEpSm.OpenSurface(sid) == KErrNone) {
			slot->iSid = sid;
			if (gEpSm.SurfaceInfo(sid, ib) == KErrNone) {
				w = ib().iSize.iWidth; h = ib().iSize.iHeight; stride = ib().iStride;
				fmt = ib().iPixelFormat; nbuf = ib().iBuffers;
			}
			if (gEpSm.MapSurface(sid, slot->iChunk) == KErrNone) {
				slot->iMapped = ETrue;
				base = (TUint32)slot->iChunk.Base();
				gEpSm.GetBufferOffset(sid, 0, off0);
				if (nbuf > 1)
					gEpSm.GetBufferOffset(sid, 1, off1);
			}
		}
	}
	A((unsigned int)d, (unsigned int)type, (unsigned int)stype, (unsigned int)source, (unsigned int)attribs,
	  (unsigned int)w, (unsigned int)h, (unsigned int)stride, (unsigned int)fmt, (unsigned int)nbuf,
	  (unsigned int)off0, (unsigned int)off1, (unsigned int)base);
	unsigned int id = slot ? CALL(CreateEndpointNOK) : 0;
	if (slot && id) {
		slot->iId = id;
		slot->iStatus = NULL;
	} else if (slot) {
		if (slot->iMapped)
			slot->iChunk.Close();
		if (!slot->iSid.IsNull())
			gEpSm.CloseSurface(slot->iSid);
		Mem::FillZ(slot, sizeof(*slot));
	}
	gEpLock.Signal();
	return (void *)id;
}

static EGLBoolean eglDestroyEndpointNOK_2(EGLDisplay d, void *ep)
{
	A((unsigned int)d, (unsigned int)ep);
	EGLBoolean r = CALL(DestroyEndpointNOK);
	if (r && gEpLock.Handle()) {
		gEpLock.Wait();
		EpLocal *e = EpFind((unsigned int)ep);
		if (e) {
			EpComplete(e, KErrCancel);
			if (e->iMapped)
				e->iChunk.Close();
			gEpSm.CloseSurface(e->iSid);
			Mem::FillZ(e, sizeof(*e));
		}
		gEpLock.Signal();
	}
	return r;
}

static EGLint eglGetEndpointAttribNOK_2(EGLDisplay d, void *ep, EGLint attrib)
{
	A((unsigned int)d, (unsigned int)ep, (unsigned int)attrib);
	return (EGLint)CALL(GetEndpointAttribNOK);
}

static EGLBoolean eglSetEndpointAttribNOK_2(EGLDisplay d, void *ep, EGLint attrib, EGLint value)
{
	A((unsigned int)d, (unsigned int)ep, (unsigned int)attrib, (unsigned int)value);
	return CALL(SetEndpointAttribNOK);
}

static EGLBoolean eglEndpointBeginStreamingNOK_2(EGLDisplay d, void *ep)
{
	A((unsigned int)d, (unsigned int)ep);
	return CALL(EndpointBeginStreamingNOK);
}

static EGLBoolean eglEndpointEndStreamingNOK_2(EGLDisplay d, void *ep)
{
	A((unsigned int)d, (unsigned int)ep);
	return CALL(EndpointEndStreamingNOK);
}

static void *eglAcquireImageNOK_2(EGLDisplay d, void *ep)
{
	A((unsigned int)d, (unsigned int)ep);
	return (void *)CALL(AcquireImageNOK);
}

static EGLBoolean eglReleaseImageNOK_2(EGLDisplay d, void *ep, void *image, EGLenum api)
{
	A((unsigned int)d, (unsigned int)ep, (unsigned int)image, (unsigned int)api);
	return CALL(ReleaseImageNOK);
}

static EGLint eglGetEndpointDirtyAreaNOK_2(EGLDisplay d, void *ep, EGLint *rects, EGLint start, EGLint max, EGLBoolean collapse)
{
	A((unsigned int)d, (unsigned int)ep, (unsigned int)rects, (unsigned int)start, (unsigned int)max, (unsigned int)collapse);
	return (EGLint)CALL(GetEndpointDirtyAreaNOK);
}

static EGLBoolean eglEndpointRequestNotificationNOK_2(EGLDisplay d, void *ep, void *status)
{
	if (!status || !gEpLock.Handle())
		return EGL_FALSE;
	gEpLock.Wait();
	EpLocal *e = EpFind((unsigned int)ep);
	if (e) {
		EpComplete(e, KErrCancel);              /* a new request replaces the old */
		e->iStatus = (TRequestStatus *)status;
		*e->iStatus = KRequestPending;
		e->iThread = RThread().Id();
	}
	if (e && !gEpPoller) {
		RThread t;
		if (t.Create(_L("KhrEndpoints"), EpPollerFn, 0x2000, NULL, NULL) == KErrNone) {
			t.Resume();
			t.Close();
			gEpPoller = ETrue;
		}
	}
	gEpLock.Signal();
	A((unsigned int)d, (unsigned int)ep);
	EGLBoolean r = e ? CALL(EndpointRequestNotificationNOK) : CALL(EndpointRequestNotificationNOK);
	if (!r && e) {
		gEpLock.Wait();
		e->iStatus = NULL;
		gEpLock.Signal();
	}
	return r;
}

static EGLBoolean eglEndpointCancelNotificationNOK_2(EGLDisplay d, void *ep)
{
	A((unsigned int)d, (unsigned int)ep);
	EGLBoolean r = CALL(EndpointCancelNotificationNOK);
	if (gEpLock.Handle()) {
		gEpLock.Wait();
		EpLocal *e = EpFind((unsigned int)ep);
		if (e)
			EpComplete(e, KErrCancel);
		gEpLock.Signal();
	}
	return r;
}

static EGLBoolean eglDestroyImageKHR_2(EGLDisplay d, void *image)
{
	A((unsigned int)d, (unsigned int)image);
	return CALL(DestroyImageKHR);
}

static unsigned int vgCreateEGLImageTargetKHR_2(void *image)
{
	A((unsigned int)image);
	return CALL(CreateEGLImageTargetKHR);
}

/* the host says whether the function exists: for which OpenGL ES (1 or 2), or an EGL extension function (3);
   the stub is ours */
typedef void (*KhrProc2)(...);
EXPORT_C KhrProc2 eglGetProcAddress(const char *name)
{
	A((unsigned int)name);
	unsigned int api = name ? CALL(GetProcAddress) : 0;
	if (api == 3) {
		static const struct { const char *iName; KhrProc2 iFn; } KEgl[] = {
			{ "eglQueryProfilingDataNOK", (KhrProc2)eglQueryProfilingDataNOK_2 },
			{ "eglCreateSyncKHR", (KhrProc2)eglCreateSyncKHR_2 },
			{ "eglDestroySyncKHR", (KhrProc2)eglDestroySyncKHR_2 },
			{ "eglClientWaitSyncKHR", (KhrProc2)eglClientWaitSyncKHR_2 },
			{ "eglSignalSyncKHR", (KhrProc2)eglSignalSyncKHR_2 },
			{ "eglGetSyncAttribKHR", (KhrProc2)eglGetSyncAttribKHR_2 },
			{ "eglCreateEndpointNOK", (KhrProc2)eglCreateEndpointNOK_2 },
			{ "eglDestroyEndpointNOK", (KhrProc2)eglDestroyEndpointNOK_2 },
			{ "eglGetEndpointAttribNOK", (KhrProc2)eglGetEndpointAttribNOK_2 },
			{ "eglSetEndpointAttribNOK", (KhrProc2)eglSetEndpointAttribNOK_2 },
			{ "eglEndpointBeginStreamingNOK", (KhrProc2)eglEndpointBeginStreamingNOK_2 },
			{ "eglEndpointEndStreamingNOK", (KhrProc2)eglEndpointEndStreamingNOK_2 },
			{ "eglAcquireImageNOK", (KhrProc2)eglAcquireImageNOK_2 },
			{ "eglReleaseImageNOK", (KhrProc2)eglReleaseImageNOK_2 },
			{ "eglGetEndpointDirtyAreaNOK", (KhrProc2)eglGetEndpointDirtyAreaNOK_2 },
			{ "eglEndpointRequestNotificationNOK", (KhrProc2)eglEndpointRequestNotificationNOK_2 },
			{ "eglEndpointCancelNotificationNOK", (KhrProc2)eglEndpointCancelNotificationNOK_2 },
			{ "eglDestroyImageKHR", (KhrProc2)eglDestroyImageKHR_2 },
			{ "vgCreateEGLImageTargetKHR", (KhrProc2)vgCreateEGLImageTargetKHR_2 },
		};
		for (TUint i = 0; i < sizeof(KEgl) / sizeof(KEgl[0]); i++)
			if (TPtrC8((const TUint8 *)KEgl[i].iName) == TPtrC8((const TUint8 *)name))
				return KEgl[i].iFn;
		return NULL;
	}
	return api ? (KhrProc2)khr_gl_proc(name, (int)api) : NULL;
}

}	/* extern "C" */

#undef A
#undef CALL
#undef CALL0

namespace OpenVGRI
{
