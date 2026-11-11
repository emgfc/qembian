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
