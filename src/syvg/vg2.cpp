/*
 * syvg: OpenVG 1.1 / VGU calls from the guest, executed by the RI front end in QEMU
 * (docs/devices/nokia-n8-rm596/egl-vg-host-v2.md). One SYGL_VG2 command per call, synchronous, in the
 * calling guest thread's kick: {fn, guest thread, argument words} (tools/gen-vg2.py). Handles are 32-bit
 * ids per guest process (the RI's are host pointers); pointers are guest virtual addresses, read and written
 * through the device during the kick (sizes from the call and, for path data, from the host's own path).
 * SPDX-License-Identifier: MIT
 */
#include "riDefs.h"
#include "riContext.h"
#include "riPath.h"
#include "riImage.h"
#include "VG/openvg.h"
#include "VG/vgu.h"
#include "syvg.h"
#include "vghost.h"
#include "vg2_fn.h"
#include "../qemu-syborg/hw/arm/syborg_gl_proto.h"
#include <unordered_map>
#include <algorithm>
#include <vector>
#include <cstring>
#include <cstdio>

namespace OpenVGRI
{
struct SyvgPath {
	static size_t coordBytes(const Path *p, const VGubyte *segs, int n)
	{
		if (!p || !segs || n <= 0)
			return 0;
		return (size_t)Path::countNumCoordinates(segs, n) * Path::getBytesPerCoordinate(p->getDatatype());
	}
	static size_t modifyBytes(const Path *p, int start, int n)
	{
		if (!p || start < 0 || n <= 0 || start + n > p->getNumSegments())
			return 0;
		return (size_t)Path::countNumCoordinates(&p->m_segments[start], n) * Path::getBytesPerCoordinate(p->getDatatype());
	}
};
}
using namespace OpenVGRI;

/* ---- the calling guest process and thread (set per call; the host EGL reads them, syvg.h) */
static uint32_t gOwner, gThread;

extern "C" uint32_t syvg_owner(void) { return gOwner; }
extern "C" uint32_t syvg_thread(void) { return gThread; }

extern "C" void syvg_set_caller(uint32_t owner, uint32_t thread)
{
	gOwner = owner;
	gThread = thread;
}

/* ---- handles: 32-bit ids per guest process <-> the RI's VGHandle (a host pointer) */
struct HandleMap {
	std::unordered_map<uint32_t, VGHandle> toHost;
	std::unordered_map<VGHandle, uint32_t> toGuest;
	uint32_t next = 1;
};
static std::unordered_map<uint32_t, HandleMap> gHandles;

static VGHandle hostHandle(uint32_t id)
{
	if (!id)
		return VG_INVALID_HANDLE;
	HandleMap &m = gHandles[gOwner];
	auto it = m.toHost.find(id);
	return it == m.toHost.end() ? (VGHandle)~(uintptr_t)0 : it->second;   /* unknown: a bad handle for the RI */
}

static uint32_t guestHandle(VGHandle h)
{
	if (h == VG_INVALID_HANDLE)
		return 0;
	HandleMap &m = gHandles[gOwner];
	auto it = m.toGuest.find(h);
	if (it != m.toGuest.end())
		return it->second;
	uint32_t id = m.next++;
	if (!m.next)
		m.next = 1;
	m.toHost[id] = h;
	m.toGuest[h] = id;
	return id;
}

static void dropHandle(uint32_t id)
{
	HandleMap &m = gHandles[gOwner];
	auto it = m.toHost.find(id);
	if (it == m.toHost.end())
		return;
	m.toGuest.erase(it->second);
	m.toHost.erase(it);
}

/* vgCreateEGLImageTargetKHR (EGL_NOK_image_endpoint, syvg_egl.cpp): a VGImage of w x h whose pixels are a copy
   of the host texture `srcKey` (an EGLImage) of the calling owner; 0 if no OpenVG context is current. Runs in the
   caller's EGL call: the RI's current context is the caller thread's. */
extern "C" uint32_t syvg_vg_image_from_texture(uint32_t owner, int w, int h, uint32_t srcKey)
{
	uint32_t so = gOwner;
	gOwner = owner;
	VGImage vi = vgCreateImage(VG_sARGB_8888_PRE, w, h, VG_IMAGE_QUALITY_NONANTIALIASED | VG_IMAGE_QUALITY_FASTER |
	                           VG_IMAGE_QUALITY_BETTER);
	uint32_t id = 0;
	if (vi != VG_INVALID_HANDLE) {
		OpenVGRI::Image *img = (OpenVGRI::Image *)vi;
		unsigned int *p = khrHostEmit(SYGL_TEX_COPY, 3);
		if (p) {
			p[0] = vgKey(img); p[1] = srcKey; p[2] = 0;
			khrHostFlush(0);
			img->m_hostGen = img->generation();     /* the host texture holds the pixels ... */
			img->root()->m_hostAhead = true;        /* ... and the CPU copy is stale (read back on demand) */
			id = guestHandle(vi);
		} else {
			vgDestroyImage(vi);
		}
	}
	gOwner = so;
	return id;
}

extern "C" uintptr_t syvg_vg_host_handle(uint32_t id)
{
	return (uintptr_t)hostHandle(id);
}

extern "C" void syvg_egl_owner_free(uint32_t owner);
extern "C" const char *syvg_vg_target(uint32_t owner, uint32_t thread, char *buf, size_t n);
namespace OpenVGRI { void syvgReleaseEGL(); }   /* src/syvg/patches/0013 */

/* the guest process is gone (its last DisplayDriver channel closed): its EGL instance (displays, surfaces,
   contexts, the contexts' VG objects: their destructors free their GPU objects too) and its ids go */
extern "C" void syvg_owner_free(uint32_t owner)
{
	uint32_t so = gOwner, st = gThread;
	syvg_set_caller(owner, 0);
	vgSetOwner(owner);
	OpenVGRI::syvgReleaseEGL();
	khrHostFlush(0);
	gHandles.erase(owner);
	syvg_egl_owner_free(owner);
	syvg_set_caller(so, st);
	vgSetOwner(so);
}

std::vector<uint32_t> syvgTlsOwners();          /* syvg_eglos.cpp */
void syvgTlsClear();
std::vector<uint32_t> syvgEglOwners();          /* syvg_egl.cpp */
void syvgEglReset();

/* a machine reset (syborg-gl's reset): the guest processes of the previous boot are gone, and the new
   boot's processes get the same ids. Every owner that has anything here is freed as at its exit (its EGL
   instance, VG objects and their GPU objects), then the tables, the batch and the object keys are emptied */
extern "C" void syvg_reset(void)
{
	std::vector<uint32_t> owners = syvgTlsOwners(), egl = syvgEglOwners();
	owners.insert(owners.end(), egl.begin(), egl.end());
	for (auto &h : gHandles)
		owners.push_back(h.first);
	std::sort(owners.begin(), owners.end());
	owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
	for (uint32_t o : owners)
		syvg_owner_free(o);
	gHandles.clear();
	syvgTlsClear();
	syvgEglReset();
	khrHostReset();
	syvg_set_caller(0, 0);
	if (!owners.empty())
		fprintf(stderr, "syvg: reset: %zu guest processes' EGL/VG state freed\n", owners.size());
}

/* ---- guest memory: an input copied in, or room for an output written back; NULL stays NULL. `back`: bytes
   of the span before the pointer (an image with a negative stride: its later rows lie below the pointer) */
class GuestBuf {
public:
	GuestBuf(uint32_t va, size_t bytes, bool in, size_t back = 0) : iVa(va), iBack(back), iOk(true), iData(NULL), iLen(0)
	{
		if (!va)
			return;
		if (bytes > (64u << 20) || back > bytes || back > va) {
			iOk = false;
			return;
		}
		/* review M4: no throwing allocation (built with -fno-exceptions, a failed resize aborts QEMU) */
		iLen = bytes ? bytes : 1;
		iData = (uint8_t *)malloc(iLen);
		if (!iData) {
			iOk = false;
			iLen = 0;
			return;
		}
		(void)in;               /* outputs are read too: the write-back must keep the bytes between image rows */
		if (bytes && syvg_guest_read(va - back, iData, bytes))
			iOk = false;
	}
	~GuestBuf() { free(iData); }
	GuestBuf(const GuestBuf &) = delete;
	GuestBuf &operator=(const GuestBuf &) = delete;
	bool ok() const { return iOk; }
	void *host() { return iVa && iData ? iData + iBack : NULL; }
	bool writeBack() { return !iVa || !iData || !syvg_guest_write(iVa - iBack, iData, iLen); }
private:
	uint32_t iVa;
	size_t iBack;
	bool iOk;
	uint8_t *iData;
	size_t iLen;
};

/* an image span: h rows of w pixels in `format`, `stride` bytes apart. VG allows a negative stride: row 0
   is at the pointer and the later rows below it (imageBack: the bytes before the pointer). The RI rejects
   a too short stride itself (VG_ILLEGAL_ARGUMENT) */
static size_t imageBytes(VGint stride, VGImageFormat format, VGint w, VGint h)
{
	if (w <= 0 || h <= 0)
		return 0;
	Color::Descriptor d = Color::formatToDescriptor(format);
	if (!Color::isValidDescriptor(d))
		return 0;               /* an unknown format: the RI reports VG_UNSUPPORTED_IMAGE_FORMAT_ERROR */
	int bpp = d.bitsPerPixel;
	size_t row = ((size_t)w * bpp + 7) / 8;
	size_t st = stride < 0 ? (size_t)-(long)stride : (size_t)stride;
	return st * (size_t)(h - 1) + row;
}

static size_t imageBack(VGint stride, VGint h)
{
	return stride < 0 && h > 0 ? (size_t)-(long)stride * (size_t)(h - 1) : 0;
}

static size_t pathCoordBytes(VGPath path, const VGubyte *segs, VGint n)
{
	VGContext *ctx = (VGContext *)syvg_current_context();
	if (!ctx || !ctx->isValidPath(path))
		return 0;
	return SyvgPath::coordBytes((const Path *)path, segs, n);
}

static size_t pathModifyBytes(VGPath path, VGint start, VGint n)
{
	VGContext *ctx = (VGContext *)syvg_current_context();
	if (!ctx || !ctx->isValidPath(path))
		return 0;
	return SyvgPath::modifyBytes((const Path *)path, start, n);
}

static inline VGfloat wordFloat(uint32_t w) { VGfloat f; memcpy(&f, &w, 4); return f; }
static inline uint32_t floatWord(VGfloat f) { uint32_t w; memcpy(&w, &f, 4); return w; }

/* ---- the call */
static uint32_t *gResult;

static int result(uint32_t r)
{
	*gResult = r;
	return 0;
}

static int badArgs(int fn)
{
	fprintf(stderr, "syvg: VG call %d: too few arguments\n", fn);
	return -1;
}

static int badPointer(int fn)
{
	static int logs;
	if (logs++ < 16)
		fprintf(stderr, "syvg: VG call %d: guest memory not readable/writable\n", fn);
	return -1;
}

static int dispatch(int fn, const uint32_t *a, size_t n)
{
	switch (fn) {
#include "vg2_dispatch.inc"
	default:
		return -1;
	}
}

/* fn, guest thread, args...: the result word at out[0]; vgGetString: out[0] = length, the bytes after it */
extern "C" int syvg_vg_call(uint32_t owner, const uint32_t *a, size_t n, uint32_t *out, size_t outWords)
{
	if (n < 2 || !out || outWords < 1)
		return -1;
	syvg_set_caller(owner, a[1]);
	vgSetOwner(owner);
	gResult = out;
	*out = 0;
	int fn = (int)a[0], r;
	if (fn == VG2_FN_vgGetString) {
		const VGubyte *str = n >= 3 ? vgGetString((VGStringID)a[2]) : NULL;
		size_t len = str ? strlen((const char *)str) : 0, room = (outWords - 1) * 4;
		if (len > room)
			len = room;
		out[0] = str ? (uint32_t)len : ~0u;
		if (len)
			memcpy(out + 1, str, len);
		r = 0;
	} else {
		static int trace = -1;          /* SYBORG_VG_TRACE=1: draws and clears with their target surface */
		if (trace < 0)
			trace = getenv("SYBORG_VG_TRACE") && *getenv("SYBORG_VG_TRACE") == '1';
		if (trace && (fn == VG2_FN_vgClear || fn == VG2_FN_vgDrawImage || fn == VG2_FN_vgDrawPath ||
		              (fn == VG2_FN_vgSetfv && n >= 3 && a[2] == VG_CLEAR_COLOR))) {
			char b[96];
			fprintf(stderr, "syvg: VG owner %u thread %u fn %d args %x %x %x %x %x %x %x -> %s\n", owner, a[1], fn,
			        n > 2 ? a[2] : 0, n > 3 ? a[3] : 0, n > 4 ? a[4] : 0, n > 5 ? a[5] : 0, n > 6 ? a[6] : 0,
			        n > 7 ? a[7] : 0, n > 8 ? a[8] : 0, syvg_vg_target(owner, a[1], b, sizeof(b)));
		}
		r = dispatch(fn, a + 2, n - 2);
	}
	/* the RI's drawing of this call goes with the batch's next command that is not OpenVG, or at its end
	   (syvg_vg_flush, phase 3): one host flush for a run of calls instead of one per call */
	return r;
}

extern "C" void syvg_vg_flush(void)
{
	khrHostFlush(0);
}
