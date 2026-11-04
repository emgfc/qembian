#!/usr/bin/env python3
"""List every XIP image in a flat ROM that imports (through 8-byte thunks) a given DLL,
with the ordinals used.

  xip_importers.py core-flat.img core.json IscApi.dll [ISIMESSAGE.DLL ...]
"""
import json, struct, sys

B = 0x80000000


def main():
    img, js = sys.argv[1], sys.argv[2]
    d = open(img, 'rb').read()
    j = json.load(open(js))
    imgs = [f for f in j['files'] if 'image' in f]
    for want in sys.argv[3:]:
        tgt = [f for f in imgs if f['path'].lower().endswith('/' + want.lower())]
        if not tgt:
            print('# not found:', want)
            continue
        ti = tgt[0]['image']
        ex = struct.unpack_from('<%dI' % ti['expcount'], d, ti['expdir'] - B)
        byaddr = {}
        for k, x in enumerate(ex, 1):
            byaddr.setdefault(x, []).append(k)
        print('# importers of %s (%d exports)' % (tgt[0]['path'], len(ex)))
        for f in imgs:
            im = f['image']
            code, size = im['code'], im['codesize']
            blob = d[code - B:code - B + size]
            n = size // 4
            words = struct.unpack_from('<%dI' % n, blob)
            ords = set()
            for k in range(n - 1):
                if words[k] == 0xE51FF004 and words[k + 1] in byaddr:
                    ords.update(byaddr[words[k + 1]])
            if ords and f['path'] != tgt[0]['path']:
                print('%-40s %s' % (f['path'].split('/')[-1], ' '.join('#%d' % o for o in sorted(ords))))


main()
