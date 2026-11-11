#!/usr/bin/env python3
"""OpenVG 1.1 + VGU remoting to the host (docs/devices/nokia-n8-rm596/egl-vg-host-v2.md): function table,
guest stubs and host dispatch, generated from the Khronos headers.

  gen-vg2.py OPENVG_H VGU_H GUESTDIR HOSTDIR
  -> HOSTDIR/vg2_fn.h        VG2_FN_<name> numbers (guest and host; committed in src/syvg)
     HOSTDIR/vg2_dispatch.inc  host: one case per function (src/syvg/vg2.cpp includes it)
     GUESTDIR/vg2_gen.c      guest: every OpenVG/VGU function as a synchronous call (khrVgCall)

Every call is one SYGL_VG2 command {fn, guest thread, args}: scalars one word each (floats as their bits),
handles as the 32-bit ids the host hands out (mapped per guest process on the host), pointers as guest
virtual addresses. The call runs in the calling guest thread's kick, so the host reads inputs and writes
outputs in guest memory directly (syvg_guest_read/write); the sizes are known on the host (SIZES below,
some from the host's own objects: a path's datatype and segments). The result is one word in the output
area (vgGetString: the string).
"""
import os, re, sys

openvg_h, vgu_h, gdir, hdir = sys.argv[1:5]
HANDLES = {'VGHandle', 'VGPath', 'VGPaint', 'VGImage', 'VGMaskLayer', 'VGFont'}
FLOATS = {'VGfloat'}

# in/out pointer parameters: function -> {param: (direction, C++ byte-size expression on the host)}
# expressions may use the other parameters (already decoded), and helpers from vg2.cpp:
#   pathCoordBytes(path, segs, n)  bytes of coordinates for n segments (path's datatype)
#   pathModifyBytes(path, start, n) bytes of the coordinates of segments start..start+n-1 of the path
#   imageBytes(stride, format, w, h) span of h rows of w pixels at stride
#   a third element: the bytes of the span before the pointer (imageBack: stride < 0, row 0 is the last row in memory)
SIZES = {
    'vgSetfv': {'values': ('in', 'count * 4')}, 'vgSetiv': {'values': ('in', 'count * 4')},
    'vgGetfv': {'values': ('out', 'count * 4')}, 'vgGetiv': {'values': ('out', 'count * 4')},
    'vgSetParameterfv': {'values': ('in', 'count * 4')}, 'vgSetParameteriv': {'values': ('in', 'count * 4')},
    'vgGetParameterfv': {'values': ('out', 'count * 4')}, 'vgGetParameteriv': {'values': ('out', 'count * 4')},
    'vgLoadMatrix': {'m': ('in', '36')}, 'vgMultMatrix': {'m': ('in', '36')}, 'vgGetMatrix': {'m': ('out', '36')},
    'vgAppendPathData': {'pathSegments': ('in', 'numSegments'),
                         'pathData': ('in', 'pathCoordBytes(dstPath, (const VGubyte *)pathSegments, numSegments)')},
    'vgModifyPathCoords': {'pathData': ('in', 'pathModifyBytes(dstPath, startIndex, numSegments)')},
    'vgPointAlongPath': {'x': ('out', '4'), 'y': ('out', '4'), 'tangentX': ('out', '4'), 'tangentY': ('out', '4')},
    'vgPathBounds': {'minX': ('out', '4'), 'minY': ('out', '4'), 'width': ('out', '4'), 'height': ('out', '4')},
    'vgPathTransformedBounds': {'minX': ('out', '4'), 'minY': ('out', '4'), 'width': ('out', '4'), 'height': ('out', '4')},
    'vgImageSubData': {'data': ('in', 'imageBytes(dataStride, dataFormat, width, height)', 'imageBack(dataStride, height)')},
    'vgGetImageSubData': {'data': ('out', 'imageBytes(dataStride, dataFormat, width, height)', 'imageBack(dataStride, height)')},
    'vgWritePixels': {'data': ('in', 'imageBytes(dataStride, dataFormat, width, height)', 'imageBack(dataStride, height)')},
    'vgReadPixels': {'data': ('out', 'imageBytes(dataStride, dataFormat, width, height)', 'imageBack(dataStride, height)')},
    'vgSetGlyphToPath': {'glyphOrigin': ('in', '8'), 'escapement': ('in', '8')},
    'vgSetGlyphToImage': {'glyphOrigin': ('in', '8'), 'escapement': ('in', '8')},
    'vgDrawGlyphs': {'glyphIndices': ('in', 'glyphCount * 4'), 'adjustments_x': ('in', 'glyphCount * 4'),
                     'adjustments_y': ('in', 'glyphCount * 4')},
    'vgColorMatrix': {'matrix': ('in', '80')},
    'vgConvolve': {'kernel': ('in', 'kernelWidth * kernelHeight * 2')},
    'vgSeparableConvolve': {'kernelX': ('in', 'kernelWidth * 2'), 'kernelY': ('in', 'kernelHeight * 2')},
    'vgLookup': {'redLUT': ('in', '256'), 'greenLUT': ('in', '256'), 'blueLUT': ('in', '256'), 'alphaLUT': ('in', '256')},
    'vgLookupSingle': {'lookupTable': ('in', '1024')},
    'vguPolygon': {'points': ('in', 'count * 8')},
    'vguComputeWarpQuadToSquare': {'matrix': ('out', '36')},
    'vguComputeWarpSquareToQuad': {'matrix': ('out', '36')},
    'vguComputeWarpQuadToQuad': {'matrix': ('out', '36')},
}
# handles a function gives back or drops: the host maps new ones and forgets destroyed ones
DESTROY = {'vgDestroyPath': 'path', 'vgDestroyPaint': 'paint', 'vgDestroyImage': 'image',
           'vgDestroyMaskLayer': 'maskLayer', 'vgDestroyFont': 'font'}


def protos():
    src = open(openvg_h).read() + open(vgu_h).read()
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    src = re.sub(r'#[^\n]*', '', src)
    out = []
    for r, n, p in re.findall(r'VGU?_API_CALL\s+([\w\s\*]+?)\s+VGU?_API_?ENTRY\s+(vgu?\w+)\s*\(([^)]*)\)\s*VGU?_API_?EXIT\s*;', src):
        ps = []
        p = ' '.join(p.split())
        if p and p != 'void':
            for a in p.split(','):
                a = a.strip()
                arr = '[' in a
                a = re.sub(r'\s*\[\s*\d*\s*\]', '', a)
                m = re.match(r'(.*?)(\w+)$', a)
                t, name = m.group(1).strip(), m.group(2)
                ptr = '*' in t or arr
                ps.append((t.replace('*', '').replace('const', '').strip(), name, ptr, 'const' in t))
        out.append((' '.join(r.split()), n, ps))
    return out


fs = protos()
os.makedirs(gdir, exist_ok=True)
os.makedirs(hdir, exist_ok=True)
with open(os.path.join(hdir, 'vg2_fn.h'), 'w') as f:
    f.write('/* Generated by tools/gen-vg2.py from the Khronos OpenVG 1.1 / VGU headers -- do not edit. */\n')
    f.write('#ifndef VG2_FN_H\n#define VG2_FN_H\nenum {\n')
    for i, (r, n, ps) in enumerate(fs):
        f.write('\tVG2_FN_%s = %d,\n' % (n, i))
    f.write('\tVG2_FN_COUNT = %d\n};\n#endif\n' % len(fs))

# ---- guest stubs
g = ['/* Generated by tools/gen-vg2.py -- do not edit. OpenVG 1.1 + VGU: every call to the host (SYGL_VG2). */',
     '#include <VG/openvg.h>', '#include <VG/vgu.h>', '#include "vg2_fn.h"',
     'unsigned int khrVgCall(int aFn, const unsigned int *aArgs, int aCount);   /* vg2.c: synchronous, result word */',
     'const VGubyte *khrVgString(VGStringID aName);                               /* vg2.c */',
     'typedef union { VGfloat f; unsigned int u; } khrVgF;', '']
for r, n, ps in fs:
    sig = ', '.join(('const ' if c else '') + t + (' *' if p else ' ') + a for t, a, p, c in ps) or 'void'
    if n == 'vgGetString':
        g.append('const VGubyte *%s(%s)\n{\n\treturn khrVgString(name);\n}\n' % (n, sig))
        continue
    g.append('%s %s(%s)\n{' % (r, n, sig))
    if ps:
        words = []
        for t, a, p, c in ps:
            if p:
                words.append('(unsigned int)%s' % a)
            elif t in FLOATS:
                words.append('((khrVgF){ .f = %s }).u' % a)
            else:
                words.append('(unsigned int)%s' % a)
        g.append('\tunsigned int a[%d] = { %s };' % (len(words), ', '.join(words)))
        call = 'khrVgCall(VG2_FN_%s, a, %d)' % (n, len(words))
    else:
        call = 'khrVgCall(VG2_FN_%s, 0, 0)' % n
    if r == 'void':
        g.append('\t%s;' % call)
    elif r in FLOATS:
        g.append('\tkhrVgF v;\n\tv.u = %s;\n\treturn v.f;' % call)
    else:
        g.append('\treturn (%s)%s;' % (r, call))
    g.append('}\n')
open(os.path.join(gdir, 'vg2_gen.c'), 'w').write('\n'.join(g))

# ---- host dispatch (C++, inside vg2.cpp's dispatch(fn, a, n)): a[] = the call's argument words
h = ['/* Generated by tools/gen-vg2.py -- do not edit. Included by src/syvg/vg2.cpp inside dispatch(). */']
for r, n, ps in fs:
    if n == 'vgGetString':
        continue                    # vg2.cpp: the string into the output area
    h.append('case VG2_FN_%s: {' % n)
    if len(ps):
        h.append('\tif (n < %d) return badArgs(fn);' % len(ps))
    sizes = SIZES.get(n, {})
    names = []
    for i, (t, a, p, c) in enumerate(ps):
        if p:
            h.append('\tuint32_t %s_va = a[%d];' % (a, i))
            names.append(a)
        elif t in FLOATS:
            h.append('\t%s %s = wordFloat(a[%d]);' % (t, a, i))
            names.append(a)
        elif t in HANDLES:
            h.append('\t%s %s = (%s)hostHandle(a[%d]);' % (t, a, t, i))
            names.append(a)
        else:
            h.append('\t%s %s = (%s)a[%d];' % (t, a, t, i))
            names.append(a)
    # pointers: copy in / room for out, after the scalars they are sized by
    for t, a, p, c in ps:
        if not p:
            continue
        if a not in sizes:
            raise SystemExit('gen-vg2: no size rule for %s(%s)' % (n, a))
        d, expr = sizes[a][:2]
        back = sizes[a][2] if len(sizes[a]) > 2 else None
        h.append('\tGuestBuf %s_b(%s_va, %s, %s%s);' % (a, a, '(size_t)(%s)' % expr, 'true' if d == 'in' else 'false',
                                                     ', (size_t)(%s)' % back if back else ''))
        h.append('\tif (!%s_b.ok()) return badPointer(fn);' % a)
        h.append('\t%s *%s = (%s *)%s_b.host();' % (t if t != 'void' else 'void', a, t if t != 'void' else 'void', a))
    call = '%s(%s)' % (n, ', '.join(names))
    if r == 'void':
        h.append('\t%s;' % call)
        res = '0'
    elif r in HANDLES:
        h.append('\t%s ret = %s;' % (r, call))
        res = 'guestHandle(ret)'
    elif r in FLOATS:
        h.append('\tVGfloat ret = %s;' % call)
        res = 'floatWord(ret)'
    else:
        h.append('\t%s ret = %s;' % (r, call))
        res = '(uint32_t)ret'
    for t, a, p, c in ps:
        if p and sizes[a][0] == 'out':
            h.append('\tif (!%s_b.writeBack()) return badPointer(fn);' % a)
    if n in DESTROY:
        h.append('\tdropHandle(a[0]);')
    h.append('\treturn result(%s);\n}' % res)
open(os.path.join(hdir, 'vg2_dispatch.inc'), 'w').write('\n'.join(h) + '\n')
print('gen-vg2: %d functions' % len(fs))
