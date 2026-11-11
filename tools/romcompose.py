#!/usr/bin/env python3
"""Compose a hybrid XIP ROM: our kernel side + the N95 user side at its original addresses.

Layout of the result (both inputs linked at the same romBase, taken from their headers: 0x80000000
for multiple-model ROMs such as the N95's, 0xF8000000 for moving-model ones such as the N73's):
  [base, ustart)      bytes of OUR rom image (TRomHeader, bootstrap, our kernel-side files;
                      our user-side files in this window are dead bytes and not listed)
  [ustart, n95end)    bytes of the N95 flat core image, verbatim (N95 user side stays
                      byte-identical, so every import thunk and DLL ref stays valid)
  [n95end, ...)       new root directory tree + exception search table
then TRomHeader fields are patched (size, root dir, secondary file = N95 efile.exe,
exception search table, paging/compression off).

  romcompose.py --ours OUR.img --n95 core-flat.img -o hybrid.img [--manifest OUT.txt]
               [--keep-ours NAME ...]   also list these of our user-side files (e.g. test tools)
               [--patches FILE ...]     byte patches (repeatable)
               [--blob ADDR=FILE ...]   raw prelinked code (e.g. build/n95gl/sygl.bin) placed at ADDR
                                        in the free gap of our window, between the end of our listed
                                        files and the N95 user side (holds only our dead user-side
                                        files); listed nowhere, reached through code patches
"""
import argparse, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import romimg  # noqa: E402

B = 0x80000000          # ROM linear base; set from the inputs' TRomHeader in main()
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
        if not (0 <= im['code'] - B and im['code'] - B + n <= len(d)):
            continue			# not an executable image (bad header), never kernel side
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
                    help='add or replace a ROM file (data only), e.g. Sys/Data/ESTARTCOMP.TXT=models/<id>/ESTARTCOMP.TXT')
    ap.add_argument('--rofs', action='append', default=[], metavar='IMG',
                    help='ROFS image(s) for the N95 ROFS media (MEDROFS.PDD): first ROFS, then ROFx extension')
    ap.add_argument('--rofs-drives', help='comma list of local drives, one per --rofs image (header version 2, '
                    'e.g. 9,10,11 for a composite over ROFS1/2/3); default: one blob on drive 8 (ROFS1 + ROFx)')
    ap.add_argument('--exec92', action='store_true', help='= --exec-personality sym92 (the original flag, bit 29)')
    ap.add_argument('--exec-personality', help='personality NAME or number from personalities/registry.toml: '
                    'kernel config bits 25..28 (+ bit 29 for sym92)')
    ap.add_argument('--trace', type=lambda x: int(x, 0), help='kernel trace mask word 0 (nk_trace.h bits)')
    ap.add_argument('--flash-mb', type=int, default=128, help='size of the board flash window <rom>.rofs must fit (board.flash.size_mb)')
    ap.add_argument('--patches', metavar='FILE', action='append', default=[],
                    help='code patch list: "ADDR ORIGHEX NEWHEX  # why" per line; original bytes are verified')
    ap.add_argument('--blob', metavar='ADDR=FILE', action='append', default=[],
                    help='raw code placed at ADDR in the gap between our listed files and the N95 user side')
    a = ap.parse_args()

    global B
    B = struct.unpack_from('<I', open(a.ours, 'rb').read(0x100), 0x8c)[0]   # TRomHeader.iRomBase
    od, orom, ofiles = load(a.ours)
    nd, nrom, nfiles = load(a.n95)
    if orom.base != nrom.base:
        raise SystemExit('ROM bases differ: ours 0x%08x (%s), firmware 0x%08x (%s): build our kernel side for the '
                         "firmware's memory model (kernel.baseport in the device profile)" % (orom.base, a.ours, nrom.base, a.n95))
    ok = kernel_side(od, ofiles)
    nk = kernel_side(nd, nfiles)
    nuser = [f for f in nfiles if f['path'] not in nk]
    ustart = min(f['addr'] for f in nuser)
    ours_listed = [f for f in ofiles if f['path'] in ok or f['path'].split('/')[-1].lower() in
                   [k.lower() for k in a.keep_ours]]
    n95end = len(nd) + B
    # Our kernel-side files sit either below the firmware's user side (N95: the firmware's own
    # kernel side was one block at the bottom) or after the whole firmware image (N73: its ROM
    # interleaves kernel- and user-side files, so our build pads its obey file past the firmware,
    # scripts/build-standins.sh). Our header, bootstrap and directory are always below ustart.
    low = [f for f in ours_listed if f['addr'] + f['size'] <= ustart]
    high = [f for f in ours_listed if f['addr'] >= n95end]
    bad = [f for f in ours_listed if f not in low and f not in high]
    if bad:
        raise SystemExit('our kernel-side files overlap the firmware image (user side from 0x%08x to 0x%08x): %s; '
                         'pad our kernel side past the firmware (kernel.place = "after-firmware")'
                         % (ustart, n95end, ', '.join('%s@0x%08x' % (f['path'], f['addr']) for f in bad[:4])))
    if orom.rootlist >= ustart:
        raise SystemExit('our ROM directory (0x%08x) is not below the firmware user side (0x%08x)' % (orom.rootlist, ustart))
    oend = max([f['addr'] + f['size'] for f in low] + [orom.rootlist])
    print('ours: %d kernel-side files below 0x%08x, %d after the firmware (from 0x%08x); firmware: dropping %d '
          'kernel-side, keeping %d user files from 0x%08x' % (len(low), ustart, len(high), n95end, len(nk), len(nuser), ustart))

    out = bytearray(od[:ustart - B].ljust(ustart - B, b'\0'))
    out += nd[ustart - B:]
    assert len(out) == n95end - B
    if high:
        # all of our image after the firmware, not just the listed extents: rombuild puts each
        # image's DLL reference table (and some export directories) just past the extent the
        # directory lists, and the kernel follows those pointers
        out += od[n95end - B:]
    for spec in a.blob:
        at, path = spec.split('=', 1)
        at = int(at, 0)
        blob = open(path, 'rb').read()
        if at < oend or at + len(blob) > ustart:
            raise SystemExit('blob %s (0x%08x+0x%x) outside the free gap 0x%08x..0x%08x'
                             % (path, at, len(blob), oend, ustart))
        out[at - B:at - B + len(blob)] = blob
        print('blob %s: 0x%x bytes at 0x%08x (gap 0x%08x..0x%08x)' % (path, len(blob), at, oend, ustart))

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
    ucode = {f['image']['code'] for f in nuser if 'image' in f}
    ents = sorted(set([x for x in ne[:-1] if ustart <= x < n95end and x in ucode] +
                      [x for x in oe[:-1] if any(f['image']['code'] == x for f in ours_listed if 'image' in f)]))
    fence = max(ne[-1] if ne else 0, n95end, (B + len(od)) if high else 0)
    est_at = B + len(out)
    out += struct.pack('<i', len(ents)) + struct.pack('<%dI' % (len(ents) + 1), *(ents + [fence]))
    # ROFS partition blob for MEDROFS.PDD: ROFS1 at 0, each following ROFx at the previous
    # image's iMaxImageSize (as on the N95 flash). It goes to the flash image <output>.rofs, not
    # into the ROM (RAM): a 4 KB header page { "SYBROFS\0", size } and the blob at +0x1000, loaded
    # by QEMU at the flash window 0x60000000 (scripts/run-modern.sh)
    rofs_at = rofs_len = 0
    if a.rofs and a.rofs_drives:
        drives = [int(x, 0) for x in a.rofs_drives.split(',')]
        if len(drives) != len(a.rofs):
            raise SystemExit('--rofs-drives: %d drives for %d images' % (len(drives), len(a.rofs)))
        blob, table = bytearray(), []
        for drv, path in zip(drives, a.rofs):
            img = open(path, 'rb').read()
            if img[:4] not in (b'ROFS', b'ROFx'):
                raise SystemExit('%s: not a ROFS image' % path)
            table.append((drv, len(blob), len(img)))
            blob += img + b'\xff' * ((-len(img)) % 0x1000)
        rofs_len = len(blob)
        if rofs_len + 0x1000 > a.flash_mb << 20:
            raise SystemExit('ROFS blob 0x%x does not fit the %d MB flash window (board.flash.size_mb)' % (rofs_len, a.flash_mb))
        hdr = bytearray(0x1000)
        struct.pack_into('<8sIII', hdr, 0, b'SYBROFS\0', rofs_len, 2, len(table))
        for i, t in enumerate(table):
            struct.pack_into('<III', hdr, 20 + 12 * i, *t)
        with open(a.out + '.rofs', 'wb') as f:
            f.write(hdr + blob)
        print('ROFS flash image (v2): %s -> %s.rofs' % (', '.join('drive %d: 0x%x bytes' % (t[0], t[2]) for t in table), a.out))
    elif a.rofs:
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
        rofs_len = len(blob)
        if rofs_len + 0x1000 > a.flash_mb << 20:
            raise SystemExit('ROFS blob 0x%x does not fit the %d MB flash window (board.flash.size_mb)' % (rofs_len, a.flash_mb))
        hdr = bytearray(0x1000)
        struct.pack_into('<8sI', hdr, 0, b'SYBROFS\0', rofs_len)
        with open(a.out + '.rofs', 'wb') as f:
            f.write(hdr + blob)
        print('ROFS flash image: %d image(s), 0x%x bytes -> %s.rofs' % (len(a.rofs), rofs_len, a.out))
    end = (B + len(out) + 0xFFF) & ~0xFFF
    out += b'\0' * (end - B - len(out))
    size = len(out)

    # patch TRomHeader
    sec = [p for p in eaddr if p.lower() == a.secondary.lower()]
    if not sec:
        raise SystemExit('secondary %s not found' % a.secondary)
    def put(off, v):
        struct.pack_into('<I', out, off, v)
    put(144, size)                 # iRomSize
    # moving memory model: "fixed" processes of the firmware have their data in the kernel section,
    # between iKernDataAddress and iKernelLimit (N73: efile, c32exe, FbServ, EwSrv, AknIconSrv at
    # 0x651..0x65B00000); the kernel puts its own mappings above iKernelLimit, so it must be the firmware's
    fixed = [f for f in nuser if 'image' in f and nrom.kerndata <= f['image']['databsslin'] < nrom.kernlimit]
    if fixed and nrom.kernlimit > orom.kernlimit:
        put(156, nrom.kernlimit)   # iKernelLimit
        print('kernel limit 0x%08x -> 0x%08x (the firmware\'s: %d fixed processes: %s)' % (
            orom.kernlimit, nrom.kernlimit, len(fixed), ', '.join(f['path'].split('/')[-1] for f in fixed)))
    # the user side's DLL data area is the firmware's (our kernel-side ROM has none): the flexible model
    # (Symbian^3, N8) reserves it at iUserDataAddress-iTotalUserDataSize and asserts the address is
    # KUserLocalDataEnd (0x7f000000); the multiple model's ROMs carry 0x40000000 on both sides
    if (orom.userdata, orom.userdatasize) != (nrom.userdata, nrom.userdatasize):
        put(220, nrom.userdata)        # iUserDataAddress
        put(224, nrom.userdatasize)    # iTotalUserDataSize
        print('user data 0x%08x+0x%x -> 0x%08x+0x%x (the firmware\'s)' % (orom.userdata, orom.userdatasize,
                                                                         nrom.userdata, nrom.userdatasize))
    put(148, rootlist_at)          # iRomRootDirectoryList
    put(164, eaddr[sec[0]])        # iSecondaryFile (TRomEntry)
    put(188, est_at)               # iRomExceptionSearchTable
    put(236, 0); put(240, size); put(244, size)   # compression off, sizes
    put(328, 0); put(332, 0); put(336, 0)         # no paging
    pers = a.exec_personality or ('sym92' if a.exec92 else None)
    if pers is not None:
        import tomllib
        ids = tomllib.load(open(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                             'personalities', 'registry.toml'), 'rb'))['ids']
        num = int(pers, 0) if re.fullmatch(r'(0x)?[0-9a-fA-F]+', pers) else ids.get(pers)
        if num is None or not 0 <= num < 16:
            raise SystemExit('--exec-personality %s: not in personalities/registry.toml' % pers)
        f = struct.unpack_from('<I', out, 184)[0] & ~(0xf << 25)      # iKernelConfigFlags
        f |= num << 25
        if num == ids['sym92']:
            f |= 1 << 29							# what kernels before the field look at
        put(184, f)
    if a.trace is not None:
        put(256, a.trace)                         # iTraceMask[0]
    for pf in a.patches:
        for ln in open(pf):
            ln = ln.split('#', 1)[0].split()
            if not ln:
                continue
            addr, orig, new = int(ln[0], 16), bytes.fromhex(ln[1]), bytes.fromhex(ln[2])
            if len(orig) != len(new):
                raise SystemExit('patch %08x: length mismatch' % addr)
            o = addr - B
            if bytes(out[o:o + len(orig)]) != orig:
                raise SystemExit('patch %08x: expected %s, found %s' % (addr, orig.hex(), bytes(out[o:o + len(orig)]).hex()))
            out[o:o + len(new)] = new
            print('patch %08x: %s -> %s' % (addr, orig.hex(), new.hex()))
    # primary/variant/extension TRomEntry pointers (and the iNextExtension chain) refer to our
    # original directory, which is still intact inside our window, so they stay valid as-is.
    open(a.out, 'wb').write(out)
    print('wrote %s: 0x%x bytes, root dir 0x%08x, secondary %s @0x%08x, EST %d entries'
          % (a.out, size, rootlist_at, sec[0], eaddr[sec[0]], len(ents)))
    if a.manifest:
        with open(a.manifest, 'w') as m:
            for k, (p, ad, sz, att) in sorted(entries.items(), key=lambda kv: kv[1][1]):
                ours = ad < ustart or any(f['addr'] == ad for f in high) or ad >= n95end
                m.write('%08x %9d %02x %s %s\n' % (ad, sz, att, 'OURS' if ours else 'N95 ', p))


if __name__ == '__main__':
    main()
