#!/usr/bin/env python3
"""Disassemble one XIP image from a flat ROM, annotating calls through import thunks.

  xipdis.py core-flat.img core.json NAME [--arm] [--sdk-lib DIR] [--strings]

Code is disassembled as Thumb (the RVCT default for S60 3.x user code) unless --arm.
Import thunks ("ldr pc,[pc,#-4]; .word target") are resolved to <dll>!<symbol> using the
target DLL's export table and, when available, the SDK import library (<dll>.dso) for names.
"""
import argparse, bisect, json, os, re, struct, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dso_ordinals  # noqa: E402

B = 0x80000000
OBJDUMP = os.environ.get('ARM_OBJDUMP', os.path.expanduser('~/gcc-builds/gcc-14.2.0/bin/arm-none-symbianelf-objdump'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('img'); ap.add_argument('json'); ap.add_argument('name')
    ap.add_argument('--arm', action='store_true')
    ap.add_argument('--sdk-lib', default=os.path.expanduser('~/work/S60_3rd_FP1/Epoc32/release/armv5/lib'))
    ap.add_argument('--strings', action='store_true', help='also dump UTF-16/ASCII strings in the code section')
    a = ap.parse_args()
    d = open(a.img, 'rb').read()
    j = json.load(open(a.json))
    imgs = [f for f in j['files'] if 'image' in f]
    me = [f for f in imgs if f['path'].lower().endswith('/' + a.name.lower())]
    if not me:
        raise SystemExit('not found: ' + a.name)
    im = me[0]['image']
    spans = sorted(((f['image']['code'], f['image']['code'] + f['image']['codesize'], f) for f in imgs), key=lambda s: s[0])
    starts = [s[0] for s in spans]
    sdk = {n.lower(): n for n in os.listdir(a.sdk_lib)} if os.path.isdir(a.sdk_lib) else {}
    namecache = {}

    def sym(addr):
        i = bisect.bisect_right(starts, addr & ~1) - 1
        if i < 0 or not (spans[i][0] <= (addr & ~1) < spans[i][1]):
            return None
        f = spans[i][2]
        fi = f['image']
        dll = f['path'].split('/')[-1]
        ex = struct.unpack_from('<%dI' % fi['expcount'], d, fi['expdir'] - B)
        ords = [k + 1 for k, x in enumerate(ex) if x == addr]
        if not ords:
            return '%s+0x%x' % (dll, addr - fi['code'])
        base = os.path.splitext(dll)[0].lower() + '.dso'
        if dll not in namecache:
            namecache[dll] = dso_ordinals.load(os.path.join(a.sdk_lib, sdk[base])) if base in sdk else {}
        nm = namecache[dll].get(ords[0], ('#%d' % ords[0],))[0]
        return '%s!%s' % (dll, nm)

    code, text, size = im['code'], im['textsize'], im['codesize']
    blob = d[code - B:code - B + size]
    words = struct.unpack_from('<%dI' % (size // 4), blob)
    thunks = {}
    for k in range(len(words) - 1):
        if words[k] == 0xE51FF004:
            thunks[code + 4 * k] = sym(words[k + 1]) or hex(words[k + 1])
    own = struct.unpack_from('<%dI' % im['expcount'], d, im['expdir'] - B) if im['expcount'] else ()
    tmp = os.path.join(os.path.dirname(os.path.abspath(a.img)), '.xipdis.bin')
    open(tmp, 'wb').write(blob[:text])
    args = [OBJDUMP, '-D', '-b', 'binary', '-m', 'arm', '--adjust-vma=0x%x' % code, tmp]
    if not a.arm:
        args[6:6] = ['-M', 'force-thumb']
    out = subprocess.run(args, capture_output=True, text=True).stdout
    os.unlink(tmp)
    print('# %s code=%08x text=%x size=%x entry=%08x exports=%d' % (me[0]['path'], code, text, size, im['entry'], im['expcount']))
    for k, x in enumerate(own, 1):
        print('# export %d -> %08x' % (k, x))
    for line in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):', line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        if any((x & ~1) == addr for x in own):
            print('%08x <export #%d>:' % (addr, [k for k, x in enumerate(own, 1) if (x & ~1) == addr][0]))
        t = re.search(r'\b(?:bl|blx|b)\s+0x([0-9a-f]+)', line)
        note = ''
        if t:
            tg = int(t.group(1), 16)
            if tg in thunks:
                note = '\t; -> ' + thunks[tg]
        # literal pool loads: show strings
        print(line + note)
    if a.strings:
        for mm in re.finditer(rb'(?:[\x20-\x7e]\x00){4,}', blob):
            print('# u16 @%08x %s' % (code + mm.start(), mm.group().decode('utf-16le')))
        for mm in re.finditer(rb'[\x20-\x7e]{5,}', blob):
            print('# a8  @%08x %s' % (code + mm.start(), mm.group().decode()))


if __name__ == '__main__':
    main()
