/* syvg port of src/khr/hostraster.cpp (v1, guest): the RI fill on the GPU, run in QEMU. */
/*
 * Rasterizer for the Khronos OpenVG RI front end (replaces sfopenvg/riRasterizer.cpp).
 *
 * The RI's paths, strokes and image draws end here as an edge list with a fill rule, a viewport,
 * scissor rectangles and a pixel pipe (paint, blend mode, masking). The RI fills them in software;
 * ours collects the same edges and hands the fill to the host GPU (syborg-gl: stencil the edges,
 * then cover with the paint): colour, image draws, patterns, linear and radial gradients.
 * SPDX-License-Identifier: MIT
 */
#include "riRasterizer.h"
#include "riPixelPipe.h"
#include "symcompat.h"
#include "vghost.h"
#include "../qemu-syborg/hw/arm/syborg_gl_proto.h"

namespace OpenVGRI
{

/* bring-up statistics: the first fills of each kind and every reason a fill is not drawn
 * (KHR_FILLLOG=N at build time: src/khr/build.sh, env KHR_FILLLOG, logs the first N of each kind) */
#ifndef KHR_FILLLOG
#define KHR_FILLLOG 4
#endif
static int gLogFill[8];
#define FILLLOG(k, ...) do { if (gLogFill[k]++ < KHR_FILLLOG) RDebug::Printf(__VA_ARGS__); } while (0)

Rasterizer::Rasterizer() :
	m_edges(), m_scissorEdges(), m_scissor(false), m_samples(), m_numSamples(1), m_numFSAASamples(1),
	m_sumWeights(1.0f), m_sampleRadius(0.5f), m_vpx(0), m_vpy(0), m_vpwidth(0), m_vpheight(0),
	m_fillRule(VG_EVEN_ODD), m_pixelPipe(NULL), m_covBuffer(NULL)
{
	m_covMinx = m_covMiny = 0;
	m_covMaxx = m_covMaxy = -1;
	clear();
}

Rasterizer::~Rasterizer()
{
}

void Rasterizer::setup(int vpx, int vpy, int vpwidth, int vpheight, VGFillRule fillRule,
                       const PixelPipe *pixelPipe, RIuint32 *covBuffer)
{
	m_vpx = vpx;
	m_vpy = vpy;
	m_vpwidth = vpwidth;
	m_vpheight = vpheight;
	m_fillRule = fillRule;
	m_pixelPipe = pixelPipe;
	m_covBuffer = covBuffer;
	m_covMinx = vpx;
	m_covMiny = vpy;
	m_covMaxx = vpx + vpwidth - 1;
	m_covMaxy = vpy + vpheight - 1;
}

void Rasterizer::setScissor(const Array<Rectangle> &scissors)
{
	m_scissor = true;
	m_scissorEdges.clear();
	for (int i = 0; i < scissors.size(); i++) {      /* kept as rectangles' vertical edges, like the RI */
		if (scissors[i].width <= 0 || scissors[i].height <= 0)
			continue;
		ScissorEdge e;
		e.miny = scissors[i].y;
		e.maxy = scissors[i].y + scissors[i].height;
		e.x = scissors[i].x;
		e.direction = 1;
		m_scissorEdges.push_back(e);
		e.x = scissors[i].x + scissors[i].width;
		e.direction = -1;
		m_scissorEdges.push_back(e);
	}
}

void Rasterizer::clear()
{
	m_edges.clear();
	m_edgeMin.set(RI_FLOAT_MAX, RI_FLOAT_MAX);
	m_edgeMax.set(-RI_FLOAT_MAX, -RI_FLOAT_MAX);
	m_scissor = false;
}

void Rasterizer::addBBox(const Vector2 &v)
{
	if (v.x < m_edgeMin.x) m_edgeMin.x = v.x;
	if (v.y < m_edgeMin.y) m_edgeMin.y = v.y;
	if (v.x > m_edgeMax.x) m_edgeMax.x = v.x;
	if (v.y > m_edgeMax.y) m_edgeMax.y = v.y;
}

void Rasterizer::addEdge(const Vector2 &v0, const Vector2 &v1)
{
	/* unlike the RI's scanline fill, the host's stencil fans need every edge of the closed
	   contours, horizontal ones included */
	if (v0.x == v1.x && v0.y == v1.y)
		return;
	if (m_edges.size() >= RI_MAX_EDGES)
		return;                 /* the RI reports out of memory; we drop the rest */
	Edge e;
	if (v0.y <= v1.y) {
		e.v0.set(v0.x, v0.y);
		e.v1.set(v1.x, v1.y);
		e.direction = 1;
	} else {
		e.v0.set(v1.x, v1.y);
		e.v1.set(v0.x, v0.y);
		e.direction = -1;
	}
	addBBox(v0);
	addBBox(v1);
	m_edges.push_back(e);
}

int Rasterizer::setupSamplingPattern(VGRenderingQuality renderingQuality, int numFSAASamples)
{
	m_numSamples = 1;
	m_numFSAASamples = numFSAASamples > 0 ? numFSAASamples : 1;
	return m_numSamples;
}

void Rasterizer::fill()
{
	m_covMinx = m_covMiny = 0;
	m_covMaxx = m_covMaxy = -1;     /* the RI's coverage buffer is not used: nothing covered here */
	const PixelPipe *pp = m_pixelPipe;
	if (!pp || !pp->m_drawable)
		return;
	if (!m_edges.size()) {
		FILLLOG(0, "KHR: fill with no edges (an empty or degenerate path): nothing drawn");
		return;
	}
	Surface *target = pp->m_drawable->getColorBuffer();
	if (!target || !target->m_host) {
		FILLLOG(0, "KHR: fill %d edges into a guest drawable %dx%d (image %d): not drawn", m_edges.size(),
		        target ? target->getWidth() : 0, target ? target->getHeight() : 0, pp->m_image != 0);
		return;                 /* a guest-memory drawable (pixmap, image target): not drawn yet */
	}
	Color c(0, 0, 0, 1, Color::sRGBA);
	if (pp->m_paint)
		c = pp->m_paint->m_paintColor;
	/* image draw (vgDrawImage) or pattern paint: a host texture with the inverse mapping */
	unsigned int kind = 0, key = 0, mode = 0;
	const Paint *gp = NULL;
	Image *img = NULL;
	const Matrix3x3 *m = NULL;
	if (pp->m_image) {
		kind = 1;
		img = pp->m_image;
		m = &pp->m_surfaceToImageMatrix;
		mode = pp->m_imageMode == VG_DRAW_IMAGE_MULTIPLY ? 1 : pp->m_imageMode == VG_DRAW_IMAGE_STENCIL ? 2 : 0;
	} else if (pp->m_paint && pp->m_paint->m_paintType == VG_PAINT_TYPE_PATTERN && pp->m_paint->m_pattern) {
		kind = 2;
		img = pp->m_paint->m_pattern;
		m = &pp->m_surfaceToPaintMatrix;
		VGTilingMode t = pp->m_paint->m_patternTilingMode;
		mode = t == VG_TILE_PAD ? 1 : t == VG_TILE_REPEAT ? 2 : t == VG_TILE_REFLECT ? 3 : 0;
		c = pp->m_tileFillColor;
	} else if (pp->m_paint && pp->m_paint->m_colorRampStops.size() >= 2 &&
	           (pp->m_paint->m_paintType == VG_PAINT_TYPE_LINEAR_GRADIENT ||
	            pp->m_paint->m_paintType == VG_PAINT_TYPE_RADIAL_GRADIENT)) {
		gp = pp->m_paint;
		kind = gp->m_paintType == VG_PAINT_TYPE_LINEAR_GRADIENT ? 3 : 4;
		m = &pp->m_surfaceToPaintMatrix;
	}
	if ((kind == 1 || kind == 2) && !(key = khrHostImage(img)))
		return;
	FILLLOG(1 + kind, "KHR: fill kind %d, %d edges, blend %x, image %dx%d key %x, colour %08x, %s, box %d,%d..%d,%d in %dx%d (surface %x)",
	        kind, m_edges.size(), pp->m_blendMode, img ? img->getWidth() : 0, img ? img->getHeight() : 0, key,
	        khrHostArgb(c), m_fillRule == VG_NON_ZERO ? "nonzero" : "evenodd", (int)m_edgeMin.x, (int)m_edgeMin.y,
	        (int)m_edgeMax.x, (int)m_edgeMax.y, target->getWidth(), target->getHeight(), (unsigned int)(uintptr_t)target->m_host);
	int ns = m_scissor ? m_scissorEdges.size() / 2 : 0;
	if (m_scissor && !ns)
		return;                 /* scissoring with no rectangles: nothing visible */
	int nstops = gp ? gp->m_colorRampStops.size() : 0;
	int extra = kind >= 3 ? 17 + nstops * 5 : kind ? 14 : 0;
	if (pp->m_masking)
		FILLLOG(6, "KHR: fill with masking: drawn without the mask");
	int ct = pp->m_colorTransform ? 9 : 0;
	if (ct)
		FILLLOG(7, "KHR: fill with a colour transform");
	khrHostTargetSync(target);       /* the target image's pending CPU change first */
	unsigned int *p = khrHostEmit(SYGL_VG_FILL, 8 + extra + ns * 4 + m_edges.size() * 4 + ct);
	if (!p)
		return;
	p[0] = (unsigned int)(uintptr_t)target->m_host;
	p[1] = m_fillRule == VG_NON_ZERO ? 1 : 0;
	p[2] = pp->m_blendMode;
	p[3] = kind;
	p[4] = khrHostArgb(c);
	p[5] = extra;
	unsigned int *b = p + 6;
	if (kind == 1 || kind == 2) {
		b[0] = key; b[1] = img->getWidth(); b[2] = img->getHeight();
		b[3] = pp->m_imageQuality == VG_IMAGE_QUALITY_NONANTIALIASED ? 0 : 1;
		b[4] = mode;
		float *f = (float *)(b + 5);
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				f[i * 3 + j] = (*m)[i][j];
	} else if (kind >= 3) {
		float *f = (float *)b;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				f[i * 3 + j] = (*m)[i][j];
		VGColorRampSpreadMode sm = gp->m_colorRampSpreadMode;
		b[9] = sm == VG_COLOR_RAMP_SPREAD_REPEAT ? 1 : sm == VG_COLOR_RAMP_SPREAD_REFLECT ? 2 : 0;
		b[10] = gp->m_colorRampPremultiplied ? 1 : 0;
		float *g = (float *)(b + 11);
		if (kind == 3) {
			g[0] = gp->m_linearGradientPoint0.x; g[1] = gp->m_linearGradientPoint0.y;
			g[2] = gp->m_linearGradientPoint1.x; g[3] = gp->m_linearGradientPoint1.y; g[4] = 0;
		} else {
			g[0] = gp->m_radialGradientCenter.x; g[1] = gp->m_radialGradientCenter.y;
			g[2] = gp->m_radialGradientFocalPoint.x; g[3] = gp->m_radialGradientFocalPoint.y;
			g[4] = gp->m_radialGradientRadius;
		}
		b[16] = nstops;
		float *st = (float *)(b + 17);
		for (int i = 0; i < nstops; i++, st += 5) {     /* sRGBA, not premultiplied (riApi validates) */
			const Paint::GradientStop &gs = gp->m_colorRampStops[i];
			st[0] = gs.offset; st[1] = gs.color.r; st[2] = gs.color.g; st[3] = gs.color.b; st[4] = gs.color.a;
		}
	}
	p[6 + extra] = ns;
	int *r = (int *)p + 7 + extra;
	for (int i = 0; i < ns; i++, r += 4) {      /* setScissor() stored each rectangle as two edges */
		const ScissorEdge &a = m_scissorEdges[2 * i], &b = m_scissorEdges[2 * i + 1];
		r[0] = a.x; r[1] = a.miny; r[2] = b.x - a.x; r[3] = a.maxy - a.miny;
	}
	r[0] = m_edges.size();
	float *e = (float *)(r + 1);
	for (int i = 0; i < m_edges.size(); i++, e += 4) {   /* in their original direction (winding) */
		const Edge &d = m_edges[i];
		const RVector2 &a = d.direction > 0 ? d.v0 : d.v1, &b = d.direction > 0 ? d.v1 : d.v0;
		e[0] = a.x; e[1] = a.y; e[2] = b.x; e[3] = b.y;
	}
	if (ct) {                       /* VG_COLOR_TRANSFORM: scale r g b a, bias r g b a */
		unsigned int *t = (unsigned int *)e;
		t[0] = 1;
		for (int i = 0; i < 8; i++)
			((float *)t)[1 + i] = pp->m_colorTransformValues[i];
	}
}

}	/* namespace OpenVGRI */
