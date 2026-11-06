/* sygl GL internals shared by gl.c (state, vertex pipeline), light.c (fixed-function lighting) and
   tex.c (texture objects). Portable C. */
#ifndef GL_INT_H
#define GL_INT_H
#include "sygl_int.h"

typedef float GLfloat;
typedef float GLclampf;
typedef int GLfixed;
typedef int GLint;
typedef int GLsizei;
typedef unsigned GLuint;
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

#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_FLOAT 0x1406
#define GL_FIXED 0x140C

/* lighting */
#define GL_LIGHTING 0x0B50
#define GL_LIGHT0 0x4000
#define GL_COLOR_MATERIAL 0x0B57
#define GL_NORMALIZE 0x0BA1
#define GL_RESCALE_NORMAL 0x803A
#define GL_AMBIENT 0x1200
#define GL_DIFFUSE 0x1201
#define GL_SPECULAR 0x1202
#define GL_POSITION 0x1203
#define GL_SPOT_DIRECTION 0x1204
#define GL_SPOT_EXPONENT 0x1205
#define GL_SPOT_CUTOFF 0x1206
#define GL_CONSTANT_ATTENUATION 0x1207
#define GL_LINEAR_ATTENUATION 0x1208
#define GL_QUADRATIC_ATTENUATION 0x1209
#define GL_EMISSION 0x1600
#define GL_SHININESS 0x1601
#define GL_AMBIENT_AND_DIFFUSE 0x1602
#define GL_LIGHT_MODEL_TWO_SIDE 0x0B52
#define GL_LIGHT_MODEL_AMBIENT 0x0B53
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408

/* textures */
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_ALPHA 0x1906
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_GENERATE_MIPMAP 0x8191
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_ENV 0x2300
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_TEXTURE_ENV_COLOR 0x2201
#define GL_MODULATE 0x2100
#define GL_DECAL 0x2101
#define GL_ADD 0x0104
#define GL_REPLACE 0x1E01
#define GL_BLEND 0x0BE2
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05

/* per-fragment */
#define GL_ALPHA_TEST 0x0BC0
#define GL_SCISSOR_TEST 0x0C11
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE 0x0308

#define SY_MAX_LIGHTS 8
#define MV_DEPTH 16
#define PROJ_DEPTH 4
#define TEX_DEPTH 4

typedef struct { int size; GLenum type; int stride; const void* ptr; int enabled; } SyArray;

typedef struct
	{
	int enabled;
	float ambient[4], diffuse[4], specular[4];
	float position[4];		/* eye coordinates (transformed by the modelview at glLight time) */
	float spotDir[3];		/* eye coordinates */
	float spotExp, spotCutoff, spotCosCutoff;
	float kc, kl, kq;
	} SyLight;

typedef struct { float ambient[4], diffuse[4], specular[4], emission[4]; float shininess; } SyMaterial;

/* texture object; texels 0xAARRGGBB, level 0 only (mipmap levels are accepted and dropped) */
typedef struct SyTex
	{
	GLuint name;
	int w, h;
	u32* data;
	GLenum base;			/* GL_RGBA, GL_RGB, GL_ALPHA, GL_LUMINANCE, GL_LUMINANCE_ALPHA */
	GLenum minF, magF, wrapS, wrapT;
	} SyTex;

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
	float color[4];			/* current colour */
	float normal[3];		/* current normal */
	float texcoord[4];		/* current texture coordinate (unit 0) */
	SyArray va, ca, na, ta;
	/* lighting */
	int lighting, colorMaterial, normalize, rescaleNormal, twoSide;
	SyLight light[SY_MAX_LIGHTS];
	float modelAmbient[4];
	SyMaterial mat;
	/* texturing (unit 0) */
	int texture2D;
	SyTex defaultTex;		/* texture object 0 */
	SyTex** texTable; int texCap; GLuint nextTexName;
	SyTex* bound;
	GLenum envMode; float envColor[4];
	int unpackAlign, packAlign;
	/* per-fragment */
	int blend; GLenum blendSrc, blendDst;
	int alphaTest; GLenum alphaFunc; float alphaRef;
	int scissor; int scX, scY, scW, scH;
	int colorMask;			/* bit 0..3 = r g b a; only all-or-nothing honoured */
	int polyOffset; float polyFactor, polyUnits;
	int errLogged;
	SySurface* draw;
	struct SyCV* vbuf; int vcap;
	} SyContext;

SyContext* sy_cur(void);
void sy_seterr(SyContext* c, int e);
static inline float sy_fx(GLfixed v) { return (float)v * (1.0f / 65536.0f); }

/* light.c */
void sy_light_init(SyContext* c);
/* aEye = eye-space position, aN = eye-space normal, aCol = vertex colour (for colour material),
   out = lit colour 0..1 (clamped) */
void sy_light_vertex(SyContext* c, const float* aEye, const float* aN, const float* aCol, float* aOut);
int sy_light_cap(SyContext* c, GLenum cap, int on);		/* 1 if cap was a lighting cap */

/* tex.c */
void sy_tex_init(SyContext* c);
void sy_tex_free_all(SyContext* c);
int sy_tex_cap(SyContext* c, GLenum cap, int on);		/* 1 if cap was a texture cap */

#endif
