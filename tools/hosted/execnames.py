#!/usr/bin/env python3
"""execnames: name the exec calls in a hosted kernel console (lines "EXEC fast|slow N (...)" printed by
src/hosted/nkern/linux/vectors.cpp with HostedTraceExecs) from a personality's exec map.

  python3 tools/hosted/execnames.py CONSOLE.log [--personality sym92] [--errors]
--errors: only calls that returned a Symbian error code (-1..-100)."""
import argparse
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load_map(pers):
    names = {}
    for line in open(os.path.join(ROOT, 'personalities', pers, 'exec.map')):
        line = line.split('#')[0].strip()
        m = re.match(r'([FS])([0-9a-fA-F]+)\s+(\S+)', line)
        if m:
            names[(m.group(1), int(m.group(2), 16))] = m.group(3)
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('log')
    ap.add_argument('--personality', default='sym92')
    ap.add_argument('--errors', action='store_true')
    a = ap.parse_args()
    names = load_map(a.personality)
    pending = None
    for line in open(a.log, errors='replace'):
        line = line.rstrip('\n')
        m = re.match(r'EXEC (fast|slow) ([0-9a-f]+) \(([^)]*)\) \[lr ([0-9a-f]+)\]', line)
        if m:
            kind = 'F' if m.group(1) == 'fast' else 'S'
            n = int(m.group(2), 16)
            pending = '%s%02x %-28s (%s) lr %s' % (kind, n, names.get((kind, n), '?'), m.group(3), m.group(4))
            continue
        r = re.match(r'\s+-> ([0-9a-f]+)', line)
        if r and pending:
            v = int(r.group(1), 16)
            v = v - (1 << 32) if v & 0x80000000 else v
            if not a.errors or -100 <= v < 0:
                print('%s -> %d' % (pending, v) if -100 <= v < 0 else '%s -> %08x' % (pending, v & 0xffffffff))
            pending = None
    if pending and not a.errors:
        print(pending + ' -> (did not return)')


if __name__ == '__main__':
    main()
