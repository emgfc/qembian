/*
 * sygl: our OpenGL ES 1.1 + EGL behind the N95's libgles_cm.dll entry points
 * (docs/gpu-roadmap.md; hacks H-gles-trampolines, H-gles-blob, H-gles-fbdirect, H-vsync-wait).
 *
 * The blob is linked at a fixed ROM address (SYGL_BASE, src/n95gl/build.sh) and runs in the calling
 * thread: no loader, no writable static data. State: SyThread per thread (plat.cpp, DllTls), which
 * points at the current context and surfaces. Every EGL entry point here is sy_<eglName>
 * (gen/entry.S branches to it; undefined ones stay logged stubs).
 *
 * Milestone 1: one display, a few configs, window surfaces (32 bpp colour + 16-bit depth in the
 * app's heap), contexts, MakeCurrent, SwapBuffers = copy to the window's place in the frame buffer
 * (H-gles-fbdirect: no clipping against other windows yet; DSA later) + wait for vsync.
 */
#include "sygl_int.h"

typedef int EGLint;
typedef unsigned int EGLBoolean;
typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLSurface;
typedef void* EGLContext;
typedef void* NativeWindowType;

#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_SUCCESS 0x3000
#define EGL_NOT_INITIALIZED 0x3001
#define EGL_BAD_ACCESS 0x3002
#define EGL_BAD_ALLOC 0x3003
#define EGL_BAD_ATTRIBUTE 0x3004
#define EGL_BAD_CONFIG 0x3005
#define EGL_BAD_CONTEXT 0x3006
#define EGL_BAD_CURRENT_SURFACE 0x3007
#define EGL_BAD_DISPLAY 0x3008
#define EGL_BAD_MATCH 0x3009
#define EGL_BAD_NATIVE_PIXMAP 0x300A
#define EGL_BAD_NATIVE_WINDOW 0x300B
#define EGL_BAD_PARAMETER 0x300C
#define EGL_BAD_SURFACE 0x300D

#define EGL_BUFFER_SIZE 0x3020
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_CONFIG_CAVEAT 0x3027
#define EGL_CONFIG_ID 0x3028
#define EGL_LEVEL 0x3029
#define EGL_MAX_PBUFFER_HEIGHT 0x302A
#define EGL_MAX_PBUFFER_PIXELS 0x302B
#define EGL_MAX_PBUFFER_WIDTH 0x302C
#define EGL_NATIVE_RENDERABLE 0x302D
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_NATIVE_VISUAL_TYPE 0x302F
#define EGL_SAMPLES 0x3031
#define EGL_SAMPLE_BUFFERS 0x3032
#define EGL_SURFACE_TYPE 0x3033
#define EGL_TRANSPARENT_TYPE 0x3034
#define EGL_TRANSPARENT_BLUE_VALUE 0x3035
#define EGL_TRANSPARENT_GREEN_VALUE 0x3036
#define EGL_TRANSPARENT_RED_VALUE 0x3037
#define EGL_NONE 0x3038
#define EGL_BIND_TO_TEXTURE_RGB 0x3039
#define EGL_BIND_TO_TEXTURE_RGBA 0x303A
#define EGL_MIN_SWAP_INTERVAL 0x303B
#define EGL_MAX_SWAP_INTERVAL 0x303C
#define EGL_DONT_CARE (-1)
#define EGL_SLOW_CONFIG 0x3050
#define EGL_NON_CONFORMANT_CONFIG 0x3051
#define EGL_TRANSPARENT_RGB 0x3052
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_EXTENSIONS 0x3055
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_LARGEST_PBUFFER 0x3058
#define EGL_DRAW 0x3059
#define EGL_READ 0x305A
#define EGL_CORE_NATIVE_ENGINE 0x305B
#define EGL_TEXTURE_FORMAT 0x3080
#define EGL_TEXTURE_TARGET 0x3081
#define EGL_MIPMAP_TEXTURE 0x3082
#define EGL_NO_TEXTURE 0x305C
#define EGL_TEXTURE_RGB 0x305D
#define EGL_TEXTURE_RGBA 0x305E
#define EGL_BACK_BUFFER 0x3084
#define EGL_PBUFFER_BIT 0x01
#define EGL_PIXMAP_BIT 0x02
#define EGL_WINDOW_BIT 0x04

#define SY_DISPLAY ((EGLDisplay)1)

/* configs (read-only table): id, buffer, r, g, b, a, depth, surface types */
typedef struct { int id, buffer, r, g, b, a, depth, types; } SyConfig;
static const SyConfig KConfigs[] =
	{
	{ 1, 16, 5, 6, 5, 0, 16, EGL_WINDOW_BIT | EGL_PIXMAP_BIT | EGL_PBUFFER_BIT },
	{ 2, 16, 5, 6, 5, 0, 0, EGL_WINDOW_BIT | EGL_PIXMAP_BIT | EGL_PBUFFER_BIT },
	{ 3, 32, 8, 8, 8, 8, 16, EGL_WINDOW_BIT | EGL_PIXMAP_BIT | EGL_PBUFFER_BIT },
	{ 4, 32, 8, 8, 8, 8, 0, EGL_WINDOW_BIT | EGL_PIXMAP_BIT | EGL_PBUFFER_BIT },
	};
#define NCONFIGS ((int)(sizeof(KConfigs) / sizeof(KConfigs[0])))

static const SyConfig* config_of(EGLConfig c)
	{
	int id = (int)c;
	return id >= 1 && id <= NCONFIGS ? &KConfigs[id - 1] : 0;
	}

static EGLBoolean fail(SyThread* t, int e)
	{
	if (t) t->eglError = e;
	return EGL_FALSE;
	}

static SyThread* ok_display(EGLDisplay d, int needInit)
	{
	SyThread* t = sy_thread();
	if (!t) return 0;
	/* one display: EGL_NO_DISPLAY (0) is accepted as ours too. Hitchcock calls eglChooseConfig on a
	   renderer object whose display field is still 0 (filled lazily later); the N95's EGL takes it. */
	if (d != SY_DISPLAY && d != 0) { t->eglError = EGL_BAD_DISPLAY; return 0; }
	if (needInit && !t->initialized)
		{
		if (t->notInitLogged < 10)
			{
			++t->notInitLogged;
			SYLOG("EGL call from thread state %08x without eglInitialize in this thread (caller %08x)",
				  (u32)t, (u32)__builtin_return_address(1));
			}
		t->eglError = EGL_NOT_INITIALIZED;
		return 0;
		}
	t->eglError = EGL_SUCCESS;
	return t;
	}

static SySurface* surface_of(EGLSurface s)
	{
	SySurface* p = (SySurface*)s;
	return p && p->magic == SY_MAGIC_SURFACE ? p : 0;
	}

static struct SyContext* context_of(EGLContext c)
	{
	struct SyContext* p = (struct SyContext*)c;
	return p && *(u32*)p == SY_MAGIC_CONTEXT ? p : 0;
	}

/* ---------------------------------------------------------------- display */

EGLint sy_eglGetError(void)
	{
	SyThread* t = sy_thread_peek();
	if (!t) return EGL_SUCCESS;
	int e = t->eglError;
	t->eglError = EGL_SUCCESS;
	return e;
	}

EGLDisplay sy_eglGetDisplay(void* aNative)
	{
	SYTRACE(sy_thread(), "eglGetDisplay(%d)", (u32)__builtin_return_address(0), (int)aNative);
	(void)aNative;
	return SY_DISPLAY;
	}

EGLBoolean sy_eglInitialize(EGLDisplay d, EGLint* aMajor, EGLint* aMinor)
	{
	SYTRACE(sy_thread(), "eglInitialize(%d)", (u32)__builtin_return_address(0), (int)d);
	SyThread* t = ok_display(d, 0);
	if (!t) return EGL_FALSE;
	if (!t->initialized)
		SYLOG("eglInitialize: sygl software renderer, EGL 1.1 / GLES-CM 1.1 [" __DATE__ " " __TIME__ "]");
	t->initialized = 1;
	if (aMajor) *aMajor = 1;
	if (aMinor) *aMinor = 1;
	return EGL_TRUE;
	}

EGLBoolean sy_eglTerminate(EGLDisplay d)
	{
	SYTRACE(sy_thread(), "eglTerminate(%d)", (u32)__builtin_return_address(0), (int)d);
	SyThread* t = ok_display(d, 0);
	if (!t) return EGL_FALSE;
	sy_hw_terminate(t);
	t->initialized = 0;
	return EGL_TRUE;
	}

const char* sy_eglQueryString(EGLDisplay d, EGLint name)
	{
	SyThread* t = ok_display(d, 1);
	if (!t) return 0;
	switch (name)
		{
	case EGL_VENDOR: return SYGL_EGL_VENDOR;		/* the device's own driver strings: [gl.identity] (identity.h) */
	case EGL_VERSION: return SYGL_EGL_VERSION;
	case EGL_EXTENSIONS: return "";
	default: t->eglError = EGL_BAD_PARAMETER; return 0;
		}
	}

void* sy_eglGetProcAddress(const char* aName)
	{
	(void)aName;
	return 0;
	}

/* ---------------------------------------------------------------- configs */

static int config_attr(const SyConfig* c, EGLint a, EGLint* v)
	{
	switch (a)
		{
	case EGL_BUFFER_SIZE: *v = c->buffer; break;
	case EGL_RED_SIZE: *v = c->r; break;
	case EGL_GREEN_SIZE: *v = c->g; break;
	case EGL_BLUE_SIZE: *v = c->b; break;
	case EGL_ALPHA_SIZE: *v = c->a; break;
	case EGL_DEPTH_SIZE: *v = c->depth; break;
	case EGL_STENCIL_SIZE: *v = 0; break;
	case EGL_CONFIG_CAVEAT: *v = EGL_NONE; break;
	case EGL_CONFIG_ID: *v = c->id; break;
	case EGL_LEVEL: *v = 0; break;
	case EGL_MAX_PBUFFER_WIDTH: case EGL_MAX_PBUFFER_HEIGHT: *v = 1024; break;
	case EGL_MAX_PBUFFER_PIXELS: *v = 1024 * 1024; break;
	case EGL_NATIVE_RENDERABLE: *v = EGL_FALSE; break;
	case EGL_NATIVE_VISUAL_ID: *v = c->buffer == 16 ? 7 : 11; break;		/* TDisplayMode EColor64K / EColor16MU */
	case EGL_NATIVE_VISUAL_TYPE: *v = EGL_NONE; break;
	case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS: *v = 0; break;
	case EGL_SURFACE_TYPE: *v = c->types; break;
	case EGL_TRANSPARENT_TYPE: *v = EGL_NONE; break;
	case EGL_TRANSPARENT_RED_VALUE: case EGL_TRANSPARENT_GREEN_VALUE: case EGL_TRANSPARENT_BLUE_VALUE: *v = 0; break;
	case EGL_BIND_TO_TEXTURE_RGB: case EGL_BIND_TO_TEXTURE_RGBA: *v = EGL_FALSE; break;
	case EGL_MIN_SWAP_INTERVAL: *v = 0; break;
	case EGL_MAX_SWAP_INTERVAL: *v = 4; break;
	default: return 0;
		}
	return 1;
	}

EGLBoolean sy_eglGetConfigs(EGLDisplay d, EGLConfig* aConfigs, EGLint aSize, EGLint* aNum)
	{
	SyThread* t = ok_display(d, 1);
	if (!t) return EGL_FALSE;
	if (!aNum) return fail(t, EGL_BAD_PARAMETER);
	int n = 0;
	for (int i = 0; i < NCONFIGS && (!aConfigs || n < aSize); ++i)
		{
		if (aConfigs) aConfigs[n] = (EGLConfig)KConfigs[i].id;
		++n;
		}
	*aNum = n;
	return EGL_TRUE;
	}

/* attribute matching (EGL 1.1 §3.4.1): sizes are minimums, SURFACE_TYPE a mask, CONFIG_ID exact */
static int config_matches(const SyConfig* c, const EGLint* aAttr)
	{
	for (const EGLint* p = aAttr; p && p[0] != EGL_NONE; p += 2)
		{
		EGLint a = p[0], want = p[1], have = 0;
		if (want == EGL_DONT_CARE)
			continue;
		switch (a)
			{
		case EGL_BUFFER_SIZE: case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE:
		case EGL_ALPHA_SIZE: case EGL_DEPTH_SIZE: case EGL_STENCIL_SIZE: case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS:
			config_attr(c, a, &have);
			if (have < want) return 0;
			break;
		case EGL_SURFACE_TYPE:
			if ((c->types & want) != want) return 0;
			break;
		case EGL_CONFIG_ID:
			if (c->id != want) return 0;
			break;
		case EGL_LEVEL:
			if (want != 0) return 0;
			break;
		default:
			break;	/* caveat, native, transparency, swap intervals: all configs qualify */
			}
		}
	return 1;
	}

static void log_attribs(const char* aWhat, const EGLint* aAttr)
	{
	char buf[200];
	int n = 0;
	for (const EGLint* p = aAttr; p && p[0] != EGL_NONE && n < 180; p += 2)
		{
		static const char hex[] = "0123456789abcdef";
		u32 a = (u32)p[0], v = (u32)p[1];
		buf[n++] = ' ';
		for (int k = 12; k >= 0; k -= 4) buf[n++] = hex[(a >> k) & 15];
		buf[n++] = '=';
		if ((int)v < 0) { buf[n++] = '-'; v = (u32)(-(int)v); }
		char tmp[12]; int m = 0;
		do { tmp[m++] = (char)('0' + v % 10); v /= 10; } while (v && m < 11);
		while (m) buf[n++] = tmp[--m];
		}
	buf[n] = 0;
	SYLOG("%s:%s", aWhat, buf);
	}

EGLBoolean sy_eglChooseConfig(EGLDisplay d, const EGLint* aAttr, EGLConfig* aConfigs, EGLint aSize, EGLint* aNum)
	{
	SYTRACE(sy_thread(), "eglChooseConfig(%d)", (u32)__builtin_return_address(0), (int)d);
	SyThread* t = ok_display(d, 1);
	if (!t)
		{
		SyThread* p = sy_thread_peek();
		SYLOG("eglChooseConfig failed: error 0x%x (display %d)", p ? p->eglError : -1, (int)d);
		log_attribs("  attribs", aAttr);
		return EGL_FALSE;
		}
	if (!aNum) return fail(t, EGL_BAD_PARAMETER);
	int n = 0;
	for (int i = 0; i < NCONFIGS; ++i)		/* table order = EGL sort order (smaller buffers first) */
		if (config_matches(&KConfigs[i], aAttr))
			{
			if (aConfigs && n < aSize) aConfigs[n] = (EGLConfig)KConfigs[i].id;
			++n;
			}
	*aNum = aConfigs && n > aSize ? aSize : n;
	if (n == 0)
		log_attribs("eglChooseConfig: no config matches", aAttr);
	return EGL_TRUE;
	}

EGLBoolean sy_eglGetConfigAttrib(EGLDisplay d, EGLConfig aConfig, EGLint aAttr, EGLint* aValue)
	{
	SYTRACE(sy_thread(), "eglGetConfigAttrib(cfg %d, 0x%x)", (u32)__builtin_return_address(0), (int)aConfig, aAttr);
	SyThread* t = ok_display(d, 1);
	if (!t) return EGL_FALSE;
	const SyConfig* c = config_of(aConfig);
	if (!c) return fail(t, EGL_BAD_CONFIG);
	if (!aValue || !config_attr(c, aAttr, aValue)) return fail(t, EGL_BAD_ATTRIBUTE);
	return EGL_TRUE;
	}

/* ---------------------------------------------------------------- surfaces */

static int surface_alloc(SySurface* s, int w, int h)
	{
	sy_big_free(s->color); sy_big_free(s->depth);
	s->color = 0; s->depth = 0;
	s->w = w; s->h = h;
	if (w <= 0 || h <= 0)
		return 1;
	s->color = (u32*)sy_big_alloc(w * h * 4);
	if (!s->color)
		return 0;
	memset(s->color, 0, (unsigned)(w * h * 4));
	if (KConfigs[s->config - 1].depth)
		{
		s->depth = (u16*)sy_big_alloc(w * h * 2);
		if (!s->depth) { sy_big_free(s->color); s->color = 0; return 0; }
		memset(s->depth, 0xff, (unsigned)(w * h * 2));
		}
	return 1;
	}

EGLSurface sy_eglCreateWindowSurface(EGLDisplay d, EGLConfig aConfig, NativeWindowType aWin, const EGLint* aAttr)
	{
	SYTRACE(sy_thread(), "eglCreateWindowSurface(cfg %d)", (u32)__builtin_return_address(0), (int)aConfig);
	(void)aAttr;
	SyThread* t = ok_display(d, 1);
	if (!t) return 0;
	const SyConfig* c = config_of(aConfig);
	if (!c) { t->eglError = EGL_BAD_CONFIG; return 0; }
	if (!(c->types & EGL_WINDOW_BIT)) { t->eglError = EGL_BAD_MATCH; return 0; }
	int x, y, w, h;
	if (!aWin || sy_window_rect(aWin, &x, &y, &w, &h)) { t->eglError = EGL_BAD_NATIVE_WINDOW; return 0; }
	SySurface* s = (SySurface*)sy_allocz(sizeof(SySurface));
	if (!s) { t->eglError = EGL_BAD_ALLOC; return 0; }
	s->magic = SY_MAGIC_SURFACE;
	s->config = c->id;
	s->kind = 1;
	s->window = aWin;
	/* the N95's window-system glue (libpvrTechviewWSEGL) reads the window's size and position once,
	   here; afterwards it only gets the visible region (docs/gpu-roadmap.md "Fidelity",
	   H-gles-window-fixed). So does sygl: the surface never tracks a resize or a rotation */
	s->wx = x; s->wy = y;
	if (!surface_alloc(s, w, h))
		{
		sy_free(s);
		t->eglError = EGL_BAD_ALLOC;
		return 0;
		}
	sy_display_open(t);
	sy_hw_surface_new(t, s);
	SYLOG("window surface %dx%d at (%d,%d), config %d%s", w, h, x, y, c->id, s->hw ? ", host GL" : "");
	return (EGLSurface)s;
	}

static SySurface* new_surface(SyThread* t, const SyConfig* c, int aKind, int w, int h)
	{
	SySurface* s = (SySurface*)sy_allocz(sizeof(SySurface));
	if (!s) { t->eglError = EGL_BAD_ALLOC; return 0; }
	s->magic = SY_MAGIC_SURFACE;
	s->config = c->id;
	s->kind = aKind;
	if (!surface_alloc(s, w, h))
		{
		sy_free(s);
		t->eglError = EGL_BAD_ALLOC;
		return 0;
		}
	sy_hw_surface_new(t, s);		/* pbuffers; pixmaps stay in software (their bitmap is the target) */
	return s;
	}

EGLSurface sy_eglCreatePbufferSurface(EGLDisplay d, EGLConfig aConfig, const EGLint* aAttr)
	{
	SYTRACE(sy_thread(), "eglCreatePbufferSurface(cfg %d)", (u32)__builtin_return_address(0), (int)aConfig);
	SyThread* t = ok_display(d, 1);
	if (!t) return 0;
	const SyConfig* c = config_of(aConfig);
	if (!c) { t->eglError = EGL_BAD_CONFIG; return 0; }
	if (!(c->types & EGL_PBUFFER_BIT)) { t->eglError = EGL_BAD_MATCH; return 0; }
	int w = 0, h = 0, largest = 0, texFormat = EGL_NO_TEXTURE;
	for (const EGLint* p = aAttr; p && p[0] != EGL_NONE; p += 2)
		switch (p[0])
			{
		case EGL_WIDTH: w = p[1]; break;
		case EGL_HEIGHT: h = p[1]; break;
		case EGL_LARGEST_PBUFFER: largest = p[1]; break;
		case EGL_TEXTURE_FORMAT: texFormat = p[1]; break;
		case EGL_TEXTURE_TARGET: case EGL_MIPMAP_TEXTURE: break;
		default: t->eglError = EGL_BAD_ATTRIBUTE; return 0;
			}
	if (w < 0 || h < 0) { t->eglError = EGL_BAD_PARAMETER; return 0; }
	if (w > 1024 || h > 1024)
		{
		if (!largest) { t->eglError = EGL_BAD_ALLOC; return 0; }
		if (w > 1024) w = 1024;
		if (h > 1024) h = 1024;
		}
	SySurface* s = new_surface(t, c, 2, w, h);
	if (!s) return 0;
	s->texFormat = texFormat;
	SYLOG("pbuffer surface %dx%d, config %d", w, h, c->id);
	return (EGLSurface)s;
	}

/* pixmap surfaces render into our 32 bpp buffer, initialised from the bitmap, and are written back
   into the bitmap (its own display mode) at the EGL sync points: eglWaitGL, glFinish/glFlush,
   eglSwapBuffers, eglCopyBuffers, and when the surface stops being current or is destroyed */
static void pixmap_flush(SySurface* s)
	{
	if (s && s->kind == 3 && s->pixmap && s->color)
		sy_bitmap_copy(s->pixmap, s->color, s->w, s->h, 1);
	}

void sy_flush_current(void)
	{
	SyThread* t = sy_thread_peek();
	if (t) pixmap_flush(t->draw);
	}

EGLSurface sy_eglCreatePixmapSurface(EGLDisplay d, EGLConfig aConfig, void* aPixmap, const EGLint* aAttr)
	{
	SYTRACE(sy_thread(), "eglCreatePixmapSurface(cfg %d)", (u32)__builtin_return_address(0), (int)aConfig);
	(void)aAttr;
	SyThread* t = ok_display(d, 1);
	if (!t) return 0;
	const SyConfig* c = config_of(aConfig);
	if (!c) { t->eglError = EGL_BAD_CONFIG; return 0; }
	if (!(c->types & EGL_PIXMAP_BIT)) { t->eglError = EGL_BAD_MATCH; return 0; }
	int w, h, mode;
	if (!aPixmap || sy_bitmap_info(aPixmap, &w, &h, &mode)) { t->eglError = EGL_BAD_NATIVE_PIXMAP; return 0; }
	SySurface* s = new_surface(t, c, 3, w, h);
	if (!s) return 0;
	s->pixmap = aPixmap;
	if (sy_bitmap_copy(aPixmap, s->color, w, h, 0))
		SYLOG("pixmap surface: bitmap display mode %d not supported (rendering, not written back)", mode);
	SYLOG("pixmap surface %dx%d mode %d, config %d", w, h, mode, c->id);
	return (EGLSurface)s;
	}

EGLBoolean sy_eglCopyBuffers(EGLDisplay d, EGLSurface aSurface, void* aPixmap)
	{
	SyThread* t = ok_display(d, 1);
	if (!t) return EGL_FALSE;
	SySurface* s = surface_of(aSurface);
	if (!s) return fail(t, EGL_BAD_SURFACE);
	if (!aPixmap || sy_bitmap_copy(aPixmap, s->color, s->w, s->h, 1)) return fail(t, EGL_BAD_NATIVE_PIXMAP);
	return EGL_TRUE;
	}

EGLBoolean sy_eglBindTexImage(EGLDisplay d, EGLSurface aSurface, EGLint aBuffer)
	{
	SyThread* t = ok_display(d, 1);
	if (!t) return EGL_FALSE;
	SySurface* s = surface_of(aSurface);
	if (!s || s->kind != 2) return fail(t, EGL_BAD_SURFACE);
	if (aBuffer != EGL_BACK_BUFFER) return fail(t, EGL_BAD_PARAMETER);
	if (s->texFormat == EGL_NO_TEXTURE) return fail(t, EGL_BAD_MATCH);
	if (!t->ctx) return EGL_TRUE;
	return sy_gl_tex_from_surface(t->ctx, s, s->texFormat == EGL_TEXTURE_RGBA) ? fail(t, EGL_BAD_ALLOC) : EGL_TRUE;
	}

EGLBoolean sy_eglReleaseTexImage(EGLDisplay d, EGLSurface aSurface, EGLint aBuffer)
	{
	(void)aBuffer;
	SyThread* t = ok_display(d, 1);
	if (!t) return EGL_FALSE;
	return surface_of(aSurface) ? EGL_TRUE : fail(t, EGL_BAD_SURFACE);
	}

EGLBoolean sy_eglDestroySurface(EGLDisplay d, EGLSurface aSurface)
	{
	SYTRACE(sy_thread(), "eglDestroySurface(%08x)", (u32)__builtin_return_address(0), (u32)aSurface);
	SyThread* t = ok_display(d, 1);
	if (!t) return EGL_FALSE;
	SySurface* s = surface_of(aSurface);
	if (!s) return fail(t, EGL_BAD_SURFACE);
	pixmap_flush(s);
	if (t->draw == s) t->draw = 0;
	if (t->read == s) t->read = 0;
	if (t->ctx) sy_gl_made_current(t->ctx, t->draw);
	sy_dsa_destroy(s);
	sy_hw_surface_free(t, s);
	sy_big_free(s->color); sy_big_free(s->depth);
	s->magic = 0;
	sy_free(s);
	return EGL_TRUE;
	}

EGLBoolean sy_eglQuerySurface(EGLDisplay d, EGLSurface aSurface, EGLint aAttr, EGLint* aValue)
	{
	SYTRACE(sy_thread(), "eglQuerySurface(%08x, 0x%x)", (u32)__builtin_return_address(0), (u32)aSurface, aAttr);
	SyThread* t = ok_display(d, 1);
	if (!t) return EGL_FALSE;
	SySurface* s = surface_of(aSurface);
	if (!s) return fail(t, EGL_BAD_SURFACE);
	if (!aValue) return fail(t, EGL_BAD_PARAMETER);
	switch (aAttr)
		{
	case EGL_WIDTH: *aValue = s->w; break;
	case EGL_HEIGHT: *aValue = s->h; break;
	case EGL_CONFIG_ID: *aValue = s->config; break;
	case EGL_LARGEST_PBUFFER: *aValue = EGL_FALSE; break;
	default: return fail(t, EGL_BAD_ATTRIBUTE);
		}
	return EGL_TRUE;
	}

EGLBoolean sy_eglSurfaceAttrib(EGLDisplay d, EGLSurface aSurface, EGLint aAttr, EGLint aValue)
	{
	(void)aAttr; (void)aValue;
	SyThread* t = ok_display(d, 1);
	if (!t) return EGL_FALSE;
	return surface_of(aSurface) ? EGL_TRUE : fail(t, EGL_BAD_SURFACE);
	}

/* ---------------------------------------------------------------- contexts */

EGLContext sy_eglCreateContext(EGLDisplay d, EGLConfig aConfig, EGLContext aShare, const EGLint* aAttr)
	{
	SYTRACE(sy_thread(), "eglCreateContext(cfg %d)", (u32)__builtin_return_address(0), (int)aConfig);
	(void)aAttr;
	SyThread* t = ok_display(d, 1);
	if (!t) return 0;
	if (!config_of(aConfig)) { t->eglError = EGL_BAD_CONFIG; return 0; }
	struct SyContext* share = 0;
	if (aShare && !(share = context_of(aShare))) { t->eglError = EGL_BAD_CONTEXT; return 0; }
	struct SyContext* c = sy_gl_create(share);
	if (!c) { t->eglError = EGL_BAD_ALLOC; return 0; }
	return (EGLContext)c;
	}

EGLBoolean sy_eglDestroyContext(EGLDisplay d, EGLContext aCtx)
	{
	SYTRACE(sy_thread(), "eglDestroyContext(%08x)", (u32)__builtin_return_address(0), (u32)aCtx);
	SyThread* t = ok_display(d, 1);
	if (!t) return EGL_FALSE;
	struct SyContext* c = context_of(aCtx);
	if (!c) return fail(t, EGL_BAD_CONTEXT);
	if (t->ctx == c) { t->ctx = 0; t->draw = 0; t->read = 0; }
	sy_gl_destroy(c);
	return EGL_TRUE;
	}

EGLBoolean sy_eglMakeCurrent(EGLDisplay d, EGLSurface aDraw, EGLSurface aRead, EGLContext aCtx)
	{
	SYTRACE(sy_thread(), "eglMakeCurrent(d %d, draw %08x, read %08x, ctx %08x)", (u32)__builtin_return_address(0), (int)d, (u32)aDraw, (u32)aRead, (u32)aCtx);
	SyThread* t = ok_display(d, 1);
	if (!t) return EGL_FALSE;
	if (t->draw && t->draw != (SySurface*)aDraw)
		pixmap_flush(t->draw);
	if (!aCtx && !aDraw && !aRead)
		{
		t->ctx = 0; t->draw = 0; t->read = 0;
		return EGL_TRUE;
		}
	struct SyContext* c = context_of(aCtx);
	if (!c) return fail(t, EGL_BAD_CONTEXT);
	SySurface* dr = surface_of(aDraw);
	SySurface* rd = surface_of(aRead);
	if (!dr || !rd) return fail(t, EGL_BAD_SURFACE);
	t->ctx = c; t->draw = dr; t->read = rd;
	sy_gl_made_current(c, dr);
	return EGL_TRUE;
	}

EGLContext sy_eglGetCurrentContext(void) { SyThread* t = sy_thread_peek(); return t ? (EGLContext)t->ctx : 0; }
EGLDisplay sy_eglGetCurrentDisplay(void)
	{
	SyThread* t = sy_thread_peek();
	SYTRACE(t, "eglGetCurrentDisplay -> %d", (u32)__builtin_return_address(0), t && t->ctx ? 1 : 0);
	return t && t->ctx ? SY_DISPLAY : 0;
	}
EGLSurface sy_eglGetCurrentSurface(EGLint aWhich)
	{
	SyThread* t = sy_thread_peek();
	if (!t) return 0;
	SYTRACE(t, "eglGetCurrentSurface(0x%x) -> %08x", (u32)__builtin_return_address(0), aWhich, (u32)(aWhich == EGL_READ ? t->read : t->draw));
	return (EGLSurface)(aWhich == EGL_READ ? t->read : t->draw);
	}

EGLBoolean sy_eglQueryContext(EGLDisplay d, EGLContext aCtx, EGLint aAttr, EGLint* aValue)
	{
	SyThread* t = ok_display(d, 1);
	if (!t) return EGL_FALSE;
	if (!context_of(aCtx)) return fail(t, EGL_BAD_CONTEXT);
	if (aAttr != EGL_CONFIG_ID || !aValue) return fail(t, EGL_BAD_ATTRIBUTE);
	*aValue = 1;
	return EGL_TRUE;
	}

EGLBoolean sy_eglWaitGL(void) { sy_flush_current(); return EGL_TRUE; }
EGLBoolean sy_eglWaitNative(EGLint aEngine) { (void)aEngine; return EGL_TRUE; }

EGLBoolean sy_eglSwapInterval(EGLDisplay d, EGLint aInterval)
	{
	SyThread* t = ok_display(d, 1);
	if (!t) return EGL_FALSE;
	t->swapInterval = aInterval < 0 ? 0 : (aInterval > 4 ? 4 : aInterval);
	return EGL_TRUE;
	}

/* ---------------------------------------------------------------- presentation */

static void blit(SyThread* t, SySurface* s, int wx, int wy, int x0, int y0, int x1, int y1)
	{
	int sw = (sy_display_rotation(t) & 1) ? 240 : 320, sh = sw == 240 ? 320 : 240;
	if (x0 < wx) x0 = wx;
	if (y0 < wy) y0 = wy;
	if (x1 > wx + s->w) x1 = wx + s->w;
	if (y1 > wy + s->h) y1 = wy + s->h;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > sw) x1 = sw;
	if (y1 > sh) y1 = sh;
	for (int yy = y0; yy < y1; ++yy)
		memcpy((u8*)t->fb + yy * t->fbPitch + x0 * 4, s->color + (yy - wy) * s->w + (x0 - wx), (unsigned)(x1 - x0) * 4);
	}

/* present: copy the surface into the frame buffer, inside the window's visible region from direct
   screen access (the frame buffer holds the picture in WSERV's current rotation, so screen
   coordinates map 1:1). Without a CONE environment (no DSA): the whole window, clipped to the
   screen only (H-gles-fbdirect). */
void sy_present(SyThread* t, SySurface* s)
	{
	if (sy_fb_info(t))
		return;
	sy_hw_sync(s);						/* host GL: the frame into s->color */
	int x = s->wx, y = s->wy;			/* fixed at creation (WSEGL) */
	int rects[16 * 4];
	int n = sy_dsa_region(t, s, rects, 16);
	if (s->frames == 0)
		SYLOG("first frame: surface %dx%d at (%d,%d), %s %d rects", s->w, s->h, x, y,
			  n < 0 ? "no DSA (direct)," : "DSA", n);
	if (n < 0)
		{
		blit(t, s, x, y, x, y, x + s->w, y + s->h);
		return;
		}
	for (int i = 0; i < n; ++i)
		blit(t, s, x, y, rects[i * 4], rects[i * 4 + 1], rects[i * 4] + rects[i * 4 + 2], rects[i * 4 + 1] + rects[i * 4 + 3]);
	}

EGLBoolean sy_eglSwapBuffers(EGLDisplay d, EGLSurface aSurface)
	{
	SyThread* t = ok_display(d, 1);
	if (!t) return EGL_FALSE;
	SySurface* s = surface_of(aSurface);
	if (!s) return fail(t, EGL_BAD_SURFACE);
	if (s->kind != 1) { pixmap_flush(s); return EGL_TRUE; }	/* no-op for pbuffers (EGL 1.1) */
	sy_present(t, s);
	for (int i = 0; i < t->swapInterval; ++i)
		sy_display_wait_vsync(t);
	++s->frames;
	unsigned now = sy_ms();
	if (++t->fpsFrames == 1)
		t->fpsTick = now;
	else if (now - t->fpsTick >= 2000)
		{
		unsigned dt = now - t->fpsTick;
		SYLOG("%d frames in %u ms: %u.%u fps", t->fpsFrames - 1, dt,
			  (unsigned)(t->fpsFrames - 1) * 1000u / dt, ((unsigned)(t->fpsFrames - 1) * 10000u / dt) % 10u);
		t->fpsFrames = 1;
		t->fpsTick = now;
		}
	return EGL_TRUE;
	}


/* default for every entry point not implemented yet (r0 = ordinal, from the generated stub) */
extern const char* const sygl_names[];
unsigned sygl_unimpl(int aOrdinal)
	{
	SYLOG("unimplemented %s (ord %d)", sygl_names[aOrdinal], aOrdinal);
	return 0;
	}
