/*
 * syvg: the OS layer of the RI's MiniEGL running in QEMU (docs/devices/nokia-n8-rm596/egl-vg-host-v2.md).
 * What src/khr/eglos.cpp was in the guest, on the host:
 *   - the EGL instance (MiniEGL's Dll::Tls) per guest process; the thread id = the calling guest thread
 *     (current contexts are per thread);
 *   - a native window = a HostWindow: the GCE surface id and size the guest made for its RWindow (the
 *     guest keeps the window glue: surface creation, SizeForEgl, SubmitUpdate after a swap);
 *   - "blit to window" = the drawable's frame becomes that surface's scan-out image (SYGL_VG_PRESENT);
 *   - no native pixmaps (CFbsBitmap memory is the guest's); no mutex (the device runs under QEMU's lock).
 * SPDX-License-Identifier: MIT
 */
#include "riDefs.h"
#include "riImage.h"
#include "egl.h"
#include "syvg.h"
#include "vghost.h"
#include <unordered_map>
#include <vector>
#include <cstdint>

static std::unordered_map<uint32_t, void *> gTls;   /* guest process -> its MiniEGL instance */

/* the guest processes with a MiniEGL instance; a machine reset forgets them all (syvg_reset) */
std::vector<uint32_t> syvgTlsOwners()
{
	std::vector<uint32_t> v;
	for (auto &t : gTls)
		v.push_back(t.first);
	return v;
}

void syvgTlsClear()
{
	gTls.clear();
}

void *Dll::Tls()
{
	auto it = gTls.find(syvg_owner());
	return it == gTls.end() ? NULL : it->second;
}

int Dll::SetTls(void *aPtr)
{
	if (aPtr)
		gTls[syvg_owner()] = aPtr;
	else
		gTls.erase(syvg_owner());
	return 0;
}

namespace OpenVGRI
{
void *eglvgGetCurrentVGContext(void);
bool eglvgIsInUse(void *image);

void *OSGetCurrentThreadID(void)
{
	return (void *)(uintptr_t)((uint64_t)syvg_owner() << 32 | syvg_thread());
}

void OSAcquireMutex(void) {}
void OSReleaseMutex(void) {}
void OSDeinitMutex(void) {}

EGLDisplay OSGetDisplay(EGLNativeDisplayType display_id)
{
	return (EGLDisplay)1;
}

void *OSCreateWindowContext(EGLNativeWindowType window)
{
	return window;                  /* the HostWindow the dispatcher made (syvg_egl.cpp) */
}

void OSDestroyWindowContext(void *context)
{
	/* the HostWindow belongs to the dispatcher (freed with the surface) */
}

bool OSIsWindow(const void *context)
{
	return context != NULL;
}

void OSGetWindowSize(const void *context, int &width, int &height)
{
	const SyvgHostWindow *w = (const SyvgHostWindow *)context;
	width = w ? w->width : 0;
	height = w ? w->height : 0;
}

void OSBlitToWindow(void *context, const Drawable *drawable)
{
	SyvgHostWindow *w = (SyvgHostWindow *)context;
	if (!w || !drawable)
		return;
	w->presented = khrHostPresent(drawable->getColorBuffer(), w->sid);
}

EGLBoolean OSGetNativePixmapInfo(NativePixmapType pixmap, int *width, int *height, int *stride,
                                 VGImageFormat *format, int **data)
{
	return EGL_FALSE;
}
}

extern "C" void *syvg_current_context(void)
{
	return OpenVGRI::eglvgGetCurrentVGContext();
}

extern "C" int syvg_image_in_use(void *image)
{
	return OpenVGRI::eglvgIsInUse(image);
}
