#!/usr/bin/env python3
"""OpenGL ES 1.1 and 2.0 remoting (docs/devices/nokia-n8-rm596/gles-host.md): the guest is forwarding only.

  gen-gles.py SDK_INCLUDE GUESTDIR HOSTDIR
  -> GUESTDIR/gles_stubs.c   khr1_<name> / khr2_<name>: every function of GLES/gl.h + glext.h and
                             GLES2/gl2.h + gl2ext.h as one command {ctx, fn, words}: scalars one word each
                             (floats as bits), pointers as the caller's virtual addresses. A call with a
                             pointer or a result is sent at once (the host reads and writes the caller's
                             memory during it); the rest is batched. No sizes, no state, no checks here.
     GUESTDIR/gles_procs.c   name -> stub, for eglGetProcAddress
     GUESTDIR/gles_alias.txt "name target" for libEGL's khr_<name> exports (src/khr/gen_khr.py)
     GUESTDIR/gl1_fn.h, gl2_fn.h   function numbers (guest)
     HOSTDIR/syborg_gl1_fn.h, syborg_gl2_fn.h   the same numbers (host)
     HOSTDIR/syborg_gl1_cases.h, syborg_gl2_cases.h   host replay of every call not written by hand
                             (src/qemu-syborg/hw/arm/syborg_gles.h): the size of each pointer's data is
                             decided here, on the host (RULES), from the call's scalars and the host's
                             GL state; outputs are written back into the caller's memory.
"""
import os, re, sys

inc, gdir, hdir = sys.argv[1:4]

SCALARS = {'GLenum', 'GLuint', 'GLint', 'GLsizei', 'GLboolean', 'GLbitfield', 'GLclampf', 'GLfloat',
           'GLfixed', 'GLclampx', 'GLshort', 'GLubyte', 'GLintptr', 'GLsizeiptr', 'GLeglImageOES', 'int'}
FLOATS = {'GLclampf', 'GLfloat'}
SIGNED = {'GLint', 'GLsizei', 'GLintptr', 'GLsizeiptr', 'GLfixed', 'GLclampx', 'GLshort', 'int'}

# host replay written by hand (syborg_gles.h): names, client arrays and draws, state the host translates
# to guest names, strings, framebuffer 0, the matrix palette, mapped buffers, EGL images
MANUAL = {
    1: {'glGenBuffers', 'glDeleteBuffers', 'glGenTextures', 'glDeleteTextures', 'glGenFramebuffersOES',
        'glDeleteFramebuffersOES', 'glGenRenderbuffersOES', 'glDeleteRenderbuffersOES', 'glBindFramebufferOES',
        'glFramebufferRenderbufferOES', 'glFramebufferTexture2DOES', 'glGetFramebufferAttachmentParameterivOES',
        'glGetString', 'glGetBooleanv', 'glGetFixedv', 'glGetFloatv', 'glGetIntegerv', 'glGetPointerv',
        'glIsEnabled', 'glEnable', 'glDisable', 'glEnableClientState', 'glDisableClientState',
        'glClientActiveTexture', 'glVertexPointer', 'glNormalPointer', 'glColorPointer', 'glTexCoordPointer',
        'glPointSizePointerOES', 'glMatrixIndexPointerOES', 'glWeightPointerOES', 'glDrawArrays',
        'glDrawElements', 'glReadPixels', 'glMatrixMode', 'glLoadIdentity', 'glLoadMatrixf', 'glLoadMatrixx',
        'glMultMatrixf', 'glMultMatrixx', 'glTranslatef', 'glTranslatex', 'glRotatef', 'glRotatex',
        'glScalef', 'glScalex', 'glFrustumf', 'glFrustumx', 'glOrthof', 'glOrthox', 'glPushMatrix',
        'glPopMatrix', 'glCurrentPaletteMatrixOES', 'glLoadPaletteFromModelViewMatrixOES',
        'glQueryMatrixxOES', 'glMapBufferOES', 'glUnmapBufferOES', 'glGetBufferPointervOES',
        'glEGLImageTargetTexture2DOES', 'glEGLImageTargetRenderbufferStorageOES', 'glGetBufferParameteriv',
        'glGetError'},
    2: {'glGenBuffers', 'glDeleteBuffers', 'glGenTextures', 'glDeleteTextures', 'glGenFramebuffers',
        'glDeleteFramebuffers', 'glGenRenderbuffers', 'glDeleteRenderbuffers', 'glCreateProgram',
        'glCreateShader', 'glDeleteProgram', 'glDeleteShader', 'glBindFramebuffer', 'glFramebufferRenderbuffer',
        'glFramebufferTexture2D', 'glGetFramebufferAttachmentParameteriv', 'glGetString', 'glGetBooleanv',
        'glGetFloatv', 'glGetIntegerv', 'glGetAttachedShaders', 'glGetVertexAttribfv', 'glGetVertexAttribiv',
        'glGetVertexAttribPointerv', 'glEnableVertexAttribArray', 'glDisableVertexAttribArray',
        'glVertexAttribPointer', 'glDrawArrays', 'glDrawElements', 'glReadPixels', 'glShaderSource',
        'glShaderBinary', 'glGetUniformfv', 'glGetUniformiv', 'glMapBufferOES', 'glUnmapBufferOES',
        'glGetBufferPointervOES', 'glEGLImageTargetTexture2DOES', 'glEGLImageTargetRenderbufferStorageOES',
        'glFramebufferTexture3DOES', 'glGetBufferParameteriv', 'glGetError'},
}
# calls without pointers that must run while the caller waits: draws read client arrays from its memory
SYNC = {'glDrawArrays', 'glFinish'}

# the size in bytes of a pointer's data, decided on the host: (api or 0, function, parameter) -> C over
# v_<scalar>, n_<name param mapped to the host> and host helpers (syborg_gles.h)
RULES = {}
def rule(fns, param, expr, api=0):
    for f in fns.split():
        RULES[(api, f, param)] = expr
rule('glBufferData glBufferSubData', 'data', 'v_size')
rule('glCompressedTexImage2D glCompressedTexSubImage2D glCompressedTexImage3DOES glCompressedTexSubImage3DOES',
     'data', 'v_imageSize')
rule('glTexImage2D glTexSubImage2D', 'pixels', 'gles_pix_bytes(v_width, v_height, 1, v_format, v_type, GL_UNPACK_ALIGNMENT)')
rule('glTexImage3DOES glTexSubImage3DOES', 'pixels', 'gles_pix_bytes(v_width, v_height, v_depth, v_format, v_type, GL_UNPACK_ALIGNMENT)')
rule('glBindAttribLocation glGetAttribLocation glGetUniformLocation', 'name', 'GLES_STRING')
rule('glGetActiveAttrib glGetActiveUniform', 'length', '4')
rule('glGetActiveAttrib glGetActiveUniform', 'size', '4')
rule('glGetActiveAttrib glGetActiveUniform', 'type', '4')
rule('glGetActiveAttrib glGetActiveUniform', 'name', 'MAX(v_bufsize, 0)')
rule('glGetProgramInfoLog glGetShaderInfoLog glGetShaderSource', 'length', '4')
rule('glGetProgramInfoLog glGetShaderInfoLog', 'infolog', 'MAX(v_bufsize, 0)')
rule('glGetShaderSource', 'source', 'MAX(v_bufsize, 0)')
rule('glGetProgramiv glGetShaderiv glGetRenderbufferParameteriv glGetRenderbufferParameterivOES', 'params', '4')
rule('glGetShaderPrecisionFormat', 'range', '8')
rule('glGetShaderPrecisionFormat', 'precision', '4')
rule('glGetTexParameterfv glGetTexParameteriv glGetTexParameterxv glTexParameterfv glTexParameteriv glTexParameterxv',
     'params', '4 * gl1_count_texparam(v_pname)')
for k in range(1, 5):
    rule('glUniform%dfv glUniform%div' % (k, k), 'v', 'MAX(v_count, 0) * %d * 4' % k)
    rule('glVertexAttrib%dfv' % k, 'values', '%d * 4' % k)
for k in (2, 3, 4):
    rule('glUniformMatrix%dfv' % k, 'value', 'MAX(v_count, 0) * %d * 4' % (k * k))
rule('glClipPlanef glClipPlanex', 'equation', '16')
rule('glGetClipPlanef glGetClipPlanex', 'eqn', '16')
for base, cnt in (('Fog', 'fog'), ('LightModel', 'lightmodel'), ('Light', 'light'), ('Material', 'material'),
                  ('PointParameter', 'pointparam'), ('TexEnv', 'texenv')):
    for t in ('fv', 'iv', 'xv'):
        rule('gl%s%s glGet%s%s' % (base, t, base, t), 'params', '4 * gl1_count_%s(v_pname)' % cnt)
for t in ('fv', 'iv', 'xv'):
    rule('glTexGen%sOES glGetTexGen%sOES' % (t, t), 'params', '4 * gl1_count_texgen(v_pname)')
rule('glDrawTexivOES glDrawTexxvOES glDrawTexfvOES', 'coords', '20')
rule('glDrawTexsvOES', 'coords', '10')
rule('glLoadMatrixf glLoadMatrixx glMultMatrixf glMultMatrixx', 'm', '64')
# object-name parameters -> kind (and created on bind); a few parameters named so are not names
NAMES = {'program': 'PROG', 'shader': 'SHADER', 'buffer': 'BUF', 'texture': 'TEX', 'framebuffer': 'FBO',
         'renderbuffer': 'RBO'}
NOT_NAMES = {('glActiveTexture', 'texture'), ('glClientActiveTexture', 'texture')}
CREATE_ON = {'glBindBuffer', 'glBindTexture', 'glBindRenderbuffer', 'glBindRenderbufferOES'}
# ES 2 extension functions the host's ES 3 context has as core functions
HOSTNAME = {2: {'glTexImage3DOES': 'glTexImage3D', 'glTexSubImage3DOES': 'glTexSubImage3D',
                'glCopyTexSubImage3DOES': 'glCopyTexSubImage3D', 'glCompressedTexImage3DOES': 'glCompressedTexImage3D',
                'glCompressedTexSubImage3DOES': 'glCompressedTexSubImage3D'}}


def parse(path):
    fs = []
    for ln in open(path):
        m = re.match(r'\s*GL_API(?:CALL)?\s+(.+?)\s*GL_APIENTRY\s+(gl\w+)\s*\((.*?)\)', ln)
        if not m:
            continue
        ret, name, params = ' '.join(m.group(1).split()), m.group(2), m.group(3).strip()
        ps = []
        if params and params != 'void':
            for p in params.split(','):
                p = p.strip()
                arr = '[' in p
                p = re.sub(r'\s*\[\s*\d*\s*\]', '', p)
                pm = re.match(r'(.*?)(\w+)$', p)
                t = pm.group(1).strip()
                ptr = t.count('*') + (1 if arr else 0)
                ps.append((t.replace('*', '').replace('const', '').strip(), pm.group(2), ptr, 'const' in t))
        fs.append((ret, name, ps))
    return fs


API = {}
gl1 = parse(os.path.join(inc, 'gles/gl.h'))
gl1x = parse(os.path.join(inc, 'gles/glext.h'))
gl2 = parse(os.path.join(inc, 'gles2/gl2.h'))
gl2x = parse(os.path.join(inc, 'gles2/gl2ext.h'))
if len(gl1) != 145 or len(gl2) != 142:
    raise SystemExit('gen-gles: %d ES 1.1 / %d ES 2 functions (want 145 / 142)' % (len(gl1), len(gl2)))
core1 = {f[1] for f in gl1}
alias1 = {f[1]: f[1][:-3] for f in gl1x if f[1].endswith('OES') and f[1][:-3] in core1}
API[1] = [f for f in gl1] + [f for f in gl1x if f[1] not in alias1]
API[2] = gl2 + gl2x
num = {a: {n: i + 1 for i, n in enumerate(sorted(f[1] for f in API[a]))} for a in API}
for a in API:
    miss = MANUAL[a] - set(num[a])
    if miss:
        raise SystemExit('gen-gles: ES %d manual functions not in the headers: %s' % (a, ' '.join(sorted(miss))))

for a in API:
    names = sorted(num[a])
    h = ('/* Generated by tools/gen-gles.py from the SDK\'s headers: OpenGL ES %s function numbers (SYGL_GL%d) */\n'
         '#ifndef GL%d_FN_H\n#define GL%d_FN_H\nenum {\n' % ('1.1' if a == 1 else '2.0', a, a, a)
         + ''.join('\tGL%d_FN_%s = %d,\n' % (a, n, num[a][n]) for n in names)
         + '\tGL%d_FN_COUNT = %d\n};\n' % (a, len(names) + 1)
         + '#define GL%d_FN_NAMES "", ' % a + ', '.join('"%s"' % n for n in names) + '\n#endif\n')
    open(os.path.join(gdir, 'gl%d_fn.h' % a), 'w').write(h)
    open(os.path.join(hdir, 'syborg_gl%d_fn.h' % a), 'w').write(h)


def word(t, v, ptr):
    if ptr:
        return '(unsigned)%s' % v
    if t in FLOATS:
        return 'khr_gl_f2u(%s)' % v
    return '(unsigned)(int)%s' % v if t in ('GLshort',) else '(unsigned)%s' % v


def ctype(t, ptr, c):
    if not ptr:
        return t
    return ('const ' if c else '') + t + ' ' + '*' * ptr


stubs, procs, alias = [], [], []
for a in (1, 2):
    host = []
    for ret, name, ps in API[a]:
        sig = ', '.join('%s %s' % (ctype(t, ptr, c), p) for t, p, ptr, c in ps) or 'void'
        rt = ret.replace('const', '').replace('*', '').strip()
        rptr = '*' in ret
        hasptr = any(ptr for _, _, ptr, _ in ps)
        sync = hasptr or ret != 'void' or name in SYNC
        b = ['\tunsigned* p = khr_gl_call(SYGL_GL%d, GL%d_FN_%s, %d);' % (a, a, name, len(ps))]
        zero = '' if ret == 'void' else ' 0'
        b.append('\tif (!p) return%s;' % zero)
        for i, (t, p, ptr, c) in enumerate(ps):
            b.append('\tp[%d] = %s;' % (i, word(t, p, ptr)))
        if name in ('glMapBufferOES', 'glUnmapBufferOES'):
            b.append('\treturn (%s)khr_gl_map(SYGL_GL%d, GL%d_FN_%s, p, %d);' % (ret, a, a, name, 1 if name == 'glMapBufferOES' else 0))
        elif name == 'glGetString':
            b.append('\treturn (const GLubyte*)khr_gl_string(khr_gl_sync(4096));')
        elif sync:
            b.append('\tconst unsigned* r = khr_gl_sync(4);')
            if ret == 'void':
                b.append('\t(void)r;')
                b.append('\tkhr_gl_done();')
            else:
                b.append('\t%s v = r ? (%s)%sr[0] : 0;' % (ret, ret, '' if not rptr else ''))
                b.append('\tkhr_gl_done();')
                b.append('\treturn v;')
        else:
            b.append('\tkhr_gl_done();')
        stubs.append('%s khr%d_%s(%s)\n{\n%s\n}\n' % (ret, a, name, sig, '\n'.join(b)))
        # host
        if name in MANUAL[a]:
            continue
        hn = HOSTNAME.get(a, {}).get(name, name)
        hb = ['\tcase GL%d_FN_%s: {' % (a, name), '\t\tif (n < %d) break;' % len(ps)]
        args, frees, outs = [None] * len(ps), [], []
        for i, (t, p, ptr, c) in enumerate(ps):
            if not ptr:
                if t in FLOATS:
                    hb.append('\t\tGLfloat v_%s = U2F(a[%d]);' % (p, i))
                else:
                    ct = 'GLint' if t in SIGNED else ('uint32_t' if t == 'GLeglImageOES' else t)
                    hb.append('\t\t%s v_%s = (%s)a[%d];' % (ct, p, ct, i))
                k = None if (name, p) in NOT_NAMES else NAMES.get(p)
                if k:
                    hb.append('\t\tGLuint n_%s = gl2_name(c, GL2_N_%s, v_%s, %d);' % (p, k, p, 1 if name in CREATE_ON else 0))
                    args[i] = 'n_%s' % p
                else:
                    args[i] = 'v_%s' % p
        for i, (t, p, ptr, c) in enumerate(ps):
            if not ptr:
                continue
            ex = RULES.get((a, name, p)) or RULES.get((0, name, p))
            if not ex:
                raise SystemExit('gen-gles: ES %d %s(%s): no size rule (add one, or write it by hand)' % (a, name, p))
            if ex == 'GLES_STRING':
                hb.append('\t\tchar *h_%s = gles_string(s, a[%d]);' % (p, i))
                hb.append('\t\tif (!h_%s) break;' % p)
                args[i] = 'h_%s' % p
                frees.append('g_free(h_%s);' % p)
                continue
            hb.append('\t\tsize_t b_%s = (size_t)(%s);' % (p, ex))
            if c:
                hb.append('\t\tvoid *h_%s = gles_in(s, a[%d], b_%s);' % (p, i, p))
                hb.append('\t\tif (a[%d] && !h_%s) { gles_bad(s, "%s"); break; }' % (i, p, name))
            else:                   # the call's arena (phase 4): capped, released by gles_call
                hb.append('\t\tvoid *h_%s = gles_room(s, b_%s);' % (p, p))
                hb.append('\t\tif (a[%d] && !h_%s) { gles_oom(c); break; }' % (i, p))
                outs.append((p, i))
            args[i] = '(%s%s *)h_%s' % ('const ' if c else '', t if t != 'GLvoid' else 'void', p)
        call = '%s(%s)' % (hn, ', '.join(args))
        if ret != 'void':
            hb.append('\t\tuint32_t r_ = (uint32_t)%s;' % call)
        else:
            hb.append('\t\t%s;' % call)
        for p, i in outs:
            hb.append('\t\tgles_put(s, a[%d], h_%s, b_%s);' % (i, p, p))
        hb += ['\t\t' + f for f in frees]
        if ret != 'void':
            hb.append('\t\treturn gles_ret(s, r_);')
        hb.append('\t\tbreak;')
        hb.append('\t}')
        host.append('\n'.join(hb))
    with open(os.path.join(hdir, 'syborg_gl%d_cases.h' % a), 'w') as f:
        f.write('/* Generated by tools/gen-gles.py: the host replay of the OpenGL ES %s calls not written by hand\n'
                '   (included in syborg_gles.h\'s switch; c = the guest context, a = the call\'s words) */\n'
                % ('1.1' if a == 1 else '2.0'))
        f.write('\n'.join(host) + '\n')

with open(os.path.join(gdir, 'gles_stubs.c'), 'w') as f:
    f.write('/* Generated by tools/gen-gles.py: every OpenGL ES 1.1 and 2.0 function as one forwarded call\n'
            '   (docs/devices/nokia-n8-rm596/gles-host.md). Nothing is decided here. */\n#include "gles_int.h"\n\n')
    f.write('\n'.join(stubs))

allnames = []
for a in (1, 2):
    for n in sorted({x[1] for x in (gl1 + gl1x if a == 1 else gl2 + gl2x)}):
        tgt = 'khr%d_%s' % (a, alias1.get(n, n) if a == 1 else n)
        allnames.append((a, n, tgt))
with open(os.path.join(gdir, 'gles_procs.c'), 'w') as f:
    f.write('/* Generated by tools/gen-gles.py: OpenGL ES names for eglGetProcAddress (the host decides which\n'
            '   exist: syvg_egl GetProcAddress) */\n')
    for t in sorted({x[2] for x in allnames}):
        f.write('extern void %s(void);\n' % t)
    f.write('const struct KhrGlProc { const char* name; void (*fn)(void); int api; } khr_gl_procs[] = {\n')
    for a, n, t in allnames:
        f.write('\t{ "%s", %s, %d },\n' % (n, t, a))
    f.write('\t{ 0, 0, 0 }\n};\n')
with open(os.path.join(gdir, 'gles_alias.txt'), 'w') as f:
    for a, n, t in allnames:
        if a == 1:
            f.write('%s %s\n' % (n, t))
print('gen-gles: ES 1.1 %d functions (+%d aliases), ES 2 %d; host by hand %d + %d'
      % (len(API[1]), len(alias1), len(API[2]), len(MANUAL[1]), len(MANUAL[2])))
