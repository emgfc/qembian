#include <stdio.h>
#include <stdlib.h>
/* syvg port of src/khr/hostgl.cpp (v1, guest): the same drawing commands, run in QEMU (vghost.h). */
/*
 * The OpenVG RI's drawables on the host GPU, in QEMU (docs/devices/nokia-n8-rm596/egl-vg-host-v2.md):
 * the RI's surfaces, images and fills become syborg-gl commands (src/qemu-syborg/hw/arm/syborg_gl_proto.h),
 * batched here and run by the device in process (syborg_gl_exec_host) when a result is needed, the buffer
 * is full or a dispatched guest call ends. Ported from the v1 guest code (src/khr/hostgl.cpp).
 * SPDX-License-Identifier: MIT
 */
#include "riImage.h"
#include <unordered_map>
#include "symcompat.h"
#include "vghost.h"
#include "../qemu-syborg/hw/arm/syborg_gl_proto.h"

namespace OpenVGRI
{

static const TInt KBufWords = 256 * 1024;
static const TInt KOutBytes = 4 << 20;

static unsigned int gBuf[KBufWords];
static TInt gLen;               /* words */
static TUint8 gOut[KOutBytes];
static unsigned int gOwner;     /* the guest process the calls being dispatched belong to (vghost.h) */
static TInt gLast = -1;          /* word index of the last command in gBuf (-1: none since the flush) */
static const TInt gState = 1;   /* the host GPU is always there */

void vgSetOwner(unsigned int aOwner)
{
	if (aOwner != gOwner)
		khrHostFlush(0);
	gOwner = aOwner;
}

bool khrHostOn()
{
	return true;
}

/* run what is batched in syborg-gl; results (read-backs) land in gOut */
TInt khrHostFlush(TInt aOutBytes)
{
	if (!gLen)
		return KErrNone;
	uint32_t e = syborg_gl_exec_host(gBuf, gLen, gOut, aOutBytes);
	TInt r = e == 0 ? KErrNone : KErrGeneral;
	if (r != KErrNone) {
		static int logs;
		if (logs++ < 16) {
			/* exec_cmds returns the failing command's word index + 1 */
			uint32_t at = e - 1;
			RDebug::Printf("host batch failed at word %u of %d: op %u, %u words, args %x %x %x %x (out %d bytes)", at, gLen,
			               at < (uint32_t)gLen ? gBuf[at] : 0, at + 1 < (uint32_t)gLen ? gBuf[at + 1] : 0,
			               at + 2 < (uint32_t)gLen ? gBuf[at + 2] : 0, at + 3 < (uint32_t)gLen ? gBuf[at + 3] : 0,
			               at + 4 < (uint32_t)gLen ? gBuf[at + 4] : 0, at + 5 < (uint32_t)gLen ? gBuf[at + 5] : 0, aOutBytes);
		}
	}
	gLen = 0;
	gLast = -1;
	return r;
}

/* room for n more words, starting a buffer with the owner id */
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

/* 32-bit ids for the device (its surfaces and textures by owner and id): even numbers per object, bit 0
   marks an image's render target (id | 1); the RI's objects are 64-bit pointers here */
static std::unordered_map<const void *, unsigned int> gKeys;
static unsigned int gNextKey = 0x10;

unsigned int vgKey(const void *aObj)
{
	auto it = gKeys.find(aObj);
	if (it != gKeys.end())
		return it->second;
	unsigned int k = gNextKey;
	gNextKey += 2;
	if (gNextKey < 0x10)
		gNextKey = 0x10;
	gKeys[aObj] = k;
	return k;
}

void vgKeyDrop(const void *aObj)
{
	gKeys.erase(aObj);
}

/* a machine reset (syvg_reset, after every object was released): nothing batched, no ids, as at power on */
void khrHostReset()
{
	gLen = 0;
	gLast = -1;
	gOwner = 0;
	gKeys.clear();
	gNextKey = 0x10;
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

unsigned int khrHostArgb(const Color &aColor)
{
	Color c = aColor;
	c.clamp();
	c.convert(Color::sRGBA_PRE);
	unsigned int a = (unsigned int)(c.a * 255.0f + 0.5f), r = (unsigned int)(c.r * 255.0f + 0.5f);
	unsigned int g = (unsigned int)(c.g * 255.0f + 0.5f), b = (unsigned int)(c.b * 255.0f + 0.5f);
	return a << 24 | r << 16 | g << 8 | b;
}

void khrHostSurfaceNew(Surface *s)
{
	if (!khrHostOn() || s->m_host)
		return;
	unsigned int *p = khrHostEmit(SYGL_SURF, 4);
	if (!p)
		return;
	p[0] = vgKey(s); p[1] = s->getWidth(); p[2] = s->getHeight(); p[3] = SYGL_SURF_STENCIL | SYGL_SURF_MSAA;
	s->m_host = (void *)(uintptr_t)vgKey(s);
}

/* an image bound as a render target (pbuffer from a VGImage): a host surface with id key|1 holding
   the image's current pixels; the image's draws sample it while it exists, and when it goes away
   the host keeps its pixels as the image's texture (the guest copy is then stale: step F v1) */
void khrHostImageTarget(Surface *s, Image *img)
{
	if (!khrHostOn() || s->m_host)
		return;
	unsigned int key = khrHostImage(img), id = vgKey(img) | 1;
	if (!key)
		return;
	int w = img->getWidth(), h = img->getHeight();
	unsigned int *p = khrHostEmit(SYGL_SURF, 4);
	if (!p)
		return;
	p[0] = id; p[1] = w; p[2] = h; p[3] = SYGL_SURF_STENCIL | SYGL_SURF_MSAA;
	p = khrHostEmit(SYGL_VG_FILL, 8 + 14 + 4 * 4);   /* copy in: the uploaded texture, blend SRC */
	if (!p)
		return;
	p[0] = id; p[1] = 1; p[2] = VG_BLEND_SRC; p[3] = 1; p[4] = 0xffffffff; p[5] = 14;
	p[6] = key; p[7] = w; p[8] = h; p[9] = 0; p[10] = SYGL_VG_TEXTURE_ONLY;
	float *f = (float *)(p + 11);
	f[0] = 1; f[1] = 0; f[2] = 0; f[3] = 0; f[4] = 1; f[5] = 0; f[6] = 0; f[7] = 0; f[8] = 1;
	p[20] = 0;                      /* no scissoring */
	p[21] = 4;
	float *e = (float *)(p + 22);
	float q[10] = { 0, 0, (float)w, 0, (float)w, (float)h, 0, (float)h, 0, 0 };
	for (int i = 0; i < 4; i++) {
		e[i * 4] = q[i * 2]; e[i * 4 + 1] = q[i * 2 + 1]; e[i * 4 + 2] = q[i * 2 + 2]; e[i * 4 + 3] = q[i * 2 + 3];
	}
	s->m_host = (void *)(uintptr_t)id;
	img->root()->m_hostAhead = true;    /* drawn on the host from now on */
	img->root()->m_hostLive = true;
}

void khrHostSurfaceFree(Surface *s)
{
	unsigned int id = (unsigned int)(uintptr_t)s->m_host, *p;
	if ((id & 1) && (p = khrHostEmit(SYGL_VG_SURF2TEX, 2)) != NULL) {
		p[0] = id; p[1] = id & ~1u;             /* the rendered pixels become the image's texture */
		Image *img = s->hostImage();
		if (img) {
			img->m_hostGen = img->generation();
			img->root()->m_hostLive = false;
		}
	}
	p = khrHostEmit(SYGL_SURF_FREE, 1);
	if (p)
		p[0] = id;
	s->m_host = NULL;
	vgKeyDrop(s);
}

/* Before drawing into an image's render target: the image's pending CPU change (read back first,
   khrHostSync, then written; otherwise uploaded lazily, when the image is next drawn) goes to the target
   now (syborg_gl.c tex_into_target puts an upload of an image with a live target into the target), so it
   lands under the new drawing. Uploaded only at its next draw, it overwrote what had been drawn into the
   target meanwhile (the 701's home-screen swipe: a dozen black frames, 2026-11-10). */
void khrHostTargetSync(Surface *s)
{
	Image *img = s ? s->hostImage() : NULL;
	if (img && img->root()->m_hostLive && img->generation() != img->m_hostGen)
		khrHostImage(img);
}

bool khrHostClear(Surface *s, const Color &c, int x, int y, int w, int h, const Array<Rectangle> *scissors)
{
	khrHostTargetSync(s);
	Rectangle r(0, 0, s->getWidth(), s->getHeight());
	r.intersect(Rectangle(x, y, w, h));
	if (!r.width || !r.height)
		return true;
	int n = scissors ? scissors->size() : 0;
	unsigned int *p = khrHostEmit(SYGL_VG_CLEAR, 3 + 4 * (n ? n : 1));
	if (!p)
		return false;
	p[0] = (unsigned int)(uintptr_t)s->m_host; p[1] = khrHostArgb(c);
	int k = 0;
	TInt32 *q = (TInt32 *)p + 3;
	for (int i = 0; i < (n ? n : 1); i++) {
		Rectangle t = r;
		if (n)
			t.intersect((*scissors)[i]);
		if (t.width <= 0 || t.height <= 0)
			continue;
		q[0] = t.x; q[1] = t.y; q[2] = t.width; q[3] = t.height;
		q += 4;
		k++;
	}
	if (!k) {                       /* everything scissored away: an empty rectangle */
		q[0] = q[1] = q[2] = q[3] = 0;
		k = 1;
	}
	/* unused rectangle slots (scissored away) are zero-sized: harmless */
	for (int i = k; i < (n ? n : 1); i++, q += 4)
		q[0] = q[1] = q[2] = q[3] = 0;
	p[2] = n ? n : 1;
	static int trace = -1;              /* SYBORG_VG_TRACE=1: where each clear lands */
	if (trace < 0)
		trace = getenv("SYBORG_VG_TRACE") && *getenv("SYBORG_VG_TRACE") == '1';
	if (trace) {
		fprintf(stderr, "syvg: clear surface %x argb %08x %d,%d %dx%d scissors %d:", (unsigned int)(uintptr_t)s->m_host,
		        khrHostArgb(c), x, y, w, h, n);
		const TInt32 *rq = (const TInt32 *)p + 3;
		for (int i = 0; i < (n ? n : 1) && i < 6; i++)
			fprintf(stderr, " %d,%d %dx%d", rq[i * 4], rq[i * 4 + 1], rq[i * 4 + 2], rq[i * 4 + 3]);
		fprintf(stderr, "\n");
	}
	return true;
}

/* Pixel copies into a host surface (RI patch 0014): vgSetPixels and vgWritePixels (an image -> the drawable) and
   vgCopyPixels (the drawable -> itself). The RI's Surface::blit wrote them into its own CPU copy, which a host
   surface never shows: the browser's scroll (it moves the page with vgCopyPixels and paints only the new strip)
   showed only the strip until the next full repaint. Here: a SRC-blend image fill of the destination rectangle,
   nearest sampling, translated by (s - d), within the scissor rectangles; for a copy the source surface first
   becomes a temporary texture (so overlapping copies read the old pixels). False: not handled, the RI's own path. */
static char gCopyTmp;           /* key holder of the temporary texture */

bool khrHostBlit(Surface *dst, const Image *src, const Surface *srcSurf, int sx, int sy, int dx, int dy, int w,
                 int h, const Array<Rectangle> &scissors)
{
	if (!khrHostOn() || !dst->m_host || (srcSurf && !srcSurf->m_host) || (!src && !srcSurf))
		return false;
	if (w <= 0 || h <= 0)
		return true;
	khrHostTargetSync(dst);
	unsigned int key, tw, th, mode = 0;
	if (src) {
		key = khrHostImage(const_cast<Image *>(src));
		if (!key)
			return false;
		tw = src->getWidth();
		th = src->getHeight();
	} else {
		key = vgKey(&gCopyTmp);
		unsigned int *t = khrHostEmit(SYGL_VG_SURF2TEX, 2);
		if (!t)
			return false;
		t[0] = (unsigned int)(uintptr_t)srcSurf->m_host;
		t[1] = key;
		tw = srcSurf->getWidth();
		th = srcSurf->getHeight();
		mode = SYGL_VG_TEXTURE_ONLY;    /* the copy, not a live render target of that key */
	}
	int ns = 0;
	for (int i = 0; i < scissors.size(); i++)
		if (scissors[i].width > 0 && scissors[i].height > 0)
			ns++;
	if (!ns)
		return true;                    /* scissoring with no rectangles: nothing visible */
	unsigned int *p = khrHostEmit(SYGL_VG_FILL, 8 + 14 + ns * 4 + 4 * 4);
	if (!p)
		return false;
	p[0] = (unsigned int)(uintptr_t)dst->m_host;
	p[1] = 1;                           /* non-zero */
	p[2] = VG_BLEND_SRC;
	p[3] = 1;                           /* image draw */
	p[4] = 0xffffffff;
	p[5] = 14;
	p[6] = key; p[7] = tw; p[8] = th; p[9] = 0 /* nearest */; p[10] = mode;
	float *f = (float *)(p + 11);       /* surface -> image: a translation */
	f[0] = 1; f[1] = 0; f[2] = (float)(sx - dx); f[3] = 0; f[4] = 1; f[5] = (float)(sy - dy); f[6] = 0; f[7] = 0; f[8] = 1;
	p[20] = ns;
	int *r = (int *)(p + 21);
	for (int i = 0; i < scissors.size(); i++)
		if (scissors[i].width > 0 && scissors[i].height > 0) {
			r[0] = scissors[i].x; r[1] = scissors[i].y; r[2] = scissors[i].width; r[3] = scissors[i].height;
			r += 4;
		}
	r[0] = 4;
	float *e = (float *)(r + 1);
	float q[10] = { (float)dx, (float)dy, (float)(dx + w), (float)dy, (float)(dx + w), (float)(dy + h), (float)dx,
	                (float)(dy + h), (float)dx, (float)dy };
	for (int i = 0; i < 4; i++) {
		e[i * 4] = q[i * 2]; e[i * 4 + 1] = q[i * 2 + 1]; e[i * 4 + 2] = q[i * 2 + 2]; e[i * 4 + 3] = q[i * 2 + 3];
	}
	if (!src) {
		unsigned int *t = khrHostEmit(SYGL_TEX_FREE, 1);
		if (t)
			t[0] = key;
	}
	return true;
}

/* surface -> image: rows of the host surface (bottom up, as the RI stores them) in bands */
bool khrHostRead(Image *dst, const Surface *src, int sx, int sy, int dx, int dy, int w, int h)
{
	int sw = src->getWidth(), sh = src->getHeight();
	if (sx < 0) { dx -= sx; w += sx; sx = 0; }
	if (sy < 0) { dy -= sy; h += sy; sy = 0; }
	if (dx < 0) { sx -= dx; w += dx; dx = 0; }
	if (dy < 0) { sy -= dy; h += dy; dy = 0; }
	w = Min(w, Min(sw - sx, dst->getWidth() - dx));
	h = Min(h, Min(sh - sy, dst->getHeight() - dy));
	if (w <= 0 || h <= 0)
		return true;
	int band = Max(1, Min(h, KOutBytes / (sw * 4)));
	for (int off = 0; off < h; off += band) {
		int rows = Min(band, h - off);
		unsigned int *p = khrHostEmit(SYGL_READ, 5);
		if (!p)
			return false;
		p[0] = (unsigned int)(uintptr_t)src->m_host; p[1] = 0; p[2] = sy + off; p[3] = rows; p[4] = SYGL_READ_ALPHA;
		if (khrHostFlush(sw * rows * 4) != KErrNone)
			return false;           /* not on the host (any more): keep the image as it is */
		if (dst->getDescriptor().format == VG_sARGB_8888_PRE && !dst->getParent()) {
			/* fast path (every swap: the window surface): same format, rows copied as they are;
			   the RI's generic blit converts each pixel in floating point (libm) */
			for (int i = 0; i < rows; i++)
				Mem::Copy(dst->getData() + (dy + off + i) * dst->getStride() + dx * 4,
				          gOut + (i * sw + sx) * 4, w * 4);
			dst->touched();
			continue;
		}
		Image band(Color::formatToDescriptor(VG_sARGB_8888_PRE), sw, rows, sw * 4, gOut);
		band.addReference();
		try {
			dst->blit(band, sx, 0, dx, dy + off, w, rows, false);
		} catch (std::bad_alloc) {
		}
		band.removeReference();
	}
	return true;
}

/* the common image formats straight into the upload's words (GL_RGBA bytes: 0xAABBGGRR,
   premultiplied); false: let the RI's generic blit convert (other formats, child images) */
static bool UploadFast(const Image *img, int y0, int rows, unsigned int *out)
{
	if (img->getParent())
		return false;                   /* children address their parent's memory with offsets */
	VGImageFormat f = img->getDescriptor().format;
	int w = img->getWidth(), stride = img->getStride();
	const RIuint8 *base = img->getData();
	switch (f) {
	case VG_sARGB_8888_PRE: case VG_sARGB_8888: case VG_sXRGB_8888: case VG_sRGB_565: case VG_A_8:
		break;
	default:
		return false;
	}
	for (int y = 0; y < rows; y++) {
		const RIuint8 *row = base + (y0 + y) * stride;
		unsigned int *o = out + y * w;
		if (f == VG_sRGB_565) {
			const unsigned short *s16 = (const unsigned short *)row;
			for (int x = 0; x < w; x++) {
				unsigned int c = s16[x], r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
				o[x] = 0xff000000u | ((b << 3 | b >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (r << 3 | r >> 2);
			}
		} else if (f == VG_A_8) {               /* alpha-only: (1,1,1,a), premultiplied */
			for (int x = 0; x < w; x++) {
				unsigned int a = row[x];
				o[x] = a << 24 | a << 16 | a << 8 | a;
			}
		} else {
			const unsigned int *s32 = (const unsigned int *)row;
			for (int x = 0; x < w; x++) {
				unsigned int c = s32[x], a = c >> 24, r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
				if (f == VG_sXRGB_8888)
					a = 255;
				else if (f == VG_sARGB_8888 && a != 255) {      /* premultiply */
					r = (r * a + 127) / 255; g = (g * a + 127) / 255; b = (b * a + 127) / 255;
				}
				o[x] = a << 24 | b << 16 | g << 8 | r;
			}
		}
	}
	return true;
}

/* an image as a host texture: uploaded (in bands) when its pixels changed since the last upload;
   GL_RGBA bytes = VG's sABGR_8888_PRE in little-endian words */
unsigned int khrHostImage(Image *img)
{
	if (!khrHostOn())
		return 0;
	unsigned int gen = img->generation();
	if (img->m_hostGen == gen)
		return vgKey(img);
	int w = img->getWidth(), h = img->getHeight();
	int band = Max(1, Min(h, (KBufWords - 16) / w));
	for (int y0 = 0; y0 < h; y0 += band) {
		int rows = Min(band, h - y0);
		unsigned int *p = khrHostEmit(SYGL_TEX, 6 + w * rows);
		if (!p)
			return 0;
		p[0] = vgKey(img); p[1] = gen; p[2] = w; p[3] = h; p[4] = y0; p[5] = rows;
		if (UploadFast(img, y0, rows, p + 6))
			continue;
		Image out(Color::formatToDescriptor(VG_sABGR_8888_PRE), w, rows, w * 4, (RIuint8 *)(p + 6));
		out.addReference();
		try {
			out.blit(*img, 0, y0, 0, 0, w, rows, false);
		} catch (std::bad_alloc) {
		}
		out.removeReference();
	}
	img->m_hostGen = gen;
	return vgKey(img);
}

/* pixels rendered on the host (an image that was a render target) back into the image's memory,
   before the CPU reads or changes them (RI patch 0005) */
void khrHostSync(const Image *ci)
{
	if (!ci)
		return;
	Image *img = const_cast<Image *>(ci)->root();
	if (!img->m_hostAhead || gState <= 0)
		return;
	img->m_hostAhead = false;       /* first: the conversion below runs through Image::blit */
	int w = img->getWidth(), h = img->getHeight();
	int band = Max(1, Min(h, KOutBytes / (w * 4)));
	bool fast = img->getDescriptor().format == VG_sARGB_8888_PRE;
	for (int y0 = 0; y0 < h; y0 += band) {
		int rows = Min(band, h - y0);
		unsigned int *p = khrHostEmit(SYGL_VG_TEXREAD, 4);
		if (!p)
			break;
		p[0] = vgKey(img); p[1] = 0; p[2] = y0; p[3] = rows;
		if (khrHostFlush(w * rows * 4) != KErrNone) {
			RDebug::Printf("KHR: image %08x not on the host: guest copy kept", img);
			break;
		}
		if (fast) {                     /* R G B A bytes -> 0xAARRGGBB words */
			for (int y = 0; y < rows; y++) {
				const TUint8 *q = gOut + y * w * 4;
				unsigned int *o = (unsigned int *)(img->getData() + (y0 + y) * img->getStride());
				for (int x = 0; x < w; x++, q += 4)
					o[x] = (unsigned int)q[3] << 24 | q[0] << 16 | q[1] << 8 | q[2];
			}
			continue;
		}
		Image tmp(Color::formatToDescriptor(VG_sABGR_8888_PRE), w, rows, w * 4, gOut);
		tmp.addReference();
		try {
			img->blit(tmp, 0, 0, 0, y0, w, rows, false);
		} catch (std::bad_alloc) {
		}
		tmp.removeReference();
	}
	img->m_hostGen = img->generation();     /* the host texture holds these pixels */
	if (img->m_hostLive)
		img->m_hostAhead = true;            /* still being rendered into */
}

/* an EGL window's swap: the drawable's frame becomes the GCE surface's image on the host */
bool khrHostPresent(const Surface *src, const unsigned int *sid)
{
	if (!src || !src->m_host || !khrHostOn())
		return false;
	unsigned int *p = khrHostEmit(SYGL_VG_PRESENT, 5);
	if (!p)
		return false;
	p[0] = (unsigned int)(uintptr_t)src->m_host;
	p[1] = sid[0]; p[2] = sid[1]; p[3] = sid[2]; p[4] = sid[3];
	return khrHostFlush(0) == KErrNone;     /* else eglos copies the frame itself */
}

void khrHostImageFree(Image *img)
{
	img->m_hostGen = 0;
	if (gState <= 0)
		return;
	unsigned int *p = khrHostEmit(SYGL_TEX_FREE, 1);
	if (p)
		p[0] = vgKey(img);
	vgKeyDrop(img);
}

/* failing EGL calls (RI patch 0004): the first ones of each source line */
void khrEglError(int line, int error)
{
	static int seen[64], n;
	if (error == 0x3000)            /* EGL_SUCCESS */
		return;
	for (int i = 0; i < n; i++)
		if (seen[i] == line)
			return;
	if (n < 64)
		seen[n++] = line;
	RDebug::Printf("EGL error %x at riMiniEGL.cpp:%d (owner %u)", error, line, gOwner);
}

/* what a failing EGL call was given (ri-patches/0009) */
void khrEglNote(const char *what, int value)
{
	static int seen[32], n;
	for (int i = 0; i < n; i++)
		if (seen[i] == value)
			return;
	if (n < 32)
		seen[n++] = value;
	RDebug::Printf("%s 0x%x (owner %u)", what, value, gOwner);
}

/* things the RI drops silently (ri-patches/0008), once per message */
void khrVgNote(const char* what)
{
	static const char* seen[16];
	static int n;
	for (int i = 0; i < n; i++)
		if (seen[i] == what)
			return;
	if (n < 16)
		seen[n++] = what;
	RDebug::Printf("%s (owner %u)", what, gOwner);
}

/* VG errors (ri-patches/0007): code and the calling RI function's address, once per caller; the address
 * is in libEGL.dll: subtract its code base (SYBCODE: line) and look it up in build/roms/<id>/<fw>/khr/obj */
void khrVgError(int error, void* caller)
{
	static void* seen[64];
	static int n;
	if (error == 0)                 /* VG_NO_ERROR */
		return;
	for (int i = 0; i < n; i++)
		if (seen[i] == caller)
			return;
	if (n < 64)
		seen[n++] = caller;
	RDebug::Printf("VG error %x from %p (owner %u)", error, caller, gOwner);
}



}	/* namespace OpenVGRI */

/* ri-patches/0008 declares it at file scope in riApi.cpp */
void khrVgNote(const char* what) { OpenVGRI::khrVgNote(what); }


