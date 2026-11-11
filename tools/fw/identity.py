#!/usr/bin/env python3
"""What a firmware says about itself, read straight from its flash files (docs/device-model-v2.md 2.2.1).

  version_file(text)            Nokia sw.txt / langsw.txt: 'V 35.0.001\\n11-11-09\\nRM-320\\nN95(c)NMP'
  assemble(path, part)          (record payload, data) of one FIASCO partition, in memory
  inflate_sos(hdr, body)        a compressed SOS* partition (N73 SOS*CORE) -> (ROM image, bytes after)
  rofs_versions(img)            {'sw': {...}, 'langsw': {...}} from a ROFS image's Resource/Versions
"""
import mmap, os, struct, sys, zlib

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOLS)
from fiasco import blocks   # tools/fiasco.py
import rofs as rofsmod      # tools/rofs.py


def version_file(text):
    """Nokia version files (sw.txt, langsw.txt, customersw.txt): UTF-16, fields one per line"""
    text = text.lstrip('﻿').replace('\\n', '\n').replace('\r', '')
    f = [x.strip() for x in text.split('\n')]
    return {'version': f[0] if f else '', 'date': f[1] if len(f) > 1 else '',
            'type_code': f[2] if len(f) > 2 else '', 'product': f[3] if len(f) > 3 else ''}


def assemble(path, part):
    """one partition of a FIASCO file: (the record's own payload, the chunks laid out by flash location,
    gaps 0xFF) as tools/fiasco.py --out writes them; part '_pre' = chunks before any record"""
    with open(path, 'rb') as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        cur, hdr, chunks = None, b'', []
        for p, tag, d, name, size, loc, q in blocks(m):
            if tag in (0x27, 0x28):
                if cur == part and chunks:
                    break
                cur = name
                if name == part:
                    hdr, chunks = bytes(m[q:q + size]), []
            elif (name or cur or '_pre') == part:		# name: a tag 0x30 chunk's '_devNN'
                chunks.append((loc, q, size))
        if not chunks:
            return hdr, b''
        lo = min(c[0] for c in chunks)
        hi = max(c[0] + c[2] for c in chunks)
        buf = bytearray(b'\xff' * (hi - lo))
        for cl, off, sz in chunks:
            buf[cl - lo:cl - lo + sz] = m[off:off + sz]
        return hdr, bytes(buf)


def inflate_sos(hdr, body):
    """A compressed SOS* partition: the record payload (flashed just before the chunks) holds the length of
    a raw prefix at +0x1C; the raw deflate stream starts after it and runs into the chunks. Returns (ROM
    image from the end of the record payload, the bytes after the stream: the N73's ROFS1)."""
    if hdr[:4] != b'\xa3\x95\x97\x80':
        raise ValueError('unexpected SOS header')
    raw = struct.unpack_from('<I', hdr, 0x1c)[0]
    d = hdr + body
    z = zlib.decompressobj(-15)
    out = z.decompress(d[raw:]) + z.flush()
    if not z.eof:
        raise ValueError('deflate stream does not end')
    img = out[len(hdr) - raw:]
    # the TRomHeader opens the image (N73) or follows a RAPIDO NAND prefix (N81 8GB RM-179, E71 RM-346:
    # 0xC00 of zeros): the image is cut to start at it (256-byte aligned, first 64 KB)
    for off in range(0, min(len(img) - 0x90, 0x10000), 0x100):
        if struct.unpack_from('<I', img, off + 0x8c)[0] in (0x80000000, 0xF8000000):
            return img[off:], z.unused_data
    base = struct.unpack_from('<I', img, 0x8c)[0]
    raise ValueError('no ROM header at the expected place (iRomBase 0x%08x)' % base)


def rofs_slice(data):
    """the first ROFS/ROFx image in data (sector aligned), cut to its iImageSize (RAPIDO NAND: 0xC00 prefix)"""
    off = 0
    while True:
        cand = [i for i in (data.find(b'ROFS', off), data.find(b'ROFx', off)) if i >= 0]
        if not cand:
            return None
        off = min(cand)
        size = struct.unpack_from('<I', data, off + 0x24)[0]
        if 0 < size <= len(data) - off:
            return data[off:off + size]
        off += 4


def rofs_versions(img):
    """the version files a ROFS image holds itself (a ROFx lists the earlier ROFS's files too: skipped)"""
    out = {}
    if not img:
        return out
    base, entries = rofsmod.files(img)
    for off, n, path in entries:
        off -= base
        if off < 0 or off + n > len(img):
            continue
        p = path.lower()
        for key in ('sw', 'langsw', 'customersw'):
            if p == 'resource/versions/%s.txt' % key:
                out[key] = version_file(img[off:off + n].decode('utf-16-le', 'replace'))
    return out
