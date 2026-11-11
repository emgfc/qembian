#!/usr/bin/env python3
"""Carve files out of a Symbian ROFS image by name (no directory walk).

  tools/research/rofs_extract.py build/roms/<id>/<fw>/rofs1.img NAME [NAME ...] [--out DIR]
  tools/research/rofs_extract.py build/roms/<id>/<fw>/rofs1.img --list

Scans the directory-entry area for the UTF-16LE file name and reads the TRofsEntry fields that
precede it. Entry layout (verified on rofs1.img against the E32 header of DATAPORT.CSY at 0x202a0):

  TUint16 iStructSize; TUint8 iUids[16]; TUint8 iNameOffset; TUint8 iAtt;
  TUint32 iFileSize; TUint32 iFileAddress;   # offset from the start of the ROFS image
  TUint8  iAttExtra; TUint8 iNameLength; TUint16 iName[iNameLength];

The carved file is the on-image E32 image (compressed); feed it to tools/e32load.py.
"""
import argparse, os, struct, sys


def header(d):
    if d[:4] not in (b'ROFS', b'ROFx'):
        raise SystemExit('not a ROFS image')
    hsize = d[4]
    dir_off, dir_size, ent_off, ent_size = struct.unpack_from('<IIII', d, 8)
    return hsize, dir_off, dir_size, ent_off, ent_size


def entries(d):
    """Yield (name, size, address) for every file entry found by scanning for name fields."""
    _, dir_off, dir_size, ent_off, ent_size = header(d)
    lo, hi = min(dir_off, ent_off), max(dir_off + dir_size, ent_off + ent_size)
    pos = lo
    while pos + 30 < hi:
        ssize = struct.unpack_from('<H', d, pos)[0]
        name_off = d[pos + 18]
        if ssize < 30 or name_off != 30 or pos + ssize > hi:
            pos += 2
            continue
        fsize, faddr = struct.unpack_from('<II', d, pos + 20)
        nlen = d[pos + 29]
        if nlen == 0 or 30 + 2 * nlen > ssize:
            pos += 2
            continue
        try:
            name = d[pos + 30:pos + 30 + 2 * nlen].decode('utf-16le')
        except UnicodeDecodeError:
            pos += 2
            continue
        if name.isprintable():
            yield name, fsize, faddr
            pos += (ssize + 3) & ~3
        else:
            pos += 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('img')
    ap.add_argument('names', nargs='*')
    ap.add_argument('--out', default='.')
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    d = open(a.img, 'rb').read()
    found = {}
    for name, size, addr in entries(d):
        if a.list:
            print(f'{addr:08x} {size:8d} {name}')
        found.setdefault(name.lower(), (name, size, addr))
    for want in a.names:
        hit = found.get(want.lower())
        if not hit or hit[2] == 0:
            print(f'{want}: not found', file=sys.stderr)
            continue
        name, size, addr = hit
        if d[addr + 16:addr + 20] != b'EPOC':
            print(f'{name}: no E32 header at {addr:#x}, carving anyway', file=sys.stderr)
        os.makedirs(a.out, exist_ok=True)
        out = os.path.join(a.out, name)
        open(out, 'wb').write(d[addr:addr + size])
        print(f'{out}: {size} bytes from {addr:#x}')


if __name__ == '__main__':
    main()
