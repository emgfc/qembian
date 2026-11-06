/* sygl GL ES 1.1 front end: state, matrix stacks, client arrays, the vertex stage (transform,
   lighting via light.c, texture coordinates), frustum clipping, culling and primitive assembly;
   triangles go to raster.c with the fragment state built here. Textures: tex.c.
   Every entry point is sy_<glName> (the libgles_cm ordinal table branches here, gen/entry.S).
   Not yet: points/lines, fog, combine texture env, multitexture, buffer objects, mipmaps. */
#include "gl_int.h"

#define GL_POINTS 0
#define GL_TRIANGLES 4
#define GL_TRIANGLE_STRIP 5
#define GL_TRIANGLE_FAN 6
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
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_LESS 0x0201
#define GL_ALWAYS 0x0207
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
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_MATRIX_MODE 0x0BA0
#define GL_MODELVIEW_MATRIX 0x0BA6
#define GL_PROJECTION_MATRIX 0x0BA7
#define GL_TEXTURE_MATRIX 0x0BA8
#define GL_SCISSOR_BOX 0x0C10
#define GL_FOG 0x0B60
#define GL_DITHER 0x0BD0
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_FOG_DENSITY 0x0B62
#define GL_FOG_START 0x0B63
#define GL_FOG_END 0x0B64
#define GL_FOG_MODE 0x0B65
#define GL_FOG_COLOR 0x0B66
#define GL_EXP 0x0800
#define GL_EXP2 0x0801
#define GL_IMPLEMENTATION_COLOR_READ_TYPE_OES 0x8B9A
#define GL_IMPLEMENTATION_COLOR_READ_FORMAT_OES 0x8B9B

typedef struct SyCV { float x, y, z, w, r, g, b, a, s, t, fog; } SyCV;		/* clip-space vertex, fog 0..1 */
#define CV_FLOATS 11

/* ---------------------------------------------------------------- helpers */

SyContext* sy_cur(void)
	{
	SyThread* t = sy_thread_peek();
	return t ? t->ctx : 0;
	}

void sy_seterr(SyContext* c, int e)
	{
	if (!c) return;
	if (c->errLogged < 30)		/* the first errors, with the GL entry point's return address */
		{
		++c->errLogged;
		SYLOG("GL error 0x%x (caller %08x)", e, (u32)__builtin_return_address(0));
		}
	if (c->error == GL_NO_ERROR)
		c->error = e;
	}

static void mat_identity(float* m)
	{
	for (int i = 0; i < 16; ++i) m[i] = (i % 5) ? 0.0f : 1.0f;
	}

static int mat_is_identity(const float* m)
	{
	for (int i = 0; i < 16; ++i)
		if (m[i] != ((i % 5) ? 0.0f : 1.0f)) return 0;
	return 1;
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

/* inverse transpose of the upper-left 3x3 of a column-major 4x4: the normal matrix (row-major 3x3) */
static void normal_matrix(const float* m, float* n)
	{
	float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
	float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
	float det = a * A + b * B + c * C;
	float id = det != 0.0f ? 1.0f / det : 0.0f;
	/* inverse = adj / det; transpose(inverse) rows = cofactor rows */
	n[0] = A * id; n[1] = B * id; n[2] = C * id;
	n[3] = -(b * i - c * h) * id; n[4] = (a * i - c * g) * id; n[5] = -(a * h - b * g) * id;
	n[6] = (b * f - c * e) * id; n[7] = -(a * f - c * d) * id; n[8] = (a * e - b * d) * id;
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

struct SyContext* sy_gl_create(struct SyContext* aShareWith)
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
	c->texcoord[3] = 1.0f;
	c->va.size = 4; c->va.type = GL_FLOAT;
	c->ca.size = 4; c->ca.type = GL_FLOAT;
	c->ta.size = 4; c->ta.type = GL_FLOAT;
	c->na.size = 3; c->na.type = GL_FLOAT;
	c->blendSrc = GL_ONE; c->blendDst = GL_ZERO;
	c->alphaFunc = GL_ALWAYS;
	c->colorMask = 15;
	c->fogMode = GL_EXP; c->fogDensity = 1.0f; c->fogStart = 0.0f; c->fogEnd = 1.0f;
	c->lineWidth = 1.0f; c->pointSize = 1.0f;
	sy_light_init(c);
	if (sy_tex_init(c, aShareWith))
		{
		sy_free(c);
		return 0;
		}
	return c;
	}

void sy_gl_destroy(struct SyContext* c)
	{
	if (!c) return;
	sy_tex_free_all(c);
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
		c->scX = 0; c->scY = 0; c->scW = aDraw->w; c->scH = aDraw->h;
		c->viewportSet = 1;
		}
	}

/* ---------------------------------------------------------------- state entry points */

GLenum sy_glGetError(void)
	{
	SyContext* c = sy_cur();
	if (!c) return GL_NO_ERROR;
	int e = c->error;
	c->error = GL_NO_ERROR;
	return (GLenum)e;
	}

void sy_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
	{
	SyContext* c = sy_cur(); if (!c) return;
	if (w < 0 || h < 0) { sy_seterr(c, GL_INVALID_VALUE); return; }
	c->vpX = x; c->vpY = y; c->vpW = w; c->vpH = h; c->viewportSet = 1;
	}

void sy_glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
	{
	SyContext* c = sy_cur(); if (!c) return;
	if (w < 0 || h < 0) { sy_seterr(c, GL_INVALID_VALUE); return; }
	c->scX = x; c->scY = y; c->scW = w; c->scH = h;
	}

void sy_glDepthRangef(GLclampf n, GLclampf f)
	{
	SyContext* c = sy_cur(); if (!c) return;
	c->depthNear = n < 0 ? 0 : (n > 1 ? 1 : n);
	c->depthFar = f < 0 ? 0 : (f > 1 ? 1 : f);
	}
void sy_glDepthRangex(GLfixed n, GLfixed f) { sy_glDepthRangef(sy_fx(n), sy_fx(f)); }

void sy_glMatrixMode(GLenum m)
	{
	SyContext* c = sy_cur(); if (!c) return;
	if (m != GL_MODELVIEW && m != GL_PROJECTION && m != GL_TEXTURE) { sy_seterr(c, GL_INVALID_ENUM); return; }
	c->matrixMode = m;
	}

void sy_glLoadIdentity(void) { SyContext* c = sy_cur(); if (c) mat_identity(cur_matrix(c)); }
void sy_glLoadMatrixf(const GLfloat* m) { SyContext* c = sy_cur(); if (c && m) memcpy(cur_matrix(c), m, 64); }
void sy_glLoadMatrixx(const GLfixed* m)
	{
	SyContext* c = sy_cur(); if (!c || !m) return;
	float* t = cur_matrix(c);
	for (int i = 0; i < 16; ++i) t[i] = sy_fx(m[i]);
	}
void sy_glMultMatrixf(const GLfloat* m) { SyContext* c = sy_cur(); if (c && m) mult_cur(c, m); }
void sy_glMultMatrixx(const GLfixed* m)
	{
	SyContext* c = sy_cur(); if (!c || !m) return;
	float t[16];
	for (int i = 0; i < 16; ++i) t[i] = sy_fx(m[i]);
	mult_cur(c, t);
	}

void sy_glPushMatrix(void)
	{
	SyContext* c = sy_cur(); if (!c) return;
	int* top; int max; float (*st)[16];
	switch (c->matrixMode)
		{
	case GL_PROJECTION: top = &c->projTop; max = PROJ_DEPTH; st = c->proj; break;
	case GL_TEXTURE: top = &c->texTop; max = TEX_DEPTH; st = c->tex; break;
	default: top = &c->mvTop; max = MV_DEPTH; st = c->mv; break;
		}
	if (*top + 1 >= max) { sy_seterr(c, GL_STACK_OVERFLOW); return; }
	memcpy(st[*top + 1], st[*top], 64);
	++*top;
	}

void sy_glPopMatrix(void)
	{
	SyContext* c = sy_cur(); if (!c) return;
	int* top = c->matrixMode == GL_PROJECTION ? &c->projTop : c->matrixMode == GL_TEXTURE ? &c->texTop : &c->mvTop;
	if (*top == 0) { sy_seterr(c, GL_STACK_UNDERFLOW); return; }
	--*top;
	}

void sy_glTranslatef(GLfloat x, GLfloat y, GLfloat z)
	{
	SyContext* c = sy_cur(); if (!c) return;
	float m[16]; mat_identity(m);
	m[12] = x; m[13] = y; m[14] = z;
	mult_cur(c, m);
	}
void sy_glTranslatex(GLfixed x, GLfixed y, GLfixed z) { sy_glTranslatef(sy_fx(x), sy_fx(y), sy_fx(z)); }

void sy_glScalef(GLfloat x, GLfloat y, GLfloat z)
	{
	SyContext* c = sy_cur(); if (!c) return;
	float m[16]; mat_identity(m);
	m[0] = x; m[5] = y; m[10] = z;
	mult_cur(c, m);
	}
void sy_glScalex(GLfixed x, GLfixed y, GLfixed z) { sy_glScalef(sy_fx(x), sy_fx(y), sy_fx(z)); }

void sy_glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
	{
	SyContext* c = sy_cur(); if (!c) return;
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
void sy_glRotatex(GLfixed a, GLfixed x, GLfixed y, GLfixed z) { sy_glRotatef(sy_fx(a), sy_fx(x), sy_fx(y), sy_fx(z)); }

void sy_glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)
	{
	SyContext* c = sy_cur(); if (!c) return;
	if (n <= 0 || f <= 0 || l == r || b == t || n == f) { sy_seterr(c, GL_INVALID_VALUE); return; }
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
	{ sy_glFrustumf(sy_fx(l), sy_fx(r), sy_fx(b), sy_fx(t), sy_fx(n), sy_fx(f)); }

void sy_glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)
	{
	SyContext* c = sy_cur(); if (!c) return;
	if (l == r || b == t || n == f) { sy_seterr(c, GL_INVALID_VALUE); return; }
	float m[16]; mat_identity(m);
	m[0] = 2 / (r - l); m[5] = 2 / (t - b); m[10] = -2 / (f - n);
	m[12] = -(r + l) / (r - l); m[13] = -(t + b) / (t - b); m[14] = -(f + n) / (f - n);
	mult_cur(c, m);
	}
void sy_glOrthox(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f)
	{ sy_glOrthof(sy_fx(l), sy_fx(r), sy_fx(b), sy_fx(t), sy_fx(n), sy_fx(f)); }

void sy_glShadeModel(GLenum m)
	{
	SyContext* c = sy_cur(); if (!c) return;
	if (m != GL_FLAT && m != GL_SMOOTH) { sy_seterr(c, GL_INVALID_ENUM); return; }
	c->shadeModel = m;
	}

void sy_glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
	{
	SYTRACE(sy_thread_peek(), "glClearColor(%d,%d,%d,%d /255)", (u32)__builtin_return_address(0), (int)(r*255), (int)(g*255), (int)(b*255), (int)(a*255));
	SyContext* c = sy_cur(); if (!c) return;
	c->clear[0] = r; c->clear[1] = g; c->clear[2] = b; c->clear[3] = a;
	}
void sy_glClearColorx(GLfixed r, GLfixed g, GLfixed b, GLfixed a) { sy_glClearColor(sy_fx(r), sy_fx(g), sy_fx(b), sy_fx(a)); }
void sy_glClearDepthf(GLclampf d) { SyContext* c = sy_cur(); if (c) c->clearDepth = d < 0 ? 0 : (d > 1 ? 1 : d); }
void sy_glClearDepthx(GLfixed d) { sy_glClearDepthf(sy_fx(d)); }
void sy_glClearStencil(GLint s) { (void)s; }

static u32 pack_rgb(const float* c)
	{
	int r = (int)(c[0] * 255.0f + 0.5f), g = (int)(c[1] * 255.0f + 0.5f), b = (int)(c[2] * 255.0f + 0.5f);
	r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
	return ((u32)r << 16) | ((u32)g << 8) | (u32)b;
	}

void sy_glClear(GLbitfield mask)
	{
	SyContext* c = sy_cur(); if (!c || !c->draw) return;
	if (mask & ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) { sy_seterr(c, GL_INVALID_VALUE); return; }
	float d = c->clearDepth * 65535.0f + 0.5f;
	int x = 0, y = 0, w = c->draw->w, h = c->draw->h;
	if (c->scissor)
		{
		x = c->scX; w = c->scW; h = c->scH;
		y = c->draw->h - (c->scY + c->scH);		/* GL y up -> surface rows */
		}
	sy_raster_clear(c->draw, (mask & GL_COLOR_BUFFER_BIT) && c->colorMask, pack_rgb(c->clear),
					(mask & GL_DEPTH_BUFFER_BIT) && c->depthWrite, (u16)(int)d, x, y, w, h);
	}

static void set_cap(GLenum cap, int on)
	{
	SYTRACE(sy_thread_peek(), "gl%sable(0x%x)", (u32)__builtin_return_address(1), on ? "En" : "Dis", cap);
	SyContext* c = sy_cur(); if (!c) return;
	if (sy_light_cap(c, cap, on) || sy_tex_cap(c, cap, on))
		return;
	switch (cap)
		{
	case GL_DEPTH_TEST: c->depthTest = on; break;
	case GL_CULL_FACE: c->cullFace = on; break;
	case GL_BLEND: c->blend = on; break;
	case GL_ALPHA_TEST: c->alphaTest = on; break;
	case GL_SCISSOR_TEST: c->scissor = on; break;
	case GL_POLYGON_OFFSET_FILL: c->polyOffset = on; break;
	case GL_FOG: c->fog = on; break;
	default: break;		/* fog, dither, multisample, polygon offset ...: accepted, not implemented yet */
		}
	}
void sy_glEnable(GLenum cap) { set_cap(cap, 1); }
void sy_glDisable(GLenum cap) { set_cap(cap, 0); }

GLboolean sy_glIsEnabled(GLenum cap)
	{
	SyContext* c = sy_cur(); if (!c) return 0;
	if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + SY_MAX_LIGHTS) return (GLboolean)c->light[cap - GL_LIGHT0].enabled;
	switch (cap)
		{
	case GL_DEPTH_TEST: return (GLboolean)c->depthTest;
	case GL_CULL_FACE: return (GLboolean)c->cullFace;
	case GL_BLEND: return (GLboolean)c->blend;
	case GL_ALPHA_TEST: return (GLboolean)c->alphaTest;
	case GL_SCISSOR_TEST: return (GLboolean)c->scissor;
	case GL_LIGHTING: return (GLboolean)c->lighting;
	case GL_COLOR_MATERIAL: return (GLboolean)c->colorMaterial;
	case GL_NORMALIZE: return (GLboolean)c->normalize;
	case GL_RESCALE_NORMAL: return (GLboolean)c->rescaleNormal;
	case GL_TEXTURE_2D: return (GLboolean)c->texture2D;
	case GL_VERTEX_ARRAY: return (GLboolean)c->va.enabled;
	case GL_COLOR_ARRAY: return (GLboolean)c->ca.enabled;
	case GL_NORMAL_ARRAY: return (GLboolean)c->na.enabled;
	case GL_TEXTURE_COORD_ARRAY: return (GLboolean)c->ta.enabled;
	case GL_DITHER: return 1;
	default: return 0;
		}
	}

void sy_glPolygonOffset(GLfloat factor, GLfloat units)
	{
	SyContext* c = sy_cur(); if (!c) return;
	c->polyFactor = factor; c->polyUnits = units;
	}
void sy_glPolygonOffsetx(GLfixed factor, GLfixed units) { sy_glPolygonOffset(sy_fx(factor), sy_fx(units)); }

void sy_glFogfv(GLenum pname, const GLfloat* p)
	{
	SyContext* c = sy_cur(); if (!c || !p) return;
	switch (pname)
		{
	case GL_FOG_MODE: c->fogMode = (GLenum)(int)p[0]; break;
	case GL_FOG_DENSITY: c->fogDensity = p[0]; break;
	case GL_FOG_START: c->fogStart = p[0]; break;
	case GL_FOG_END: c->fogEnd = p[0]; break;
	case GL_FOG_COLOR: memcpy(c->fogColor, p, 16); break;
	default: sy_seterr(c, GL_INVALID_ENUM); break;
		}
	}
void sy_glFogf(GLenum pname, GLfloat v) { sy_glFogfv(pname, &v); }
void sy_glFogxv(GLenum pname, const GLfixed* p)
	{
	if (!p) return;
	GLfloat f[4];
	if (pname == GL_FOG_MODE) f[0] = (GLfloat)p[0];
	else for (int i = 0; i < (pname == GL_FOG_COLOR ? 4 : 1); ++i) f[i] = sy_fx(p[i]);
	sy_glFogfv(pname, f);
	}
void sy_glFogx(GLenum pname, GLfixed v) { sy_glFogxv(pname, &v); }

/* no stencil buffer in our configs (EGL_STENCIL_SIZE 0): GL passes the stencil test then; the state
   is accepted. Logic op, sample coverage: accepted, not applied. Lines/points: widths stored. */
void sy_glStencilFunc(GLenum f, GLint ref, GLuint mask) { (void)f; (void)ref; (void)mask; }
void sy_glStencilMask(GLuint m) { (void)m; }
void sy_glStencilOp(GLenum a, GLenum b, GLenum c2) { (void)a; (void)b; (void)c2; }
void sy_glLogicOp(GLenum op) { (void)op; }
void sy_glSampleCoverage(GLclampf v, GLboolean inv) { (void)v; (void)inv; }
void sy_glSampleCoveragex(GLfixed v, GLboolean inv) { (void)v; (void)inv; }
void sy_glLineWidth(GLfloat w) { SyContext* c = sy_cur(); if (c) c->lineWidth = w; }
void sy_glLineWidthx(GLfixed w) { sy_glLineWidth(sy_fx(w)); }
void sy_glPointSize(GLfloat s) { SyContext* c = sy_cur(); if (c) c->pointSize = s; }
void sy_glPointSizex(GLfixed s) { sy_glPointSize(sy_fx(s)); }

void sy_glDepthFunc(GLenum f) { SyContext* c = sy_cur(); if (c) c->depthFunc = f; }
void sy_glDepthMask(GLboolean m) { SyContext* c = sy_cur(); if (c) c->depthWrite = m != 0; }
void sy_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
	{
	SyContext* c = sy_cur(); if (!c) return;
	c->colorMask = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
	}
void sy_glCullFace(GLenum m) { SyContext* c = sy_cur(); if (c) c->cullMode = m; }
void sy_glFrontFace(GLenum m) { SyContext* c = sy_cur(); if (c) c->frontFace = m; }
void sy_glBlendFunc(GLenum s, GLenum d) {
	SYTRACE(sy_thread_peek(), "glBlendFunc(0x%x, 0x%x)", (u32)__builtin_return_address(0), s, d); SyContext* c = sy_cur(); if (c) { c->blendSrc = s; c->blendDst = d; } }
void sy_glAlphaFunc(GLenum f, GLclampf ref)
	{
	SyContext* c = sy_cur(); if (!c) return;
	c->alphaFunc = f; c->alphaRef = ref < 0 ? 0 : (ref > 1 ? 1 : ref);
	}
void sy_glAlphaFuncx(GLenum f, GLfixed ref) { sy_glAlphaFunc(f, sy_fx(ref)); }
void sy_glFlush(void) { sy_flush_current(); }
void sy_glFinish(void) { sy_flush_current(); }
void sy_glHint(GLenum t, GLenum m) { (void)t; (void)m; }

void sy_glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
	{
	SYTRACE(sy_thread_peek(), "glColor4f(%d,%d,%d,%d /255)", (u32)__builtin_return_address(0), (int)(r*255), (int)(g*255), (int)(b*255), (int)(a*255));
	SyContext* c = sy_cur(); if (!c) return;
	c->color[0] = r; c->color[1] = g; c->color[2] = b; c->color[3] = a;
	if (c->colorMaterial)
		{
		memcpy(c->mat.ambient, c->color, 16);
		memcpy(c->mat.diffuse, c->color, 16);
		}
	}
void sy_glColor4x(GLfixed r, GLfixed g, GLfixed b, GLfixed a) {
	SYTRACE(sy_thread_peek(), "glColor4x(%d,%d,%d,%d /65536)", (u32)__builtin_return_address(0), r, g, b, a); sy_glColor4f(sy_fx(r), sy_fx(g), sy_fx(b), sy_fx(a)); }
void sy_glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a)
	{
	SYTRACE(sy_thread_peek(), "glColor4ub(%d,%d,%d,%d)", (u32)__builtin_return_address(0), r, g, b, a); sy_glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }

void sy_glMultiTexCoord4f(GLenum unit, GLfloat s, GLfloat t, GLfloat r, GLfloat q)
	{
	SyContext* c = sy_cur(); if (!c) return;
	(void)unit;
	c->texcoord[0] = s; c->texcoord[1] = t; c->texcoord[2] = r; c->texcoord[3] = q;
	}
void sy_glMultiTexCoord4x(GLenum unit, GLfixed s, GLfixed t, GLfixed r, GLfixed q)
	{ sy_glMultiTexCoord4f(unit, sy_fx(s), sy_fx(t), sy_fx(r), sy_fx(q)); }

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
	SYTRACE(sy_thread_peek(), "glEnableClientState(0x%x)", (u32)__builtin_return_address(0), a);
	SyContext* c = sy_cur(); if (!c) return;
	SyArray* p = array_of(c, a);
	if (p) p->enabled = 1; else if (a != GL_POINT_SIZE_ARRAY_OES) sy_seterr(c, GL_INVALID_ENUM);
	}
void sy_glDisableClientState(GLenum a)
	{
	SyContext* c = sy_cur(); if (!c) return;
	SyArray* p = array_of(c, a);
	if (p) p->enabled = 0; else if (a != GL_POINT_SIZE_ARRAY_OES) sy_seterr(c, GL_INVALID_ENUM);
	}

static void set_array(SyArray* a, GLint size, GLenum type, GLsizei stride, const void* ptr)
	{
	a->size = size; a->type = type; a->stride = stride; a->ptr = ptr;
	}
void sy_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void* ptr)
	{
	SyContext* c = sy_cur(); if (!c) return;
	if (size < 2 || size > 4 || stride < 0) { sy_seterr(c, GL_INVALID_VALUE); return; }
	if (type != GL_BYTE && type != GL_SHORT && type != GL_FIXED && type != GL_FLOAT) { sy_seterr(c, GL_INVALID_ENUM); return; }
	set_array(&c->va, size, type, stride, ptr);
	}
void sy_glColorPointer(GLint size, GLenum type, GLsizei stride, const void* ptr)
	{
	SYTRACE(sy_thread_peek(), "glColorPointer(%d, 0x%x)", (u32)__builtin_return_address(0), size, type);
	SyContext* c = sy_cur(); if (!c) return;
	if (size != 4 || stride < 0) { sy_seterr(c, GL_INVALID_VALUE); return; }
	if (type != GL_UNSIGNED_BYTE && type != GL_FIXED && type != GL_FLOAT) { sy_seterr(c, GL_INVALID_ENUM); return; }
	set_array(&c->ca, size, type, stride, ptr);
	}
void sy_glNormalPointer(GLenum type, GLsizei stride, const void* ptr)
	{
	SyContext* c = sy_cur(); if (!c) return;
	if (stride < 0) { sy_seterr(c, GL_INVALID_VALUE); return; }
	if (type != GL_BYTE && type != GL_SHORT && type != GL_FIXED && type != GL_FLOAT) { sy_seterr(c, GL_INVALID_ENUM); return; }
	set_array(&c->na, 3, type, stride, ptr);
	}
void sy_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void* ptr)
	{
	SyContext* c = sy_cur(); if (!c) return;
	if (size < 2 || size > 4 || stride < 0) { sy_seterr(c, GL_INVALID_VALUE); return; }
	if (type != GL_BYTE && type != GL_SHORT && type != GL_FIXED && type != GL_FLOAT) { sy_seterr(c, GL_INVALID_ENUM); return; }
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
	default: sy_seterr(sy_cur(), GL_INVALID_ENUM); return 0;
		}
	}

/* implementation limits: answered with or without a current context (Hitchcock reads
   GL_MAX_TEXTURE_SIZE into an uninitialised variable and divides by it to tile big images) */
static int get_const(GLenum name, GLint* v)
	{
	switch (name)
		{
	case GL_MAX_VIEWPORT_DIMS: v[0] = 1024; v[1] = 1024; return 1;
	case GL_MAX_TEXTURE_SIZE: *v = 1024; return 1;
	case GL_MAX_MODELVIEW_STACK_DEPTH: *v = MV_DEPTH; return 1;
	case GL_MAX_PROJECTION_STACK_DEPTH: *v = PROJ_DEPTH; return 1;
	case GL_MAX_TEXTURE_STACK_DEPTH: *v = TEX_DEPTH; return 1;
	case GL_MAX_LIGHTS: *v = SY_MAX_LIGHTS; return 1;
	case GL_MAX_TEXTURE_UNITS: *v = 1; return 1;
	case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: *v = 8; return 1;
	case GL_ALPHA_BITS: case GL_STENCIL_BITS: *v = 0; return 1;
	case GL_DEPTH_BITS: *v = 16; return 1;
	case GL_IMPLEMENTATION_COLOR_READ_FORMAT_OES: *v = GL_RGB; return 1;
	case GL_IMPLEMENTATION_COLOR_READ_TYPE_OES: *v = GL_UNSIGNED_SHORT_5_6_5; return 1;
	default: return 0;
		}
	}

void sy_glGetIntegerv(GLenum name, GLint* v)
	{
	if (!sy_cur())
		SYTRACE(sy_thread_peek(), "glGetIntegerv(0x%x) without a context", (u32)__builtin_return_address(0), name);
	if (!v) return;
	if (get_const(name, v)) return;
	SyContext* c = sy_cur(); if (!c) return;
	switch (name)
		{
	case GL_VIEWPORT: v[0] = c->vpX; v[1] = c->vpY; v[2] = c->vpW; v[3] = c->vpH; break;
	case GL_SCISSOR_BOX: v[0] = c->scX; v[1] = c->scY; v[2] = c->scW; v[3] = c->scH; break;
	case GL_TEXTURE_BINDING_2D: *v = (GLint)c->bound->name; break;
	case GL_MATRIX_MODE: *v = (GLint)c->matrixMode; break;
	case GL_UNPACK_ALIGNMENT: *v = c->unpackAlign; break;
	case GL_PACK_ALIGNMENT: *v = c->packAlign; break;
	default: sy_seterr(c, GL_INVALID_ENUM); break;
		}
	}

void sy_glGetFloatv(GLenum name, GLfloat* v)
	{
	if (!v) return;
	GLint cv[2] = {0, 0};
	if (get_const(name, cv)) { v[0] = (GLfloat)cv[0]; v[1] = (GLfloat)cv[1]; return; }
	SyContext* c = sy_cur(); if (!c) return;
	switch (name)
		{
	case GL_MODELVIEW_MATRIX: memcpy(v, c->mv[c->mvTop], 64); break;
	case GL_PROJECTION_MATRIX: memcpy(v, c->proj[c->projTop], 64); break;
	case GL_TEXTURE_MATRIX: memcpy(v, c->tex[c->texTop], 64); break;
	default:
		{
		GLint iv[4] = {0, 0, 0, 0};
		int e = c->error;
		sy_glGetIntegerv(name, iv);
		if (c->error != e) return;
		for (int i = 0; i < 4; ++i) v[i] = (GLfloat)iv[i];
		break;
		}
		}
	}

/* ---------------------------------------------------------------- vertex stage */

static const u8* elem(const SyArray* a, int i, int bytesPer)
	{
	int stride = a->stride ? a->stride : a->size * bytesPer;
	return (const u8*)a->ptr + i * stride;
	}

static int type_bytes(GLenum t)
	{
	switch (t) { case GL_BYTE: case GL_UNSIGNED_BYTE: return 1; case GL_SHORT: case GL_UNSIGNED_SHORT: return 2; default: return 4; }
	}

/* aNorm: 1 = unsigned byte colour 0..1, 2 = signed normal [-1, 1] */
static float comp(GLenum type, const u8* p, int k, int aNorm)
	{
	switch (type)
		{
	case GL_FLOAT: return ((const float*)p)[k];
	case GL_FIXED: return sy_fx(((const int*)p)[k]);
	case GL_SHORT:
		{ float v = (float)((const short*)p)[k]; return aNorm == 2 ? (2.0f * v + 1.0f) / 65535.0f : v; }
	case GL_BYTE:
		{ float v = (float)((const signed char*)p)[k]; return aNorm == 2 ? (2.0f * v + 1.0f) / 255.0f : v; }
	case GL_UNSIGNED_BYTE: return aNorm == 1 ? ((const u8*)p)[k] / 255.0f : (float)((const u8*)p)[k];
	default: return 0.0f;
		}
	}

typedef struct
	{
	float mvp[16];
	const float* mv;
	float nm[9];		/* normal matrix (row-major) */
	int lit, textured, texIdentity, fog;
	const float* texm;
	} SyFrame;

static void transform(SyContext* c, const SyFrame* f, int idx, SyCV* o)
	{
	float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	const u8* p = elem(&c->va, idx, type_bytes(c->va.type));
	for (int k = 0; k < c->va.size; ++k)
		v[k] = comp(c->va.type, p, k, 0);
	const float* m = f->mvp;
	o->x = m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12] * v[3];
	o->y = m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13] * v[3];
	o->z = m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14] * v[3];
	o->w = m[3] * v[0] + m[7] * v[1] + m[11] * v[2] + m[15] * v[3];
	float col[4];
	if (c->ca.enabled && c->ca.ptr)
		{
		const u8* q = elem(&c->ca, idx, type_bytes(c->ca.type));
		col[0] = comp(c->ca.type, q, 0, 1); col[1] = comp(c->ca.type, q, 1, 1);
		col[2] = comp(c->ca.type, q, 2, 1); col[3] = comp(c->ca.type, q, 3, 1);
		}
	else
		memcpy(col, c->color, 16);
	if (f->lit)
		{
		const float* w = f->mv;
		float eye[3] = { w[0] * v[0] + w[4] * v[1] + w[8] * v[2] + w[12] * v[3],
						 w[1] * v[0] + w[5] * v[1] + w[9] * v[2] + w[13] * v[3],
						 w[2] * v[0] + w[6] * v[1] + w[10] * v[2] + w[14] * v[3] };
		float n[3];
		if (c->na.enabled && c->na.ptr)
			{
			const u8* q = elem(&c->na, idx, type_bytes(c->na.type));
			n[0] = comp(c->na.type, q, 0, 2); n[1] = comp(c->na.type, q, 1, 2); n[2] = comp(c->na.type, q, 2, 2);
			}
		else
			memcpy(n, c->normal, 12);
		const float* nm = f->nm;
		float en[3] = { nm[0] * n[0] + nm[1] * n[1] + nm[2] * n[2],
						nm[3] * n[0] + nm[4] * n[1] + nm[5] * n[2],
						nm[6] * n[0] + nm[7] * n[1] + nm[8] * n[2] };
		if (c->normalize || c->rescaleNormal)
			{
			float l = sy_sqrtf(en[0] * en[0] + en[1] * en[1] + en[2] * en[2]);
			if (l > 0) { en[0] /= l; en[1] /= l; en[2] /= l; }
			}
		float lit[4];
		sy_light_vertex(c, eye, en, col, lit);
		o->r = lit[0]; o->g = lit[1]; o->b = lit[2]; o->a = lit[3];
		}
	else
		{
		o->r = col[0]; o->g = col[1]; o->b = col[2]; o->a = col[3];
		}
	o->fog = 1.0f;
	if (f->fog)
		{
		const float* w = f->mv;		/* fog coordinate = |eye z| (GL allows the eye-plane distance) */
		float ze = w[2] * v[0] + w[6] * v[1] + w[10] * v[2] + w[14] * v[3];
		float dz = ze < 0 ? -ze : ze, fo;
		if (c->fogMode == GL_LINEAR)
			fo = c->fogEnd != c->fogStart ? (c->fogEnd - dz) / (c->fogEnd - c->fogStart) : 1.0f;
		else
			{
			float e = c->fogDensity * dz;
			fo = 1.0f / sy_powf(2.71828183f, c->fogMode == GL_EXP2 ? e * e : e);
			}
		o->fog = fo < 0 ? 0 : (fo > 1 ? 1 : fo);
		}
	if (f->textured)
		{
		float tc[4] = { c->texcoord[0], c->texcoord[1], c->texcoord[2], c->texcoord[3] };
		if (c->ta.enabled && c->ta.ptr)
			{
			const u8* q = elem(&c->ta, idx, type_bytes(c->ta.type));
			tc[2] = 0.0f; tc[3] = 1.0f;
			for (int k = 0; k < c->ta.size; ++k) tc[k] = comp(c->ta.type, q, k, 0);
			}
		if (!f->texIdentity)
			{
			const float* t = f->texm;
			float s = t[0] * tc[0] + t[4] * tc[1] + t[8] * tc[2] + t[12] * tc[3];
			float tt = t[1] * tc[0] + t[5] * tc[1] + t[9] * tc[2] + t[13] * tc[3];
			float q = t[3] * tc[0] + t[7] * tc[1] + t[11] * tc[2] + t[15] * tc[3];
			tc[0] = s; tc[1] = tt; tc[3] = q;
			}
		if (tc[3] != 1.0f && tc[3] != 0.0f) { tc[0] /= tc[3]; tc[1] /= tc[3]; }
		o->s = tc[0]; o->t = tc[1];
		}
	else
		{
		o->s = 0; o->t = 0;
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
	for (int i = 0; i < CV_FLOATS; ++i) po[i] = pa[i] + (pb[i] - pa[i]) * t;
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
	o->iw = iw; o->sw = in->s * iw; o->tw = in->t * iw;
	o->fog = in->fog * 255.0f;
	}

static void draw_triangle(SyContext* c, const SyRState* st, const SyCV* a, const SyCV* b, const SyCV* cc, const SyCV* provoking)
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
		float r = provoking->r * 255.0f, g = provoking->g * 255.0f, b2 = provoking->b * 255.0f, al = provoking->a * 255.0f;
		for (int i = 0; i < n; ++i) { w[i].r = r; w[i].g = g; w[i].b = b2; w[i].a = al; }
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
	if (c->traceTri > 0)
		{
		--c->traceTri;
		for (int i = 0; i < 3 && i < n; ++i)
			SYTRACE(sy_thread_peek(), "  v%d x %d y %d z %d rgba %d %d %d %d iw %d sw %d tw %d (/1000) tex %d lin %d", 0, i,
					(int)(w[i].x * 1000), (int)(w[i].y * 1000), (int)(w[i].z * 1000), (int)w[i].r, (int)w[i].g, (int)w[i].b,
					(int)w[i].a, (int)(w[i].iw * 1000), (int)(w[i].sw * 1000), (int)(w[i].tw * 1000),
					st->tex ? st->tex->w : 0, st->tex ? st->tex->linear : -1);
		}
	for (int i = 1; i + 1 < n; ++i)
		sy_raster_triangle(c->draw, st, &w[0], &w[i], &w[i + 1]);
	}

static int prepare(SyContext* c, int count, SyFrame* f, SyRState* st, SyRTex* rt)
	{
	if (!c->draw || !c->va.enabled || !c->va.ptr)
		return 0;
	if (count > c->vcap)
		{
		SyCV* nb = (SyCV*)sy_alloc(count * (int)sizeof(SyCV));
		if (!nb) { sy_seterr(c, GL_OUT_OF_MEMORY); return 0; }
		sy_free(c->vbuf);
		c->vbuf = nb; c->vcap = count;
		}
	mat_mul(f->mvp, c->proj[c->projTop], c->mv[c->mvTop]);
	f->mv = c->mv[c->mvTop];
	f->lit = c->lighting;
	if (f->lit)
		normal_matrix(f->mv, f->nm);
	const SyTex* t = c->bound;
	f->textured = c->texture2D && t && t->data && t->w && t->h;
	f->texm = c->tex[c->texTop];
	f->texIdentity = mat_is_identity(f->texm);
	f->fog = c->fog;

	memset(st, 0, sizeof(*st));
	st->depthTest = c->depthTest; st->depthWrite = c->depthWrite; st->depthFunc = (int)c->depthFunc;
	st->writeColor = c->colorMask != 0;
	if (c->fog)
		{
		st->fog = 1;
		st->fogR = (int)(c->fogColor[0] * 255.0f); st->fogG = (int)(c->fogColor[1] * 255.0f); st->fogB = (int)(c->fogColor[2] * 255.0f);
		}
	if (c->polyOffset)
		{
		st->polyOffset = 1; st->polyFactor = c->polyFactor; st->polyUnits = c->polyUnits;
		}
	if (f->textured)
		{
		rt->data = t->data; rt->w = t->w; rt->h = t->h;
		GLenum minF = t->minF;
		rt->linear = !(t->magF == GL_NEAREST && (minF == GL_NEAREST || minF == GL_NEAREST_MIPMAP_NEAREST || minF == GL_NEAREST_MIPMAP_LINEAR));
		rt->clampS = t->wrapS == GL_CLAMP_TO_EDGE; rt->clampT = t->wrapT == GL_CLAMP_TO_EDGE;
		rt->hasRGB = t->base != GL_ALPHA;
		rt->hasA = t->base == GL_RGBA || t->base == GL_ALPHA || t->base == GL_LUMINANCE_ALPHA;
		st->tex = rt;
		st->envMode = (int)c->envMode;
		st->envR = (int)(c->envColor[0] * 255.0f); st->envG = (int)(c->envColor[1] * 255.0f);
		st->envB = (int)(c->envColor[2] * 255.0f); st->envA = (int)(c->envColor[3] * 255.0f);
		}
	if (c->alphaTest)
		{
		st->alphaTest = 1; st->alphaFunc = (int)c->alphaFunc; st->alphaRef = (int)(c->alphaRef * 255.0f + 0.5f);
		}
	if (c->blend && !(c->blendSrc == GL_ONE && c->blendDst == GL_ZERO))
		{
		st->blend = 1; st->blendSrc = (int)c->blendSrc; st->blendDst = (int)c->blendDst;
		}
	if (c->scissor)
		{
		st->scX0 = c->scX; st->scX1 = c->scX + c->scW;
		st->scY0 = c->draw->h - (c->scY + c->scH); st->scY1 = c->draw->h - c->scY;
		}
	else
		{
		st->scX0 = 0; st->scY0 = 0; st->scX1 = c->draw->w; st->scY1 = c->draw->h;
		}
	return 1;
	}

static void trace_draw(SyContext* c, const SyCV* v, int n)
	{
	SyThread* t = sy_thread_peek();
	if (!t || t->traced >= SY_TRACE_MAX || n <= 0) return;
	float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
	for (int i = 0; i < n; ++i)
		{
		if (v[i].w == 0.0f) continue;
		float x = (float)c->vpX + (v[i].x / v[i].w + 1.0f) * 0.5f * (float)c->vpW;
		float y = (float)c->vpY + (v[i].y / v[i].w + 1.0f) * 0.5f * (float)c->vpH;
		if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
		}
	SYTRACE(t, "  draw bbox x %d..%d y %d..%d (GL y up) va 0x%x ta %s0x%x/%d ca %s0x%x col0 %d,%d,%d,%d st0 %d,%d /1000",
			0, (int)x0, (int)x1, (int)y0, (int)y1, c->va.type, c->ta.enabled ? "" : "off ", c->ta.type, c->ta.size,
			c->ca.enabled ? "" : "off ", c->ca.type, (int)(v[0].r * 255), (int)(v[0].g * 255), (int)(v[0].b * 255),
			(int)(v[0].a * 255), (int)(v[0].s * 1000), (int)(v[0].t * 1000));
	}

static void assemble(SyContext* c, const SyRState* st, GLenum mode, const SyCV* v, int n)
	{
	trace_draw(c, v, n);
	c->traceTri = 1;
	switch (mode)
		{
	case GL_TRIANGLES:
		for (int i = 0; i + 2 < n; i += 3) draw_triangle(c, st, &v[i], &v[i + 1], &v[i + 2], &v[i + 2]);
		break;
	case GL_TRIANGLE_STRIP:
		for (int i = 0; i + 2 < n; ++i)
			if (i & 1) draw_triangle(c, st, &v[i + 1], &v[i], &v[i + 2], &v[i + 2]);
			else draw_triangle(c, st, &v[i], &v[i + 1], &v[i + 2], &v[i + 2]);
		break;
	case GL_TRIANGLE_FAN:
		for (int i = 1; i + 1 < n; ++i) draw_triangle(c, st, &v[0], &v[i], &v[i + 1], &v[i + 1]);
		break;
	default:
		break;	/* points and lines: later */
		}
	}

void sy_glDrawArrays(GLenum mode, GLint first, GLsizei count)
	{
	SYTRACE(sy_thread_peek(), "glDrawArrays(%d, %d, %d) tex2d %d bound %d env 0x%x blend %d", (u32)__builtin_return_address(0), mode, first, count, sy_cur() ? sy_cur()->texture2D : -1, sy_cur() && sy_cur()->bound ? (int)sy_cur()->bound->name : -1, sy_cur() ? sy_cur()->envMode : 0, sy_cur() ? sy_cur()->blend : -1);
	SyContext* c = sy_cur(); if (!c) return;
	if (count < 0 || first < 0) { sy_seterr(c, GL_INVALID_VALUE); return; }
	if (mode > GL_TRIANGLE_FAN) { sy_seterr(c, GL_INVALID_ENUM); return; }
	SyFrame f; SyRState st; SyRTex rt;
	if (count == 0 || !prepare(c, count, &f, &st, &rt)) return;
	for (int i = 0; i < count; ++i)
		transform(c, &f, first + i, &c->vbuf[i]);
	assemble(c, &st, mode, c->vbuf, count);
	}

void sy_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices)
	{
	SYTRACE(sy_thread_peek(), "glDrawElements(%d, %d) tex2d %d bound %d env 0x%x blend %d", (u32)__builtin_return_address(0), mode, count, sy_cur() ? sy_cur()->texture2D : -1, sy_cur() && sy_cur()->bound ? (int)sy_cur()->bound->name : -1, sy_cur() ? sy_cur()->envMode : 0, sy_cur() ? sy_cur()->blend : -1);
	SyContext* c = sy_cur(); if (!c) return;
	if (count < 0) { sy_seterr(c, GL_INVALID_VALUE); return; }
	if (mode > GL_TRIANGLE_FAN || (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT)) { sy_seterr(c, GL_INVALID_ENUM); return; }
	SyFrame f; SyRState st; SyRTex rt;
	if (count == 0 || !indices || !prepare(c, count, &f, &st, &rt)) return;
	for (int i = 0; i < count; ++i)
		{
		int idx = type == GL_UNSIGNED_BYTE ? ((const u8*)indices)[i] : ((const u16*)indices)[i];
		transform(c, &f, idx, &c->vbuf[i]);
		}
	assemble(c, &st, mode, c->vbuf, count);
	}

/* ---------------------------------------------------------------- pixel reads */

void sy_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels)
	{
	SyContext* c = sy_cur(); if (!c || !c->draw || !pixels) return;
	if (w < 0 || h < 0) { sy_seterr(c, GL_INVALID_VALUE); return; }
	int bpp;
	if (format == GL_RGBA && type == GL_UNSIGNED_BYTE) bpp = 4;
	else if (format == GL_RGB && type == GL_UNSIGNED_BYTE) bpp = 3;
	else if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) bpp = 2;
	else { sy_seterr(c, GL_INVALID_OPERATION); return; }
	int rowBytes = (w * bpp + c->packAlign - 1) / c->packAlign * c->packAlign;
	SySurface* s = c->draw;
	for (int j = 0; j < h; ++j)
		{
		u8* d = (u8*)pixels + j * rowBytes;
		int sy = s->h - 1 - (y + j);			/* GL row 0 is the bottom */
		for (int i = 0; i < w; ++i, d += bpp)
			{
			int sx = x + i;
			u32 px = (sx >= 0 && sx < s->w && sy >= 0 && sy < s->h) ? s->color[sy * s->w + sx] : 0;
			u8 r = (u8)(px >> 16), g = (u8)(px >> 8), b = (u8)px;
			if (bpp == 2) { u16 v = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); d[0] = (u8)v; d[1] = (u8)(v >> 8); }
			else { d[0] = r; d[1] = g; d[2] = b; if (bpp == 4) d[3] = 255; }
			}
		}
	}
