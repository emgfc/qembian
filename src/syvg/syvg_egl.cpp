#include <array>
/*
 * syvg: the phone's EGL, on the host (docs/devices/nokia-n8-rm596/gles-host.md, egl-vg-host-v2.md; protocol
 * src/syvg/egl2_fn.h). The guest only forwards its EGL calls. Here: the device's configs (the real phone's,
 * the profile's file), displays, surfaces, contexts, the bound API and the current contexts of each guest
 * thread (one per API, as EGL 1.4 has it), errors, strings (the profile's [gl.identity]).
 *   OpenVG: contexts and surfaces of the Khronos RI's MiniEGL (renamed syvg_egl* in this build: inside this
 *           file eglX is MiniEGL's), made when a VG context first uses a surface.
 *   OpenGL ES 1.1/2: syborg-gl's host contexts and surfaces (host batches: SYGL_GL2_NEW / _CURRENT / _FREE,
 *           SYGL_SURF, SYGL_VG_PRESENT, SYGL_GL_TEXIMAGE); the GL replay asks syvg_gl_current which
 *           context a guest thread has.
 * SPDX-License-Identifier: MIT
 */
#include "riDefs.h"
#include "egl.h"
#include "syvg.h"
#include "vghost.h"
#include "egl2_fn.h"
#include "../qemu-syborg/hw/arm/syborg_gl_proto.h"
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>

using OpenVGRI::vgSetOwner;
using OpenVGRI::khrHostFlush;
using OpenVGRI::khrHostEmit;

extern "C" uintptr_t syvg_vg_host_handle(uint32_t id);     /* vg2.cpp: a VG handle id -> the RI's handle */

#ifndef EGL_OPENGL_ES2_BIT
#define EGL_OPENGL_ES2_BIT 0x0004
#endif
#define EGL_OPENGL_ES_API_ 0x30A0
#define EGL_OPENVG_API_ 0x30A1

/* ---- configs: the device's (attribute -> value), handles are their EGL_CONFIG_IDs */
struct Cfg {
	std::unordered_map<EGLint, EGLint> a;
	EGLConfig ri = 0;           /* the MiniEGL config VG surfaces/contexts of this config use */
	bool riDone = false;
	EGLint get(EGLint k, EGLint d = 0) const { auto it = a.find(k); return it == a.end() ? d : it->second; }
};
static std::vector<Cfg> gCfg;

extern "C" void syvg_egl_load_configs(const char *path)
{
	gCfg.clear();
	FILE *f = path && *path ? fopen(path, "r") : NULL;
	if (!f) {
		if (path && *path)
			fprintf(stderr, "syvg: EGL configs %s: not readable\n", path);
		return;
	}
	char line[4096];
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#' || line[0] == '\n')
			continue;
		Cfg c;
		for (char *t = strtok(line, " \n"); t; t = strtok(NULL, " \n")) {
			char *eq = strchr(t, '=');
			if (eq)
				c.a[(EGLint)strtol(t, NULL, 0)] = (EGLint)strtol(eq + 1, NULL, 0);
		}
		if (!c.a.empty())
			gCfg.push_back(c);
	}
	fclose(f);
}

static Cfg *cfgOf(uint32_t id)
{
	for (auto &c : gCfg)
		if ((uint32_t)c.get(EGL_CONFIG_ID) == id)
			return &c;
	return NULL;
}

/* the MiniEGL config with this config's colour and mask sizes (for OpenVG) */
static EGLConfig riConfig(Cfg *c)
{
	if (c->riDone)
		return c->ri;
	c->riDone = true;
	EGLint want[] = { c->get(EGL_RED_SIZE), c->get(EGL_GREEN_SIZE), c->get(EGL_BLUE_SIZE), c->get(EGL_ALPHA_SIZE),
	                  c->get(EGL_ALPHA_MASK_SIZE) };
	EGLint n = 0;
	EGLConfig all[256];
	eglGetConfigs((EGLDisplay)1, all, 256, &n);
	int best = -1, bestScore = 1 << 30;
	for (int i = 0; i < n; i++) {
		EGLint v[5], rt = 0;
		eglGetConfigAttrib((EGLDisplay)1, all[i], EGL_RED_SIZE, &v[0]);
		eglGetConfigAttrib((EGLDisplay)1, all[i], EGL_GREEN_SIZE, &v[1]);
		eglGetConfigAttrib((EGLDisplay)1, all[i], EGL_BLUE_SIZE, &v[2]);
		eglGetConfigAttrib((EGLDisplay)1, all[i], EGL_ALPHA_SIZE, &v[3]);
		eglGetConfigAttrib((EGLDisplay)1, all[i], EGL_ALPHA_MASK_SIZE, &v[4]);
		eglGetConfigAttrib((EGLDisplay)1, all[i], EGL_RENDERABLE_TYPE, &rt);
		if (!(rt & EGL_OPENVG_BIT))
			continue;
		int score = 0;
		for (int k = 0; k < 5; k++)
			score += (v[k] - want[k]) * (v[k] - want[k]) * (k == 4 ? 4 : 1);
		if (score < bestScore) {
			bestScore = score;
			best = i;
		}
	}
	c->ri = best >= 0 ? all[best] : 0;
	return c->ri;
}

/* ---- objects of a guest process */
enum { KWindow = 1, KPbuffer, KPixmap, KClientBuffer };
struct Surf {
	int kind = 0;
	uint32_t cfg = 0;
	int w = 0, h = 0;
	SyvgHostWindow *win = NULL;         /* windows: the guest's GCE surface and size */
	EGLSurface vg = 0;                  /* the MiniEGL surface (OpenVG) */
	uint32_t gl = 0;                    /* the syborg-gl surface (OpenGL ES), 0 until used */
	uint32_t vgImage = 0;               /* client buffer surfaces: the VG image id */
	EGLint swap = EGL_BUFFER_DESTROYED, resolve = EGL_MULTISAMPLE_RESOLVE_DEFAULT, texFormat = EGL_NO_TEXTURE,
	       texTarget = EGL_NO_TEXTURE, mipmap = 0, mipLevel = 0, largest = 0;
	EGLint vgAlpha = EGL_VG_ALPHA_FORMAT_NONPRE, vgColor = EGL_VG_COLORSPACE_sRGB;
	int current = 0;                    /* bound as draw/read surface (count) */
	bool dead = false;                  /* destroyed while current: freed when released */
};
struct Ctx {
	EGLenum api = 0;
	int version = 1;
	uint32_t cfg = 0;
	EGLContext vg = 0;                  /* MiniEGL's */
	uint32_t gl = 0;                    /* syborg-gl's context id */
	int current = 0;
	bool dead = false;
};
struct Thread {
	EGLenum api = EGL_OPENGL_ES_API_;
	uint32_t ctx[2] = { 0, 0 }, draw[2] = { 0, 0 }, read[2] = { 0, 0 };     /* [0] OpenGL ES, [1] OpenVG */
	EGLint err = EGL_SUCCESS;
};
/* an EGL sync object (EGL_KHR_reusable_sync; EGL_NOK_resource_profiling's memory-usage syncs) */
struct Sync {
	EGLenum type = 0;
	EGLint status = 0x30F3;             /* EGL_UNSIGNALED_KHR */
	EGLint condition = 0;               /* profiling: EGL_PROF_TOTAL_MEMORY_USAGE_GT/LT_NOK */
	uint64_t threshold = 0;             /* profiling: bytes */
};
/* EGL_NOK_image_endpoint: a consumer endpoint on a GCE surface. The producer's frames are known from the
   window server's backend (gcebe: SYGL_SURF_UPDATED, guest-drawn surfaces) and from host presents (syborg-gl
   vg_present); an acquire snapshots the current frame into a host texture (the EGLImage) */
struct Endpoint {
	uint32_t sid[4] = { 0, 0, 0, 0 };
	int w = 0, h = 0, stride = 0, nbuf = 1;
	uint32_t format = 0, off[2] = { 0, 0 }, base = 0;   /* the surface as mapped in the owner process (0: not) */
	int buffer = 0;                     /* the producer's current buffer (-1: a host image) */
	bool host = false;                  /* the last frame was presented on the host */
	int updates = 0;                    /* frames since the last acquire (EGL_ENDPOINT_READY_NOK) */
	EGLint delay = 0;                   /* EGL_DELAY_NOK */
	bool streaming = false, armed = false, due = false;
	std::vector<std::array<int, 4> > dirty;     /* rectangles since the last acquire */
	uint32_t acquired = 0;              /* the image handed out and not released */
};
struct EglImage {
	uint32_t key;                       /* its host texture (this owner's) */
	int w, h;
};
struct Proc {
	std::unordered_map<uint32_t, Endpoint> endpoints;
	std::unordered_map<uint32_t, EglImage> images;
	std::unordered_map<uint32_t, Sync> syncs;
	std::unordered_map<uint32_t, Surf> surfs;
	std::unordered_map<uint32_t, Ctx> ctxs;
	std::unordered_map<uint32_t, Thread> threads;
	uint32_t next = 1;
	int initialized = 0;
};
static std::unordered_map<uint32_t, Proc> gProc;
static uint32_t gImageKey = 0xE0000000u;    /* EGLImage textures: a key range of their own (even, as vgKey) */
static const char *const KEglNames[] = { "GetError", "GetDisplay", "Initialize", "Terminate", "QueryString", "GetConfigs", "ChooseConfig", "GetConfigAttrib", "CreateWindowSurface", "CreatePbufferSurface", "CreatePbufferFromClientBuffer", "DestroySurface", "QuerySurface", "BindAPI", "QueryAPI", "WaitClient", "ReleaseThread", "SurfaceAttrib", "BindTexImage", "ReleaseTexImage", "SwapInterval", "CreateContext", "DestroyContext", "MakeCurrent", "GetCurrentContext", "GetCurrentSurface", "GetCurrentDisplay", "QueryContext", "WaitGL", "WaitNative", "SwapBuffers", "WindowUpdate", "CreatePixmapSurface", "CopyBuffers", "GetProcAddress", "QueryProfilingDataNOK", "CreateSyncKHR", "DestroySyncKHR", "ClientWaitSyncKHR", "SignalSyncKHR", "GetSyncAttribKHR", "CreateEndpointNOK", "DestroyEndpointNOK", "GetEndpointAttribNOK", "SetEndpointAttribNOK", "EndpointBeginStreamingNOK", "EndpointEndStreamingNOK", "AcquireImageNOK", "ReleaseImageNOK", "GetEndpointDirtyAreaNOK", "EndpointRequestNotificationNOK", "EndpointCancelNotificationNOK", "EndpointPollNOK", "DestroyImageKHR", "CreateEGLImageTargetKHR", "COUNT" };
static bool EglTrace()
{
	static int on = -1;
	if (on < 0)
		on = getenv("SYBORG_EGL_TRACE") && *getenv("SYBORG_EGL_TRACE") == '1';
	return on;
}
static const size_t KMaxThreads = 512, KMaxObjects = 4096;    /* per process (S6, S10) */
static uint32_t gOwnerNow, gThreadNow;

static Proc &P() { return gProc[gOwnerNow]; }
static Thread &T() { return P().threads[gThreadNow]; }
static int slot(EGLenum api) { return api == EGL_OPENVG_API_ ? 1 : 0; }

static uint32_t fail(EGLint e) { T().err = e; return 0; }

/* the syborg-gl ids of GL objects: surfaces share the device's table with OpenVG's (even keys from 0x10) */
static uint32_t glSurfId(uint32_t id) { return 0x80000000u | id; }
static uint32_t glCtxId(uint32_t id) { return 0x40000000u | id; }

/* a producer's new frame of GCE surface sid (syborg-gl: gcebe's SYGL_SURF_UPDATED with the buffer, or a host
   present with buffer -1): every endpoint on it gets the update and its dirty rectangle */
extern "C" uint32_t syvg_vg_image_from_texture(uint32_t owner, int w, int h, uint32_t srcKey);   /* vg2.cpp */
/* The last update of every surface, endpoint or not: a consumer often makes its endpoint after the producer's
   first frame (the 808 browser: the web process swaps once, then waits for the browser to take that frame).
   A new endpoint starts with that frame waiting, as the surface's current content [I: the extension's
   "content submitted before creation" case; without it the browser page stayed white until a full redraw]. */
struct SurfLast { uint32_t sid[4]; bool host; int32_t buffer; };
static SurfLast gSurfLast[256];         /* recent surfaces; the least recently added slot is reused */
static unsigned gSurfLastNext;

static SurfLast *surf_last(const uint32_t *sid, bool add)
{
	for (auto &l : gSurfLast)
		if (!memcmp(l.sid, sid, 16))
			return &l;
	if (!add)
		return NULL;
	SurfLast *l = &gSurfLast[gSurfLastNext++ % 256];
	memcpy(l->sid, sid, 16);
	l->host = false;
	l->buffer = 0;
	return l;
}

extern "C" void syvg_endpoint_updated(const uint32_t *sid, int32_t buffer, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	{
		SurfLast *it = surf_last(sid, true);
		if (buffer < 0)
			it->host = true;
		else
			it->buffer = buffer;
	}
	for (auto &pp : gProc)
		for (auto &ep : pp.second.endpoints) {
			Endpoint &e = ep.second;
			if (memcmp(e.sid, sid, 16))
				continue;
			if (buffer < 0)
				e.host = true;          /* presented on the host: its frames stay there (the producer's
				                           SubmitUpdate that follows reports buffer 0 of an unused buffer) */
			else
				e.buffer = buffer;
			e.updates++;
			if (e.dirty.size() < 64)        /* w 0: the whole surface */
				e.dirty.push_back({ (int)x, (int)y, w ? (int)w : e.w, w ? (int)h : e.h });
			if (e.armed)
				e.due = true;
		}
}

/* SYBORG_VG_TRACE: the OpenVG draw surface current in a guest thread, as text ("none", "win 360x640 id 5", ...) */
extern "C" const char *syvg_vg_target(uint32_t owner, uint32_t thread, char *buf, size_t n)
{
	auto p = gProc.find(owner);
	if (p == gProc.end())
		return "no-egl";
	auto t = p->second.threads.find(thread);
	if (t == p->second.threads.end() || !t->second.draw[1])
		return "none";
	auto sf = p->second.surfs.find(t->second.draw[1]);
	if (sf == p->second.surfs.end())
		return "gone";
	static const char *const kind[] = { "?", "window", "pbuffer", "pixmap", "image" };
	snprintf(buf, n, "%s %dx%d id %u vgimage %u", kind[sf->second.kind < 5 ? sf->second.kind : 0], sf->second.w,
	         sf->second.h, t->second.draw[1], sf->second.vgImage);
	return buf;
}

extern "C" uint32_t syvg_gl_current(uint32_t owner, uint32_t thread)
{
	auto p = gProc.find(owner);
	if (p == gProc.end())
		return 0;
	auto t = p->second.threads.find(thread);
	if (t == p->second.threads.end() || !t->second.ctx[0])
		return 0;
	auto c = p->second.ctxs.find(t->second.ctx[0]);
	return c == p->second.ctxs.end() ? 0 : c->second.gl;
}

/* ---- host batches to syborg-gl (OpenGL ES objects) */
static void hostCmd(int op, const uint32_t *w, int n)
{
	vgSetOwner(gOwnerNow);
	unsigned int *p = khrHostEmit(op, n);
	if (!p)
		return;
	for (int i = 0; i < n; i++)
		p[i] = w[i];
	khrHostFlush(0);
}

static void glSurfMake(Surf &s, uint32_t id)
{
	if (s.gl || s.w <= 0 || s.h <= 0)
		return;
	Cfg *c = cfgOf(s.cfg);
	uint32_t fl = 0;
	if (c && c->get(EGL_DEPTH_SIZE))
		fl |= SYGL_SURF_DEPTH;
	if (c && c->get(EGL_STENCIL_SIZE))
		fl |= SYGL_SURF_DEPTH | SYGL_SURF_STENCIL;
	s.gl = glSurfId(id);
	uint32_t w[4] = { s.gl, (uint32_t)s.w, (uint32_t)s.h, fl };
	hostCmd(SYGL_SURF, w, 4);
}

static void freeSurf(uint32_t id)
{
	auto it = P().surfs.find(id);
	if (it == P().surfs.end())
		return;
	Surf &s = it->second;
	if (s.vg)
		eglDestroySurface((EGLDisplay)1, s.vg);
	if (s.gl) {
		uint32_t w[1] = { s.gl };
		hostCmd(SYGL_SURF_FREE, w, 1);
	}
	delete s.win;
	P().surfs.erase(it);
}

static void freeCtx(uint32_t id)
{
	auto it = P().ctxs.find(id);
	if (it == P().ctxs.end())
		return;
	if (it->second.vg)
		eglDestroyContext((EGLDisplay)1, it->second.vg);
	if (it->second.gl) {
		uint32_t w[1] = { it->second.gl };
		hostCmd(SYGL_GL2_FREE, w, 1);
	}
	P().ctxs.erase(it);
}

/* ---- guest memory */
static bool attribs(uint32_t va, std::vector<EGLint> &out)
{
	out.clear();
	if (!va)
		return true;
	for (int i = 0; i < 256; i += 2) {
		EGLint kv[2];
		if (syvg_guest_read(va + i * 4, kv, 4))
			return false;
		if (kv[0] == EGL_NONE) {
			out.push_back(EGL_NONE);
			return true;
		}
		if (syvg_guest_read(va + i * 4 + 4, kv + 1, 4))
			return false;
		out.push_back(kv[0]);
		out.push_back(kv[1]);
	}
	return false;
}

static bool putInt(uint32_t va, EGLint v) { return !va || !syvg_guest_write(va, &v, 4); }

/* ---- config choice (EGL 1.4 section 3.4.1) */
enum { MAtLeast, MExact, MMask, MIgnore };
static int matchKind(EGLint a)
{
	switch (a) {
	case EGL_BUFFER_SIZE: case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: case EGL_LUMINANCE_SIZE:
	case EGL_ALPHA_SIZE: case EGL_ALPHA_MASK_SIZE: case EGL_DEPTH_SIZE: case EGL_STENCIL_SIZE: case EGL_SAMPLES:
	case EGL_SAMPLE_BUFFERS:
		return MAtLeast;
	case EGL_SURFACE_TYPE: case EGL_RENDERABLE_TYPE: case EGL_CONFORMANT:
		return MMask;
	case EGL_MAX_PBUFFER_WIDTH: case EGL_MAX_PBUFFER_HEIGHT: case EGL_MAX_PBUFFER_PIXELS: case EGL_NATIVE_VISUAL_ID:
	case EGL_MATCH_NATIVE_PIXMAP:
		return MIgnore;
	default:
		return MExact;
	}
}

static bool knownAttr(EGLint a)
{
	return a >= EGL_BUFFER_SIZE && a <= EGL_CONFORMANT;
}

static bool chooseConfigs(const std::vector<EGLint> &at, std::vector<uint32_t> &out)
{
	std::unordered_map<EGLint, EGLint> want;
	want[EGL_SURFACE_TYPE] = EGL_WINDOW_BIT;
	want[EGL_RENDERABLE_TYPE] = EGL_OPENGL_ES_BIT;
	want[EGL_CONFIG_CAVEAT] = EGL_DONT_CARE;
	want[EGL_COLOR_BUFFER_TYPE] = EGL_RGB_BUFFER;
	want[EGL_TRANSPARENT_TYPE] = EGL_NONE;
	for (size_t i = 0; i + 1 < at.size() && at[i] != EGL_NONE; i += 2) {
		if (!knownAttr(at[i])) {
			fail(EGL_BAD_ATTRIBUTE);
			return false;
		}
		want[at[i]] = at[i + 1];
	}
	auto id = want.find(EGL_CONFIG_ID);
	if (id != want.end() && id->second != EGL_DONT_CARE) {
		if (cfgOf(id->second))
			out.push_back(id->second);
		return true;
	}
	std::vector<Cfg *> m;
	for (auto &c : gCfg) {
		bool ok = true;
		for (auto &kv : want) {
			if (kv.second == EGL_DONT_CARE)
				continue;
			EGLint v = c.get(kv.first);
			switch (matchKind(kv.first)) {
			case MAtLeast: ok = v >= kv.second; break;
			case MMask: ok = (v & kv.second) == kv.second; break;
			case MExact: ok = v == kv.second; break;
			default: break;
			}
			if (!ok)
				break;
		}
		if (ok)
			m.push_back(&c);
	}
	/* the sort of EGL 1.4 table 3.4 */
	auto caveat = [](EGLint v) { return v == EGL_NONE ? 0 : v == EGL_SLOW_CONFIG ? 1 : 2; };
	auto bits = [&](const Cfg *c) {
		int b = 0;
		const EGLint ks[] = { EGL_RED_SIZE, EGL_GREEN_SIZE, EGL_BLUE_SIZE, EGL_ALPHA_SIZE, EGL_LUMINANCE_SIZE };
		for (EGLint k : ks) {
			auto w = want.find(k);
			if (w != want.end() && w->second != 0 && w->second != EGL_DONT_CARE)
				b += c->get(k);
		}
		return b;
	};
	std::stable_sort(m.begin(), m.end(), [&](const Cfg *x, const Cfg *y) {
		if (caveat(x->get(EGL_CONFIG_CAVEAT)) != caveat(y->get(EGL_CONFIG_CAVEAT)))
			return caveat(x->get(EGL_CONFIG_CAVEAT)) < caveat(y->get(EGL_CONFIG_CAVEAT));
		if (x->get(EGL_COLOR_BUFFER_TYPE) != y->get(EGL_COLOR_BUFFER_TYPE))
			return x->get(EGL_COLOR_BUFFER_TYPE) == EGL_RGB_BUFFER;
		if (bits(x) != bits(y))
			return bits(x) > bits(y);
		const EGLint smaller[] = { EGL_BUFFER_SIZE, EGL_SAMPLE_BUFFERS, EGL_SAMPLES, EGL_DEPTH_SIZE, EGL_STENCIL_SIZE,
		                           EGL_ALPHA_MASK_SIZE, EGL_CONFIG_ID };
		for (EGLint k : smaller)
			if (x->get(k) != y->get(k))
				return x->get(k) < y->get(k);
		return false;
	});
	for (Cfg *c : m)
		out.push_back(c->get(EGL_CONFIG_ID));
	return true;
}

/* ---- current contexts */

/* the MiniEGL surface an OpenVG context draws on (made on first use) */
static EGLSurface vgSurface(uint32_t id)
{
	auto it = P().surfs.find(id);
	if (it == P().surfs.end())
		return EGL_NO_SURFACE;
	Surf &s = it->second;
	if (s.vg)
		return s.vg;
	Cfg *cfg = cfgOf(s.cfg);
	EGLConfig rc = cfg ? riConfig(cfg) : 0;
	if (!rc)
		return EGL_NO_SURFACE;
	eglBindAPI(EGL_OPENVG_API);
	const EGLint at[] = { EGL_VG_ALPHA_FORMAT, s.vgAlpha, EGL_VG_COLORSPACE, s.vgColor, EGL_NONE };
	if (s.kind == KWindow) {
		s.vg = eglCreateWindowSurface((EGLDisplay)1, rc, (EGLNativeWindowType)s.win, at);
	} else if (s.kind == KPbuffer) {
		const EGLint pb[] = { EGL_WIDTH, s.w, EGL_HEIGHT, s.h, EGL_VG_ALPHA_FORMAT, s.vgAlpha,
		                      EGL_VG_COLORSPACE, s.vgColor, EGL_NONE };
		s.vg = eglCreatePbufferSurface((EGLDisplay)1, rc, pb);
	}
	if (s.vg && s.swap != EGL_BUFFER_DESTROYED)
		eglSurfaceAttrib((EGLDisplay)1, s.vg, EGL_SWAP_BEHAVIOR, s.swap);
	return s.vg;
}

static uint32_t makeCurrent(uint32_t draw, uint32_t read, uint32_t ctx)
{
	Proc &p = P();
	Thread &t = T();
	int k = slot(t.api);
	if (!ctx && (draw || read))
		return fail(EGL_BAD_MATCH);
	Ctx *c = NULL;
	if (ctx) {
		auto it = p.ctxs.find(ctx);
		if (it == p.ctxs.end() || it->second.dead)
			return fail(EGL_BAD_CONTEXT);
		c = &it->second;
		k = slot(c->api);
		if (!draw || !read)
			return fail(EGL_BAD_MATCH);
		if (!p.surfs.count(draw) || !p.surfs.count(read) || p.surfs[draw].dead || p.surfs[read].dead)
			return fail(EGL_BAD_SURFACE);
	}
	uint32_t old = t.ctx[k], od = t.draw[k], orr = t.read[k];
	if (k == 1) {
		eglBindAPI(EGL_OPENVG_API);
		if (c) {
			EGLSurface ds = vgSurface(draw), rs = vgSurface(read);
			if (!ds || !rs || !eglMakeCurrent((EGLDisplay)1, ds, rs, c->vg))
				return fail(EGL_BAD_MATCH);
		} else {
			eglMakeCurrent((EGLDisplay)1, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		}
	} else if (c) {
		Surf &ds = p.surfs[draw];
		glSurfMake(ds, draw);
		glSurfMake(p.surfs[read], read);
		uint32_t w[4] = { c->gl, ds.gl, (uint32_t)ds.w, (uint32_t)ds.h };
		hostCmd(SYGL_GL2_CURRENT, w, 4);
	}
	t.ctx[k] = ctx; t.draw[k] = draw; t.read[k] = read;
	if (c) {
		c->current++;
		p.surfs[draw].current++;
		p.surfs[read].current++;
	}
	/* what was released and destroyed meanwhile goes now */
	if (old) {
		auto o = p.ctxs.find(old);
		if (o != p.ctxs.end() && --o->second.current <= 0 && o->second.dead)
			freeCtx(old);
	}
	for (uint32_t sid : { od, orr }) {
		auto o = p.surfs.find(sid);
		if (sid && o != p.surfs.end() && --o->second.current <= 0 && o->second.dead)
			freeSurf(sid);
	}
	return 1;
}

/* ---- surfaces */
static uint32_t newSurf(Surf &&s)
{
	Proc &p = P();
	uint32_t id = p.next++;
	p.surfs[id] = std::move(s);
	return id;
}

static bool surfAttribs(Surf &s, const std::vector<EGLint> &at)
{
	for (size_t i = 0; i + 1 < at.size() && at[i] != EGL_NONE; i += 2) {
		switch (at[i]) {
		case EGL_WIDTH: s.w = at[i + 1]; break;
		case EGL_HEIGHT: s.h = at[i + 1]; break;
		case EGL_LARGEST_PBUFFER: s.largest = at[i + 1]; break;
		case EGL_TEXTURE_FORMAT: s.texFormat = at[i + 1]; break;
		case EGL_TEXTURE_TARGET: s.texTarget = at[i + 1]; break;
		case EGL_MIPMAP_TEXTURE: s.mipmap = at[i + 1]; break;
		case EGL_VG_ALPHA_FORMAT: s.vgAlpha = at[i + 1]; break;
		case EGL_VG_COLORSPACE: s.vgColor = at[i + 1]; break;
		case EGL_RENDER_BUFFER: break;
		default:
			if (at[i] < 0x3000 || at[i] > 0x33ff)      /* the phone's extensions' attributes are accepted */
				return false;
			break;
		}
	}
	return true;
}

/* ---- the call: {fn, guest thread, args}; the result word at out[0] (strings: length, bytes) */
extern "C" int syvg_egl_call(uint32_t owner, const uint32_t *w, size_t n, uint32_t *out, size_t outWords)
{
	if (n < 2 || !out || outWords < 1)
		return -1;
	gOwnerNow = owner;
	gThreadNow = w[1];
	syvg_set_caller(owner, w[1]);
	vgSetOwner(owner);
	const uint32_t *a = w + 2;
	size_t na = n - 2;
	uint32_t r = 0;
	std::vector<EGLint> at;
	Proc &p = P();
	/* S10: thread ids come from the caller; idle records (no context or surface, no pending error) go when
	   a process has many, and a process with thousands of busy ones gets no more */
	if (!p.threads.count(gThreadNow) && p.threads.size() >= KMaxThreads) {
		for (auto it = p.threads.begin(); it != p.threads.end();) {
			const Thread &x = it->second;
			bool idle = !x.ctx[0] && !x.ctx[1] && !x.draw[0] && !x.draw[1] && !x.read[0] && !x.read[1] &&
			            x.err == EGL_SUCCESS;
			it = idle ? p.threads.erase(it) : std::next(it);
		}
		if (p.threads.size() >= KMaxThreads * 8)
			return -1;
	}
	Thread &t = T();
	if (w[0] != EGL2_FN_GetError)
		t.err = EGL_SUCCESS;
	switch (w[0]) {                     /* S6: a budget of EGL objects per process */
	case EGL2_FN_CreateWindowSurface: case EGL2_FN_CreatePbufferSurface:
	case EGL2_FN_CreatePbufferFromClientBuffer: case EGL2_FN_CreatePixmapSurface: case EGL2_FN_CreateContext:
		if (p.surfs.size() + p.ctxs.size() >= KMaxObjects) {
			out[0] = fail(EGL_BAD_ALLOC);
			return 0;
		}
	}
#define NEED(k) do { if (na < (k)) return -1; } while (0)
#define DPY(d) do { if ((d) != 1) { r = fail(EGL_BAD_DISPLAY); goto done; } } while (0)
#define INIT() do { if (!p.initialized) { r = fail(EGL_NOT_INITIALIZED); goto done; } } while (0)
	switch (w[0]) {
	case EGL2_FN_GetError: r = t.err; t.err = EGL_SUCCESS; break;
	case EGL2_FN_GetDisplay: NEED(1); r = 1; break;
	case EGL2_FN_Initialize:
		NEED(3);
		DPY(a[0]);
		if (!p.initialized)
			eglInitialize((EGLDisplay)1, NULL, NULL);      /* MiniEGL, for OpenVG */
		p.initialized = 1;
		if (!putInt(a[1], 1) || !putInt(a[2], 4))
			return -1;
		r = 1;
		break;
	case EGL2_FN_Terminate:
		NEED(1);
		DPY(a[0]);
		p.initialized = 0;          /* objects stay until released (current) or the process goes */
		p.syncs.clear();            /* sync handles end with the display (EGL_KHR_reusable_sync) */
		for (auto &im : p.images) { /* so do endpoints and EGLImages */
			uint32_t k = im.second.key;
			hostCmd(SYGL_TEX_FREE, &k, 1);
		}
		p.images.clear();
		p.endpoints.clear();
		r = 1;
		break;
	case EGL2_FN_QueryString: {
		NEED(2);
		const char *s = NULL;
		if (a[0] != 1)
			fail(EGL_BAD_DISPLAY);
		else if (!p.initialized)
			fail(EGL_NOT_INITIALIZED);
		else
			switch ((EGLint)a[1]) {
			case EGL_VENDOR: s = syborg_gl_identity("egl_vendor"); break;
			case EGL_VERSION: s = syborg_gl_identity("egl_version"); break;
			case EGL_EXTENSIONS: s = syborg_gl_identity("egl_extensions"); break;
			case EGL_CLIENT_APIS: s = syborg_gl_identity("egl_client_apis"); break;
			default: fail(EGL_BAD_PARAMETER); break;
			}
		size_t len = s ? strlen(s) : 0, room = (outWords - 1) * 4;
		if (len > room)
			len = room;
		out[0] = s ? (uint32_t)len : ~0u;
		if (len)
			memcpy(out + 1, s, len);
		return 0;
	}
	case EGL2_FN_GetConfigs:
	case EGL2_FN_ChooseConfig: {
		bool choose = w[0] == EGL2_FN_ChooseConfig;
		NEED(choose ? 5 : 4);
		DPY(a[0]);
		INIT();
		const uint32_t *b = choose ? a + 1 : a;      /* configs*, size, num* */
		if (!b[3]) {
			r = fail(EGL_BAD_PARAMETER);
			break;
		}
		std::vector<uint32_t> ids;
		if (choose) {
			if (!attribs(a[1], at))
				return -1;
			if (!chooseConfigs(at, ids))
				break;
		} else {
			for (auto &c : gCfg)
				ids.push_back(c.get(EGL_CONFIG_ID));
		}
		EGLint num = (EGLint)ids.size();
		if (b[1]) {
			num = (EGLint)b[2] < num ? (EGLint)b[2] : num;
			if (num < 0)
				num = 0;
			for (EGLint i = 0; i < num; i++)
				if (!putInt(b[1] + i * 4, ids[i]))
					return -1;
		}
		if (!putInt(b[3], num))
			return -1;
		if (EglTrace()) {
			fprintf(stderr, "syvg: EGL owner %u %sConfig: %d matched of %zu, attribs", owner, choose ? "Choose" : "Get",
			        (int)ids.size(), gCfg.size());
			for (size_t k = 0; k + 1 < at.size(); k += 2)
				fprintf(stderr, " %x=%x", at[k], at[k + 1]);
			fprintf(stderr, "\n");
		}
		r = 1;
		break;
	}
	case EGL2_FN_GetConfigAttrib: {
		NEED(4);
		DPY(a[0]);
		INIT();
		Cfg *c = cfgOf(a[1]);
		if (!c) {
			r = fail(EGL_BAD_CONFIG);
			break;
		}
		auto it = c->a.find((EGLint)a[2]);
		if (it == c->a.end()) {
			r = fail(EGL_BAD_ATTRIBUTE);      /* as the phone (PRESERVED_RESOURCES, MATCH_NATIVE_PIXMAP) */
			break;
		}
		if (!putInt(a[3], it->second))
			return -1;
		r = 1;
		break;
	}
	case EGL2_FN_CreateWindowSurface: {
		NEED(9);
		DPY(a[0]);
		INIT();
		Cfg *c = cfgOf(a[1]);
		if (!c) {
			r = fail(EGL_BAD_CONFIG);
			break;
		}
		if (!(c->get(EGL_SURFACE_TYPE) & EGL_WINDOW_BIT)) {
			r = fail(EGL_BAD_MATCH);
			break;
		}
		if (!attribs(a[8], at))
			return -1;
		if (!a[2] && !a[3] && !a[4] && !a[5]) {       /* the guest's glue had no surface for the window */
			r = fail(EGL_BAD_NATIVE_WINDOW);
			break;
		}
		Surf s;
		s.kind = KWindow;
		s.cfg = a[1];
		if (!surfAttribs(s, at)) {
			r = fail(EGL_BAD_ATTRIBUTE);
			break;
		}
		s.win = new SyvgHostWindow;
		memcpy(s.win->sid, a + 2, sizeof(s.win->sid));
		s.w = s.win->width = (int)a[6];
		s.h = s.win->height = (int)a[7];
		s.win->presented = 0;
		r = newSurf(std::move(s));
		break;
	}
	case EGL2_FN_WindowUpdate: {
		NEED(7);
		auto it = p.surfs.find(a[0]);
		if (it != p.surfs.end() && it->second.win) {
			Surf &s = it->second;
			memcpy(s.win->sid, a + 1, sizeof(s.win->sid));
			s.w = s.win->width = (int)a[5];
			s.h = s.win->height = (int)a[6];
			if (s.gl) {             /* the GL surface follows the window's size */
				uint32_t f[1] = { s.gl };
				hostCmd(SYGL_SURF_FREE, f, 1);
				s.gl = 0;
				glSurfMake(s, a[0]);
				for (auto &tt : p.threads)
					if (tt.second.draw[0] == a[0] && tt.second.ctx[0]) {
						auto c = p.ctxs.find(tt.second.ctx[0]);
						if (c != p.ctxs.end()) {
							uint32_t cw[4] = { c->second.gl, s.gl, (uint32_t)s.w, (uint32_t)s.h };
							hostCmd(SYGL_GL2_CURRENT, cw, 4);
						}
					}
			}
			r = 1;
		}
		break;
	}
	case EGL2_FN_CreatePbufferSurface: {
		NEED(3);
		DPY(a[0]);
		INIT();
		Cfg *c = cfgOf(a[1]);
		if (!c) {
			r = fail(EGL_BAD_CONFIG);
			break;
		}
		if (!(c->get(EGL_SURFACE_TYPE) & EGL_PBUFFER_BIT)) {
			r = fail(EGL_BAD_MATCH);
			break;
		}
		if (!attribs(a[2], at))
			return -1;
		Surf s;
		s.kind = KPbuffer;
		s.cfg = a[1];
		if (!surfAttribs(s, at)) {
			r = fail(EGL_BAD_ATTRIBUTE);
			break;
		}
		EGLint mw = c->get(EGL_MAX_PBUFFER_WIDTH, 2048), mh = c->get(EGL_MAX_PBUFFER_HEIGHT, 2048);
		if (s.w < 0 || s.h < 0) {
			r = fail(EGL_BAD_PARAMETER);
			break;
		}
		if (s.w > mw || s.h > mh) {
			if (!s.largest) {
				r = fail(EGL_BAD_ALLOC);
				break;
			}
			s.w = s.w > mw ? mw : s.w;
			s.h = s.h > mh ? mh : s.h;
		}
		if ((s.texFormat == EGL_NO_TEXTURE) != (s.texTarget == EGL_NO_TEXTURE)) {
			r = fail(EGL_BAD_MATCH);
			break;
		}
		r = newSurf(std::move(s));
		break;
	}
	case EGL2_FN_CreatePbufferFromClientBuffer: {
		NEED(5);
		DPY(a[0]);
		INIT();
		Cfg *c = cfgOf(a[3]);
		if (!c) {
			r = fail(EGL_BAD_CONFIG);
			break;
		}
		if (a[1] != EGL_OPENVG_IMAGE) {
			r = fail(EGL_BAD_PARAMETER);
			break;
		}
		if (!attribs(a[4], at))
			return -1;
		EGLConfig rc = riConfig(c);
		eglBindAPI(EGL_OPENVG_API);
		EGLSurface vs = rc ? eglCreatePbufferFromClientBuffer((EGLDisplay)1, EGL_OPENVG_IMAGE,
		                                                      (EGLClientBuffer)syvg_vg_host_handle(a[2]), rc,
		                                                      at.empty() ? NULL : at.data()) : EGL_NO_SURFACE;
		if (!vs) {
			EGLint e = eglGetError();
			r = fail(e != EGL_SUCCESS ? e : EGL_BAD_ACCESS);
			break;
		}
		Surf s;
		s.kind = KClientBuffer;
		s.cfg = a[3];
		s.vg = vs;
		s.vgImage = a[2];
		EGLint vw = 0, vh = 0;
		eglQuerySurface((EGLDisplay)1, vs, EGL_WIDTH, &vw);
		eglQuerySurface((EGLDisplay)1, vs, EGL_HEIGHT, &vh);
		s.w = vw;
		s.h = vh;
		r = newSurf(std::move(s));
		break;
	}
	case EGL2_FN_CreatePixmapSurface:
		r = fail(EGL_BAD_NATIVE_PIXMAP);    /* native pixmaps: not yet (H-egl-no-pixmap) */
		break;
	case EGL2_FN_DestroySurface: {
		NEED(2);
		DPY(a[0]);
		auto it = p.surfs.find(a[1]);
		if (it == p.surfs.end() || it->second.dead) {
			r = fail(EGL_BAD_SURFACE);
			break;
		}
		if (it->second.current > 0)
			it->second.dead = true;
		else
			freeSurf(a[1]);
		r = 1;
		break;
	}
	case EGL2_FN_QuerySurface: {
		NEED(4);
		DPY(a[0]);
		auto it = p.surfs.find(a[1]);
		if (it == p.surfs.end()) {
			r = fail(EGL_BAD_SURFACE);
			break;
		}
		Surf &s = it->second;
		EGLint v;
		switch ((EGLint)a[2]) {
		case EGL_WIDTH: v = s.w; break;
		case EGL_HEIGHT: v = s.h; break;
		case EGL_CONFIG_ID: v = s.cfg; break;
		case EGL_LARGEST_PBUFFER: v = s.largest; break;
		case EGL_TEXTURE_FORMAT: v = s.texFormat; break;
		case EGL_TEXTURE_TARGET: v = s.texTarget; break;
		case EGL_MIPMAP_TEXTURE: v = s.mipmap; break;
		case EGL_MIPMAP_LEVEL: v = s.mipLevel; break;
		case EGL_RENDER_BUFFER: v = s.kind == KPixmap ? EGL_SINGLE_BUFFER : EGL_BACK_BUFFER; break;
		case EGL_SWAP_BEHAVIOR: v = s.swap; break;
		case EGL_MULTISAMPLE_RESOLVE: v = s.resolve; break;
		case EGL_VG_ALPHA_FORMAT: v = s.vgAlpha; break;
		case EGL_VG_COLORSPACE: v = s.vgColor; break;
		case EGL_HORIZONTAL_RESOLUTION: case EGL_VERTICAL_RESOLUTION: case EGL_PIXEL_ASPECT_RATIO: v = EGL_UNKNOWN; break;
		default: r = fail(EGL_BAD_ATTRIBUTE); goto done;
		}
		if (!putInt(a[3], v))
			return -1;
		r = 1;
		break;
	}
	case EGL2_FN_SurfaceAttrib: {
		NEED(4);
		DPY(a[0]);
		auto it = p.surfs.find(a[1]);
		if (it == p.surfs.end()) {
			r = fail(EGL_BAD_SURFACE);
			break;
		}
		Surf &s = it->second;
		Cfg *c = cfgOf(s.cfg);
		EGLint st = c ? c->get(EGL_SURFACE_TYPE) : 0;
		switch ((EGLint)a[2]) {
		case EGL_SWAP_BEHAVIOR:
			if ((EGLint)a[3] == EGL_BUFFER_PRESERVED && !(st & EGL_SWAP_BEHAVIOR_PRESERVED_BIT)) {
				r = fail(EGL_BAD_MATCH);
				goto done;
			}
			s.swap = a[3];
			break;
		case EGL_MULTISAMPLE_RESOLVE:
			if ((EGLint)a[3] == EGL_MULTISAMPLE_RESOLVE_BOX && !(st & EGL_MULTISAMPLE_RESOLVE_BOX_BIT)) {
				r = fail(EGL_BAD_MATCH);
				goto done;
			}
			s.resolve = a[3];
			break;
		case EGL_MIPMAP_LEVEL: s.mipLevel = a[3]; break;
		default: r = fail(EGL_BAD_ATTRIBUTE); goto done;
		}
		if (s.vg)
			eglSurfaceAttrib((EGLDisplay)1, s.vg, (EGLint)a[2], (EGLint)a[3]);
		r = 1;
		break;
	}
	case EGL2_FN_BindAPI:
		NEED(1);
		if (a[0] != EGL_OPENGL_ES_API_ && a[0] != EGL_OPENVG_API_) {
			r = fail(EGL_BAD_PARAMETER);
			break;
		}
		t.api = a[0];
		r = 1;
		break;
	case EGL2_FN_QueryAPI: r = t.api; break;
	case EGL2_FN_WaitClient: case EGL2_FN_WaitGL: case EGL2_FN_WaitNative:
		if (t.ctx[1]) {
			eglBindAPI(EGL_OPENVG_API);
			eglWaitClient();
		}
		r = 1;
		break;
	case EGL2_FN_ReleaseThread:
		for (EGLenum api : { (EGLenum)EGL_OPENGL_ES_API_, (EGLenum)EGL_OPENVG_API_ }) {
			T().api = api;
			if (T().ctx[slot(api)])
				makeCurrent(0, 0, 0);
		}
		p.threads.erase(gThreadNow);    /* S10: nothing of the thread is left (t is not used after this) */
		r = 1;
		out[0] = r;
		khrHostFlush(0);
		return 0;
	case EGL2_FN_BindTexImage: case EGL2_FN_ReleaseTexImage: {
		NEED(3);
		DPY(a[0]);
		auto it = p.surfs.find(a[1]);
		if (it == p.surfs.end() || it->second.kind != KPbuffer) {
			r = fail(EGL_BAD_SURFACE);
			break;
		}
		Surf &s = it->second;
		if ((EGLint)a[2] != EGL_BACK_BUFFER) {
			r = fail(EGL_BAD_PARAMETER);
			break;
		}
		if (s.texFormat == EGL_NO_TEXTURE) {
			r = fail(EGL_BAD_MATCH);
			break;
		}
		if (w[0] == EGL2_FN_BindTexImage && t.ctx[0]) {
			auto c = p.ctxs.find(t.ctx[0]);
			glSurfMake(s, a[1]);
			if (c != p.ctxs.end()) {
				uint32_t cw[5] = { c->second.gl, s.gl, (uint32_t)s.w, (uint32_t)s.h, s.texFormat == EGL_TEXTURE_RGBA };
				hostCmd(SYGL_GL_TEXIMAGE, cw, 5);
			}
		}
		r = 1;
		break;
	}
	case EGL2_FN_SwapInterval: NEED(2); DPY(a[0]); r = t.ctx[slot(t.api)] ? 1 : fail(EGL_BAD_CONTEXT); break;
	case EGL2_FN_CreateContext: {
		NEED(4);
		DPY(a[0]);
		INIT();
		Cfg *cf = cfgOf(a[1]);
		if (!cf) {
			r = fail(EGL_BAD_CONFIG);
			break;
		}
		if (!attribs(a[3], at))
			return -1;
		Ctx c;
		c.api = t.api;
		c.cfg = a[1];
		for (size_t i = 0; i + 1 < at.size() && at[i] != EGL_NONE; i += 2) {
			if (at[i] == EGL_CONTEXT_CLIENT_VERSION && c.api == EGL_OPENGL_ES_API_) {
				c.version = at[i + 1];
			} else {
				r = fail(EGL_BAD_ATTRIBUTE);
				goto done;
			}
		}
		EGLint rt = cf->get(EGL_RENDERABLE_TYPE);
		if ((c.api == EGL_OPENVG_API_ && !(rt & EGL_OPENVG_BIT)) ||
		    (c.api == EGL_OPENGL_ES_API_ && (c.version < 1 || c.version > 2)) ||
		    (c.api == EGL_OPENGL_ES_API_ && !(rt & (c.version == 2 ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_ES_BIT)))) {
			r = fail(EGL_BAD_MATCH);
			break;
		}
		Ctx *share = NULL;
		if (a[2]) {
			auto it = p.ctxs.find(a[2]);
			if (it == p.ctxs.end() || it->second.api != c.api || it->second.version != c.version) {
				r = fail(EGL_BAD_CONTEXT);
				break;
			}
			share = &it->second;
		}
		uint32_t id = p.next++;
		if (c.api == EGL_OPENVG_API_) {
			EGLConfig rc = riConfig(cf);
			eglBindAPI(EGL_OPENVG_API);
			c.vg = rc ? eglCreateContext((EGLDisplay)1, rc, share ? share->vg : EGL_NO_CONTEXT, NULL) : 0;
			if (!c.vg) {
				r = fail(EGL_BAD_ALLOC);
				break;
			}
		} else {
			c.gl = glCtxId(id);
			uint32_t cw[3] = { c.gl, share ? share->gl : 0, (uint32_t)c.version };
			hostCmd(SYGL_GL2_NEW, cw, 3);
		}
		p.ctxs[id] = c;
		r = id;
		break;
	}
	case EGL2_FN_DestroyContext: {
		NEED(2);
		DPY(a[0]);
		auto it = p.ctxs.find(a[1]);
		if (it == p.ctxs.end() || it->second.dead) {
			r = fail(EGL_BAD_CONTEXT);
			break;
		}
		if (it->second.current > 0)
			it->second.dead = true;
		else
			freeCtx(a[1]);
		r = 1;
		break;
	}
	case EGL2_FN_MakeCurrent:
		NEED(4);
		if (a[0] != 1 && (a[1] || a[2] || a[3])) {
			r = fail(EGL_BAD_DISPLAY);
			break;
		}
		r = makeCurrent(a[1], a[2], a[3]);
		break;
	case EGL2_FN_GetCurrentContext: r = t.ctx[slot(t.api)]; break;
	case EGL2_FN_GetCurrentSurface:
		NEED(1);
		if ((EGLint)a[0] == EGL_READ)
			r = t.read[slot(t.api)];
		else if ((EGLint)a[0] == EGL_DRAW)
			r = t.draw[slot(t.api)];
		else
			r = fail(EGL_BAD_PARAMETER);
		break;
	case EGL2_FN_GetCurrentDisplay: r = t.ctx[slot(t.api)] ? 1 : 0; break;
	case EGL2_FN_QueryContext: {
		NEED(4);
		DPY(a[0]);
		auto it = p.ctxs.find(a[1]);
		if (it == p.ctxs.end()) {
			r = fail(EGL_BAD_CONTEXT);
			break;
		}
		EGLint v;
		switch ((EGLint)a[2]) {
		case EGL_CONFIG_ID: v = it->second.cfg; break;
		case EGL_CONTEXT_CLIENT_TYPE: v = it->second.api; break;
		case EGL_CONTEXT_CLIENT_VERSION: v = it->second.api == EGL_OPENGL_ES_API_ ? it->second.version : 1; break;
		case EGL_RENDER_BUFFER: {
			uint32_t d = 0;
			for (auto &tt : p.threads)
				for (int k = 0; k < 2; k++)
					if (tt.second.ctx[k] == a[1])
						d = tt.second.draw[k];
			auto s = p.surfs.find(d);
			v = s == p.surfs.end() ? EGL_NONE : s->second.kind == KPixmap ? EGL_SINGLE_BUFFER : EGL_BACK_BUFFER;
			break;
		}
		default: r = fail(EGL_BAD_ATTRIBUTE); goto done;
		}
		if (!putInt(a[3], v))
			return -1;
		r = 1;
		break;
	}
	case EGL2_FN_SwapBuffers: {
		NEED(2);
		DPY(a[0]);
		auto it = p.surfs.find(a[1]);
		if (it == p.surfs.end()) {
			r = fail(EGL_BAD_SURFACE);
			break;
		}
		Surf &s = it->second;
		if (s.kind != KWindow) {
			r = 1;                  /* no effect for pbuffers and pixmaps */
			break;
		}
		int k = t.draw[1] == a[1] && t.ctx[1] ? 1 : t.draw[0] == a[1] && t.ctx[0] ? 0 : -1;
		if (k < 0) {
			r = fail(EGL_BAD_SURFACE);
			break;
		}
		s.win->presented = 0;
		if (k == 1) {
			eglBindAPI(EGL_OPENVG_API);
			r = eglSwapBuffers((EGLDisplay)1, s.vg);
		} else {
			uint32_t pw[5] = { s.gl, s.win->sid[0], s.win->sid[1], s.win->sid[2], s.win->sid[3] };
			hostCmd(SYGL_VG_PRESENT, pw, 5);
			s.win->presented = 1;
			r = 1;
		}
		if (s.win->presented)
			r |= 2;                 /* the frame is the window surface's scan-out image: the guest submits */
		break;
	}
	case EGL2_FN_CopyBuffers:
		NEED(3);
		r = fail(EGL_BAD_NATIVE_PIXMAP);    /* native pixmaps: not yet (H-egl-no-pixmap) */
		break;
	case EGL2_FN_GetProcAddress: {
		NEED(1);
		char name[128];
		size_t i = 0;
		for (; i < sizeof(name) - 1; i++)
			if (syvg_guest_read(a[0] + i, &name[i], 1) || !name[i])
				break;
		name[i] = 0;
		/* the EGL extension functions the host implements: the guest's own stubs (3) */
		static const char *const kEglExt[] = { "eglQueryProfilingDataNOK", "eglCreateSyncKHR", "eglDestroySyncKHR",
		                                       "eglClientWaitSyncKHR", "eglSignalSyncKHR", "eglGetSyncAttribKHR",
		                                       "eglCreateEndpointNOK", "eglDestroyEndpointNOK", "eglGetEndpointAttribNOK",
		                                       "eglSetEndpointAttribNOK", "eglEndpointBeginStreamingNOK",
		                                       "eglEndpointEndStreamingNOK", "eglAcquireImageNOK", "eglReleaseImageNOK",
		                                       "eglGetEndpointDirtyAreaNOK", "eglEndpointRequestNotificationNOK",
		                                       "eglEndpointCancelNotificationNOK", "eglDestroyImageKHR",
		                                       "vgCreateEGLImageTargetKHR" };
		bool egl = false;
		for (const char *e : kEglExt)
			egl = egl || !strcmp(e, name);
		if (egl) {
			r = 3;
			break;
		}
		/* which API's stub: the current OpenGL ES context's when it has the function, else the one that has it */
		int has = syborg_gl_has_proc(name), cur = 0;
		auto c = p.ctxs.find(t.ctx[0]);
		if (t.ctx[0] && c != p.ctxs.end())
			cur = c->second.version;
		r = (has & 2) && cur == 2 ? 2 : (has & 1) ? 1 : (has & 2) ? 2 : 0;
		break;
	}
	case EGL2_FN_QueryProfilingDataNOK: {   /* dpy, query_bits, data*, data_size, data_count* */
		NEED(5);
		DPY(a[0]);
		INIT();
		if (!a[4]) {
			r = fail(EGL_BAD_PARAMETER);
			break;
		}
		/* the layout the guest's memory monitor (GOOM) parses: TOTAL, USED (global bit); per process
		   PROCESS_ID (64 bits as two words, low first), PRIVATE and SHARED bytes (memory-usage bit) */
		std::vector<EGLint> v;
		uint32_t owners[256];
		uint64_t bytes[256], used = 0;
		int np = syborg_gl_mem_usage(owners, bytes, 256, &used);
		if (a[1] & 0x0001) {
			v.push_back(0x3070); v.push_back((EGLint)syborg_gl_mem_total());
			v.push_back(0x3071); v.push_back((EGLint)used);
		}
		if (a[1] & 0x0002)
			for (int k = 0; k < np && k < 256; k++) {
				v.push_back(0x3072); v.push_back((EGLint)owners[k]); v.push_back(0);
				v.push_back(0x3073); v.push_back((EGLint)bytes[k]);
				v.push_back(0x3074); v.push_back(0);
			}
		EGLint n = (EGLint)v.size();
		if (a[2]) {
			n = (EGLint)a[3] < n ? (EGLint)a[3] : n;
			if (n < 0)
				n = 0;
			for (EGLint k = 0; k < n; k++)
				if (!putInt(a[2] + k * 4, v[k]))
					return -1;
		}
		if (!putInt(a[4], n))
			return -1;
		r = 1;
		break;
	}
	case EGL2_FN_CreateSyncKHR: {           /* dpy, type, attribs* */
		NEED(3);
		DPY(a[0]);
		INIT();
		if (!attribs(a[2], at))
			return -1;
		Sync y;
		y.type = a[1];
		if (y.type == 0x30FA) {             /* EGL_SYNC_REUSABLE_KHR: no attributes */
			if (!at.empty()) {
				r = fail(EGL_BAD_ATTRIBUTE);
				break;
			}
		} else if (y.type == 0x3078) {      /* EGL_SYNC_RESOURCE_PROFILING_NOK */
			bool haveCond = false, haveThr = false;
			for (size_t k = 0; k + 1 < at.size(); k += 2) {
				if (at[k] == 0x30F8 && (at[k + 1] == 0x3075 || at[k + 1] == 0x3076)) {   /* EGL_SYNC_CONDITION_KHR */
					y.condition = at[k + 1]; haveCond = true;
				} else if (at[k] == 0x3077) {                                           /* threshold */
					y.threshold = (uint32_t)at[k + 1]; haveThr = true;
				} else {
					haveCond = false;
					break;
				}
			}
			if (!haveCond || !haveThr) {
				r = fail(EGL_BAD_ATTRIBUTE);
				break;
			}
		} else {
			r = fail(EGL_BAD_ATTRIBUTE);
			break;
		}
		uint32_t id = p.next++;
		p.syncs[id] = y;
		r = id;
		break;
	}
	case EGL2_FN_DestroySyncKHR: {          /* dpy, sync */
		NEED(2);
		DPY(a[0]);
		INIT();
		if (!p.syncs.erase(a[1]))
			r = fail(EGL_BAD_PARAMETER);
		else
			r = 1;
		break;
	}
	case EGL2_FN_ClientWaitSyncKHR:         /* dpy, sync, flags */
	case EGL2_FN_GetSyncAttribKHR: {        /* dpy, sync, attribute, value* */
		NEED(w[0] == EGL2_FN_ClientWaitSyncKHR ? 3 : 4);
		DPY(a[0]);
		INIT();
		auto it = p.syncs.find(a[1]);
		if (it == p.syncs.end()) {
			r = w[0] == EGL2_FN_ClientWaitSyncKHR ? (uint32_t)fail(EGL_BAD_PARAMETER) : fail(EGL_BAD_PARAMETER);
			break;
		}
		Sync &y = it->second;
		if (y.type == 0x3078) {             /* a memory-usage condition: true now? */
			uint64_t used = 0;
			syborg_gl_mem_usage(NULL, NULL, 0, &used);
			bool met = y.condition == 0x3075 ? used > y.threshold : used < y.threshold;
			if (met)
				y.status = 0x30F2;          /* EGL_SIGNALED_KHR (stays signaled) */
		}
		if (w[0] == EGL2_FN_ClientWaitSyncKHR) {
			/* never blocks the vCPU: unsignaled answers EGL_TIMEOUT_EXPIRED_KHR and the guest stub sleeps and
			   asks again until its timeout ran out */
			r = y.status == 0x30F2 ? 0x30F6 : 0x30F5;
			break;
		}
		EGLint val;
		switch ((EGLint)a[2]) {
		case 0x30F7: val = (EGLint)y.type; break;                       /* EGL_SYNC_TYPE_KHR */
		case 0x30F1: val = y.status; break;                              /* EGL_SYNC_STATUS_KHR */
		case 0x30F8: if (y.type != 0x3078) { r = fail(EGL_BAD_ATTRIBUTE); goto done; } val = y.condition; break;
		case 0x3077: if (y.type != 0x3078) { r = fail(EGL_BAD_ATTRIBUTE); goto done; } val = (EGLint)y.threshold; break;
		default: r = fail(EGL_BAD_ATTRIBUTE); goto done;
		}
		if (!a[3]) {
			r = fail(EGL_BAD_PARAMETER);
			break;
		}
		if (!putInt(a[3], val))
			return -1;
		r = 1;
		break;
	}
	case EGL2_FN_CreateEndpointNOK: {   /* dpy, type, source_type, source*, attribs*, w, h, stride, fmt, nbuf, off0, off1, base */
		NEED(13);
		DPY(a[0]);
		INIT();
		if (a[1] != 0x30D5 || a[2] != 0x30D3) {     /* EGL_ENDPOINT_TYPE_CONSUMER_NOK, EGL_TSURFACEID_NOK */
			r = fail(EGL_BAD_PARAMETER);
			break;
		}
		Endpoint e;
		if (!a[3] || syvg_guest_read(a[3], e.sid, 16) || !(e.sid[0] | e.sid[1] | e.sid[2] | e.sid[3])) {
			r = fail(EGL_BAD_PARAMETER);
			break;
		}
		if (!attribs(a[4], at))
			return -1;
		for (size_t k = 0; k + 1 < at.size(); k += 2) {
			if (at[k] == 0x30D7)                    /* EGL_DELAY_NOK */
				e.delay = at[k + 1];
			else {
				r = fail(EGL_BAD_ATTRIBUTE);
				goto done;
			}
		}
		e.w = (int)a[5]; e.h = (int)a[6]; e.stride = (int)a[7]; e.format = a[8];
		e.nbuf = a[9] ? (int)a[9] : 1; e.off[0] = a[10]; e.off[1] = a[11]; e.base = a[12];
		if (e.w <= 0 || e.h <= 0 || e.w > 4096 || e.h > 4096) {
			r = fail(EGL_BAD_PARAMETER);        /* the guest could not open the surface */
			break;
		}
		{
			const SurfLast *last = surf_last(e.sid, false);
			if (last) {
				e.host = last->host;
				e.buffer = last->buffer;
				e.updates = 1;
				e.dirty.push_back({ 0, 0, e.w, e.h });
			}
		}
		uint32_t id = p.next++;
		p.endpoints[id] = e;
		r = id;
		break;
	}
	case EGL2_FN_DestroyEndpointNOK:
	case EGL2_FN_GetEndpointAttribNOK:
	case EGL2_FN_SetEndpointAttribNOK:
	case EGL2_FN_EndpointBeginStreamingNOK:
	case EGL2_FN_EndpointEndStreamingNOK:
	case EGL2_FN_AcquireImageNOK:
	case EGL2_FN_ReleaseImageNOK:
	case EGL2_FN_GetEndpointDirtyAreaNOK:
	case EGL2_FN_EndpointRequestNotificationNOK:
	case EGL2_FN_EndpointCancelNotificationNOK: {
		NEED(2);
		DPY(a[0]);
		INIT();
		auto it = p.endpoints.find(a[1]);
		if (it == p.endpoints.end()) {
			r = w[0] == EGL2_FN_GetEndpointAttribNOK ? (fail(0x30D4), 0u) : fail(0x30D4);   /* EGL_BAD_ENDPOINT_NOK */
			break;
		}
		Endpoint &e = it->second;
		switch (w[0]) {
		case EGL2_FN_DestroyEndpointNOK:
			if (e.acquired) {                   /* its image goes with it */
				auto im = p.images.find(e.acquired);
				if (im != p.images.end()) {
					uint32_t k = im->second.key;
					hostCmd(SYGL_TEX_FREE, &k, 1);
					p.images.erase(im);
				}
			}
			p.endpoints.erase(it);
			r = 1;
			break;
		case EGL2_FN_GetEndpointAttribNOK:
			NEED(3);
			if (a[2] == 0x30D6)                 /* EGL_ENDPOINT_READY_NOK: a frame not acquired yet */
				r = e.updates > 0 ? 1 : 0;
			else if (a[2] == 0x30D7)
				r = (uint32_t)e.delay;
			else
				r = fail(EGL_BAD_ATTRIBUTE);
			break;
		case EGL2_FN_SetEndpointAttribNOK:
			NEED(4);
			if (a[2] == 0x30D7) {
				e.delay = (EGLint)a[3];
				r = 1;
			} else
				r = fail(EGL_BAD_ATTRIBUTE);
			break;
		case EGL2_FN_EndpointBeginStreamingNOK:
		case EGL2_FN_EndpointEndStreamingNOK:
			e.streaming = w[0] == EGL2_FN_EndpointBeginStreamingNOK;
			r = 1;
			break;
		case EGL2_FN_AcquireImageNOK: {
			if (e.acquired) {                   /* one image at a time: released first */
				r = fail(EGL_BAD_ACCESS);
				break;
			}
			uint32_t key = gImageKey;
			gImageKey += 2;
			if (gImageKey < 0xE0000000u)
				gImageKey = 0xE0000000u;
			bool ok = false;
			if (e.host) {                       /* the producer presented on the host: a GPU copy */
				uint32_t c[5] = { key, e.sid[0], e.sid[1], e.sid[2], e.sid[3] };
				unsigned int *q = khrHostEmit(SYGL_TEX_FROM_SCAN, 5);
				if (q) {
					memcpy(q, c, sizeof(c));
					ok = khrHostFlush(0) == 0;
				}
			}
			if (!ok && e.base) {                /* drawn in guest memory: the current buffer, read in this
			                                       process (the guest mapped it), rows bottom up as VG's */
				uint32_t buf = e.base + e.off[e.nbuf > 1 ? (e.buffer & 1) : 0];
				int bpp = e.format == 0x1027549Eu ? 2 : 4;
				std::vector<uint8_t> row((size_t)e.w * bpp);
				int band = std::max(1, std::min(e.h, (128 * 1024 - 16) / e.w));     /* a host batch is 256K words */
				ok = true;
				for (int y0 = 0; y0 < e.h && ok; y0 += band) {
					int rows = std::min(band, e.h - y0);
					unsigned int *q = khrHostEmit(SYGL_TEX, 6 + e.w * rows);
					if (!q) {
						ok = false;
						break;
					}
					q[0] = key; q[1] = 1; q[2] = e.w; q[3] = e.h; q[4] = y0; q[5] = rows;
					for (int y = 0; y < rows && ok; y++) {
						uint32_t src = buf + (uint32_t)(e.h - 1 - (y0 + y)) * e.stride;
						if (syvg_guest_read(src, row.data(), row.size())) {
							ok = false;
							break;
						}
						unsigned int *o = q + 6 + y * e.w;
						for (int x = 0; x < e.w; x++) {
							unsigned int c, al, rr, gg, bb;
							if (bpp == 2) {
								c = row[x * 2] | row[x * 2 + 1] << 8;
								rr = (c >> 11) & 31; gg = (c >> 5) & 63; bb = c & 31;
								o[x] = 0xff000000u | (bb << 3 | bb >> 2) << 16 | (gg << 2 | gg >> 4) << 8 | (rr << 3 | rr >> 2);
								continue;
							}
							memcpy(&c, &row[x * 4], 4);
							al = c >> 24; rr = (c >> 16) & 255; gg = (c >> 8) & 255; bb = c & 255;
							if (e.format == 0x10275496u)
								al = 255;
							else if (e.format == 0x10275498u && al != 255) {
								rr = (rr * al + 127) / 255; gg = (gg * al + 127) / 255; bb = (bb * al + 127) / 255;
							}
							o[x] = al << 24 | bb << 16 | gg << 8 | rr;
						}
					}
					if (ok)
						ok = khrHostFlush(0) == 0;
				}
			}
			if (!ok) {
				r = fail(EGL_BAD_ACCESS);       /* no frame yet (nothing presented, not mapped) */
				break;
			}
			uint32_t id = p.next++;
			p.images[id] = EglImage{ key, e.w, e.h };
			e.acquired = id;
			e.updates = 0;
			e.dirty.clear();
			r = id;
			break;
		}
		case EGL2_FN_ReleaseImageNOK:
			NEED(4);
			if (!a[2] || a[2] != e.acquired) {
				r = fail(EGL_BAD_PARAMETER);
				break;
			}
			e.acquired = 0;                     /* the image stays until eglDestroyImageKHR */
			r = 1;
			break;
		case EGL2_FN_GetEndpointDirtyAreaNOK: {     /* rects*, start, max, collapse */
			NEED(6);
			int n = (int)e.dirty.size(), start = (int)a[3], max = (int)a[4];
			if (start < 0 || max < 0) {
				r = fail(EGL_BAD_PARAMETER);
				break;
			}
			std::vector<std::array<int, 4> > v = e.dirty;
			if (n == 0)                         /* no rectangles known: the whole surface */
				v.push_back({ 0, 0, e.w, e.h });
			if (a[5] && v.size() > 1) {         /* collapse: the bounding rectangle */
				int x0 = v[0][0], y0 = v[0][1], x1 = v[0][0] + v[0][2], y1 = v[0][1] + v[0][3];
				for (auto &q : v) {
					x0 = std::min(x0, q[0]); y0 = std::min(y0, q[1]);
					x1 = std::max(x1, q[0] + q[2]); y1 = std::max(y1, q[1] + q[3]);
				}
				v.assign(1, { x0, y0, x1 - x0, y1 - y0 });
			}
			if (!a[2]) {                        /* no array: the number of rectangles */
				r = (uint32_t)v.size();
				break;
			}
			int k = 0;
			for (int i2 = start; i2 < (int)v.size() && k < max; i2++, k++)
				for (int j = 0; j < 4; j++)     /* x0, y0, x1, y1 */
					if (!putInt(a[2] + (k * 4 + j) * 4, j < 2 ? v[i2][j] : v[i2][j - 2] + v[i2][j]))
						return -1;
			r = (uint32_t)k;
			break;
		}
		case EGL2_FN_EndpointRequestNotificationNOK:
			e.armed = true;
			e.due = e.updates > 0;               /* a frame already waiting: completes at once */
			r = 1;
			break;
		case EGL2_FN_EndpointCancelNotificationNOK:
			e.armed = e.due = false;
			r = 1;
			break;
		}
		break;
	}
	case EGL2_FN_EndpointPollNOK: {         /* -> n; out[1..] = endpoints whose notification is due (disarmed) */
		uint32_t n = 0;
		for (auto &ep : p.endpoints)
			if (ep.second.armed && ep.second.due && n + 1 < outWords) {
				out[1 + n++] = ep.first;
				ep.second.armed = ep.second.due = false;
			}
		r = n;
		break;
	}
	case EGL2_FN_DestroyImageKHR: {         /* dpy, image */
		NEED(2);
		DPY(a[0]);
		INIT();
		auto im = p.images.find(a[1]);
		if (im == p.images.end()) {
			r = fail(EGL_BAD_PARAMETER);
			break;
		}
		for (auto &ep : p.endpoints)
			if (ep.second.acquired == a[1])
				ep.second.acquired = 0;
		uint32_t k = im->second.key;
		hostCmd(SYGL_TEX_FREE, &k, 1);
		p.images.erase(im);
		r = 1;
		break;
	}
	case EGL2_FN_CreateEGLImageTargetKHR: { /* image -> VGImage (the caller's current OpenVG context) */
		NEED(1);
		auto im = p.images.find(a[0]);
		if (im == p.images.end()) {
			r = 0;                              /* VG_ILLEGAL_ARGUMENT_ERROR is the VG context's to set */
			break;
		}
		r = syvg_vg_image_from_texture(owner, im->second.w, im->second.h, im->second.key);
		break;
	}
	case EGL2_FN_SignalSyncKHR: {           /* dpy, sync, mode */
		NEED(3);
		DPY(a[0]);
		INIT();
		auto it = p.syncs.find(a[1]);
		if (it == p.syncs.end()) {
			r = fail(EGL_BAD_PARAMETER);
			break;
		}
		if (it->second.type != 0x30FA) {   /* only reusable syncs are signalled by the application */
			r = fail(EGL_BAD_MATCH);
			break;
		}
		if (a[2] != 0x30F2 && a[2] != 0x30F3) {
			r = fail(EGL_BAD_PARAMETER);
			break;
		}
		it->second.status = (EGLint)a[2];
		r = 1;
		break;
	}
	default:
		return -1;
	}
done:
#undef NEED
#undef DPY
#undef INIT
	if (EglTrace())                     /* SYBORG_EGL_TRACE=1: every call, its arguments, result and error */
		fprintf(stderr, "syvg: EGL owner %u thread %u %s(%x %x %x %x %x) = %x err %x\n", owner, w[1],
		        w[0] < sizeof(KEglNames) / sizeof(KEglNames[0]) ? KEglNames[w[0]] : "?", na > 0 ? a[0] : 0,
		        na > 1 ? a[1] : 0, na > 2 ? a[2] : 0, na > 3 ? a[3] : 0, na > 4 ? a[4] : 0, r, T().err);
	out[0] = r;
	khrHostFlush(0);
	return 0;
}

extern "C" void syvg_egl_owner_free(uint32_t owner)
{
	auto it = gProc.find(owner);
	if (it == gProc.end())
		return;
	for (auto &s : it->second.surfs)
		delete s.second.win;
	gProc.erase(it);            /* the MiniEGL and syborg-gl objects go with the process's instance */
}

std::vector<uint32_t> syvgEglOwners()
{
	std::vector<uint32_t> v;
	for (auto &p : gProc)
		v.push_back(p.first);
	return v;
}

void syvgEglReset()
{
	for (auto &p : gProc)
		for (auto &s : p.second.surfs)
			delete s.second.win;
	gProc.clear();
	gOwnerNow = gThreadNow = 0;
}
