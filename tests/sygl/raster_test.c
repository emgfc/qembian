/* host test of src/n95gl/raster.c (portable C): textured full-screen quad, MODULATE, no blend.
   Build: tests/sygl/run.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sygl_int.h"

#define GL_MODULATE 0x2100
#define GL_LESS 0x0201

int main(void)
	{
	enum { W = 240, H = 320, TW = 128, TH = 256 };
	SySurface s; memset(&s, 0, sizeof(s));
	s.w = W; s.h = H; s.color = calloc(W * H, 4); s.depth = 0;
	u32* tex = malloc(TW * TH * 4);
	for (int i = 0; i < TW * TH; ++i) tex[i] = 0xff181c18u;		/* dark, opaque */
	int failures = 0;
	for (int mode = 0; mode < 4; ++mode) {
	memset(s.color, 0, W * H * 4);
	SyRTex rt = { tex, 0, TW, TH, mode & 1, (mode & 2) != 0, (mode & 2) != 0, 1, 1, TW - 1, TH - 1 };
	SyRState st; memset(&st, 0, sizeof(st));
	st.tex = &rt; st.envMode = GL_MODULATE; st.depthFunc = GL_LESS; st.writeColor = 1;
	st.scX0 = 0; st.scY0 = 0; st.scX1 = W; st.scY1 = H;
	/* quad (strip 0 1 2 3) in window coords y down; s/t with w = 1 */
	SyRVtx v[4] = {
		{ 0, 0, 0.5f, 255, 255, 255, 255, 1, 0, 1 },
		{ W, 0, 0.5f, 255, 255, 255, 255, 1, 1, 1 },
		{ 0, H, 0.5f, 255, 255, 255, 255, 1, 0, 0 },
		{ W, H, 0.5f, 255, 255, 255, 255, 1, 1, 0 } };
	sy_raster_triangle(&s, &st, &v[0], &v[1], &v[2]);
	sy_raster_triangle(&s, &st, &v[1], &v[2], &v[3]);
	int bad = 0;
	for (int i = 0; i < W * H; ++i)
		if (s.color[i] != 0x181c18u) { if (bad < 5) printf("pixel %d,%d = %06x\n", i % W, i / W, s.color[i]); ++bad; }
	printf("%s: linear %d clamp %d: %d of %d pixels differ from the texel 181c18\n", bad ? "FAIL" : "ok",
		   mode & 1, (mode & 2) != 0, bad, W * H);
	failures += bad != 0;
	}
	return failures != 0;
	}
