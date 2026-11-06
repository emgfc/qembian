/* Host GL backend: the rasteriser's input (window-space triangles with their raster state, clears,
   texture images, read-backs) goes to the syborg-gl QEMU device, which draws it with the host GPU
   (src/qemu-syborg/hw/arm/syborg_gl.c, protocol in syborg_gl_proto.h). Everything before the
   rasteriser (GLES 1.1 state, transform, lighting, clipping, EGL rules) stays in sygl, so behaviour
   is the same with or without it. Transport: DisplayDriver controls 0x7f10/0x7f11 (our d_display
   stand-in copies the buffer into RAM the device reads). No host GL (QEMU -global
   syborg-gl.backend=off, no GPU, old QEMU): sy_hw_on answers 0 and sygl rasterises in software.
   Per thread state only (SyThread): the blob has no writable static data. */
#include "sygl_int.h"
#include "gl_int.h"
#include "../qemu-syborg/hw/arm/syborg_gl_proto.h"

#define KHwBufWords (256 * 1024)	/* 1 MB, as the DisplayDriver's copy buffer */
#define KCtlHwQuery 0x7f10
#define KCtlHwSubmit 0x7f11

static void submit(SyThread* t, void* aOut, int aOutLen);

int sy_hw_on(SyThread* t)
	{
	if (!t) return 0;
	if (t->hwState == 0)
		{
		u32 q[2] = { 0, 0 };		/* host GL ready, the caller's kernel thread id (owner) */
		t->hwState = -1;
		if (!sy_display_open(t) && !sy_display_ctl(t, KCtlHwQuery, q) && q[0])
			{
			t->hwBuf = (u32*)sy_big_alloc(KHwBufWords * 4);
			if (t->hwBuf)
				{
				t->hwState = 1;
				t->hwCap = KHwBufWords;
				t->hwDraw = -1;
				t->hwOwner = q[1];
				}
			}
		SYLOG("host GL backend: %s", t->hwState > 0 ? "on (syborg-gl)" : "off, software rasteriser");
		}
	return t->hwState > 0;
	}

static void close_draw(SyThread* t)
	{
	t->hwDraw = -1;
	t->hwDrawSurf = 0;
	}

/* room for n words; a new buffer starts with SYGL_OWNER */
static u32* emit(SyThread* t, int n)
	{
	if (t->hwLen + n > t->hwCap)
		submit(t, 0, 0);
	if (t->hwLen == 0)
		{
		t->hwBuf[0] = SYGL_OWNER; t->hwBuf[1] = 3; t->hwBuf[2] = t->hwOwner;
		t->hwLen = 3;
		}
	if (t->hwLen + n > t->hwCap)
		return 0;
	u32* p = t->hwBuf + t->hwLen;
	t->hwLen += n;
	return p;
	}

static void submit(SyThread* t, void* aOut, int aOutLen)
	{
	if (t->hwLen == 0 && !aOut)
		return;
	close_draw(t);
	u32 a[4] = { (u32)t->hwBuf, (u32)(t->hwLen * 4), (u32)aOut, (u32)aOutLen };
	int r = sy_display_ctl(t, KCtlHwSubmit, a);
	if (r)
		SYLOG("host GL submit failed %d (%d words)", r, t->hwLen);
	t->hwLen = 0;
	}

void sy_hw_surface_new(SyThread* t, SySurface* s)
	{
	if (!sy_hw_on(t) || (s->kind != 1 && s->kind != 2) || s->w <= 0 || s->h <= 0)
		return;
	u32* p = emit(t, 6);
	if (!p) return;
	s->hw = 1;
	s->hwId = ++t->hwNextId;
	p[0] = SYGL_SURF; p[1] = 6; p[2] = s->hwId; p[3] = (u32)s->w; p[4] = (u32)s->h; p[5] = s->depth != 0;
	}

void sy_hw_surface_free(SyThread* t, SySurface* s)
	{
	if (!s->hw || !t || t->hwState <= 0)
		return;
	if (t->hwDrawSurf == s)
		close_draw(t);
	u32* p = emit(t, 3);
	if (p) { p[0] = SYGL_SURF_FREE; p[1] = 3; p[2] = s->hwId; }
	s->hw = 0;
	}

void sy_hw_tex_free(void* aTex)
	{
	SyThread* t = sy_thread_peek();
	SyTex* x = (SyTex*)aTex;
	if (!t || t->hwState <= 0 || !x || !x->gen)
		return;
	u32* p = emit(t, 3);
	if (p) { p[0] = SYGL_TEX_FREE; p[1] = 3; p[2] = (u32)x; }
	x->gen = 0;
	}

void sy_hw_terminate(SyThread* t)
	{
	if (!t || t->hwState <= 0)
		return;
	u32* p = emit(t, 3);
	if (p) { p[0] = SYGL_OWNER_FREE; p[1] = 3; p[2] = t->hwOwner; }
	submit(t, 0, 0);
	}

/* the texture's current image on the host (bands of at most half a buffer) */
static void upload(SyThread* t, SyTex* x)
	{
	if (x->gen || !x->data || x->w <= 0 || x->h <= 0)
		return;
	close_draw(t);		/* the image must not land between an open draw's vertices */
	x->gen = ++t->hwTexGen ? t->hwTexGen : ++t->hwTexGen;
	int rows = (KHwBufWords / 2) / x->w;
	if (rows < 1) return;
	for (int y = 0; y < x->h; y += rows)
		{
		int n = x->h - y < rows ? x->h - y : rows;
		u32* p = emit(t, 8 + n * x->w);
		if (!p) return;
		p[0] = SYGL_TEX; p[1] = 8 + n * x->w; p[2] = (u32)x; p[3] = x->gen; p[4] = (u32)x->w; p[5] = (u32)x->h;
		p[6] = (u32)y; p[7] = (u32)n;
		memcpy(p + 8, x->data + y * x->w, (unsigned)(n * x->w * 4));
		}
	}

static void state_of(const SyRState* st, SyglState* o)
	{
	memset(o, 0, sizeof(*o));
	o->depthTest = st->depthTest; o->depthWrite = st->depthWrite; o->depthFunc = st->depthFunc;
	if (st->tex)
		{
		SyTex* x = (SyTex*)st->tex->src;
		o->texKey = (u32)x; o->texGen = x ? x->gen : 0;
		o->texHasRGB = st->tex->hasRGB; o->texHasA = st->tex->hasA;
		o->texLinear = st->tex->linear; o->texClampS = st->tex->clampS; o->texClampT = st->tex->clampT;
		o->envMode = (u32)st->envMode;
		o->envColor = (u32)st->envA << 24 | (u32)st->envR << 16 | (u32)st->envG << 8 | (u32)st->envB;
		}
	o->alphaTest = st->alphaTest; o->alphaFunc = st->alphaFunc; o->alphaRef = st->alphaRef;
	o->blend = st->blend; o->blendSrc = st->blendSrc; o->blendDst = st->blendDst;
	o->scX0 = st->scX0; o->scY0 = st->scY0; o->scX1 = st->scX1; o->scY1 = st->scY1;
	o->writeColor = st->writeColor;
	o->polyOffset = st->polyOffset; o->polyFactor = st->polyFactor; o->polyUnits = st->polyUnits;
	o->fog = st->fog; o->fogColor = (u32)st->fogR << 16 | (u32)st->fogG << 8 | (u32)st->fogB;
	}

static int same_words(const u32* a, const u32* b, int n)
	{
	for (int i = 0; i < n; ++i)
		if (a[i] != b[i]) return 0;
	return 1;
	}

static void vtx(float* p, const SyRVtx* v)
	{
	p[0] = v->x; p[1] = v->y; p[2] = v->z; p[3] = v->r; p[4] = v->g; p[5] = v->b; p[6] = v->a;
	p[7] = v->iw; p[8] = v->sw; p[9] = v->tw; p[10] = v->fog; p[11] = 0;
	}

void sy_hw_triangle(SySurface* s, const SyRState* st, const SyRVtx* v0, const SyRVtx* v1, const SyRVtx* v2)
	{
	SyThread* t = sy_thread_peek();
	if (!t || t->hwState <= 0) return;
	if (st->tex && st->tex->src)
		upload(t, (SyTex*)st->tex->src);	/* before the draw that uses it */
	SyglState ns;
	state_of(st, &ns);
	const int tri = 3 * SYGL_VTX_WORDS;
	if (t->hwDraw >= 0 && t->hwDrawSurf == s && t->hwLen + tri <= t->hwCap
		&& same_words(t->hwBuf + t->hwDraw + 3, (const u32*)&ns, (int)SYGL_STATE_WORDS))
		;	/* same surface and state: extend the open draw */
	else
		{
		close_draw(t);
		u32* p = emit(t, 4 + SYGL_STATE_WORDS + tri);
		if (!p) return;
		t->hwLen -= tri;					/* the triangle goes in below */
		p[0] = SYGL_DRAW; p[1] = 4 + SYGL_STATE_WORDS; p[2] = s->hwId;
		memcpy(p + 3, &ns, sizeof(ns));
		p[3 + SYGL_STATE_WORDS] = 0;
		t->hwDraw = p - t->hwBuf;
		t->hwDrawSurf = s;
		}
	u32* d = t->hwBuf + t->hwDraw;
	float* p = (float*)(t->hwBuf + t->hwLen);
	vtx(p, v0); vtx(p + SYGL_VTX_WORDS, v1); vtx(p + 2 * SYGL_VTX_WORDS, v2);
	t->hwLen += tri;
	d[1] += tri;
	d[3 + SYGL_STATE_WORDS] += 3;
	s->hwDirty = 1;
	}

void sy_hw_clear(SySurface* s, int aColor, u32 aRgb, int aDepth, u16 aZ, int aX, int aY, int aW, int aH)
	{
	SyThread* t = sy_thread_peek();
	if (!t || t->hwState <= 0) return;
	close_draw(t);
	u32* p = emit(t, 10);
	if (!p) return;
	p[0] = SYGL_CLEAR; p[1] = 10; p[2] = s->hwId; p[3] = (aColor ? 1 : 0) | (aDepth ? 2 : 0);
	p[4] = aRgb; p[5] = aZ; p[6] = (u32)aX; p[7] = (u32)aY; p[8] = (u32)aW; p[9] = (u32)aH;
	s->hwDirty = 1;
	}

/* read the host's picture back into s->color (bands of one buffer) */
void sy_hw_sync(SySurface* s)
	{
	SyThread* t = sy_thread_peek();
	if (!s || !s->hw || !s->hwDirty || !t || t->hwState <= 0 || !s->color)
		return;
	int rows = KHwBufWords / s->w;
	for (int y = 0; y < s->h; y += rows)
		{
		int n = s->h - y < rows ? s->h - y : rows;
		u32* p = emit(t, 6);
		if (!p) return;
		p[0] = SYGL_READ; p[1] = 6; p[2] = s->hwId; p[3] = 0; p[4] = (u32)y; p[5] = (u32)n;
		submit(t, s->color + y * s->w, n * s->w * 4);
		}
	s->hwDirty = 0;
	}
