#!/usr/bin/env python3
"""Set 32-bit cell properties in a flattened device tree (DTB) in place.

  dtb_set.py IN.dtb OUT.dtb NODE-SUBSTRING:PROP=VALUE ...
  e.g. dtb_set.py syborg.dtb syborg-n95.dtb framebuffer:width=240 framebuffer:height=320

Only properties that already exist with a 4-byte value are changed (no re-layout needed).
"""
import struct, sys

FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def main():
    src, dst, *specs = sys.argv[1:]
    d = bytearray(open(src, 'rb').read())
    magic, total, off_struct, off_strings = struct.unpack_from('>IIII', d, 0)
    if magic != 0xd00dfeed:
        raise SystemExit('not a DTB')
    want = []
    for sp in specs:
        node, rest = sp.split(':', 1)
        prop, val = rest.split('=', 1)
        want.append((node, prop, int(val, 0)))
    p = off_struct
    path = []
    done = set()
    while True:
        tok = struct.unpack_from('>I', d, p)[0]; p += 4
        if tok == FDT_BEGIN_NODE:
            e = d.index(b'\0', p); path.append(d[p:e].decode()); p = (e + 4) & ~3
        elif tok == FDT_END_NODE:
            path.pop()
        elif tok == FDT_PROP:
            ln, noff = struct.unpack_from('>II', d, p); p += 8
            name = d[off_strings + noff:d.index(b'\0', off_strings + noff)].decode()
            for i, (node, prop, val) in enumerate(want):
                if path and node in path[-1] and prop == name and ln == 4:
                    old = struct.unpack_from('>I', d, p)[0]
                    struct.pack_into('>I', d, p, val)
                    print('%s/%s: %d -> %d' % ('/'.join(path), name, old, val))
                    done.add(i)
            p = (p + ln + 3) & ~3
        elif tok == FDT_NOP:
            pass
        elif tok == FDT_END:
            break
        else:
            raise SystemExit('bad token %d' % tok)
    missing = [specs[i] for i in range(len(want)) if i not in done]
    if missing:
        raise SystemExit('not found: %s' % ', '.join(missing))
    open(dst, 'wb').write(d)


if __name__ == '__main__':
    main()
