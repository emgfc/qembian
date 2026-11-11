#!/usr/bin/env python3
"""kabi (personality sym92): compare the kernel ABI that third-party drivers see on the N95 (its own 9.2 ekern.exe in
build/roms/<id>/<fw>/core-flat.img) with our kernel's (ekern.exe in build/roms/<id>/<fw>/hybrid.img).

Drivers import ekern by ordinal; the ordinals match (eabi/ekernu.def), so each exported vtable
(_ZTV...) can be read from both ROMs slot by slot and every entry named through the export tables
(N95) or our ELF symbols (ours). Differences in slot order/count are ABI breaks for drivers that
derive from those classes.

  tools/personality/kabi.py                 report every exported vtable, OK or the differing slots
  tools/personality/kabi.py --json OUT      also write the slot tables (for generated checks)
"""
import argparse, bisect, json, os, re, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as devmodel                       # the device's build dir ($SYBORG_DEVICE / the registry default)
N95 = devmodel.paths(devmodel.load())['dir']
DEF = os.path.join(ROOT, 'build', 'epocroot', 'sf', 'os', 'kernelhwsrv', 'kernel', 'eka', 'eabi', 'ekernu.def')
SYM = os.path.join(ROOT, 'build', 'epocroot', 'epoc32', 'release', 'armv5', 'udeb', '_syborg_ekern.exe.sym')
BASE = 0x80000000


def demangle(names):
    out = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True).stdout.splitlines()
    return dict(zip(names, out))


def load_def():
    ords = {}
    for ln in open(DEF):
        m = re.match(r'\s+(\S+)\s+@\s+(\d+)\s+NONAME(\s+ABSENT)?', ln)
        if m and not m.group(3):
            ords[int(m.group(2))] = m.group(1)
    return ords


class Kernel:
    def __init__(self, img, index, path, hdr=None):
        self.d = open(img, 'rb').read()
        if hdr is None:
            hdr = [f for f in json.load(open(index))['files'] if f['path'].lower() == path.lower()][0]['addr']
        im = self.header(hdr)                    # read the live header, never a stale index
        self.code, self.codesize = im['code'], im['codesize']
        self.exp = [self.word(im['expdir'] + 4 * i) for i in range(im['expcount'])]
        self.byaddr = {}
        for i, a in enumerate(self.exp):
            self.byaddr.setdefault(a & ~1, i + 1)

    def header(self, a):     # TRomImageHeader: code @0x14, codesize @0x1c, expcount @0x3c, expdir @0x40
        w = lambda o: self.word(a + o)
        return {'code': w(0x14), 'codesize': w(0x1c), 'expcount': w(0x3c), 'expdir': w(0x40)}

    def word(self, a):
        o = a - BASE
        return struct.unpack('<I', self.d[o:o + 4])[0]

    def in_code(self, a):
        return self.code <= (a & ~1) < self.code + self.codesize


def vtable(k, a, limit=64):
    """slots after offset-to-top and typeinfo; stop at the first word that is not kernel code"""
    out = []
    for i in range(2, limit):
        v = k.word(a + 4 * i)
        if not k.in_code(v):
            break
        out.append(v)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--json')
    a = ap.parse_args()
    ords = load_def()
    n95 = Kernel(os.path.join(N95, 'core-flat.img'), os.path.join(N95, 'core.json'), 'Sys/Bin/ekern.exe')
    hdr = [int(l.split()[0], 16) for l in open(os.path.join(N95, 'hybrid-manifest.txt'))
           if l.split()[-1].lower() == 'sys/bin/ekern.exe'][0]
    ours = Kernel(os.path.join(N95, 'hybrid.img'), None, None, hdr)
    # our symbols (exported or not), for naming our slots
    syms = []
    out = subprocess.run(['nm', '-n', '--defined-only', SYM], capture_output=True, text=True).stdout
    for ln in out.splitlines():
        f = ln.split(' ', 2)
        if len(f) == 3 and f[1] in 'tTwW':
            syms.append(((int(f[0], 16) - 0x8000 + ours.code) & ~1, f[2]))
    syms.sort()
    saddr = [s[0] for s in syms]

    def name_ours(v):
        i = bisect.bisect_right(saddr, v & ~1) - 1
        if i >= 0 and syms[i][0] == v & ~1:
            return syms[i][1]
        return 'ours+%x' % (v - ours.code)

    def name_n95(v):
        o = n95.byaddr.get(v & ~1)
        if o and o in ords:
            return ords[o]
        return 'n95ekern+%x (not exported)' % (v - n95.code)

    report, tables = [], {}
    vt = sorted((o, n) for o, n in ords.items() if n.startswith('_ZTV') and 'cxxabiv1' not in n)
    allnames = set()
    rows = {}
    for o, n in vt:
        if o > len(n95.exp) or o > len(ours.exp):
            continue
        a_n, a_o = n95.exp[o - 1], ours.exp[o - 1]
        s_n = [name_n95(v) for v in vtable(n95, a_n)]
        s_o = [name_ours(v) for v in vtable(ours, a_o)]
        rows[n] = (o, s_n, s_o)
        allnames.update(s_n + s_o + [n])
    dm = demangle(sorted(allnames))
    for n, (o, s_n, s_o) in rows.items():
        cls = dm[n].replace('vtable for ', '')
        tables[cls] = {'ordinal': o, 'n95': [dm.get(x, x) for x in s_n], 'ours': [dm.get(x, x) for x in s_o]}
        same = len(s_n) == len(s_o) and all(x == y for x, y in zip(s_n, s_o))
        print('%-22s ord %d: N95 %d slots, ours %d: %s' % (cls, o, len(s_n), len(s_o), 'same' if same else 'DIFFERENT'))
        if not same:
            for i in range(max(len(s_n), len(s_o))):
                x = dm.get(s_n[i], s_n[i]) if i < len(s_n) else '-'
                y = dm.get(s_o[i], s_o[i]) if i < len(s_o) else '-'
                print('   %2d %s %-60s | %s' % (i, ' ' if x == y else '*', x[:60], y))
    if a.json:
        json.dump(tables, open(a.json, 'w'), indent=1)
    return 0




def disasm(k, addr, n=24):
    """ARM disassembly (llvm-mc) of n words at an exported function (Thumb not handled)"""
    words = ' '.join('0x%02x' % b for i in range(n) for b in struct.pack('<I', k.word((addr & ~1) + 4 * i)))
    out = subprocess.run(['llvm-mc', '--disassemble', '-triple=armv6-none-eabi'], input=words,
                         capture_output=True, text=True).stdout
    return [l.strip() for l in out.splitlines() if l.strip() and not l.strip().startswith('.')]


def func_main(names, n):
    ords = load_def()
    rev = {v: k for k, v in ords.items()}
    n95 = Kernel(os.path.join(N95, 'core-flat.img'), os.path.join(N95, 'core.json'), 'Sys/Bin/ekern.exe')
    hdr = [int(l.split()[0], 16) for l in open(os.path.join(N95, 'hybrid-manifest.txt'))
           if l.split()[-1].lower() == 'sys/bin/ekern.exe'][0]
    ours = Kernel(os.path.join(N95, 'hybrid.img'), None, None, hdr)
    for name in names:
        o = rev.get(name)
        if not o:
            print('%s: not in ekernu.def' % name); continue
        a, b = disasm(n95, n95.exp[o - 1], n), disasm(ours, ours.exp[o - 1], n)
        print('== %s (ord %d)  N95 %08x | ours %08x' % (name, o, n95.exp[o - 1], ours.exp[o - 1]))
        for i in range(max(len(a), len(b))):
            print('  %-44s | %s' % (a[i] if i < len(a) else '', b[i] if i < len(b) else ''))


if __name__ == '__main__' and len(sys.argv) > 2 and sys.argv[1] == '--func':
    func_main(sys.argv[2:], 20)

elif __name__ == '__main__':
    sys.exit(main())
