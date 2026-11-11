#!/usr/bin/env python3
"""Byte patches that survive firmware versions (docs/device-model-v2.md 2.2.3, step 4).

A patch line names a ROM module and an offset from that module's code start instead of an absolute ROM
address; the address comes from the firmware's core.json at build time, and romcompose.py still checks
the original bytes, so a module whose code changed fails loudly instead of being patched in the wrong
place.

  MODULE+0xOFF  ORIGHEX  NEWHEX   # why  [H-id]      (module = the ROM file's name, e.g. fmradioaudiocontrol.dll)
  ADDR          ORIGHEX  NEWHEX   # why              (absolute: still accepted, for code outside any module)

  patches.py resolve FILE [--device ID] [-o OUT]     absolute lines for romcompose (stdout or OUT)
  patches.py convert FILE [--device ID]              rewrite absolute lines as module-relative ones (in place)
  patches.py check FILE --device ID [--search]       resolve against another firmware; --search looks for
                                                     the original bytes in the module when they moved
"""
import argparse, json, os, re, sys

FW = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(FW))
import device as devmodel   # tools/device.py

REL = re.compile(r'^(\s*)([\w.$-]+\.(?:dll|exe|ldd|pdd|fsy|fxt|csy|ext))\+(0x[0-9a-fA-F]+|[0-9a-fA-F]+)(\s.*)$', re.I)
ABS = re.compile(r'^(\s*)([0-9a-fA-F]{8})(\s.*)$')


def modules(core_json):
    """basename (lower case) -> (code address, code size, ROM path) of every XIP file with code"""
    with open(core_json) as f:
        cj = json.load(f)
    out = {}
    for f in cj['files']:
        im = f.get('image') or {}
        if im.get('code') and f['addr'] <= im['code'] < f['addr'] + f['size']:
            # the code size some kernel-side headers give is not their extent: bound it by the file's
            size = min(im.get('codesize', 0), f['addr'] + f['size'] - im['code'])
            out.setdefault(f['path'].split('/')[-1].lower(), (im['code'], size, f['path']))
    return out


def resolve_lines(lines, mods, what='patches'):
    out = []
    for ln in lines:
        m = REL.match(ln)
        if not m:
            out.append(ln)
            continue
        name, off = m.group(2).lower(), int(m.group(3), 16)
        if name not in mods:
            raise SystemExit('%s: module %s is not in this firmware' % (what, m.group(2)))
        code, size, _ = mods[name]
        if off >= size:
            raise SystemExit('%s: %s+0x%x is past the module code (0x%x bytes)' % (what, m.group(2), off, size))
        out.append('%s%08x%s' % (m.group(1), code + off, m.group(4)))
    return out


def convert_lines(lines, mods):
    by_addr = sorted((c, c + s, p) for c, s, p in mods.values())
    out, n = [], 0
    for ln in lines:
        m = ABS.match(ln)
        if not m:
            out.append(ln)
            continue
        a = int(m.group(2), 16)
        hit = next((x for x in by_addr if x[0] <= a < x[1]), None)
        if not hit:
            out.append(ln)				# outside every module's code: stays absolute
            continue
        out.append('%s%s+0x%x%s' % (m.group(1), hit[2].split('/')[-1], a - hit[0], m.group(3)))
        n += 1
    return out, n


def search(lines, mods, core_flat, base):
    """for module-relative lines whose original bytes moved: where they are in the module now"""
    img = open(core_flat, 'rb').read()
    for ln in lines:
        m = REL.match(ln)
        if not m:
            continue
        f = m.group(4).split()
        orig = bytes.fromhex(f[0])
        code, size, _ = mods[m.group(2).lower()]
        off = int(m.group(3), 16)
        at = img[code - base + off:code - base + off + len(orig)]
        if at == orig:
            print('ok     %s+0x%x' % (m.group(2), off))
            continue
        seg = img[code - base:code - base + size]
        hits = [i for i in range(len(seg)) if seg.startswith(orig, i)]
        print('MOVED  %s+0x%x: original bytes at %s' % (m.group(2), off, ', '.join('+0x%x' % h for h in hits[:8]) or 'nowhere'))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['resolve', 'convert', 'check'])
    ap.add_argument('file')
    ap.add_argument('--device')
    ap.add_argument('-o', '--out')
    ap.add_argument('--search', action='store_true')
    a = ap.parse_args()
    prof = devmodel.load(a.device)
    ps = devmodel.paths(prof)
    mods = modules(ps['core_json'])
    lines = open(a.file, newline='').read().split('\n')
    if a.cmd == 'convert':
        out, n = convert_lines(lines, mods)
        open(a.file, 'w', newline='').write('\n'.join(out))
        print('%s: %d lines made module-relative' % (a.file, n))
    elif a.cmd == 'resolve':
        text = '\n'.join(resolve_lines(lines, mods, a.file))
        if a.out:
            open(a.out, 'w', newline='').write(text)
        else:
            print(text)
    else:
        if a.search:
            base = json.load(open(ps['core_json']))['base']
            search(lines, mods, ps['core_flat'], base)
        else:
            print('\n'.join(l for l in resolve_lines(lines, mods, a.file) if ABS.match(l)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
