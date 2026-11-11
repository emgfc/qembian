#!/usr/bin/env python3
"""Name EKA1 ROM exports from SDK import libraries (ordinal -> export table -> address).

Reads ARMI import libraries (GCC 2.9 dlltool archives of PE-ARM COFF members:
the idata$5 word is 0x80000000|ordinal, the external non-__imp_ symbol is the mangled name)
and the ROM's TRomImageHeader export directories (e32rom.h, EKA1: iExportDirCount at 0x3C,
iExportDir at 0x40). Writes analysis/user-exports.tsv; --apply labels the open ROM.bin in
Ghidra over the REST plugin (port 8089).

A library is used for a ROM module only when its highest ordinal fits the module's export
count; mismatches are listed (another SDK version) and skipped.
"""
import argparse, json, pathlib, struct, sys, urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]


def ar_members(data):
    if data[:8] != b'!<arch>\n':
        raise ValueError('not an ar archive')
    off, longnames = 8, b''
    while off + 60 <= len(data):
        hdr = data[off:off + 60]
        name = hdr[:16].rstrip()
        size = int(hdr[48:58])
        body = data[off + 60:off + 60 + size]
        if name == b'//':
            longnames = body
        elif name.startswith(b'/') and name[1:].isdigit() and longnames:
            s = int(name[1:]); name = longnames[s:longnames.index(b'/', s)]
        yield name.rstrip(b'/').decode('latin1'), body
        off += 60 + size + (size & 1)


def coff_import(obj):
    """(ordinal, name) of one dlltool import member, or None."""
    if len(obj) < 20:
        return None
    nsec, _, symptr, nsyms = struct.unpack_from('<HIII', obj, 2)
    if not (0 < nsec < 32) or symptr + nsyms * 18 > len(obj):
        return None
    ordinal = None
    for k in range(nsec):
        sname, _, _, rsize, rptr = struct.unpack_from('<8sIIII', obj, 20 + 40 * k)
        if sname.rstrip(b'\0') == b'.idata$5' and rsize >= 4:
            v = struct.unpack_from('<I', obj, rptr)[0]
            if v & 0x80000000:
                ordinal = v & 0xffff
    strtab = obj[symptr + nsyms * 18:]
    name, i = None, 0
    while i < nsyms:
        e = obj[symptr + i * 18:symptr + i * 18 + 18]
        raw, sec, cls, aux = e[:8], struct.unpack_from('<h', e, 12)[0], e[16], e[17]
        if raw[:4] == b'\0\0\0\0':
            o = struct.unpack_from('<I', raw, 4)[0]
            s = strtab[o:strtab.index(b'\0', o)].decode('latin1')
        else:
            s = raw.rstrip(b'\0').decode('latin1')
        if cls == 2 and sec > 0 and not s.startswith(('__imp_', '_imp__', '.')):
            name = s
        i += 1 + aux
    return (ordinal, name) if ordinal and name else None


def read_lib(path):
    out = {}
    for _, body in ar_members(path.read_bytes()):
        r = coff_import(body)
        if r:
            out[r[0]] = r[1]
    return out


def rom_exports(rom, base, hdr_addr):
    o = hdr_addr - base
    count, edir = struct.unpack_from('<iI', rom, o + 0x3C)
    if not (0 < count < 5000) or not (base <= edir < base + len(rom)):
        return []
    return list(struct.unpack_from('<%dI' % count, rom, edir - base))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('libdir', type=pathlib.Path, help='SDK epoc32/release/armi/urel (the .lib files)')
    ap.add_argument('--rom', type=pathlib.Path, default=ROOT / 'output/rom/ROM.bin')
    ap.add_argument('--manifest', type=pathlib.Path, default=ROOT / 'output/rom/manifest.json')
    ap.add_argument('--out', type=pathlib.Path, default=ROOT / 'analysis/user-exports.tsv')
    ap.add_argument('--apply', action='store_true', help='label/rename in Ghidra (REST)')
    ap.add_argument('--server', default='http://127.0.0.1:8089')
    ap.add_argument('--only', help='comma-separated module names (e.g. euser.dll)')
    a = ap.parse_args()

    rom = a.rom.read_bytes()
    man = json.loads(a.manifest.read_text())
    base = int(man['rom_base'], 0) if isinstance(man['rom_base'], str) else man['rom_base']
    libs = {p.stem.lower(): p for p in a.libdir.iterdir() if p.suffix.lower() == '.lib'}
    only = set(a.only.lower().split(',')) if a.only else None

    rows, skipped, used = [], [], 0
    for f in man['files']:
        name = f['path'].split('/')[-1]
        stem = name.rsplit('.', 1)[0].lower()
        if only and name.lower() not in only:
            continue
        if stem not in libs or name.lower().endswith(('.lib', '.rsc', '.txt')):
            continue
        hdr = f.get('rom_virtual_address')
        exps = rom_exports(rom, base, hdr) if hdr else []
        if not exps:
            continue
        names = read_lib(libs[stem])
        if not names or max(names) > len(exps):
            skipped.append((name, len(exps), max(names) if names else 0))
            continue
        used += 1
        for ordn in sorted(names):
            addr = exps[ordn - 1]
            rows.append((name, ordn, addr & ~1, 'thumb' if addr & 1 else 'arm', names[ordn]))

    a.out.write_text('module\tordinal\taddress\tmode\tname\n' + ''.join(
        f'{m}\t{o}\t0x{ad:08x}\t{md}\t{n}\n' for m, o, ad, md, n in rows))
    print(f'{used} modules named, {len(rows)} exports -> {a.out}')
    for s in skipped:
        print('skipped (export count %d < lib max ordinal %d): %s' % (s[1], s[2], s[0]))

    if a.apply:
        def req(path, params=None):
            data = json.dumps(params).encode() if params is not None else None
            r = urllib.request.Request(a.server + '/' + path, data=data,
                                       headers={'Content-Type': 'application/json'})
            with urllib.request.urlopen(r, timeout=60) as h:
                return json.loads(h.read().decode())
        cur = req('get_current_address')
        if cur.get('program', '').split('/')[-1] != 'ROM.bin':
            sys.exit('refusing: active program is not ROM.bin')
        renamed = labelled = 0
        for m, o, ad, md, n in rows:
            h = '%08x' % ad
            fn = req('get_function_by_address?address=' + h)
            if int(fn.get('entry_point', '0') or '0', 16) == ad:
                req('rename_function_by_address', {'address': h, 'function_address': h, 'new_name': n})
                renamed += 1
            else:
                req('create_label', {'address': h, 'name': n})
                labelled += 1
        print(f'Ghidra: {renamed} functions renamed, {labelled} labels created')


if __name__ == '__main__':
    main()
