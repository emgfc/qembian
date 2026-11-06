/* sygl GL ES 1.1 front end, milestone 1: state, matrix stacks, client vertex/colour arrays,
   transform, frustum clipping, culling, primitive assembly; triangles go to raster.c.
   Every entry point is sy_<glName> (the libgles_cm ordinal table branches here, gen/entry.S).
   Not yet (milestone 2+): lighting, textures, blending, alpha test, fog, scissor, lines/points
   beyond the basics, buffer objects. Unimplemented entries stay logged stubs. */
#include "sygl_int.h"

typedef float GLfloat;
typedef float GLclampf;
typedef int GLfixed;
typedef int GLint;
typedef int GLsizei;
typedef unsigned GLenum;
typedef unsigned GLbitfield;
typedef unsigned char GLubyte;
typedef unsigned char GLboolean;

#define GL_NO_ERROR 0
#define GL_INVALID_ENUM 0x0500
#define GL_INVALID_VALUE 0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_STACK_OVERFLOW 0x0503
#define GL_STACK_UNDERFLOW 0x0504
#define GL_OUT_OF_MEMORY 0x0505
#define GL_POINTS 0
#define GL_LINES 1
#define GL_LINE_LOOP 2
#define GL_LINE_STRIP 3
#define GL_TRIANGLES 4
#define GL_TRIANGLE_STRIP 5
#define GL_TRIANGLE_FAN 6
#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_FLOAT 0x1406
#define GL_FIXED 0x140C
#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GL_TEXTURE 0x1702
#define GL_FLAT 0x1D00
#define GL_SMOOTH 0x1D01
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_VERTEX_ARRAY 0x8074
#define GL_NORMAL_ARRAY 0x8075
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078
#define GL_POINT_SIZE_ARRAY_OES 0x8B9C
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_LESS 0x0201
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_MODELVIEW_STACK_DEPTH 0x0D36
#define GL_MAX_PROJECTION_STACK_DEPTH 0x0D38
#define GL_MAX_TEXTURE_STACK_DEPTH 0x0D39
#define GL_VIEWPORT 0x0BA2
#define GL_MAX_VIEWPORT_DIMS 0x0D3A
#define GL_MAX_LIGHTS 0x0D31
#define GL_MAX_TEXTURE_UNITS 0x84E2
#define GL_RED_BITS 0x0D52
#define GL_GREEN_BITS 0x0D53
#define GL_BLUE_BITS 0x0D54
#define GL_ALPHA_BITS 0x0D55
#define GL_DEPTH_BITS 0x0D56
#define GL_STENCIL_BITS 0x0D57

#define MV_DEPTH 16
#define PROJ_DEPTH 4
#define TEX_DEPTH 4

typedef struct { int size; GLenum type; int stride; const void* ptr; int enabled; } SyArray;
typedef struct { float x, y, z, w, r, g, b, a; } SyCV;		/* clip-space vertex + colour 0..1 */

typedef struct SyContext
	{
	u32 magic;
	int error;
	GLenum matrixMode;
	float mv[MV_DEPTH][16]; int mvTop;
	float proj[PROJ_DEPTH][16]; int projTop;
	float tex[TEX_DEPTH][16]; int texTop;
	int vpX, vpY, vpW, vpH, viewportSet;
	float depthNear, depthFar;
	float clear[4]; float clearDepth;
	GLenum shadeModel;
	int depthTest, depthWrite; GLenum depthFunc;
	int cullFace; GLenum cullMode, frontFace;
	float color[4];
	SyArray va, ca, na, ta;
	SySurface* draw;
	SyCV* vbuf; int vcap;		/* transformed vertices of the current draw call */
	} SyContext;

/* ---------------------------------------------------------------- helpers */

static SyContext* cur(void)
	{
	SyThread* t = sy_thread_peek();
	return t ? t->ctx : 0;
	}

static void seterr(SyContext* c, int e)
	{
	if (c && c->error == GL_NO_ERROR)
		c->error = e;
	}

static float fx(GLfixed v) { return (float)v * (1.0f / 65536.0f); }

static void mat_identity(float* m)
	{
	for (int i = 0; i < 16; ++i) m[i] = (i % 5) ? 0.0f : 1.0f;
	}

/* r = a * b (column-major) */
static void mat_mul(float* r, const float* a, const float* b)
	{
	float t[16];
	for (int c = 0; c < 4; ++c)
		for (int rr = 0; rr < 4; ++rr)
			t[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] +
							a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
	memcpy(r, t, sizeof(t));
	}

static float* cur_matrix(SyContext* c)
	{
	switch (c->matrixMode)
		{
	case GL_PROJECTION: return c->proj[c->projTop];
	case GL_TEXTURE: return c->tex[c->texTop];
	default: return c->mv[c->mvTop];
		}
	}

static void mult_cur(SyContext* c, const float* m)
	{
	float* t = cur_matrix(c);
	mat_mul(t, t, m);
	}

/* ---------------------------------------------------------------- context */

struct SyContext* sy_gl_create(void)
	{
	SyContext* c = (SyContext*)sy_allocz(sizeof(SyContext));
	if (!c)
		return 0;
	c->magic = SY_MAGIC_CONTEXT;
	c->matrixMode = GL_MODELVIEW;
	mat_identity(c->mv[0]); mat_identity(c->proj[0]); mat_identity(c->tex[0]);
	c->depthNear = 0.0f; c->depthFar = 1.0f;
	c->clearDepth = 1.0f;
	c->shadeModel = GL_SMOOTH;
	c->depthWrite = 1; c->depthFunc = GL_LESS;
	c->cullMode = GL_BACK; c->frontFace = GL_CCW;
	c->color[0] = c->color[1] = c->color[2] = c->color[3] = 1.0f;
	c->va.size = 4; c->va.type = GL_FLOAT;
	c->ca.size = 4; c->ca.type = GL_FLOAT;
	c->ta.size = 4; c->ta.type = GL_FLOAT;
	c->na.size = 3; c->na.type = GL_FLOAT;
	return c;
	}

void sy_gl_destroy(struct SyContext* c)
	{
	if (!c) return;
	sy_free(c->vbuf);
	c->magic = 0;
	sy_free(c);
	}

void sy_gl_made_current(struct SyContext* c, SySurface* aDraw)
	{
	c->draw = aDraw;
	if (!c->viewportSet && aDraw)
		{
		c->vpX = 0; c->vpY = 0; c->vpW = aDraw->w; c->vpH = aDraw->h;
		c->viewportSet = 1;
		}
	}

/* ---------------------------------------------------------------- state entry points */

GLenum sy_glGetError(void)
	{
	SyContext* c = cur();
	if (!c) return GL_NO_ERROR;
	int e = c->error;
	c->error = GL_NO_ERROR;
	return (GLenum)e;
	}

void sy_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
	{
	SyContext* c = cur(); if (!c) return;
	if (w < 0 || h < 0) { seterr(c, GL_INVALID_VALUE); return; }
	c->vpX = x; c->vpY = y; c->vpW = w; c->vpH = h; c->viewportSet = 1;
	}

void sy_glDepthRangef(GLclampf n, GLclampf f)
	{
	SyContext* c = cur(); if (!c) return;
	c->depthNear = n < 0 ? 0 : (n > 1 ? 1 : n);
	c->depthFar = f < 0 ? 0 : (f > 1 ? 1 : f);
	}
void sy_glDepthRangex(GLfixed n, GLfixed f) { sy_glDepthRangef(fx(n), fx(f)); }

void sy_glMatrixMode(GLenum m)
	{
	SyContext* c = cur(); if (!c) return;
	if (m != GL_MODELVIEW && m != GL_PROJECTION && m != GL_TEXTURE) { seterr(c, GL_INVALID_ENUM); return; }
	c->matrixMode = m;
	}

void sy_glLoadIdentity(void) { SyContext* c = cur(); if (c) mat_identity(cur_matrix(c)); }
void sy_glLoadMatrixf(const GLfloat* m) { SyContext* c = cur(); if (c && m) memcpy(cur_matrix(c), m, 64); }
void sy_glLoadMatrixx(const GLfixed* m)
	{
	SyContext* c = cur(); if (!c || !m) return;
	float* t = cur_matrix(c);
	for (int i = 0; i < 16; ++i) t[i] = fx(m[i]);
	}
void sy_glMultMatrixf(const GLfloat* m) { SyContext* c = cur(); if (c && m) mult_cur(c, m); }
void sy_glMultMatrixx(const GLfixed* m)
	{
	SyContext* c = cur(); if (!c || !m) return;
	float t[16];
	for (int i = 0; i < 16; ++i) t[i] = fx(m[i]);
	mult_cur(c, t);
	}

void sy_glPushMatrix(void)
	{
	SyContext* c = cur(); if (!c) return;
	int* top; int max; float (*st)[16];
	switch (c->matrixMode)
		{
	case GL_PROJECTION: top = &c->projTop; max = PROJ_DEPTH; st = c->proj; break;
	case GL_TEXTURE: top = &c->texTop; max = TEX_DEPTH; st = c->tex; break;
	default: top = &c->mvTop; max = MV_DEPTH; st = c->mv; break;
		}
	if (*top + 1 >= max) { seterr(c, GL_STACK_OVERFLOW); return; }
	memcpy(st[*top + 1], st[*top], 64);
	++*top;
	}

void sy_glPopMatrix(void)
	{
	SyContext* c = cur(); if (!c) return;
	int* top = c->matrixMode == GL_PROJECTION ? &c->projTop : c->matrixMode == GL_TEXTURE ? &c->texTop : &c->mvTop;
	if (*top == 0) { seterr(c, GL_STACK_UNDERFLOW); return; }
	--*top;
	}

void sy_glTranslatef(GLfloat x, GLfloat y, GLfloat z)
	{
	SyContext* c = cur(); if (!c) return;
	float m[16]; mat_identity(m);
	m[12] = x; m[13] = y; m[14] = z;
	mult_cur(c, m);
	}
void sy_glTranslatex(GLfixed x, GLfixed y, GLfixed z) { sy_glTranslatef(fx(x), fx(y), fx(z)); }

void sy_glScalef(GLfloat x, GLfloat y, GLfloat z)
	{
	SyContext* c = cur(); if (!c) return;
	float m[16]; mat_identity(m);
	m[0] = x; m[5] = y; m[10] = z;
	mult_cur(c, m);
	}
void sy_glScalex(GLfixed x, GLfixed y, GLfixed z) { sy_glScalef(fx(x), fx(y), fx(z)); }

void sy_glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
	{
	SyContext* c = cur(); if (!c) return;
	float len = sy_sqrtf(x * x + y * y + z * z);
	if (len == 0.0f) return;
	x /= len; y /= len; z /= len;
	float a = angle * (3.14159265f / 180.0f);
	float s = sy_sinf(a), co = sy_cosf(a), ic = 1.0f - co;
	float m[16];
	m[0] = x * x * ic + co;     m[4] = x * y * ic - z * s;  m[8] = x * z * ic + y * s;  m[12] = 0;
	m[1] = y * x * ic + z * s;  m[5] = y * y * ic + co;     m[9] = y * z * ic - x * s;  m[13] = 0;
	m[2] = x * z * ic - y * s;  m[6] = y * z * ic + x * s;  m[10] = z * z * ic + co;    m[14] = 0;
	m[3] = 0; m[7] = 0; m[11] = 0; m[15] = 1;
	mult_cur(c, m);
	}
void sy_glRotatex(GLfixed a, GLfixed x, GLfixed y, GLfixed z) { sy_glRotatef(fx(a), fx(x), fx(y), fx(z)); }

void sy_glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)
	{
	SyContext* c = cur(); if (!c) return;
	if (n <= 0 || f <= 0 || l == r || b == t || n == f) { seterr(c, GL_INVALID_VALUE); return; }
	float m[16];
	for (int i = 0; i < 16; ++i) m[i] = 0;
	m[0] = 2 * n / (r - l);
	m[5] = 2 * n / (t - b);
	m[8] = (r + l) / (r - l);
	m[9] = (t + b) / (t - b);
	m[10] = -(f + n) / (f - n);
	m[11] = -1;
	m[14] = -2 * f * n / (f - n);
	mult_cur(c, m);
	}
void sy_glFrustumx(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f)
	{ sy_glFrustumf(fx(l), fx(r), fx(b), fx(t), fx(n), fx(f)); }

void sy_glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)
	{
	SyContext* c = cur(); if (!c) return;
	if (l == r || b == t || n == f) { seterr(c, GL_INVALID_VALUE); return; }
	float m[16]; mat_identity(m);
	m[0] = 2 / (r - l); m[5] = 2 / (t - b); m[10] = -2 / (f - n);
	m[12] = -(r + l) / (r - l); m[13] = -(t + b) / (t - b); m[14] = -(f + n) / (f - n);
	mult_cur(c, m);
	}
void sy_glOrthox(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f)
	{ sy_glOrthof(fx(l), fx(r), fx(b), fx(t), fx(n), fx(f)); }

void sy_glShadeModel(GLenum m)
	{
	SyContext* c = cur(); if (!c) return;
	if (m != GL_FLAT && m != GL_SMOOTH) { seterr(c, GL_INVALID_ENUM); return; }
	c->shadeModel = m;
	}

void sy_glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
	{
	SyContext* c = cur(); if (!c) return;
	c->clear[0] = r; c->clear[1] = g; c->clear[2] = b; c->clear[3] = a;
	}
void sy_glClearColorx(GLfixed r, GLfixed g, GLfixed b, GLfixed a) { sy_glClearColor(fx(r), fx(g), fx(b), fx(a)); }
void sy_glClearDepthf(GLclampf d) { SyContext* c = cur(); if (c) c->clearDepth = d < 0 ? 0 : (d > 1 ? 1 : d); }
void sy_glClearDepthx(GLfixed d) { sy_glClearDepthf(fx(d)); }
void sy_glClearStencil(GLint s) { (void)s; }

static u32 pack_rgb(const float* c)
	{
	int r = (int)(c[0] * 255.0f + 0.5f), g = (int)(c[1] * 255.0f + 0.5f), b = (int)(c[2] * 255.0f + 0.5f);
	r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
	return ((u32)r << 16) | ((u32)g << 8) | (u32)b;
	}

void sy_glClear(GLbitfield mask)
	{
	SyContext* c = cur(); if (!c || !c->draw) return;
	if (mask & ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) { seterr(c, GL_INVALID_VALUE); return; }
	float d = c->clearDepth * 65535.0f + 0.5f;
	sy_raster_clear(c->draw, (mask & GL_COLOR_BUFFER_BIT) != 0, pack_rgb(c->clear),
					(mask & GL_DEPTH_BUFFER_BIT) && c->depthWrite, (u16)(int)d, 0, 0, c->draw->w, c->draw->h);
	}

static void set_cap(GLenum cap, int on)
	{
	SyContext* c = cur(); if (!c) return;
	switch (cap)
		{
	case GL_DEPTH_TEST: c->depthTest = on; break;
	case GL_CULL_FACE: c->cullFace = on; break;
	default: break;		/* accepted, not implemented yet (milestone 2: lighting, textures, blend ...) */
		}
	}
void sy_glEnable(GLenum cap) { set_cap(cap, 1); }
void sy_glDisable(GLenum cap) { set_cap(cap, 0); }

void sy_glDepthFunc(GLenum f) { SyContext* c = cur(); if (c) c->depthFunc = f; }
void sy_glDepthMask(GLboolean m) { SyContext* c = cur(); if (c) c->depthWrite = m != 0; }
void sy_glCullFace(GLenum m) { SyContext* c = cur(); if (c) c->cullMode = m; }
void sy_glFrontFace(GLenum m) { SyContext* c = cur(); if (c) c->frontFace = m; }
void sy_glFlush(void) {}
void sy_glFinish(void) {}
void sy_glHint(GLenum t, GLenum m) { (void)t; (void)m; }

void sy_glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
	{
	SyContext* c = cur(); if (!c) return;
	c->color[0] = r; c->color[1] = g; c->color[2] = b; c->color[3] = a;
	}
void sy_glColor4x(GLfixed r, GLfixed g, GLfixed b, GLfixed a) { sy_glColor4f(fx(r), fx(g), fx(b), fx(a)); }
void sy_glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a)
	{ sy_glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }

static SyArray* array_of(SyContext* c, GLenum a)
	{
	switch (a)
		{
	case GL_VERTEX_ARRAY: return &c->va;
	case GL_COLOR_ARRAY: return &c->ca;
	case GL_NORMAL_ARRAY: return &c->na;
	case GL_TEXTURE_COORD_ARRAY: return &c->ta;
	default: return 0;
		}
	}
void sy_glEnableClientState(GLenum a)
	{
	SyContext* c = cur(); if (!c) return;
	SyArray* p = array_of(c, a);
	if (p) p->enabled = 1; else if (a != GL_POINT_SIZE_ARRAY_OES) seterr(c, GL_INVALID_ENUM);
	}
void sy_glDisableClientState(GLenum a)
	{
	SyContext* c = cur(); if (!c) return;
	SyArray* p = array_of(c, a);
	if (p) p->enabled = 0; else if (a != GL_POINT_SIZE_ARRAY_OES) seterr(c, GL_INVALID_ENUM);
	}

static void set_array(SyArray* a, GLint size, GLenum type, GLsizei stride, const void* ptr)
	{
	a->size = size; a->type = type; a->stride = stride; a->ptr = ptr;
	}
void sy_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void* ptr)
	{
	SyContext* c = cur(); if (!c) return;
	if (size < 2 || size > 4 || stride < 0) { seterr(c, GL_INVALID_VALUE); return; }
	if (type != GL_BYTE && type != GL_SHORT && type != GL_FIXED && type != GL_FLOAT) { seterr(c, GL_INVALID_ENUM); return; }
	set_array(&c->va, size, type, stride, ptr);
	}
void sy_glColorPointer(GLint size, GLenum type, GLsizei stride, const void* ptr)
	{
	SyContext* c = cur(); if (!c) return;
	if (size != 4 || stride < 0) { seterr(c, GL_INVALID_VALUE); return; }
	if (type != GL_UNSIGNED_BYTE && type != GL_FIXED && type != GL_FLOAT) { seterr(c, GL_INVALID_ENUM); return; }
	set_array(&c->ca, size, type, stride, ptr);
	}
void sy_glNormalPointer(GLenum type, GLsizei stride, const void* ptr)
	{
	SyContext* c = cur(); if (!c) return;
	set_array(&c->na, 3, type, stride, ptr);
	}
void sy_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void* ptr)
	{
	SyContext* c = cur(); if (!c) return;
	set_array(&c->ta, size, type, stride, ptr);
	}

/* ---------------------------------------------------------------- queries */

const GLubyte* sy_glGetString(GLenum name)
	{
	switch (name)
		{
	case GL_VENDOR: return (const GLubyte*)"syborg-pg";
	case GL_RENDERER: return (const GLubyte*)"sygl software rasterizer";
	case GL_VERSION: return (const GLubyte*)"OpenGL ES-CM 1.1";
	case GL_EXTENSIONS: return (const GLubyte*)"";
	default: seterr(cur(), GL_INVALID_ENUM); return 0;
		}
	}

void sy_glGetIntegerv(GLenum name, GLint* v)
	{
	SyContext* c = cur(); if (!c || !v) return;
	switch (name)
		{
	case GL_VIEWPORT: v[0] = c->vpX; v[1] = c->vpY; v[2] = c->vpW; v[3] = c->vpH; break;
	case GL_MAX_VIEWPORT_DIMS: v[0] = 1024; v[1] = 1024; break;
	case GL_MAX_TEXTURE_SIZE: *v = 512; break;
	case GL_MAX_MODELVIEW_STACK_DEPTH: *v = MV_DEPTH; break;
	case GL_MAX_PROJECTION_STACK_DEPTH: *v = PROJ_DEPTH; break;
	case GL_MAX_TEXTURE_STACK_DEPTH: *v = TEX_DEPTH; break;
	case GL_MAX_LIGHTS: *v = 8; break;
	case GL_MAX_TEXTURE_UNITS: *v = 2; break;
	case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: *v = 8; break;
	case GL_ALPHA_BITS: case GL_STENCIL_BITS: *v = 0; break;
	case GL_DEPTH_BITS: *v = 16; break;
	default: seterr(c, GL_INVALID_ENUM); break;
		}
	}

/* ---------------------------------------------------------------- vertex pipeline */

static const u8* elem(const SyArray* a, int i, int bytesPer)
	{
	int stride = a->stride ? a->stride : a->size * bytesPer;
	return (const u8*)a->ptr + i * stride;
	}

static int type_bytes(GLenum t)
	{
	switch (t) { case GL_BYTE: case GL_UNSIGNED_BYTE: return 1; case GL_SHORT: case GL_UNSIGNED_SHORT: return 2; default: return 4; }
	}

static float comp(GLenum type, const u8* p, int k, int normalized)
	{
	switch (type)
		{
	case GL_FLOAT: return ((const float*)p)[k];
	case GL_FIXED: return fx(((const int*)p)[k]);
	case GL_SHORT: return (float)((const short*)p)[k];
	case GL_BYTE: return (float)((const signed char*)p)[k];
	case GL_UNSIGNED_BYTE: return normalized ? ((const u8*)p)[k] / 255.0f : (float)((const u8*)p)[k];
	default: return 0.0f;
		}
	}

static void transform(SyContext* c, const float* mvp, int idx, SyCV* o)
	{
	float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	const u8* p = elem(&c->va, idx, type_bytes(c->va.type));
	for (int k = 0; k < c->va.size; ++k)
		v[k] = comp(c->va.type, p, k, 0);
	o->x = mvp[0] * v[0] + mvp[4] * v[1] + mvp[8] * v[2] + mvp[12] * v[3];
	o->y = mvp[1] * v[0] + mvp[5] * v[1] + mvp[9] * v[2] + mvp[13] * v[3];
	o->z = mvp[2] * v[0] + mvp[6] * v[1] + mvp[10] * v[2] + mvp[14] * v[3];
	o->w = mvp[3] * v[0] + mvp[7] * v[1] + mvp[11] * v[2] + mvp[15] * v[3];
	if (c->ca.enabled && c->ca.ptr)
		{
		const u8* q = elem(&c->ca, idx, type_bytes(c->ca.type));
		o->r = comp(c->ca.type, q, 0, 1); o->g = comp(c->ca.type, q, 1, 1);
		o->b = comp(c->ca.type, q, 2, 1); o->a = comp(c->ca.type, q, 3, 1);
		}
	else
		{
		o->r = c->color[0]; o->g = c->color[1]; o->b = c->color[2]; o->a = c->color[3];
		}
	}

/* clip plane distance: planes 0..5 = w+x, w-x, w+y, w-y, w+z, w-z */
static float plane_dist(const SyCV* v, int p)
	{
	switch (p)
		{
	case 0: return v->w + v->x;
	case 1: return v->w - v->x;
	case 2: return v->w + v->y;
	case 3: return v->w - v->y;
	case 4: return v->w + v->z;
	default: return v->w - v->z;
		}
	}

static int outcode(const SyCV* v)
	{
	int o = 0;
	for (int p = 0; p < 6; ++p)
		if (plane_dist(v, p) < 0.0f) o |= 1 << p;
	return o;
	}

static void lerp(SyCV* o, const SyCV* a, const SyCV* b, float t)
	{
	const float* pa = (const float*)a; const float* pb = (const float*)b; float* po = (float*)o;
	for (int i = 0; i < 8; ++i) po[i] = pa[i] + (pb[i] - pa[i]) * t;
	}

#define MAXPOLY 12

static void project(SyContext* c, const SyCV* in, SyRVtx* o)
	{
	float iw = 1.0f / in->w;
	float nx = in->x * iw, ny = in->y * iw, nz = in->z * iw;
	float wx = (float)c->vpX + (nx + 1.0f) * 0.5f * (float)c->vpW;
	float wy = (float)c->vpY + (ny + 1.0f) * 0.5f * (float)c->vpH;
	o->x = wx;
	o->y = (float)c->draw->h - wy;		/* GL window y is bottom-up; surfaces are top-down */
	o->z = c->depthNear + (nz + 1.0f) * 0.5f * (c->depthFar - c->depthNear);
	o->r = in->r * 255.0f; o->g = in->g * 255.0f; o->b = in->b * 255.0f; o->a = in->a * 255.0f;
	}

static void draw_triangle(SyContext* c, const SyCV* a, const SyCV* b, const SyCV* cc, const SyCV* provoking)
	{
	SyCV poly[2][MAXPOLY];
	int n = 3, src = 0;
	poly[0][0] = *a; poly[0][1] = *b; poly[0][2] = *cc;
	int oa = outcode(a), ob = outcode(b), oc = outcode(cc);
	if (oa & ob & oc)
		return;					/* all outside one plane */
	if (oa | ob | oc)			/* Sutherland-Hodgman against the planes that are crossed */
		{
		for (int p = 0; p < 6 && n >= 3; ++p)
			{
			if (!((oa | ob | oc) & (1 << p)))
				continue;
			int m = 0;
			for (int i = 0; i < n; ++i)
				{
				const SyCV* u = &poly[src][i];
				const SyCV* v = &poly[src][(i + 1) % n];
				float du = plane_dist(u, p), dv = plane_dist(v, p);
				if (du >= 0.0f && m < MAXPOLY) poly[src ^ 1][m++] = *u;
				if ((du >= 0.0f) != (dv >= 0.0f) && m < MAXPOLY)
					lerp(&poly[src ^ 1][m++], u, v, du / (du - dv));
				}
			n = m;
			src ^= 1;
			}
		if (n < 3)
			return;
		}
	SyRVtx w[MAXPOLY];
	for (int i = 0; i < n; ++i)
		project(c, &poly[src][i], &w[i]);
	if (c->shadeModel == GL_FLAT)
		{
		SyRVtx pv; project(c, provoking, &pv);
		for (int i = 0; i < n; ++i) { w[i].r = pv.r; w[i].g = pv.g; w[i].b = pv.b; w[i].a = pv.a; }
		}
	if (c->cullFace)
		{
		/* orientation in GL window coordinates (y up): flip the sign of our y-down area */
		float area = -((w[1].x - w[0].x) * (w[2].y - w[0].y) - (w[2].x - w[0].x) * (w[1].y - w[0].y));
		int ccw = area > 0.0f;
		int front = c->frontFace == GL_CCW ? ccw : !ccw;
		if (c->cullMode == GL_FRONT_AND_BACK || (c->cullMode == GL_FRONT && front) || (c->cullMode == GL_BACK && !front))
			return;
		}
	SyRState st = { c->depthTest, c->depthWrite, (int)c->depthFunc };
	for (int i = 1; i + 1 < n; ++i)
		sy_raster_triangle(c->draw, &st, &w[0], &w[i], &w[i + 1]);
	}

static int prepare(SyContext* c, int count, float* mvp)
	{
	if (!c->draw || !c->va.enabled || !c->va.ptr)
		return 0;
	if (count > c->vcap)
		{
		SyCV* nb = (SyCV*)sy_alloc(count * (int)sizeof(SyCV));
		if (!nb) { seterr(c, GL_OUT_OF_MEMORY); return 0; }
		sy_free(c->vbuf);
		c->vbuf = nb; c->vcap = count;
		}
	mat_mul(mvp, c->proj[c->projTop], c->mv[c->mvTop]);
	return 1;
	}

static void assemble(SyContext* c, GLenum mode, const SyCV* v, int n)
	{
	switch (mode)
		{
	case GL_TRIANGLES:
		for (int i = 0; i + 2 < n; i += 3) draw_triangle(c, &v[i], &v[i + 1], &v[i + 2], &v[i + 2]);
		break;
	case GL_TRIANGLE_STRIP:
		for (int i = 0; i + 2 < n; ++i)
			if (i & 1) draw_triangle(c, &v[i + 1], &v[i], &v[i + 2], &v[i + 2]);
			else draw_triangle(c, &v[i], &v[i + 1], &v[i + 2], &v[i + 2]);
		break;
	case GL_TRIANGLE_FAN:
		for (int i = 1; i + 1 < n; ++i) draw_triangle(c, &v[0], &v[i], &v[i + 1], &v[i + 1]);
		break;
	default:
		break;	/* points and lines: milestone 2 */
		}
	}

void sy_glDrawArrays(GLenum mode, GLint first, GLsizei count)
	{
	SyContext* c = cur(); if (!c) return;
	if (count < 0 || first < 0) { seterr(c, GL_INVALID_VALUE); return; }
	if (mode > GL_TRIANGLE_FAN) { seterr(c, GL_INVALID_ENUM); return; }
	float mvp[16];
	if (count == 0 || !prepare(c, count, mvp)) return;
	for (int i = 0; i < count; ++i)
		transform(c, mvp, first + i, &c->vbuf[i]);
	assemble(c, mode, c->vbuf, count);
	}

void sy_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices)
	{
	SyContext* c = cur(); if (!c) return;
	if (count < 0) { seterr(c, GL_INVALID_VALUE); return; }
	if (mode > GL_TRIANGLE_FAN || (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT)) { seterr(c, GL_INVALID_ENUM); return; }
	float mvp[16];
	if (count == 0 || !indices || !prepare(c, count, mvp)) return;
	for (int i = 0; i < count; ++i)
		{
		int idx = type == GL_UNSIGNED_BYTE ? ((const u8*)indices)[i] : ((const u16*)indices)[i];
		transform(c, mvp, idx, &c->vbuf[i]);
		}
	assemble(c, mode, c->vbuf, count);
	}
