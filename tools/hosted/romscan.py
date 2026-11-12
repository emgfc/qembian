#!/usr/bin/env python3
"""romscan: what a hosted EKA2 must intercept in a phone's core ROM (static scan, no phone).

Per ROM module (code section, ARM-state words): exec stubs (`svc #n` followed by `bx lr`: EUSER's
exec call stubs; n = exec number, bit 23 = fast exec), other svc words, SWP/SWPB (emulated by Linux
through an undef trap on ARMv8: the cost question of docs/hosted-eka2/roadmap.md), CP15 barrier MCRs
(c7 c10 4/5, c7 c5 4), LDREX/STREX. Counts outside stub patterns are upper bounds (literal pools can
look like instructions).

  python3 tools/hosted/romscan.py [--device ID] [--json OUT] [--module EUser.dll]
The ROM comes from the device's build dir (tools/device.py paths: core_flat + core_json).
"""
import argparse
import json
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def device_paths(dev):
    env = dict(os.environ)
    if dev:
        env['SYBORG_DEVICE'] = dev
    out = subprocess.run([sys.executable, os.path.join(ROOT, 'tools/device.py'), 'paths'], env=env,
                         capture_output=True, text=True, check=True).stdout
    p = {}
    for line in out.splitlines():
        k, _, v = line.partition(' ')
        p[k] = v.strip()
    return p


BX_LR = 0xe12fff1e


def scan(img, base, code, size):
    off = code - base
    words = struct.unpack_from('<%dI' % (size // 4), img, off)
    r = {'stubs': [], 'svc_other': 0, 'swp': 0, 'swpb': 0, 'cp15_barrier': 0, 'ldrex': 0, 'strex': 0}
    n = len(words)
    for i, w in enumerate(words):
        if (w & 0x0f000000) == 0x0f000000 and (w >> 28) != 0xf:
            if i + 1 < n and words[i + 1] == BX_LR:
                r['stubs'].append((code + 4 * i, w & 0x00ffffff))
            else:
                r['svc_other'] += 1
        elif (w & 0x0ff00ff0) == 0x01000090 and (w >> 28) != 0xf:
            r['swp'] += 1
        elif (w & 0x0ff00ff0) == 0x01400090 and (w >> 28) != 0xf:
            r['swpb'] += 1
        elif (w & 0x0fff0fff) in (0x0e070f9a, 0x0e070fba, 0x0e070f95):   # mcr p15,0,rX,c7,c10,4/5 ; c7,c5,4
            r['cp15_barrier'] += 1
        elif (w & 0x0ff00fff) == 0x01900f9f:
            r['ldrex'] += 1
        elif (w & 0x0ff00ff0) == 0x01800f90:
            r['strex'] += 1
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--device')
    ap.add_argument('--json')
    ap.add_argument('--module', help='print every exec stub of this module')
    a = ap.parse_args()
    p = device_paths(a.device)
    idx = json.load(open(p['core_json']))
    img = open(p['core_flat'], 'rb').read()
    base = idx['base']
    mods = []
    for f in idx['files']:
        im = f.get('image')
        if not im or not im.get('codesize'):
            continue
        code, size = im['code'], im['textsize'] or im['codesize']
        if code < base or code + size > base + len(img):
            continue
        r = scan(img, base, code, size & ~3)
        r['path'] = f['path']
        r['code'] = code
        r['size'] = size
        mods.append(r)
    tot = {k: sum(m[k] for m in mods) for k in ('swp', 'swpb', 'cp15_barrier', 'ldrex', 'strex')}
    nstubs = sum(len(m['stubs']) for m in mods)
    print('%d modules, %d exec stubs, totals (upper bounds) %s' % (len(mods), nstubs, tot))
    for m in mods:
        if m['path'].lower().endswith('euser.dll'):
            print('EUSER: %d stubs, swp %d swpb %d cp15 barriers %d ldrex %d strex %d' % (len(m['stubs']), m['swp'], m['swpb'], m['cp15_barrier'], m['ldrex'], m['strex']))
    print('modules with exec stubs:')
    for m in sorted(mods, key=lambda m: -len(m['stubs'])):
        if m['stubs']:
            lo = min(s[0] for s in m['stubs'])
            hi = max(s[0] for s in m['stubs']) + 8
            fast = sum(1 for s in m['stubs'] if s[1] & 0x800000)
            print('  %-40s %4d stubs (%d fast) in %08x..%08x' % (m['path'], len(m['stubs']), fast, lo, hi))
    print('most SWP/SWPB sites (upper bounds):')
    for m in sorted(mods, key=lambda m: -(m['swp'] + m['swpb']))[:12]:
        if m['swp'] + m['swpb']:
            print('  %-40s swp %4d swpb %3d' % (m['path'], m['swp'], m['swpb']))
    if a.module:
        for m in mods:
            if m['path'].lower().endswith(a.module.lower()):
                for addr, n in m['stubs']:
                    print('  %08x  svc #%06x  %s %d' % (addr, n, 'fast' if n & 0x800000 else 'slow', n & 0x7fffff))
    if a.json:
        with open(a.json, 'w') as f:
            json.dump({'base': base, 'modules': mods}, f, indent=1)
        print('json:', a.json)


if __name__ == '__main__':
    main()
