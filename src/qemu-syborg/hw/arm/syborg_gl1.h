/*
 * syborg-gl: OpenGL ES 1.1 replay (docs/devices/nokia-n8-rm596/gles1-host.md). Included by syborg_gl.c after
* syborg_gl2.h: an ES 1.1 guest context is a Gl2HostCtx with api 1 whose host context is a Mesa OpenGL ES
 * 1.1 context sharing with the device's context. The calls ES 1.1 shares with ES 2 come as SYGL_GL2 commands
 * (syborg_gl2.h runs them); its own are SYGL_GL1 {ctx, fn, words} (tools/gen-gl1.py generates the plain
 * ones: syborg_gl1_cases.h). Here: client arrays and draws, framebuffer objects (OES names: the only ones
 * an ES 1.1 context has), state queries, OES_mapbuffer reads, OES_query_matrix, and OES_matrix_palette,
 * which Mesa's ES 1.1 does not have: the host keeps the palette and skins the vertices of a draw on its
 * CPU (H-gles1-palette-cpu).
 * Only OpenGL ES 1.1 entry points may be called here (an ES 2-only one aborts QEMU in libepoxy).
 */
#include "syborg_gl1_fn.h"
#include "syborg_gl1_counts.h"

/* room for any glGet*v answer: the host GL writes all of a list (GL_COMPRESSED_TEXTURE_FORMATS: one word per
   format, Mesa's ES 3 has dozens), whatever the guest is told; only the first values go back */
#define GLES_GETMAX 512

enum { GL1_SLOT_VERTEX = 0, GL1_SLOT_NORMAL, GL1_SLOT_COLOR, GL1_SLOT_POINT_SIZE, GL1_SLOT_MATRIX_INDEX,
       GL1_SLOT_WEIGHT, GL1_SLOT_TEXCOORD0 = 8, GL1_SLOTS = 16 };

#define GL1_MATRIX_PALETTE 0x8840
#define GL1_CURRENT_PALETTE_MATRIX 0x8843
#define GL1_MAX_PALETTE 0x8842
#define GL1_MAX_UNITS 0x86A4
#define GL1_MATRIX_INDEX_ARRAY 0x8844
#define GL1_WEIGHT_ARRAY 0x86AD
#define GL1_MAP_READ 0x0001

typedef struct Gl1State {
    int palMode;                        /* the guest's matrix mode is MATRIX_PALETTE_OES (the host's is kept) */
    int palEnabled;                     /* GL_MATRIX_PALETTE_OES enabled */
    int palCur;                         /* CURRENT_PALETTE_MATRIX_OES */
    float pal[GL1_MAX_PALETTE_MATRICES][16];
    int idxOn, wOn;                     /* MATRIX_INDEX_ARRAY_OES, WEIGHT_ARRAY_OES enabled */
} Gl1State;

/* ---- 4x4 matrices, column-major as GL's */
static void m4_identity(float *m)
{
    for (int i = 0; i < 16; i++) {
        m[i] = i % 5 ? 0.0f : 1.0f;
    }
}

static void m4_mul(float *m, const float *b)    /* m = m * b */
{
    float r[16];
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            float v = 0;
            for (int k = 0; k < 4; k++) {
                v += m[k * 4 + row] * b[c * 4 + k];
            }
            r[c * 4 + row] = v;
        }
    }
    memcpy(m, r, sizeof(r));
}

static void m4_translate(float *m, float x, float y, float z)
{
    float t[16];
    m4_identity(t);
    t[12] = x; t[13] = y; t[14] = z;
    m4_mul(m, t);
}

static void m4_scale(float *m, float x, float y, float z)
{
    float t[16];
    m4_identity(t);
    t[0] = x; t[5] = y; t[10] = z;
    m4_mul(m, t);
}

static void m4_rotate(float *m, float deg, float x, float y, float z)
{
    float l = sqrtf(x * x + y * y + z * z);
    if (l == 0.0f) {
        return;
    }
    x /= l; y /= l; z /= l;
    float a = deg * (float)M_PI / 180.0f, c = cosf(a), s = sinf(a), ic = 1.0f - c;
    float t[16] = {
        x * x * ic + c, y * x * ic + z * s, x * z * ic - y * s, 0,
        x * y * ic - z * s, y * y * ic + c, y * z * ic + x * s, 0,
        x * z * ic + y * s, y * z * ic - x * s, z * z * ic + c, 0,
        0, 0, 0, 1 };
    m4_mul(m, t);
}

static void m4_frustum(float *m, float l, float r, float b, float t, float n, float f)
{
    if (l == r || b == t || n <= 0 || f <= 0 || n == f) {
        return;
    }
    float x[16] = { 2 * n / (r - l), 0, 0, 0, 0, 2 * n / (t - b), 0, 0,
                    (r + l) / (r - l), (t + b) / (t - b), -(f + n) / (f - n), -1, 0, 0, -2 * f * n / (f - n), 0 };
    m4_mul(m, x);
}

static void m4_ortho(float *m, float l, float r, float b, float t, float n, float f)
{
    if (l == r || b == t || n == f) {
        return;
    }
    float x[16] = { 2 / (r - l), 0, 0, 0, 0, 2 / (t - b), 0, 0, 0, 0, -2 / (f - n), 0,
                    -(r + l) / (r - l), -(t + b) / (t - b), -(f + n) / (f - n), 1 };
    m4_mul(m, x);
}

/* the inverse transpose of m's upper 3x3 (for normals), as 9 floats column-major */
static void m3_normal(const float *m, float *o)
{
    float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    float det = a * A + b * B + c * C;
    float k = det != 0.0f ? 1.0f / det : 0.0f;
    /* inverse = adj / det; its transpose's column-major layout is the cofactor matrix row by row */
    o[0] = A * k; o[1] = B * k; o[2] = C * k;
    o[3] = -(b * i - c * h) * k; o[4] = (a * i - c * g) * k; o[5] = -(a * h - b * g) * k;
    o[6] = (b * f - c * e) * k; o[7] = -(a * f - c * d) * k; o[8] = (a * e - b * d) * k;
}

static float fx2f(uint32_t x) { return (float)(int32_t)x / 65536.0f; }

static void gl1_state_init(Gl2HostCtx *c)
{
    Gl1State *g = g_new0(Gl1State, 1);
    for (int i = 0; i < GL1_MAX_PALETTE_MATRICES; i++) {
        m4_identity(g->pal[i]);
    }
    c->g1 = g;
}

/* ---- arrays */

static void gl1_set_pointer(int slot, int size, GLenum type, int stride, const void *p)
{
    switch (slot) {
    case GL1_SLOT_VERTEX: glVertexPointer(size, type, stride, p); break;
    case GL1_SLOT_NORMAL: glNormalPointer(type, stride, p); break;
    case GL1_SLOT_COLOR: glColorPointer(size, type, stride, p); break;
    case GL1_SLOT_POINT_SIZE: glPointSizePointerOES(type, stride, p); break;
    case GL1_SLOT_MATRIX_INDEX: case GL1_SLOT_WEIGHT: break;     /* the palette's: the host reads them */
    default:
        if (slot >= GL1_SLOT_TEXCOORD0 && slot < GL1_SLOTS) {
            GLint cat = GL_TEXTURE0;
            glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &cat);
            glClientActiveTexture(GL_TEXTURE0 + slot - GL1_SLOT_TEXCOORD0);
            glTexCoordPointer(size, type, stride, p);
            glClientActiveTexture(cat);
        }
        break;
    }
}

/* a draw's client arrays (the guest's rows first..: their base is the copy minus first rows) */
typedef struct Gl1Draw {
    const uint8_t *base[GL1_SLOTS];
    int size[GL1_SLOTS], type[GL1_SLOTS], stride[GL1_SLOTS];
} Gl1Draw;

static int gl1_type_size(GLenum t)
{
    return t == GL_BYTE || t == GL_UNSIGNED_BYTE ? 1 : t == GL_SHORT || t == GL_UNSIGNED_SHORT ? 2 : 4;
}

/* bytes [off, off + len) of the buffer bound to `target` (zeros past its end or if it cannot be read) */
static uint8_t *gl1_read_bound(GLenum target, size_t off, size_t len)
{
    uint8_t *r = g_malloc0(len ? len : 1);
    GLint size = 0;
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    if (off < (size_t)size && len) {
        size_t n = MIN(len, (size_t)size - off);
        void *m = glMapBufferRangeEXT(target, off, n, GL1_MAP_READ);
        if (m) {
            memcpy(r, m, n);
            glUnmapBufferOES(target);
        }
    }
    return r;
}

static uint8_t *gl1_read_buffer(GLuint buf, size_t off, size_t len)
{
    GLint ab = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    uint8_t *r = gl1_read_bound(GL_ARRAY_BUFFER, off, len);
    glBindBuffer(GL_ARRAY_BUFFER, ab);
    return r;
}

/* ---- OES_matrix_palette: skinning on the host CPU */

/* component i of an element (normals of byte/short type are normalized, as GL's fixed function does) */
static float gl1_comp(const uint8_t *p, GLenum type, int i, int norm)
{
    switch (type) {
    case GL_BYTE: { float v = (int8_t)p[i]; return norm ? fmaxf(v / 127.0f, -1.0f) : v; }
    case GL_UNSIGNED_BYTE: return norm ? p[i] / 255.0f : p[i];
    case GL_SHORT: { int16_t v; memcpy(&v, p + 2 * i, 2); return norm ? fmaxf(v / 32767.0f, -1.0f) : v; }
    case GL_FIXED: { int32_t v; memcpy(&v, p + 4 * i, 4); return v / 65536.0f; }
    default: { float v; memcpy(&v, p + 4 * i, 4); return v; }
    }
}

/* one source array of a draw over rows lo..hi: the client copy, or the rows read from its buffer */
typedef struct Gl1Src {
    const uint8_t *base;                /* row 0 (may lie before the data: only lo..hi are read) */
    uint8_t *own;                       /* read from a buffer: freed after the draw */
    int size, type, stride;
} Gl1Src;

static int gl1_source(Gl2HostCtx *c, Gl1Draw *d, int slot, int lo, int hi, Gl1Src *o)
{
    memset(o, 0, sizeof(*o));
    if (d->base[slot]) {
        o->base = d->base[slot]; o->size = d->size[slot]; o->type = d->type[slot]; o->stride = d->stride[slot];
        return 1;
    }
    Gl1Array *r = &c->arr[slot];
    if (!r->buf) {
        return 0;
    }
    int elem = r->size * gl1_type_size(r->type);
    int stride = r->stride ? r->stride : elem;
    size_t len = (size_t)(hi - lo) * stride + elem;
    o->own = gl1_read_buffer(r->buf, r->off + (size_t)lo * stride, len);
    o->base = o->own - (size_t)lo * stride;
    o->size = r->size; o->type = r->type; o->stride = stride;
    return 1;
}

typedef struct Gl1Skin {
    float *pos, *nrm;
    int tempNormals;                    /* the normal array was enabled for the skinned normals */
    GLint mode;
} Gl1Skin;

/* the vertices lo..hi blended by their palette matrices (eye space), drawn with an identity modelview */
static int gl1_skin(Gl2HostCtx *c, Gl1Draw *d, int lo, int hi, Gl1Skin *k)
{
    Gl1State *g = c->g1;
    memset(k, 0, sizeof(*k));
    Gl1Src v, nm, ix, wt;
    if (hi < lo || hi - lo > (1 << 20) || !gl1_source(c, d, GL1_SLOT_VERTEX, lo, hi, &v)) {
        return -1;
    }
    int hasN = glIsEnabled(GL_NORMAL_ARRAY) && gl1_source(c, d, GL1_SLOT_NORMAL, lo, hi, &nm);
    int hasI = g->idxOn && gl1_source(c, d, GL1_SLOT_MATRIX_INDEX, lo, hi, &ix);
    int hasW = g->wOn && gl1_source(c, d, GL1_SLOT_WEIGHT, lo, hi, &wt);
    int units = hasI ? ix.size : 1;
    if (hasW && wt.size < units) {
        units = wt.size;
    }
    float cur[4] = { 0, 0, 1, 0 };
    if (!hasN) {
        glGetFloatv(GL_CURRENT_NORMAL, cur);
    }
    float nmat[GL1_MAX_PALETTE_MATRICES][9];
    for (int i = 0; i < GL1_MAX_PALETTE_MATRICES; i++) {
        m3_normal(g->pal[i], nmat[i]);
    }
    int count = hi - lo + 1;
    k->pos = g_malloc0((size_t)count * 16);
    k->nrm = g_malloc0((size_t)count * 12);
    for (int r = 0; r < count; r++) {
        int row = lo + r;
        const uint8_t *pv = v.base + (size_t)row * v.stride;
        float p[4] = { 0, 0, 0, 1 }, n[3] = { cur[0], cur[1], cur[2] };
        for (int i = 0; i < v.size && i < 4; i++) {
            p[i] = gl1_comp(pv, v.type, i, 0);
        }
        if (hasN) {
            const uint8_t *pn = nm.base + (size_t)row * nm.stride;
            for (int i = 0; i < 3; i++) {
                n[i] = gl1_comp(pn, nm.type, i, 1);
            }
        }
        float *op = k->pos + r * 4, *on = k->nrm + r * 3;
        for (int u = 0; u < units; u++) {
            int m = hasI ? (int)gl1_comp(ix.base + (size_t)row * ix.stride, ix.type, u, 0) : 0;
            float w = hasW ? gl1_comp(wt.base + (size_t)row * wt.stride, wt.type, u, 0) : (u ? 0.0f : 1.0f);
            if (m < 0 || m >= GL1_MAX_PALETTE_MATRICES || w == 0.0f) {
                continue;
            }
            const float *M = g->pal[m], *N = nmat[m];
            for (int i = 0; i < 4; i++) {
                op[i] += w * (M[i] * p[0] + M[4 + i] * p[1] + M[8 + i] * p[2] + M[12 + i] * p[3]);
            }
            for (int i = 0; i < 3; i++) {
                on[i] += w * (N[i] * n[0] + N[3 + i] * n[1] + N[6 + i] * n[2]);
            }
        }
    }
    g_free(v.own);
    if (hasN) {
        g_free(nm.own);
    }
    if (hasI) {
        g_free(ix.own);
    }
    if (hasW) {
        g_free(wt.own);
    }
    GLint ab = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexPointer(4, GL_FLOAT, 0, k->pos - (size_t)lo * 4);
    glNormalPointer(GL_FLOAT, 0, k->nrm - (size_t)lo * 3);
    glBindBuffer(GL_ARRAY_BUFFER, ab);
    if (!glIsEnabled(GL_NORMAL_ARRAY)) {
        glEnableClientState(GL_NORMAL_ARRAY);
        k->tempNormals = 1;
    }
    glGetIntegerv(GL_MATRIX_MODE, &k->mode);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    return 0;
}

static void gl1_unskin(Gl2HostCtx *c, Gl1Skin *k)
{
    glPopMatrix();
    glMatrixMode(k->mode);
    if (k->tempNormals) {
        glDisableClientState(GL_NORMAL_ARRAY);
    }
    /* the vertex and normal arrays as the guest set them (client arrays come again with the next draw) */
    for (int slot = GL1_SLOT_VERTEX; slot <= GL1_SLOT_NORMAL; slot++) {
        Gl1Array *r = &c->arr[slot];
        if (r->buf) {
            GLint ab = 0;
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab);
            glBindBuffer(GL_ARRAY_BUFFER, r->buf);
            gl1_set_pointer(slot, r->size, r->type, r->stride, (const void *)r->off);
            glBindBuffer(GL_ARRAY_BUFFER, ab);
        }
    }
    g_free(k->pos);
    g_free(k->nrm);
}

static void gl1_index_range(const uint8_t *idx, GLenum type, uint32_t count, int *lo, int *hi)
{
    int mn = 0x7fffffff, mx = 0;
    for (uint32_t i = 0; i < count; i++) {
        int v;
        if (type == GL_UNSIGNED_BYTE) {
            v = idx[i];
        } else if (type == GL_UNSIGNED_SHORT) {
            uint16_t x;
            memcpy(&x, idx + 2 * i, 2);
            v = x;
        } else {
            uint32_t x;
            memcpy(&x, idx + 4 * i, 4);
            v = (int)MIN(x, 0x7fffffffu);
        }
        mn = MIN(mn, v);
        mx = MAX(mx, v);
    }
    *lo = count ? mn : 0;
    *hi = count ? mx : 0;
}

/* ---- queries */

static void gl1_get(SyborgGlState *s, Gl2HostCtx *c, uint32_t fn, GLenum p, uint32_t *v, int *cnt)
{
    Gl1State *g = c->g1;
    int emul = 1;
    GLint e = 0;
    *cnt = gl1_count_get(p);
    switch (p) {
    case GL_MATRIX_MODE: if (g->palMode) { e = GL1_MATRIX_PALETTE; } else { emul = 0; } break;
    case GL1_MAX_PALETTE: e = GL1_MAX_PALETTE_MATRICES; break;
    case GL1_MAX_UNITS: e = GL1_MAX_VERTEX_UNITS; break;
    case GL1_CURRENT_PALETTE_MATRIX: e = g->palCur; break;
    case GL1_MATRIX_PALETTE: e = g->palEnabled; break;
    case GL1_MATRIX_INDEX_ARRAY: e = g->idxOn; break;
    case GL1_WEIGHT_ARRAY: e = g->wOn; break;
    case 0x898D: case 0x898E: case 0x898F: {   /* OES_matrix_get: the matrix's float bits */
        float m[16];
        glGetFloatv(p == 0x898D ? GL_MODELVIEW_MATRIX : p == 0x898E ? GL_PROJECTION_MATRIX : GL_TEXTURE_MATRIX, m);
        if (p == 0x898D && g->palMode) {
            memcpy(m, g->pal[g->palCur], sizeof(m));
        }
        memcpy(v, m, sizeof(m));
        return;
    }
    default: emul = 0; break;
    }
    if (emul) {
        if (fn == GL1_FN_glGetFloatv) {
            float f = (float)e;
            memcpy(v, &f, 4);
        } else if (fn == GL1_FN_glGetFixedv) {
            v[0] = (uint32_t)(e * 65536);
        } else {
            v[0] = fn == GL1_FN_glGetBooleanv ? e != 0 : (uint32_t)e;
        }
        *cnt = 1;
        return;
    }
    if (p == GL_COMPRESSED_TEXTURE_FORMATS) {
        GLint k = 0;
        glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &k);
        *cnt = MIN(k, GLES_GETMAX);
    }
    if (fn == GL1_FN_glGetFloatv) {
        glGetFloatv(p, (GLfloat *)v);
    } else if (fn == GL1_FN_glGetFixedv) {
        glGetFixedv(p, (GLfixed *)v);
    } else if (fn == GL1_FN_glGetBooleanv) {
        GLboolean b[GLES_GETMAX] = { 0 };
        glGetBooleanv(p, b);
        for (int i = 0; i < *cnt; i++) {
            v[i] = b[i];
        }
    } else {
        glGetIntegerv(p, (GLint *)v);
        switch (p) {                    /* names as the guest knows them */
        case GL_TEXTURE_BINDING_2D: case 0x8514: v[0] = gl2_guest(c, GL2_N_TEX, v[0]); break;
        case 0x8CA7: v[0] = gl2_guest(c, GL2_N_RBO, v[0]); break;  /* RENDERBUFFER_BINDING_OES */
        case 0x8CA6: v[0] = c->fbo; break;                         /* FRAMEBUFFER_BINDING_OES */
        }
    }
    (void)s;
}

