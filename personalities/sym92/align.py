#!/usr/bin/env python3
"""Build a draft 9.2 -> ours exec mapping by sequence alignment.

Anchors: export-matched pairs (swi_map --compare output) and EKA2L1 v93 names that normalise to
one of our exec names. Between two consecutive anchors whose gaps are equal on both sides,
entries are paired positionally ("interp"). Everything else stays unknown.
Writes lines: <N95 num> <our name|?> <source>   (source: export|eka2l1|interp|unknown)
"""
import re, sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import exec_table

execs, cmp_txt, svc_cpp = sys.argv[1:4]
ours = exec_table.ours(execs)                    # num -> name
byname = {re.sub(r'[^a-z0-9]', '', v.lower()): k for k, v in ours.items()}
e93 = exec_table.eka2l1(svc_cpp)['v93']
num = lambda s: (0x800000 | int(s[1:], 16)) if s[0] == 'F' else int(s[1:], 16)
fmt = lambda n: ('F%02x' % (n & 0xFFFF)) if n & 0x800000 else ('S%02x' % n)
n95 = []
exp = {}
for l in open(cmp_txt):
    p = l.split()
    if l.startswith('#') or not p:
        continue
    n95.append(num(p[0]))
    if len(p) > 2 and 'CONFLICT' not in l:
        exp[num(p[0])] = num(p[1])
alias = {'chunknew': 'chunkcreate', 'processgetid': 'processid', 'userafter': 'timerafter',
         'messagecompletehandle': 'messagecompletewithhandle', 'clearinactivitytime': 'resetinactivitytime',
         'messageopenhandle': 'messageopenobject', 'safeinc32': None, 'safedec32': None}
eka = {}
for n in n95:
    e = e93.get(n)
    if not e:
        continue
    k = re.sub(r'[^a-z0-9]', '', e.lower())
    k = alias.get(k, k)
    for cand in ([k] if k else []) + ([k.replace('usersvr', '')] if k else []):
        if cand in byname and (byname[cand] & 0x800000) == (n & 0x800000):
            eka[n] = byname[cand]
            break
res = {}
for n in n95:
    a, b = exp.get(n), eka.get(n)
    if a is not None and b is not None and a != b:
        res[n] = (b, 'eka2l1>export')     # EKA2L1 wins on disagreement; flagged
    elif a is not None:
        res[n] = (a, 'export+eka2l1' if b == a else 'export')
    elif b is not None:
        res[n] = (b, 'eka2l1')
# drop anchors that break monotonicity (likely mis-pairings), per table
for fast in (0, 0x800000):
    ks = sorted(k for k in res if (k & 0x800000) == fast)
    changed = True
    while changed:
        changed = False
        for i in range(1, len(ks) - 1):
            p, c, nx = res.get(ks[i - 1]), res.get(ks[i]), res.get(ks[i + 1])
            if p and c and nx and not (p[0] < c[0] < nx[0]) and p[0] < nx[0] and c[1] == 'export':
                del res[ks[i]]; ks.pop(i); changed = True; break
# interpolate between anchors with equal gaps
for fast in (0, 0x800000):
    ks = sorted(k for k in n95 if (k & 0x800000) == fast)
    anchors = [k for k in ks if k in res]
    for x, y in zip(anchors, anchors[1:]):
        if y - x == res[y][0] - res[x][0] and y - x > 1:
            for k in range(x + 1, y):
                if k not in res:
                    res[k] = (res[x][0] + (k - x), 'interp')
used = {}
for n in n95:
    if n in res:
        used.setdefault(res[n][0], []).append(n)
for n in n95:
    if n in res:
        o, src = res[n]
        dup = ' DUP' if len(used[o]) > 1 else ''
        print('%s %s %s%s' % (fmt(n), ours[o], src, dup))
    else:
        print('%s ? unknown%s' % (fmt(n), (' eka2l1=' + e93[n]) if n in e93 else ''))
