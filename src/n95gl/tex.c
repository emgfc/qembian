/* sygl texture objects (one unit for now): names, upload in every GL ES 1.1 format/type into
   0xAARRGGBB texels (level 0; other levels are accepted and dropped), sub-image updates,
   parameters and the texture environment. Texel storage comes from the big heap. */
#include "gl_int.h"

static void tex_defaults(SyTex* t, GLuint aName)
	{
	memset(t, 0, sizeof(*t));
	t->name = aName;
	t->base = GL_RGBA;
	t->minF = GL_NEAREST_MIPMAP_LINEAR;		/* GL default (sampled as linear: no mipmaps) */
	t->magF = GL_LINEAR;
	t->wrapS = GL_REPEAT; t->wrapT = GL_REPEAT;
	}

void sy_tex_init(SyContext* c)
	{
	tex_defaults(&c->defaultTex, 0);
	c->bound = &c->defaultTex;
	c->nextTexName = 1;
	c->envMode = GL_MODULATE;
	c->unpackAlign = 4; c->packAlign = 4;
	}

void sy_tex_free_all(SyContext* c)
	{
	for (int i = 0; i < c->texCap; ++i)
		if (c->texTable[i])
			{
			sy_big_free(c->texTable[i]->data);
			sy_free(c->texTable[i]);
			}
	sy_free(c->texTable);
	sy_big_free(c->defaultTex.data);
	c->texTable = 0; c->texCap = 0;
	}

int sy_tex_cap(SyContext* c, GLenum cap, int on)
	{
	if (cap == GL_TEXTURE_2D) { c->texture2D = on; return 1; }
	return 0;
	}

static SyTex* lookup(SyContext* c, GLuint aName, int aCreate)
	{
	if (aName == 0)
		return &c->defaultTex;
	if ((int)aName >= c->texCap)
		{
		if (!aCreate) return 0;
		int cap = c->texCap ? c->texCap : 16;
		while (cap <= (int)aName) cap *= 2;
		SyTex** nt = (SyTex**)sy_allocz(cap * (int)sizeof(SyTex*));
		if (!nt) return 0;
		if (c->texTable) memcpy(nt, c->texTable, (unsigned)c->texCap * sizeof(SyTex*));
		sy_free(c->texTable);
		c->texTable = nt; c->texCap = cap;
		}
	SyTex* t = c->texTable[aName];
	if (!t && aCreate)
		{
		t = (SyTex*)sy_alloc(sizeof(SyTex));
		if (!t) return 0;
		tex_defaults(t, aName);
		c->texTable[aName] = t;
		}
	return t;
	}

void sy_glGenTextures(GLsizei n, GLuint* names)
	{
	SyContext* c = sy_cur(); if (!c) return;
	if (n < 0) { sy_seterr(c, GL_INVALID_VALUE); return; }
	for (int i = 0; i < n && names; ++i)
		{
		while ((int)c->nextTexName < c->texCap && c->texTable[c->nextTexName]) ++c->nextTexName;
		names[i] = c->nextTexName++;
		}
	}

void sy_glDeleteTextures(GLsizei n, const GLuint* names)
	{
	SyContext* c = sy_cur(); if (!c || !names) return;
	for (int i = 0; i < n; ++i)
		{
		GLuint nm = names[i];
		if (nm == 0 || (int)nm >= c->texCap || !c->texTable[nm]) continue;
		SyTex* t = c->texTable[nm];
		if (c->bound == t) c->bound = &c->defaultTex;
		sy_big_free(t->data);
		sy_free(t);
		c->texTable[nm] = 0;
		if (nm < c->nextTexName) c->nextTexName = nm;
		}
	}

GLboolean sy_glIsTexture(GLuint name)
	{
	SyContext* c = sy_cur(); if (!c) return 0;
	return name != 0 && (int)name < c->texCap && c->texTable[name] != 0;
	}

void sy_glBindTexture(GLenum target, GLuint name)
	{
	SyContext* c = sy_cur(); if (!c) return;
	if (target != GL_TEXTURE_2D) { sy_seterr(c, GL_INVALID_ENUM); return; }
	SyTex* t = lookup(c, name, 1);
	if (!t) { sy_seterr(c, GL_OUT_OF_MEMORY); return; }
	c->bound = t;
	}

void sy_glActiveTexture(GLenum unit)
	{
	SyContext* c = sy_cur(); if (!c) return;
	if (unit != GL_TEXTURE0)
		SYLOG("glActiveTexture(unit %d): only unit 0 so far", (int)(unit - GL_TEXTURE0));
	}
void sy_glClientActiveTexture(GLenum unit) { sy_glActiveTexture(unit); }

void sy_glPixelStorei(GLenum pname, GLint v)
	{
	SyContext* c = sy_cur(); if (!c) return;
	if (v != 1 && v != 2 && v != 4 && v != 8) { sy_seterr(c, GL_INVALID_VALUE); return; }
	if (pname == GL_UNPACK_ALIGNMENT) c->unpackAlign = v;
	else if (pname == GL_PACK_ALIGNMENT) c->packAlign = v;
	else sy_seterr(c, GL_INVALID_ENUM);
	}

/* bytes per pixel of a format/type pair, 0 if invalid */
static int pixel_bytes(GLenum format, GLenum type)
	{
	if (type == GL_UNSIGNED_BYTE)
		switch (format)
			{
		case GL_RGBA: return 4;
		case GL_RGB: return 3;
		case GL_LUMINANCE_ALPHA: return 2;
		case GL_LUMINANCE: case GL_ALPHA: return 1;
		default: return 0;
			}
	if (type == GL_UNSIGNED_SHORT_5_6_5) return format == GL_RGB ? 2 : 0;
	if (type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) return format == GL_RGBA ? 2 : 0;
	return 0;
	}

/* one source pixel -> 0xAARRGGBB. Components a format lacks are 255 (white RGB for ALPHA, opaque for
   colour-only formats), which makes GL_MODULATE right; the other env modes check hasRGB/hasA. */
static u32 convert(GLenum format, GLenum type, const u8* p)
	{
	u32 r, g, b, a;
	if (type == GL_UNSIGNED_BYTE)
		switch (format)
			{
		case GL_RGBA: r = p[0]; g = p[1]; b = p[2]; a = p[3]; break;
		case GL_RGB: r = p[0]; g = p[1]; b = p[2]; a = 255; break;
		case GL_LUMINANCE_ALPHA: r = g = b = p[0]; a = p[1]; break;
		case GL_LUMINANCE: r = g = b = p[0]; a = 255; break;
		default: r = g = b = 255; a = p[0]; break;		/* GL_ALPHA */
			}
	else
		{
		u32 v = (u32)p[0] | ((u32)p[1] << 8);
		if (type == GL_UNSIGNED_SHORT_5_6_5)
			{ r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31; r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2); a = 255; }
		else if (type == GL_UNSIGNED_SHORT_4_4_4_4)
			{ r = ((v >> 12) & 15) * 17; g = ((v >> 8) & 15) * 17; b = ((v >> 4) & 15) * 17; a = (v & 15) * 17; }
		else
			{ r = (v >> 11) & 31; g = (v >> 6) & 31; b = (v >> 1) & 31; r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2); a = (v & 1) ? 255 : 0; }
		}
	return (a << 24) | (r << 16) | (g << 8) | b;
	}

static void upload(SyContext* c, SyTex* t, int x0, int y0, int w, int h, GLenum format, GLenum type, const void* pixels)
	{
	int bpp = pixel_bytes(format, type);
	int rowBytes = w * bpp;
	rowBytes = (rowBytes + c->unpackAlign - 1) / c->unpackAlign * c->unpackAlign;
	const u8* src = (const u8*)pixels;
	for (int y = 0; y < h; ++y)
		{
		u32* dst = t->data + (y0 + y) * t->w + x0;
		const u8* s = src + y * rowBytes;
		for (int x = 0; x < w; ++x, s += bpp)
			dst[x] = convert(format, type, s);
		}
	}

void sy_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLint border,
					 GLenum format, GLenum type, const void* pixels)
	{
	SyContext* c = sy_cur(); if (!c) return;
	if (target != GL_TEXTURE_2D) { sy_seterr(c, GL_INVALID_ENUM); return; }
	if (level < 0 || w < 0 || h < 0 || w > 1024 || h > 1024 || border != 0) { sy_seterr(c, GL_INVALID_VALUE); return; }
	if (!pixel_bytes(format, type)) { sy_seterr(c, GL_INVALID_ENUM); return; }
	if ((GLenum)internalformat != format) { sy_seterr(c, GL_INVALID_OPERATION); return; }
	if (level > 0)
		return;		/* mipmap levels: dropped (we sample level 0) */
	SyTex* t = c->bound;
	if (t->w != w || t->h != h || !t->data)
		{
		sy_big_free(t->data);
		t->data = 0; t->w = 0; t->h = 0;
		if (w && h)
			{
			t->data = (u32*)sy_big_alloc(w * h * 4);
			if (!t->data) { sy_seterr(c, GL_OUT_OF_MEMORY); return; }
			memset(t->data, 0, (unsigned)(w * h * 4));
			}
		t->w = w; t->h = h;
		}
	t->base = format;
	if (pixels && w && h)
		upload(c, t, 0, 0, w, h, format, type, pixels);
	}

void sy_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
						GLenum format, GLenum type, const void* pixels)
	{
	SyContext* c = sy_cur(); if (!c) return;
	if (target != GL_TEXTURE_2D) { sy_seterr(c, GL_INVALID_ENUM); return; }
	if (!pixel_bytes(format, type)) { sy_seterr(c, GL_INVALID_ENUM); return; }
	SyTex* t = c->bound;
	if (level > 0) return;
	if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > t->w || y + h > t->h) { sy_seterr(c, GL_INVALID_VALUE); return; }
	if (pixels && t->data)
		upload(c, t, x, y, w, h, format, type, pixels);
	}

void sy_glTexParameteri(GLenum target, GLenum pname, GLint v)
	{
	SyContext* c = sy_cur(); if (!c) return;
	if (target != GL_TEXTURE_2D) { sy_seterr(c, GL_INVALID_ENUM); return; }
	SyTex* t = c->bound;
	switch (pname)
		{
	case GL_TEXTURE_MIN_FILTER: t->minF = (GLenum)v; break;
	case GL_TEXTURE_MAG_FILTER: t->magF = (GLenum)v; break;
	case GL_TEXTURE_WRAP_S: t->wrapS = (GLenum)v; break;
	case GL_TEXTURE_WRAP_T: t->wrapT = (GLenum)v; break;
	case GL_GENERATE_MIPMAP: break;		/* no mipmaps yet */
	default: sy_seterr(c, GL_INVALID_ENUM); break;
		}
	}
void sy_glTexParameterf(GLenum target, GLenum pname, GLfloat v) { sy_glTexParameteri(target, pname, (GLint)v); }
void sy_glTexParameterx(GLenum target, GLenum pname, GLfixed v) { sy_glTexParameteri(target, pname, v); }
void sy_glTexParameteriv(GLenum target, GLenum pname, const GLint* v) { if (v) sy_glTexParameteri(target, pname, v[0]); }
void sy_glTexParameterfv(GLenum target, GLenum pname, const GLfloat* v) { if (v) sy_glTexParameteri(target, pname, (GLint)v[0]); }
void sy_glTexParameterxv(GLenum target, GLenum pname, const GLfixed* v) { if (v) sy_glTexParameteri(target, pname, v[0]); }

void sy_glTexEnvfv(GLenum target, GLenum pname, const GLfloat* v)
	{
	SyContext* c = sy_cur(); if (!c || !v) return;
	if (target != GL_TEXTURE_ENV) { sy_seterr(c, GL_INVALID_ENUM); return; }
	if (pname == GL_TEXTURE_ENV_MODE)
		{
		GLenum m = (GLenum)(int)v[0];
		if (m != GL_MODULATE && m != GL_REPLACE && m != GL_DECAL && m != GL_BLEND && m != GL_ADD)
			{
			SYLOG("glTexEnv mode 0x%x not implemented (combine?), using MODULATE", m);
			m = GL_MODULATE;
			}
		c->envMode = m;
		}
	else if (pname == GL_TEXTURE_ENV_COLOR)
		memcpy(c->envColor, v, 16);
	/* combiner parameters (ES 1.1 COMBINE): accepted, not implemented yet */
	}
void sy_glTexEnvf(GLenum target, GLenum pname, GLfloat v) { sy_glTexEnvfv(target, pname, &v); }
void sy_glTexEnvi(GLenum target, GLenum pname, GLint v) { GLfloat f = (GLfloat)v; sy_glTexEnvfv(target, pname, &f); }
void sy_glTexEnvx(GLenum target, GLenum pname, GLfixed v)
	{
	GLfloat f = pname == GL_TEXTURE_ENV_MODE ? (GLfloat)v : sy_fx(v);
	sy_glTexEnvfv(target, pname, &f);
	}
void sy_glTexEnviv(GLenum target, GLenum pname, const GLint* v)
	{
	if (!v) return;
	GLfloat f[4];
	if (pname == GL_TEXTURE_ENV_COLOR)
		for (int i = 0; i < 4; ++i) f[i] = (GLfloat)v[i] / 2147483647.0f;
	else
		f[0] = (GLfloat)v[0];
	sy_glTexEnvfv(target, pname, f);
	}
void sy_glTexEnvxv(GLenum target, GLenum pname, const GLfixed* v)
	{
	if (!v) return;
	GLfloat f[4];
	if (pname == GL_TEXTURE_ENV_COLOR)
		for (int i = 0; i < 4; ++i) f[i] = sy_fx(v[i]);
	else
		f[0] = (GLfloat)v[0];
	sy_glTexEnvfv(target, pname, f);
	}

/* ---------------------------------------------------------------- textures from the colour buffer */

/* copy a rectangle of the surface (GL coordinates: row 0 at the bottom) into t at (dx, dy) */
static void copy_from_surface(SyTex* t, const SySurface* s, int dx, int dy, int x, int y, int w, int h)
	{
	for (int j = 0; j < h; ++j)
		{
		int sy = s->h - 1 - (y + j);
		u32* d = t->data + (dy + j) * t->w + dx;
		for (int i = 0; i < w; ++i)
			{
			int sx = x + i;
			u32 px = (sx >= 0 && sx < s->w && sy >= 0 && sy < s->h) ? s->color[sy * s->w + sx] : 0;
			d[i] = px | 0xff000000u;	/* our colour buffers have no alpha: opaque */
			}
		}
	}

static int tex_storage(SyTex* t, int w, int h)
	{
	if (t->w == w && t->h == h && t->data)
		return 1;
	sy_big_free(t->data);
	t->data = 0; t->w = 0; t->h = 0;
	if (w && h)
		{
		t->data = (u32*)sy_big_alloc(w * h * 4);
		if (!t->data) return 0;
		}
	t->w = w; t->h = h;
	return 1;
	}

void sy_glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei w, GLsizei h, GLint border)
	{
	SyContext* c = sy_cur(); if (!c || !c->draw) return;
	if (target != GL_TEXTURE_2D) { sy_seterr(c, GL_INVALID_ENUM); return; }
	if (level < 0 || w < 0 || h < 0 || w > 1024 || h > 1024 || border != 0) { sy_seterr(c, GL_INVALID_VALUE); return; }
	if (level > 0) return;
	SyTex* t = c->bound;
	if (!tex_storage(t, w, h)) { sy_seterr(c, GL_OUT_OF_MEMORY); return; }
	t->base = internalformat;
	copy_from_surface(t, c->draw, 0, 0, x, y, w, h);
	}

void sy_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLint x, GLint y, GLsizei w, GLsizei h)
	{
	SyContext* c = sy_cur(); if (!c || !c->draw) return;
	if (target != GL_TEXTURE_2D) { sy_seterr(c, GL_INVALID_ENUM); return; }
	if (level > 0) return;
	SyTex* t = c->bound;
	if (xoff < 0 || yoff < 0 || w < 0 || h < 0 || xoff + w > t->w || yoff + h > t->h) { sy_seterr(c, GL_INVALID_VALUE); return; }
	copy_from_surface(t, c->draw, xoff, yoff, x, y, w, h);
	}

/* eglBindTexImage: the pbuffer's colour buffer becomes the bound texture's level 0 */
int sy_gl_tex_from_surface(struct SyContext* c, SySurface* s, int aHasAlpha)
	{
	SyTex* t = c->bound;
	if (!tex_storage(t, s->w, s->h))
		return -1;
	t->base = aHasAlpha ? GL_RGBA : GL_RGB;
	/* texture row 0 = the surface's bottom row (GL convention) */
	copy_from_surface(t, s, 0, 0, 0, 0, s->w, s->h);
	return 0;
	}
