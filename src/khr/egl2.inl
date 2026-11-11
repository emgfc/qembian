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
	if (w && w->iSize != w->iWindow->SizeForEgl() && MakeSurface(w) == KErrNone) {
		const unsigned int *sid = (const unsigned int *)w->iSurface.iInternal;
		A((unsigned int)s, sid[0], sid[1], sid[2], sid[3], (unsigned int)w->iSize.iWidth, (unsigned int)w->iSize.iHeight);
		CALL(WindowUpdate);
	}
	A((unsigned int)d, (unsigned int)s);
	unsigned int r = CALL(SwapBuffers);
	if (w && (r & 2) && w->iSusOpen) {
		w->iSus.SubmitUpdate(KAllScreens, w->iSurface, 0, NULL);
		if (w->iFrames++ < 3)
			RDebug::Printf("KHR: swap %d window %08x %dx%d (host EGL)", w->iFrames, w->iWindow, w->iSize.iWidth, w->iSize.iHeight);
	}
	return r & 1;
}

EXPORT_C EGLBoolean eglCopyBuffers(EGLDisplay d, EGLSurface s, EGLNativePixmapType px) { A((unsigned int)d, (unsigned int)s, (unsigned int)px); return CALL(CopyBuffers); }

/* the host says whether the function exists and for which OpenGL ES (1 or 2); the stub is ours */
typedef void (*KhrProc2)(...);
EXPORT_C KhrProc2 eglGetProcAddress(const char *name)
{
	A((unsigned int)name);
	unsigned int api = name ? CALL(GetProcAddress) : 0;
	return api ? (KhrProc2)khr_gl_proc(name, (int)api) : NULL;
}

}	/* extern "C" */

#undef A
#undef CALL
#undef CALL0

namespace OpenVGRI
{
