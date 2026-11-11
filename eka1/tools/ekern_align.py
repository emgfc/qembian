#!/usr/bin/env python3
"""Align the Symbian 6.1 kernel export names with the RH-29 ARM ekern.exe ordinals.

Facts this relies on (see docs/eka1-ngage-roadmap.md, P0):
- The ARM export order is the GCC 2.9 mangled names sorted (C locale).
- The 6.1 names come from the S60 0.9 SDK's WINS EKERN.DLL (Borland-mangled, 244 exports),
  converted by bor2gcc.py. The ARM kernel has 506 exports; the rest are ARM-only and
  interleave.
- Anchors: ekern_evidence.tsv (ordinal, name, the code evidence): the typed handle
  lookups, Find functions and container getters (by container address), and other
  exports identified from their code.

Output: for every gap between anchors, the ordinal range, the candidate names and whether
the gap is exact (as many names as slots: the mapping inside it is determined).

usage: ekern_align.py WINS_EKERN.DLL [--out analysis/ekern-align.tsv]
"""
import argparse, pathlib, struct, subprocess, sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from bor2gcc import convert  # noqa: E402

EVIDENCE = HERE / 'ekern_evidence.tsv'


def load_anchors(path=EVIDENCE):
    """(ordinal, name prefix, evidence) rows; '#' lines are comments."""
    out = []
    for line in path.read_text().splitlines():
        if line.strip() and not line.startswith('#'):
            o, n, ev = line.split('\t', 2)
            out.append((None if o == '-' else int(o), n, ev))
    return out


NEXPORTS = 506


def pe_export_names(path):
    d = path.read_bytes()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    opt = struct.unpack_from('<H', d, pe + 20)[0]
    erva = struct.unpack_from('<I', d, pe + 24 + 96)[0]
    secs = [struct.unpack_from('<8sIIII', d, pe + 24 + opt + 40 * k) for k in range(nsec)]

    def off(r):
        for _, vs, va, rs, ro in secs:
            if va <= r < va + max(vs, rs):
                return r - va + ro
    e = off(erva)
    nn, names_rva = struct.unpack_from('<I', d, e + 24)[0], struct.unpack_from('<I', d, e + 32)[0]
    out = []
    for k in range(nn):
        o = off(struct.unpack_from('<I', d, off(names_rva) + 4 * k)[0])
        out.append(d[o:d.index(b'\0', o)].decode('latin1'))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('wins_ekern', type=pathlib.Path)
    ap.add_argument('--out', type=pathlib.Path, default=HERE.parent / 'analysis/ekern-align.tsv')
    ap.add_argument('--apply', action='store_true', help='name the anchor/exact exports in Ghidra (REST)')
    ap.add_argument('--server', default='http://127.0.0.1:8089')
    ap.add_argument('--rom', type=pathlib.Path, default=HERE.parent / 'output/rom/ROM.bin')
    a = ap.parse_args()
    names = sorted({convert(n) for n in pe_export_names(a.wins_ekern)}, key=lambda s: s.encode())
    anchors = load_anchors()
    absent = {n for o, n, _ in anchors if o is None}
    names = [n for n in names if n not in absent]
    anchors = [x for x in anchors if x[0] is not None]
    rank = {}
    for p in anchors:
        hit = [i for i, n in enumerate(names) if n == p[1]] or [
            i for i, n in enumerate(names) if n.startswith(p[1]) and n[len(p[1])] in 'iRPvCUT0123456789']
        if len(hit) != 1:
            sys.exit('anchor %s matches %r' % (p[1], [names[i] for i in hit]))
        rank[p[0]] = hit[0]
    pts = [(0, -1)] + sorted((o, rank[o]) for o, _, _ in anchors) + [(NEXPORTS + 1, len(names))]
    rows, exact = [], 0
    for (o, r) in pts[1:-1]:
        rows.append((str(o), str(o), 'anchor', names[r]))
    for (o1, r1), (o2, r2) in zip(pts, pts[1:]):
        slots, nn = o2 - o1 - 1, r2 - r1 - 1
        if r2 <= r1 and r2 != len(names):
            sys.exit('anchors out of sort order at ordinals %d/%d' % (o1, o2))
        if nn > slots:
            sys.exit('inconsistent gap %d-%d: %d names for %d slots' % (o1, o2, nn, slots))
        if nn and nn == slots:
            for k in range(nn):
                rows.append((str(o1 + 1 + k), str(o1 + 1 + k), 'exact', names[r1 + 1 + k])); exact += 1
        elif nn:
            for k in range(nn):
                rows.append(('%d' % (o1 + 1 + k), '%d' % (o2 - nn + k), 'range', names[r1 + 1 + k]))
    rows.sort(key=lambda x: (int(x[0]), x[3].encode()))
    a.out.write_text('ordinal_min\tordinal_max\tkind\tgcc_name\n' + ''.join('\t'.join(r) + '\n' for r in rows))
    print('%d 6.1 names, %d anchors, %d exact, %d with an ordinal range -> %s' % (
        len(names), len(anchors), exact, sum(r[2] == 'range' for r in rows), a.out))
    if a.apply:
        apply(a, [(int(r[0]), r[3]) for r in rows if r[2] in ('anchor', 'exact')])


EXPORT_DIR, ROM_BASE = 0x50342068, 0x50000000   # ekern.exe iExportDir (RH-29 V 04.10)


def apply(a, named):
    import json, urllib.request
    rom = a.rom.read_bytes()
    exps = struct.unpack_from('<%dI' % NEXPORTS, rom, EXPORT_DIR - ROM_BASE)

    def req(path, params=None):
        data = json.dumps(params).encode() if params is not None else None
        r = urllib.request.Request(a.server + '/' + path, data=data, headers={'Content-Type': 'application/json'})
        with urllib.request.urlopen(r, timeout=60) as h:
            return json.loads(h.read().decode())
    if req('get_current_address').get('program', '').split('/')[-1] != 'ROM.bin':
        sys.exit('refusing: active program is not ROM.bin')
    ren = lab = 0
    for o, n in named:
        ad = exps[o - 1] & ~1; h = '%08x' % ad
        fn = req('get_function_by_address?address=' + h)
        if int(fn.get('entry_point', '0') or '0', 16) == ad:
            req('rename_function_by_address', {'address': h, 'function_address': h, 'new_name': n}); ren += 1
        else:
            req('create_label', {'address': h, 'name': n}); lab += 1
    print('Ghidra: %d functions renamed, %d labels created (ekern.exe)' % (ren, lab))


if __name__ == '__main__':
    main()
