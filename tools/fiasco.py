#!/usr/bin/env python3
"""Nokia BB5 firmware container (.C0R / .v13 / .fpsx, "B2" FIASCO) splitter.

Layout (reverse-engineered on RM-320 35.0.001; see docs/devices/nokia-n95-8gb-rm320/n95-firmware-inventory.md):
  B2 <u32be hdrlen> ... header ...  then a chain of blocks:
  <marker 0x54|0x5D> <nsub=1> <tag> <len> <desc[len]> <ck8> <data[size]>
    tag 0x27 (marker 0x5D, len 45): partition record
        desc = sha1[20] name[12] ck32[1+4]... size u32be @37, location u32be @41
    tag 0x28 (marker 0x5D, len 67): same first 45 bytes as 0x27 (key/cert records)
    tag 0x17 (marker 0x54, len 14): data chunk
        desc[6:10] = size u32be, desc[10:14] = flash location u32be
    tag 0x2E (fpsx UDA, len 25): size u32be @17, location u32be @21.
    tag 0x30 (marker 0x54, len 21; E71 RM-346 .v14 / .01U, 2008): data chunk of a block device of its
        own (eMMC/mass memory image, location from 0 on that device, not flash): desc[8] = device
        number (08 .01U user data, 09 the .v14's), desc[13:17] = size u32be, desc[17:21] = location.
        Yielded with name '_dev%02x' (desc[8]): its chunks form that pseudo partition, not the
        preceding record's.
Chunks following a partition record belong to that partition (tag 0x30 chunks: their '_devNN').

Usage: fiasco.py FILE [--out DIR]   (writes DIR/<partition>@<lo>.bin, location-ordered, gaps 0xFF,
                                      and DIR/<partition>@<rec_loc>.hdr: the record's payload)
"""
import argparse, mmap, os, struct, sys


def blocks(m):
    p = struct.unpack('>I', m[1:5])[0] + 5
    # header may be followed by padding up to the first marker
    while p < len(m) and m[p] not in (0x54, 0x5D):
        p += 1
    while p < len(m):
        marker, nsub = m[p], m[p + 1]
        if marker not in (0x54, 0x5D) or nsub != 1:
            raise ValueError('desync at 0x%x: %s' % (p, m[p:p + 16].hex()))
        tag, ln = m[p + 2], m[p + 3]
        d = bytes(m[p + 4:p + 4 + ln])
        q = p + 4 + ln + 1
        if tag in (0x27, 0x28):
            size, loc = struct.unpack('>II', d[37:45])
            name = d[20:32].rstrip(b'\0').decode('latin1')
        elif tag == 0x17:
            size, loc = struct.unpack('>II', d[6:14])
            name = None
        elif tag == 0x2E:
            size, loc = struct.unpack('>II', d[17:25])
            name = None
        elif tag == 0x30 and ln >= 21:
            size, loc = struct.unpack('>II', d[13:21])
            name = '_dev%02x' % d[8]
        else:
            raise ValueError('unknown tag 0x%x at 0x%x' % (tag, p))
        yield p, tag, d, name, size, loc, q
        p = q + size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--out')
    a = ap.parse_args()
    f = open(a.file, 'rb')
    m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    parts = []  # [name, loc, size, [(loc, off, size)]]
    cur = None
    for p, tag, d, name, size, loc, q in blocks(m):
        if tag in (0x27, 0x28):
            cur = [name, loc, size, []]
            parts.append(cur)
            # a 0x27 record's own payload is a signature/header blob
            cur.append((q, size))
        elif name:					# a chunk of its own device (tag 0x30)
            dev = next((x for x in parts if x[0] == name), None)
            if dev is None:
                dev = [name, 0, 0, [], None]
                parts.append(dev)
            dev[3].append((loc, q, size))
        else:
            if cur is None:
                cur = ['_pre', 0, 0, [], None]
                parts.append(cur)
            cur[3].append((loc, q, size))
    for name, loc, size, chunks, *rest in parts:
        tot = sum(c[2] for c in chunks)
        lo = min((c[0] for c in chunks), default=0)
        hi = max((c[0] + c[2] for c in chunks), default=0)
        print('%-12s rec_loc=0x%08x rec_size=0x%x chunks=%4d bytes=%9d span=0x%08x-0x%08x'
              % (name, loc, size, len(chunks), tot, lo, hi))
        if a.out and chunks:
            os.makedirs(a.out, exist_ok=True)
            buf = bytearray(b'\xff' * (hi - lo))
            for cl, off, sz in chunks:
                buf[cl - lo:cl - lo + sz] = m[off:off + sz]
            stem = name.replace('+', '_').replace('/', '_')
            fn = os.path.join(a.out, '%s@%08x.bin' % (stem, lo))
            open(fn, 'wb').write(buf)
            # the record's own payload is the partition image header, flashed at rec_loc just before
            # the chunks (compressed SOS* partitions start their data inside it: tools/fw/inventory.py)
            if rest and rest[0] and rest[0][1]:
                q, sz = rest[0]
                open(os.path.join(a.out, '%s@%08x.hdr' % (stem, loc)), 'wb').write(m[q:q + sz])


if __name__ == '__main__':
    main()
