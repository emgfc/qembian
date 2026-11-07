#!/usr/bin/env python3
"""Compare two firmwares' kernels exec by exec: the immediate sizes their slow exec handlers pass to
the copy-in/out calls ("mov r2, #n"), a cheap way to spot user/kernel structure changes between OS
versions (docs/adding-a-device.md step 3; found the 9.1 loader structures and find handles).

  exec_sizes.py NEW-core-flat.img NEW-core.json KNOWN-core-flat.img KNOWN-core.json [--map exec.map]
Prints the slow exec table of each kernel and the numbers whose handlers differ. Small constants
(< 8) are mostly noise from neighbouring code; look at the sizes."""
import argparse, json, struct


def imm(w):
    rot = (w >> 8) & 0xf; i = w & 0xff
    return ((i >> (2 * rot)) | (i << (32 - 2 * rot))) & 0xffffffff


def kernel(img, cjson):
    d = open(img, 'rb').read()
    base = struct.unpack_from('<I', d, 0x8c)[0]
    ek = [f for f in json.load(open(cjson))['files'] if f['path'].lower().endswith('ekern.exe')][0]['image']
    cs, ce = ek['code'], ek['code'] + ek['codesize']
    for off in range(cs - base, ce - base, 4):
        n = struct.unpack_from('<I', d, off)[0]
        if 0x80 <= n <= 0x140:
            a, b = struct.unpack_from('<II', d, off + 4)
            if cs <= a < ce and cs <= b < ce and all(cs <= struct.unpack_from('<I', d, off + 12 + 8 * i + 4)[0] < ce for i in range(n)):
                out = {}
                for k in range(n):
                    fn = struct.unpack_from('<I', d, off + 12 + 8 * k + 4)[0]
                    sizes = []
                    for i in range(40):
                        w = struct.unpack_from('<I', d, fn - base + 4 * i)[0]
                        if (w & 0x0ffff000) == 0x03a02000:
                            sizes.append(imm(w))
                        if (w & 0x0fff0000) == 0x08bd0000 and w & 0x8000:
                            break
                    out[k] = tuple(s for s in sizes if s >= 8)
                return base + off, n, out
    raise SystemExit('%s: no slow exec table found in ekern' % img)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('new_img'); ap.add_argument('new_json'); ap.add_argument('known_img'); ap.add_argument('known_json')
    ap.add_argument('--map', help='exec.map for names')
    a = ap.parse_args()
    names = {}
    if a.map:
        for l in open(a.map):
            p = l.split('#')[0].split()
            if len(p) >= 2 and p[0][0] == 'S':
                names[int(p[0][1:], 16)] = p[1]
    ta, na, ha = kernel(a.new_img, a.new_json)
    tb, nb, hb = kernel(a.known_img, a.known_json)
    print('new: slow exec table 0x%08x, %d entries; known: 0x%08x, %d entries' % (ta, na, tb, nb))
    for k in sorted(set(ha) & set(hb)):
        if ha[k] != hb[k]:
            print('S%02x %-28s new %-24s known %s' % (k, names.get(k, '?'), [hex(x) for x in ha[k]], [hex(x) for x in hb[k]]))


if __name__ == '__main__':
    main()
