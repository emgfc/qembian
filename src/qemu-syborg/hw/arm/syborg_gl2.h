/*
 * syborg-gl: OpenGL ES 2.0 replay (docs/devices/nokia-n8-rm596/gles2-host.md). Included by syborg_gl.c
 * (it uses the device's state and helpers). The guest (src/khr/gl2.c) records each OpenGL ES 2.0 call as
 * SYGL_GL2 {ctx, fn, words}; here every guest context has a host EGL context of its own, sharing with
 * the device's context (surface textures and renderbuffers, and all shareable objects, live in that one
 * share group). Guest object names are mapped to host names per guest share group (framebuffers per
 * context); framebuffer 0 is the current surface's framebuffer in the guest context's host context.
 * The calls are replayed by syborg_gles.h (generated cases: tools/gen-gles.py).
 */
#include "syborg_gl2_fn.h"
#include "syborg_gl2_precision.h"

enum { GL2_N_BUF, GL2_N_TEX, GL2_N_RBO, GL2_N_FBO, GL2_N_PROG, GL2_N_SHADER, GL2_N_KINDS };

/* a share group (S5): its name maps and counters live as long as one of its contexts does; its host
   objects are deleted with the last one */
typedef struct Gl2Share {
    int refs;
    GHashTable *names[GL2_N_KINDS];     /* guest name -> host name (all kinds but framebuffers) */
    uint32_t next[GL2_N_KINDS];         /* guest names handed out */
} Gl2Share;

typedef struct Gl2HostCtx {
    uint64_t k;                         /* owner << 32 | guest context */
    uint32_t owner, id;
    Gl2Share *sg;                       /* its share group */
    EGLContext hc;
    GHashTable *names[GL2_N_KINDS];     /* framebuffers only (a context's own) */
    GHashTable *sfbo;                   /* surface id -> this context's framebuffer for it */
    uint32_t surf, sw, sh;              /* current surface */
    GLuint surfFbo;
    GLuint fbo;                         /* guest framebuffer bound (guest name; 0 = the surface) */
    int made;
    int api;                            /* 1: OpenGL ES 1.1 (syborg_gl1.h), 2: OpenGL ES 2 */
    struct Gl1State *g1;                /* ES 1.1: the matrix palette */
    /* the guest's vertex arrays (ES 2 attributes; ES 1.1 slots GL1_SLOT_*): client arrays are the caller's
       memory, read at each draw; buffer arrays are set in the host context too */
    struct GlesArray {
        int enabled, size, type, norm, stride;
        uint32_t va;                    /* client array: its address in the caller (0: none) */
        uintptr_t off;                  /* buffer array: offset */
        GLuint buf;                     /* the host buffer (0: a client array) */
    } arr[16];
    uint32_t next[GL2_N_KINDS];         /* framebuffer names handed out (the rest: the share group's) */
    struct { uint32_t va, size; GLuint buf; } map[2];   /* OES_mapbuffer: ARRAY, ELEMENT_ARRAY */
    GLenum err;                         /* an error decided by the replay itself (reported first) */
} Gl2HostCtx;
typedef struct GlesArray Gl1Array;

static GHashTable *gl2_ctxs;            /* (owner << 32 | ctx) -> Gl2HostCtx* */
static void gl1_state_init(Gl2HostCtx *c);  /* syborg_gl1.h */

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
#define U2F(x) u2f(x)

/* the device's own context current again (every command but SYGL_GL2*) */
static void gl2_main(SyborgGlState *s)
{
    if (eglGetCurrentContext() != s->ctx) {
        eglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s->ctx);
    }
}

static void gl2_use(SyborgGlState *s, Gl2HostCtx *c)
{
    if (eglGetCurrentContext() != c->hc) {
        eglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, c->hc);
    }
}

static Gl2HostCtx *gl2_find(SyborgGlState *s, uint32_t id)
{
    uint64_t k = okey(gl_ex->owner, id);
    return gl2_ctxs ? g_hash_table_lookup(gl2_ctxs, &k) : NULL;
}

static GHashTable *gl2_map(Gl2HostCtx *c, int kind)
{
    return kind == GL2_N_FBO ? c->names[kind] : c->sg->names[kind];
}

/* the host name of a guest name; unknown names are created when `create` (binds, glGen*) */
static GLuint gl2_name(Gl2HostCtx *c, int kind, uint32_t guest, int create)
{
    if (!guest) {
        return kind == GL2_N_FBO ? c->surfFbo : 0;
    }
    GHashTable *m = gl2_map(c, kind);
    gpointer v = g_hash_table_lookup(m, GUINT_TO_POINTER(guest));
    if (v || !create) {
        return GPOINTER_TO_UINT(v);
    }
    if (g_hash_table_size(m) >= (1u << 18)) {   /* S6: names of one kind per share group */
        c->err = c->err ? c->err : GL_OUT_OF_MEMORY;
        return 0;
    }
    GLuint h = 0;
    switch (kind) {
    case GL2_N_BUF: glGenBuffers(1, &h); break;
    case GL2_N_TEX: glGenTextures(1, &h); break;
    case GL2_N_RBO: if (c->api == 1) glGenRenderbuffersOES(1, &h); else glGenRenderbuffers(1, &h); break;
    case GL2_N_FBO: if (c->api == 1) glGenFramebuffersOES(1, &h); else glGenFramebuffers(1, &h); break;
    default: return 0;
    }
    g_hash_table_insert(m, GUINT_TO_POINTER(guest), GUINT_TO_POINTER(h));
    return h;
}

/* the guest name of a host name (queries that return names), 0 if none */
static uint32_t gl2_guest(Gl2HostCtx *c, int kind, GLuint host)
{
    GHashTableIter it;
    gpointer k, v;
    if (!host) {
        return 0;
    }
    g_hash_table_iter_init(&it, gl2_map(c, kind));
    while (g_hash_table_iter_next(&it, &k, &v)) {
        if (GPOINTER_TO_UINT(v) == host) {
            return GPOINTER_TO_UINT(k);
        }
    }
    return 0;
}

static void gl2_unmap(Gl2HostCtx *c, int kind, uint32_t guest)
{
    g_hash_table_remove(gl2_map(c, kind), GUINT_TO_POINTER(guest));
}

/* results go to the guest's output area (one call with results per batch flush) */
static int gl2_out(SyborgGlState *s, const void *p, size_t n)
{
    if (n > gl_ex->outlen) {
        n = gl_ex->outlen;
    }
    return gmem_rw(s, gl_ex->out, (void *)p, n, true);
}

static int gl2_out_words(SyborgGlState *s, const uint32_t *w, int count)
{
    uint32_t buf[18];
    buf[0] = count;
    memcpy(buf + 1, w, MIN(count, 17) * 4);
    return gl2_out(s, buf, (1 + MIN(count, 17)) * 4);
}

/* the last context of a share group gone: its objects (they live in the device context's share group too,
   so destroying the contexts alone would keep them) */
static void gl2_share_unref(SyborgGlState *s, Gl2Share *g)
{
    if (--g->refs > 0) {
        return;
    }
    gl2_main(s);
    for (int k = 0; k < GL2_N_KINDS; k++) {
        GHashTableIter it;
        gpointer gn, h;
        if (!g->names[k]) {
            continue;
        }
        g_hash_table_iter_init(&it, g->names[k]);
        while (g_hash_table_iter_next(&it, &gn, &h)) {
            GLuint n = GPOINTER_TO_UINT(h);
            switch (k) {
            case GL2_N_BUF: glDeleteBuffers(1, &n); break;
            case GL2_N_TEX: glDeleteTextures(1, &n); break;
            case GL2_N_RBO: glDeleteRenderbuffers(1, &n); break;
            case GL2_N_PROG: glDeleteProgram(n); break;
            case GL2_N_SHADER: glDeleteShader(n); break;
            }
        }
        g_hash_table_destroy(g->names[k]);
    }
    g_free(g);
}

/* a context already taken out of gl2_ctxs: its host context (and with it its framebuffers), then its group */
static void gl2_ctx_free(SyborgGlState *s, Gl2HostCtx *c)
{
    budget_give(s, c->owner, BUDGET_CTX);
    gl2_main(s);
    eglDestroyContext(s->dpy, c->hc);
    g_free(c->g1);
    if (c->names[GL2_N_FBO]) {
        g_hash_table_destroy(c->names[GL2_N_FBO]);
    }
    g_hash_table_destroy(c->sfbo);
    gl2_share_unref(s, c->sg);
    g_free(c);
}

static void gl2_new(SyborgGlState *s, uint32_t id, uint32_t share, uint32_t api)
{
    if (!gl2_ctxs) {
        gl2_ctxs = g_hash_table_new(g_int64_hash, g_int64_equal);
    }
    Gl2HostCtx *old = gl2_find(s, id);
    if (old) {                          /* an id made again: the old context is destroyed first */
        g_hash_table_remove(gl2_ctxs, &old->k);
        gl2_ctx_free(s, old);
    }
    /* ES 1.1 contexts share with the device's ES 3 context too (Mesa: textures, renderbuffers, buffers) */
    const EGLint attr[] = { EGL_CONTEXT_CLIENT_VERSION, api == 1 ? 1 : s->es3 ? 3 : 2, EGL_NONE };
    if (budget_take(s, BUDGET_CTX)) {
        return;
    }
    EGLContext hc = eglCreateContext(s->dpy, gl2_cfg, s->ctx, attr);
    if (hc == EGL_NO_CONTEXT) {
        budget_give(s, gl_ex->owner, BUDGET_CTX);
        warn_report("syborg-gl: no host context for an OpenGL ES %s context (0x%x)", api == 1 ? "1.1" : "2",
                    eglGetError());
        return;
    }
    Gl2HostCtx *c = g_new0(Gl2HostCtx, 1);
    c->owner = gl_ex->owner;
    c->id = id;
    c->k = okey(gl_ex->owner, id);
    c->hc = hc;
    c->api = api == 1 ? 1 : 2;
    if (c->api == 1) {
        gl1_state_init(c);
    }
    Gl2HostCtx *sh = share ? gl2_find(s, share) : NULL;
    if (sh && sh->api != c->api) {
        sh = NULL;
    }
    if (sh) {
        c->sg = sh->sg;
    } else {
        c->sg = g_new0(Gl2Share, 1);
        for (int k = 0; k < GL2_N_KINDS; k++) {
            if (k != GL2_N_FBO) {
                c->sg->names[k] = g_hash_table_new(g_direct_hash, g_direct_equal);
            }
        }
    }
    c->sg->refs++;
    c->names[GL2_N_FBO] = g_hash_table_new(g_direct_hash, g_direct_equal);
    c->sfbo = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_insert(gl2_ctxs, &c->k, c);
}

static void gl2_destroy(SyborgGlState *s, Gl2HostCtx *c)
{
    g_hash_table_remove(gl2_ctxs, &c->k);
    gl2_ctx_free(s, c);
}

typedef struct { SyborgGlState *s; uint32_t owner; } Gl2Drop;

/* owner ~0: every context (a machine reset) */
static gboolean gl2_owner_drop(gpointer k, gpointer v, gpointer p)
{
    Gl2HostCtx *c = v;
    Gl2Drop *d = p;
    if (d->owner != ~0u && c->owner != d->owner) {
        return FALSE;
    }
    gl2_ctx_free(d->s, c);
    return TRUE;
}

/* SYGL_OWNER_FREE: a process's contexts go with it */
static void gl2_owner_free(SyborgGlState *s, uint32_t owner)
{
    Gl2Drop d = { s, owner };
    if (gl2_ctxs) {
        gl2_main(s);
        g_hash_table_foreach_remove(gl2_ctxs, gl2_owner_drop, &d);
    }
}

/* bind a framebuffer in the current context (ES 1.1: the OES names, the only ones it has) */
static void gl2_bind_fbo(Gl2HostCtx *c, GLuint f)
{
    if (c->api == 1) {
        glBindFramebufferOES(GL_FRAMEBUFFER_OES, f);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, f);
    }
}

/* the framebuffer bound for the guest's binding: its framebuffer, or the current surface's */
static void gl2_rebind_fbo(Gl2HostCtx *c)
{
    gl2_bind_fbo(c, c->fbo ? gl2_name(c, GL2_N_FBO, c->fbo, 1) : c->surfFbo);
}

/* the surface's framebuffer in this context (current), made on first use: its colour texture and
   depth/stencil renderbuffer are the surface's, made in the device's context */
static GLuint gl2_surf_fbo(SyborgGlState *s, Gl2HostCtx *c, uint32_t surf)
{
    GlSurf *g = surf ? find_surf(s, surf) : NULL;
    if (!g) {
        return 0;
    }
    GLuint f = GPOINTER_TO_UINT(g_hash_table_lookup(c->sfbo, GUINT_TO_POINTER(surf)));
    if (f) {
        return f;
    }
    GLenum st;
    if (c->api == 1) {
        glGenFramebuffersOES(1, &f);
        glBindFramebufferOES(GL_FRAMEBUFFER_OES, f);
        glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, g->color, 0);
        if (g->depth) {
            glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_DEPTH_ATTACHMENT_OES, GL_RENDERBUFFER_OES, g->depth);
            if (g->stencil) {
                glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_STENCIL_ATTACHMENT_OES, GL_RENDERBUFFER_OES, g->depth);
            }
        }
        st = glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES);
    } else {
        glGenFramebuffers(1, &f);
        glBindFramebuffer(GL_FRAMEBUFFER, f);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->color, 0);
        if (g->depth) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g->depth);
            if (g->stencil) {
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g->depth);
            }
        }
        st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    }
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        warn_report("syborg-gl: OpenGL ES %s surface %ux%u framebuffer incomplete (0x%x)", c->api == 1 ? "1.1" : "2",
                    g->w, g->h, st);
    }
    g_hash_table_insert(c->sfbo, GUINT_TO_POINTER(surf), GUINT_TO_POINTER(f));
    return f;
}

/* eglMakeCurrent: the default framebuffer is now the surface's */
static void gl2_current_surf(SyborgGlState *s, Gl2HostCtx *c, uint32_t surf, uint32_t w, uint32_t h)
{
    gl2_use(s, c);
    c->surf = surf; c->sw = w; c->sh = h;
    c->surfFbo = gl2_surf_fbo(s, c, surf);
    if (!c->fbo) {
        gl2_bind_fbo(c, c->surfFbo);
    } else {
        gl2_rebind_fbo(c);
    }
    if (!c->made && surf) {             /* EGL: the first make-current sets viewport and scissor */
        glViewport(0, 0, w, h);
        glScissor(0, 0, w, h);
        c->made = 1;
    }
}

/* glReadPixels in a format/type the host driver does not serve (GLES 2 guarantees only RGBA/UNSIGNED_BYTE
   and one implementation format; the N8's driver also reads GL_RGB, which GTA III's re3 uses to grab the
   frame for its menu background): read RGBA8 and pack the guest's format/type. */
static void gl2_pack_rgba(const uint8_t *src, uint8_t *dst, int w, int h, size_t row, GLenum fmt, GLenum type)
{
    for (int y = 0; y < h; y++) {
        const uint8_t *p = src + (size_t)y * w * 4;
        uint8_t *d = dst + (size_t)y * row;
        for (int x = 0; x < w; x++, p += 4) {
            uint16_t v;
            switch (type) {
            case GL_UNSIGNED_SHORT_5_6_5:
                v = (p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3;
                memcpy(d, &v, 2); d += 2; break;
            case GL_UNSIGNED_SHORT_4_4_4_4:
                v = (p[0] >> 4) << 12 | (p[1] >> 4) << 8 | (p[2] >> 4) << 4 | p[3] >> 4;
                memcpy(d, &v, 2); d += 2; break;
            case GL_UNSIGNED_SHORT_5_5_5_1:
                v = (p[0] >> 3) << 11 | (p[1] >> 3) << 6 | (p[2] >> 3) << 1 | p[3] >> 7;
                memcpy(d, &v, 2); d += 2; break;
            default:                        /* GL_UNSIGNED_BYTE */
                switch (fmt) {
                case GL_RGB: *d++ = p[0]; *d++ = p[1]; *d++ = p[2]; break;
                case GL_ALPHA: *d++ = p[3]; break;
                case GL_LUMINANCE: *d++ = p[0]; break;
                case GL_LUMINANCE_ALPHA: *d++ = p[0]; *d++ = p[3]; break;
                default: memcpy(d, p, 4); d += 4; break;
                }
            }
        }
    }
}

static int gl2_count(GLenum pname)
{
    switch (pname) {
    case 0x0BA2: case 0x0C10: case 0x0C23: case 0x0C22: case 0x8005: return 4;  /* viewport, scissor, masks, colours */
    case 0x0B70: case 0x846D: case 0x846E: case 0x0D3A: return 2;              /* ranges, max viewport */
    default: return 1;
    }
}

static int gl2_uniform_count(GLenum type)
{
    switch (type) {
    case GL_FLOAT_VEC2: case GL_INT_VEC2: case GL_BOOL_VEC2: return 2;
    case GL_FLOAT_VEC3: case GL_INT_VEC3: case GL_BOOL_VEC3: return 3;
    case GL_FLOAT_VEC4: case GL_INT_VEC4: case GL_BOOL_VEC4: case GL_FLOAT_MAT2: return 4;
    case GL_FLOAT_MAT3: return 9;
    case GL_FLOAT_MAT4: return 16;
    default: return 1;
    }
}

/* components of the uniform at `loc` (glGetUniform*v writes as many) */
static int gl2_uniform_size(GLuint prog, GLint loc)
{
    GLint n = 0;
    char name[256];
    glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &n);
    for (GLint i = 0; i < n; i++) {
        GLint size;
        GLenum type;
        GLsizei len = 0;
        glGetActiveUniform(prog, i, sizeof(name) - 4, &len, &size, &type, name);
        for (GLint e = 0; e < size; e++) {
            char nm[270];
            char *br = strchr(name, '[');
            if (br) {
                *br = 0;
            }
            if (size > 1) {
                snprintf(nm, sizeof(nm), "%s[%d]", name, e);
            } else {
                snprintf(nm, sizeof(nm), "%s", name);
            }
            if (glGetUniformLocation(prog, nm) == loc) {
                return gl2_uniform_count(type);
            }
        }
    }
    return 1;
}

