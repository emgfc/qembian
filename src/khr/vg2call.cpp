/*
 * OpenVG on the host (v2, docs/devices/nokia-n8-rm596/egl-vg-host-v2.md): the transport of the generated
 * OpenVG/VGU stubs (vg2_gen.c, tools/gen-vg2.py). Every call is one SYGL_VG2 command, sent at once and
 * answered with one word; the host reads and writes the call's pointer arguments in this thread's memory.
 * Under the RI mutex: the command buffer is shared by the process's threads.
 * SPDX-License-Identifier: MIT
 */
#include <e32std.h>
#include <e32debug.h>
#include <VG/openvg.h>
#include "khrhost.h"
#include "../qemu-syborg/hw/arm/syborg_gl_proto.h"
#include "../syvg/vg2_fn.h"

namespace OpenVGRI
{
void OSAcquireMutex(void);
void OSReleaseMutex(void);
}

extern "C" unsigned int khrVgCall(int aFn, const unsigned int *aArgs, int aCount)
{
	OpenVGRI::OSAcquireMutex();
	unsigned int r = 0;
	unsigned int *p = OpenVGRI::khrHostOn() ? OpenVGRI::khrHostEmit(SYGL_VG2, 2 + aCount) : NULL;
	if (p) {
		p[0] = aFn;
		p[1] = (unsigned int)RThread().Id();
		for (int i = 0; i < aCount; i++)
			p[2 + i] = aArgs[i];
		if (OpenVGRI::khrHostFlush(4) == KErrNone)
			r = OpenVGRI::khrHostOutArea()[0];
	}
	OpenVGRI::OSReleaseMutex();
	return r;
}

/* A void call, queued (docs/gpu-boundary-roadmap.md phase 3): it goes with the next call that waits (a
   result, vgFlush/vgFinish, any EGL call) or when the batch is full. The host runs the batch in order, so
   errors, handles and drawing come out as if each call had been sent alone. Small inputs (vgSetfv's values,
   a matrix, glyph indices) are copied behind the call's words and the argument names that copy: the host
   reads it from the command buffer, which stays as it is until the batch has run. A NULL input stays NULL;
   an input whose size is negative or large goes the synchronous way, with the app's own pointer, so the
   host decides the error. */
static const int KInlineMax = 4096;
typedef struct { int idx; const void *p; int bytes; } khrVgIn;     /* as in the generated vg2_gen.c */

extern "C" void khrVgPost(int aFn, const unsigned int *aArgs, int aCount, const khrVgIn *aIn, int aNIn)
{
	int bytes = 0;
	for (int i = 0; i < aNIn; i++) {
		if (!aIn[i].p)
			continue;
		if (aIn[i].bytes < 0 || aIn[i].bytes > KInlineMax - bytes) {
			khrVgCall(aFn, aArgs, aCount);
			return;
		}
		bytes += (aIn[i].bytes + 3) & ~3;
	}
	OpenVGRI::OSAcquireMutex();
	unsigned int *p = OpenVGRI::khrHostOn() ? OpenVGRI::khrHostEmit(SYGL_VG2, 2 + aCount + bytes / 4) : NULL;
	if (p) {
		p[0] = aFn;
		p[1] = (unsigned int)RThread().Id();
		for (int i = 0; i < aCount; i++)
			p[2 + i] = aArgs[i];
		unsigned int *d = p + 2 + aCount;
		for (int i = 0; i < aNIn; i++) {
			if (!aIn[i].p)
				continue;
			Mem::Copy(d, aIn[i].p, aIn[i].bytes);
			p[2 + aIn[i].idx] = (unsigned int)d;
			d += (aIn[i].bytes + 3) / 4;
		}
	}
	OpenVGRI::OSReleaseMutex();
}

/* vgGetString: the four strings, kept here (the pointers must stay valid) */
extern "C" const VGubyte *khrVgString(VGStringID aName)
{
	static char sStr[4][256];
	TInt i = aName - VG_VENDOR;
	if (i < 0 || i > 3)
		return NULL;
	OpenVGRI::OSAcquireMutex();
	const VGubyte *ret = NULL;
	unsigned int *p = OpenVGRI::khrHostOn() ? OpenVGRI::khrHostEmit(SYGL_VG2, 3) : NULL;
	if (p) {
		p[0] = VG2_FN_vgGetString;
		p[1] = (unsigned int)RThread().Id();
		p[2] = aName;
		if (OpenVGRI::khrHostFlush(4 + 255) == KErrNone && OpenVGRI::khrHostOutArea()[0] != ~0u) {
			TInt len = Min((TInt)OpenVGRI::khrHostOutArea()[0], 255);
			Mem::Copy(sStr[i], OpenVGRI::khrHostOutArea() + 1, len);
			sStr[i][len] = 0;
			ret = (const VGubyte *)sStr[i];
		}
	}
	OpenVGRI::OSReleaseMutex();
	return ret;
}
