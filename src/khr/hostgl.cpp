/*
 * The guest's command transport to the syborg-gl device: EGL/OpenVG calls (egl2.inl, vg2call.cpp; on the
 * host since v2, docs/devices/nokia-n8-rm596/egl-vg-host-v2.md), sygl's OpenGL ES 1.x drawing (hw_n8.cpp) and
 * OpenGL ES 2 (gl2.c).
 *
 * Transport: our d_display stand-in's DisplayDriver: control 0x7f10 (query: host GL ready, our
 * process id), then each batch runs in place: the device reads the command buffer and writes
 * read-backs in our own memory (a SyglKick naming them, SYGL_REG_KICK). The kick is a store to the
 * device's register page when the driver maps it for user mode (control 0x7f13), else control
 * 0x7f12; when the device cannot reach our pages (not present), control 0x7f11 copies the batch.
 * Commands are batched per process (every caller holds the process mutex, eglos.cpp OSAcquireMutex) and
 * sent when a result is needed or the buffer is full. Protocol:
 * src/qemu-syborg/hw/arm/syborg_gl_proto.h (SYGL_SURF/SURF_FREE/READ, SYGL_VG_*).
 * SPDX-License-Identifier: MIT
 */
#include <e32std.h>
#include <e32debug.h>
#include "khrhost.h"
#include "../qemu-syborg/hw/arm/syborg_gl_proto.h"

namespace OpenVGRI
{

static const TInt KCtlQuery = 0x7f10;
static const TInt KCtlSubmit = 0x7f11;
static const TInt KCtlKick = 0x7f12;
static const TInt KCtlUserRegs = 0x7f13;
static const TInt KBufWords = 256 * 1024;       /* 1 MB: the DisplayDriver's copy buffer */
static const TInt KOutBytes = 1 << 20;

class RKhrDisplay : public RBusLogicalChannel
{
public:
	TInt Open()
	{
		_LIT(KDev, "DisplayDriver");
		return DoCreate(KDev, TVersion(1, 1, 1540), KNullUnit, NULL, NULL, EOwnerProcess);
	}
	TInt Ctl(TInt aFn, TAny *a1) { return DoControl(aFn, a1); }
};

static RKhrDisplay gChan;
static TInt gState;             /* 0 not tried, 1 on, -1 no host GPU */
static unsigned int gOwner;
static unsigned int *gBuf;
static TInt gLen;               /* words */
static TUint8 *gOut;
static volatile TUint32 *gRegs;     /* the device's registers, user-mapped (or NULL) */
static SyglKick gKick;
static TInt gKicks, gCopies;
static TInt gLast = -1;          /* word index of the last command in gBuf (-1: none since the flush) */

bool khrHostOn()
{
	if (gState == 0) {
		gState = -1;
		TUint32 q[2] = { 0, 0 };
		TInt ro = gChan.Open(), rq = ro == KErrNone ? gChan.Ctl(KCtlQuery, q) : ro;
		if (ro != KErrNone || rq != KErrNone || !q[0])
			RDebug::Printf("KHR: DisplayDriver open %d, query %d: ready %d", ro, rq, q[0]);
		if (ro == KErrNone && rq == KErrNone && q[0]) {
			/* our own chunk, not the client's heap: an app's heap limit (often 1 MB) would refuse
			   the 2 MB, and the host GPU would stay off for OpenGL ES apps */
			static RChunk chunk;
			if (chunk.CreateLocal(KBufWords * 4 + KOutBytes, KBufWords * 4 + KOutBytes) == KErrNone) {
				gBuf = (unsigned int *)chunk.Base();
				gOut = chunk.Base() + KBufWords * 4;
			} else {
				RDebug::Printf("KHR: no chunk for the host GPU buffers");
			}
			if (gBuf && gOut) {
				gOwner = q[1];
				gState = 1;
				TUint32 lin = 0;
				if (gChan.Ctl(KCtlUserRegs, &lin) == KErrNone && lin)
					gRegs = (volatile TUint32 *)lin;
			}
		}
		RDebug::Printf("KHR: host GPU %s (owner %d, kick %s)", gState > 0 ? "on" : "off", gOwner,
		               gRegs ? "user" : "kernel");
	}
	return gState > 0;
}

/* send what is batched; the output (read-backs) goes to gOut */
/* KErrNone, or the batch (or its read) failed: the output area holds nothing new */
TInt khrHostFlush(TInt aOutBytes)
{
	if (gState <= 0)
		return KErrNotSupported;
	if (!gLen)
		return KErrNone;
	gKick.buf = (TUint32)gBuf; gKick.len = gLen * 4; gKick.out = (TUint32)gOut; gKick.outlen = aOutBytes;
	gKick.result = ~0u;
	TInt r;
	if (gRegs) {
		__asm__ volatile("" ::: "memory");          /* the device reads gBuf/gKick, writes gOut/result */
		gRegs[SYGL_REG_KICK] = (TUint32)&gKick;     /* runs synchronously in the device */
		__asm__ volatile("" ::: "memory");
		r = gKick.result == 0 ? KErrNone : gKick.result == ~0u ? KErrBadDescriptor : KErrGeneral;
	} else {
		r = gChan.Ctl(KCtlKick, &gKick);
	}
	if (r == KErrBadDescriptor || r == KErrNotSupported) {     /* pages the device cannot reach */
		TUint32 a[4] = { (TUint32)gBuf, (unsigned int)(gLen * 4), (unsigned int)gOut, (unsigned int)aOutBytes };
		r = gChan.Ctl(KCtlSubmit, a);
		if (gCopies++ < 4)
			RDebug::Printf("KHR: batch copied by the driver (%d words): %d", gLen, r);
	}
	if (r != KErrNone)
		RDebug::Printf("KHR: host submit %d (%d words)", r, gLen);
	gKicks++;
	gLen = 0;
	gLast = -1;
	return r;
}

/* room for n more words, starting a buffer with our owner id */
unsigned int *khrHostEmit(TInt aOp, TInt aArgs)
{
	TInt words = aArgs + 2;
	if (gLen + words > KBufWords)
		khrHostFlush(0);
	if (words + 3 > KBufWords)
		return NULL;
	if (!gLen) {
		gBuf[0] = SYGL_OWNER; gBuf[1] = 3; gBuf[2] = gOwner;
		gLen = 3;
	}
	unsigned int *p = gBuf + gLen;
	p[0] = aOp;
	p[1] = words;
	gLast = gLen;
	gLen += words;
	return p + 2;
}

/* ---- for sygl's GLES backend (hw_n8.cpp): it batches into the same buffer, under the same mutex */

/* where a flush with output leaves the host's results */
const unsigned int *khrHostOutArea()
{
	return (const unsigned int *)gOut;
}

/* the arguments of the last command in the buffer (NULL if it was flushed since) */
unsigned int *khrHostLast()
{
	return gLast >= 0 ? gBuf + gLast + 2 : NULL;
}

/* n more words at the end of the last command (an open SYGL_DRAW), NULL if they do not fit */
unsigned int *khrHostGrow(TInt aWords)
{
	if (gLast < 0 || gLen + aWords > KBufWords)
		return NULL;
	unsigned int *p = gBuf + gLen;
	gBuf[gLast + 1] += aWords;
	gLen += aWords;
	return p;
}

/* rows y0.. of a host surface (w pixels wide) as 0xAARRGGBB words, GL row order (bottom row first)
   into aDst; false if the host could not read it */
bool khrHostReadRows(unsigned int aSurf, TInt aW, TInt aY0, TInt aRows, unsigned int *aDst)
{
	TInt band = Max(1, KOutBytes / (aW * 4));
	for (TInt off = 0; off < aRows; off += band) {
		TInt rows = Min(band, aRows - off);
		unsigned int *p = khrHostEmit(SYGL_READ, 5);
		if (!p)
			return false;
		p[0] = aSurf; p[1] = 0; p[2] = aY0 + off; p[3] = rows; p[4] = SYGL_READ_ALPHA;
		if (khrHostFlush(aW * rows * 4) != KErrNone)
			return false;
		Mem::Copy(aDst + off * aW, gOut, aW * rows * 4);
	}
	return true;
}

/* an EGL window's frame from a host surface by id (GLES windows: hw_n8.cpp) */
bool khrHostPresentId(unsigned int aSurf, const unsigned int *sid)
{
	if (!khrHostOn())
		return false;
	unsigned int *p = khrHostEmit(SYGL_VG_PRESENT, 5);
	if (!p)
		return false;
	p[0] = aSurf; p[1] = sid[0]; p[2] = sid[1]; p[3] = sid[2]; p[4] = sid[3];
	return khrHostFlush(0) == KErrNone;
}


}	/* namespace OpenVGRI */

/* ---- for OpenGL ES (gles.c, C): the command buffer and the process lock (eglos.cpp) */
namespace OpenVGRI { void OSAcquireMutex(void); void OSReleaseMutex(void); }
extern "C" {
void khr_lock(void) { OpenVGRI::OSAcquireMutex(); }
void khr_unlock(void) { OpenVGRI::OSReleaseMutex(); }
int khr_host_on(void) { return OpenVGRI::khrHostOn() ? 1 : 0; }
unsigned* khr_emit(int aOp, int aArgs) { return OpenVGRI::khrHostEmit(aOp, aArgs); }
int khr_flush(int aOutBytes) { return OpenVGRI::khrHostFlush(aOutBytes); }
const unsigned* khr_out(void) { return OpenVGRI::khrHostOutArea(); }
}
