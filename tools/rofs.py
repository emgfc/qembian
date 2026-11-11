#!/usr/bin/env python3
"""List / extract files from a Symbian ROFS (or ROFx extension) image.

  rofs.py IMG                    list "<offset> <size> <path>" (ROFx entries whose data is in ROFS1: "rofs1@<off>")
  rofs.py IMG --extract PATH OUT extract one file (path match is case-insensitive, '/' separators)
"""
import argparse, struct


def walk(d, dir_off, path='', out=None, base=0):
    """Offsets inside the image are relative to the partition start; a ROFx extension image is
    placed at ROFS1's iMaxImageSize, so its offsets carry that base and we subtract it."""
    out = [] if out is None else out
    dir_off -= base
    if not 0 <= dir_off <= len(d) - 12:
        return out              # a ROFx directory kept in ROFS1 (N81 8GB 21.0.010): not in this image
    size, pad, first, fblock, fsize = struct.unpack_from('<HBBII', d, dir_off)
    p = dir_off + first
    end = dir_off + size
    while p < end:                                   # sub-directory entries
        es = struct.unpack_from('<H', d, p)[0]
        if es == 0:
            break
        noff, att, fs, fa, attx, nl = struct.unpack_from('<BBIIBB', d, p + 18)
        name = d[p + noff:p + noff + 2 * nl].decode('utf-16le')
        walk(d, fa, path + name + '/', out, base)
        p += es
    p, end = fblock - base, min(fblock - base + fsize, len(d))
    if p < 0:
        return out              # the file block lives in ROFS1 too
    while p < end:                                   # file entries
        es = struct.unpack_from('<H', d, p)[0]
        if es == 0:
            break
        noff, att, fs, fa, attx, nl = struct.unpack_from('<BBIIBB', d, p + 18)
        name = d[p + noff:p + noff + 2 * nl].decode('utf-16le')
        out.append((fa, fs, path + name))   # partition-relative
        p += es
    return out


def entry(d, want, dir_off=None, path=''):
    """offset of the directory entry (TRofsEntry) of file `want` in a ROFS1 image, or None"""
    if dir_off is None:
        dir_off = struct.unpack_from('<I', d, 8)[0]
    want = want.lower().strip('/')
    size, pad, first, fblock, fsize = struct.unpack_from('<HBBII', d, dir_off)
    p, end = dir_off + first, dir_off + size
    while p < end:
        es = struct.unpack_from('<H', d, p)[0]
        if es == 0:
            break
        noff, att, fs, fa, attx, nl = struct.unpack_from('<BBIIBB', d, p + 18)
        name = d[p + noff:p + noff + 2 * nl].decode('utf-16le')
        hit = entry(d, want, fa, path + name + '/')
        if hit is not None:
            return hit
        p += es
    p, end = fblock, fblock + fsize
    while p < end:
        es = struct.unpack_from('<H', d, p)[0]
        if es == 0:
            break
        noff, att, fs, fa, attx, nl = struct.unpack_from('<BBIIBB', d, p + 18)
        name = d[p + noff:p + noff + 2 * nl].decode('utf-16le')
        if (path + name).lower() == want:
            return p
        p += es
    return None


def files(d):
    if d[:4] not in (b'ROFS', b'ROFx'):
        raise SystemExit('not a ROFS image')
    tree_off = struct.unpack_from('<I', d, 8)[0]
    # ROFx: offsets count from ROFS1's iMaxImageSize, the tree right after the header (iHeaderSize at +4);
    # the image may be larger than that base (N81 8GB 21.0.010: 0x21d04b0 bytes, base 0x2030000)
    hsize = d[4]
    if d[:4] == b'ROFx' and tree_off > hsize and (tree_off - hsize) & 0xfff == 0:
        base = tree_off - hsize
    else:
        base = tree_off & ~0xfff if tree_off >= len(d) else 0
    return base, walk(d, tree_off, base=base)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('img')
    ap.add_argument('--extract', nargs=2, metavar=('PATH', 'OUT'))
    a = ap.parse_args()
    d = open(a.img, 'rb').read()
    base, fl = files(d)
    if a.extract:
        want = a.extract[0].lower().strip('/')
        hit = [f for f in fl if f[2].lower() == want]
        if not hit:
            raise SystemExit('not found: ' + want)
        fa, fs, p = hit[0]
        if fa < base:
            raise SystemExit('%s: data lives in ROFS1 at 0x%x (ROFx reuses it); extract from rofs1.img' % (p, fa))
        fa -= base
        open(a.extract[1], 'wb').write(d[fa:fa + fs])
        print('%s: %d bytes from 0x%x' % (p, fs, fa))
        return
    for fa, fs, p in fl:
        where = '0x%08x' % (fa - base) if fa >= base else 'rofs1@%x' % fa
        print('%-14s %8d %s' % (where, fs, p))


if __name__ == '__main__':
    main()
