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
 * word offset + 1), 5 OUT (guest-physical address of the output area for SYGL_READ), 6 OUTLEN.
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

enum {
    SYGL_SURF = 1,      /* id, w, h, depth (0/1): create, or reallocate at a new size */
    SYGL_SURF_FREE,     /* id */
    SYGL_TEX,           /* key, gen, w, h, y0, rows, then rows*w words 0xAARRGGBB (band of a w x h
                           image; y0 = 0 (re)defines the texture; big images come in several bands) */
    SYGL_TEX_FREE,      /* key */
    SYGL_CLEAR,         /* surf, flags (1 colour, 2 depth), rgb 0x00RRGGBB, z 0..65535, x, y, w, h */
    SYGL_DRAW,          /* surf, SyglState, n vertices, then n SyglVtx (triangle list) */
    SYGL_READ,          /* surf, byte offset in OUT, y0, rows: rows*w words 0x00RRGGBB, top row first */
    SYGL_OWNER,         /* owner: the guest thread the following ids and keys belong to (every
                           buffer starts with it; ids and keys are only unique per owner) */
    SYGL_OWNER_FREE,    /* owner: free all of its surfaces and textures (eglTerminate) */
};

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
