/* sygl fixed-function lighting (GL ES 1.1 §2.12): per vertex, front material only (two-sided
   lighting is accepted but lights back faces like front faces), infinite viewer, colour material =
   AMBIENT_AND_DIFFUSE (the only mode ES 1.1 has). Light positions/directions are stored in eye
   coordinates, transformed by the modelview current at glLight time. */
#include "gl_int.h"

static void set4(float* d, float a, float b, float c, float e) { d[0] = a; d[1] = b; d[2] = c; d[3] = e; }

void sy_light_init(SyContext* c)
	{
	for (int i = 0; i < SY_MAX_LIGHTS; ++i)
		{
		SyLight* l = &c->light[i];
		l->enabled = 0;
		set4(l->ambient, 0, 0, 0, 1);
		if (i == 0) { set4(l->diffuse, 1, 1, 1, 1); set4(l->specular, 1, 1, 1, 1); }
		else { set4(l->diffuse, 0, 0, 0, 1); set4(l->specular, 0, 0, 0, 1); }
		set4(l->position, 0, 0, 1, 0);
		l->spotDir[0] = 0; l->spotDir[1] = 0; l->spotDir[2] = -1;
		l->spotExp = 0; l->spotCutoff = 180; l->spotCosCutoff = -1;
		l->kc = 1; l->kl = 0; l->kq = 0;
		}
	set4(c->modelAmbient, 0.2f, 0.2f, 0.2f, 1);
	set4(c->mat.ambient, 0.2f, 0.2f, 0.2f, 1);
	set4(c->mat.diffuse, 0.8f, 0.8f, 0.8f, 1);
	set4(c->mat.specular, 0, 0, 0, 1);
	set4(c->mat.emission, 0, 0, 0, 1);
	c->mat.shininess = 0;
	c->normal[0] = 0; c->normal[1] = 0; c->normal[2] = 1;
	}

int sy_light_cap(SyContext* c, GLenum cap, int on)
	{
	if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + SY_MAX_LIGHTS) { c->light[cap - GL_LIGHT0].enabled = on; return 1; }
	switch (cap)
		{
	case GL_LIGHTING: c->lighting = on; return 1;
	case GL_NORMALIZE: c->normalize = on; return 1;
	case GL_RESCALE_NORMAL: c->rescaleNormal = on; return 1;
	case GL_COLOR_MATERIAL:
		c->colorMaterial = on;
		if (on)		/* the material tracks the current colour from now on */
			{
			memcpy(c->mat.ambient, c->color, 16);
			memcpy(c->mat.diffuse, c->color, 16);
			}
		return 1;
	default: return 0;
		}
	}

static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static void norm3(float* v)
	{
	float l = sy_sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; }
	}

void sy_light_vertex(SyContext* c, const float* aEye, const float* aN, const float* aCol, float* aOut)
	{
	const float* amb = c->colorMaterial ? aCol : c->mat.ambient;
	const float* dif = c->colorMaterial ? aCol : c->mat.diffuse;
	const float* spe = c->mat.specular;
	int hasSpec = spe[0] != 0 || spe[1] != 0 || spe[2] != 0;
	float r = c->mat.emission[0] + c->modelAmbient[0] * amb[0];
	float g = c->mat.emission[1] + c->modelAmbient[1] * amb[1];
	float b = c->mat.emission[2] + c->modelAmbient[2] * amb[2];
	for (int i = 0; i < SY_MAX_LIGHTS; ++i)
		{
		const SyLight* l = &c->light[i];
		if (!l->enabled)
			continue;
		float L[3], att = 1.0f;
		if (l->position[3] == 0.0f)
			{
			L[0] = l->position[0]; L[1] = l->position[1]; L[2] = l->position[2];
			norm3(L);
			}
		else
			{
			L[0] = l->position[0] - aEye[0]; L[1] = l->position[1] - aEye[1]; L[2] = l->position[2] - aEye[2];
			float d = sy_sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
			if (d > 0) { L[0] /= d; L[1] /= d; L[2] /= d; }
			if (l->kl != 0 || l->kq != 0 || l->kc != 1)
				{
				float den = l->kc + l->kl * d + l->kq * d * d;
				att = den > 0 ? 1.0f / den : 1.0f;
				}
			if (l->spotCutoff != 180.0f)
				{
				float sd[3] = { l->spotDir[0], l->spotDir[1], l->spotDir[2] };
				norm3(sd);
				float cs = -(L[0] * sd[0] + L[1] * sd[1] + L[2] * sd[2]);
				if (cs < l->spotCosCutoff)
					att = 0;
				else if (l->spotExp != 0)
					att *= sy_powf(cs, l->spotExp);
				}
			}
		if (att == 0.0f)
			continue;
		float ndl = aN[0] * L[0] + aN[1] * L[1] + aN[2] * L[2];
		float lr = l->ambient[0] * amb[0], lg = l->ambient[1] * amb[1], lb = l->ambient[2] * amb[2];
		if (ndl > 0)
			{
			lr += ndl * l->diffuse[0] * dif[0]; lg += ndl * l->diffuse[1] * dif[1]; lb += ndl * l->diffuse[2] * dif[2];
			if (hasSpec)
				{
				float H[3] = { L[0], L[1], L[2] + 1.0f };	/* infinite viewer: eye direction (0,0,1) */
				norm3(H);
				float ndh = aN[0] * H[0] + aN[1] * H[1] + aN[2] * H[2];
				if (ndh > 0)
					{
					float s = c->mat.shininess != 0 ? sy_powf(ndh, c->mat.shininess) : 1.0f;
					lr += s * l->specular[0] * spe[0]; lg += s * l->specular[1] * spe[1]; lb += s * l->specular[2] * spe[2];
					}
				}
			}
		r += att * lr; g += att * lg; b += att * lb;
		}
	aOut[0] = clamp01(r); aOut[1] = clamp01(g); aOut[2] = clamp01(b);
	aOut[3] = clamp01(dif[3]);
	}

/* ---------------------------------------------------------------- entry points */

static void mv_point(const SyContext* c, const float* p, float* o)
	{
	const float* m = c->mv[c->mvTop];
	for (int i = 0; i < 4; ++i)
		o[i] = m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2] + m[12 + i] * p[3];
	}

static void mv_dir(const SyContext* c, const float* p, float* o)
	{
	const float* m = c->mv[c->mvTop];
	for (int i = 0; i < 3; ++i)
		o[i] = m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2];
	}

void sy_glLightfv(GLenum light, GLenum pname, const GLfloat* p)
	{
	SyContext* c = sy_cur(); if (!c || !p) return;
	if (light < GL_LIGHT0 || light >= GL_LIGHT0 + SY_MAX_LIGHTS) { sy_seterr(c, GL_INVALID_ENUM); return; }
	SyLight* l = &c->light[light - GL_LIGHT0];
	switch (pname)
		{
	case GL_AMBIENT: memcpy(l->ambient, p, 16); break;
	case GL_DIFFUSE: memcpy(l->diffuse, p, 16); break;
	case GL_SPECULAR: memcpy(l->specular, p, 16); break;
	case GL_POSITION: mv_point(c, p, l->position); break;
	case GL_SPOT_DIRECTION: mv_dir(c, p, l->spotDir); break;
	case GL_SPOT_EXPONENT: l->spotExp = p[0]; break;
	case GL_SPOT_CUTOFF:
		l->spotCutoff = p[0];
		l->spotCosCutoff = p[0] == 180.0f ? -1.0f : sy_cosf(p[0] * (3.14159265f / 180.0f));
		break;
	case GL_CONSTANT_ATTENUATION: l->kc = p[0]; break;
	case GL_LINEAR_ATTENUATION: l->kl = p[0]; break;
	case GL_QUADRATIC_ATTENUATION: l->kq = p[0]; break;
	default: sy_seterr(c, GL_INVALID_ENUM); break;
		}
	}
void sy_glLightf(GLenum light, GLenum pname, GLfloat v) { sy_glLightfv(light, pname, &v); }
void sy_glLightxv(GLenum light, GLenum pname, const GLfixed* p)
	{
	if (!p) return;
	float f[4];
	int n = (pname == GL_POSITION || pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR) ? 4 :
			pname == GL_SPOT_DIRECTION ? 3 : 1;
	for (int i = 0; i < n; ++i) f[i] = sy_fx(p[i]);
	sy_glLightfv(light, pname, f);
	}
void sy_glLightx(GLenum light, GLenum pname, GLfixed v) { sy_glLightxv(light, pname, &v); }

void sy_glLightModelfv(GLenum pname, const GLfloat* p)
	{
	SyContext* c = sy_cur(); if (!c || !p) return;
	switch (pname)
		{
	case GL_LIGHT_MODEL_AMBIENT: memcpy(c->modelAmbient, p, 16); break;
	case GL_LIGHT_MODEL_TWO_SIDE: c->twoSide = p[0] != 0; break;
	default: sy_seterr(c, GL_INVALID_ENUM); break;
		}
	}
void sy_glLightModelf(GLenum pname, GLfloat v) { sy_glLightModelfv(pname, &v); }
void sy_glLightModelxv(GLenum pname, const GLfixed* p)
	{
	if (!p) return;
	float f[4];
	int n = pname == GL_LIGHT_MODEL_AMBIENT ? 4 : 1;
	for (int i = 0; i < n; ++i) f[i] = sy_fx(p[i]);
	sy_glLightModelfv(pname, f);
	}
void sy_glLightModelx(GLenum pname, GLfixed v) { sy_glLightModelxv(pname, &v); }

void sy_glMaterialfv(GLenum face, GLenum pname, const GLfloat* p)
	{
	SyContext* c = sy_cur(); if (!c || !p) return;
	if (face != GL_FRONT_AND_BACK) { sy_seterr(c, GL_INVALID_ENUM); return; }	/* ES 1.1: FRONT_AND_BACK only */
	SyMaterial* m = &c->mat;
	switch (pname)
		{
	case GL_AMBIENT: memcpy(m->ambient, p, 16); break;
	case GL_DIFFUSE: memcpy(m->diffuse, p, 16); break;
	case GL_AMBIENT_AND_DIFFUSE: memcpy(m->ambient, p, 16); memcpy(m->diffuse, p, 16); break;
	case GL_SPECULAR: memcpy(m->specular, p, 16); break;
	case GL_EMISSION: memcpy(m->emission, p, 16); break;
	case GL_SHININESS: m->shininess = p[0]; break;
	default: sy_seterr(c, GL_INVALID_ENUM); break;
		}
	}
void sy_glMaterialf(GLenum face, GLenum pname, GLfloat v) { sy_glMaterialfv(face, pname, &v); }
void sy_glMaterialxv(GLenum face, GLenum pname, const GLfixed* p)
	{
	if (!p) return;
	float f[4];
	int n = pname == GL_SHININESS ? 1 : 4;
	for (int i = 0; i < n; ++i) f[i] = sy_fx(p[i]);
	sy_glMaterialfv(face, pname, f);
	}
void sy_glMaterialx(GLenum face, GLenum pname, GLfixed v) { sy_glMaterialxv(face, pname, &v); }

void sy_glNormal3f(GLfloat x, GLfloat y, GLfloat z)
	{
	SyContext* c = sy_cur(); if (!c) return;
	c->normal[0] = x; c->normal[1] = y; c->normal[2] = z;
	}
void sy_glNormal3x(GLfixed x, GLfixed y, GLfixed z) { sy_glNormal3f(sy_fx(x), sy_fx(y), sy_fx(z)); }
