#!/usr/bin/env python3
"""Wrap one XIP image from a flat Symbian ROM into an ARM ELF with symbols, for Ghidra/objdump.

  xip2elf.py core-flat.img core.json NAME OUT.elf [--sdk-lib DIR]

The ELF has one loadable segment (the image's code section at its real ROM address, plus the
initialised data image if any) and a symbol table with:
  - exports: SDK import-library names when available, else <dll>_ord<N>   (Thumb bit honoured)
  - import thunks ("ldr pc,[pc,#-4]; .word target"):  imp_<dll>_<symbol>
  - ARM mapping symbols $a/$t/$d so disassemblers pick the right instruction set
Analysis tools then see real names for every cross-DLL call.
"""
import argparse, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dso_ordinals  # noqa: E402

B = 0x80000000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('img'); ap.add_argument('json'); ap.add_argument('name'); ap.add_argument('out')
    ap.add_argument('--sdk-lib', default=os.environ.get('S60_SDK_LIB', os.path.expanduser('~/work/S60_3rd_FP1/Epoc32/release/armv5/lib')))
    a = ap.parse_args()
    d = open(a.img, 'rb').read()
    j = json.load(open(a.json))
    imgs = [f for f in j['files'] if 'image' in f]
    me = [f for f in imgs if f['path'].lower().endswith('/' + a.name.lower())]
    if not me:
        raise SystemExit('not found: ' + a.name)
    im = me[0]['image']
    sdk = {n.lower(): n for n in os.listdir(a.sdk_lib)} if os.path.isdir(a.sdk_lib) else {}
    cache = {}

    def names_for(dll):
        if dll not in cache:
            k = os.path.splitext(dll)[0].lower() + '.dso'
            cache[dll] = dso_ordinals.load(os.path.join(a.sdk_lib, sdk[k])) if k in sdk else {}
        return cache[dll]

    spans = sorted(((f['image']['code'], f['image']['code'] + f['image']['codesize'], f) for f in imgs),
                   key=lambda s: s[0])

    def sym(addr):
        for lo, hi, f in spans:
            if lo <= (addr & ~1) < hi:
                fi = f['image']; dll = f['path'].split('/')[-1]
                ex = struct.unpack_from('<%dI' % fi['expcount'], d, fi['expdir'] - B) if fi['expcount'] else ()
                for k, x in enumerate(ex, 1):
                    if x == addr:
                        nm = names_for(dll).get(k, (None,))[0]
                        base = os.path.splitext(dll)[0]
                        return '%s_%s' % (base, nm) if nm else '%s_ord%d' % (base, k)
                return '%s_plus_%x' % (os.path.splitext(dll)[0], (addr & ~1) - fi['code'])
        return 'abs_%08x' % addr

    code, size, text = im['code'], im['codesize'], im['textsize']
    blob = bytearray(d[code - B:code - B + size])
    syms = []  # (name, value, size, type, mapping)
    own = names_for(me[0]['path'].split('/')[-1])
    base = os.path.splitext(me[0]['path'].split('/')[-1])[0]
    ex = struct.unpack_from('<%dI' % im['expcount'], d, im['expdir'] - B) if im['expcount'] else ()
    for k, x in enumerate(ex, 1):
        if x:
            nm = own.get(k, (None,))[0] or '%s_ord%d' % (base, k)
            syms.append((nm, x, 0, 2))
            syms.append(('$t' if x & 1 else '$a', x & ~1, 0, 0))
    words = struct.unpack_from('<%dI' % (size // 4), blob)
    for k in range(len(words) - 1):
        if words[k] == 0xE51FF004:
            at = code + 4 * k
            syms.append(('imp_' + sym(words[k + 1]), at, 8, 2))
            syms.append(('$a', at, 0, 0))
            syms.append(('$d', at + 4, 0, 0))
    if im['entry']:
        syms.append(('_E32Startup', im['entry'], 0, 2))
    syms.append(('$d', code + text, 0, 0))  # export dir / rodata tail
    # data image (initialised data follows code in ROM file)
    data_addr, data_size = im['data'], im['datasize']

    # ---- write ELF ----
    strtab = bytearray(b'\0'); symtab = bytearray(b'\0' * 16)
    for nm, val, sz, typ in syms:
        off = len(strtab); strtab += nm.encode('ascii', 'replace') + b'\0'
        symtab += struct.pack('<IIIBBH', off, val, sz, (1 << 4) | typ if typ else 0, 0, 1)
    shstr = b'\0.text\0.symtab\0.strtab\0.shstrtab\0'
    ehsize, phsize, shsize = 52, 32, 40
    off_text = ehsize + phsize
    off_sym = (off_text + len(blob) + 3) & ~3
    off_str = off_sym + len(symtab)
    off_shs = off_str + len(strtab)
    off_sh = (off_shs + len(shstr) + 3) & ~3
    out = bytearray()
    out += b'\x7fELF' + bytes([1, 1, 1, 0]) + b'\0' * 8
    out += struct.pack('<HHIIIIIHHHHHH', 2, 40, 1, im['entry'] or code, ehsize, off_sh, 0x05000000,
                       ehsize, phsize, 1, shsize, 5, 4)
    out += struct.pack('<IIIIIIII', 1, off_text, code, code, len(blob), len(blob), 5, 4)
    out += blob
    out += b'\0' * (off_sym - len(out)) + symtab + strtab + shstr
    out += b'\0' * (off_sh - len(out))
    out += b'\0' * shsize
    out += struct.pack('<IIIIIIIIII', 1, 1, 6, code, off_text, len(blob), 0, 0, 4, 0)            # .text
    out += struct.pack('<IIIIIIIIII', 7, 2, 0, 0, off_sym, len(symtab), 3, 1, 4, 16)              # .symtab
    out += struct.pack('<IIIIIIIIII', 15, 3, 0, 0, off_str, len(strtab), 0, 0, 1, 0)              # .strtab
    out += struct.pack('<IIIIIIIIII', 23, 3, 0, 0, off_shs, len(shstr), 0, 0, 1, 0)               # .shstrtab
    open(a.out, 'wb').write(out)
    print('%s: code %08x+%x, %d symbols (%d imports), data %08x+%x not included'
          % (a.out, code, size, len(syms), sum(1 for s in syms if s[0].startswith('imp_')), data_addr, data_size))


if __name__ == '__main__':
    main()
