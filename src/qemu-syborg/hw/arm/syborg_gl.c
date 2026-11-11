/*
 * syborg-gl: host OpenGL ES 2 backend for the guest's GLES 1.1 rasteriser (sygl).
 *
 * The guest sends window-space triangles, clears, texture images and read-backs
 * (syborg_gl_proto.h); this device runs them synchronously, in the vCPU thread, on a surfaceless
 * EGL context of the host's GPU (Mesa), one framebuffer object per guest surface. Rows are kept
 * top-first (surface row 0 = GL row 0), so read-backs need no flip.
 *
 * OpenVG (src/khr) draws into multisampled surfaces (anti-aliased fills, resolved when read), and
 * the N8's composition backend (src/gcebe) has the screen composed here: EGL windows' frames stay
 * on the GPU (SYGL_VG_PRESENT), the other GCE surfaces are read from guest memory, and only the
 * finished screen is written to the frame buffer. Buffers may be named by guest-virtual address
 * (SYGL_REG_KICK): read and written in place, no copies in the guest.
 *
 *   -global syborg-gl.backend=host   (default) use the host GPU when an EGL/GLES context opens
 *   -global syborg-gl.backend=off    no host GL: CAPS reads 0 and the guest rasterises in software
 *   -global syborg-gl.msaa=N         samples of OpenVG surfaces (default 8; 0 = no anti-aliasing)
 *   -global syborg-gl.frame-file=F   a 4 KB shared file: word 0 "FSEQ", word 1 a sequence bumped
 *                                    before and after each host-composed frame is written to the
 *                                    frame buffer (odd = writing): readers in other processes
 *                                    (sydb phone) copy whole frames and only new ones
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
#include "hw/core/cpu.h"
#include "system/address-spaces.h"
#include "qom/object.h"
#include "qapi/visitor.h"
#include "syborg.h"
#include "syborg_gl_proto.h"
#include "syvg.h"                   /* the host EGL/OpenVG (src/syvg, libsyvg.a) */
#include <math.h>
#include <sys/mman.h>
#include "qemu/atomic.h"
#include "qemu/timer.h"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

OBJECT_DECLARE_SIMPLE_TYPE(SyborgGlState, SYBORG_GL)

#define MAX_SURF 4096                   /* sanity caps: hash tables, these only stop runaways */
#define MAX_TEX  65536

#define MAX_SCAN 64
#define RAMP_N 1024

/* fbo/color: the surface's pixels (a texture); multisampled surfaces draw into msfbo and are
   resolved into color when something reads them (dirty) */
typedef struct {
    uint64_t k;                         /* hash key: owner << 32 | id */
    uint32_t owner, id, w, h; GLuint fbo, color, depth; int stencil;
    GLuint msfbo, mscolor, msdepth; int dirty;
    int want_msaa;                      /* an OpenVG surface: follows the `msaa` setting */
    int samples;                        /* its sample count now (0: single-sampled) */
} GlSurf;
typedef struct { uint64_t k; uint32_t owner, key, gen, w, h; GLuint tex; } GlTex;
/* a GCE surface's image for composition: from an EGL window (host: GL rows bottom-up) or read from
   guest memory (rows top-down, guest pixel format) */
typedef struct { uint32_t sid[4]; GLuint tex; uint32_t w, h, fmt; int host, used; } GlScan;

struct SyborgGlState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    char *backend;
    char *frame_file;               /* property: shared frame sequence (SyglFrameSeq), "" = none */
    char *identity;                 /* property: the device's GL identity (key=value lines: tools/device.py) */
    char *egl_configs;              /* property: the device's EGL configs (the real phone's: tools/egl-configs.py) */
    volatile uint32_t *frame_seq;   /* its mapping: [0] magic, [1] sequence (odd while writing), [2] width,
                                       [3] height, [4] 1 = the pixels at byte 4096 are the screen */
    uint8_t *hframe;                /* the host-composed screen (syborg_gl_frame): what the display shows */
    uint32_t hfW, hfH;
    size_t hfCap;
    bool hfOn;
    uint32_t msaa;                  /* property (runtime): samples wanted for OpenVG surfaces */
    uint32_t msaa_applied;          /* the value `samples` was last computed from */
    int filter;                     /* property (runtime) "filter": 0 auto, 1 nearest, 2 linear */
    bool compose_host;              /* property (runtime) "compose": host (default) or guest */
    uint32_t buf, len, out, outlen, result;
    int va;                         /* buf/out are guest-virtual (SYGL_REG_KICK) */
    int es3;                        /* GLES 3 context: multisampling, row length */
    int has_minmax;                 /* GL_MIN/GL_MAX blend equations (ES3 or EXT_blend_minmax) */
    int has_ds;                     /* packed depth/stencil (ES3 or OES_packed_depth_stencil) */
    uint64_t glerrors;              /* GL errors seen (logged the first few) */
    /* statistics (SYBORG_GL_STATS=1 prints them every 5 s; the `stats` property later) */
    uint64_t st_kicks, st_kick_ns, st_compose, st_compose_ns, st_read_ns, st_fills, st_dropped;
    uint64_t st_last_print;
    GLint samples;                  /* OpenVG surfaces' samples (0: single-sampled) */
    uint32_t owner;                 /* SYGL_OWNER: whose ids/keys the commands use */
    int ready;                      /* 0 not tried, 1 ok, -1 failed */
    EGLDisplay dpy;
    EGLContext ctx;
    GLuint prog, vbo;
    GLuint vgProg;                  /* OpenVG: solid paint (src/khr, vg-host.md) */
    GLint vgSize, vgColor, vgKind, vgMode, vgM0, vgM1, vgM2, vgImgSize, vgTex;
    GLint vgG, vgR, vgSpread, vgRamp, vgCT, vgCTs, vgCTb;
    GLuint rampTex;
    uint8_t ramp[RAMP_N * 4];
    GLuint cpProg;                  /* composition */
    GLint cpSize, cpTex, cpFmt, cpAlpha;
    GLuint scrFbo, scrTex;
    uint32_t scrW, scrH;
    GlScan scan[MAX_SCAN];
    uint8_t *cpix;                  /* composition scratch */
    /* asynchronous frame read-back (GLES 3): compose reads into a PBO with a fence and returns;
       a timer on the main loop writes the frame into guest RAM when the GPU is done */
    GLuint pbo;
    GLsync fence;
    int pending;                    /* a frame in the PBO not yet written */
    uint32_t pW, pH, pStride;
    hwaddr *pPages;                 /* guest-physical page of each 4K page of the frame buffer */
    vaddr pBase;                    /* its first byte (virtual) */
    size_t pNPages, pPagesCap;
    QEMUTimer *rb_timer;
    uint64_t st_async, st_async_wait;
    size_t cpixcap;
    float *vgv;                     /* vertex scratch for fills */
    size_t vgvcap;
    GLint uSize, uTexOn, uHasRGB, uHasA, uEnv, uEnvColor, uAlphaFunc, uAlphaRef, uFog, uFogColor;
    GHashTable *surfs, *texs;       /* (owner << 32 | id/key) -> GlSurf* / GlTex* (owned) */
    uint32_t *cmd;                  /* host copy of the command buffer */
    size_t cmdcap;
    uint8_t *pix;                   /* read-back scratch */
    size_t pixcap;
    uint8_t *pix2;                  /* GL2 read-back as RGBA before converting (syborg_gl2.h) */
    size_t pix2cap;
    int gl2_readconv;               /* converted GL2 read-backs logged */
    int gl2_fb0_logs;               /* refused GL2 attachments to framebuffer 0 logged */
    uint64_t draws, tris;
    bool host_batch;                /* running a host batch (syvg): no guest memory is touched */
    uint8_t *host_out;              /* its results go here (byte offsets as SYGL_REG_OUT offsets) */
    size_t host_outlen;
};

static EGLConfig gl2_cfg;               /* OpenGL ES 2 contexts (syborg_gl2.h) are made with this config */

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

static int gl_has_minmax = 1;           /* SyborgGlState.has_minmax, for vg_blend */

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
    for (int v = 3; v >= 2 && (s->ctx == EGL_NO_CONTEXT || !s->ctx); v--) {   /* GLES 3 if we can */
        const EGLint cattr[] = { EGL_RENDERABLE_TYPE, v == 3 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT, EGL_NONE };
        EGLConfig cfg = NULL;
        EGLint n = 0;
        eglChooseConfig(s->dpy, cattr, &cfg, 1, &n);
        const EGLint xattr[] = { EGL_CONTEXT_CLIENT_VERSION, v, EGL_NONE };
        s->ctx = eglCreateContext(s->dpy, n ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, xattr);
        gl2_cfg = n ? cfg : EGL_NO_CONFIG_KHR;     /* OpenGL ES 2 contexts share with this one */
        if (s->ctx != EGL_NO_CONTEXT && !eglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s->ctx)) {
            eglDestroyContext(s->dpy, s->ctx);
            s->ctx = EGL_NO_CONTEXT;
        }
        s->es3 = v == 3 && s->ctx != EGL_NO_CONTEXT;
    }
    if (s->ctx == EGL_NO_CONTEXT) {
        warn_report("syborg-gl: no GLES context: software rendering in the guest");
        return 0;
    }
    {   /* what a GLES 2 context may lack (GLES 3 has both) */
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        s->has_minmax = s->es3 || (ext && strstr(ext, "GL_EXT_blend_minmax"));
        s->has_ds = s->es3 || (ext && strstr(ext, "GL_OES_packed_depth_stencil"));
        gl_has_minmax = s->has_minmax;
        if (!s->has_ds) {
            warn_report("syborg-gl: no packed depth/stencil: no OpenVG fills on the host");
        }
    }
    s->msaa_applied = s->msaa;
    if (s->es3 && s->msaa) {
        GLint m = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &m);
        s->samples = MIN((GLint)s->msaa, m);
        if (s->samples < 2) {
            s->samples = 0;
        }
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
    {   /* OpenVG: positions in surface pixels (GL orientation), a premultiplied colour */
        static const char *vvs = "attribute vec2 aPos; uniform vec2 uSize;\n"
            "void main() { gl_Position = vec4(aPos / uSize * 2.0 - 1.0, 0.0, 1.0); }\n";
        /* paint: 0 colour; 1 image draw (mode 0 normal, 1 multiply, 2 stencil); 2 pattern (mode =
           tiling 0 fill, 1 pad, 2 repeat, 3 reflect). uM0..2: surface (pixel centres) -> image */
        static const char *vfs = "precision highp float;\n"
            "uniform vec4 uColor; uniform int uKind, uMode; uniform vec3 uM0, uM1, uM2;\n"
            "uniform vec2 uImgSize; uniform sampler2D uTex;\n"
            "uniform vec4 uG; uniform float uR; uniform int uSpread; uniform sampler2D uRamp;\n"
            "uniform int uCT; uniform vec4 uCTs, uCTb;\n"
            "vec4 ct(vec4 c) {\n"
            "  if (uCT == 0) return c;\n"
            "  vec4 u = c.a > 0.0 ? vec4(c.rgb / c.a, c.a) : vec4(0.0);\n"
            "  u = clamp(u * uCTs + uCTb, 0.0, 1.0); return vec4(u.rgb * u.a, u.a);\n"
            "}\n"
            "vec4 paint() {\n"
            "  if (uKind == 0) { return uColor; }\n"
            "  vec3 q = vec3(gl_FragCoord.xy, 1.0);\n"
            "  vec3 p = vec3(dot(uM0, q), dot(uM1, q), dot(uM2, q));\n"
            "  if (uKind >= 3) {\n"                 /* gradients, as the RI's PixelPipe */
            "    float g = 1.0;\n"
            "    if (uKind == 3) {\n"               /* uG = p0, p1 */
            "      vec2 u = uG.zw - uG.xy; float uu = dot(u, u);\n"
            "      if (uu > 0.0) g = dot(p.xy - uG.xy, u) / uu;\n"
            "    } else if (uR > 0.0) {\n"          /* uG = centre, focal point - centre (clamped) */
            "      vec2 fp = uG.zw, pp = p.xy - uG.xy, d = pp - fp;\n"
            "      float D = -1.0 / (dot(fp, fp) - uR * uR), cr = pp.x * fp.y - pp.y * fp.x;\n"
            "      g = (dot(fp, d) + sqrt(max(uR * uR * dot(d, d) - cr * cr, 0.0))) * D;\n"
            "    }\n"
            "    if (uSpread == 0) g = clamp(g, 0.0, 1.0);\n"
            "    else if (uSpread == 1) g = fract(g);\n"
            "    else { g = mod(g, 2.0); g = g < 1.0 ? g : 2.0 - g; }\n"
            "    return texture2D(uRamp, vec2((g * 1023.0 + 0.5) / 1024.0, 0.5));\n"
            "  }\n"
            "  vec2 t = p.xy / p.z / uImgSize;\n"
            "  bool outside = t.x < 0.0 || t.y < 0.0 || t.x > 1.0 || t.y > 1.0;\n"
            "  if (uKind == 1) {\n"
            "    if (outside) discard;\n"
            "    vec4 c = texture2D(uTex, t);\n"
            "    if (uMode == 1) c = c * uColor; else if (uMode == 2) c = uColor * c.a;\n"
            "    return c;\n"
            "  }\n"
            "  if (uMode == 0) { if (outside) { return uColor; } }\n"
            "  else if (uMode == 1) t = clamp(t, 0.0, 1.0);\n"
            "  else if (uMode == 2) t = fract(t);\n"
            "  else t = 1.0 - abs(mod(t, 2.0) - 1.0);\n"
            "  return texture2D(uTex, t);\n"
            "}\n"
            "void main() { gl_FragColor = ct(paint()); }\n";
        GLuint v = compile(GL_VERTEX_SHADER, vvs), f = compile(GL_FRAGMENT_SHADER, vfs);
        s->vgProg = glCreateProgram();
        glAttachShader(s->vgProg, v);
        glAttachShader(s->vgProg, f);
        glBindAttribLocation(s->vgProg, 0, "aPos");
        glLinkProgram(s->vgProg);
        s->vgSize = glGetUniformLocation(s->vgProg, "uSize");
        s->vgColor = glGetUniformLocation(s->vgProg, "uColor");
        s->vgKind = glGetUniformLocation(s->vgProg, "uKind");
        s->vgMode = glGetUniformLocation(s->vgProg, "uMode");
        s->vgM0 = glGetUniformLocation(s->vgProg, "uM0");
        s->vgM1 = glGetUniformLocation(s->vgProg, "uM1");
        s->vgM2 = glGetUniformLocation(s->vgProg, "uM2");
        s->vgImgSize = glGetUniformLocation(s->vgProg, "uImgSize");
        s->vgTex = glGetUniformLocation(s->vgProg, "uTex");
        s->vgG = glGetUniformLocation(s->vgProg, "uG");
        s->vgR = glGetUniformLocation(s->vgProg, "uR");
        s->vgSpread = glGetUniformLocation(s->vgProg, "uSpread");
        s->vgRamp = glGetUniformLocation(s->vgProg, "uRamp");
        s->vgCT = glGetUniformLocation(s->vgProg, "uCT");
        s->vgCTs = glGetUniformLocation(s->vgProg, "uCTs");
        s->vgCTb = glGetUniformLocation(s->vgProg, "uCTb");
        glGenTextures(1, &s->rampTex);
        glBindTexture(GL_TEXTURE_2D, s->rampTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, RAMP_N, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    {   /* composition: a layer's texture onto the screen, premultiplied; the screen keeps the frame
           buffer's byte order (B, G, R, A: little-endian 0xAARRGGBB words) */
        static const char *cvs = "attribute vec2 aPos; attribute vec2 aUV; uniform vec2 uSize; varying vec2 vUV;\n"
            "void main() { gl_Position = vec4(aPos / uSize * 2.0 - 1.0, 0.0, 1.0); vUV = aUV; }\n";
        /* uFmt: 0 XRGB, 1 ARGB, 2 ARGB PRE (guest words as bytes), 3 RGB 565, 4 host image (RGBA PRE),
           5 copy as is (surf_resample: a surface's pixels into its new sample buffers) */
        static const char *cfs = "precision mediump float;\n"
            "uniform sampler2D uTex; uniform int uFmt; uniform float uAlpha; varying vec2 vUV;\n"
            "void main() {\n"
            "  vec4 c = texture2D(uTex, vUV);\n"
            "  if (uFmt <= 2) c = c.bgra;\n"
            "  if (uFmt == 0 || uFmt == 3) c.a = 1.0; else if (uFmt == 1) c.rgb *= c.a;\n"
            "  gl_FragColor = uFmt == 5 ? c * uAlpha : (c * uAlpha).bgra;\n"
            "}\n";
        s->cpProg = glCreateProgram();
        glAttachShader(s->cpProg, compile(GL_VERTEX_SHADER, cvs));
        glAttachShader(s->cpProg, compile(GL_FRAGMENT_SHADER, cfs));
        glBindAttribLocation(s->cpProg, 0, "aPos");
        glBindAttribLocation(s->cpProg, 1, "aUV");
        glLinkProgram(s->cpProg);
        s->cpSize = glGetUniformLocation(s->cpProg, "uSize");
        s->cpTex = glGetUniformLocation(s->cpProg, "uTex");
        s->cpFmt = glGetUniformLocation(s->cpProg, "uFmt");
        s->cpAlpha = glGetUniformLocation(s->cpProg, "uAlpha");
    }
    info_report("syborg-gl: host GLES %d on %s (%s), OpenVG %dx MSAA", s->es3 ? 3 : 2,
                (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION), s->samples);
    s->ready = 1;
    /* not current between kicks: frame_timer may use it on the main loop */
    eglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return 1;
}

static inline uint64_t okey(uint32_t owner, uint32_t id)
{
    return (uint64_t)owner << 32 | id;
}

static void surf_resample(SyborgGlState *s, GlSurf *g);

static GlSurf *find_surf(SyborgGlState *s, uint32_t id)
{
    uint64_t k = okey(s->owner, id);
    GlSurf *g = id ? g_hash_table_lookup(s->surfs, &k) : NULL;
    if (g && g->want_msaa && g->samples != s->samples) {
        surf_resample(s, g);            /* the `msaa` setting changed since it was made */
    }
    return g;
}

/* the surface's GL objects (the table entry stays) */
static void surf_gl_free(GlSurf *g)
{
    glDeleteFramebuffers(1, &g->fbo);
    glDeleteTextures(1, &g->color);
    if (g->depth) {
        glDeleteRenderbuffers(1, &g->depth);
    }
    if (g->msfbo) {
        glDeleteFramebuffers(1, &g->msfbo);
        glDeleteRenderbuffers(1, &g->mscolor);
        glDeleteRenderbuffers(1, &g->msdepth);
    }
    g->fbo = g->color = g->depth = g->msfbo = g->mscolor = g->msdepth = 0;
    g->stencil = g->dirty = 0;
}

static void surf_free(SyborgGlState *s, GlSurf *g)
{
    surf_gl_free(g);
    g_hash_table_remove(s->surfs, &g->k);       /* frees g */
}

static GlTex *find_tex(SyborgGlState *s, uint32_t key)
{
    uint64_t k = okey(s->owner, key);
    return key ? g_hash_table_lookup(s->texs, &k) : NULL;
}

/* the owner's texture `key`, made (empty, no storage) if it does not exist */
static GlTex *get_tex(SyborgGlState *s, uint32_t key)
{
    GlTex *t = find_tex(s, key);
    if (t || !key) {
        return t;
    }
    if (g_hash_table_size(s->texs) >= MAX_TEX) {
        qemu_log_mask(LOG_GUEST_ERROR, "syborg-gl: too many textures\n");
        return NULL;
    }
    t = g_new0(GlTex, 1);
    t->owner = s->owner;
    t->key = key;
    t->k = okey(s->owner, key);
    glGenTextures(1, &t->tex);
    g_hash_table_insert(s->texs, &t->k, t);
    return t;
}

static void tex_free(SyborgGlState *s, GlTex *t)
{
    glDeleteTextures(1, &t->tex);
    g_hash_table_remove(s->texs, &t->k);        /* frees t */
}

static gboolean owner_surf_drop(gpointer k, gpointer v, gpointer owner)
{
    GlSurf *g = v;
    if (g->owner != *(uint32_t *)owner) {
        return FALSE;
    }
    surf_gl_free(g);
    return TRUE;
}

static gboolean owner_tex_drop(gpointer k, gpointer v, gpointer owner)
{
    GlTex *t = v;
    if (t->owner != *(uint32_t *)owner) {
        return FALSE;
    }
    glDeleteTextures(1, &t->tex);
    return TRUE;
}

/* a multisampled surface's samples -> its colour texture */
static void resolve(GlSurf *g)
{
    if (!g->msfbo || !g->dirty) {
        return;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g->msfbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->fbo);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, g->w, g->h, 0, 0, g->w, g->h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    g->dirty = 0;
}

/* the single-sampled pixels, for reading */
static void bind_read(GlSurf *g)
{
    resolve(g);
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glViewport(0, 0, g->w, g->h);
}

static void surf_make(SyborgGlState *s, uint32_t id, uint32_t w, uint32_t h, uint32_t depth)
{
    GlSurf *g = find_surf(s, id);
    GLint maxsz = 0;
    if (g && g->w == w && g->h == h) {
        return;
    }
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxsz);
    if (!id || !w || !h || w > (uint32_t)maxsz || h > (uint32_t)maxsz) {
        qemu_log_mask(LOG_GUEST_ERROR, "syborg-gl: surface %x %ux%u refused (max %d)\n", id, w, h, maxsz);
        if (g) {
            surf_free(s, g);
        }
        return;
    }
    if (g) {
        surf_gl_free(g);
    } else {
        if (g_hash_table_size(s->surfs) >= MAX_SURF) {
            qemu_log_mask(LOG_GUEST_ERROR, "syborg-gl: too many surfaces\n");
            return;
        }
        g = g_new0(GlSurf, 1);
        g->k = okey(s->owner, id);
        g_hash_table_insert(s->surfs, &g->k, g);
    }
    g->owner = s->owner; g->id = id; g->w = w; g->h = h;
    g->want_msaa = (depth & SYGL_SURF_MSAA) && (depth & SYGL_SURF_STENCIL);
    g->samples = 0;
    glGenTextures(1, &g->color);
    glBindTexture(GL_TEXTURE_2D, g->color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &g->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->color, 0);
    if ((depth & SYGL_SURF_MSAA) && (depth & SYGL_SURF_STENCIL) && s->samples) {
        /* anti-aliased OpenVG: colour and stencil samples; the texture gets the resolved pixels */
        glGenRenderbuffers(1, &g->mscolor);
        glBindRenderbuffer(GL_RENDERBUFFER, g->mscolor);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, s->samples, GL_RGBA8, w, h);
        glGenRenderbuffers(1, &g->msdepth);
        glBindRenderbuffer(GL_RENDERBUFFER, g->msdepth);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, s->samples, GL_DEPTH24_STENCIL8, w, h);
        glGenFramebuffers(1, &g->msfbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g->msfbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g->mscolor);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g->msdepth);
        g->stencil = 1;
        g->dirty = 1;
        g->samples = s->samples;
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            warn_report("syborg-gl: multisampled surface %ux%u incomplete: no anti-aliasing", w, h);
            glDeleteFramebuffers(1, &g->msfbo);
            glDeleteRenderbuffers(1, &g->mscolor);
            glDeleteRenderbuffers(1, &g->msdepth);
            g->msfbo = g->mscolor = g->msdepth = 0;
            g->stencil = 0;
            glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
            depth &= ~SYGL_SURF_MSAA;
        }
    }
    if (g->msfbo) {
        /* the single-sampled framebuffer has colour only */
    } else if (depth & SYGL_SURF_STENCIL) {       /* OpenVG fills: stencil (packed with a depth buffer) */
        glGenRenderbuffers(1, &g->depth);
        glBindRenderbuffer(GL_RENDERBUFFER, g->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, s->has_ds ? GL_DEPTH24_STENCIL8_OES : GL_DEPTH_COMPONENT16, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g->depth);
        if (s->has_ds) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g->depth);
        }
        g->stencil = s->has_ds;     /* no stencil: vg_fill skips the surface (logged at init) */
    } else if (depth) {
        glGenRenderbuffers(1, &g->depth);
        glBindRenderbuffer(GL_RENDERBUFFER, g->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g->depth);
    }
    /* a new EGL surface's content is undefined; the guest's software buffers start black */
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glViewport(0, 0, w, h);
    glScissor(0, 0, w, h);
    glColorMask(1, 1, 1, 1);
    glDepthMask(1);
    glClearColor(0, 0, 0, 0);
    glClearDepthf(1.0f);
    glStencilMask(0xff);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | (depth && !g->msfbo ? GL_DEPTH_BUFFER_BIT : 0) |
            (g->stencil && !g->msfbo ? GL_STENCIL_BUFFER_BIT : 0));
    if (g->msfbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, g->msfbo);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
}


static void tex_upload(SyborgGlState *s, const uint32_t *a, size_t words)
{
    uint32_t key = a[0], gen = a[1], w = a[2], h = a[3], y0 = a[4], rows = a[5];
    if (!w || !h || y0 + rows > h || (size_t)w * rows + 6 > words) {
        return;
    }
    GLint maxsz = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxsz);
    if (w > (uint32_t)maxsz || h > (uint32_t)maxsz) {
        qemu_log_mask(LOG_GUEST_ERROR, "syborg-gl: texture %ux%u refused (max %d)\n", w, h, maxsz);
        return;
    }
    GlTex *t = get_tex(s, key);
    if (!t) {
        return;
    }
    glBindTexture(GL_TEXTURE_2D, t->tex);
    if (y0 == 0) {      /* a new image: (re)define it */
        t->gen = gen; t->w = w; t->h = h;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, w, rows, GL_RGBA, GL_UNSIGNED_BYTE, a + 6);
}

/* bind for drawing: a multisampled surface's samples (then unresolved) */
static void bind_surf(GlSurf *g)
{
    glBindFramebuffer(GL_FRAMEBUFFER, g->msfbo ? g->msfbo : g->fbo);
    if (g->msfbo) {
        g->dirty = 1;
    }
    glViewport(0, 0, g->w, g->h);
}

/* guest memory: physical, or virtual in the address space of the kick (SYGL_REG_KICK) */
static int gmem_rw(SyborgGlState *s, uint32_t addr, void *p, size_t n, bool write)
{
    if (s->host_batch) {                /* a host batch: the only memory it touches is its output area */
        uint32_t off = addr - s->out;
        if (!write || !s->host_out || off > s->host_outlen || n > s->host_outlen - off) {
            return -1;
        }
        memcpy(s->host_out + off, p, n);
        return 0;
    }
    if (s->va) {
        return current_cpu ? cpu_memory_rw_debug(current_cpu, addr, p, n, write) : -1;
    }
    MemTxResult r = write ? address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, p, n)
                          : address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, p, n);
    return r == MEMTX_OK ? 0 : -1;
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
        return -1;          /* nothing written: the guest must not take the output */
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
    bind_read(g);
    glReadPixels(0, y0, g->w, rows, GL_RGBA, GL_UNSIGNED_BYTE, s->pix);
    uint32_t *w = (uint32_t *)s->pix;
    uint32_t amask = (a[4] & SYGL_READ_ALPHA) ? 0xff000000u : 0;    /* a[4]: 0 when not sent */
    for (size_t i = 0; i < bytes / 4; i++) {          /* R G B A bytes -> 0x00RRGGBB / 0xAARRGGBB words */
        const uint8_t *p = s->pix + i * 4;
        w[i] = ((uint32_t)p[3] << 24 & amask) | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    }
    return gmem_rw(s, s->out + a[1], s->pix, bytes, true) ? -1 : 0;
}

/* ---- OpenVG (src/khr; docs/devices/nokia-n8-rm596/vg-host.md) ---- */
static void vg_color(GLint u, uint32_t argb)
{
    glUniform4f(u, ((argb >> 16) & 255) / 255.0f, ((argb >> 8) & 255) / 255.0f, (argb & 255) / 255.0f,
                (argb >> 24) / 255.0f);
}

/* premultiplied colours; the VG blend modes GL's fixed blending can express (multiply, screen,
   darken and lighten are exact for an opaque destination) */
static void vg_blend(uint32_t mode)
{
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    switch (mode) {
    case 0x2000: glDisable(GL_BLEND); break;                                  /* VG_BLEND_SRC */
    case 0x2002: glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE); break;          /* DST_OVER */
    case 0x2003: glBlendFunc(GL_DST_ALPHA, GL_ZERO); break;                   /* SRC_IN */
    case 0x2004: glBlendFunc(GL_ZERO, GL_SRC_ALPHA); break;                   /* DST_IN */
    case 0x2005: glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA); break;    /* MULTIPLY */
    case 0x2006: glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR); break;          /* SCREEN */
    case 0x2007:                                                              /* DARKEN */
    case 0x2008:                                                              /* LIGHTEN */
        if (gl_has_minmax) {
            glBlendEquation(mode == 0x2007 ? GL_MIN_EXT : GL_MAX_EXT);
            glBlendFunc(GL_ONE, GL_ONE);
        } else {
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);      /* no min/max: SRC_OVER */
        }
        break;
    case 0x2009: glBlendFunc(GL_ONE, GL_ONE); break;                          /* ADDITIVE */
    default: glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;              /* SRC_OVER */
    }
}

static void vg_use(SyborgGlState *s, GlSurf *g)
{
    bind_surf(g);
    glUseProgram(s->vgProg);
    glUniform2f(s->vgSize, g->w, g->h);
    glUniform1i(s->vgKind, 0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
    glDisable(GL_DEPTH_TEST);
}

static void vg_restore(SyborgGlState *s)
{
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);       /* DARKEN/LIGHTEN set MIN/MAX: not into the GLES 1.1 path */
    glColorMask(1, 1, 1, 1);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glUseProgram(s->prog);
}

/*
 * an OpenVG surface made with another sample count than `msaa` now asks for: its pixels go into
 * new buffers (resolved, then drawn into the new multisampled buffer: blitting into one is not
 * allowed); the stencil is scratch between fills and starts clear
 */
static void surf_resample(SyborgGlState *s, GlSurf *g)
{
    int to = s->samples;
    resolve(g);                         /* g->color holds the pixels */
    if (g->msfbo) {
        glDeleteFramebuffers(1, &g->msfbo);
        glDeleteRenderbuffers(1, &g->mscolor);
        glDeleteRenderbuffers(1, &g->msdepth);
        g->msfbo = g->mscolor = g->msdepth = 0;
    }
    if (g->depth) {
        glDeleteRenderbuffers(1, &g->depth);
        g->depth = 0;
    }
    if (to) {
        glGenRenderbuffers(1, &g->mscolor);
        glBindRenderbuffer(GL_RENDERBUFFER, g->mscolor);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, to, GL_RGBA8, g->w, g->h);
        glGenRenderbuffers(1, &g->msdepth);
        glBindRenderbuffer(GL_RENDERBUFFER, g->msdepth);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, to, GL_DEPTH24_STENCIL8, g->w, g->h);
        glGenFramebuffers(1, &g->msfbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g->msfbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g->mscolor);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g->msdepth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glDeleteFramebuffers(1, &g->msfbo);
            glDeleteRenderbuffers(1, &g->mscolor);
            glDeleteRenderbuffers(1, &g->msdepth);
            g->msfbo = g->mscolor = g->msdepth = 0;
            to = 0;
        }
    }
    glViewport(0, 0, g->w, g->h);
    glScissor(0, 0, g->w, g->h);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glColorMask(1, 1, 1, 1);
    glStencilMask(0xff);
    glClearStencil(0);
    glClearDepthf(1.0f);
    if (to) {
        /* the pixels into the samples: a textured quad, no blending */
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glDisable(GL_BLEND);
        glUseProgram(s->cpProg);
        glUniform2f(s->cpSize, g->w, g->h);
        glUniform1i(s->cpTex, 0);
        glUniform1i(s->cpFmt, 5);
        glUniform1f(s->cpAlpha, 1.0f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g->color);
        float q[24] = { 0, 0, 0, 0,  g->w, 0, 1, 0,  g->w, g->h, 1, 1,
                        0, 0, 0, 0,  g->w, g->h, 1, 1,  0, g->h, 0, 1 };
        glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_STREAM_DRAW);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glDisableVertexAttribArray(2);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, 0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glEnableVertexAttribArray(2);
        glUseProgram(s->prog);
        g->stencil = 1;
        g->dirty = 0;
    } else {
        /* single-sampled: stencil (and depth) on the colour framebuffer */
        glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
        glGenRenderbuffers(1, &g->depth);
        glBindRenderbuffer(GL_RENDERBUFFER, g->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, s->has_ds ? GL_DEPTH24_STENCIL8_OES : GL_DEPTH_COMPONENT16,
                              g->w, g->h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g->depth);
        if (s->has_ds) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g->depth);
        }
        glClear(GL_DEPTH_BUFFER_BIT | (s->has_ds ? GL_STENCIL_BUFFER_BIT : 0));
        g->stencil = s->has_ds;
        g->dirty = 0;
    }
    g->samples = to;
}

static int vg_clear(SyborgGlState *s, const uint32_t *a, size_t n)
{
    GlSurf *g = find_surf(s, a[0]);
    uint32_t nr = a[2];
    if (n < 3 + (size_t)nr * 4) {
        return -1;
    }
    if (!g) {
        return 0;
    }
    bind_surf(g);
    glColorMask(1, 1, 1, 1);
    glClearColor(((a[1] >> 16) & 255) / 255.0f, ((a[1] >> 8) & 255) / 255.0f, (a[1] & 255) / 255.0f,
                 (a[1] >> 24) / 255.0f);
    if (!nr) {
        scissor(g, 0, 0, g->w, g->h);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    for (uint32_t k = 0; k < nr; k++) {
        const int32_t *r = (const int32_t *)a + 3 + k * 4;
        scissor(g, r[0], r[1], r[0] + r[2], r[1] + r[3]);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    return 0;
}

/* stencil, then cover: each edge adds a triangle (pivot, v0, v1) to the stencil, front faces
   incrementing and back faces decrementing (non-zero) or both inverting (even-odd); then the
   edges' bounding box is covered with the paint where the stencil is set, and the stencil cleared */
/* a gradient's colour ramp (the RI's colorRamp at zero filter size): stops interpolated
   premultiplied or not, as the paint says; the texture is premultiplied */
static void vg_ramp(SyborgGlState *s, int premul, uint32_t ns, const float *st)
{
    for (int i = 0; i < RAMP_N; i++) {
        float g = (float)i / (RAMP_N - 1), c[4];
        uint32_t k = 0;
        while (k + 1 < ns && g >= st[(k + 1) * 5]) {
            k++;
        }
        const float *a = st + k * 5, *b = st + MIN(k + 1, ns - 1) * 5;
        float t = (k + 1 < ns && b[0] > a[0]) ? (g - a[0]) / (b[0] - a[0]) : 0.0f;
        if (g <= st[0]) {
            t = 0.0f; b = a;
        }
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        for (int j = 0; j < 4; j++) {
            float ca = a[1 + j], cb = b[1 + j];
            if (premul && j < 3) {
                ca *= a[4]; cb *= b[4];
            }
            c[j] = ca + (cb - ca) * t;
        }
        if (!premul) {
            c[0] *= c[3]; c[1] *= c[3]; c[2] *= c[3];
        }
        for (int j = 0; j < 4; j++) {
            float v = c[j] < 0 ? 0 : c[j] > 1 ? 1 : c[j];
            s->ramp[i * 4 + j] = (uint8_t)(v * 255.0f + 0.5f);
        }
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s->rampTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, RAMP_N, 1, GL_RGBA, GL_UNSIGNED_BYTE, s->ramp);
    glActiveTexture(GL_TEXTURE0);
}

static int vg_fill(SyborgGlState *s, const uint32_t *a, size_t n)
{
    uint32_t rule = a[1], blend = a[2], kind = a[3], argb = a[4];
    if (n < 6) {
        return -1;
    }
    size_t x = a[5];                                 /* paint block length */
    if (n < 7 + x || (kind >= 1 && kind <= 2 && x < 14) || (kind >= 3 && x < 17)) {
        return -1;
    }
    const uint32_t *pb = a + 6;
    uint32_t ns = a[6 + x];
    if (n < 7 + x + (size_t)ns * 4 + 1) {
        return -1;
    }
    const int32_t *sc = (const int32_t *)a + 7 + x;
    uint32_t ne = a[7 + x + ns * 4];
    const float *e = (const float *)(a + 8 + x + ns * 4);
    if (n < 8 + x + (size_t)ns * 4 + (size_t)ne * 4) {
        return -1;
    }
    /* optional: colour transform (VG_COLOR_TRANSFORM): a word 1, then scale r g b a, bias r g b a */
    const uint32_t *ctw = a + 8 + x + ns * 4 + (size_t)ne * 4;
    int ct = n >= 8 + x + (size_t)ns * 4 + (size_t)ne * 4 + 9 && ctw[0] == 1;
    if (kind >= 3 && (pb[11] < 1 || 17 + (size_t)pb[16] * 5 > x)) {
        return -1;
    }
    GLuint texName = 0;
    if (kind == 1 || kind == 2) {   /* an image being rendered into (surface key|1) wins over its upload */
        GlSurf *rt = (pb[4] & SYGL_VG_TEXTURE_ONLY) ? NULL : find_surf(s, pb[0] | 1);
        GlTex *tex = rt ? NULL : find_tex(s, pb[0]);
        if (rt) {
            resolve(rt);
        }
        texName = rt ? rt->color : tex ? tex->tex : 0;
        if (!texName) {
            return 0;                                /* not uploaded */
        }
    }
    GlSurf *g = find_surf(s, a[0]);
    if (!g || !ne || !g->stencil) {
        return 0;
    }
    size_t need = (size_t)ne * 6 + 12;
    if (s->vgvcap < need) {
        s->vgv = g_realloc(s->vgv, need * sizeof(float));
        s->vgvcap = need;
    }
    float minx = e[0], maxx = e[0], miny = e[1], maxy = e[1];
    float px = e[0], py = e[1];                      /* the fan's pivot: any point works */
    float *v = s->vgv;
    for (uint32_t k = 0; k < ne; k++, e += 4) {
        v[0] = px; v[1] = py; v[2] = e[0]; v[3] = e[1]; v[4] = e[2]; v[5] = e[3];
        v += 6;
        minx = MIN(minx, MIN(e[0], e[2])); maxx = MAX(maxx, MAX(e[0], e[2]));
        miny = MIN(miny, MIN(e[1], e[3])); maxy = MAX(maxy, MAX(e[1], e[3]));
    }
    int bx0 = (int)floorf(minx), by0 = (int)floorf(miny), bx1 = (int)ceilf(maxx), by1 = (int)ceilf(maxy);
    vg_use(s, g);
    /* stencil pass */
    scissor(g, bx0, by0, bx1, by1);
    glColorMask(0, 0, 0, 0);
    glDisable(GL_BLEND);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xff);
    glStencilFunc(GL_ALWAYS, 0, 0xff);
    if (rule == 0) {
        glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
    } else {
        glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
        glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
    }
    glDisable(GL_CULL_FACE);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)ne * 6 * sizeof(float), s->vgv, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glDrawArrays(GL_TRIANGLES, 0, ne * 3);
    /* cover pass: where the stencil says inside (even-odd: odd count), resetting it */
    glColorMask(1, 1, 1, 1);
    vg_blend(blend);
    vg_color(s->vgColor, argb);
    if (kind >= 3) {
        const float *m = (const float *)pb, *gp = (const float *)(pb + 11);
        vg_ramp(s, pb[10] != 0, pb[16], (const float *)(pb + 17));
        glUniform1i(s->vgKind, kind);
        glUniform3f(s->vgM0, m[0], m[1], m[2]);
        glUniform3f(s->vgM1, m[3], m[4], m[5]);
        glUniform3f(s->vgM2, m[6], m[7], m[8]);
        glUniform1i(s->vgSpread, pb[9]);
        glUniform1i(s->vgRamp, 1);
        if (kind == 3) {
            glUniform4f(s->vgG, gp[0], gp[1], gp[2], gp[3]);
        } else {                                      /* focal point clamped inside the circle */
            float fx = gp[2] - gp[0], fy = gp[3] - gp[1], r = gp[4], l = sqrtf(fx * fx + fy * fy);
            if (l > 0.999f * r && l > 0) {
                fx *= 0.999f * r / l; fy *= 0.999f * r / l;
            }
            glUniform4f(s->vgG, gp[0], gp[1], fx, fy);
            glUniform1f(s->vgR, r);
        }
    } else if (kind) {
        const float *m = (const float *)(pb + 5);
        glUniform1i(s->vgKind, kind);
        glUniform1i(s->vgMode, pb[4] & 0xff);
        glUniform3f(s->vgM0, m[0], m[1], m[2]);
        glUniform3f(s->vgM1, m[3], m[4], m[5]);
        glUniform3f(s->vgM2, m[6], m[7], m[8]);
        glUniform2f(s->vgImgSize, pb[1], pb[2]);
        glUniform1i(s->vgTex, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texName);
        GLint f = s->filter == 1 ? GL_NEAREST : s->filter == 2 ? GL_LINEAR : pb[3] ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glUniform1i(s->vgCT, ct);
    if (ct) {
        const float *f = (const float *)(ctw + 1);
        glUniform4f(s->vgCTs, f[0], f[1], f[2], f[3]);
        glUniform4f(s->vgCTb, f[4], f[5], f[6], f[7]);
    }
    glStencilFunc(GL_NOTEQUAL, 0, rule == 0 ? 1 : 0xff);
    glStencilOp(GL_ZERO, GL_ZERO, GL_ZERO);
    float q[12] = { bx0, by0, bx1, by0, bx1, by1, bx0, by0, bx1, by1, bx0, by1 };
    glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    if (!ns) {
        glDrawArrays(GL_TRIANGLES, 0, 6);
    } else {
        for (uint32_t k = 0; k < ns; k++, sc += 4) {     /* one cover per scissor rectangle */
            scissor(g, MAX(bx0, sc[0]), MAX(by0, sc[1]), MIN(bx1, sc[0] + sc[2]), MIN(by1, sc[1] + sc[3]));
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
        /* stencil left set outside the scissor rectangles: clear the box */
        scissor(g, bx0, by0, bx1, by1);
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
    }
    vg_restore(s);
    s->draws++;
    return 0;
}

static void vg_surf2tex(SyborgGlState *s, const uint32_t *a)
{
    GlSurf *g = find_surf(s, a[0]);
    if (!g) {
        return;
    }
    GlTex *t = get_tex(s, a[1]);
    if (!t) {
        return;
    }
    bind_read(g);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, g->w, g->h, 0);
    t->w = g->w; t->h = g->h;
}

/* an image's pixels on the host back to the guest (CPU reads of images rendered into) */
static int vg_texread(SyborgGlState *s, const uint32_t *a)
{
    GlSurf *g = find_surf(s, a[0] | 1);
    GlTex *t = g ? NULL : find_tex(s, a[0]);
    uint32_t w = g ? g->w : t ? t->w : 0, h = g ? g->h : t ? t->h : 0, y0 = a[2], rows = a[3];
    if (!w || y0 + rows > h) {
        return -1;          /* nothing written: the guest must not take the output */
    }
    size_t bytes = (size_t)w * rows * 4;
    if ((uint64_t)a[1] + bytes > s->outlen) {
        return -1;
    }
    if (s->pixcap < bytes) {
        s->pix = g_realloc(s->pix, bytes);
        s->pixcap = bytes;
    }
    GLuint fbo = 0;
    if (g) {
        bind_read(g);
    } else {
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    }
    glReadPixels(0, y0, w, rows, GL_RGBA, GL_UNSIGNED_BYTE, s->pix);
    if (fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
    }
    return gmem_rw(s, s->out + a[1], s->pix, bytes, true) ? -1 : 0;
}

/* ---- composition (src/gcebe, docs/devices/nokia-n8-rm596/composition.md) ---- */
static GlScan *find_scan(SyborgGlState *s, const uint32_t *sid, int make)
{
    GlScan *free_ = NULL;
    for (int i = 0; i < MAX_SCAN; i++) {
        GlScan *c = &s->scan[i];
        if (c->used && !memcmp(c->sid, sid, 16)) {
            return c;
        }
        if (!c->used && !free_) {
            free_ = c;
        }
    }
    if (!make || !free_) {
        return NULL;
    }
    memset(free_, 0, sizeof(*free_));
    memcpy(free_->sid, sid, 16);
    free_->used = 1;
    glGenTextures(1, &free_->tex);
    glBindTexture(GL_TEXTURE_2D, free_->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return free_;
}

static void scan_free(GlScan *c)
{
    if (c && c->used) {
        glDeleteTextures(1, &c->tex);
        memset(c, 0, sizeof(*c));
    }
}

/* an EGL window's swap: its frame becomes the GCE surface's image (no read-back to the guest) */
/* Debugging: SYBORG_GL_DUMP=N:DIR writes every surface (resolved colour, RGBA, bottom row first as GL
 * reads it) to DIR/surf-<owner>-<id>-<w>x<h>.pam at the N-th VG present (from 1); N = 0: at every present,
 * overwriting (the last present's state stays). */
static void dump_surf(gpointer k, gpointer v, gpointer u)
{
    GlSurf *g = v;
    const char *dir = u;
    static unsigned present_no;
    (void)k;
    if (!g->fbo || !g->w || !g->h) {
        return;
    }
    bind_read(g);
    size_t n = (size_t)g->w * g->h * 4;
    uint8_t *px = g_malloc(n);
    glReadPixels(0, 0, g->w, g->h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    g_autofree char *path = g_strdup_printf("%s/surf-%08x-%08x-%ux%u.pam", dir, g->owner, g->id, g->w, g->h);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", g->w, g->h);
        fwrite(px, 1, n, f);
        fclose(f);
    }
    g_free(px);
    (void)present_no;
}

/* A guest upload of an image that is being rendered into (its target surface key|1 is live): the CPU changed
 * the image (the guest read the target's pixels back first, VG_TEXREAD, then wrote its copy and uploaded it),
 * so the new pixels go into the target, which every draw of the image samples (vg_fill: the target wins over
 * the upload). Without this, alf's render buffers kept their cleared pixels after vgImageSubData and the N8
 * drew a transparent screen once its effect path was on (EGL vendor "Broadcom"). The copy is the guest's own
 * copy-in at bind: a fill with the upload (texture only), source blend, over the whole image. */
static void tex_into_target(SyborgGlState *s, uint32_t key, uint32_t w, uint32_t h)
{
    if (!find_surf(s, key | 1) || !find_tex(s, key)) {
        return;
    }
    union { float f; uint32_t u; } one = { 1.0f }, zero = { 0.0f }, fw = { (float)w }, fh = { (float)h };
    uint32_t c[6 + 14 + 1 + 1 + 16] = {
        key | 1, 0, 0x2000 /* VG_BLEND_SRC */, 1 /* image draw */, 0xffffffffu, 14,
        key, w, h, 0 /* nearest */, SYGL_VG_TEXTURE_ONLY,
        one.u, zero.u, zero.u, zero.u, one.u, zero.u, zero.u, zero.u, one.u,
        0,                                       /* no scissoring */
        4,
        zero.u, zero.u, fw.u, zero.u,  fw.u, zero.u, fw.u, fh.u,
        fw.u, fh.u, zero.u, fh.u,      zero.u, fh.u, zero.u, zero.u,
    };
    vg_fill(s, c, G_N_ELEMENTS(c));
}

static void vg_dump_maybe(SyborgGlState *s)
{
    static unsigned presents, at;
    static char *dir;
    static bool init;
    if (!init) {
        const char *e = getenv("SYBORG_GL_DUMP");
        init = true;
        if (e && strchr(e, ':')) {
            at = strtoul(e, NULL, 10);
            dir = g_strdup(strchr(e, ':') + 1);
        }
    }
    if (!dir || (++presents != at && at)) {
        return;
    }
    g_mkdir_with_parents(dir, 0755);
    g_hash_table_foreach(s->surfs, dump_surf, dir);
    if (at) {
        warn_report("syborg-gl: present %u: surfaces dumped to %s", presents, dir);
    }
}

static void vg_present(SyborgGlState *s, const uint32_t *a)
{
    GlSurf *g = find_surf(s, a[0]);
    GlScan *c = g ? find_scan(s, a + 1, 1) : NULL;
    if (!c) {
        return;
    }
    bind_read(g);
    glBindTexture(GL_TEXTURE_2D, c->tex);
    if (c->w != g->w || c->h != g->h || !c->host) {
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, g->w, g->h, 0);
    } else {
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, g->w, g->h);
    }
    c->w = g->w; c->h = g->h; c->host = 1; c->fmt = 4;
    vg_dump_maybe(s);
}

/* a layer's image from guest memory (rows top-down, the guest's pixel format) */
static int scan_upload(SyborgGlState *s, GlScan *c, const SyglLayer *l)
{
    uint32_t bpp = l->fmt == 3 ? 2 : 4;
    if (!l->va || !l->w || !l->h || l->w > 4096 || l->h > 4096 || l->stride < l->w * bpp) {
        return -1;
    }
    size_t bytes = (size_t)l->stride * (l->h - 1) + l->w * bpp;
    if (s->cpixcap < bytes) {
        s->cpix = g_realloc(s->cpix, bytes);
        s->cpixcap = bytes;
    }
    if (gmem_rw(s, l->va, s->cpix, bytes, false)) {
        return -1;
    }
    glBindTexture(GL_TEXTURE_2D, c->tex);
    GLenum fmt = bpp == 2 ? GL_RGB : GL_RGBA, type = bpp == 2 ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE;
    if (l->stride % bpp == 0 && s->es3) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, l->stride / bpp);
        glTexImage2D(GL_TEXTURE_2D, 0, fmt, l->w, l->h, 0, fmt, type, s->cpix);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, fmt, l->w, l->h, 0, fmt, type, NULL);
        for (uint32_t y = 0; y < l->h; y++) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, l->w, 1, fmt, type, s->cpix + (size_t)y * l->stride);
        }
    }
    c->w = l->w; c->h = l->h; c->fmt = l->fmt; c->host = 0;
    return 0;
}

/* the pending frame (PBO) into guest RAM through the pages translated at compose; under the
   frame seqlock. Context current, BQL held (vCPU thread or main loop). */
#define FRAME_FILE_PIXELS (2048u * 2048u * 4u)     /* the frame file's pixel area (byte 4096 on) */
static SyborgGlState *gl_dev;           /* the machine's one device (also for syvg's host batches) */

/* a composed frame becomes the screen: kept on the host (the display and the frame file show it); the
   guest's display memory is not written (it is the screen surface DSA apps draw into) */
static void frame_store(SyborgGlState *s, const uint8_t *px, uint32_t W, uint32_t H)
{
    size_t bytes = (size_t)W * H * 4;
    if (s->hfCap < bytes) {
        s->hframe = g_realloc(s->hframe, bytes);
        s->hfCap = bytes;
    }
    if (s->frame_seq) {
        qatomic_set(&s->frame_seq[1], s->frame_seq[1] + 1);
        smp_wmb();
    }
    memcpy(s->hframe, px, bytes);
    s->hfW = W;
    s->hfH = H;
    s->hfOn = true;
    if (s->frame_seq) {
        if (bytes <= FRAME_FILE_PIXELS) {
            memcpy((uint8_t *)s->frame_seq + 4096, px, bytes);
            s->frame_seq[2] = W;
            s->frame_seq[3] = H;
            s->frame_seq[4] = 1;
        }
        smp_wmb();
        qatomic_set(&s->frame_seq[1], s->frame_seq[1] + 1);
    }
}

bool syborg_gl_frame(uint32_t *w, uint32_t *h, const uint8_t **px)
{
    SyborgGlState *s = gl_dev;
    if (!s || !s->hfOn || !s->hframe) {
        return false;
    }
    *w = s->hfW;
    *h = s->hfH;
    *px = s->hframe;
    return true;
}

static void frame_write_pending(SyborgGlState *s, bool wait)
{
    if (!s->pending) {
        return;
    }
    GLenum st = glClientWaitSync(s->fence, wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0,
                                 wait ? 1000000000ull : 0);
    if (st == GL_TIMEOUT_EXPIRED && !wait) {
        timer_mod(s->rb_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1);
        return;
    }
    if (wait) {
        s->st_async_wait++;
    }
    glDeleteSync(s->fence);
    s->fence = NULL;
    s->pending = 0;
    size_t bytes = (size_t)s->pW * s->pH * 4;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s->pbo);
    const uint8_t *px = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
    if (px) {
        frame_store(s, px, s->pW, s->pH);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

/* main loop: the GPU may be done with the pending frame */
static void frame_timer(void *opaque)
{
    SyborgGlState *s = opaque;
    if (!s->pending || s->ready <= 0) {
        return;
    }
    eglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s->ctx);
    frame_write_pending(s, false);
    eglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

/* the screen FBO (bound) into the PBO; the frame reaches guest RAM later (frame_timer) */
static int compose_async(SyborgGlState *s, uint32_t fb, uint32_t W, uint32_t H, uint32_t stride)
{
    frame_write_pending(s, true);       /* the previous frame first (normally long done) */
    size_t bytes = (size_t)W * H * 4;
    vaddr end = (vaddr)fb + (vaddr)(H - 1) * stride + W * 4;
    vaddr pg0 = fb & ~(vaddr)0xfff;
    size_t np = (end - pg0 + 4095) >> 12;
    if (s->pPagesCap < np) {
        s->pPages = g_renew(hwaddr, s->pPages, np);
        s->pPagesCap = np;
    }
    for (size_t i = 0; i < np; i++) {   /* translate now: the guest's page tables are current */
        TranslateForDebugResult tr;
        s->pPages[i] = cpu_translate_for_debug(current_cpu, pg0 + i * 4096, &tr) ?
                       (tr.physaddr & ~(hwaddr)0xfff) : (hwaddr)-1;
    }
    s->pNPages = np;
    s->pBase = fb; s->pW = W; s->pH = H; s->pStride = stride;
    if (!s->pbo) {
        glGenBuffers(1, &s->pbo);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s->pbo);
    GLint have = 0;
    glGetBufferParameteriv(GL_PIXEL_PACK_BUFFER, GL_BUFFER_SIZE, &have);
    if ((size_t)have < bytes) {
        glBufferData(GL_PIXEL_PACK_BUFFER, bytes, NULL, GL_STREAM_READ);
    }
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    s->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    s->pending = 1;
    s->st_async++;
    s->st_compose++;
    timer_mod(s->rb_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1);
    return 0;
}

static int compose(SyborgGlState *s, const uint32_t *a, size_t n)
{
    uint32_t fb = a[0], W = a[1], H = a[2], stride = a[3], nl = a[4] & 0xffff;
    uint32_t rot = (a[4] >> 24) & 3;    /* the screen's rotation: layers are in rotated coordinates */
    if (n < 5 + (size_t)nl * SYGL_LAYER_WORDS || !W || !H || W > 4096 || H > 4096 || stride < W * 4) {
        return -1;
    }
    if (s->scrW != W || s->scrH != H) {
        if (!s->scrFbo) {
            glGenFramebuffers(1, &s->scrFbo);
            glGenTextures(1, &s->scrTex);
        }
        glBindTexture(GL_TEXTURE_2D, s->scrTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, s->scrFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->scrTex, 0);
        s->scrW = W; s->scrH = H;
    }
    /* screen rows top-down: GL row y = screen row y */
    glBindFramebuffer(GL_FRAMEBUFFER, s->scrFbo);
    glViewport(0, 0, W, H);
    glScissor(0, 0, W, H);
    glColorMask(1, 1, 1, 1);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(s->cpProg);
    glUniform2f(s->cpSize, W, H);
    glUniform1i(s->cpTex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    for (uint32_t i = 0; i < nl; i++) {
        SyglLayer lc = *(const SyglLayer *)(a + 5 + i * SYGL_LAYER_WORDS);
        uint32_t lrot = (lc.fmt >> 8) & 3;    /* the layer's source rotation (anticlockwise, OpenWF-C) */
        lc.fmt &= 0xff;
        const SyglLayer *l = &lc;
        GlScan *c = find_scan(s, l->sid, 1);
        if (!c) {
            continue;
        }
        if (!c->host && (l->dirty || c->w != l->w || c->h != l->h || c->fmt != l->fmt) && scan_upload(s, c, l)) {
            continue;
        }
        if (!c->w || !c->h || l->dx1 <= l->dx0 || l->dy1 <= l->dy0 || !l->alpha) {
            continue;
        }
        if (getenv("SYBORG_GL_DUMP_LAYERS") && s->st_compose == 600) {   /* debugging: the layers of one
                                                                          composition (the 600th), as PPM */
            {
                GLuint f;
                uint8_t *px = g_malloc((size_t)c->w * c->h * 4);
                glGenFramebuffers(1, &f);
                glBindFramebuffer(GL_FRAMEBUFFER, f);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->tex, 0);
                glReadPixels(0, 0, c->w, c->h, GL_RGBA, GL_UNSIGNED_BYTE, px);
                char fn[256];
                snprintf(fn, sizeof(fn), "%s/layer-%08x-%ux%u-%s.ppm", getenv("SYBORG_GL_DUMP_LAYERS"),
                         l->sid[0], c->w, c->h, c->host ? "host" : "guest");
                FILE *o = fopen(fn, "wb");
                if (o) {
                    fprintf(o, "P6 %u %u 255\n", c->w, c->h);
                    for (uint32_t y = 0; y < c->h; y++) {
                        for (uint32_t x = 0; x < c->w; x++) {
                            fwrite(px + ((size_t)y * c->w + x) * 4, 1, 3, o);
                        }
                    }
                    fclose(o);
                }
                g_free(px);
                glDeleteFramebuffers(1, &f);
                glBindFramebuffer(GL_FRAMEBUFFER, s->scrFbo);
            }
        }
        float sx0 = l->sx0, sy0 = l->sy0, sx1 = l->sx1, sy1 = l->sy1;
        if (sx1 <= sx0 || sy1 <= sy0) {
            sx0 = sy0 = 0; sx1 = c->w; sy1 = c->h;
        }
        float u0 = sx0 / c->w, u1 = sx1 / c->w, v0 = sy0 / c->h, v1 = sy1 / c->h;
        if (c->host) {                                /* GL rows bottom-up */
            v0 = 1.0f - v0; v1 = 1.0f - v1;
        }
        int scaled = (l->dx1 - l->dx0) != (int)(sx1 - sx0) || (l->dy1 - l->dy0) != (int)(sy1 - sy0);
        glBindTexture(GL_TEXTURE_2D, c->tex);
        GLint cf = s->filter == 1 ? GL_NEAREST : (scaled || s->filter == 2) ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, cf);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, cf);
        glUniform1i(s->cpFmt, c->host ? 4 : c->fmt);
        glUniform1f(s->cpAlpha, MIN(l->alpha, 255u) / 255.0f);
        float x0 = l->dx0, y0 = l->dy0, x1 = l->dx1, y1 = l->dy1;
        /* the source's corners for the destination's TL, TR, BR, BL: turned anticlockwise by lrot */
        float cu[4] = { u0, u1, u1, u0 }, cv[4] = { v0, v0, v1, v1 };
        float tu[4], tv[4];
        for (int k = 0; k < 4; k++) {
            tu[k] = cu[(k + lrot) & 3];
            tv[k] = cv[(k + lrot) & 3];
        }
        float q[24] = { x0, y0, tu[0], tv[0],  x1, y0, tu[1], tv[1],  x1, y1, tu[2], tv[2],
                        x0, y0, tu[0], tv[0],  x1, y1, tu[2], tv[2],  x0, y1, tu[3], tv[3] };
        for (int k = 0; k < 24 && rot; k += 4) {     /* rotated screen -> the panel */
            float lx = q[k], ly = q[k + 1];
            q[k] = rot == 1 ? W - ly : rot == 2 ? W - lx : ly;
            q[k + 1] = rot == 1 ? lx : rot == 2 ? H - ly : H - lx;
        }
        glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_STREAM_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, 0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glDisable(GL_BLEND);
    glEnableVertexAttribArray(2);
    glUseProgram(s->prog);
    /* the finished screen into the frame buffer */
    size_t bytes = (size_t)W * H * 4;
    if (s->es3 && current_cpu && !getenv("SYBORG_GL_SYNC_READBACK")) {
        return compose_async(s, fb, W, H, stride);
    }
    if (s->pixcap < bytes) {
        s->pix = g_realloc(s->pix, bytes);
        s->pixcap = bytes;
    }
    int64_t tr = get_clock();
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, s->pix);
    s->st_read_ns += get_clock() - tr;
    s->st_compose++;
    frame_store(s, s->pix, W, H);
    (void)fb;
    (void)stride;
    return 0;
}

static uint32_t run_batch(SyborgGlState *s);
static uint32_t exec_cmds(SyborgGlState *s, size_t total);

/* one kick, timed: the vCPU waits for all of it (the guest does not run meanwhile) */
static uint32_t run(SyborgGlState *s)
{
    int64_t t0 = get_clock();
    uint32_t r = run_batch(s);
    if (s->ready > 0) {                 /* free for frame_timer on the main loop */
        eglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    int64_t now = get_clock();
    s->st_kicks++;
    s->st_kick_ns += now - t0;
    if (now - (int64_t)s->st_last_print > 5000000000LL && getenv("SYBORG_GL_STATS")) {
        s->st_last_print = now;
        fprintf(stderr, "syborg-gl: kicks %" PRIu64 " in %.1f ms, composes %" PRIu64 " in %.1f ms "
                "(sync read-back %.1f ms, async %" PRIu64 ", of which waited %" PRIu64 "), fills %" PRIu64 "\n",
                s->st_kicks, s->st_kick_ns / 1e6, s->st_compose, s->st_compose_ns / 1e6,
                s->st_read_ns / 1e6, s->st_async, s->st_async_wait, s->st_fills);
    }
    return r;
}

#include "syborg_gl2.h"             /* OpenGL ES 2.0 replay (gles2-host.md) */
#include "syborg_gl1.h"             /* OpenGL ES 1.1: the matrix palette, queries (gles-host.md) */
#include "syborg_gles.h"            /* OpenGL ES 1.1 and 2.0 replay: the guest only forwards (gles-host.md) */

#include "syborg_gl_capture.h"

/* the commands in s->cmd[0..total) (a guest batch, or a host one from syvg: syborg_gl_exec_host) */
static uint32_t exec_cmds(SyborgGlState *s, size_t total)
{
    size_t i = 0;
    while (i + 2 <= total) {
        uint32_t op = s->cmd[i], words = s->cmd[i + 1];
        if (words < 2 || i + words > total) {
            return i + 1;
        }
        gl_capture_record(s, s->cmd + i, words);
        const uint32_t *a = s->cmd + i + 2;
        size_t n = words - 2;
        if (op < SYGL_GL2 || op > SYGL_GL_TEXIMAGE) {
            gl2_main(s);                /* after OpenGL ES calls: the device's context again */
        }
        switch (op) {
        case SYGL_GL2: if (gles_call(s, 2, a, n)) return i + 1; break;
        case SYGL_GL1: if (gles_call(s, 1, a, n)) return i + 1; break;
        case SYGL_GL_TEXIMAGE: gl2_tex_from_surf(s, a, n); break;
        case SYGL_GL2_NEW: if (n >= 2) gl2_new(s, a[0], a[1], n >= 3 ? a[2] : 2); break;
        case SYGL_GL2_CURRENT: if (n >= 4) {
            Gl2HostCtx *c2 = gl2_find(s, a[0]);
            if (c2) gl2_current_surf(s, c2, a[1], a[2], a[3]);
            break;
        }
        case SYGL_GL2_FREE: if (n >= 1) {
            Gl2HostCtx *c2 = gl2_find(s, a[0]);
            if (c2) gl2_destroy(s, c2);
            break;
        }
        case SYGL_SURF: if (n >= 4) surf_make(s, a[0], a[1], a[2], a[3]); break;
        case SYGL_SURF_FREE: { GlSurf *g = find_surf(s, a[0]); if (g) surf_free(s, g); break; }
        case SYGL_TEX:
            tex_upload(s, a, n);
            if (n >= 6 && a[4] + a[5] == a[3]) {
                tex_into_target(s, a[0], a[2], a[3]);
            }
            break;
        case SYGL_TEX_FREE: {
            GlTex *t = find_tex(s, a[0]);
            if (t) { tex_free(s, t); }
            break;
        }
        case SYGL_CLEAR: if (n >= 8) gl_clear(s, a); break;
        case SYGL_DRAW: gl_draw(s, a, n); break;
        case SYGL_READ: {
            uint32_t r5[5] = { 0, 0, 0, 0, 0 };          /* the optional 5th word (flags) */
            memcpy(r5, a, MIN(n, 5) * 4);
            if (n >= 4 && readback(s, r5)) return i + 1;
            break;
        }
        case SYGL_VG_CLEAR: if (n >= 3 && vg_clear(s, a, n)) return i + 1; break;
        case SYGL_VG_FILL: s->st_fills++; if (n < 6 || vg_fill(s, a, n)) return i + 1; break;
        case SYGL_VG_SURF2TEX: if (n >= 2) vg_surf2tex(s, a); break;
        case SYGL_VG_PRESENT:           /* "compose" = guest: refused, eglos copies the frame */
            if (!s->compose_host) return i + 1;
            if (n >= 5) vg_present(s, a);
            gl2_frame_end();                /* SYBORG_GL2_TRACE */
            break;
        case SYGL_COMPOSE: {            /* "compose" = guest: refused, gcebe copies the layers */
            if (!s->compose_host) return i + 1;
            int64_t tc = get_clock();
            int bad = n < 5 || compose(s, a, n);
            s->st_compose_ns += get_clock() - tc;
            if (bad) return i + 1;
            break;
        }
        case SYGL_SCANOUT_FREE: if (n >= 4) scan_free(find_scan(s, a, 0)); break;
        case SYGL_VG_TEXREAD: if (n < 4 || vg_texread(s, a)) return i + 1; break;
        case SYGL_OWNER: if (n >= 1) s->owner = a[0]; break;
        case SYGL_VG2:
        case SYGL_EGL: {                /* the host EGL/OpenVG (syvg): only in a guest thread's own kick */
            uint32_t res[257];
            size_t ow = MIN(s->outlen / 4, (uint32_t)G_N_ELEMENTS(res));
            if (s->host_batch || !s->va || ow < 1) return i + 1;
            int e = op == SYGL_VG2 ? syvg_vg_call(s->owner, a, n, res, ow) : syvg_egl_call(s->owner, a, n, res, ow);
            if (e || gmem_rw(s, s->out, res, ow * 4, true)) return i + 1;
            break;
        }
        case SYGL_OWNER_FREE:           /* eglTerminate, or the owner's last channel closed */
            if (n >= 1) {
                uint32_t o = a[0];
                g_hash_table_foreach_remove(s->surfs, owner_surf_drop, &o);
                g_hash_table_foreach_remove(s->texs, owner_tex_drop, &o);
                gl2_owner_free(s, o);
                syvg_owner_free(o);
            }
            break;
        default: return i + 1;
        }
        i += words;
    }
    gl2_main(s);                        /* the errors below are the device's, not an app's */
    for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError()) {
        if (s->glerrors++ < 8) {   /* black frames otherwise give no clue */
            warn_report("syborg-gl: GL error 0x%x after a batch of %zu words", e, total);
        }
    }
    return 0;
}

/* syvg (the host OpenVG/EGL, src/syvg): run commands built in the host, results (SYGL_READ, SYGL_VG_TEXREAD,
   ...) at byte offsets into `out`. Called from within a guest batch (a VG call's dispatch): the batch state
   is saved and restored around it. */
static SyborgGlState *gl_dev;

/* syvg reads a call's pointer arguments and writes its outputs in the calling guest thread's memory
   (during that thread's kick: SYGL_REG_KICK, s->va) */
int syvg_guest_read(uint32_t va, void *dst, size_t n)
{
    SyborgGlState *s = gl_dev;
    if (!s || !s->va || s->host_batch || !current_cpu) {
        return -1;
    }
    return cpu_memory_rw_debug(current_cpu, va, dst, n, false) ? -1 : 0;
}

int syvg_guest_write(uint32_t va, const void *src, size_t n)
{
    SyborgGlState *s = gl_dev;
    if (!s || !s->va || s->host_batch || !current_cpu) {
        return -1;
    }
    return cpu_memory_rw_debug(current_cpu, va, (void *)src, n, true) ? -1 : 0;
}

uint32_t syborg_gl_exec_host(const uint32_t *cmd, size_t words, void *out, size_t outlen)
{
    SyborgGlState *s = gl_dev;
    if (!s || !gl_init(s)) {
        return 1;
    }
    uint32_t *scmd = s->cmd, so = s->out, sol = s->outlen;
    size_t scap = s->cmdcap;
    uint8_t *sho = s->host_out;
    size_t shl = s->host_outlen;
    bool shb = s->host_batch;
    s->host_batch = true;
    s->cmd = (uint32_t *)cmd;
    s->cmdcap = words * 4;
    s->out = 0;
    s->outlen = out ? (uint32_t)outlen : 0;   /* the read-backs check their size against it */
    s->host_out = out;
    s->host_outlen = out ? outlen : 0;
    eglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s->ctx);
    uint32_t r = exec_cmds(s, words);
    s->cmd = scmd; s->cmdcap = scap; s->out = so; s->outlen = sol;
    s->host_out = sho; s->host_outlen = shl; s->host_batch = shb;
    return r;
}

static uint32_t run_batch(SyborgGlState *s)
{
    if (!gl_init(s) || s->len < 8 || s->len > 64 * 1024 * 1024) {
        return 1;
    }
    if (s->cmdcap < s->len) {
        s->cmd = g_realloc(s->cmd, s->len);
        s->cmdcap = s->len;
    }
    if (gmem_rw(s, s->buf, s->cmd, s->len, false)) {
        return ~0u;
    }
    eglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s->ctx);
    if (s->msaa != s->msaa_applied) {   /* `msaa` set at run time: surfaces follow lazily */
        GLint m = 0;
        if (s->es3) {
            glGetIntegerv(GL_MAX_SAMPLES, &m);
        }
        s->samples = MIN((GLint)s->msaa, m);
        if (s->samples < 2) {
            s->samples = 0;
        }
        s->msaa_applied = s->msaa;
        info_report("syborg-gl: OpenVG %dx MSAA", s->samples);
    }
    return exec_cmds(s, s->len / 4);
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
    case SYGL_REG_KICK: return s->result;
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
    case SYGL_REG_KICK: {
        /* a SyglKick at a virtual address of the writer: its buffers are used in place */
        SyglKick k;
        CPUState *cs = current_cpu;
        if (!cs || (value & 3) || cpu_memory_rw_debug(cs, value, &k, sizeof(k), false)) {
            s->result = ~0u;
            break;
        }
        uint32_t sb = s->buf, sl = s->len, so = s->out, sol = s->outlen;
        s->buf = k.buf; s->len = k.len; s->out = k.out; s->outlen = k.outlen;
        s->va = 1;
        s->result = run(s);
        s->va = 0;
        s->buf = sb; s->len = sl; s->out = so; s->outlen = sol;
        cpu_memory_rw_debug(cs, value + offsetof(SyglKick, result), &s->result, 4, true);
        break;
    }
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
    gl_dev = SYBORG_GL(dev);           /* for syvg's host batches (one device per machine) */
    SyborgGlState *s = SYBORG_GL(dev);
    if (s->frame_file && *s->frame_file) {
        int fd = open(s->frame_file, O_RDWR | O_CREAT | O_TRUNC, 0644);
        void *m = MAP_FAILED;
        if (fd >= 0 && ftruncate(fd, 4096 + FRAME_FILE_PIXELS) == 0) {
            m = mmap(NULL, 4096 + FRAME_FILE_PIXELS, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        }
        if (fd >= 0) {
            close(fd);
        }
        if (m == MAP_FAILED) {
            warn_report("syborg-gl: frame-file %s: %s", s->frame_file, strerror(errno));
        } else {
            s->frame_seq = m;
            s->frame_seq[0] = 0x51455346;   /* "FSEQ" */
        }
    }
    gles_ident_load(s->identity);
    syvg_egl_load_configs(s->egl_configs);
    s->rb_timer = timer_new_ms(QEMU_CLOCK_REALTIME, frame_timer, s);
    s->surfs = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, g_free);
    s->texs = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, g_free);
    memory_region_init_io(&s->iomem, OBJECT(s), &syborg_gl_ops, s, "syborg-gl", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const Property syborg_gl_properties[] = {
    DEFINE_PROP_STRING("backend", SyborgGlState, backend),
    DEFINE_PROP_STRING("frame-file", SyborgGlState, frame_file),
    DEFINE_PROP_STRING("identity", SyborgGlState, identity),
    DEFINE_PROP_STRING("egl-configs", SyborgGlState, egl_configs),
};

/*
 * Runtime settings (QOM properties: -global syborg-gl.NAME=V at start, qom-set /machine/gl NAME V
 * at any time; docs/gpu-host-review.md section 3). Setters only record the value: the device
 * applies it on the vCPU thread at the next batch (GL is never called from here).
 */
static void gl_get_msaa(Object *o, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint32_t x = SYBORG_GL(o)->msaa;
    visit_type_uint32(v, name, &x, errp);
}

static void gl_set_msaa(Object *o, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint32_t x;
    if (!visit_type_uint32(v, name, &x, errp)) {
        return;
    }
    if (x != 0 && x != 2 && x != 4 && x != 8 && x != 16) {
        error_setg(errp, "msaa: 0, 2, 4, 8 or 16");
        return;
    }
    SYBORG_GL(o)->msaa = x;
}

static const char *const filter_names[] = { "auto", "nearest", "linear", NULL };

static int gl_get_filter(Object *o, Error **errp)
{
    return SYBORG_GL(o)->filter;
}

static void gl_set_filter(Object *o, int v, Error **errp)
{
    SYBORG_GL(o)->filter = v;
}

static const QEnumLookup filter_lookup = { .array = filter_names, .size = 3 };
static const char *const compose_names[] = { "host", "guest", NULL };
static const QEnumLookup compose_lookup = { .array = compose_names, .size = 2 };

static int gl_get_compose(Object *o, Error **errp)
{
    return SYBORG_GL(o)->compose_host ? 0 : 1;
}

static void gl_set_compose(Object *o, int v, Error **errp)
{
    SYBORG_GL(o)->compose_host = v == 0;
}

static char *gl_get_stats(Object *o, Error **errp)
{
    SyborgGlState *s = SYBORG_GL(o);
    return g_strdup_printf("{\"ready\": %d, \"msaa\": %u, \"samples\": %d, \"kicks\": %" PRIu64
                           ", \"kick_ms\": %.1f, \"composes\": %" PRIu64 ", \"compose_ms\": %.1f, "
                           "\"async_readbacks\": %" PRIu64 ", \"waited\": %" PRIu64 ", \"fills\": %" PRIu64
                           ", \"surfaces\": %u, \"textures\": %u, \"gl_errors\": %" PRIu64 "}",
                           s->ready, s->msaa, s->samples, s->st_kicks, s->st_kick_ns / 1e6, s->st_compose,
                           s->st_compose_ns / 1e6, s->st_async, s->st_async_wait, s->st_fills,
                           s->surfs ? g_hash_table_size(s->surfs) : 0,
                           s->texs ? g_hash_table_size(s->texs) : 0, s->glerrors);
}

static void syborg_gl_instance_init(Object *o)
{
    SyborgGlState *s = SYBORG_GL(o);
    s->msaa = 8;
    s->compose_host = true;
    object_property_add(o, "msaa", "uint32", gl_get_msaa, gl_set_msaa, NULL, NULL);
    object_property_set_description(o, "msaa", "OpenVG samples: 0 (off), 2, 4, 8 (default), 16; runtime");
    object_property_add_enum(o, "filter", "SyborgGlFilter", &filter_lookup, gl_get_filter, gl_set_filter);
    object_property_set_description(o, "filter", "image sampling: auto (VG image quality), nearest, linear; runtime");
    object_property_add_enum(o, "compose", "SyborgGlCompose", &compose_lookup, gl_get_compose, gl_set_compose);
    object_property_set_description(o, "compose", "host: composition and presentation on the GPU; guest: the guest copies (fallback); runtime");
    object_property_add_str(o, "capture", gl_capture_status, gl_capture_start);
    object_property_add_str(o, "stats", gl_get_stats, NULL);
    object_property_set_description(o, "stats", "read-only JSON: kicks, timings, objects, errors");
}

static gboolean reset_surf_drop(gpointer k, gpointer v, gpointer u)
{
    surf_gl_free(v);
    return TRUE;
}

static gboolean reset_tex_drop(gpointer k, gpointer v, gpointer u)
{
    glDeleteTextures(1, &((GlTex *)v)->tex);
    return TRUE;
}

/*
 * A machine reset (sydb's Reset, a guest reboot, the modem's power cycle): the guest's processes are gone and
 * the next boot's processes get the same ids (owner = process id), so nothing of theirs may survive: syvg's
 * EGL/OpenVG state first (it frees its own GPU objects through host batches), then every surface, texture,
 * GCE scan-out image and GLES 2 context left, and a frame read-back still pending (it would write the old
 * frame into the new boot's RAM). The device's own GL set-up (context, programs, screen FBO) stays: it is
 * the host's, not the guest's. Runs on the main loop with the vCPUs stopped; the GL context is free then
 * (run() and frame_timer release it).
 */
static void syborg_gl_reset(DeviceState *dev)
{
    SyborgGlState *s = SYBORG_GL(dev);
    timer_del(s->rb_timer);
    if (s->ready > 0) {
        syvg_reset();
        eglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s->ctx);
        gl2_owner_free(s, ~0u);
        gl2_main(s);
        g_hash_table_foreach_remove(s->surfs, reset_surf_drop, NULL);
        g_hash_table_foreach_remove(s->texs, reset_tex_drop, NULL);
        for (int i = 0; i < MAX_SCAN; i++) {
            scan_free(&s->scan[i]);
        }
        if (s->fence) {
            glDeleteSync(s->fence);
            s->fence = 0;
        }
        if (s->scrFbo) {                /* no picture of the previous boot */
            glBindFramebuffer(GL_FRAMEBUFFER, s->scrFbo);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        while (glGetError() != GL_NO_ERROR) {
        }
        eglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    s->pending = 0;
    s->hfOn = false;                    /* the display shows guest memory until the next composition */
    if (s->frame_seq) {
        s->frame_seq[4] = 0;
    }
    s->owner = 0;
    s->buf = s->len = s->out = s->outlen = s->result = 0;
    s->va = 0;
    s->host_batch = false;
    s->host_out = NULL;
    s->host_outlen = 0;
}

static void syborg_gl_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = syborg_gl_realize;
    device_class_set_legacy_reset(dc, syborg_gl_reset);
    device_class_set_props(dc, syborg_gl_properties);
}

static const TypeInfo syborg_gl_types[] = {
    {
        .name = TYPE_SYBORG_GL,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SyborgGlState),
        .instance_init = syborg_gl_instance_init,
        .class_init = syborg_gl_class_init,
    },
};

DEFINE_TYPES(syborg_gl_types)
