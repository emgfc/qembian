#!/usr/bin/env python3
"""Load a (RAM-loaded, e.g. ROFS) Symbian E32 image the way the loader would, for analysis.

  e32load.py IMAGE OUT.elf [--base ADDR] [--rom core-flat.img --rom-json core.json] [--sdk-lib DIR]

- decompresses the image (Symbian "deflate" KUidCompressionDeflate, port of f32 sf_inflate.cpp and
  euser us_decode.cpp; or uncompressed)
- relocates code (and data) to --base (default: the link address iCodeBase); data goes to iDataBase
- resolves ELF-style imports against the export tables of the XIP core ROM, naming the import
  words imp_<dll>_<symbol> (SDK import-library names where available)
- writes an ARM ELF with symbols: exports, imports, entry point, $a/$t/$d mapping symbols
Use the run address printed in the kernel trace ("CodeSeg @... Added By Address 70014000") as
--base to match addresses seen at run time.
"""
import argparse, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dso_ordinals  # noqa: E402

KUidCompressionDeflate = 0x101F7AFC
KUidCompressionBytePair = 0x102822AA

# ---------------------------------------------------------------- Symbian deflate ("inflate")
META = [0x10000000, 0x1c000000, 0x12000000, 0x1d000000, 0x26000000, 0x26800000, 0x2f000000, 0x37400000,
        0x37600000, 0x37800000, 0x3fa00000, 0x3fb00000, 0x3fc00000, 0x3fd00000, 0x47e00000, 0x47e80000,
        0x47f00000, 0x4ff80000, 0x57fc0000, 0x5ffe0000, 0x67ff0000, 0x77ff8000, 0x7fffa000, 0x7fffb000,
        0x7fffc000, 0x7fffd000, 0x7fffe000, 0x87fff000, 0x87fff800]
KMaxCodeLength = 27
ELiterals, ELengths, ESpecials, EDistances = 256, 7 * 4, 1, 11 * 4
ELitLens = ELiterals + ELengths + ESpecials
EEos = ELiterals + ELengths
KDeflateMinLength, KDeflateMaxDistance = 3, 1 << 12


class Bits:
    def __init__(self, data, pos=0):
        self.d = data; self.p = pos * 8

    def bit(self):
        b = (self.d[self.p >> 3] >> (7 - (self.p & 7))) & 1
        self.p += 1
        return b

    def read(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | self.bit()
        return v


def table_from_encoding(enc):
    """{(len, code): symbol} from Symbian encode-table entries (len<<27 | code<<(27-len))."""
    t = {}
    for sym, e in enumerate(enc):
        ln = e >> KMaxCodeLength
        if ln:
            t[(ln, (e & ((1 << KMaxCodeLength) - 1)) >> (KMaxCodeLength - ln))] = sym
    return t


def canonical(lengths):
    count = [0] * (KMaxCodeLength + 1)
    for l in lengths:
        if l:
            count[l] += 1
    nxt, code = [0] * (KMaxCodeLength + 1), 0
    for l in range(1, KMaxCodeLength + 1):
        code <<= 1
        nxt[l] = code
        code += count[l]
    t = {}
    for sym, l in enumerate(lengths):
        if l:
            t[(l, nxt[l])] = sym
            nxt[l] += 1
    return t


def huff(bits, table):
    code = ln = 0
    while True:
        code = (code << 1) | bits.bit(); ln += 1
        s = table.get((ln, code))
        if s is not None:
            return s
        if ln > KMaxCodeLength:
            raise ValueError('bad huffman code')


META_T = table_from_encoding(META)


def internalize(bits, n):
    """Huffman::InternalizeL: code lengths, MTF + RLE-0 coded with the fixed meta code."""
    lst = list(range(KMaxCodeLength + 1))
    last = 0; out = []; rl = 0
    while len(out) + rl < n:
        c = huff(bits, META_T)
        if c < 2:
            rl += rl + c + 1
        else:
            out += [last] * rl; rl = 0
            c -= 1
            lst[0] = last
            last = lst[c]
            lst[1:c + 1] = lst[0:c]
            out.append(last)
    out += [last] * (n - len(out))
    return out


def inflate(data, pos, out_len):
    bits = Bits(data, pos)
    lens = internalize(bits, ELitLens + EDistances)
    lit = canonical(lens[:ELitLens])
    dist = canonical(lens[ELitLens:])
    out = bytearray()
    while len(out) < out_len:
        val = huff(bits, lit) - ELiterals
        if val < 0:
            out.append(val + 256); continue
        if val == EEos - ELiterals:
            break
        code = val & 0xff
        if code >= 8:
            xtra = (code >> 2) - 1
            code -= xtra << 2; code <<= xtra; code |= bits.read(xtra)
        length = code + KDeflateMinLength
        dv = huff(bits, dist)          # distance symbols are numbered from 0 here (no 0x200 base)
        code = dv & 0xff
        if code >= 8:
            xtra = (code >> 2) - 1
            code -= xtra << 2; code <<= xtra; code |= bits.read(xtra)
        src = len(out) - (code + 1)
        for _ in range(length):
            out.append(out[src]); src += 1
    return bytes(out)


# ---------------------------------------------------------------- E32 image
HDR = ('uid1 uid2 uid3 uidck sig hdrcrc modver comp toolsver timelo timehi flags codesize datasize heapmin '
       'heapmax stack bss entry codebase database dllrefcount expdiroff expdircount textsize codeoff dataoff '
       'importoff codereloff datareloff').split()


def parse(path):
    raw = open(path, 'rb').read()
    h = dict(zip(HDR, struct.unpack_from('<30I', raw, 0)))
    if raw[16:20] != b'EPOC':
        raise SystemExit('not an E32 image')
    h['uncomp'] = struct.unpack_from('<I', raw, 124)[0]
    hsize = h['codeoff']
    if h['comp'] == 0:
        img = raw
    elif h['comp'] == KUidCompressionDeflate:
        img = raw[:hsize] + inflate(raw, hsize, h['uncomp'])
    else:
        raise SystemExit('compression %08x not supported yet' % h['comp'])
    return h, img


def relocs(img, off):
    if not off:
        return []
    size, n = struct.unpack_from('<II', img, off)
    out, p, end = [], off + 8, off + 8 + size
    while p < end:
        page, bsize = struct.unpack_from('<II', img, p)
        for k in range((bsize - 8) // 2):
            e = struct.unpack_from('<H', img, p + 8 + 2 * k)[0]
            if e & 0xF000:
                out.append((page + (e & 0xFFF), e & 0xF000))
        p += bsize
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image'); ap.add_argument('out')
    ap.add_argument('--base', type=lambda s: int(s, 0))
    ap.add_argument('--rom', default='build/n95/core-flat.img')
    ap.add_argument('--rom-json', default='build/n95/core.json')
    ap.add_argument('--sdk-lib', default=os.environ.get('S60_SDK_LIB', os.path.expanduser('~/work/S60_3rd_FP1/Epoc32/release/armv5/lib')))
    a = ap.parse_args()
    h, img = parse(a.image)
    base = a.base if a.base is not None else h['codebase']
    cs, ds = h['codesize'], h['datasize']
    code = bytearray(img[h['codeoff']:h['codeoff'] + cs])
    data = bytearray(img[h['dataoff']:h['dataoff'] + ds]) if ds else bytearray()
    cdelta = base - h['codebase']
    dbase = h['database'] if ds or h['bss'] else 0
    for off, typ in relocs(img, h['codereloff']):
        v = struct.unpack_from('<I', code, off)[0]
        is_data = typ == 0x2000 or (typ == 0x3000 and not (h['codebase'] <= v < h['codebase'] + cs))
        struct.pack_into('<I', code, off, (v + (0 if is_data else cdelta)) & 0xffffffff)
    for off, typ in relocs(img, h['datareloff']):
        v = struct.unpack_from('<I', data, off)[0]
        is_data = typ == 0x2000 or (typ == 0x3000 and not (h['codebase'] <= v < h['codebase'] + cs))
        struct.pack_into('<I', data, off, (v + (0 if is_data else cdelta)) & 0xffffffff)

    # core ROM exports for import resolution
    rom = open(a.rom, 'rb').read() if os.path.exists(a.rom) else None
    B = struct.unpack_from('<I', rom, 0x8c)[0] if rom else 0x80000000	# TRomHeader.iRomBase (N73: 0xF8000000)
    files = {}
    if rom:
        for f in json.load(open(a.rom_json))['files']:
            if 'image' in f:
                files[f['path'].split('/')[-1].lower()] = f['image']
    sdk = {n.lower(): n for n in os.listdir(a.sdk_lib)} if os.path.isdir(a.sdk_lib) else {}
    syms = []
    imp_fmt = (h['flags'] >> 28) & 7       # 1 = ELF-style import blocks (code offsets)
    if h['importoff']:
        isize = struct.unpack_from('<I', img, h['importoff'])[0]
        p = h['importoff'] + 4
        for _ in range(h['dllrefcount']):
            noff, n = struct.unpack_from('<Ii', img, p)
            nm = img[h['importoff'] + noff:img.index(b'\0', h['importoff'] + noff)].decode()
            import re as _re
            dll = _re.sub(r'[\[{][0-9a-fA-F]+[\]}]', '', nm)	# euser[100039e5].dll / euser{000a0000}.dll
            dll_l = dll.lower()
            k = os.path.splitext(dll_l)[0] + '.dso'
            names = dso_ordinals.load(os.path.join(a.sdk_lib, sdk[k])) if k in sdk else {}
            fi = files.get(dll_l)
            exps = struct.unpack_from('<%dI' % fi['expcount'], rom, fi['expdir'] - B) if fi and fi['expcount'] else ()
            for j in range(n):
                coff = struct.unpack_from('<I', img, p + 8 + 4 * j)[0]
                w = struct.unpack_from('<I', code, coff)[0]
                ordn, addend = w & 0xffff, w >> 16
                target = (exps[ordn - 1] + addend) if 0 < ordn <= len(exps) else 0
                struct.pack_into('<I', code, coff, target)
                sym = names.get(ordn, ('ord%d' % ordn,))[0]
                nmx = 'imp_%s_%s' % (os.path.splitext(dll)[0], sym)
                if coff >= 4 and struct.unpack_from('<I', code, coff - 4)[0] == 0xE51FF004:
                    # import stub "ldr pc,[pc,#-4]; .word target": callers branch to the instruction
                    syms.append((nmx, base + coff - 4, 8, 2))
                    syms.append(('$a', base + coff - 4, 0, 0))
                else:
                    syms.append((nmx, base + coff, 4, 1))
                syms.append(('$d', base + coff, 0, 0))
            p += 8 + 4 * n
    # exports
    if h['expdircount']:
        eoff = h['expdiroff'] - h['codeoff']
        for k in range(h['expdircount']):
            v = struct.unpack_from('<I', code, eoff + 4 * k)[0]
            if v:
                syms.append(('export_%d' % (k + 1), v, 0, 2))
                syms.append(('$t' if v & 1 else '$a', v & ~1, 0, 0))
    ep = base + h['entry']
    syms.append(('_E32Startup', ep, 0, 2)); syms.append(('$a', ep, 0, 0))

    # ---- ELF
    segs = [(base, bytes(code), 5)] + ([(dbase, bytes(data), 6)] if data else [])
    strtab = bytearray(b'\0'); symtab = bytearray(b'\0' * 16)
    for nm, val, sz, typ in syms:
        o = len(strtab); strtab += nm.encode('ascii', 'replace') + b'\0'
        symtab += struct.pack('<IIIBBH', o, val, sz, (1 << 4) | typ if typ else 0, 0, 1)
    shstr = b'\0.text\0.symtab\0.strtab\0.shstrtab\0.data\0'
    nph = len(segs)
    off = 52 + 32 * nph
    seg_off = []
    for vaddr, blob, fl in segs:
        seg_off.append(off); off += (len(blob) + 3) & ~3
    off_sym = off; off_str = off_sym + len(symtab); off_shs = off_str + len(strtab)
    off_sh = (off_shs + len(shstr) + 3) & ~3
    nsh = 5 + (1 if data else 0)
    out = bytearray(b'\x7fELF' + bytes([1, 1, 1, 0]) + b'\0' * 8)
    out += struct.pack('<HHIIIIIHHHHHH', 2, 40, 1, ep, 52, off_sh, 0x05000000, 52, 32, nph, 40, nsh, 4)
    for (vaddr, blob, fl), so in zip(segs, seg_off):
        out += struct.pack('<IIIIIIII', 1, so, vaddr, vaddr, len(blob), len(blob), fl, 4)
    for (vaddr, blob, fl), so in zip(segs, seg_off):
        out += b'\0' * (so - len(out)) + blob
    out += b'\0' * (off_sym - len(out)) + symtab + strtab + shstr
    out += b'\0' * (off_sh - len(out)) + b'\0' * 40
    out += struct.pack('<10I', 1, 1, 6, base, seg_off[0], len(code), 0, 0, 4, 0)
    out += struct.pack('<10I', 7, 2, 0, 0, off_sym, len(symtab), 3, 1, 4, 16)
    out += struct.pack('<10I', 15, 3, 0, 0, off_str, len(strtab), 0, 0, 1, 0)
    out += struct.pack('<10I', 23, 3, 0, 0, off_shs, len(shstr), 0, 0, 1, 0)
    if data:
        out += struct.pack('<10I', 33, 1, 3, dbase, seg_off[1], len(data), 0, 0, 4, 0)
    open(a.out, 'wb').write(out)
    print('%s: uid3 %08x comp %08x code %x@%08x (link %08x) data %x@%08x, %d imports, %d exports, entry %08x'
          % (a.image, h['uid3'], h['comp'], cs, base, h['codebase'], ds, dbase,
             sum(1 for s in syms if s[0].startswith('imp_')), h['expdircount'], ep))


if __name__ == '__main__':
    main()
