/* Host-only test of the syvg library (the OpenVG RI front end built for the host): a drawable and a context
   made by hand (as the host EGL will), a clear and a filled rectangle, pixels checked.
   Build/run: tests/syvg/run.sh. SPDX-License-Identifier: MIT */
#include "riDefs.h"
#include "riContext.h"
#include "riImage.h"
#include "VG/openvg.h"
#include "VG/vgu.h"
#include "syvg.h"
#include <cstdio>

using namespace OpenVGRI;
static VGContext *gCtx;
extern "C" void *syvg_current_context(void) { return gCtx; }
extern "C" int syvg_image_in_use(void *) { return 0; }

int main()
{
	const int W = 64, H = 48;
	Drawable *d = new Drawable(Color::formatToDescriptor(VG_sRGBA_8888), W, H, 1, 0);
	d->addReference();
	gCtx = new VGContext(NULL);
	gCtx->setDefaultDrawable(d);
	VGfloat blue[4] = { 0, 0, 1, 1 }, red[4] = { 1, 0, 0, 1 };
	vgSetfv(VG_CLEAR_COLOR, 4, blue);
	vgClear(0, 0, W, H);
	VGPath p = vgCreatePath(VG_PATH_FORMAT_STANDARD, VG_PATH_DATATYPE_F, 1, 0, 0, 0, VG_PATH_CAPABILITY_ALL);
	vguRect(p, 16, 12, 32, 24);
	VGPaint paint = vgCreatePaint();
	vgSetParameterfv(paint, VG_PAINT_COLOR, 4, red);
	vgSetPaint(paint, VG_FILL_PATH);
	vgDrawPath(p, VG_FILL_PATH);
	unsigned int px[W * H];
	vgReadPixels(px, W * 4, VG_sRGBA_8888, 0, 0, W, H);
	unsigned int in = px[24 * W + 32], out = px[2 * W + 2];
	printf("handles: path %#lx paint %#lx (pointer-sized), error %#x\n", (unsigned long)p, (unsigned long)paint, vgGetError());
	printf("inside %08x (want ff0000ff), outside %08x (want 0000ffff)\n", in, out);
	vgDestroyPath(p);
	vgDestroyPaint(paint);
	bool ok = in == 0xff0000ffu && out == 0x0000ffffu;
	puts(ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
