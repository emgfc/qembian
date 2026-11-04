#!/usr/bin/env python3
"""Compose a hybrid XIP ROM: our kernel side + the N95 user side at its original addresses.

Layout of the result (romBase 0x80000000 in both inputs):
  [base, ustart)      bytes of OUR rom image (TRomHeader, bootstrap, our kernel-side files;
                      our user-side files in this window are dead bytes and not listed)
  [ustart, n95end)    bytes of the N95 flat core image, verbatim (N95 user side stays
                      byte-identical, so every import thunk and DLL ref stays valid)
  [n95end, ...)       new root directory tree + exception search table
then TRomHeader fields are patched (size, root dir, secondary file = N95 efile.exe,
exception search table, paging/compression off).

  romcompose.py --ours OUR.img --n95 core-flat.img -o hybrid.img [--manifest OUT.txt]
               [--keep-ours NAME ...]   also list these of our user-side files (e.g. test tools)
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import romimg  # noqa: E402

B = 0x80000000
EKERN_NAMES = ('ekern.exe',)
KMASK = 0xF0000000
LDR_PC = 0xE51FF004


def load(path):
    d = open(path, 'rb').read()
    rom = romimg.Rom(d)
    files = list(rom.walk())
    for f in files:
        if romimg.is_image(rom, f):
            f['image'] = rom.image(f['addr'])
    return d, rom, files


def kernel_side(d, files):
    """Kernel side = primary/variant/extension/device flag, or imports from EKERN."""
    ek = [f for f in files if f['path'].lower().endswith(EKERN_NAMES) and 'image' in f]
    ekr = (ek[0]['image']['code'], ek[0]['image']['code'] + ek[0]['image']['codesize']) if ek else (0, 0)
    out = set()
    for f in files:
        im = f.get('image')
        if not im:
            continue
        if im['flags'] & KMASK:
            out.add(f['path'])
            continue
        n = im['codesize']
        ws = struct.unpack_from('<%dI' % (n // 4), d, im['code'] - B)
        if any(ws[i] == LDR_PC and ekr[0] <= ws[i + 1] < ekr[1] for i in range(len(ws) - 1)):
            out.add(f['path'])
    return out


def sort_key(name):
    # rombuild: stricmp on UTF-8 (fold ASCII only)
    return bytes(c + 32 if 65 <= c <= 90 else c for c in name.encode('utf-8'))


class DirNode:
    def __init__(self):
        self.dirs = {}   # lower name -> (name, DirNode)
        self.files = {}  # lower name -> (name, addr, size, att)


def build_tree(entries):
    root = DirNode()
    for path, addr, size, att in entries:
        parts = path.split('/')
        n = root
        for p in parts[:-1]:
            k = p.lower()
            if k not in n.dirs:
                n.dirs[k] = (p, DirNode())
            n = n.dirs[k][1]
        n.files[parts[-1].lower()] = (parts[-1], addr, size, att)
    return root


def emit_dirs(root, at):
    """Lay out TRomDir structures from address `at`; returns (bytes, root_addr, {path: entry_addr})."""
    out = bytearray()
    entry_addr = {}

    def dir_size(node):
        items = [n for n, _ in node.dirs.values()] + [v[0] for v in node.files.values()]
        es = sum((10 + 2 * len(nm) + 3) & ~3 for nm in items)
        return 4 + es + 4 + 2 * len(items), es

    order = []

    def plan(node, path):
        order.append((node, path))
        for k in sorted(node.dirs, key=lambda k: sort_key(node.dirs[k][0])):
            nm, sub = node.dirs[k]
            plan(sub, path + nm + '/')
    plan(root, '')
    addr = {}
    a = at
    for node, path in order:
        addr[id(node)] = a
        a += (dir_size(node)[0] + 3) & ~3
    for node, path in order:
        base = addr[id(node)]
        total, es = dir_size(node)
        buf = bytearray(struct.pack('<I', es))
        dir_items = [(node.dirs[k][0], 0, addr[id(node.dirs[k][1])], 0x11) for k in
                     sorted(node.dirs, key=lambda k: sort_key(node.dirs[k][0]))]
        file_items = [(v[0], v[2], v[1], v[3]) for k, v in
                      sorted(node.files.items(), key=lambda kv: sort_key(kv[1][0]))]
        offs = []
        for nm, size, ealin, att in dir_items + file_items:
            offs.append((len(buf) - 4) // 4)
            if not att & 0x10:
                entry_addr[path + nm] = base + len(buf)
            e = struct.pack('<IIBB', size, ealin, att, len(nm)) + nm.encode('utf-16le')
            e += b'\0' * (((len(e) + 3) & ~3) - len(e))
            buf += e
        buf += struct.pack('<HH', len(dir_items), len(file_items))
        buf += struct.pack('<%dH' % len(offs), *offs)
        buf += b'\0' * (((len(buf) + 3) & ~3) - len(buf))
        assert base - at == len(out), (hex(base), hex(at + len(out)))
        out += buf
    return bytes(out), addr[id(root)], entry_addr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ours', required=True)
    ap.add_argument('--n95', required=True)
    ap.add_argument('-o', '--out', required=True)
    ap.add_argument('--manifest')
    ap.add_argument('--keep-ours', action='append', default=[])
    ap.add_argument('--secondary', default='sys/bin/efile.exe')
    ap.add_argument('--file', action='append', default=[], metavar='ROMPATH=LOCAL',
                    help='add or replace a ROM file (data only), e.g. Sys/Data/ESTARTCOMP.TXT=rom/hybrid/ESTARTCOMP.TXT')
    ap.add_argument('--rofs', action='append', default=[], metavar='IMG',
                    help='ROFS image(s) for the N95 ROFS media (MEDROFS.PDD): first ROFS, then ROFx extension')
    ap.add_argument('--exec92', action='store_true', help='set the 9.2 exec personality flag (kernel config bit 29)')
    ap.add_argument('--trace', type=lambda x: int(x, 0), help='kernel trace mask word 0 (nk_trace.h bits)')
    a = ap.parse_args()

    od, orom, ofiles = load(a.ours)
    nd, nrom, nfiles = load(a.n95)
    ok = kernel_side(od, ofiles)
    nk = kernel_side(nd, nfiles)
    nuser = [f for f in nfiles if f['path'] not in nk]
    ustart = min(f['addr'] for f in nuser)
    ours_listed = [f for f in ofiles if f['path'] in ok or f['path'].split('/')[-1].lower() in
                   [k.lower() for k in a.keep_ours]]
    oend = max(f['addr'] + f['size'] for f in ours_listed)
    if oend > ustart:
        raise SystemExit('our kernel side ends at 0x%08x, beyond the N95 user window start 0x%08x' % (oend, ustart))
    n95end = len(nd) + B
    print('ours: %d kernel-side files end 0x%08x; N95: dropping %d kernel-side, keeping %d user files from 0x%08x'
          % (len(ours_listed), oend, len(nk), len(nuser), ustart))

    out = bytearray(od[:ustart - B].ljust(ustart - B, b'\0'))
    out += nd[ustart - B:]
    assert len(out) == n95end - B

    # directory tree: N95 user side, then ours (ours win on name clash, reported)
    entries = {}
    clash = []
    for f in nuser:
        entries[f['path'].lower()] = (f['path'], f['addr'], f['size'], f['att'])
    for f in ours_listed:
        k = f['path'].lower()
        if k in entries:
            clash.append(f['path'])
        entries[k] = (f['path'], f['addr'], f['size'], f['att'])
    if clash:
        print('name clashes (ours used): %s' % ', '.join(clash))
    # file overrides: payload placed after the N95 image, before the directory
    for spec in a.file:
        rp, local = spec.split('=', 1)
        data = open(local, 'rb').read()
        at = (B + len(out) + 3) & ~3
        out += b'\0' * (at - B - len(out)) + data
        old = entries.get(rp.lower())
        entries[rp.lower()] = (old[0] if old else rp, at, len(data), old[3] if old else 0x01)
        print('override %s <- %s (%d bytes @0x%08x)%s' % (rp, local, len(data), at, '' if old else ' [new]'))
    tree = build_tree(entries.values())
    dir_at = (B + len(out) + 0xFFF) & ~0xFFF
    out += b'\0' * (dir_at - B - len(out))
    dbytes, root_addr, eaddr = emit_dirs(tree, dir_at)
    out += dbytes
    rootlist_at = B + len(out)
    out += struct.pack('<III', 1, struct.unpack_from('<I', od, orom.rootlist - B + 4)[0], root_addr)

    # exception search table: N95 entries in the user window + our entries, sorted, + fencepost
    def est(d, rom):
        a = struct.unpack_from('<I', d, 188)[0]
        if not a:
            return []
        n = struct.unpack_from('<i', d, a - B)[0]
        return list(struct.unpack_from('<%dI' % (n + 1), d, a - B + 4))
    ne = est(nd, nrom)
    oe = est(od, orom)
    ents = sorted(set([x for x in ne[:-1] if ustart <= x < n95end] +
                      [x for x in oe[:-1] if any(f['image']['code'] == x for f in ours_listed if 'image' in f)]))
    fence = max(ne[-1] if ne else 0, n95end)
    est_at = B + len(out)
    out += struct.pack('<i', len(ents)) + struct.pack('<%dI' % (len(ents) + 1), *(ents + [fence]))
    # ROFS partition blob for MEDROFS.PDD: ROFS1 at 0, each following ROFx at the previous
    # image's iMaxImageSize (as on the N95 flash), then a 16-byte trailer at the end of the ROM
    rofs_at = rofs_len = 0
    if a.rofs:
        blob = bytearray()
        for k, path in enumerate(a.rofs):
            img = open(path, 'rb').read()
            if img[:4] not in (b'ROFS', b'ROFx'):
                raise SystemExit('%s: not a ROFS image' % path)
            blob += img
            if k + 1 < len(a.rofs):
                mx = struct.unpack_from('<I', img, 44)[0]
                if mx < len(img):
                    raise SystemExit('%s: iMaxImageSize smaller than image' % path)
                blob += b'\xff' * (mx - len(img))
        rofs_at = (B + len(out) + 0xFFFF) & ~0xFFFF
        out += b'\0' * (rofs_at - B - len(out)) + blob
        rofs_len = len(blob)
        print('ROFS media blob: %d image(s), 0x%x bytes @0x%08x' % (len(a.rofs), rofs_len, rofs_at))
    end = (B + len(out) + 16 + 0xFFF) & ~0xFFF
    out += b'\0' * (end - B - len(out))
    if a.rofs:
        struct.pack_into('<8sII', out, len(out) - 16, b'N95ROFS\0', rofs_at, rofs_len)
    size = len(out)

    # patch TRomHeader
    sec = [p for p in eaddr if p.lower() == a.secondary.lower()]
    if not sec:
        raise SystemExit('secondary %s not found' % a.secondary)
    def put(off, v):
        struct.pack_into('<I', out, off, v)
    put(144, size)                 # iRomSize
    put(148, rootlist_at)          # iRomRootDirectoryList
    put(164, eaddr[sec[0]])        # iSecondaryFile (TRomEntry)
    put(188, est_at)               # iRomExceptionSearchTable
    put(236, 0); put(240, size); put(244, size)   # compression off, sizes
    put(328, 0); put(332, 0); put(336, 0)         # no paging
    if a.exec92:
        put(184, struct.unpack_from('<I', out, 184)[0] | (1 << 29))   # iKernelConfigFlags
    if a.trace is not None:
        put(256, a.trace)                         # iTraceMask[0]
    # primary/variant/extension TRomEntry pointers (and the iNextExtension chain) refer to our
    # original directory, which is still intact inside our window, so they stay valid as-is.
    open(a.out, 'wb').write(out)
    print('wrote %s: 0x%x bytes, root dir 0x%08x, secondary %s @0x%08x, EST %d entries'
          % (a.out, size, rootlist_at, sec[0], eaddr[sec[0]], len(ents)))
    if a.manifest:
        with open(a.manifest, 'w') as m:
            for k, (p, ad, sz, att) in sorted(entries.items(), key=lambda kv: kv[1][1]):
                m.write('%08x %9d %02x %s %s\n' % (ad, sz, att, 'OURS' if ad < ustart else 'N95 ', p))


if __name__ == '__main__':
    main()
