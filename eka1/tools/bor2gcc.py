#!/usr/bin/env python3
"""Borland C++ (WINS) mangled names -> GCC 2.9 (ARMI) mangled names, for EKA1 export lists.

The Symbian 6.1 WINS EKERN.DLL exports Borland-mangled names (@Class@Method$q<args>);
the ARM ekern's export order is the sorted GCC 2.9 mangled names. This converts one into
the other so the 6.1 names can be merged into the ARM ordinals. Covers the type codes that
occur in the EKA1 kernel/user exports; raises on anything else so a gap is visible.
"""
import re, sys

BUILTIN = {'v': 'v', 'i': 'i', 'ui': 'Ui', 'c': 'c', 'uc': 'Uc', 'zc': 'Sc', 's': 's', 'us': 'Us',
           'l': 'l', 'ul': 'Ul', 'j': 'x', 'uj': 'Ux', 'f': 'f', 'd': 'd', 'g': 'r', 'o': 'b',
           'b': 'w', 'e': 'e'}


class P:
    def __init__(self, s):
        self.s, self.i = s, 0

    def peek(self, n=1):
        return self.s[self.i:self.i + n]

    def take(self, n=1):
        r = self.s[self.i:self.i + n]; self.i += n; return r

    def eof(self):
        return self.i >= len(self.s)


def name(p):
    m = re.match(r'\d+', p.s[p.i:])
    n = int(m.group()); p.i += len(m.group())
    return p.take(n)


def template(p):
    # %Name$<args>% ; args: i256 (int value), <type>
    assert p.take() == '%'
    j = p.s.index('$', p.i); nm = p.s[p.i:j]; p.i = j + 1
    args = []
    while p.peek() != '%':
        if p.peek() == '$':
            p.take(); continue
        if p.peek() == 'i' and re.match(r'i\$?i?(\d+)', p.s[p.i:]):
            m = re.match(r'i\$i(\d+)|i(\d+)', p.s[p.i:])
            v = m.group(1) or m.group(2); p.i += len(m.group())
            args.append('i' + v)
        else:
            args.append('Z' + typ(p, []))
    p.take()
    return 't%d%s%d%s' % (len(nm), nm, len(args), ''.join(args))


def typ(p, seen):
    c = p.peek()
    if c == 'x':
        p.take(); return 'C' + typ(p, seen)
    if c == 'w':
        p.take(); return 'V' + typ(p, seen)
    if c == 'p':
        p.take()
        if p.peek() == 'q':          # pointer to function: pq<args>$<ret>
            p.take(); args = []
            while p.peek() != '$':
                args.append(typ(p, args))
            p.take(); ret = typ(p, [])
            return 'PF' + ''.join(args) + '_' + ret
        return 'P' + typ(p, seen)
    if c == 'r':
        p.take(); return 'R' + typ(p, seen)
    if c == 'a':                       # array a<n>$<type>
        p.take(); m = re.match(r'(\d+)\$', p.s[p.i:]); p.i += len(m.group())
        return 'A%d_%s' % (int(m.group(1)) - 1, typ(p, seen))
    if c.isdigit():
        n = name(p)
        if n.startswith('%'):
            return template(P(n))
        return '%d%s' % (len(n), n)
    if c == '%':
        return template(p)
    for k in ('ui', 'uc', 'us', 'ul', 'uj', 'zc'):
        if p.peek(2) == k:
            p.take(2); return BUILTIN[k]
    if c in BUILTIN:
        p.take(); return BUILTIN[c]
    raise ValueError('type code %r at %d in %s' % (c, p.i, p.s))


def args(p):
    out = []
    while not p.eof():
        if p.peek() == 't':           # tN: repeat of argument N (1-based); GCC TN (0 = class)
            p.take(); m = re.match(r'\d', p.s[p.i:]); p.i += 1
            out.append('T' + m.group())
            continue
        out.append(typ(p, out))
    # GCC collapses runs of one repeated reference: TnTn -> N2n
    s, i = [], 0
    while i < len(out):
        j = i
        while j + 1 < len(out) and out[j + 1] == out[i] and out[i].startswith('T'):
            j += 1
        s.append(out[i] if j == i else 'N%d%s' % (j - i + 1, out[i][1:]))
        i = j + 1
    return ''.join(s) or 'v'


def convert(b):
    """'@Kern@ThreadFromHandle$qi' -> 'ThreadFromHandle__4Kerni'."""
    m = re.match(r'@(\w+)@(\$b\w+|\w+)\$(x?)q(.*)$', b)
    if not m:
        m2 = re.match(r'^_?(\w+)$', b)
        if m2:
            return m2.group(1)
        raise ValueError(b)
    cls, meth, const, a = m.groups()
    cl = '%d%s' % (len(cls), cls)
    q = 'C' if const else ''
    if meth == '$bdtr':
        return '_._' + cl
    if meth == '$bctr':
        al = args(P(a))
        return '__' + cl + ('' if al == 'v' else al) if al != 'v' else '__' + cl
    return '%s__%s%s%s' % (meth, q, cl, args(P(a)))


if __name__ == '__main__':
    for line in sys.stdin:
        b = line.strip()
        if not b:
            continue
        try:
            print('%s\t%s' % (convert(b), b))
        except Exception as e:
            print('ERROR\t%s\t%s' % (b, e))
