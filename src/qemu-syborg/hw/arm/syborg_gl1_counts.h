/* OpenGL ES 1.1 remoting (docs/devices/nokia-n8-rm596/gles1-host.md): how many values a vector parameter has,
   by pname. Shared by the guest (src/khr/gl1.c, gl1_gen.c: how many it sends) and the host (syborg_gl1.h:
   how many a query returns). SPDX-License-Identifier: MIT */
#ifndef SYBORG_GL1_COUNTS_H
#define SYBORG_GL1_COUNTS_H

/* OES_matrix_palette limits, as Broadcom's GLES 1.1 [inferred: its client code's GL11_CONFIG_*] */
#define GL1_MAX_PALETTE_MATRICES 32
#define GL1_MAX_VERTEX_UNITS 3

static inline int gl1_count_light(unsigned p)            /* glLight*v, glGetLight*v */
{
    switch (p) {
    case 0x1200: case 0x1201: case 0x1202: case 0x1203: return 4;   /* AMBIENT DIFFUSE SPECULAR POSITION */
    case 0x1204: return 3;                                          /* SPOT_DIRECTION */
    default: return 1;
    }
}

static inline int gl1_count_material(unsigned p)         /* glMaterial*v, glGetMaterial*v */
{
    switch (p) {
    case 0x1200: case 0x1201: case 0x1202: case 0x1600: case 0x1602: return 4;   /* + EMISSION, AMBIENT_AND_DIFFUSE */
    default: return 1;                                                           /* SHININESS */
    }
}

static inline int gl1_count_lightmodel(unsigned p) { return p == 0x0B53 ? 4 : 1; }   /* LIGHT_MODEL_AMBIENT */
static inline int gl1_count_fog(unsigned p) { return p == 0x0B66 ? 4 : 1; }          /* FOG_COLOR */
static inline int gl1_count_texenv(unsigned p) { return p == 0x2201 ? 4 : 1; }       /* TEXTURE_ENV_COLOR */
static inline int gl1_count_texparam(unsigned p) { return p == 0x8B9D ? 4 : 1; }     /* TEXTURE_CROP_RECT_OES */
static inline int gl1_count_pointparam(unsigned p) { return p == 0x8129 ? 3 : 1; }   /* POINT_DISTANCE_ATTENUATION */
static inline int gl1_count_texgen(unsigned p) { (void)p; return 1; }                /* TEXTURE_GEN_MODE_OES */

/* glGet{Boolean,Fixed,Float,Integer}v: values of a state variable (unknown: 1) */
static inline int gl1_count_get(unsigned p)
{
    switch (p) {
    case 0x0BA6: case 0x0BA7: case 0x0BA8:              /* MODELVIEW, PROJECTION, TEXTURE_MATRIX */
    case 0x898D: case 0x898E: case 0x898F:              /* the same as float bits (OES_matrix_get) */
        return 16;
    case 0x0C22: case 0x0C23: case 0x0B00: case 0x0B03: /* COLOR_CLEAR_VALUE, COLOR_WRITEMASK, CURRENT_COLOR, _TEXTURE_COORDS */
    case 0x0B66: case 0x0B53: case 0x0C10: case 0x0BA2: /* FOG_COLOR, LIGHT_MODEL_AMBIENT, SCISSOR_BOX, VIEWPORT */
        return 4;
    case 0x0B02: case 0x8129:                           /* CURRENT_NORMAL, POINT_DISTANCE_ATTENUATION */
        return 3;
    case 0x846D: case 0x846E: case 0x0B12: case 0x0B22: /* ALIASED/SMOOTH POINT_SIZE/LINE_WIDTH ranges */
    case 0x0B70: case 0x0D3A:                           /* DEPTH_RANGE, MAX_VIEWPORT_DIMS */
        return 2;
    default:
        return 1;
    }
}
#endif
