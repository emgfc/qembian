/*
 * The guest side of EGL (EGL and OpenVG run on the host: docs/devices/nokia-n8-rm596/egl-vg-host-v2.md):
 * the window glue that has to stay in the phone, and the OpenVG EGL forwarder (egl2.inl, included below).
 * Windows: EGLNativeWindowType is the client's RWindow (RDrawableWindow). As the phone's own EGL does under
 * ScreenPlay, a window surface is a surface-manager surface set as the window's background surface
 * (RWindowBase::SetBackgroundSurface); gce gets a layer for it and our backend (src/gcebe) shows it. The
 * host makes each frame that surface's scan-out image; RSurfaceUpdateSession::SubmitUpdate then tells the
 * composition backend; the window glue is the same for OpenVG and OpenGL ES windows.
 * SPDX-License-Identifier: MIT
 */
#include <e32std.h>
#include <w32std.h>
#include <fbs.h>
#include <graphics/surface.h>
#include <graphics/surfacemanager.h>
#include <graphics/surfaceupdateclient.h>
#include <pixelformats.h>
#include "egl.h"
#include "khrhost.h"

/* ---- Dll::Tls / SetTls: normally from the static edll.lib, here from our entry stub (startup.s):
   the DLL is identified by its entry point and the code segment ID the loader writes at entry+12 */
extern "C" void _E32Dll(void);
TAny *Dll::Tls()
{
	return UserSvr::DllTls((TInt)&_E32Dll, *(((TInt *)&_E32Dll) + 3));
}

TInt Dll::SetTls(TAny *aPtr)
{
	return UserSvr::DllSetTls((TInt)&_E32Dll, *(((TInt *)&_E32Dll) + 3), aPtr);
}

void Dll::FreeTls()
{
	UserSvr::DllFreeTls((TInt)&_E32Dll);
}

namespace OpenVGRI
{

/* one process-wide mutex for the RI's API functions (they are not thread safe) */
static RMutex gMutex;
static TInt gMutexState;         /* 0 none, 1 created */

void *OSGetCurrentThreadID(void)
{
	return (void *)(TUint)RThread().Id();
}

void OSAcquireMutex(void)
{
	if (!gMutexState && gMutex.CreateLocal() == KErrNone)
		gMutexState = 1;
	if (gMutexState)
		gMutex.Wait();
}

void OSReleaseMutex(void)
{
	if (gMutexState)
		gMutex.Signal();
}

void OSDeinitMutex(void)
{
}

EGLDisplay OSGetDisplay(EGLNativeDisplayType display_id)
{
	return (EGLDisplay)1;      /* EGL_DEFAULT_DISPLAY and anything else: our one display */
}

void OSDestroyWindowContext(void *context);

struct WindowContext {
	RWindowBase *iWindow;
	RSurfaceManager iSm;
	RSurfaceUpdateSession iSus;
	TSurfaceId iSurface;
	RChunk iChunk;
	TUint8 *iBase;
	TInt iStride;
	TSize iSize;
	TInt iSmOpen, iSusOpen, iFrames;
};

static void FreeSurface(WindowContext *c)
{
	if (c->iSurface.IsNull())
		return;
	c->iWindow->RemoveBackgroundSurface(ETrue);
	c->iChunk.Close();
	c->iSm.CloseSurface(c->iSurface);
	c->iSurface = TSurfaceId::CreateNullId();
	c->iBase = 0;
}

/* a surface of the window's current size, shown as the window's background. The size is the one for
   EGL (RWindowBase::SizeForEgl): a window whose app called FixNativeOrientation() keeps the display's
   native orientation (360x640) for its surface while the window itself turns with the screen mode
   (640x360); the app renders rotated itself (Angry Birds). As the phone's own EGL glue does
   (eglwindow.dll imports SizeForEgl). */
static TInt MakeSurface(WindowContext *c)
{
	FreeSurface(c);
	TSize size = c->iWindow->SizeForEgl();
	if (size.iWidth <= 0 || size.iHeight <= 0)
		return KErrArgument;
	RSurfaceManager::TSurfaceCreationAttributesBuf buf;
	RSurfaceManager::TSurfaceCreationAttributes &a = buf();
	a.iSize = size;
	a.iBuffers = 1;
	a.iPixelFormat = EUidPixelFormatARGB_8888_PRE;
	a.iStride = size.iWidth * 4;
	a.iOffsetToFirstBuffer = 0;
	a.iAlignment = 4;
	a.iContiguous = EFalse;
	a.iCacheAttrib = RSurfaceManager::ECached;
	a.iMappable = ETrue;
	TInt r = c->iSm.CreateSurface(buf, c->iSurface);
	if (r == KErrNone)
		r = c->iSm.MapSurface(c->iSurface, c->iChunk);
	if (r != KErrNone) {
		RDebug::Printf("KHR: window surface %dx%d: %d", size.iWidth, size.iHeight, r);
		c->iSurface = TSurfaceId::CreateNullId();
		return r;
	}
	TInt off = 0;
	c->iSm.GetBufferOffset(c->iSurface, 0, off);
	c->iBase = c->iChunk.Base() + off;
	c->iStride = a.iStride;
	c->iSize = size;
	Mem::FillZ(c->iBase, c->iStride * size.iHeight);
	r = c->iWindow->SetBackgroundSurface(c->iSurface);
	c->iWindow->Session()->Flush();
	RDebug::Printf("KHR: window %08x surface %08x %dx%d, background %d", c->iWindow, c->iSurface.iInternal[0],
	               size.iWidth, size.iHeight, r);
	return KErrNone;
}

void *OSCreateWindowContext(EGLNativeWindowType window)
{
	if (!window)
		return NULL;
	WindowContext *c = new WindowContext;
	if (!c)
		return NULL;
	Mem::FillZ(c, sizeof(*c));
	new (&c->iSm) RSurfaceManager;
	new (&c->iSus) RSurfaceUpdateSession;
	c->iSurface = TSurfaceId::CreateNullId();
	c->iWindow = (RWindowBase *)window;
	c->iSmOpen = c->iSm.Open() == KErrNone;
	c->iSusOpen = c->iSus.Connect() == KErrNone;
	if (!c->iSmOpen || MakeSurface(c) != KErrNone) {
		RDebug::Printf("KHR: window context: surface manager %d", c->iSmOpen);
		OSDestroyWindowContext(c);
		return NULL;
	}
	return c;
}

void OSDestroyWindowContext(void *context)
{
	WindowContext *c = (WindowContext *)context;
	if (!c)
		return;
	FreeSurface(c);
	if (c->iSusOpen)
		c->iSus.Close();
	if (c->iSmOpen)
		c->iSm.Close();
	delete c;
}

bool OSIsWindow(const void *context)
{
	return context != NULL;
}

void OSGetWindowSize(const void *context, int &width, int &height)
{
	const WindowContext *c = (const WindowContext *)context;
	TSize s = c ? c->iWindow->SizeForEgl() : TSize(0, 0);
	width = s.iWidth;
	height = s.iHeight;
}


#include "egl2.inl"	/* EGL on the host: the forwarder, with the window glue above */

}	/* namespace OpenVGRI */

/* ---- guest services under the forwarders (gles.c): the calling thread, memory a call hands to the app */
extern "C" unsigned khr_thread_id(void) { return (unsigned)RThread().Id(); }
extern "C" void* khr_alloc(int aBytes) { return User::Alloc(aBytes); }
extern "C" void khr_free(void* aPtr) { User::Free(aPtr); }
/* large blocks (a mapped buffer): a chunk of their own, not the app's heap; its handle before the data */
extern "C" void* khr_big_alloc(int aBytes)
	{
	RChunk c;
	if (aBytes <= 0 || c.CreateLocal(aBytes + 8, aBytes + 8) != KErrNone)
		return 0;
	*(TInt*)c.Base() = c.Handle();
	return c.Base() + 8;
	}
extern "C" void khr_big_free(void* aPtr)
	{
	if (!aPtr)
		return;
	RChunk c;
	c.SetHandle(*(TInt*)((TUint8*)aPtr - 8));
	c.Close();
	}
