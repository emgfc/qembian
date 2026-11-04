#!/usr/bin/env python3
"""Expand a demand-paged EKA2 core ROM image into a flat XIP image.

The pageable part of a paged ROM is stored as per-4K-page byte-pair compressed blobs, indexed by
SRomPageInfo{u32 iDataStart; u16 iDataSize; u8 iCompressionType; u8 iPagingAttributes} at
TRomHeader.iRomPageIndex (header offset 336). The decompressor is a port of
kernel/eka/kernel/byte_pair.cpp (BytePairDecompress).

  rom_unpage.py IN OUT      IN may be a raw partition dump; the TRomHeader is located
                            automatically (iRomBase 0x80000000 at +140, 256-byte aligned).
"""
import struct, sys

PAGE = 0x1000


def bytepair(src, dst_size=PAGE):
    lut0 = list(range(256))
    lut1 = [0] * 256
    marker = -1
    i = 0
    ntok = src[i]; i += 1
    if ntok:
        marker = src[i]; i += 1
        lut0[marker] = (~marker) & 0xFF
        if ntok < 32:
            for _ in range(ntok):
                b, p1, p2 = src[i], src[i + 1], src[i + 2]; i += 3
                lut0[b] = p1; lut1[b] = p2
        else:
            mask = src[i:i + 32]; i += 32
            for b in range(256):
                if mask[b >> 3] & (1 << (b & 7)):
                    lut0[b] = src[i]; lut1[b] = src[i + 1]; i += 2
    out = bytearray()
    n = len(src)
    while i < n and len(out) < dst_size:
        b = src[i]; i += 1
        if b == marker:
            out.append(src[i]); i += 1
            continue
        stack = [b]
        while stack:
            if len(out) > dst_size or len(stack) > 0x100:
                raise ValueError('corrupt byte-pair data (overrun)')
            c = stack.pop()
            if lut0[c] == c:
                out.append(c)
            else:
                stack.append(lut1[c])
                stack.append(lut0[c])
    if len(out) > dst_size:
        raise ValueError('overrun')
    return bytes(out)


def find_header(d):
    for off in range(0, min(len(d), 0x10000), 0x100):
        base, size, root = struct.unpack_from('<III', d, off + 140)
        if base == 0x80000000 and 0x100000 < size < 0x10000000 and base < root < base + size:
            return off
    raise SystemExit('no TRomHeader found')


def main():
    src = open(sys.argv[1], 'rb').read()
    hoff = find_header(src)
    src = src[hoff:]
    print('TRomHeader at partition offset 0x%x' % hoff)
    pidx = struct.unpack_from('<I', src, 336)[0]
    uncomp = struct.unpack_from('<I', src, 244)[0]
    pstart, psize = struct.unpack_from('<II', src, 328)
    npages = (uncomp + PAGE - 1) // PAGE
    print('pageIndex=0x%x pages=%d pageableStart=0x%x pageableSize=0x%x' % (pidx, npages, pstart, psize))
    out = bytearray(npages * PAGE)
    stats = {}
    for p in range(npages):
        ds, dz, ct, at = struct.unpack_from('<IHBB', src, pidx + 8 * p)
        stats[(ct, at)] = stats.get((ct, at), 0) + 1
        blob = src[ds:ds + dz]
        if ct == 0 or dz == PAGE:
            # unpaged-region pages carry ct=1 but are stored raw (4096 bytes)
            page = blob
        elif ct == 1:
            page = bytepair(blob)
        else:
            raise ValueError('page %d compression %d' % (p, ct))
        out[p * PAGE:p * PAGE + len(page)] = page
    print('page (compression, attrs) counts:', stats)
    open(sys.argv[2], 'wb').write(out)


if __name__ == '__main__':
    main()
