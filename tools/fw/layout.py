#!/usr/bin/env python3
"""Flash-file layout, read from the files themselves (docs/device-model-v2.md, decision 5).

A profile says which files form the firmware (roles and globs); where the images are inside each file
is the file's business, and it differs between versions of one phone (N73 3.0717: ROFS1 after the
compressed core, a bare-stream language file; 4.0839: ROFS1 in its own SOS+ROFS partition, a SOS+ROFX
record). Rules [verified on every flash file held in roms/, 2026-11-10]:
  core      partition SOS+CORE (plain) or SOS*CORE (compressed: raw deflate after the record payload)
  ROFS1     SOS+ROFS1 / SOS+ROFS in the core's file; else, for a compressed core, right after the stream
  language  SOS+ROFS2 / SOS+ROFX; else a bare chunk stream (no record, '_pre') starting with ROFx/ROFS
  customer  SOS+ROFS3
  ignored   loaders, DSP/modem software (ADA PRIMAPP RAP3NAND SOS*ISASW ...), SOS*ENO, IVE3B, udaerase
Paging is read from the core's ROM header (iRomPageIndex at +336), not declared.

  layout.py FILE...        print what each file holds (no extraction)
"""
import os, struct, sys, mmap

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from fiasco import blocks   # tools/fiasco.py

CORE = ('SOS+CORE', 'SOS*CORE', 'SOS_CORE')
ROFS1 = ('SOS+ROFS1', 'SOS+ROFS', 'SOS_ROFS')
LANG = ('SOS+ROFS2', 'SOS+ROFX', 'SOS_ROFX')
CUST = ('SOS+ROFS3',)


def partitions(path):
    """{record name, '_pre' or '_devNN' (tools/fiasco.py tag 0x30): bytes of data chunks} of a FIASCO file"""
    out = {}
    with open(path, 'rb') as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        if m[:1] != b'\xb2':
            raise ValueError('%s: not a FIASCO container' % path)
        cur = None
        for p, tag, d, name, size, loc, q in blocks(m):
            if tag in (0x27, 0x28):
                cur = name
                out.setdefault(cur, 0)
            elif name:				# tag 0x30: a chunk of a device of its own ('_devNN')
                out[name] = out.get(name, 0) + size
            else:
                if cur is None:
                    cur = '_pre'
                    out.setdefault(cur, 0)
                out[cur] += size
    return out


def first_bytes(path, part, n=4):
    """the first n data bytes of a partition (bare streams: is it a ROFS?)"""
    with open(path, 'rb') as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        cur = None
        for p, tag, d, name, size, loc, q in blocks(m):
            if tag in (0x27, 0x28):
                cur = name
            elif (name or cur or '_pre') == part and size:
                return bytes(m[q:q + n])
    return b''


def plan(path):
    """what a flash file holds: {'core': (partition, compressed), 'rofs1': partition | 'after-core',
    'lang': partition, 'cust': partition} (only the keys present)"""
    parts = {k: v for k, v in partitions(path).items() if v}
    out = {}
    core = next((n for n in parts if n in CORE), None)
    if core:
        out['core'] = (core, '*' in core)
        r1 = next((n for n in parts if n in ROFS1), None)
        out['rofs1'] = r1 or ('after-core' if '*' in core else None)
    lang = next((n for n in parts if n in LANG), None)
    if not lang and '_pre' in parts and not core and first_bytes(path, '_pre') in (b'ROFx', b'ROFS'):
        lang = '_pre'
    if lang:
        out['lang'] = lang
    cust = next((n for n in parts if n in CUST), None)
    if cust:
        out['cust'] = cust
    return out


def rom_is_paged(img):
    """a demand-paged core: the TRomHeader (iRomBase 0x80000000 or 0xF8000000 at +140, 256-byte aligned in
    the first 64 KB of the partition) has a page index (+336) and a pageable size (+332)"""
    for off in range(0, min(len(img), 0x10000), 0x100):
        base, size, root = struct.unpack_from('<III', img, off + 140)
        if base in (0x80000000, 0xF8000000) and 0x100000 < size < 0x10000000 and base < root < base + size:
            pidx = struct.unpack_from('<I', img, off + 336)[0]
            psize = struct.unpack_from('<I', img, off + 332)[0]
            return bool(pidx and psize)
    raise ValueError('no TRomHeader in the core partition')


def main():
    for p in sys.argv[1:]:
        try:
            print('%-60s %s' % (os.path.basename(p), plan(p)))
        except ValueError as e:
            print('%-60s -- %s' % (os.path.basename(p), e))


if __name__ == '__main__':
    main()
