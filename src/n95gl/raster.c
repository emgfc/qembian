/* sygl rasterizer: triangles into a SySurface (32 bpp colour, 16-bit depth).
   Edge functions on 28.4 fixed-point vertices with the top-left fill rule. Attributes come from
   plane equations set up once per triangle in float and stepped per pixel in fixed point (colour
   16.16, depth 16.14: headroom for the bounding-box corners), so the inner loop has no soft-float work. Colours are affine (Gouraud);
   perspective-correct interpolation comes with texturing (milestone 2). */
#include "sygl_int.h"

#define GL_NEVER    0x0200
#define GL_LESS     0x0201
#define GL_EQUAL    0x0202
#define GL_LEQUAL   0x0203
#define GL_GREATER  0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL   0x0206
#define GL_ALWAYS   0x0207

static int fx4(float v) { return (int)(v * 16.0f + (v >= 0 ? 0.5f : -0.5f)); }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

static int depth_pass(int aFunc, u32 aZ, u32 aOld)
	{
	switch (aFunc)
		{
	case GL_NEVER: return 0;
	case GL_LESS: return aZ < aOld;
	case GL_EQUAL: return aZ == aOld;
	case GL_LEQUAL: return aZ <= aOld;
	case GL_GREATER: return aZ > aOld;
	case GL_NOTEQUAL: return aZ != aOld;
	case GL_GEQUAL: return aZ >= aOld;
	default: return 1;
		}
	}

/* attribute plane: value at (x0 + dx, y0 + dy) = a0 + dadx*dx + dady*dy */
typedef struct { int start, dx, dy; } SyStep;

static void setup_attr(SyStep* aS, float a0, float a1, float a2, float aScale,
					   float e1x, float e1y, float e2x, float e2y, float aInvArea, float aOffX, float aOffY)
	{
	float d1 = (a1 - a0) * aScale, d2 = (a2 - a0) * aScale;
	float dadx = (d1 * e2y - d2 * e1y) * aInvArea;
	float dady = (d2 * e1x - d1 * e2x) * aInvArea;
	aS->start = (int)(a0 * aScale + dadx * aOffX + dady * aOffY);
	aS->dx = (int)dadx;
	aS->dy = (int)dady;
	}

static u32 clamp255(int v) { return v < 0 ? 0 : (v > (255 << 16) ? 255u : (u32)v >> 16); }

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
		area = -area;
		}
	int minx = imax(0, (imin(X0, imin(X1, X2)) + 15) >> 4);
	int maxx = imin(aS->w - 1, (imax(X0, imax(X1, X2)) - 1) >> 4);
	int miny = imax(0, (imin(Y0, imin(Y1, Y2)) + 15) >> 4);
	int maxy = imin(aS->h - 1, (imax(Y0, imax(Y1, Y2)) - 1) >> 4);
	/* pixel centres: (px*16+8, py*16+8) */
	if (minx > maxx || miny > maxy)
		{
		/* thin triangles can still cover a centre at the box edge: widen by one on each side */
		minx = imax(0, imin(X0, imin(X1, X2)) >> 4); maxx = imin(aS->w - 1, imax(X0, imax(X1, X2)) >> 4);
		miny = imax(0, imin(Y0, imin(Y1, Y2)) >> 4); maxy = imin(aS->h - 1, imax(Y0, imax(Y1, Y2)) >> 4);
		if (minx > maxx || miny > maxy)
			return;
		}

	/* edge functions E_ab(P) = (Xb-Xa)*(Py-Ya) - (Yb-Ya)*(Px-Xa); inside: all >= 0.
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

	/* attribute planes in pixel units (float), relative to vertex 0 */
	float e1x = v1->x - v0->x, e1y = v1->y - v0->y, e2x = v2->x - v0->x, e2y = v2->y - v0->y;
	float fa = e1x * e2y - e2x * e1y;
	if (fa == 0.0f)
		return;
	float inv = 1.0f / fa;
	float offx = (float)minx + 0.5f - v0->x, offy = (float)miny + 0.5f - v0->y;
	SyStep r, g, b, z;
	setup_attr(&r, v0->r, v1->r, v2->r, 65536.0f, e1x, e1y, e2x, e2y, inv, offx, offy);
	setup_attr(&g, v0->g, v1->g, v2->g, 65536.0f, e1x, e1y, e2x, e2y, inv, offx, offy);
	setup_attr(&b, v0->b, v1->b, v2->b, 65536.0f, e1x, e1y, e2x, e2y, inv, offx, offy);
	setup_attr(&z, v0->z, v1->z, v2->z, 65535.0f * 16384.0f, e1x, e1y, e2x, e2y, inv, offx, offy);

	int dtest = aSt->depthTest && aS->depth, dwrite = aSt->depthWrite && aS->depth, dfunc = aSt->depthFunc;
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
				if (!dtest || depth_pass(dfunc, zz, zrow[x]))
					{
					row[x] = (clamp255(cr) << 16) | (clamp255(cg) << 8) | clamp255(cb);
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
