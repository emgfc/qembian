/*
 * syborg-gl: host OpenGL ES 2 backend for the guest's GLES 1.1 rasteriser (sygl).
 *
 * The guest sends window-space triangles, clears, texture images and read-backs
 * (syborg_gl_proto.h); this device runs them synchronously, in the vCPU thread, on a surfaceless
 * EGL context of the host's GPU (Mesa), one framebuffer object per guest surface. Rows are kept
 * top-first (surface row 0 = GL row 0), so read-backs need no flip.
 *
 *   -global syborg-gl.backend=host   (default) use the host GPU when an EGL/GLES 2 context opens
 *   -global syborg-gl.backend=off    no host GL: CAPS reads 0 and the guest rasterises in software
 *
 * Device state (GL objects) is not migrated: after a snapshot restore the guest's surfaces and
 * textures are unknown here and their commands are skipped until the guest creates them again.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "qom/object.h"
#include "syborg.h"
#include "syborg_gl_proto.h"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

OBJECT_DECLARE_SIMPLE_TYPE(SyborgGlState, SYBORG_GL)

#define MAX_SURF 256
#define MAX_TEX  4096

typedef struct { uint32_t owner, id, w, h; GLuint fbo, color, depth; } GlSurf;
typedef struct { uint32_t owner, key, gen, w, h; GLuint tex; } GlTex;

struct SyborgGlState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    char *backend;
    uint32_t buf, len, out, outlen, result;
    uint32_t owner;                 /* SYGL_OWNER: whose ids/keys the commands use */
    int ready;                      /* 0 not tried, 1 ok, -1 failed */
    EGLDisplay dpy;
    EGLContext ctx;
    GLuint prog, vbo;
    GLint uSize, uTexOn, uHasRGB, uHasA, uEnv, uEnvColor, uAlphaFunc, uAlphaRef, uFog, uFogColor;
    GlSurf surf[MAX_SURF];
    GlTex tex[MAX_TEX];
    uint32_t *cmd;                  /* host copy of the command buffer */
    size_t cmdcap;
    uint8_t *pix;                   /* read-back scratch */
    size_t pixcap;
    uint64_t draws, tris;
};

static const char *vs_src =
    "attribute vec3 aPos; attribute vec4 aCol; attribute vec4 aTex;\n"   /* aTex = iw, s/w, t/w, fog */
    "uniform vec2 uSize;\n"
    "varying vec4 vCol; varying vec2 vST; varying float vFog;\n"
    "void main() {\n"
    "  float w = 1.0 / max(aTex.x, 1e-20);\n"
    "  vec3 ndc = vec3(aPos.x / uSize.x * 2.0 - 1.0, aPos.y / uSize.y * 2.0 - 1.0, aPos.z * 2.0 - 1.0);\n"
    "  gl_Position = vec4(ndc * w, w);\n"   /* perspective-correct interpolation, as sygl does */
    "  vCol = aCol / 255.0; vST = aTex.yz * w; vFog = aTex.w / 255.0;\n"
    "}\n";

/* GLES 1.1 texture environments (table 3.15/3.16 of the ES 1.1 spec), alpha test, fog */
static const char *fs_src =
    "precision mediump float;\n"
    "uniform sampler2D uTex; uniform int uTexOn, uHasRGB, uHasA, uEnv, uAlphaFunc, uFog;\n"
    "uniform vec4 uEnvColor; uniform float uAlphaRef; uniform vec3 uFogColor;\n"
    "varying vec4 vCol; varying vec2 vST; varying float vFog;\n"
    "void main() {\n"
    "  vec4 c = vCol;\n"
    "  if (uTexOn == 1) {\n"
    "    vec4 t = texture2D(uTex, vST).bgra;\n"       /* words 0xAARRGGBB uploaded as bytes */
    "    if (uEnv == 0) { if (uHasRGB == 1) c.rgb = t.rgb; if (uHasA == 1) c.a = t.a; }\n"
    "    else if (uEnv == 1) { if (uHasRGB == 1) c.rgb *= t.rgb; if (uHasA == 1) c.a *= t.a; }\n"
    "    else if (uEnv == 2) { if (uHasRGB == 1) c.rgb = mix(c.rgb, t.rgb, uHasA == 1 ? t.a : 1.0); }\n"
    "    else if (uEnv == 3) { if (uHasRGB == 1) c.rgb = mix(c.rgb, uEnvColor.rgb, t.rgb);\n"
    "                          if (uHasA == 1) c.a *= t.a; }\n"
    "    else { if (uHasRGB == 1) c.rgb = min(c.rgb + t.rgb, 1.0); if (uHasA == 1) c.a *= t.a; }\n"
    "  }\n"
    "  if (uAlphaFunc >= 0) {\n"
    "    float a = floor(clamp(c.a, 0.0, 1.0) * 255.0 + 0.5);\n"
    "    bool pass = uAlphaFunc == 7 || (uAlphaFunc == 1 && a < uAlphaRef) || (uAlphaFunc == 2 && a == uAlphaRef)\n"
    "      || (uAlphaFunc == 3 && a <= uAlphaRef) || (uAlphaFunc == 4 && a > uAlphaRef)\n"
    "      || (uAlphaFunc == 5 && a != uAlphaRef) || (uAlphaFunc == 6 && a >= uAlphaRef);\n"
    "    if (!pass) discard;\n"
    "  }\n"
    "  if (uFog == 1) c.rgb = mix(uFogColor, c.rgb, vFog);\n"
    "  gl_FragColor = c;\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        error_report("syborg-gl: shader: %s", log);
    }
    return s;
}

static int gl_init(SyborgGlState *s)
{
    if (s->ready) {
        return s->ready > 0;
    }
    s->ready = -1;
    if (s->backend && strcmp(s->backend, "host") != 0) {
        return 0;
    }
    /* client extensions only: epoxy aborts on EGL 1.5 entry points before a display exists */
    if (!epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_EXT_platform_base") ||
        !epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        warn_report("syborg-gl: no surfaceless EGL platform: software rendering in the guest");
        return 0;
    }
    s->dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (s->dpy == EGL_NO_DISPLAY || !eglInitialize(s->dpy, NULL, NULL)) {
        warn_report("syborg-gl: no surfaceless EGL display: software rendering in the guest");
        return 0;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint cattr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig cfg = NULL;
    EGLint n = 0;
    eglChooseConfig(s->dpy, cattr, &cfg, 1, &n);
    static const EGLint xattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    s->ctx = eglCreateContext(s->dpy, n ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, xattr);
    if (s->ctx == EGL_NO_CONTEXT || !eglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s->ctx)) {
        warn_report("syborg-gl: no GLES 2 context: software rendering in the guest");
        return 0;
    }
    s->prog = glCreateProgram();
    glAttachShader(s->prog, compile(GL_VERTEX_SHADER, vs_src));
    glAttachShader(s->prog, compile(GL_FRAGMENT_SHADER, fs_src));
    glBindAttribLocation(s->prog, 0, "aPos");
    glBindAttribLocation(s->prog, 1, "aCol");
    glBindAttribLocation(s->prog, 2, "aTex");
    glLinkProgram(s->prog);
    GLint ok = 0;
    glGetProgramiv(s->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        warn_report("syborg-gl: shader link failed: software rendering in the guest");
        return 0;
    }
    glUseProgram(s->prog);
#define U(n) s->n = glGetUniformLocation(s->prog, #n)
    U(uSize); U(uTexOn); U(uHasRGB); U(uHasA); U(uEnv); U(uEnvColor); U(uAlphaFunc); U(uAlphaRef);
    U(uFog); U(uFogColor);
#undef U
    glUniform1i(glGetUniformLocation(s->prog, "uTex"), 0);
    glGenBuffers(1, &s->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glEnable(GL_SCISSOR_TEST);
    info_report("syborg-gl: host GLES 2 on %s (%s)", (const char *)glGetString(GL_RENDERER),
                (const char *)glGetString(GL_VERSION));
    s->ready = 1;
    return 1;
}

static GlSurf *find_surf(SyborgGlState *s, uint32_t id)
{
    for (int i = 0; i < MAX_SURF; i++) {
        if (s->surf[i].id == id && id && s->surf[i].owner == s->owner) {
            return &s->surf[i];
        }
    }
    return NULL;
}

static void surf_free(GlSurf *g)
{
    glDeleteFramebuffers(1, &g->fbo);
    glDeleteTextures(1, &g->color);
    if (g->depth) {
        glDeleteRenderbuffers(1, &g->depth);
    }
    memset(g, 0, sizeof(*g));
}

static void surf_make(SyborgGlState *s, uint32_t id, uint32_t w, uint32_t h, uint32_t depth)
{
    GlSurf *g = find_surf(s, id);
    if (g && g->w == w && g->h == h) {
        return;
    }
    if (g) {
        surf_free(g);
    } else {
        for (int i = 0; i < MAX_SURF && !g; i++) {
            if (!s->surf[i].id) {
                g = &s->surf[i];
            }
        }
        if (!g) {
            qemu_log_mask(LOG_GUEST_ERROR, "syborg-gl: too many surfaces\n");
            return;
        }
    }
    g->owner = s->owner; g->id = id; g->w = w; g->h = h;
    glGenTextures(1, &g->color);
    glBindTexture(GL_TEXTURE_2D, g->color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &g->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->color, 0);
    if (depth) {
        glGenRenderbuffers(1, &g->depth);
        glBindRenderbuffer(GL_RENDERBUFFER, g->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g->depth);
    }
    /* a new EGL surface's content is undefined; the guest's software buffers start black */
    glViewport(0, 0, w, h);
    glScissor(0, 0, w, h);
    glColorMask(1, 1, 1, 1);
    glDepthMask(1);
    glClearColor(0, 0, 0, 0);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | (depth ? GL_DEPTH_BUFFER_BIT : 0));
}

static GlTex *find_tex(SyborgGlState *s, uint32_t key)
{
    for (int i = 0; i < MAX_TEX; i++) {
        if (s->tex[i].key == key && key && s->tex[i].owner == s->owner) {
            return &s->tex[i];
        }
    }
    return NULL;
}

static void tex_upload(SyborgGlState *s, const uint32_t *a, size_t words)
{
    uint32_t key = a[0], gen = a[1], w = a[2], h = a[3], y0 = a[4], rows = a[5];
    if (!w || !h || y0 + rows > h || (size_t)w * rows + 6 > words) {
        return;
    }
    GlTex *t = find_tex(s, key);
    for (int i = 0; i < MAX_TEX && !t; i++) {
        if (!s->tex[i].key) {
            t = &s->tex[i];
            t->owner = s->owner;
            t->key = key;
            glGenTextures(1, &t->tex);
        }
    }
    if (!t) {
        qemu_log_mask(LOG_GUEST_ERROR, "syborg-gl: too many textures\n");
        return;
    }
    glBindTexture(GL_TEXTURE_2D, t->tex);
    if (y0 == 0) {      /* a new image: (re)define it */
        t->gen = gen; t->w = w; t->h = h;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, w, rows, GL_RGBA, GL_UNSIGNED_BYTE, a + 6);
}

static void bind_surf(GlSurf *g)
{
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glViewport(0, 0, g->w, g->h);
}

static void scissor(GlSurf *g, int x0, int y0, int x1, int y1)
{
    x0 = MAX(x0, 0); y0 = MAX(y0, 0);
    x1 = MIN(x1, (int)g->w); y1 = MIN(y1, (int)g->h);
    glScissor(x0, y0, MAX(x1 - x0, 0), MAX(y1 - y0, 0));
}

static void gl_draw(SyborgGlState *s, const uint32_t *a, size_t words)
{
    GlSurf *g = find_surf(s, a[0]);
    const SyglState *st = (const SyglState *)(a + 1);
    uint32_t n = a[1 + SYGL_STATE_WORDS];
    const float *v = (const float *)(a + 2 + SYGL_STATE_WORDS);
    if (!g || n == 0 || 2 + SYGL_STATE_WORDS + (size_t)n * SYGL_VTX_WORDS > words) {
        return;
    }
    bind_surf(g);
    scissor(g, st->scX0, st->scY0, st->scX1, st->scY1);
    if (st->depthTest && g->depth) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(st->depthFunc);
        glDepthMask(st->depthWrite);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    if (st->polyOffset) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(st->polyFactor, st->polyUnits);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    if (st->blend) {
        glEnable(GL_BLEND);
        glBlendFunc(st->blendSrc, st->blendDst);
    } else {
        glDisable(GL_BLEND);
    }
    glColorMask(st->writeColor, st->writeColor, st->writeColor, st->writeColor);
    GlTex *t = st->texKey ? find_tex(s, st->texKey) : NULL;
    glUniform1i(s->uTexOn, t != NULL);
    if (t) {
        glBindTexture(GL_TEXTURE_2D, t->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, st->texLinear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, st->texLinear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, st->texClampS ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, st->texClampT ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glUniform1i(s->uHasRGB, st->texHasRGB);
        glUniform1i(s->uHasA, st->texHasA);
        int env = st->envMode == 0x1E01 ? 0 : st->envMode == 0x2100 ? 1 : st->envMode == 0x2101 ? 2
                : st->envMode == 0x0BE2 ? 3 : 4;
        glUniform1i(s->uEnv, env);
        uint32_t e = st->envColor;
        glUniform4f(s->uEnvColor, ((e >> 16) & 255) / 255.0f, ((e >> 8) & 255) / 255.0f, (e & 255) / 255.0f,
                    (e >> 24) / 255.0f);
    }
    glUniform1i(s->uAlphaFunc, st->alphaTest ? (int)(st->alphaFunc - 0x0200) : -1);
    glUniform1f(s->uAlphaRef, st->alphaRef);
    glUniform1i(s->uFog, st->fog != 0);
    glUniform3f(s->uFogColor, ((st->fogColor >> 16) & 255) / 255.0f, ((st->fogColor >> 8) & 255) / 255.0f,
                (st->fogColor & 255) / 255.0f);
    glUniform2f(s->uSize, g->w, g->h);
    glBufferData(GL_ARRAY_BUFFER, n * sizeof(SyglVtx), v, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SyglVtx), (void *)offsetof(SyglVtx, x));
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(SyglVtx), (void *)offsetof(SyglVtx, r));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(SyglVtx), (void *)offsetof(SyglVtx, iw));
    glDrawArrays(GL_TRIANGLES, 0, n);
    s->draws++;
    s->tris += n / 3;
}

static void gl_clear(SyborgGlState *s, const uint32_t *a)
{
    GlSurf *g = find_surf(s, a[0]);
    if (!g) {
        return;
    }
    bind_surf(g);
    scissor(g, a[4], a[5], a[4] + a[6], a[5] + a[7]);
    GLbitfield m = 0;
    if (a[1] & 1) {
        glColorMask(1, 1, 1, 1);
        glClearColor(((a[2] >> 16) & 255) / 255.0f, ((a[2] >> 8) & 255) / 255.0f, (a[2] & 255) / 255.0f, 1.0f);
        m |= GL_COLOR_BUFFER_BIT;
    }
    if ((a[1] & 2) && g->depth) {
        glDepthMask(1);
        glClearDepthf(a[3] / 65535.0f);
        m |= GL_DEPTH_BUFFER_BIT;
    }
    if (m) {
        glClear(m);
    }
}

static int readback(SyborgGlState *s, const uint32_t *a)
{
    GlSurf *g = find_surf(s, a[0]);
    if (!g) {
        return 0;
    }
    uint32_t y0 = a[2], rows = a[3];
    if (y0 + rows > g->h) {
        return -1;
    }
    size_t bytes = (size_t)g->w * rows * 4;
    if ((uint64_t)a[1] + bytes > s->outlen) {
        return -1;
    }
    if (s->pixcap < bytes) {
        s->pix = g_realloc(s->pix, bytes);
        s->pixcap = bytes;
    }
    bind_surf(g);
    glReadPixels(0, y0, g->w, rows, GL_RGBA, GL_UNSIGNED_BYTE, s->pix);
    uint32_t *w = (uint32_t *)s->pix;
    for (size_t i = 0; i < bytes / 4; i++) {          /* R G B A bytes -> 0x00RRGGBB words */
        const uint8_t *p = s->pix + i * 4;
        w[i] = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    }
    address_space_write(&address_space_memory, s->out + a[1], MEMTXATTRS_UNSPECIFIED, s->pix, bytes);
    return 0;
}

static uint32_t run(SyborgGlState *s)
{
    if (!gl_init(s) || s->len < 8 || s->len > 64 * 1024 * 1024) {
        return 1;
    }
    if (s->cmdcap < s->len) {
        s->cmd = g_realloc(s->cmd, s->len);
        s->cmdcap = s->len;
    }
    address_space_read(&address_space_memory, s->buf, MEMTXATTRS_UNSPECIFIED, s->cmd, s->len);
    eglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s->ctx);
    size_t total = s->len / 4, i = 0;
    while (i + 2 <= total) {
        uint32_t op = s->cmd[i], words = s->cmd[i + 1];
        if (words < 2 || i + words > total) {
            return i + 1;
        }
        const uint32_t *a = s->cmd + i + 2;
        size_t n = words - 2;
        switch (op) {
        case SYGL_SURF: if (n >= 4) surf_make(s, a[0], a[1], a[2], a[3]); break;
        case SYGL_SURF_FREE: { GlSurf *g = find_surf(s, a[0]); if (g) surf_free(g); break; }
        case SYGL_TEX: tex_upload(s, a, n); break;
        case SYGL_TEX_FREE: {
            GlTex *t = find_tex(s, a[0]);
            if (t) { glDeleteTextures(1, &t->tex); memset(t, 0, sizeof(*t)); }
            break;
        }
        case SYGL_CLEAR: if (n >= 8) gl_clear(s, a); break;
        case SYGL_DRAW: gl_draw(s, a, n); break;
        case SYGL_READ: if (n >= 4 && readback(s, a)) return i + 1; break;
        case SYGL_OWNER: if (n >= 1) s->owner = a[0]; break;
        case SYGL_OWNER_FREE:
            for (int k = 0; n >= 1 && k < MAX_SURF; k++) {
                if (s->surf[k].id && s->surf[k].owner == a[0]) surf_free(&s->surf[k]);
            }
            for (int k = 0; n >= 1 && k < MAX_TEX; k++) {
                if (s->tex[k].key && s->tex[k].owner == a[0]) {
                    glDeleteTextures(1, &s->tex[k].tex);
                    memset(&s->tex[k], 0, sizeof(s->tex[k]));
                }
            }
            break;
        default: return i + 1;
        }
        i += words;
    }
    return 0;
}

static uint64_t syborg_gl_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgGlState *s = opaque;
    switch (offset >> 2) {
    case SYGL_REG_ID: return SYGL_ID;
    case SYGL_REG_CAPS: return gl_init(s) ? 1 : 0;
    case SYGL_REG_BUF: return s->buf;
    case SYGL_REG_LEN: return s->len;
    case SYGL_REG_EXEC: return s->result;
    case SYGL_REG_OUT: return s->out;
    case SYGL_REG_OUTLEN: return s->outlen;
    }
    return 0;
}

static void syborg_gl_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SyborgGlState *s = opaque;
    switch (offset >> 2) {
    case SYGL_REG_BUF: s->buf = value; break;
    case SYGL_REG_LEN: s->len = value; break;
    case SYGL_REG_EXEC: s->result = run(s); break;
    case SYGL_REG_OUT: s->out = value; break;
    case SYGL_REG_OUTLEN: s->outlen = value; break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "syborg-gl: write 0x%x ignored\n", (int)offset);
    }
}

static const MemoryRegionOps syborg_gl_ops = {
    .read = syborg_gl_read,
    .write = syborg_gl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void syborg_gl_realize(DeviceState *dev, Error **errp)
{
    SyborgGlState *s = SYBORG_GL(dev);
    memory_region_init_io(&s->iomem, OBJECT(s), &syborg_gl_ops, s, "syborg-gl", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const Property syborg_gl_properties[] = {
    DEFINE_PROP_STRING("backend", SyborgGlState, backend),
};

static void syborg_gl_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = syborg_gl_realize;
    device_class_set_props(dc, syborg_gl_properties);
}

static const TypeInfo syborg_gl_types[] = {
    {
        .name = TYPE_SYBORG_GL,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SyborgGlState),
        .class_init = syborg_gl_class_init,
    },
};

DEFINE_TYPES(syborg_gl_types)
