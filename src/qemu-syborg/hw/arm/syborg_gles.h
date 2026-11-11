/*
 * syborg-gl: OpenGL ES 1.1 and 2.0 executed for the guest (docs/devices/nokia-n8-rm596/gles-host.md). The
 * guest only forwards (tools/gen-gles.py stubs): each call is {ctx, fn, words}, pointers are the caller's
 * virtual addresses, and a call with a pointer or a result runs while the caller waits, in its kick, so its
 * memory is read and written here (gmem_rw). Everything else is decided here: the size of each pointer's
 * data, names, vertex arrays, strings and limits (the device profile's [gl.identity], -global
 * syborg-gl.identity=FILE), errors, framebuffer 0, mapped buffers, the matrix palette (syborg_gl1.h).
 * Included by syborg_gl.c after syborg_gl2.h (contexts, names) and syborg_gl1.h (ES 1.1 helpers).
 * Under an ES 1.1 context only ES 1.1 entry points may be called (others abort QEMU in libepoxy).
 */
#include "syborg_gl1_fn.h"
#include "syborg_gl2_fn.h"

/* ---- the caller's memory */

static void *gles_in(SyborgGlState *s, uint32_t va, size_t n)
{
    if (!va || n > (64u << 20)) {
        return NULL;
    }
    void *h = g_malloc(n ? n : 1);
    if (n && gmem_rw(s, va, h, n, false)) {
        g_free(h);
        return NULL;
    }
    return h;
}

static void *gles_room(size_t n)
{
    return g_malloc0(n ? n : 1);
}

static void gles_put(SyborgGlState *s, uint32_t va, const void *h, size_t n)
{
    if (va && h && n) {
        gmem_rw(s, va, (void *)h, n, true);
    }
}

/* a NUL-terminated string of the caller (at most 1 MB) */
static char *gles_string(SyborgGlState *s, uint32_t va)
{
    if (!va) {
        return NULL;
    }
    GString *g = g_string_new(NULL);
    char b[256];
    while (g->len < (1u << 20)) {
        size_t chunk = 256 - (va & 255);       /* never past a page boundary in one read */
        if (gmem_rw(s, va, b, chunk, false)) {
            break;
        }
        char *z = memchr(b, 0, chunk);
        if (z) {
            g_string_append_len(g, b, z - b);
            return g_string_free(g, FALSE);
        }
        g_string_append_len(g, b, chunk);
        va += chunk;
    }
    g_string_free(g, TRUE);
    return NULL;
}

/* the call's result: word 0 of the output area */
static int gles_ret(SyborgGlState *s, uint32_t v)
{
    return gl2_out(s, &v, 4) ? 1 : 0;
}

static void gles_bad(SyborgGlState *s, const char *fn)
{
    static int logs;
    if (logs++ < 16) {
        warn_report("syborg-gl: %s: the caller's memory could not be read", fn);
    }
}

static int gles_bpp(GLenum fmt, GLenum type)
{
    int comps;
    switch (fmt) {
    case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT: case 0x1903 /* RED */: comps = 1; break;
    case GL_LUMINANCE_ALPHA: case 0x8227 /* RG */: case GL_DEPTH_STENCIL: comps = 2; break;
    case GL_RGB: comps = 3; break;
    case GL_RGBA: case 0x80E1 /* BGRA_EXT */: comps = 4; break;
    default: return 0;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: return comps;
    case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_5_6_5: return 2;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case 0x8D61 /* HALF_FLOAT_OES */: case GL_HALF_FLOAT: return 2 * comps;
    case GL_UNSIGNED_INT_24_8: return 4;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return 4 * comps;
    default: return 0;
    }
}

/* bytes of a w x h x d image in the caller's memory, rows aligned as the context's alignment says */
static size_t gles_pix_bytes(GLint w, GLint h, GLint d, GLenum fmt, GLenum type, GLenum alignment)
{
    int bpp = gles_bpp(fmt, type);
    if (w <= 0 || h <= 0 || d <= 0 || !bpp) {
        return 0;
    }
    GLint al = 4;
    glGetIntegerv(alignment, &al);
    if (al < 1) {
        al = 1;
    }
    size_t row = ((size_t)w * bpp + al - 1) / al * al;
    return row * ((size_t)h * d - 1) + (size_t)w * bpp;
}

/* bytes [off, off + len) of the buffer bound to `target` in an ES 2 guest context (the host's ES 3) */
static uint8_t *gl2_read_bound(GLenum target, size_t off, size_t len)
{
    uint8_t *r = g_malloc0(len ? len : 1);
    GLint size = 0;
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    if (off < (size_t)size && len) {
        size_t n = MIN(len, (size_t)size - off);
        void *m = glMapBufferRange(target, off, n, GL_MAP_READ_BIT);
        if (m) {
            memcpy(r, m, n);
            glUnmapBuffer(target);
        }
    }
    return r;
}

/* ---- the device's GL identity (the profile's [gl.identity], written by tools/device.py at each launch) */

static GHashTable *gles_ident;

static void gles_ident_load(const char *path)
{
    gles_ident = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_autofree char *txt = NULL;
    if (!path || !*path || !g_file_get_contents(path, &txt, NULL, NULL)) {
        if (path && *path) {
            warn_report("syborg-gl: identity %s: not readable", path);
        }
        return;
    }
    g_auto(GStrv) lines = g_strsplit(txt, "\n", -1);
    for (char **l = lines; *l; l++) {
        char *eq = strchr(*l, '=');
        if (eq && **l != '#') {
            g_hash_table_insert(gles_ident, g_strndup(*l, eq - *l), g_strdup(eq + 1));
        }
    }
}

static const char *gles_id(const char *key)
{
    const char *v = gles_ident ? g_hash_table_lookup(gles_ident, key) : NULL;
    return v ? v : "";
}

/* for the host EGL (src/syvg/syvg_egl.cpp) */
const char *syborg_gl_identity(const char *key)
{
    return gles_id(key);
}

/* eglGetProcAddress: whether the replay has the function: 1 OpenGL ES 1.1 (also an *OES alias of an ES 1.1
   function), 2 OpenGL ES 2.0, 3 both */
int syborg_gl_has_proc(const char *name)
{
    int mask = 0;
    static const char *const n1[] = { GL1_FN_NAMES }, *const n2[] = { GL2_FN_NAMES };
    size_t len = strlen(name);
    for (int k = 0; k < 2; k++) {
        const char *const *t = k ? n2 : n1;
        int cnt = k ? GL2_FN_COUNT : GL1_FN_COUNT;
        for (int i = 1; i < cnt; i++) {
            size_t l = strlen(t[i]);
            if ((l == len && !strcmp(t[i], name)) ||
                (k == 0 && len == l + 3 && !strncmp(t[i], name, l) && !strcmp(name + l, "OES"))) {
                mask |= 1 << k;
            }
        }
    }
    return mask;
}

/* glGetString: [length, bytes] in the output area (the guest keeps a copy) */
static int gles_get_string(SyborgGlState *s, Gl2HostCtx *c, GLenum name)
{
    const char *v = NULL;
    int es1 = c->api == 1;
    switch (name) {
    case GL_VENDOR: v = gles_id("vendor"); break;
    case GL_RENDERER: v = gles_id("renderer"); break;
    case GL_VERSION: v = es1 ? gles_id("version") : gles_id("gles2_version"); break;
    case GL_EXTENSIONS: v = es1 ? gles_id("extensions") : gles_id("gles2_extensions"); break;
    case GL_SHADING_LANGUAGE_VERSION: v = es1 ? NULL : gles_id("glsl_version"); break;
    }
    uint32_t buf[1 + 1024];
    if (!v) {
        c->err = c->err ? c->err : GL_INVALID_ENUM;
        buf[0] = ~0u;
        return gl2_out(s, buf, 4) ? 1 : 0;
    }
    size_t len = MIN(strlen(v), sizeof(buf) - 4);
    buf[0] = len;
    memcpy(buf + 1, v, len);
    return gl2_out(s, buf, 4 + len) ? 1 : 0;
}

/* ---- names: handed out here, per share group (programs and shaders share one counter, as in GL) */

static uint32_t gles_new_name(Gl2HostCtx *c, int kind)
{
    int k = kind == GL2_N_SHADER ? GL2_N_PROG : kind;
    Gl2HostCtx *r = kind == GL2_N_FBO ? c : c->root;
    uint32_t n;
    do {
        n = ++r->next[k];
    } while (g_hash_table_lookup(gl2_map(c, kind), GUINT_TO_POINTER(n)));
    return n;
}

static void gles_gen(SyborgGlState *s, Gl2HostCtx *c, int kind, const uint32_t *a, size_t n)
{
    if (n < 2 || (int32_t)a[0] < 0 || a[0] > (1u << 20)) {
        c->err = c->err ? c->err : GL_INVALID_VALUE;
        return;
    }
    uint32_t *names = g_new(uint32_t, a[0] ? a[0] : 1);
    for (uint32_t i = 0; i < a[0]; i++) {
        names[i] = gles_new_name(c, kind);
        gl2_name(c, kind, names[i], 1);
    }
    gles_put(s, a[1], names, a[0] * 4);
    g_free(names);
}

static void gles_unbind_buffer(Gl2HostCtx *c, GLuint h)
{
    for (int i = 0; i < 16; i++) {
        if (c->arr[i].buf == h) {
            c->arr[i].buf = 0;
            c->arr[i].va = 0;
        }
    }
}

static void gles_delete(SyborgGlState *s, Gl2HostCtx *c, int kind, const uint32_t *a, size_t n)
{
    if (n < 2 || (int32_t)a[0] < 0 || a[0] > (1u << 20)) {
        return;
    }
    uint32_t *names = gles_in(s, a[1], (size_t)a[0] * 4);
    for (uint32_t i = 0; names && i < a[0]; i++) {
        GLuint h = gl2_name(c, kind, names[i], 0);
        if (!h) {
            continue;
        }
        switch (kind) {
        case GL2_N_BUF: gles_unbind_buffer(c, h); glDeleteBuffers(1, &h); break;
        case GL2_N_TEX: glDeleteTextures(1, &h); break;
        case GL2_N_FBO:
            if (c->api == 1) {
                glDeleteFramebuffersOES(1, &h);
            } else {
                glDeleteFramebuffers(1, &h);
            }
            if (c->fbo == names[i]) {
                c->fbo = 0;
                gl2_bind_fbo(c, c->surfFbo);
            }
            break;
        default:
            if (c->api == 1) {
                glDeleteRenderbuffersOES(1, &h);
            } else {
                glDeleteRenderbuffers(1, &h);
            }
            break;
        }
        gl2_unmap(c, kind, names[i]);
    }
    g_free(names);
}

/* ---- vertex arrays and draws */

/* gl*Pointer / glVertexAttribPointer: a buffer array is set in the host context now; a client array is the
   caller's memory, read at each draw */
static void gles_pointer(Gl2HostCtx *c, int slot, int size, GLenum type, int norm, int stride, uint32_t ptr)
{
    if (slot < 0 || slot >= 16) {
        c->err = c->err ? c->err : GL_INVALID_VALUE;
        return;
    }
    struct GlesArray *r = &c->arr[slot];
    GLint ab = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab);
    r->size = size; r->type = type; r->norm = norm; r->stride = stride;
    r->buf = ab;
    r->va = ab ? 0 : ptr;
    r->off = ab ? ptr : 0;
    if (!ab) {
        return;
    }
    if (c->api == 2) {
        glVertexAttribPointer(slot, size, type, norm, stride, (const void *)(uintptr_t)ptr);
    } else {
        gl1_set_pointer(slot, size, type, stride, (const void *)(uintptr_t)ptr);
    }
}

/* the client arrays of a draw over rows lo..hi, read from the caller and set as host client arrays; the
   copies are freed after the draw (gles_draw_done) */
typedef struct GlesDraw {
    Gl1Draw d;                          /* ES 1.1 palette skinning reads these */
    void *copy[16];
} GlesDraw;

static int gles_draw_arrays(SyborgGlState *s, Gl2HostCtx *c, int lo, int hi, GlesDraw *g)
{
    memset(g, 0, sizeof(*g));
    GLint ab = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab);
    int bound = 0;
    for (int i = 0; i < 16; i++) {
        struct GlesArray *r = &c->arr[i];
        if (!r->enabled || !r->va) {
            continue;
        }
        int elem = r->size * gl1_type_size(r->type);
        int stride = r->stride ? r->stride : elem;
        size_t bytes = (size_t)(hi - lo) * stride + elem;
        uint8_t *h = gles_in(s, r->va + (uint32_t)lo * stride, bytes);
        if (!h) {
            gles_bad(s, "draw (a client array)");
            continue;
        }
        g->copy[i] = h;
        const uint8_t *base = h - (size_t)lo * stride;
        g->d.base[i] = base; g->d.size[i] = r->size; g->d.type[i] = r->type; g->d.stride[i] = stride;
        if (!bound) {
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            bound = 1;
        }
        if (c->api == 2) {
            glVertexAttribPointer(i, r->size, r->type, r->norm, r->stride, base);
        } else {
            gl1_set_pointer(i, r->size, r->type, r->stride, base);
        }
    }
    if (bound) {
        glBindBuffer(GL_ARRAY_BUFFER, ab);
    }
    return 0;
}

static void gles_draw_done(GlesDraw *g)
{
    for (int i = 0; i < 16; i++) {
        g_free(g->copy[i]);
    }
}

static int gles_has_client(Gl2HostCtx *c)
{
    for (int i = 0; i < 16; i++) {
        if (c->arr[i].enabled && c->arr[i].va) {
            return 1;
        }
    }
    return 0;
}

static void gles_draw(SyborgGlState *s, Gl2HostCtx *c, int elements, const uint32_t *a, size_t n)
{
    GlesDraw g;
    Gl1Skin k;
    int skin = 0;
    if (!elements) {                    /* mode, first, count */
        if (n < 3 || (int32_t)a[1] < 0 || (int32_t)a[2] <= 0) {
            return;
        }
        int lo = a[1], hi = a[1] + a[2] - 1;
        gles_draw_arrays(s, c, lo, hi, &g);
        if (c->api == 1 && c->g1->palEnabled) {
            skin = !gl1_skin(c, &g.d, lo, hi, &k);
        }
        glDrawArrays(a[0], a[1], a[2]);
    } else {                            /* mode, count, type, indices */
        if (n < 4 || (int32_t)a[1] <= 0) {
            return;
        }
        int isz = gl1_type_size(a[2]);
        GLint eb = 0;
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &eb);
        uint8_t *idx = NULL;
        size_t ib = (size_t)a[1] * isz;
        if (!eb) {
            idx = gles_in(s, a[3], ib);
            if (!idx) {
                gles_bad(s, "glDrawElements (indices)");
                return;
            }
        }
        int lo = 0, hi = 0;
        int need = gles_has_client(c) || (c->api == 1 && c->g1->palEnabled);
        if (need) {
            uint8_t *ix = idx ? idx : (c->api == 1 ? gl1_read_bound(GL_ELEMENT_ARRAY_BUFFER, a[3], ib)
                                                   : gl2_read_bound(GL_ELEMENT_ARRAY_BUFFER, a[3], ib));
            gl1_index_range(ix, a[2], a[1], &lo, &hi);
            if (ix != idx) {
                g_free(ix);
            }
        }
        gles_draw_arrays(s, c, lo, hi, &g);
        if (c->api == 1 && c->g1->palEnabled) {
            skin = !gl1_skin(c, &g.d, lo, hi, &k);
        }
        glDrawElements(a[0], a[1], a[2], idx ? (const void *)idx : (const void *)(uintptr_t)a[3]);
        g_free(idx);
    }
    if (skin) {
        gl1_unskin(c, &k);
    }
    gles_draw_done(&g);
    s->draws++;
}

/* ---- queries both APIs share */

/* glGet{Boolean,Float,Integer,Fixed}v of an ES 2 context: values as the guest knows them */
static void gles2_get(Gl2HostCtx *c, GLenum p, uint32_t *v, int *cnt, int kind)
{
    *cnt = gl1_count_get(p);
    if (p == GL_COMPRESSED_TEXTURE_FORMATS) {
        GLint k = 0;
        glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &k);
        *cnt = MIN(k, GLES_GETMAX);
    }
    if (p == GL_SHADER_BINARY_FORMATS) {
        *cnt = 0;
        return;
    }
    if (kind == 'f') {
        glGetFloatv(p, (GLfloat *)v);
    } else if (kind == 'b') {
        GLboolean b[GLES_GETMAX] = { 0 };
        glGetBooleanv(p, b);
        for (int i = 0; i < *cnt; i++) {
            v[i] = b[i];
        }
    } else {
        glGetIntegerv(p, (GLint *)v);
    }
    if (kind != 'i') {
        return;
    }
    switch (p) {
    case GL_TEXTURE_BINDING_2D: case GL_TEXTURE_BINDING_CUBE_MAP: v[0] = gl2_guest(c, GL2_N_TEX, v[0]); break;
    case GL_RENDERBUFFER_BINDING: v[0] = gl2_guest(c, GL2_N_RBO, v[0]); break;
    case GL_FRAMEBUFFER_BINDING: v[0] = c->fbo; break;
    case GL_ARRAY_BUFFER_BINDING: case GL_ELEMENT_ARRAY_BUFFER_BINDING: v[0] = gl2_guest(c, GL2_N_BUF, v[0]); break;
    case GL_CURRENT_PROGRAM: v[0] = gl2_guest(c, GL2_N_PROG, v[0]); break;
    case GL_MAX_VERTEX_ATTRIBS: v[0] = MIN(v[0], 16u); break;
    case GL_NUM_SHADER_BINARY_FORMATS: v[0] = 0; break;
    }
}

/* write a get's values into the caller's array in its type (booleans are bytes) */
static void gles_put_values(SyborgGlState *s, uint32_t va, const uint32_t *v, int cnt, int kind)
{
    cnt = MIN(MAX(cnt, 0), GLES_GETMAX);
    if (kind == 'b') {
        uint8_t b[GLES_GETMAX];
        for (int i = 0; i < cnt; i++) {
            b[i] = v[i] != 0;
        }
        gles_put(s, va, b, cnt);
    } else {
        gles_put(s, va, v, (size_t)cnt * 4);
    }
}

static void gles_read_pixels(SyborgGlState *s, const uint32_t *a, size_t n)
{
    if (n < 7) {
        return;
    }
    size_t bytes = gles_pix_bytes(a[2], a[3], 1, a[4], a[5], GL_PACK_ALIGNMENT);
    if (!bytes) {
        glReadPixels(a[0], a[1], a[2], a[3], a[4], a[5], NULL);     /* the host's own error */
        return;
    }
    GLint align = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    size_t row = ((size_t)a[2] * gles_bpp(a[4], a[5]) + align - 1) / align * align;
    uint8_t *px = g_malloc0(row * a[3]);
    GLint rf = 0, rt = 0;
    if (a[4] != GL_RGBA || a[5] != GL_UNSIGNED_BYTE) {
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &rf);
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &rt);
    }
    if ((a[4] == GL_RGBA && a[5] == GL_UNSIGNED_BYTE) || ((GLenum)rf == a[4] && (GLenum)rt == a[5])) {
        glReadPixels(a[0], a[1], a[2], a[3], a[4], a[5], px);
    } else {
        /* the host reads only RGBA (and its own format): read that and pack the guest's (H-gl2-readformat) */
        uint8_t *rgba = g_malloc((size_t)a[2] * 4 * a[3]);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(a[0], a[1], a[2], a[3], GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glPixelStorei(GL_PACK_ALIGNMENT, align);
        gl2_pack_rgba(rgba, px, a[2], a[3], row, a[4], a[5]);
        g_free(rgba);
        if (s->gl2_readconv++ < 4) {
            warn_report("syborg-gl: glReadPixels format 0x%x type 0x%x read as RGBA and converted", a[4], a[5]);
        }
    }
    gles_put(s, a[6], px, bytes);
    g_free(px);
}

/* OES_mapbuffer: the caller's memory stands for the mapping (the guest allocates it, khr_gl_map); its
   contents go to the buffer at unmap. {target, access} -> size; {target, access, va} -> filled, 1;
   unmap {target} -> [ok, va to free] */
static int gles_map(SyborgGlState *s, Gl2HostCtx *c, int unmap, const uint32_t *a, size_t n)
{
    if (n < 1) {
        return 0;
    }
    int k = a[0] == GL_ARRAY_BUFFER ? 0 : a[0] == GL_ELEMENT_ARRAY_BUFFER ? 1 : -1;
    GLint bound = 0, size = 0;
    if (k >= 0) {
        glGetIntegerv(k ? GL_ELEMENT_ARRAY_BUFFER_BINDING : GL_ARRAY_BUFFER_BINDING, &bound);
        if (bound) {
            glGetBufferParameteriv(a[0], GL_BUFFER_SIZE, &size);
        }
    }
    if (unmap) {
        uint32_t r[2] = { 0, 0 };
        if (k < 0 || !c->map[k].va || c->map[k].buf != (GLuint)bound) {
            c->err = c->err ? c->err : (k < 0 ? GL_INVALID_ENUM : GL_INVALID_OPERATION);
        } else {
            void *h = gles_in(s, c->map[k].va, c->map[k].size);
            if (h) {
                glBufferSubData(a[0], 0, c->map[k].size, h);
                g_free(h);
            }
            r[0] = 1;
            r[1] = c->map[k].va;
            memset(&c->map[k], 0, sizeof(c->map[k]));
        }
        return gl2_out(s, r, 8) ? 1 : 0;
    }
    if (k < 0 || n < 2 || a[1] != 0x88B9 /* WRITE_ONLY_OES */ || !bound || size <= 0 || c->map[k].va) {
        c->err = c->err ? c->err : (k < 0 || n < 2 || a[1] != 0x88B9 ? GL_INVALID_ENUM : GL_INVALID_OPERATION);
        return gles_ret(s, 0);
    }
    if (n < 3) {
        return gles_ret(s, size);
    }
    uint8_t *d = c->api == 1 ? gl1_read_bound(a[0], 0, size) : gl2_read_bound(a[0], 0, size);
    gles_put(s, a[2], d, size);
    g_free(d);
    c->map[k].va = a[2];
    c->map[k].size = size;
    c->map[k].buf = bound;
    return gles_ret(s, 1);
}

static void gles_buffer_param(SyborgGlState *s, Gl2HostCtx *c, const uint32_t *a, size_t n)
{
    if (n < 3) {
        return;
    }
    int k = a[0] == GL_ARRAY_BUFFER ? 0 : a[0] == GL_ELEMENT_ARRAY_BUFFER ? 1 : -1;
    GLint v = 0, bound = 0;
    if (k >= 0) {
        glGetIntegerv(k ? GL_ELEMENT_ARRAY_BUFFER_BINDING : GL_ARRAY_BUFFER_BINDING, &bound);
    }
    if (k >= 0 && a[1] == 0x88BC) {                 /* BUFFER_MAPPED_OES */
        v = c->map[k].va && c->map[k].buf == (GLuint)bound;
    } else if (k >= 0 && a[1] == 0x88BB) {          /* BUFFER_ACCESS_OES */
        v = 0x88B9;
    } else {
        glGetBufferParameteriv(a[0], a[1], &v);
    }
    gles_put(s, a[2], &v, 4);
}

static void gles_buffer_pointer(SyborgGlState *s, Gl2HostCtx *c, const uint32_t *a, size_t n)
{
    if (n < 3) {
        return;
    }
    int k = a[0] == GL_ARRAY_BUFFER ? 0 : a[0] == GL_ELEMENT_ARRAY_BUFFER ? 1 : -1;
    uint32_t v = k >= 0 && a[1] == 0x88BD ? c->map[k].va : 0;
    if (k < 0 || a[1] != 0x88BD) {
        c->err = c->err ? c->err : GL_INVALID_ENUM;
    }
    gles_put(s, a[2], &v, 4);
}

static int gles_get_error(SyborgGlState *s, Gl2HostCtx *c)
{
    uint32_t e = c->err ? c->err : glGetError();
    c->err = 0;
    return gles_ret(s, e);
}

/* ---- OpenGL ES 2.0 */

static void gles2_shader_source(SyborgGlState *s, Gl2HostCtx *c, const uint32_t *a, size_t n)
{
    if (n < 4 || (int32_t)a[1] < 0 || a[1] > 4096) {
        c->err = c->err ? c->err : GL_INVALID_VALUE;
        return;
    }
    uint32_t cnt = a[1];
    uint32_t *strs = gles_in(s, a[2], cnt * 4);
    int32_t *lens = a[3] ? gles_in(s, a[3], cnt * 4) : NULL;
    if (!strs || (a[3] && !lens)) {
        g_free(strs);
        g_free(lens);
        gles_bad(s, "glShaderSource");
        return;
    }
    GString *src = g_string_new(NULL);
    for (uint32_t i = 0; i < cnt; i++) {
        if (lens && lens[i] >= 0) {
            void *h = gles_in(s, strs[i], lens[i]);
            if (h) {
                g_string_append_len(src, h, lens[i]);
            }
            g_free(h);
        } else {
            char *z = gles_string(s, strs[i]);
            if (z) {
                g_string_append(src, z);
            }
            g_free(z);
        }
    }
    g_free(strs);
    g_free(lens);
    GLuint shader = gl2_name(c, GL2_N_SHADER, a[0], 0);
    const char *policy = getenv("SYBORG_GL2_HIGHP");
    if (!policy || strcmp(policy, "0")) {
        /* H-gl2-highp: both stages or neither (a uniform shared by the stages must keep one precision) */
        static int highp = -1;
        GLint type = 0;
        if (highp < 0) {
            GLint range[2] = { 0, 0 }, vbits = 0, fbits = 0;
            glGetShaderPrecisionFormat(GL_VERTEX_SHADER, GL_HIGH_FLOAT, range, &vbits);
            glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &fbits);
            highp = vbits > 0 && fbits > 0;
        }
        glGetShaderiv(shader, GL_SHADER_TYPE, &type);
        if (highp && (type == GL_VERTEX_SHADER || type == GL_FRAGMENT_SHADER)) {
            int changed = gl2_promote_float_defaults(src->str, src->len);
            static unsigned logs;
            if (changed && logs++ < 16) {
                info_report("syborg-gl: GL2 shader %u (%s): promoted %d mediump float defaults to highp",
                            a[0], type == GL_VERTEX_SHADER ? "vertex" : "fragment", changed);
            }
        }
    }
    const GLchar *p = src->str;
    GLint len = src->len;
    glShaderSource(shader, 1, &p, &len);
    g_string_free(src, TRUE);
}

static int gl2_call_1(SyborgGlState *s, Gl2HostCtx *c, uint32_t fn, const uint32_t *a, size_t n)
{
    switch (fn) {
#include "syborg_gl2_cases.h"
    case GL2_FN_glGenBuffers: gles_gen(s, c, GL2_N_BUF, a, n); break;
    case GL2_FN_glGenTextures: gles_gen(s, c, GL2_N_TEX, a, n); break;
    case GL2_FN_glGenFramebuffers: gles_gen(s, c, GL2_N_FBO, a, n); break;
    case GL2_FN_glGenRenderbuffers: gles_gen(s, c, GL2_N_RBO, a, n); break;
    case GL2_FN_glDeleteBuffers: gles_delete(s, c, GL2_N_BUF, a, n); break;
    case GL2_FN_glDeleteTextures: gles_delete(s, c, GL2_N_TEX, a, n); break;
    case GL2_FN_glDeleteFramebuffers: gles_delete(s, c, GL2_N_FBO, a, n); break;
    case GL2_FN_glDeleteRenderbuffers: gles_delete(s, c, GL2_N_RBO, a, n); break;
    case GL2_FN_glCreateProgram: case GL2_FN_glCreateShader: {
        int prog = fn == GL2_FN_glCreateProgram;
        GLuint h = prog ? glCreateProgram() : (n >= 1 ? glCreateShader(a[0]) : 0);
        uint32_t g = 0;
        if (h) {
            g = gles_new_name(c, prog ? GL2_N_PROG : GL2_N_SHADER);
            g_hash_table_insert(gl2_map(c, prog ? GL2_N_PROG : GL2_N_SHADER), GUINT_TO_POINTER(g), GUINT_TO_POINTER(h));
        }
        return gles_ret(s, g);
    }
    case GL2_FN_glDeleteProgram: case GL2_FN_glDeleteShader:
        if (n >= 1 && a[0]) {
            int prog = fn == GL2_FN_glDeleteProgram;
            GLuint h = gl2_name(c, prog ? GL2_N_PROG : GL2_N_SHADER, a[0], 0);
            if (h) {
                if (prog) {
                    glDeleteProgram(h);
                } else {
                    glDeleteShader(h);
                }
                gl2_unmap(c, prog ? GL2_N_PROG : GL2_N_SHADER, a[0]);
            } else {
                c->err = c->err ? c->err : GL_INVALID_VALUE;
            }
        }
        break;
    case GL2_FN_glBindFramebuffer: if (n >= 2) { c->fbo = a[1]; gl2_rebind_fbo(c); } break;
    case GL2_FN_glFramebufferRenderbuffer: case GL2_FN_glFramebufferTexture2D: case GL2_FN_glFramebufferTexture3DOES:
        if (!c->fbo) {                  /* framebuffer 0's attachments are the surface's: refused (H-gl2-fb0) */
            if (s->gl2_fb0_logs++ < 4) {
                warn_report("syborg-gl: GL2 attachment change on framebuffer 0 (the surface) ignored");
            }
            c->err = c->err ? c->err : GL_INVALID_OPERATION;
        } else if (fn == GL2_FN_glFramebufferRenderbuffer && n >= 4) {
            glFramebufferRenderbuffer(a[0], a[1], a[2], gl2_name(c, GL2_N_RBO, a[3], 0));
        } else if (fn == GL2_FN_glFramebufferTexture2D && n >= 5) {
            glFramebufferTexture2D(a[0], a[1], a[2], gl2_name(c, GL2_N_TEX, a[3], 0), (GLint)a[4]);
        } else if (n >= 6) {
            glFramebufferTextureLayer(a[0], a[1], gl2_name(c, GL2_N_TEX, a[3], 0), (GLint)a[4], (GLint)a[5]);
        }
        break;
    case GL2_FN_glGetFramebufferAttachmentParameteriv:
        if (n >= 4) {
            GLint v = 0, type = 0;
            glGetFramebufferAttachmentParameteriv(a[0], a[1], a[2], &v);
            if (a[2] == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) {
                glGetFramebufferAttachmentParameteriv(a[0], a[1], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
                v = gl2_guest(c, type == GL_TEXTURE ? GL2_N_TEX : GL2_N_RBO, v);
            }
            gles_put(s, a[3], &v, 4);
        }
        break;
    case GL2_FN_glGetString: if (n >= 1) return gles_get_string(s, c, a[0]); break;
    case GL2_FN_glGetError: return gles_get_error(s, c);
    case GL2_FN_glGetIntegerv: case GL2_FN_glGetFloatv: case GL2_FN_glGetBooleanv:
        if (n >= 2) {
            uint32_t v[GLES_GETMAX] = { 0 };
            int cnt = 1, kind = fn == GL2_FN_glGetFloatv ? 'f' : fn == GL2_FN_glGetBooleanv ? 'b' : 'i';
            gles2_get(c, a[0], v, &cnt, kind);
            gles_put_values(s, a[1], v, cnt, kind);
        }
        break;
    case GL2_FN_glGetAttachedShaders:
        if (n >= 4 && (int32_t)a[1] >= 0) {
            GLuint sh[64];
            GLsizei got = 0;
            glGetAttachedShaders(gl2_name(c, GL2_N_PROG, a[0], 0), MIN(a[1], 64u), &got, sh);
            for (GLsizei i = 0; i < got; i++) {
                sh[i] = gl2_guest(c, GL2_N_SHADER, sh[i]);
            }
            gles_put(s, a[2], &got, 4);
            gles_put(s, a[3], sh, (size_t)got * 4);
        }
        break;
    case GL2_FN_glEnableVertexAttribArray: case GL2_FN_glDisableVertexAttribArray:
        if (n >= 1 && a[0] < 16) {
            int on = fn == GL2_FN_glEnableVertexAttribArray;
            c->arr[a[0]].enabled = on;
            if (on) {
                glEnableVertexAttribArray(a[0]);
            } else {
                glDisableVertexAttribArray(a[0]);
            }
        } else {
            c->err = c->err ? c->err : GL_INVALID_VALUE;
        }
        break;
    case GL2_FN_glVertexAttribPointer:
        if (n >= 6) {
            if (a[0] >= 16 || (int32_t)a[1] < 1 || a[1] > 4 || (int32_t)a[4] < 0) {
                c->err = c->err ? c->err : GL_INVALID_VALUE;
            } else {
                gles_pointer(c, a[0], a[1], a[2], a[3], a[4], a[5]);
            }
        }
        break;
    case GL2_FN_glGetVertexAttribiv: case GL2_FN_glGetVertexAttribfv:
        if (n >= 3) {
            uint32_t v[4] = { 0 };
            int cnt = 1, fl = fn == GL2_FN_glGetVertexAttribfv;
            struct GlesArray *r = a[0] < 16 ? &c->arr[a[0]] : NULL;
            GLint iv = 0;
            int local = 1;
            switch (a[1]) {
            case GL_VERTEX_ATTRIB_ARRAY_ENABLED: iv = r ? r->enabled : 0; break;
            case GL_VERTEX_ATTRIB_ARRAY_SIZE: iv = r && r->size ? r->size : 4; break;
            case GL_VERTEX_ATTRIB_ARRAY_STRIDE: iv = r ? r->stride : 0; break;
            case GL_VERTEX_ATTRIB_ARRAY_TYPE: iv = r && r->type ? r->type : GL_FLOAT; break;
            case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED: iv = r ? r->norm : 0; break;
            case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: iv = r ? gl2_guest(c, GL2_N_BUF, r->buf) : 0; break;
            default: local = 0; break;
            }
            if (local) {
                if (fl) {
                    float f = (float)iv;
                    memcpy(v, &f, 4);
                } else {
                    v[0] = iv;
                }
            } else if (fl) {
                glGetVertexAttribfv(a[0], a[1], (GLfloat *)v);
                cnt = a[1] == GL_CURRENT_VERTEX_ATTRIB ? 4 : 1;
            } else {
                glGetVertexAttribiv(a[0], a[1], (GLint *)v);
                cnt = a[1] == GL_CURRENT_VERTEX_ATTRIB ? 4 : 1;
            }
            gles_put(s, a[2], v, (size_t)cnt * 4);
        }
        break;
    case GL2_FN_glGetVertexAttribPointerv:
        if (n >= 3) {
            uint32_t v = a[0] < 16 ? (c->arr[a[0]].va ? c->arr[a[0]].va : (uint32_t)c->arr[a[0]].off) : 0;
            gles_put(s, a[2], &v, 4);
        }
        break;
    case GL2_FN_glDrawArrays: gles_draw(s, c, 0, a, n); break;
    case GL2_FN_glDrawElements: gles_draw(s, c, 1, a, n); break;
    case GL2_FN_glReadPixels: gles_read_pixels(s, a, n); break;
    case GL2_FN_glShaderSource: gles2_shader_source(s, c, a, n); break;
    case GL2_FN_glShaderBinary:         /* no binary formats (GL_NUM_SHADER_BINARY_FORMATS 0) */
        c->err = c->err ? c->err : GL_INVALID_ENUM;
        break;
    case GL2_FN_glGetUniformfv: case GL2_FN_glGetUniformiv:
        if (n >= 3) {
            GLuint prog = gl2_name(c, GL2_N_PROG, a[0], 0);
            uint32_t v[16] = { 0 };
            if (fn == GL2_FN_glGetUniformfv) {
                glGetUniformfv(prog, a[1], (GLfloat *)v);
            } else {
                glGetUniformiv(prog, a[1], (GLint *)v);
            }
            gles_put(s, a[2], v, (size_t)gl2_uniform_size(prog, a[1]) * 4);
        }
        break;
    case GL2_FN_glGetBufferParameteriv: gles_buffer_param(s, c, a, n); break;
    case GL2_FN_glMapBufferOES: return gles_map(s, c, 0, a, n);
    case GL2_FN_glUnmapBufferOES: return gles_map(s, c, 1, a, n);
    case GL2_FN_glGetBufferPointervOES: gles_buffer_pointer(s, c, a, n); break;
    case GL2_FN_glEGLImageTargetTexture2DOES: case GL2_FN_glEGLImageTargetRenderbufferStorageOES:
        c->err = c->err ? c->err : GL_INVALID_OPERATION;    /* no EGL images yet (H-gles-no-eglimage) */
        break;
    default:
        if (s->glerrors++ < 8) {
            warn_report("syborg-gl: OpenGL ES 2 function %u not replayed", fn);
        }
        break;
    }
    return 0;
}

/* ---- OpenGL ES 1.1 */

static int gl1_call_1(SyborgGlState *s, Gl2HostCtx *c, uint32_t fn, const uint32_t *a, size_t n)
{
    Gl1State *g = c->g1;
    float *P = g->pal[g->palCur];       /* the current palette matrix (palette mode) */
    switch (fn) {
#include "syborg_gl1_cases.h"
    case GL1_FN_glGenBuffers: gles_gen(s, c, GL2_N_BUF, a, n); break;
    case GL1_FN_glGenTextures: gles_gen(s, c, GL2_N_TEX, a, n); break;
    case GL1_FN_glGenFramebuffersOES: gles_gen(s, c, GL2_N_FBO, a, n); break;
    case GL1_FN_glGenRenderbuffersOES: gles_gen(s, c, GL2_N_RBO, a, n); break;
    case GL1_FN_glDeleteBuffers: gles_delete(s, c, GL2_N_BUF, a, n); break;
    case GL1_FN_glDeleteTextures: gles_delete(s, c, GL2_N_TEX, a, n); break;
    case GL1_FN_glDeleteFramebuffersOES: gles_delete(s, c, GL2_N_FBO, a, n); break;
    case GL1_FN_glDeleteRenderbuffersOES: gles_delete(s, c, GL2_N_RBO, a, n); break;
    case GL1_FN_glBindFramebufferOES: if (n >= 2) { c->fbo = a[1]; gl2_rebind_fbo(c); } break;
    case GL1_FN_glFramebufferRenderbufferOES: case GL1_FN_glFramebufferTexture2DOES:
        if (!c->fbo) {
            if (s->gl2_fb0_logs++ < 4) {
                warn_report("syborg-gl: GL1 attachment change on framebuffer 0 (the surface) ignored");
            }
            c->err = c->err ? c->err : GL_INVALID_OPERATION;
        } else if (fn == GL1_FN_glFramebufferRenderbufferOES && n >= 4) {
            glFramebufferRenderbufferOES(a[0], a[1], a[2], gl2_name(c, GL2_N_RBO, a[3], 0));
        } else if (fn == GL1_FN_glFramebufferTexture2DOES && n >= 5) {
            glFramebufferTexture2DOES(a[0], a[1], a[2], gl2_name(c, GL2_N_TEX, a[3], 0), (GLint)a[4]);
        }
        break;
    case GL1_FN_glGetFramebufferAttachmentParameterivOES:
        if (n >= 4) {
            GLint v = 0, type = 0;
            glGetFramebufferAttachmentParameterivOES(a[0], a[1], a[2], &v);
            if (a[2] == 0x8CD1) {       /* FRAMEBUFFER_ATTACHMENT_OBJECT_NAME_OES */
                glGetFramebufferAttachmentParameterivOES(a[0], a[1], 0x8CD0, &type);
                v = gl2_guest(c, type == GL_TEXTURE ? GL2_N_TEX : GL2_N_RBO, v);
            }
            gles_put(s, a[3], &v, 4);
        }
        break;
    case GL1_FN_glGetString: if (n >= 1) return gles_get_string(s, c, a[0]); break;
    case GL1_FN_glGetError: return gles_get_error(s, c);
    case GL1_FN_glGetIntegerv: case GL1_FN_glGetFloatv: case GL1_FN_glGetFixedv: case GL1_FN_glGetBooleanv:
        if (n >= 2) {
            uint32_t v[GLES_GETMAX] = { 0 };
            int cnt = 1;
            GLenum p = a[0];
            const struct GlesArray *t = NULL;
            GLint cat = GL_TEXTURE0, local = 1, iv = 0;
            glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &cat);
            t = &c->arr[GL1_SLOT_TEXCOORD0 + ((cat - GL_TEXTURE0) & 7)];
            switch (p) {                /* the arrays as the guest set them (client arrays are not in GL) */
            case 0x807A: iv = c->arr[GL1_SLOT_VERTEX].size; break;
            case 0x807B: iv = c->arr[GL1_SLOT_VERTEX].type; break;
            case 0x807C: iv = c->arr[GL1_SLOT_VERTEX].stride; break;
            case 0x807E: iv = c->arr[GL1_SLOT_NORMAL].type; break;
            case 0x807F: iv = c->arr[GL1_SLOT_NORMAL].stride; break;
            case 0x8081: iv = c->arr[GL1_SLOT_COLOR].size; break;
            case 0x8082: iv = c->arr[GL1_SLOT_COLOR].type; break;
            case 0x8083: iv = c->arr[GL1_SLOT_COLOR].stride; break;
            case 0x8088: iv = t->size; break;
            case 0x8089: iv = t->type; break;
            case 0x808A: iv = t->stride; break;
            case 0x898A: iv = c->arr[GL1_SLOT_POINT_SIZE].type; break;
            case 0x898B: iv = c->arr[GL1_SLOT_POINT_SIZE].stride; break;
            case 0x8846: iv = c->arr[GL1_SLOT_MATRIX_INDEX].size; break;
            case 0x8847: iv = c->arr[GL1_SLOT_MATRIX_INDEX].type; break;
            case 0x8848: iv = c->arr[GL1_SLOT_MATRIX_INDEX].stride; break;
            case 0x86AB: iv = c->arr[GL1_SLOT_WEIGHT].size; break;
            case 0x86A9: iv = c->arr[GL1_SLOT_WEIGHT].type; break;
            case 0x86AA: iv = c->arr[GL1_SLOT_WEIGHT].stride; break;
            case 0x8896: iv = gl2_guest(c, GL2_N_BUF, c->arr[GL1_SLOT_VERTEX].buf); break;
            case 0x8897: iv = gl2_guest(c, GL2_N_BUF, c->arr[GL1_SLOT_NORMAL].buf); break;
            case 0x8898: iv = gl2_guest(c, GL2_N_BUF, c->arr[GL1_SLOT_COLOR].buf); break;
            case 0x889A: iv = gl2_guest(c, GL2_N_BUF, t->buf); break;
            case 0x8B9F: iv = gl2_guest(c, GL2_N_BUF, c->arr[GL1_SLOT_POINT_SIZE].buf); break;
            case 0x8B9E: iv = gl2_guest(c, GL2_N_BUF, c->arr[GL1_SLOT_MATRIX_INDEX].buf); break;
            case 0x889E: iv = gl2_guest(c, GL2_N_BUF, c->arr[GL1_SLOT_WEIGHT].buf); break;
            case GL_ARRAY_BUFFER_BINDING: case GL_ELEMENT_ARRAY_BUFFER_BINDING:
                glGetIntegerv(p, &iv);
                iv = gl2_guest(c, GL2_N_BUF, iv);
                break;
            default: local = 0; break;
            }
            int kind = fn == GL1_FN_glGetBooleanv ? 'b' : 'i';
            if (local) {
                if (fn == GL1_FN_glGetFloatv) {
                    float f = (float)iv;
                    memcpy(v, &f, 4);
                } else if (fn == GL1_FN_glGetFixedv) {
                    v[0] = (uint32_t)(iv * 65536);
                } else {
                    v[0] = (uint32_t)iv;
                }
            } else {
                gl1_get(s, c, fn, p, v, &cnt);
            }
            gles_put_values(s, a[1], v, cnt, kind);
        }
        break;
    case GL1_FN_glGetPointerv:
        if (n >= 2) {
            int slot;
            GLint cat = GL_TEXTURE0;
            glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &cat);
            switch (a[0]) {
            case 0x808E: slot = GL1_SLOT_VERTEX; break;
            case 0x808F: slot = GL1_SLOT_NORMAL; break;
            case 0x8090: slot = GL1_SLOT_COLOR; break;
            case 0x8092: slot = GL1_SLOT_TEXCOORD0 + ((cat - GL_TEXTURE0) & 7); break;
            case 0x898C: slot = GL1_SLOT_POINT_SIZE; break;
            case 0x8849: slot = GL1_SLOT_MATRIX_INDEX; break;
            case 0x86AC: slot = GL1_SLOT_WEIGHT; break;
            default: slot = -1; c->err = c->err ? c->err : GL_INVALID_ENUM; break;
            }
            uint32_t v = slot < 0 ? 0 : c->arr[slot].va ? c->arr[slot].va : (uint32_t)c->arr[slot].off;
            gles_put(s, a[1], &v, 4);
        }
        break;
    /* ---- client state */
    case GL1_FN_glEnableClientState: case GL1_FN_glDisableClientState:
        if (n >= 1) {
            int on = fn == GL1_FN_glEnableClientState, slot = -1;
            GLint cat = GL_TEXTURE0;
            switch (a[0]) {
            case GL_VERTEX_ARRAY: slot = GL1_SLOT_VERTEX; break;
            case GL_NORMAL_ARRAY: slot = GL1_SLOT_NORMAL; break;
            case GL_COLOR_ARRAY: slot = GL1_SLOT_COLOR; break;
            case 0x8B9C: slot = GL1_SLOT_POINT_SIZE; break;
            case GL1_MATRIX_INDEX_ARRAY: slot = GL1_SLOT_MATRIX_INDEX; g->idxOn = on; break;
            case GL1_WEIGHT_ARRAY: slot = GL1_SLOT_WEIGHT; g->wOn = on; break;
            case GL_TEXTURE_COORD_ARRAY:
                glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &cat);
                slot = GL1_SLOT_TEXCOORD0 + ((cat - GL_TEXTURE0) & 7);
                break;
            }
            if (slot < 0) {
                c->err = c->err ? c->err : GL_INVALID_ENUM;
                break;
            }
            c->arr[slot].enabled = on;
            if (slot != GL1_SLOT_MATRIX_INDEX && slot != GL1_SLOT_WEIGHT) {
                if (on) {
                    glEnableClientState(a[0]);
                } else {
                    glDisableClientState(a[0]);
                }
            }
        }
        break;
    case GL1_FN_glClientActiveTexture: if (n >= 1) glClientActiveTexture(a[0]); break;
    case GL1_FN_glIsEnabled:
        if (n >= 1) {
            uint32_t r = a[0] == GL1_MATRIX_PALETTE ? g->palEnabled : a[0] == GL1_MATRIX_INDEX_ARRAY ? g->idxOn
                       : a[0] == GL1_WEIGHT_ARRAY ? g->wOn : glIsEnabled(a[0]);
            return gles_ret(s, r);
        }
        break;
    case GL1_FN_glEnable: case GL1_FN_glDisable:
        if (n >= 1) {
            int on = fn == GL1_FN_glEnable;
            if (a[0] == GL1_MATRIX_PALETTE) {
                g->palEnabled = on;
            } else if (on) {
                glEnable(a[0]);
            } else {
                glDisable(a[0]);
            }
        }
        break;
    /* ---- arrays: {size, type, stride, pointer} (normals and point sizes: {type, stride, pointer}) */
    case GL1_FN_glVertexPointer: if (n >= 4) gles_pointer(c, GL1_SLOT_VERTEX, a[0], a[1], 0, a[2], a[3]); break;
    case GL1_FN_glColorPointer: if (n >= 4) gles_pointer(c, GL1_SLOT_COLOR, a[0], a[1], 0, a[2], a[3]); break;
    case GL1_FN_glNormalPointer: if (n >= 3) gles_pointer(c, GL1_SLOT_NORMAL, 3, a[0], 0, a[1], a[2]); break;
    case GL1_FN_glPointSizePointerOES: if (n >= 3) gles_pointer(c, GL1_SLOT_POINT_SIZE, 1, a[0], 0, a[1], a[2]); break;
    case GL1_FN_glMatrixIndexPointerOES: if (n >= 4) gles_pointer(c, GL1_SLOT_MATRIX_INDEX, a[0], a[1], 0, a[2], a[3]); break;
    case GL1_FN_glWeightPointerOES: if (n >= 4) gles_pointer(c, GL1_SLOT_WEIGHT, a[0], a[1], 0, a[2], a[3]); break;
    case GL1_FN_glTexCoordPointer:
        if (n >= 4) {
            GLint cat = GL_TEXTURE0;
            glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &cat);
            gles_pointer(c, GL1_SLOT_TEXCOORD0 + ((cat - GL_TEXTURE0) & 7), a[0], a[1], 0, a[2], a[3]);
        }
        break;
    case GL1_FN_glDrawArrays: gles_draw(s, c, 0, a, n); break;
    case GL1_FN_glDrawElements: gles_draw(s, c, 1, a, n); break;
    case GL1_FN_glReadPixels: gles_read_pixels(s, a, n); break;
    /* ---- matrices (the palette's on the host CPU) */
    case GL1_FN_glMatrixMode:
        if (n >= 1) {
            if (a[0] == GL1_MATRIX_PALETTE) {
                g->palMode = 1;
            } else {
                g->palMode = 0;
                glMatrixMode(a[0]);
            }
        }
        break;
    case GL1_FN_glLoadIdentity: if (g->palMode) m4_identity(P); else glLoadIdentity(); break;
    case GL1_FN_glLoadMatrixf: case GL1_FN_glMultMatrixf: case GL1_FN_glLoadMatrixx: case GL1_FN_glMultMatrixx:
        if (n >= 1) {
            uint32_t *w = gles_in(s, a[0], 64);
            if (!w) {
                gles_bad(s, "glLoadMatrix/glMultMatrix");
                break;
            }
            float m[16];
            int fx = fn == GL1_FN_glLoadMatrixx || fn == GL1_FN_glMultMatrixx;
            for (int i = 0; i < 16; i++) {
                m[i] = fx ? fx2f(w[i]) : u2f(w[i]);
            }
            g_free(w);
            int load = fn == GL1_FN_glLoadMatrixf || fn == GL1_FN_glLoadMatrixx;
            if (g->palMode) {
                if (load) {
                    memcpy(P, m, sizeof(m));
                } else {
                    m4_mul(P, m);
                }
            } else if (load) {
                glLoadMatrixf(m);
            } else {
                glMultMatrixf(m);
            }
        }
        break;
    case GL1_FN_glTranslatef: case GL1_FN_glTranslatex: case GL1_FN_glScalef: case GL1_FN_glScalex:
        if (n >= 3) {
            int fx = fn == GL1_FN_glTranslatex || fn == GL1_FN_glScalex;
            float x = fx ? fx2f(a[0]) : u2f(a[0]), y = fx ? fx2f(a[1]) : u2f(a[1]), z = fx ? fx2f(a[2]) : u2f(a[2]);
            int tr = fn == GL1_FN_glTranslatef || fn == GL1_FN_glTranslatex;
            if (g->palMode) {
                if (tr) {
                    m4_translate(P, x, y, z);
                } else {
                    m4_scale(P, x, y, z);
                }
            } else if (tr) {
                glTranslatef(x, y, z);
            } else {
                glScalef(x, y, z);
            }
        }
        break;
    case GL1_FN_glRotatef: case GL1_FN_glRotatex:
        if (n >= 4) {
            int fx = fn == GL1_FN_glRotatex;
            float v[4];
            for (int i = 0; i < 4; i++) {
                v[i] = fx ? fx2f(a[i]) : u2f(a[i]);
            }
            if (g->palMode) {
                m4_rotate(P, v[0], v[1], v[2], v[3]);
            } else {
                glRotatef(v[0], v[1], v[2], v[3]);
            }
        }
        break;
    case GL1_FN_glFrustumf: case GL1_FN_glFrustumx: case GL1_FN_glOrthof: case GL1_FN_glOrthox:
        if (n >= 6) {
            int fx = fn == GL1_FN_glFrustumx || fn == GL1_FN_glOrthox;
            float v[6];
            for (int i = 0; i < 6; i++) {
                v[i] = fx ? fx2f(a[i]) : u2f(a[i]);
            }
            int fr = fn == GL1_FN_glFrustumf || fn == GL1_FN_glFrustumx;
            if (g->palMode) {
                if (fr) {
                    m4_frustum(P, v[0], v[1], v[2], v[3], v[4], v[5]);
                } else {
                    m4_ortho(P, v[0], v[1], v[2], v[3], v[4], v[5]);
                }
            } else if (fr) {
                glFrustumf(v[0], v[1], v[2], v[3], v[4], v[5]);
            } else {
                glOrthof(v[0], v[1], v[2], v[3], v[4], v[5]);
            }
        }
        break;
    case GL1_FN_glPushMatrix: if (!g->palMode) glPushMatrix(); break;     /* no stack for the palette */
    case GL1_FN_glPopMatrix: if (!g->palMode) glPopMatrix(); break;
    case GL1_FN_glCurrentPaletteMatrixOES:
        if (n >= 1 && a[0] < GL1_MAX_PALETTE_MATRICES) {
            g->palCur = a[0];
        } else {
            c->err = c->err ? c->err : GL_INVALID_VALUE;
        }
        break;
    case GL1_FN_glLoadPaletteFromModelViewMatrixOES: glGetFloatv(GL_MODELVIEW_MATRIX, P); break;
    case GL1_FN_glQueryMatrixxOES:
        if (n >= 2) {
            int32_t m[16] = { 0 }, e[16] = { 0 };
            uint32_t st = 0;
            if (g->palMode) {
                for (int i = 0; i < 16; i++) {
                    m[i] = (int32_t)(P[i] * 65536.0f);
                }
            } else {
                st = glQueryMatrixxOES((GLfixed *)m, (GLint *)e);
            }
            gles_put(s, a[0], m, 64);
            gles_put(s, a[1], e, 64);
            return gles_ret(s, st);
        }
        break;
    case GL1_FN_glGetBufferParameteriv: gles_buffer_param(s, c, a, n); break;
    case GL1_FN_glMapBufferOES: return gles_map(s, c, 0, a, n);
    case GL1_FN_glUnmapBufferOES: return gles_map(s, c, 1, a, n);
    case GL1_FN_glGetBufferPointervOES: gles_buffer_pointer(s, c, a, n); break;
    case GL1_FN_glEGLImageTargetTexture2DOES: case GL1_FN_glEGLImageTargetRenderbufferStorageOES:
        c->err = c->err ? c->err : GL_INVALID_OPERATION;    /* no EGL images yet (H-gles-no-eglimage) */
        break;
    default:
        if (s->glerrors++ < 8) {
            warn_report("syborg-gl: OpenGL ES 1.1 function %u not replayed", fn);
        }
        break;
    }
    return 0;
}

/* SYGL_GL1 / SYGL_GL2 {ctx, fn, words}: nonzero = malformed (the batch stops) */
static int gles_call(SyborgGlState *s, int api, const uint32_t *w, size_t words)
{
    static int check = -1, logs;
    if (words < 2) {
        return 1;
    }
    /* w[0]: the calling guest thread; its current OpenGL ES context is the host EGL's to say */
    Gl2HostCtx *c = gl2_find(s, syvg_gl_current(s->owner, w[0]));
    if (!c || c->api != api) {
        /* no context of that API current: the call does nothing, but a caller waiting for a result gets
           one (glGetString: the device's strings, as the old guest library answered without a context) */
        if (w[1] == (api == 1 ? GL1_FN_glGetString : GL2_FN_glGetString) && words >= 3) {
            Gl2HostCtx tmp = { .api = api };
            return gles_get_string(s, &tmp, w[2]);
        }
        uint32_t z[2] = { 0, 0 };
        gl2_out(s, z, MIN(s->outlen, 8u));
        return 0;
    }
    gl2_use(s, c);
    int r = api == 1 ? gl1_call_1(s, c, w[1], w + 2, words - 2) : gl2_call_1(s, c, w[1], w + 2, words - 2);
    if (check < 0) {
        check = getenv("SYBORG_GL2_CHECK") && *getenv("SYBORG_GL2_CHECK") == '1';
    }
    if (check) {
        static const char *const n1[] = { GL1_FN_NAMES }, *const n2[] = { GL2_FN_NAMES };
        GLenum e = glGetError();
        if (e != GL_NO_ERROR && logs++ < 200) {
            warn_report("syborg-gl: GL%d %s (ctx %08x): GL error 0x%x", api,
                        api == 1 ? (w[1] < GL1_FN_COUNT ? n1[w[1]] : "?") : (w[1] < GL2_FN_COUNT ? n2[w[1]] : "?"),
                        c->id, e);
        }
    }
    return r;
}

static void gl2_frame_end(void)
{
}

/* SYGL_GL_TEXIMAGE (eglBindTexImage): the surface's pixels into the texture bound to the context's active unit */
static void gl2_tex_from_surf(SyborgGlState *s, const uint32_t *a, size_t n)
{
    Gl2HostCtx *c = n >= 5 ? gl2_find(s, a[0]) : NULL;
    if (!c) {
        return;
    }
    gl2_use(s, c);
    GLuint f = gl2_surf_fbo(s, c, a[1]);
    if (!f) {
        return;
    }
    gl2_bind_fbo(c, f);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, a[4] ? GL_RGBA : GL_RGB, 0, 0, a[2], a[3], 0);
    gl2_rebind_fbo(c);
}
