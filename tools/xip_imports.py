#!/usr/bin/env python3
"""ROM-wide import census for a flat XIP core image (output of rom_unpage.py + romimg.py --json).

In 9.x XIP images rombuild leaves one 8-byte import thunk per import at the end of the code
section ("ldr pc,[pc,#-4]" 0xE51FF004 + absolute target). This tool finds those thunks and
attributes every target to the ROM file that owns the address, and additionally looks for:
  - other words (outside thunks) equal to an export address of the selected libraries
  - direct ARM BL/BLX and Thumb BL/BLX instructions that land on such exports (would mean
    rombuild branch collapsing was used, which in-place thunk patching would miss)

  xip_imports.py core-flat.img core.json [--lib Sys/Bin/EUser.dll ...] [--ordinals OUT.tsv]
"""
import argparse, bisect, collections, json, struct

LDR_PC = 0xE51FF004


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('img')
    ap.add_argument('json')
    ap.add_argument('--lib', action='append', default=[])
    ap.add_argument('--ordinals', help='write per-ordinal usage of the first --lib')
    a = ap.parse_args()
    libs = a.lib or ['Sys/Bin/EUser.dll', 'Sys/Bin/EFSrv.dll', 'Sys/Bin/Hal.dll']
    d = open(a.img, 'rb').read()
    j = json.load(open(a.json))
    base = j['base']
    files = j['files']
    imgs = [f for f in files if 'image' in f]
    spans = sorted((f['image']['code'], f['image']['code'] + f['image']['codesize'], f['path']) for f in imgs)
    starts = [s[0] for s in spans]

    def owner(addr):
        i = bisect.bisect_right(starts, addr & ~1) - 1
        if i >= 0 and spans[i][0] <= (addr & ~1) < spans[i][1]:
            return spans[i][2]
        return None

    F = {f['path']: f for f in imgs}
    lower = {k.lower(): k for k in F}       # Symbian names are case-insensitive (701: euser.dll)
    libs = [lower.get(L.lower(), L) for L in libs]
    exp_ord = {}  # lib -> {addr: [ordinals]}
    for L in libs:
        im = F[L]['image']
        ex = struct.unpack_from('<%dI' % im['expcount'], d, im['expdir'] - base)
        m = collections.defaultdict(list)
        for i, x in enumerate(ex, 1):
            if x:
                m[x].append(i)
        exp_ord[L] = m
    all_exp = {}
    for L, m in exp_ord.items():
        for x in m:
            all_exp[x] = L
            all_exp[x & ~1] = L

    tot_thunks = 0
    thunk_owner = collections.Counter()
    importers = collections.defaultdict(set)
    ord_use = collections.Counter()
    ord_users = collections.defaultdict(set)
    unresolved_thunks = 0
    other_words = collections.Counter()
    direct_br = collections.Counter()
    for f in imgs:
        im = f['image']
        if f['path'] in libs and False:
            continue
        c0, n = im['code'], im['codesize']
        off = c0 - base
        if off < 0 or off + n > len(d):
            continue
        words = struct.unpack_from('<%dI' % (n // 4), d, off)
        thunk_idx = set()
        for i in range(len(words) - 1):
            if words[i] == LDR_PC:
                t = words[i + 1]
                thunk_idx.add(i + 1)
                tot_thunks += 1
                o = owner(t)
                if o is None:
                    unresolved_thunks += 1
                    continue
                thunk_owner[o] += 1
                importers[o].add(f['path'])
                if o == libs[0]:
                    for od in exp_ord[libs[0]].get(t, [-1]):
                        ord_use[od] += 1
                        ord_users[od].add(f['path'])
        # non-thunk literal references to library exports (code + initialised data)
        for i, w in enumerate(words):
            if i not in thunk_idx and w in all_exp and f['path'] not in libs:
                other_words[all_exp[w]] += 1
            # direct ARM BL (cond AL) into a library
            if (w & 0xFF000000) == 0xEB000000 and f['path'] not in libs:
                tgt = c0 + 4 * i + 8 + ((((w & 0xFFFFFF) ^ 0x800000) - 0x800000) << 2)
                if tgt in all_exp or (tgt | 1) in all_exp:
                    direct_br[('arm-bl', all_exp.get(tgt) or all_exp.get(tgt | 1))] += 1
        # direct Thumb BL/BLX pairs into a library
        if f['path'] not in libs:
            hw = struct.unpack_from('<%dH' % (n // 2), d, off)
            for i in range(len(hw) - 1):
                h1, h2 = hw[i], hw[i + 1]
                if (h1 & 0xF800) == 0xF000 and (h2 & 0xE800) == 0xE800:
                    hi = ((h1 & 0x7FF) ^ 0x400) - 0x400
                    pc = c0 + 2 * i + 4
                    tgt = pc + (hi << 12) + ((h2 & 0x7FF) << 1)
                    if (h2 & 0xF800) == 0xE800:  # BLX -> ARM
                        tgt &= ~3
                    L = all_exp.get(tgt) or all_exp.get(tgt | 1)
                    if L:
                        direct_br[('thumb-bl/blx', L)] += 1

    print('XIP images: %d; import thunks: %d (unresolved targets %d)' % (len(imgs), tot_thunks, unresolved_thunks))
    print('top import targets (thunks / importing images):')
    for o, k in thunk_owner.most_common(25):
        print('  %-40s %6d %5d' % (o, k, len(importers[o])))
    for L in libs:
        print('%s: %d thunks from %d images; %d distinct exports used of %d' % (
            L, thunk_owner[L], len(importers[L]),
            len([1 for x in exp_ord[L] if any(True for _ in [0])]) if False else
            sum(1 for od in ord_use if od > 0) if L == libs[0] else -1,
            F[L]['image']['expcount']))
    print('non-thunk words equal to an export of %s: %s' % (', '.join(libs), dict(other_words)))
    print('direct branches into those libraries (heuristic decode): %s' % dict(direct_br))
    if a.ordinals:
        with open(a.ordinals, 'w') as fo:
            for od, k in sorted(ord_use.items()):
                fo.write('%d\t%d\t%d\t%s\n' % (od, k, len(ord_users[od]), ' '.join(sorted(ord_users[od]))[:400]))


if __name__ == '__main__':
    main()
