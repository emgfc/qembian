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
struct Proc {
	std::unordered_map<uint32_t, Surf> surfs;
	std::unordered_map<uint32_t, Ctx> ctxs;
	std::unordered_map<uint32_t, Thread> threads;
	uint32_t next = 1;
	int initialized = 0;
};
static std::unordered_map<uint32_t, Proc> gProc;
static uint32_t gOwnerNow, gThreadNow;

static Proc &P() { return gProc[gOwnerNow]; }
static Thread &T() { return P().threads[gThreadNow]; }
static int slot(EGLenum api) { return api == EGL_OPENVG_API_ ? 1 : 0; }

static uint32_t fail(EGLint e) { T().err = e; return 0; }

/* the syborg-gl ids of GL objects: surfaces share the device's table with OpenVG's (even keys from 0x10) */
static uint32_t glSurfId(uint32_t id) { return 0x80000000u | id; }
static uint32_t glCtxId(uint32_t id) { return 0x40000000u | id; }

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
	Thread &t = T();
	if (w[0] != EGL2_FN_GetError)
		t.err = EGL_SUCCESS;
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
		T().api = EGL_OPENGL_ES_API_;
		r = 1;
		break;
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
		/* which API's stub: the current OpenGL ES context's when it has the function, else the one that has it */
		int has = syborg_gl_has_proc(name), cur = 0;
		auto c = p.ctxs.find(t.ctx[0]);
		if (t.ctx[0] && c != p.ctxs.end())
			cur = c->second.version;
		r = (has & 2) && cur == 2 ? 2 : (has & 1) ? 1 : (has & 2) ? 2 : 0;
		break;
	}
	default:
		return -1;
	}
done:
#undef NEED
#undef DPY
#undef INIT
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
