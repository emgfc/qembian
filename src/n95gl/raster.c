/* sygl rasterizer: triangles into a SySurface (32 bpp colour, 16-bit depth).
   Edge functions on 28.4 fixed-point vertices with the top-left fill rule. Colour and depth come
   from plane equations set up once per triangle in float and stepped per pixel in fixed point
   (colour 16.16, depth 16.14: headroom for the bounding-box corners); colours are affine (Gouraud).
   Texture coordinates are perspective-correct: s/w, t/w, 1/w are evaluated exactly every 16 pixels
   and stepped linearly in between. Fragment order as GL: texture env, alpha test, depth test,
   blend, write. Two loops: a fast one for untextured opaque triangles, a general one. */
#include "sygl_int.h"

#define GL_NEVER    0x0200
#define GL_LESS     0x0201
#define GL_EQUAL    0x0202
#define GL_LEQUAL   0x0203
#define GL_GREATER  0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL   0x0206
#define GL_ALWAYS   0x0207
#define GL_MODULATE 0x2100
#define GL_DECAL 0x2101
#define GL_ADD 0x0104
#define GL_REPLACE 0x1E01
#define GL_BLEND 0x0BE2
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

#define SPAN 16

static int fx4(float v) { return (int)(v * 16.0f + (v >= 0 ? 0.5f : -0.5f)); }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

static int cmp_pass(int aFunc, u32 aV, u32 aRef)
	{
	switch (aFunc)
		{
	case GL_NEVER: return 0;
	case GL_LESS: return aV < aRef;
	case GL_EQUAL: return aV == aRef;
	case GL_LEQUAL: return aV <= aRef;
	case GL_GREATER: return aV > aRef;
	case GL_NOTEQUAL: return aV != aRef;
	case GL_GEQUAL: return aV >= aRef;
	default: return 1;
		}
	}

/* attribute plane: value at (x0 + dx, y0 + dy) = a0 + dadx*dx + dady*dy */
typedef struct { int start, dx, dy; } SyStep;
typedef struct { float a, dx, dy; } SyPlaneF;

static void plane(float* aDx, float* aDy, float a0, float a1, float a2,
				  float e1x, float e1y, float e2x, float e2y, float aInvArea)
	{
	float d1 = a1 - a0, d2 = a2 - a0;
	*aDx = (d1 * e2y - d2 * e1y) * aInvArea;
	*aDy = (d2 * e1x - d1 * e2x) * aInvArea;
	}

static void setup_attr(SyStep* aS, float a0, float a1, float a2, float aScale,
					   float e1x, float e1y, float e2x, float e2y, float aInvArea, float aOffX, float aOffY)
	{
	float dadx, dady;
	plane(&dadx, &dady, a0 * aScale, a1 * aScale, a2 * aScale, e1x, e1y, e2x, e2y, aInvArea);
	aS->start = (int)(a0 * aScale + dadx * aOffX + dady * aOffY);
	aS->dx = (int)dadx;
	aS->dy = (int)dady;
	}

static void setup_plane_f(SyPlaneF* aP, float a0, float a1, float a2,
						  float e1x, float e1y, float e2x, float e2y, float aInvArea, float aOffX, float aOffY)
	{
	plane(&aP->dx, &aP->dy, a0, a1, a2, e1x, e1y, e2x, e2y, aInvArea);
	aP->a = a0 + aP->dx * aOffX + aP->dy * aOffY;		/* value at (minx + 0.5, miny + 0.5) */
	}

static int clamp255(int v) { return v < 0 ? 0 : (v > (255 << 16) ? 255 : v >> 16); }

/* texel fetch: aU/aV in 16.16 texel units */
static u32 texel(const SyRTex* t, int aU, int aV)
	{
	int w = t->w, h = t->h;
	if (!t->linear)
		{
		int x = aU >> 16, y = aV >> 16;
		if (t->clampS) x = x < 0 ? 0 : (x >= w ? w - 1 : x); else { x %= w; if (x < 0) x += w; }
		if (t->clampT) y = y < 0 ? 0 : (y >= h ? h - 1 : y); else { y %= h; if (y < 0) y += h; }
		return t->data[y * w + x];
		}
	int u = aU - 32768, v = aV - 32768;		/* texel centres */
	int x0 = u >> 16, y0 = v >> 16, fu = (u >> 8) & 0xff, fv = (v >> 8) & 0xff;
	int x1 = x0 + 1, y1 = y0 + 1;
	if (t->clampS) { x0 = x0 < 0 ? 0 : (x0 >= w ? w - 1 : x0); x1 = x1 < 0 ? 0 : (x1 >= w ? w - 1 : x1); }
	else { x0 %= w; if (x0 < 0) x0 += w; x1 %= w; if (x1 < 0) x1 += w; }
	if (t->clampT) { y0 = y0 < 0 ? 0 : (y0 >= h ? h - 1 : y0); y1 = y1 < 0 ? 0 : (y1 >= h ? h - 1 : y1); }
	else { y0 %= h; if (y0 < 0) y0 += h; y1 %= h; if (y1 < 0) y1 += h; }
	u32 a = t->data[y0 * w + x0], b = t->data[y0 * w + x1], c = t->data[y1 * w + x0], d = t->data[y1 * w + x1];
	u32 out = 0;
	for (int sh = 0; sh < 32; sh += 8)
		{
		int ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff, cc = (c >> sh) & 0xff, cd = (d >> sh) & 0xff;
		int top = ca * (256 - fu) + cb * fu, bot = cc * (256 - fu) + cd * fu;
		out |= (u32)(((top * (256 - fv) + bot * fv) >> 16) & 0xff) << sh;
		}
	return out;
	}

static int blend_factor(int aF, int sc, int sa, int dc, int da, int aIsAlpha)
	{
	switch (aF)
		{
	case GL_ZERO: return 0;
	case GL_ONE: return 255;
	case GL_SRC_COLOR: return sc;
	case GL_ONE_MINUS_SRC_COLOR: return 255 - sc;
	case GL_SRC_ALPHA: return sa;
	case GL_ONE_MINUS_SRC_ALPHA: return 255 - sa;
	case GL_DST_ALPHA: return da;
	case GL_ONE_MINUS_DST_ALPHA: return 255 - da;
	case GL_DST_COLOR: return dc;
	case GL_ONE_MINUS_DST_COLOR: return 255 - dc;
	case GL_SRC_ALPHA_SATURATE: return aIsAlpha ? 255 : (sa < 255 - da ? sa : 255 - da);
	default: return 255;
		}
	}

static int m255(int a, int b) { int x = a * b + 128; return (x + (x >> 8)) >> 8; }

/* the general fragment path: returns the 0x00RRGGBB to write, or -1 to discard (alpha test) */
static int shade(const SyRState* st, int r, int g, int b, int a, int u, int v, u32 aDst)
	{
	if (st->tex)
		{
		const SyRTex* t = st->tex;
		u32 tx = texel(t, u, v);
		int ta = (int)(tx >> 24), tr = (tx >> 16) & 0xff, tg = (tx >> 8) & 0xff, tb = tx & 0xff;
		switch (st->envMode)
			{
		case GL_REPLACE:
			if (t->hasRGB) { r = tr; g = tg; b = tb; }
			if (t->hasA) a = ta;
			break;
		case GL_DECAL:
			if (t->hasA && t->hasRGB)
				{ r = m255(r, 255 - ta) + m255(tr, ta); g = m255(g, 255 - ta) + m255(tg, ta); b = m255(b, 255 - ta) + m255(tb, ta); }
			else if (t->hasRGB) { r = tr; g = tg; b = tb; }
			break;
		case GL_BLEND:
			if (t->hasRGB)
				{
				r = m255(r, 255 - tr) + m255(st->envR, tr);
				g = m255(g, 255 - tg) + m255(st->envG, tg);
				b = m255(b, 255 - tb) + m255(st->envB, tb);
				}
			if (t->hasA) a = m255(a, ta);
			break;
		case GL_ADD:
			if (t->hasRGB) { r = imin(r + tr, 255); g = imin(g + tg, 255); b = imin(b + tb, 255); }
			if (t->hasA) a = m255(a, ta);
			break;
		default:	/* GL_MODULATE: formats without colour/alpha store 255 there */
			r = m255(r, tr); g = m255(g, tg); b = m255(b, tb); a = m255(a, ta);
			break;
			}
		}
	if (st->alphaTest && !cmp_pass(st->alphaFunc, (u32)a, (u32)st->alphaRef))
		return -1;
	if (st->blend)
		{
		int dr = (aDst >> 16) & 0xff, dg = (aDst >> 8) & 0xff, db = aDst & 0xff, da = 255;
		int fr = blend_factor(st->blendSrc, r, a, dr, da, 0), fg = blend_factor(st->blendSrc, g, a, dg, da, 0);
		int fb = blend_factor(st->blendSrc, b, a, db, da, 0);
		int gr = blend_factor(st->blendDst, r, a, dr, da, 0), gg = blend_factor(st->blendDst, g, a, dg, da, 0);
		int gb = blend_factor(st->blendDst, b, a, db, da, 0);
		r = imin(m255(r, fr) + m255(dr, gr), 255);
		g = imin(m255(g, fg) + m255(dg, gg), 255);
		b = imin(m255(b, fb) + m255(db, gb), 255);
		}
	return (r << 16) | (g << 8) | b;
	}

void sy_raster_triangle(SySurface* aS, const SyRState* aSt, const SyRVtx* v0, const SyRVtx* v1, const SyRVtx* v2)
	{
	int X0 = fx4(v0->x), Y0 = fx4(v0->y), X1 = fx4(v1->x), Y1 = fx4(v1->y), X2 = fx4(v2->x), Y2 = fx4(v2->y);
	int area = (X1 - X0) * (Y2 - Y0) - (Y1 - Y0) * (X2 - X0);
	if (area == 0)
		return;
	if (area < 0)	/* make it clockwise on screen (y down): positive area */
		{
		const SyRVtx* t = v1; v1 = v2; v2 = t;
		int tx = X1, ty = Y1; X1 = X2; Y1 = Y2; X2 = tx; Y2 = ty;
		}
	int cx0 = imax(0, aSt->scX0), cy0 = imax(0, aSt->scY0);
	int cx1 = imin(aS->w, aSt->scX1) - 1, cy1 = imin(aS->h, aSt->scY1) - 1;
	int minx = imax(cx0, imin(X0, imin(X1, X2)) >> 4);
	int maxx = imin(cx1, imax(X0, imax(X1, X2)) >> 4);
	int miny = imax(cy0, imin(Y0, imin(Y1, Y2)) >> 4);
	int maxy = imin(cy1, imax(Y0, imax(Y1, Y2)) >> 4);
	if (minx > maxx || miny > maxy)
		return;

	/* edge functions E_ab(P) = (Xb-Xa)*(Py-Ya) - (Yb-Ya)*(Px-Xa) at pixel centres; inside: all >= 0.
	   Top-left rule: an edge that is neither top nor left excludes pixels exactly on it (bias -1). */
	int px0 = minx * 16 + 8, py0 = miny * 16 + 8;
#define EDGE(xa, ya, xb, yb, E, SX, SY) \
	int E = (xb - xa) * (py0 - ya) - (yb - ya) * (px0 - xa); \
	int SX = -(yb - ya) * 16, SY = (xb - xa) * 16; \
	if (!((yb - ya) < 0 || ((yb - ya) == 0 && (xb - xa) > 0))) E -= 1;
	EDGE(X0, Y0, X1, Y1, E01, s01x, s01y)
	EDGE(X1, Y1, X2, Y2, E12, s12x, s12y)
	EDGE(X2, Y2, X0, Y0, E20, s20x, s20y)
#undef EDGE

	float e1x = v1->x - v0->x, e1y = v1->y - v0->y, e2x = v2->x - v0->x, e2y = v2->y - v0->y;
	float fa = e1x * e2y - e2x * e1y;
	if (fa == 0.0f)
		return;
	float inv = 1.0f / fa;
	float offx = (float)minx + 0.5f - v0->x, offy = (float)miny + 0.5f - v0->y;
	SyStep r, g, b, a, z;
	setup_attr(&r, v0->r, v1->r, v2->r, 65536.0f, e1x, e1y, e2x, e2y, inv, offx, offy);
	setup_attr(&g, v0->g, v1->g, v2->g, 65536.0f, e1x, e1y, e2x, e2y, inv, offx, offy);
	setup_attr(&b, v0->b, v1->b, v2->b, 65536.0f, e1x, e1y, e2x, e2y, inv, offx, offy);
	setup_attr(&z, v0->z, v1->z, v2->z, 65535.0f * 16384.0f, e1x, e1y, e2x, e2y, inv, offx, offy);

	int dtest = aSt->depthTest && aS->depth, dwrite = aSt->depthWrite && aS->depth, dfunc = aSt->depthFunc;
	int general = aSt->tex || aSt->blend || aSt->alphaTest;

	if (!general)
		{
		for (int y = miny; y <= maxy; ++y)
			{
			int e01 = E01, e12 = E12, e20 = E20;
			int cr = r.start, cg = g.start, cb = b.start, cz = z.start;
			u32* row = aS->color + y * aS->w;
			u16* zrow = aS->depth ? aS->depth + y * aS->w : 0;
			for (int x = minx; x <= maxx; ++x)
				{
				if ((e01 | e12 | e20) >= 0)
					{
					u32 zz = cz < 0 ? 0 : (u32)cz >> 14;
					if (zz > 0xffff) zz = 0xffff;
					if (!dtest || cmp_pass(dfunc, zz, zrow[x]))
						{
						if (aSt->writeColor)
							row[x] = ((u32)clamp255(cr) << 16) | ((u32)clamp255(cg) << 8) | (u32)clamp255(cb);
						if (dwrite)
							zrow[x] = (u16)zz;
						}
					}
				e01 += s01x; e12 += s12x; e20 += s20x;
				cr += r.dx; cg += g.dx; cb += b.dx; cz += z.dx;
				}
			E01 += s01y; E12 += s12y; E20 += s20y;
			r.start += r.dy; g.start += g.dy; b.start += b.dy; z.start += z.dy;
			}
		return;
		}

	setup_attr(&a, v0->a, v1->a, v2->a, 65536.0f, e1x, e1y, e2x, e2y, inv, offx, offy);
	SyPlaneF pw, ps, pt;
	float tw = 0.0f, th = 0.0f;
	if (aSt->tex)
		{
		tw = (float)aSt->tex->w * 65536.0f; th = (float)aSt->tex->h * 65536.0f;
		setup_plane_f(&pw, v0->iw, v1->iw, v2->iw, e1x, e1y, e2x, e2y, inv, offx, offy);
		setup_plane_f(&ps, v0->sw, v1->sw, v2->sw, e1x, e1y, e2x, e2y, inv, offx, offy);
		setup_plane_f(&pt, v0->tw, v1->tw, v2->tw, e1x, e1y, e2x, e2y, inv, offx, offy);
		}
	for (int y = miny; y <= maxy; ++y)
		{
		int e01 = E01, e12 = E12, e20 = E20;
		int cr = r.start, cg = g.start, cb = b.start, ca = a.start, cz = z.start;
		u32* row = aS->color + y * aS->w;
		u16* zrow = aS->depth ? aS->depth + y * aS->w : 0;
		float rowW = 0, rowS = 0, rowT = 0;
		if (aSt->tex)
			{
			float dy = (float)(y - miny);
			rowW = pw.a + pw.dy * dy; rowS = ps.a + ps.dy * dy; rowT = pt.a + pt.dy * dy;
			}
		int u = 0, v = 0, du = 0, dv = 0;
		for (int x = minx; x <= maxx; ++x)
			{
			if (aSt->tex && ((x - minx) % SPAN) == 0)
				{
				/* exact (u, v) at this pixel and SPAN pixels on; step linearly in between */
				int n = imin(SPAN, maxx - x + 1);
				float dx0 = (float)(x - minx), dx1 = dx0 + (float)n;
				float w0 = rowW + pw.dx * dx0, w1 = rowW + pw.dx * dx1;
				float iw0 = w0 != 0.0f ? 1.0f / w0 : 0.0f, iw1 = w1 != 0.0f ? 1.0f / w1 : 0.0f;
				int u0 = (int)((rowS + ps.dx * dx0) * iw0 * tw), v0i = (int)((rowT + pt.dx * dx0) * iw0 * th);
				int u1 = (int)((rowS + ps.dx * dx1) * iw1 * tw), v1i = (int)((rowT + pt.dx * dx1) * iw1 * th);
				u = u0; v = v0i; du = (u1 - u0) / n; dv = (v1i - v0i) / n;
				}
			if ((e01 | e12 | e20) >= 0)
				{
				u32 zz = cz < 0 ? 0 : (u32)cz >> 14;
				if (zz > 0xffff) zz = 0xffff;
				if (!dtest || cmp_pass(dfunc, zz, zrow[x]))
					{
					int c = shade(aSt, clamp255(cr), clamp255(cg), clamp255(cb), clamp255(ca), u, v, row[x]);
					if (c >= 0)
						{
						if (aSt->writeColor)
							row[x] = (u32)c;
						if (dwrite)
							zrow[x] = (u16)zz;
						}
					}
				}
			e01 += s01x; e12 += s12x; e20 += s20x;
			cr += r.dx; cg += g.dx; cb += b.dx; ca += a.dx; cz += z.dx;
			u += du; v += dv;
			}
		E01 += s01y; E12 += s12y; E20 += s20y;
		r.start += r.dy; g.start += g.dy; b.start += b.dy; a.start += a.dy; z.start += z.dy;
		}
	}

void sy_raster_clear(SySurface* aS, int aColor, u32 aRgb, int aDepth, u16 aZ, int aX, int aY, int aW, int aH)
	{
	int x0 = imax(0, aX), y0 = imax(0, aY), x1 = imin(aS->w, aX + aW), y1 = imin(aS->h, aY + aH);
	for (int y = y0; y < y1; ++y)
		{
		if (aColor)
			{
			u32* p = aS->color + y * aS->w;
			for (int x = x0; x < x1; ++x) p[x] = aRgb;
			}
		if (aDepth && aS->depth)
			{
			u16* q = aS->depth + y * aS->w;
			for (int x = x0; x < x1; ++x) q[x] = aZ;
			}
		}
	}
