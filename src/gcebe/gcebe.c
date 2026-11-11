/*
 * gcebe: our composition backend for the Nokia N8's window server (Symbian^3 GCE).
 *
 * The firmware's gce.dll (Gce::CGce, XIP) creates its backend with compositionbackend.dll ordinal 1
 * and then uses only the object's vtables; the firmware backend drives the Broadcom VideoCore. This
 * blob sits at a fixed ROM address (src/gcebe/gcebe.ld) and gce's import thunk is rebound to
 * gcebe_create (tools/blob_glue.py rebind). Interface: docs/devices/nokia-n8-rm596/composition.md
 * "Backend interface" (slot table checked against gce.dll's call sites).
 *
 * Object layout (as the firmware's CVCInternalDisplayCompositionBackend, because gce and the
 * surface update server adjust pointers by these offsets):
 *   +0x00 primary vtable (33 slots: Gce::CCompositionBackend, a CBase)
 *   +0x04 MCompositionSurfaceUpdate vtable (4 slots, UID 0x10286498)
 *   +0x5C MDisplayControl vtable (9 slots, UID 0x10286497)
 *   vtSurfaceNotify: a per-surface one-shot notification (UID 0x2002E6D5, name unknown), asked for by
 *   Belle FP2's gcerenderstage on behalf of alfcompositorrs: slot 0 request (surface, 1, 0, status),
 *   slot 1 cancel (surface). Completed at once (NULL interface: the window server died)
 * Composition: on the host GPU (syborg-gl SYGL_COMPOSE through our DisplayDriver): the staged
 * layers, bottom to top, blended and scaled; EGL windows' frames never leave the GPU (khr presents
 * them, SYGL_VG_PRESENT), the other surfaces are read from guest memory in place; the host writes
 * the finished screen into the display memory (HAL EDisplayMemoryAddress). Without the host GPU the
 * layers are copied here (no scaling, no blending).
 * SPDX-License-Identifier: MIT
 */
#include "gen/rom_imports.h"
#include "../qemu-syborg/hw/arm/syborg_gl_proto.h"

typedef int TInt;
typedef unsigned int TUint32;
typedef struct { TInt x0, y0, x1, y1; } TRect;
typedef struct { TInt w, h; } TSize;
typedef struct { TUint32 w[4]; } TSurfaceId;
typedef struct { TInt iStatus, iFlags; } TRequestStatus;

#define KErrNone 0
#define KErrGeneral (-2)
#define KErrCancel (-3)
#define KErrNotSupported (-5)
#define KErrArgument (-6)
#define KErrAlreadyExists (-11)
#define KErrExtensionNotSupported (-47)
#define KRequestPending ((TInt)0x80000001)

#define UID_DISPLAY_CONTROL 0x10286497u
#define UID_SURFACE_UPDATE  0x10286498u
#define UID_SURFACE_NOTIFY  0x2002E6D5u

/* ---- ROM exports (veneers generated from models/<id>/composition/imports.txt) ---- */
extern void RDebug_Printf(const char *fmt, ...) __asm__("_ZN6RDebug6PrintfEPKcz");
extern void *User_AllocZ(TInt size) __asm__("_ZN4User6AllocZEi");
extern void User_Free(void *p) __asm__("_ZN4User4FreeEPv");
extern void User_RequestComplete(TRequestStatus **s, TInt r) __asm__("_ZN4User15RequestCompleteERP14TRequestStatusi");
extern TUint32 User_FastCounter(void) __asm__("_ZN4User11FastCounterEv");
extern TInt RArray_Append(void *array, const void *entry) __asm__("_ZN10RArrayBase6AppendEPKv");
extern void RArray_Reset(void *array) __asm__("_ZN10RArrayBase5ResetEv");
extern unsigned char *RChunk_Base(const TInt *chunk) __asm__("_ZNK6RChunk4BaseEv");
extern void RHandle_Close(TInt *h) __asm__("_ZN11RHandleBase5CloseEv");
extern TInt HAL_Get(TInt attr, TInt *val) __asm__("_ZN3HAL3GetEN7HALData10TAttributeERi");
extern TInt SUS_Start(void **provider) __asm__("surfaceupdateserver_ord1");
extern void SM_Ctor(void *sm) __asm__("_ZN15RSurfaceManagerC1Ev");
extern TInt SM_Open(void *sm) __asm__("_ZN15RSurfaceManager4OpenEv");
extern void SM_Close(void *sm) __asm__("_ZN15RSurfaceManager5CloseEv");
extern TInt SM_Info(void *sm, const TSurfaceId *id, void *pckg) __asm__("_ZN15RSurfaceManager11SurfaceInfoERK10TSurfaceIdR8TPckgBufINS_15TSurfaceInfoV01EE");
extern TInt SM_Map(void *sm, const TSurfaceId *id, TInt *chunk) __asm__("_ZN15RSurfaceManager10MapSurfaceERK10TSurfaceIdR6RChunk");
extern TInt SM_BufOffset(void *sm, const TSurfaceId *id, TInt buffer, TInt *offset) __asm__("_ZN15RSurfaceManager15GetBufferOffsetERK10TSurfaceIdiRi");
extern TInt SM_OpenSurface(void *sm, const TSurfaceId *id) __asm__("_ZN15RSurfaceManager11OpenSurfaceERK10TSurfaceId");
extern TInt SM_CloseSurface(void *sm, const TSurfaceId *id) __asm__("_ZN15RSurfaceManager12CloseSurfaceERK10TSurfaceId");
extern TInt BLC_DoCreate(TInt *h, const void *name, const void *ver, TInt unit, const void *drv, const void *info,
                         TInt type) __asm__("_ZN18RBusLogicalChannel8DoCreateERK7TDesC16RK8TVersioniPS1_PK6TDesC8i");
extern TInt BLC_DoControl(TInt *h, TInt fn, void *a1) __asm__("_ZN18RBusLogicalChannel9DoControlEiPv");

/* HALData::TAttribute (hal_data.h) */
enum { EDisplayXPixels = 31, EDisplayYPixels = 32, EDisplayXTwips = 33, EDisplayYTwips = 34,
       EDisplayBitsPerPixel = 76, EDisplayMemoryAddress = 78, EDisplayOffsetToFirstPixel = 79,
       EDisplayOffsetBetweenLines = 80 };
/* TUidPixelFormat (pixelformats.h) */
#define PF_XRGB_8888     0x10275496u
#define PF_ARGB_8888     0x10275498u
#define PF_ARGB_8888_PRE 0x1027549Au
#define PF_RGB_565       0x1027549Eu

/* RSurfaceManager::TSurfaceInfoV01 inside a TPckgBuf (TBuf8: length|type word, max length, data) */
typedef struct { TSize iSize; TInt iBuffers; TUint32 iPixelFormat; TInt iStride; TInt iContiguous;
                 TInt iCacheAttrib; TInt iMappable; } TSurfaceInfoV01;
typedef struct { TUint32 iLength; TInt iMaxLength; TSurfaceInfoV01 info; } TInfoPckg;

/* gce's layer (CLayerImpl): the backend reads it through these virtual slots */
typedef struct Layer { void **vt; } Layer;
#define LV(l, slot) ((l)->vt[slot])
typedef TInt (*LGetRect)(Layer *, TRect *);
typedef TInt (*LInt)(Layer *);
typedef Layer *(*LLayer)(Layer *);
typedef const TSurfaceId *(*LSurf)(Layer *);
enum { L_SRC_RECT = 1, L_ALPHA = 3, L_DST_RECT = 5, L_ROTATION = 7, L_NEXT = 14, L_PREV = 15,
       L_SURFACE = 17, L_ZORDER = 25, L_NULL_SURFACE = 27 };

#define MAX_SURF 24
#define MAX_STAGE 32
typedef struct {
	TSurfaceId id;
	TInt chunk;             /* RChunk handle (thread-owned by the window server thread) */
	unsigned char *base;    /* mapping, valid process-wide while the handle is open */
	TSurfaceInfoV01 info;
	TInt offset[2];         /* buffer offsets (first two buffers) */
	TInt current;           /* buffer of the last ContentUpdated */
	TInt used;
	TInt dirty;             /* updated since the host last read it */
	TInt updated;           /* ever announced by ContentUpdated (else read at every composition) */
} Surf;

typedef struct Backend {
	const void *const *vt;              /* +0x00 */
	const void *const *vtSurfaceUpdate; /* +0x04 */
	TInt screen;                        /* +0x08 */
	TInt w, h, twW, twH;                /* +0x0C native (rotation 0) size */
	TInt rotation;                      /* +0x1C */
	void *provider;                     /* +0x20 MSurfaceUpdateServerProvider */
	unsigned char *fb;                  /* +0x24 display memory */
	TInt fbStride, fbBpp;               /* +0x28 */
	TRequestStatus *dispChange, *cfgChange;   /* +0x30 */
	TInt logCount;                      /* +0x38 */
	TInt nStaged;                       /* +0x3C */
	TInt commits, updates;              /* +0x40 */
	TInt pad[4];                        /* +0x48 .. 0x57 */
	TInt pad2;                          /* +0x58 */
	const void *const *vtDisplay;       /* +0x5C */
	TInt sm[4];                         /* RSurfaceManager */
	TInt smOpen;
	Layer *staged[MAX_STAGE];
	Layer *scene[MAX_STAGE];            /* last committed screen scene */
	TInt nScene;
	Surf surf[MAX_SURF];
	TInt gl;                            /* host composition: 0 not tried, 1 on, -1 off */
	TInt glChan;                        /* RBusLogicalChannel to DisplayDriver (process-owned) */
	volatile TUint32 *glRegs;           /* syborg-gl registers, user-mapped (or 0: kick by control) */
	TInt glFails;
	TUint32 sceneSig;                   /* layer list of the last logged scene (commit log) */
	TInt sceneLogs;
	TInt rotLogs;                       /* landscape layers logged (HostCompose) */
	const void *const *vtSurfaceNotify; /* UID 0x2002E6D5 (FP2 surface notification) */
	TInt notifyLogs;
	TInt offLogs;                       /* off-screen composition requests logged */
} Backend;

_Static_assert(__builtin_offsetof(Backend, vtDisplay) == 0x5C, "MDisplayControl base must be at +0x5C");

#define LOGMAX 1500
#define LOG(b, ...) do { if ((b)->logCount < LOGMAX) { (b)->logCount++; RDebug_Printf(__VA_ARGS__); } } while (0)

static Backend *FromSU(void *p) { return (Backend *)((char *)p - 4); }
static Backend *FromDC(void *p) { return (Backend *)((char *)p - 0x5C); }

static void Complete(TRequestStatus **s, TInt r)
{
	if (*s)
		User_RequestComplete(s, r);     /* sets *s = NULL */
}

static int SameId(const TSurfaceId *a, const TSurfaceId *b)
{
	return a->w[0] == b->w[0] && a->w[1] == b->w[1] && a->w[2] == b->w[2] && a->w[3] == b->w[3];
}

static int IsNullId(const TSurfaceId *a)
{
	return !(a->w[0] | a->w[1] | a->w[2] | a->w[3]);
}

/* ---- surfaces ---- */
static Surf *FindSurf(Backend *b, const TSurfaceId *id)
{
	for (int i = 0; i < MAX_SURF; i++)
		if (b->surf[i].used && SameId(&b->surf[i].id, id))
			return &b->surf[i];
	return 0;
}

/* window server thread only: opens, maps and describes a surface */
static Surf *MapSurf(Backend *b, const TSurfaceId *id)
{
	Surf *s = FindSurf(b, id);
	if (s)
		return s;
	/* a screen surface (TSurfaceId type 1, EScreenSurface: {screen, HAL mode [| flipped], pixel format}) is
	   the display memory (HAL EDisplayMemoryAddress), where CFbsScreenDevice draws: the window server sets
	   it as the background of a window that uses direct screen access. The host composition reads it like
	   any layer; its own output is the host's (the display memory is not the scan-out). */
	if (!IsNullId(id) && (id->w[3] >> 24) == 1) {
		/* registered with every screen's backend: only the screen it belongs to (with display memory)
		   has its pixels; elsewhere it registers without them (not composed there) */
		unsigned char *mem = (TInt)id->w[0] == b->screen ? b->fb : 0;
		for (int i = 0; i < MAX_SURF; i++) {
			if (b->surf[i].used)
				continue;
			s = &b->surf[i];
			TInt flipped = (id->w[1] & 0x80000000u) != 0;
			s->id = *id;
			s->chunk = 0;
			s->base = mem;
			s->info.iSize.w = flipped ? b->h : b->w;
			s->info.iSize.h = flipped ? b->w : b->h;
			s->info.iBuffers = 1;
			s->info.iPixelFormat = id->w[2];
			s->info.iStride = flipped ? s->info.iSize.w * 4 : b->fbStride;
			s->offset[0] = s->offset[1] = 0;
			s->current = 0;
			s->used = 1;
			LOG(b, "GCEBE: screen surface %08x %08x %08x %08x: %dx%d fmt %08x stride %d (the display memory)",
			    id->w[0], id->w[1], id->w[2], id->w[3], s->info.iSize.w, s->info.iSize.h, s->info.iPixelFormat,
			    s->info.iStride);
			return s;
		}
		return 0;
	}
	if (!b->smOpen || IsNullId(id))
		return 0;
	for (int i = 0; i < MAX_SURF; i++) {
		if (b->surf[i].used)
			continue;
		s = &b->surf[i];
		TInfoPckg pk = { (3u << 28) | sizeof(TSurfaceInfoV01), sizeof(TSurfaceInfoV01), { { 0, 0 }, 0, 0, 0, 0, 0, 0 } };
		TInt r = SM_OpenSurface(b->sm, id);
		if (r == KErrNone)
			r = SM_Info(b->sm, id, &pk);
		TInt chunk = 0;
		if (r == KErrNone)
			r = SM_Map(b->sm, id, &chunk);
		if (r < 0) {
			LOG(b, "GCEBE: surface %08x %08x %08x %08x: open/info/map %d", id->w[0], id->w[1], id->w[2], id->w[3], r);
			return 0;
		}
		s->id = *id;
		s->chunk = r > 0 ? r : chunk;           /* MapSurface returns the handle (SetReturnedHandle) */
		s->base = RChunk_Base(&s->chunk);
		s->info = pk.info;
		s->offset[0] = s->offset[1] = 0;
		SM_BufOffset(b->sm, id, 0, &s->offset[0]);
		if (s->info.iBuffers > 1)
			SM_BufOffset(b->sm, id, 1, &s->offset[1]);
		s->current = 0;
		s->used = 1;
		LOG(b, "GCEBE: surface %08x %08x %08x %08x: %dx%d fmt %08x stride %d buffers %d base %08x off %x/%x",
		    id->w[0], id->w[1], id->w[2], id->w[3], s->info.iSize.w, s->info.iSize.h, s->info.iPixelFormat,
		    s->info.iStride, s->info.iBuffers, (TUint32)s->base, s->offset[0], s->offset[1]);
		return s;
	}
	LOG(b, "GCEBE: surface table full");
	return 0;
}

/* ---- host composition (syborg-gl through DisplayDriver controls 0x7f10/0x7f12/0x7f13) ---- */
static const unsigned short KDisplayDriverName[] = { 'D','i','s','p','l','a','y','D','r','i','v','e','r' };
static const TUint32 KDisplayDriverDes[2] = { (1u << 28) | 13, (TUint32)KDisplayDriverName };   /* TPtrC16 */
static const unsigned char KDisplayDriverVer[4] = { 1, 1, 1540 & 255, 1540 >> 8 };               /* TVersion */
#define MAX_HOST_LAYERS 16

static int HostOn(Backend *b)
{
	if (b->gl)
		return b->gl > 0;
	b->gl = -1;
	TInt r = BLC_DoCreate(&b->glChan, KDisplayDriverDes, KDisplayDriverVer, -1, 0, 0, 0 /* EOwnerProcess */);
	TUint32 q[2] = { 0, 0 }, lin = 0;
	if (r == KErrNone)
		r = BLC_DoControl(&b->glChan, 0x7f10, q);
	if (r == KErrNone && q[0]) {
		b->gl = 1;
		if (BLC_DoControl(&b->glChan, 0x7f13, &lin) == KErrNone && lin)
			b->glRegs = (volatile TUint32 *)lin;
	}
	LOG(b, "GCEBE: screen %d host composition %s (%d, kick %s)", b->screen, b->gl > 0 ? "on" : "off", r,
	    b->glRegs ? "user" : "kernel");
	return b->gl > 0;
}

static TInt HostKick(Backend *b, TUint32 *cmd, TInt words)
{
	SyglKick k = { (TUint32)cmd, (TUint32)words * 4, 0, 0, ~0u };
	if (b->glRegs) {
		/* the device reads cmd and k and writes k.result behind the compiler's back */
		__asm__ volatile("" ::: "memory");
		b->glRegs[SYGL_REG_KICK] = (TUint32)&k;     /* runs synchronously in the device */
		__asm__ volatile("" ::: "memory");
		return k.result == 0 ? KErrNone : KErrGeneral;
	}
	return BLC_DoControl(&b->glChan, 0x7f12, &k);
}

static TInt PixelFormatCode(TUint32 f)
{
	return f == PF_XRGB_8888 ? 0 : f == PF_ARGB_8888 ? 1 : f == PF_ARGB_8888_PRE ? 2 : f == PF_RGB_565 ? 3 : -1;
}

/* the scene's layers to the host: 1 if composed there */
/* The layers of the committed scene in composition order. gce stages chain heads; a chain continues
 * through NextLayer in z order (Anna's home screen and Belle's TV-out clone use chains of two), and a
 * layer can head one chain and follow in another: each layer once. */
static int SceneLayers(Backend *b, Layer **out, int max)
{
	int n = 0;
	for (int i = 0; i < b->nScene; i++)
		for (Layer *l = b->scene[i]; l && n < max; l = ((LLayer)LV(l, L_NEXT))(l)) {
			int seen = 0;
			for (int k = 0; k < n && !seen; k++)
				seen = out[k] == l;
			if (seen)
				break;
			out[n++] = l;
		}
	return n;
}

static int HostCompose(Backend *b)
{
	if (!b->fb || b->fbBpp != 32 || !HostOn(b))
		return 0;
	TUint32 cmd[7 + MAX_HOST_LAYERS * SYGL_LAYER_WORDS];
	TInt n = 0;
	Layer *all[MAX_STAGE];
	int nAll = SceneLayers(b, all, MAX_STAGE);
	for (int i = 0; i < nAll && n < MAX_HOST_LAYERS; i++) {
		Layer *l = all[i];
		if (!l || ((LInt)LV(l, L_NULL_SURFACE))(l))
			continue;
		const TSurfaceId *id = ((LSurf)LV(l, L_SURFACE))(l);
		Surf *s = FindSurf(b, id);
		TInt fmt = s ? PixelFormatCode(s->info.iPixelFormat) : -1;
		if (!s || !s->base || fmt < 0)
			continue;
		SyglLayer *y = (SyglLayer *)(cmd + 7 + n * SYGL_LAYER_WORDS);
		TRect d, src;
		((LGetRect)LV(l, L_DST_RECT))(l, &d);
		if (!((LGetRect)LV(l, L_SRC_RECT))(l, &src))
			src.x0 = src.y0 = 0, src.x1 = s->info.iSize.w, src.y1 = s->info.iSize.h;
		TInt alpha = ((LInt)LV(l, L_ALPHA))(l);
		TInt rot = ((LInt)LV(l, L_ROTATION))(l) & 3;     /* OpenWF-C: source turned anticlockwise */
		/* virtual rotation (wsini.ini VIRTUALROTATION, screen mode 2 = 640x360 at 90): alf draws the UI
		   pre-rotated into its portrait surface, but an app window's layer keeps the window's rectangle
		   in the landscape screen (Angry Birds: 0,0-640,360). Such a rectangle (not inside the panel,
		   inside the panel turned) goes onto the panel turned 90 anticlockwise (alf maps its portrait
		   surface the other way, rot 3). Its content turns too, unless the source is already in the
		   panel's orientation (a FixNativeOrientation window: its EGL surface is 360x640 and the app
		   renders rotated itself). */
		if ((d.x1 > b->w || d.y1 > b->h) && d.x0 >= 0 && d.y0 >= 0 && d.x1 <= b->h && d.y1 <= b->w) {
			TRect p = { d.y0, b->h - d.x1, d.y1, b->h - d.x0 };
			TInt sw = src.x1 - src.x0, sh = src.y1 - src.y0;
			if ((rot & 1) ? sh > sw : sw > sh)          /* a landscape picture: turn it with the rect */
				rot = (rot + 1) & 3;
			if (b->rotLogs++ < 8)
				LOG(b, "GCEBE: landscape layer %d,%d-%d,%d -> panel %d,%d-%d,%d, source %dx%d rot %d",
				    d.x0, d.y0, d.x1, d.y1, p.x0, p.y0, p.x1, p.y1, sw, sh, rot);
			d = p;
		}
		for (int k = 0; k < 4; k++)
			y->sid[k] = id->w[k];
		y->dx0 = d.x0; y->dy0 = d.y0; y->dx1 = d.x1; y->dy1 = d.y1;
		y->sx0 = src.x0; y->sy0 = src.y0; y->sx1 = src.x1; y->sy1 = src.y1;
		y->alpha = alpha < 0 || alpha > 255 ? 255 : alpha;
		y->fmt = fmt | rot << 8;            /* the host: format in the low byte, rotation in bits 8..9 */
		y->va = (TUint32)(s->base + s->offset[s->current & 1]);
		y->stride = s->info.iStride;
		y->w = s->info.iSize.w;
		y->h = s->info.iSize.h;
		y->dirty = s->dirty || !s->updated;
		s->dirty = 0;
		n++;
	}
	cmd[0] = SYGL_COMPOSE;
	cmd[1] = 7 + n * SYGL_LAYER_WORDS;
	cmd[2] = (TUint32)b->fb; cmd[3] = b->w; cmd[4] = b->h; cmd[5] = b->fbStride;
	cmd[6] = n | (TUint32)(b->rotation & 3) << 24;    /* layer rectangles are in the rotated screen */
	TInt r = HostKick(b, cmd, cmd[1]);
	if (r != KErrNone && b->glFails++ < 4)
		LOG(b, "GCEBE: host composition %d (%d layers)", r, n);
	return r == KErrNone;
}

static void HostForget(Backend *b, const TSurfaceId *id)
{
	if (b->gl <= 0)
		return;
	TUint32 cmd[6] = { SYGL_SCANOUT_FREE, 6, id->w[0], id->w[1], id->w[2], id->w[3] };
	HostKick(b, cmd, 6);
}

static void UnmapSurf(Backend *b, const TSurfaceId *id)
{
	Surf *s = FindSurf(b, id);
	if (!s)
		return;
	for (int i = 0; i < b->nScene; i++)     /* forget it in the scene: it is going away */
		if (b->scene[i] && SameId(((LSurf)LV(b->scene[i], L_SURFACE))(b->scene[i]), id))
			b->scene[i] = 0;
	if (s->chunk) {                     /* surface-manager surfaces (screen surfaces have no chunk) */
		RHandle_Close(&s->chunk);
		SM_CloseSurface(b->sm, id);
	}
	HostForget(b, id);
	s->used = 0;
}

/* ---- composition: copy a layer's visible part into the display memory ---- */
static void Blit(Backend *b, Layer *l)
{
	if (!l || !b->fb || ((LInt)LV(l, L_NULL_SURFACE))(l))
		return;
	const TSurfaceId *id = ((LSurf)LV(l, L_SURFACE))(l);
	Surf *s = FindSurf(b, id);
	if (!s || !s->base)
		return;
	TRect d, src;
	((LGetRect)LV(l, L_DST_RECT))(l, &d);
	if (!((LGetRect)LV(l, L_SRC_RECT))(l, &src))
		src.x0 = src.y0 = 0, src.x1 = s->info.iSize.w, src.y1 = s->info.iSize.h;
	/* no scaling yet: the source origin maps to the destination origin, clipped to both */
	TInt w = d.x1 - d.x0, h = d.y1 - d.y0;
	if (w > src.x1 - src.x0) w = src.x1 - src.x0;
	if (h > src.y1 - src.y0) h = src.y1 - src.y0;
	TInt dx = d.x0, dy = d.y0, sx = src.x0, sy = src.y0;
	if (dx < 0) { sx -= dx; w += dx; dx = 0; }
	if (dy < 0) { sy -= dy; h += dy; dy = 0; }
	if (dx + w > b->w) w = b->w - dx;
	if (dy + h > b->h) h = b->h - dy;
	if (w <= 0 || h <= 0)
		return;
	const unsigned char *sp = s->base + s->offset[s->current & 1];
	TUint32 fmt = s->info.iPixelFormat;
	int s32 = fmt == PF_XRGB_8888 || fmt == PF_ARGB_8888 || fmt == PF_ARGB_8888_PRE;
	for (TInt y = 0; y < h; y++) {
		const unsigned char *srow = sp + (sy + y) * s->info.iStride;
		unsigned char *drow = b->fb + (dy + y) * b->fbStride;
		if (b->fbBpp == 32 && s32) {
			const TUint32 *p = (const TUint32 *)srow + sx;
			TUint32 *q = (TUint32 *)drow + dx;
			for (TInt x = 0; x < w; x++)
				q[x] = p[x] | 0xff000000u;
		} else if (b->fbBpp == 32 && fmt == PF_RGB_565) {
			const unsigned short *p = (const unsigned short *)srow + sx;
			TUint32 *q = (TUint32 *)drow + dx;
			for (TInt x = 0; x < w; x++) {
				TUint32 c = p[x];
				q[x] = 0xff000000u | ((c & 0xf800) << 8) | ((c & 0x07e0) << 5) | ((c & 0x001f) << 3);
			}
		}
	}
}

static void ComposeScene(Backend *b)
{
	Layer *all[MAX_STAGE];
	int n = SceneLayers(b, all, MAX_STAGE);
	for (int i = 0; i < n; i++)
		Blit(b, all[i]);
}

/* ---- primary vtable (Gce::CCompositionBackend) ---- */
static void *Dtor(Backend *b) { return b; }
static void *DtorDelete(Backend *b) { User_Free(b); return b; }

static TInt Extension(Backend *b, TUint32 uid, void **out, void *arg)
{
	if (uid == UID_DISPLAY_CONTROL) { *out = (char *)b + 0x5C; return KErrNone; }
	if (uid == UID_SURFACE_UPDATE) { *out = (char *)b + 4; return KErrNone; }
	if (uid == UID_SURFACE_NOTIFY) { *out = &b->vtSurfaceNotify; return KErrNone; }
	*out = 0;
	return KErrExtensionNotSupported;
}

static void Release(Backend *b)
{
	LOG(b, "GCEBE: release screen %d", b->screen);
	for (int i = 0; i < MAX_SURF; i++)
		if (b->surf[i].used)
			UnmapSurf(b, &b->surf[i].id);
	if (b->smOpen)
		SM_Close(b->sm);
	Complete(&b->dispChange, KErrCancel);
	Complete(&b->cfgChange, KErrCancel);
	User_Free(b);
}

static TInt ApiVersion(Backend *b) { return 1; }
static TUint32 InternalVersion(Backend *b) { return 1 | (8 << 8) | (1 << 16); }   /* TVersion 1.8.1 */
static TInt AttachLayer(Backend *b, Layer *l) { LOG(b, "GCEBE: attach layer %08x", (TUint32)l); return KErrNone; }

static void DetachLayer(Backend *b, Layer *l)
{
	for (int i = 0; i < b->nScene; i++)
		if (b->scene[i] == l)
			b->scene[i] = 0;
	for (int i = 0; i < b->nStaged; i++)
		if (b->staged[i] == l)
			b->staged[i] = 0;
}

static TInt ResizeLayerRegions(Backend *b, Layer *l, TInt n) { return KErrNone; }
static void SetScreenRotation(Backend *b, TInt r)
{
	if ((r & 3) != b->rotation)
		LOG(b, "GCEBE: screen rotation %d", r);
	b->rotation = r & 3;
}
static TInt UseDirtyRects(Backend *b) { return 0; }
/* every rotation: the host composition turns the scene onto the panel (SYGL_COMPOSE rotation bits),
   as the N8's composition hardware does; apps in landscape get a 640x360 screen */
static TInt SupportsRotation(Backend *b, TInt r) { return r >= 0 && r <= 3; }

static void ScreenSize(TSize *out, Backend *b)
{
	out->w = (b->rotation & 1) ? b->h : b->w;
	out->h = (b->rotation & 1) ? b->w : b->h;
}

static void SuContentUpdated(void *su, const TSurfaceId *id, TInt buffer, const void *region,
                             TRequestStatus *consumed, TRequestStatus *displayed, TUint32 *stamp,
                             TRequestStatus *dispX, TInt *count);

static void ContentUpdated(Backend *b, const TSurfaceId *id, TInt buffer, const void *region,
                           TRequestStatus *consumed, TRequestStatus *displayed, TUint32 *stamp,
                           TRequestStatus *dispX, TInt *count)
{
	SuContentUpdated((char *)b + 4, id, buffer, region, consumed, displayed, stamp, dispX, count);
}

static void SurfaceSize(TSize *out, Backend *b, const TSurfaceId *id)
{
	Surf *s = MapSurf(b, id);
	out->w = s ? s->info.iSize.w : 0;
	out->h = s ? s->info.iSize.h : 0;
}

static TInt Query3C(Backend *b) { return 0; }
static void Resume(Backend *b) { LOG(b, "GCEBE: resume"); }
static void Pause(Backend *b) { LOG(b, "GCEBE: pause"); }

static TInt RegisterSurface(Backend *b, const TSurfaceId *id)
{
	Surf *s = MapSurf(b, id);
	return s ? KErrNone : KErrArgument;
}

static TInt UnregisterSurface(Backend *b, const TSurfaceId *id)
{
	UnmapSurf(b, id);
	return KErrNone;
}

static void Stage(Backend *b, void *regions, Layer *l, const TSurfaceId *target, TInt sync)
{
	if (!IsNullId(target)) {
		if (b->offLogs < 20) {
			b->offLogs++;
			LOG(b, "GCEBE: screen %d stage for off-screen target %08x %08x (layer %08x, sync %d): not composed",
			    b->screen, target->w[0], target->w[3], (TUint32)l, sync);
		}
		return;                 /* off-screen targets: not composed yet */
	}
	if (l && b->nStaged < MAX_STAGE)
		b->staged[b->nStaged++] = l;
}

static void Commit(Backend *b, const TSurfaceId *target, TRequestStatus *done)
{
	if (!IsNullId(target) && b->offLogs < 40) {
		b->offLogs++;
		LOG(b, "GCEBE: screen %d commit to off-screen target %08x %08x (done %08x)", b->screen,
		    target->w[0], target->w[3], (TUint32)done);
	}
	if (IsNullId(target)) {
		for (int i = 0; i < b->nStaged; i++)
			b->scene[i] = b->staged[i];
		b->nScene = b->nStaged;
		b->nStaged = 0;
		/* gce stages each layer chain head; the layers of a chain follow through NextLayer */
		/* logged when the layer list changes (first 40 times), not every frame */
		Layer *all[MAX_STAGE];
		int nAll = SceneLayers(b, all, MAX_STAGE);
		TUint32 sig = nAll;
		for (int i = 0; i < nAll; i++) {
			TRect d;
			((LGetRect)LV(all[i], L_DST_RECT))(all[i], &d);
			sig = sig * 31 + (TUint32)all[i] + ((LSurf)LV(all[i], L_SURFACE))(all[i])->w[0];
			sig = sig * 31 + d.x0 + d.y0 * 7 + d.x1 * 13 + d.y1 * 17 + ((LInt)LV(all[i], L_ROTATION))(all[i]) * 19
			      + ((LInt)LV(all[i], L_ALPHA))(all[i]) * 23;
		}
		if (sig != b->sceneSig && b->sceneLogs < 150) {
			b->sceneSig = sig;
			b->sceneLogs++;
			LOG(b, "GCEBE: screen %d commit %d: %d staged", b->screen, b->commits, b->nScene);
			for (int i = 0; i < b->nScene; i++) {
				Layer *l = b->scene[i];
				for (int k = 0; l && k < 8; k++, l = ((LLayer)LV(l, L_NEXT))(l)) {
					TRect d; ((LGetRect)LV(l, L_DST_RECT))(l, &d);
					const TSurfaceId *id = ((LSurf)LV(l, L_SURFACE))(l);
					TRect sr = { 0, 0, 0, 0 };
					((LGetRect)LV(l, L_SRC_RECT))(l, &sr);
					LOG(b, "GCEBE:   [%d.%d] layer %08x z %d alpha %d null %d dst %d,%d-%d,%d src %d,%d-%d,%d rot %d surface %08x %08x",
					    i, k, (TUint32)l, ((LInt)LV(l, L_ZORDER))(l), ((LInt)LV(l, L_ALPHA))(l),
					    ((LInt)LV(l, L_NULL_SURFACE))(l), d.x0, d.y0, d.x1, d.y1, sr.x0, sr.y0, sr.x1, sr.y1,
					    ((LInt)LV(l, L_ROTATION))(l), id->w[0], id->w[3]);
				}
			}
		}
		for (int i = 0; i < b->nScene; i++)     /* surfaces registered before the window server mapped them */
			if (b->scene[i])
				MapSurf(b, ((LSurf)LV(b->scene[i], L_SURFACE))(b->scene[i]));
		if (!HostCompose(b))
			ComposeScene(b);
		b->commits++;
	}
	Complete(&done, KErrNone);
}

/* MDisplayControl, also reachable through primary slots 22..30 */
typedef struct { TSize px, tw; TUint32 flags, spare; } TResolution;
typedef struct { TInt iVersion; TUint32 iDefined; TSize iResolution; TInt iRotation; TSize iTwips; } TDisplayConfiguration1;

static TInt NumberOfResolutions(Backend *b) { return 1; }

static TInt GetResolutions(Backend *b, void *array)
{
	TResolution r = { { b->w, b->h }, { b->twW, b->twH }, 0xfu, 0 };      /* ERotation{Normal,90,180,270}Supported */
	RArray_Reset(array);
	return RArray_Append(array, &r);
}

static void GetConfiguration(Backend *b, TDisplayConfiguration1 *c)
{
	if (c->iVersion < (TInt)sizeof(TDisplayConfiguration1))
		return;
	c->iDefined = 7;                 /* EResolution, ERotation, EResolutionTwips */
	c->iResolution.w = (b->rotation & 1) ? b->h : b->w;
	c->iResolution.h = (b->rotation & 1) ? b->w : b->h;
	c->iRotation = b->rotation;
	c->iTwips.w = (b->rotation & 1) ? b->twH : b->twW;
	c->iTwips.h = (b->rotation & 1) ? b->twW : b->twH;
}

static TInt SetConfiguration(Backend *b, const TDisplayConfiguration1 *c)
{
	if ((c->iDefined & 2) && (c->iRotation < 0 || c->iRotation > 3))
		return KErrArgument;
	if (c->iDefined & 1) {           /* the one resolution, in either orientation */
		TInt rw = c->iResolution.w, rh = c->iResolution.h;
		if (!((rw == b->w && rh == b->h) || (rw == b->h && rh == b->w) || (rw == 0 && rh == 0)))
			return KErrArgument;
	}
	if ((c->iDefined & 2) && (c->iRotation & 3) != b->rotation)
		LOG(b, "GCEBE: configuration rotation %d (%dx%d)", c->iRotation, c->iResolution.w, c->iResolution.h);
	if (c->iDefined & 2)
		b->rotation = c->iRotation & 3;
	return KErrNone;
}

static TInt PreferredVersion(Backend *b) { return 1; }

static void Arm(TRequestStatus **slot, TRequestStatus *s)
{
	s->iStatus = KRequestPending;
	s->iFlags |= 1;                  /* TRequestStatus::EActive */
	*slot = s;
}

static void NotifyDisplay(Backend *b, TRequestStatus *s) { Arm(&b->dispChange, s); }
static void NotifyDisplayCancel(Backend *b) { Complete(&b->dispChange, KErrCancel); }
static void NotifyConfig(Backend *b, TRequestStatus *s) { Arm(&b->cfgChange, s); }
static void NotifyConfigCancel(Backend *b) { Complete(&b->cfgChange, KErrCancel); }
static void ApplyScreenConfig(Backend *b) { }
static void WaitScreenEnable(Backend *b, TRequestStatus *s) { Complete(&s, KErrNotSupported); }

static const void *const KPrimary[33] = {
	Dtor, DtorDelete, Extension, Release, ApiVersion, InternalVersion, AttachLayer, DetachLayer,
	ResizeLayerRegions, SetScreenRotation, UseDirtyRects, SupportsRotation, ScreenSize, ContentUpdated,
	SurfaceSize, Query3C, Resume, Pause, RegisterSurface, UnregisterSurface, Stage, Commit,
	NumberOfResolutions, GetResolutions, GetConfiguration, SetConfiguration, PreferredVersion,
	NotifyConfig, NotifyConfigCancel, NotifyDisplay, NotifyDisplayCancel, ApplyScreenConfig, WaitScreenEnable,
};

/* ---- MCompositionSurfaceUpdate (object + 4) ---- */
static TInt SuApiVersion(void *su) { return 1; }
static TUint32 SuInternalVersion(void *su) { return 1 | (8 << 8) | (1 << 16); }

static void SuContentUpdated(void *su, const TSurfaceId *id, TInt buffer, const void *region,
                             TRequestStatus *consumed, TRequestStatus *displayed, TUint32 *stamp,
                             TRequestStatus *dispX, TInt *count)
{
	Backend *b = FromSU(su);
	Surf *s = FindSurf(b, id);
	if (b->updates++ < 8)
		LOG(b, "GCEBE: content updated %08x %08x buffer %d known %d", id->w[0], id->w[3], buffer, s != 0);
	if (s) {
		s->current = buffer;
		s->dirty = 1;
		s->updated = 1;
		int shown = 0;
		for (int i = 0; i < b->nScene; i++)
			if (b->scene[i] && SameId(((LSurf)LV(b->scene[i], L_SURFACE))(b->scene[i]), id))
				shown = 1;
		if (shown && !HostCompose(b))           /* re-show the scene layers that use it */
			for (int i = 0; i < b->nScene; i++)
				if (b->scene[i] && SameId(((LSurf)LV(b->scene[i], L_SURFACE))(b->scene[i]), id))
					Blit(b, b->scene[i]);
	}
	Complete(&consumed, KErrNone);
	if (stamp)
		*stamp = User_FastCounter();
	Complete(&displayed, KErrNone);
	Complete(&dispX, KErrNone);
}

static TInt SuExtension(void *su, TUint32 uid, void **out, void *arg)
{
	return Extension(FromSU(su), uid, out, arg);
}

static const void *const KSurfaceUpdate[4] = { SuApiVersion, SuInternalVersion, SuContentUpdated, SuExtension };

/* ---- MDisplayControl (object + 0x5C) ---- */
static TInt DcNumber(void *d) { return NumberOfResolutions(FromDC(d)); }
static TInt DcGet(void *d, void *a) { return GetResolutions(FromDC(d), a); }
static void DcGetCfg(void *d, TDisplayConfiguration1 *c) { GetConfiguration(FromDC(d), c); }
static TInt DcSetCfg(void *d, const TDisplayConfiguration1 *c) { return SetConfiguration(FromDC(d), c); }
static TInt DcPreferred(void *d) { return 1; }
static void DcNotifyDisplay(void *d, TRequestStatus *s) { NotifyDisplay(FromDC(d), s); }
static void DcNotifyDisplayCancel(void *d) { NotifyDisplayCancel(FromDC(d)); }
static void DcNotifyConfig(void *d, TRequestStatus *s) { NotifyConfig(FromDC(d), s); }
static void DcNotifyConfigCancel(void *d) { NotifyConfigCancel(FromDC(d)); }

static const void *const KDisplay[9] = {
	DcNumber, DcGet, DcGetCfg, DcSetCfg, DcPreferred, DcNotifyDisplay, DcNotifyDisplayCancel,
	DcNotifyConfig, DcNotifyConfigCancel,
};

/* ---- UID 0x2002E6D5 (&b->vtSurfaceNotify): one-shot notification per surface (Belle FP2) ----
   [V] gcerenderstage resolves the UID on the next stage (gce, then this backend) and forwards two slots:
   slot 0 (const TSurfaceId &, 1, 0, TRequestStatus &) and slot 1 (const TSurfaceId &). The caller is an
   alfcompositorrs active object (surface ID at +0x1C, iStatus at +4): it requests, SetActive()s, cancels
   with slot 1, and its RunL calls its observer once with the surface on KErrNone, without asking again.
   [I] "surface ready" (arguments 1 and 0 unknown): our surfaces are ready once registered, so the
   request completes at once in the calling thread. Not a coordinate mapping: 2026-11-09's identity
   copy wrote the surface ID over the active object's iStatus and queue link (Wserv died in
   TDblQueBase::DoAddPriority). */
static Backend *FromSN(void *p) { return (Backend *)((char *)p - __builtin_offsetof(Backend, vtSurfaceNotify)); }
static void SnRequest(void *d, const TSurfaceId *id, TInt a2, TInt a3, TRequestStatus *status)
{
	Backend *b = FromSN(d);
	if (b->notifyLogs++ < 6)
		LOG(b, "GCEBE: surface notify %08x %08x (%d, %d): completed", id ? id->w[0] : 0, id ? id->w[3] : 0, a2, a3);
	Complete(&status, KErrNone);
}
static void SnCancel(void *d, const TSurfaceId *id) { (void)d; (void)id; }   /* already completed */
static TInt SnNone(void *d) { (void)d; return KErrNotSupported; }
static const void *const KSurfaceNotify[4] = { SnRequest, SnCancel, SnNone, SnNone };

/* ---- factory: compositionbackend.dll ordinal 1 (gce's import thunk is rebound here) ---- */
__attribute__((section(".text.entry")))
Backend *gcebe_create(TInt screen)
{
	Backend *b = (Backend *)User_AllocZ(sizeof(Backend));
	if (!b)
		return 0;       /* gce's ConstructL dereferences it: a leave would be cleaner [inferred] */
	b->vt = KPrimary;
	b->vtSurfaceUpdate = KSurfaceUpdate;
	b->vtDisplay = KDisplay;
	b->vtSurfaceNotify = KSurfaceNotify;
	b->screen = screen;
	TInt v = 0;
	b->w = HAL_Get(EDisplayXPixels, &v) == KErrNone ? v : 360;
	b->h = HAL_Get(EDisplayYPixels, &v) == KErrNone ? v : 640;
	b->twW = HAL_Get(EDisplayXTwips, &v) == KErrNone ? v : 0;
	b->twH = HAL_Get(EDisplayYTwips, &v) == KErrNone ? v : 0;
	TInt addr = 0, off = 0;
	HAL_Get(EDisplayMemoryAddress, &addr);
	HAL_Get(EDisplayOffsetToFirstPixel, &off);
	/* screen 0 is the panel; the others (TV-out) have no display memory of their own yet */
	b->fb = (addr && screen == 0) ? (unsigned char *)addr + off : 0;
	b->fbStride = HAL_Get(EDisplayOffsetBetweenLines, &v) == KErrNone ? v : b->w * 4;
	b->fbBpp = HAL_Get(EDisplayBitsPerPixel, &v) == KErrNone ? v : 32;
	SM_Ctor(b->sm);
	TInt r = SM_Open(b->sm);
	b->smOpen = r == KErrNone;
	TInt rs = SUS_Start(&b->provider);
	TInt rr = -1;
	if ((rs == KErrNone || rs == KErrAlreadyExists) && b->provider) {
		typedef TInt (*Register)(void *self, TInt screen, void *receiver, TInt priority);
		rr = ((Register)(*(void ***)b->provider)[0])(b->provider, screen, b, 10 - screen);
	}
	LOG(b, "GCEBE: screen %d %dx%d twips %dx%d fb %08x stride %d bpp %d; surface manager %d, update server %d register %d",
	    screen, b->w, b->h, b->twW, b->twH, (TUint32)b->fb, b->fbStride, b->fbBpp, r, rs, rr);
	return b;
}
