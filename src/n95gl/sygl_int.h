/* sygl internals: shared by the EGL layer (egl.c), GL state and vertex pipeline (gl.c), the
   rasterizer (raster.c) and the Symbian platform layer (plat.cpp). Portable C except plat.cpp.
   Rule: no writable static data anywhere (the blob is ROM); state lives in SyThread (per thread,
   UserSvr::DllTls) and in the objects it points to. */
#ifndef SYGL_INT_H
#define SYGL_INT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#include "n95imports.h"
typedef int (*SyPrintf)(const char*, ...);
#define SYLOG(...) ((SyPrintf)N95_RDebug_Printf)("SYGL: " __VA_ARGS__)

#define SY_MAGIC_SURFACE 0x53595346u	/* 'SYSF' */
#define SY_MAGIC_CONTEXT 0x53594358u	/* 'SYCX' */

/* a rendering surface: 32 bpp 0x00RRGGBB colour (top row first) + 16-bit depth */
typedef struct SySurface
	{
	u32 magic;
	int config;			/* config id */
	int kind;			/* 1 window, 2 pbuffer, 3 pixmap */
	void* pixmap;		/* CFbsBitmap* of a pixmap surface */
	int texFormat;		/* pbuffer EGL_TEXTURE_FORMAT (eglBindTexImage) */
	void* window;		/* RWindow* (native window) */
	void* dsa;			/* CSyDsa* (plat.cpp): direct screen access for window surfaces */
	int w, h;
	u32* color;
	u16* depth;
	int frames;
	} SySurface;

struct SyContext;

typedef struct SyThread
	{
	int eglError;
	int initialized;	/* eglInitialize done (per thread: no process-wide writable data) */
	struct SyContext* ctx;
	SySurface* draw;
	SySurface* read;
	int swapInterval;
	int display;		/* RBusLogicalChannel handle to DisplayDriver (0 = not open) */
	u32* fb;			/* frame buffer (HAL EDisplayMemoryAddress) */
	int fbPitch;		/* bytes per line */
	unsigned fpsTick;	/* NTickCount at the last fps report */
	int fpsFrames;
	void* bigHeap;		/* RHeap* (plat.cpp) */
	int traced;			/* EGL calls traced so far (SYTRACE) */
	int notInitLogged;
	} SyThread;

/* trace of the first EGL calls of each thread with the caller's address (sygl entry points are
   reached by a plain branch from libgles_cm, so LR is the app's return address) */
#ifndef SY_TRACE_MAX				/* src/n95gl/build.sh --trace sets 700 */
#define SY_TRACE_MAX 0
#endif
#define SYTRACE(t, fmt, ...) do { if ((t) && (t)->traced < SY_TRACE_MAX) { ++(t)->traced; \
	((SyPrintf)N95_RDebug_Printf)("SYGL: trace [%08x] %08x " fmt, (u32)(t), __VA_ARGS__); } } while (0)

/* ---- platform (plat.cpp) ---- */
void* sy_alloc(int aBytes);
void* sy_allocz(int aBytes);
void sy_free(void* aPtr);
SyThread* sy_thread(void);				/* current thread's state, created on first use (NULL: no memory) */
SyThread* sy_thread_peek(void);			/* NULL if none yet */
unsigned sy_ms(void);					/* millisecond tick */
int sy_window_rect(void* aWindow, int* aX, int* aY, int* aW, int* aH);	/* 0 ok */
int sy_display_open(SyThread* aT);		/* 0 ok */
int sy_display_rotation(SyThread* aT);	/* WSERV rotation index 0..3 (3 = portrait) */
void sy_display_wait_vsync(SyThread* aT);
void sy_display_close(SyThread* aT);
int sy_fb_info(SyThread* aT);			/* fills aT->fb / fbPitch, 0 ok */
int sy_dsa_region(SyThread* aT, SySurface* aS, int* aRects, int aMax);	/* visible rects (screen), -1: no DSA */
int sy_bitmap_info(void* aBmp, int* aW, int* aH, int* aMode);			/* CFbsBitmap*: 0 ok */
int sy_bitmap_copy(void* aBmp, u32* aPix, int aW, int aH, int aToBitmap);	/* 0 ok, -1 unsupported mode */
void sy_dsa_destroy(SySurface* aS);

/* ---- libc bits (libc.c); C++ gets them from the SDK headers ---- */
#if defined(SYGL_HOST)			/* host tests (tests/sygl): the C library's */
#include <string.h>
#elif !defined(__cplusplus)
void* memcpy(void* aDst, const void* aSrc, unsigned aLen);
void* memset(void* aDst, int aVal, unsigned aLen);
#endif
float sy_sinf(float aRad);
float sy_cosf(float aRad);
float sy_sqrtf(float aX);
float sy_powf(float aX, float aE);

/* ---- GL (gl.c) ---- */
struct SyContext* sy_gl_create(struct SyContext* aShareWith);	/* shares texture objects with it (EGL share context) */
void sy_gl_destroy(struct SyContext* aCtx);
void sy_gl_made_current(struct SyContext* aCtx, SySurface* aDraw);

/* ---- rasterizer (raster.c) ---- */
/* window coords (y down), z 0..1, colour 0..255, iw = 1/w_clip, sw/tw = s/w, t/w (perspective) */
typedef struct SyRVtx { float x, y, z; float r, g, b, a; float iw, sw, tw; float fog; } SyRVtx;	/* fog 0..255 (255 = no fog) */
typedef struct SyRTex
	{
	const u32* data;		/* 0xAARRGGBB */
	int w, h;
	int linear;				/* bilinear (else nearest) */
	int clampS, clampT;		/* clamp to edge (else repeat) */
	int hasRGB, hasA;		/* base format has colour / alpha components */
	} SyRTex;
typedef struct SyRState
	{
	int depthTest, depthWrite, depthFunc;
	const SyRTex* tex;		/* NULL: untextured */
	int envMode;			/* GL_MODULATE, GL_REPLACE, GL_DECAL, GL_BLEND, GL_ADD */
	int envR, envG, envB, envA;
	int alphaTest, alphaFunc, alphaRef;		/* ref 0..255 */
	int blend, blendSrc, blendDst;
	int scX0, scY0, scX1, scY1;			/* drawable rect in surface coords (x1/y1 exclusive) */
	int writeColor;
	int polyOffset; float polyFactor, polyUnits;	/* glPolygonOffset (depth units of 1/65535) */
	int fog, fogR, fogG, fogB;						/* fog colour 0..255; per-vertex factor in SyRVtx.fog */
	} SyRState;
void sy_raster_triangle(SySurface* aS, const SyRState* aSt, const SyRVtx* aV0, const SyRVtx* aV1, const SyRVtx* aV2);
void sy_raster_clear(SySurface* aS, int aColor, u32 aRgb, int aDepth, u16 aZ, int aX, int aY, int aW, int aH);

/* big buffers (surfaces, textures): our own chunk heap per thread (plat.cpp), like Nokia's PVRHeap,
   so the app's heap limit (often 1 MB) does not apply */
void* sy_big_alloc(int aBytes);
void sy_big_free(void* aPtr);

/* ---- EGL (egl.c) ---- */
void sy_present(SyThread* aT, SySurface* aS);
void sy_flush_current(void);			/* glFlush/glFinish: write pixmap surfaces back */
struct SyContext;
int sy_gl_tex_from_surface(struct SyContext* aCtx, SySurface* aS, int aHasAlpha);	/* eglBindTexImage */

#ifdef __cplusplus
}
#endif
#endif
