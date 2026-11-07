#!/usr/bin/env python3
"""Derive a new personality's exec.map from an existing one, through the two firmwares' EUSERs.

Each exported EUSER function that issues SWIs does so in both firmwares (same export name, from the
SDK .dso); pairing the SWI lists of equal-named exports maps the new numbering onto the known one, and
the known personality's exec.map names our handler. Numbers no export pairs are filled by sequence
(fast and slow calls are numbered in source order, so a gap between two anchors that has the same
width on both sides pairs positionally); the rest are reported as unknown for manual review.

  derive_map.py NEW-euser-swi.json KNOWN-euser-swi.json personalities/KNOWN/exec.map \
                --name "Symbian 9.1 (N73 ...)" > personalities/NEW/exec.map

(the *-euser-swi.json files come from tools/swi_map.py --rom-image ... --names SDK-euser.dso)
"""
import argparse, collections, json, sys

F = 0x800000
fmt = lambda n: ('F%02x' % (n & 0xFFFF)) if n & F else ('S%02x' % n)
num = lambda s: (F | int(s[1:], 16)) if s[0] == 'F' else int(s[1:], 16)


def load_map(path):
    m = {}
    for l in open(path):
        p = l.split('#')[0].split()
        if len(p) >= 2:
            m[num(p[0])] = p[1]
    return m


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('new'); ap.add_argument('known'); ap.add_argument('known_map')
    ap.add_argument('--name', default='new personality')
    a = ap.parse_args()
    new, old = json.load(open(a.new)), json.load(open(a.known))
    kmap = load_map(a.known_map)
    votes = collections.defaultdict(collections.Counter)
    for name, sw in new['byname'].items():
        o = old['byname'].get(name)
        if o and len(o) == len(sw):
            for x, y in zip(sw, o):
                if (x & F) == (y & F):
                    votes[x][y] += 1
    pair, conflict = {}, {}
    for x, c in votes.items():
        (y, n), *rest = c.most_common()
        pair[x] = (y, n, 'export x%d' % n)
        if rest:
            conflict[x] = c
    # sequence fill between anchors, separately for fast and slow numbering
    for fast in (True, False):
        xs = sorted(s for s in new['swis'] if bool(s & F) == fast)
        ys = sorted(s for s in old['swis'] if bool(s & F) == fast) + sorted(k for k in kmap if bool(k & F) == fast)
        ys = sorted(set(ys))
        anchors = sorted((x, pair[x][0]) for x in xs if x in pair)
        for (x0, y0), (x1, y1) in zip(anchors, anchors[1:]):
            if x1 - x0 == y1 - y0 and x1 - x0 > 1:
                for i in range(1, x1 - x0):
                    if x0 + i not in pair:
                        pair[x0 + i] = (y0 + i, 0, 'sequence')
    out = []
    out.append('# %s executive call numbering -> our kernel.' % a.name)
    out.append('# <number> <our exec name | @Shim[:NativeExecForFlags] | !invalid>  # source')
    out.append('# Derived by tools/personality/derive_map.py from %s (export pairing with the known' % a.new.split('/')[-1])
    out.append('# personality %s, then sequence fill); "manual" marks reviewed lines.' % a.known_map)
    unknown = []
    top = {True: max((s & 0xFFFF for s in new['swis'] if s & F), default=-1),
           False: max((s for s in new['swis'] if not s & F), default=-1)}
    for fast in (True, False):
        for i in range(top[fast] + 1):
            x = (F | i) if fast else i
            if x in pair:
                y, n, src = pair[x]
                name = kmap.get(y, '?')
                note = '%s; 9.2 %s' % (src, fmt(y)) + ('; CONFLICT %s' % dict((fmt(k), v) for k, v in conflict[x].items())
                                                       if x in conflict else '')
                out.append('%-4s %-36s # %s' % (fmt(x), name, note))
            elif x in new['swis']:
                out.append('%-4s %-36s # unknown: used by EUSER, no export pairs it' % (fmt(x), '?'))
                unknown.append(fmt(x))
            else:
                out.append('%-4s %-36s # no caller in EUSER' % (fmt(x), '!invalid'))
    print('\n'.join(out))
    print('derive_map: %d numbers, %d paired by export, %d by sequence, %d conflicts, %d unknown: %s'
          % (len(new['swis']), sum(1 for v in pair.values() if v[1]), sum(1 for v in pair.values() if not v[1]),
             len(conflict), len(unknown), ' '.join(unknown)), file=sys.stderr)


if __name__ == '__main__':
    main()
