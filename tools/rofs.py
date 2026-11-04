#!/usr/bin/env python3
"""List / extract files from a Symbian ROFS (or ROFx extension) image.

  rofs.py IMG                    list "<offset> <size> <path>"
  rofs.py IMG --extract PATH OUT extract one file (path match is case-insensitive, '/' separators)
"""
import argparse, struct


def walk(d, dir_off, path='', out=None):
    out = [] if out is None else out
    size, pad, first, fblock, fsize = struct.unpack_from('<HBBII', d, dir_off)
    p = dir_off + first
    end = dir_off + size
    while p < end:                                   # sub-directory entries
        es = struct.unpack_from('<H', d, p)[0]
        if es == 0:
            break
        noff, att, fs, fa, attx, nl = struct.unpack_from('<BBIIBB', d, p + 18)
        name = d[p + noff:p + noff + 2 * nl].decode('utf-16le')
        walk(d, fa, path + name + '/', out)
        p += es
    p, end = fblock, fblock + fsize
    while p < end:                                   # file entries
        es = struct.unpack_from('<H', d, p)[0]
        if es == 0:
            break
        noff, att, fs, fa, attx, nl = struct.unpack_from('<BBIIBB', d, p + 18)
        name = d[p + noff:p + noff + 2 * nl].decode('utf-16le')
        out.append((fa, fs, path + name))
        p += es
    return out


def files(d):
    if d[:4] not in (b'ROFS', b'ROFx'):
        raise SystemExit('not a ROFS image')
    tree_off = struct.unpack_from('<I', d, 8)[0]
    return walk(d, tree_off)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('img')
    ap.add_argument('--extract', nargs=2, metavar=('PATH', 'OUT'))
    a = ap.parse_args()
    d = open(a.img, 'rb').read()
    fl = files(d)
    if a.extract:
        want = a.extract[0].lower().strip('/')
        hit = [f for f in fl if f[2].lower() == want]
        if not hit:
            raise SystemExit('not found: ' + want)
        fa, fs, p = hit[0]
        open(a.extract[1], 'wb').write(d[fa:fa + fs])
        print('%s: %d bytes from 0x%x' % (p, fs, fa))
        return
    for fa, fs, p in fl:
        print('0x%08x %8d %s' % (fa, fs, p))


if __name__ == '__main__':
    main()
