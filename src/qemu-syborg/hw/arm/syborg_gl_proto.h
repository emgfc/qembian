/*
 * syborg-gl command stream, shared by the guest (src/n95gl/hw.c, the sygl blob) and the QEMU device
 * (syborg_gl.c). docs/gpu-roadmap.md "Host GL backend".
 *
 * The guest keeps the whole GLES 1.1 state machine, transform, lighting and clipping (sygl); what
 * crosses is the rasteriser's input: window-space triangles with their raster state, clears,
 * texture images and read-backs. The device executes a buffer synchronously on a host GLES 2
 * context (one FBO per guest surface) when the guest writes EXEC.
 *
 * Registers (word index, MMIO 0xc000f000): 0 ID (SYGL_ID), 1 CAPS (read; bit 0 = host GL ready,
 * initialised on first read), 2 BUF (guest-physical address of the command buffer), 3 LEN (bytes),
 * 4 EXEC (write: run the buffer; read: result of the last run, 0 = ok, else the failing command's
 * word offset + 1), 5 OUT (guest-physical address of the output area for SYGL_READ), 6 OUTLEN,
 * 7 KICK (write: the guest-virtual address of a SyglKick in the writer's address space; the buffers
 * it names are virtual addresses in that same space, read and written in place by the device: no
 * copy into contiguous memory and, from a user-mapped register page, no kernel call. The device
 * translates through the vCPU's current page tables while the write runs).
 *
 * Every command is a run of 32-bit little-endian words: { op, words (including these two), args }.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SYBORG_GL_PROTO_H
#define SYBORG_GL_PROTO_H

#define SYGL_ID          0xc51d0011u    /* audio is ...10 */
#define SYGL_REG_ID      0
#define SYGL_REG_CAPS    1
#define SYGL_REG_BUF     2
#define SYGL_REG_LEN     3
#define SYGL_REG_EXEC    4
#define SYGL_REG_OUT     5
#define SYGL_REG_OUTLEN  6
#define SYGL_REG_KICK    7

/* SYGL_REG_KICK descriptor (guest-virtual, 4-byte aligned) */
typedef struct SyglKick {
    unsigned int buf, len;          /* command words (virtual address, bytes) */
    unsigned int out, outlen;       /* SYGL_READ output area (virtual address, bytes) */
    unsigned int result;            /* written back: 0 ok, failing word offset + 1, or ~0 (not mapped) */
} SyglKick;

enum {
    SYGL_SURF = 1,      /* id, w, h, depth (0/1): create, or reallocate at a new size */
    SYGL_SURF_FREE,     /* id */
    SYGL_TEX,           /* key, gen, w, h, y0, rows, then rows*w words 0xAARRGGBB (band of a w x h
                           image; y0 = 0 (re)defines the texture; big images come in several bands) */
    SYGL_TEX_FREE,      /* key */
    SYGL_CLEAR,         /* surf, flags (1 colour, 2 depth), rgb 0x00RRGGBB, z 0..65535, x, y, w, h */
    SYGL_DRAW,          /* surf, SyglState, n vertices, then n SyglVtx (triangle list) */
    SYGL_READ,          /* surf, byte offset in OUT, y0, rows: rows*w words 0x00RRGGBB, top row first */
    SYGL_OWNER,         /* owner: the guest process the following ids and keys belong to (every
                           buffer starts with it; ids and keys are addresses, unique per process) */
    SYGL_OWNER_FREE,    /* owner: free all of its surfaces and textures (eglTerminate) */
    /* OpenVG (src/khr, docs/devices/nokia-n8-rm596/vg-host.md): VG surface coordinates are GL's
       (origin bottom left, y up); colours are premultiplied 0xAARRGGBB */
    SYGL_VG_CLEAR = 10, /* surf, argb, n, then n rects {x, y, w, h} (n = 0: the whole surface) */
    SYGL_VG_FILL,       /* surf, rule (0 even-odd, 1 non-zero), blend (VGBlendMode), paint kind
                           (0 colour, 1 image draw, 2 pattern paint, 3 linear gradient, 4 radial
                           gradient), argb (paint colour; tile fill colour for patterns), paint
                           block length L, L words of paint block, nScissor, nScissor rects
                           {x, y, w, h} (0 = no scissoring), nEdges, nEdges edges {x0, y0, x1, y1} as
                           IEEE floats.
                           Paint block, kinds 1/2: texture key, w, h, quality (0 nearest, 1 linear),
                           image mode (0 normal, 1 multiply, 2 stencil) or tiling (0 fill, 1 pad,
                           2 repeat, 3 reflect), 3x3 surface-to-image/paint matrix (row major, floats;
                           surface pixel centres -> image pixel units).
                           Kinds 3/4: 3x3 surface-to-paint matrix, spread (0 pad, 1 repeat,
                           2 reflect), premultiplied ramp (0/1), 5 floats (linear x0 y0 x1 y1 -;
                           radial cx cy fx fy r), nStops, nStops x {offset, r, g, b, a} floats
                           (non-premultiplied sRGBA, offsets increasing) */
    SYGL_VG_SURF2TEX,   /* surf, key: the surface's pixels become texture `key` (an image render
                           target going away keeps what was drawn into it) */
    SYGL_VG_PRESENT,    /* surf, sid0..sid3: the surface's pixels become the scan-out image of
                           GCE surface sid (an EGL window's swap; composed by SYGL_COMPOSE) */
    SYGL_COMPOSE,       /* fb (virtual), fbW, fbH, fbStride (bytes; 32 bpp), nLayers | rotation << 24
                           (0..3: the screen's rotation, layer rectangles are in the rotated screen; 90CW
                           maps (x, y) to (fbW - y, x)), then nLayers SyglLayer, bottom first: composed
                           on the host and written to fb */
    SYGL_SCANOUT_FREE,  /* sid0..sid3: forget the host image of a GCE surface */
    SYGL_VG_TEXREAD,    /* key, byte offset in OUT, y0, rows: an image's host pixels (its live render
                           target key|1, else texture key) as R G B A bytes, premultiplied, row y0
                           first (VG rows: bottom up) */
    /* OpenGL ES 2.0, remoted (docs/devices/nokia-n8-rm596/gles2-host.md, guest src/khr/gl2.c, host
       syborg_gl2.c): ctx = the guest context's address (unique in the owner process) */
    SYGL_GL2 = 20,      /* ctx, fn (gl2_fn.h), the call's words; calls with results write them at OUT */
    SYGL_GL2_NEW,       /* ctx, share ctx (0: none)[, api: 1 = OpenGL ES 1.1, else 2]: a context (a host
                           context of its own) */
    SYGL_GL2_CURRENT,   /* ctx, surf (0: none), w, h: the default framebuffer is now that surface */
    SYGL_GL2_FREE,      /* ctx */
    /* OpenGL ES 1.1, remoted the same way (docs/devices/nokia-n8-rm596/gles1-host.md, guest src/khr/gl1.c,
       host syborg_gl1.h): its own functions; the calls it shares with ES 2 (buffers, textures, pixels,
       plain state) are SYGL_GL2 commands on the ES 1.1 context */
    SYGL_GL1,           /* ctx, fn (gl1_fn.h), the call's words */
    SYGL_GL_TEXIMAGE,   /* ctx, surf, w, h, rgba: eglBindTexImage: the surface's pixels into the texture
                           bound to the context's active unit (a copy) */
    /* EGL and OpenVG on the host (v2, docs/devices/nokia-n8-rm596/egl-vg-host-v2.md): one call each, in the
       calling thread's kick (SYGL_REG_KICK); pointers are its virtual addresses; the result word at OUT */
    SYGL_VG2 = 30,      /* fn (src/syvg/vg2_fn.h), guest thread id, the call's words */
    SYGL_EGL,           /* fn (src/syvg/egl2_fn.h), guest thread id, the call's words */
};

/* SYGL_COMPOSE layer: a GCE surface; its pixels come from its scan-out image (SYGL_VG_PRESENT) or,
   when it has none, from guest memory at va (re-read when dirty or not yet cached) */
typedef struct SyglLayer {
    unsigned int sid[4];
    int dx0, dy0, dx1, dy1;         /* destination rectangle on the screen */
    int sx0, sy0, sx1, sy1;         /* source rectangle in the surface */
    unsigned int alpha;             /* layer opacity 0..255 */
    unsigned int fmt;               /* 0 XRGB 8888, 1 ARGB 8888, 2 ARGB 8888 PRE, 3 RGB 565; bits 8..9:
                                       the source turned anticlockwise by 90 degrees that many times */
    unsigned int va, stride, w, h;  /* the current buffer in guest memory */
    unsigned int dirty;
} SyglLayer;
#define SYGL_LAYER_WORDS (sizeof(SyglLayer) / 4)
/* SYGL_VG_FILL image mode/tiling word: the texture `key` itself even if a render target surface
   key|1 exists (otherwise image draws sample the live render target) */
#define SYGL_VG_TEXTURE_ONLY 0x100u

/* SYGL_SURF depth word: bit 0 depth buffer, bit 1 stencil (a packed depth/stencil buffer) */
#define SYGL_SURF_DEPTH   1u
#define SYGL_SURF_STENCIL 2u
#define SYGL_SURF_MSAA    4u    /* multisampled (anti-aliased OpenVG fills); resolved when read */
/* SYGL_READ optional 5th word: bit 0 = keep alpha (words 0xAARRGGBB, rows bottom-up as GL) */
#define SYGL_READ_ALPHA   1u

/* raster state of a draw (mirrors sygl's SyRState); GL enums are the GLES values */
typedef struct SyglState {
    unsigned int depthTest, depthWrite, depthFunc;
    unsigned int texKey, texGen;    /* 0: untextured */
    unsigned int texHasRGB, texHasA;
    unsigned int texLinear, texClampS, texClampT;   /* sampler: may change without a new image */
    unsigned int envMode, envColor; /* envColor 0xAARRGGBB */
    unsigned int alphaTest, alphaFunc, alphaRef;   /* ref 0..255 */
    unsigned int blend, blendSrc, blendDst;
    int scX0, scY0, scX1, scY1;     /* drawable rect in surface pixels, y down, x1/y1 exclusive */
    unsigned int writeColor;
    unsigned int polyOffset;
    float polyFactor, polyUnits;    /* glPolygonOffset; units in 1/65535 depth steps */
    unsigned int fog, fogColor;     /* fogColor 0x00RRGGBB */
} SyglState;

/* a window-space vertex (sygl's SyRVtx): x, y in surface pixels (y down), z 0..1, colour 0..255,
   iw = 1/w, sw = s/w, tw = t/w, fog factor 0..255 (255 = no fog) */
typedef struct SyglVtx {
    float x, y, z, r, g, b, a, iw, sw, tw, fog, pad;
} SyglVtx;

#define SYGL_STATE_WORDS (sizeof(SyglState) / 4)
#define SYGL_VTX_WORDS   (sizeof(SyglVtx) / 4)

#endif
