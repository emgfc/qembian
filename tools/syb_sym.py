#!/usr/bin/env python3
"""syb-sym: symbol and address queries over the N95 core ROM (cached index).

  syb_sym.py addr ADDR [ADDR...]        module+offset, export/function, thunk target, literal
  syb_sym.py sym REGEX                  exports whose (demangled) name matches -> address
  syb_sym.py callers SYMBOL|ADDR [--sites] [--module M]
                                        modules importing an export, and BL/BLX call sites
  syb_sym.py str TEXT [--refs]          UTF-16/ASCII string occurrences (+ code that references them)
  syb_sym.py read ADDR [N]              N words at ADDR, each symbolised
  syb_sym.py lit ADDR                   decode a TLitC16/TLitC8 at ADDR (or pointed to by ADDR)
  syb_sym.py fn MODULE ADDR|NAME        print one decompiled function (runs Ghidra if needed)
  syb_sym.py mod MODULE                 module summary (range, entry, exports, imports)
  syb_sym.py reindex                    rebuild the cache (build/n95/symindex.json)

ADDR is hex (0x prefix optional). Module names are file names (case-insensitive).
The index is built from build/n95/core.json + core-flat.img + the S60 SDK import libraries; Ghidra
function starts are taken from build/n95/ghidra/*.c when present.
"""
import bisect, json, os, re, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as devmodel  # tools/device.py ($SYBORG_DEVICE / devices/default)
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import dso_ordinals  # noqa: E402

N95 = devmodel.paths(devmodel.load())['dir']   # the device's extracted firmware (build/devices/<id>)
IMG = os.path.join(N95, 'core-flat.img')
JSN = os.path.join(N95, 'core.json')
IDX = os.path.join(N95, 'symindex.json')
GHI = os.path.join(N95, 'ghidra')
SDK = os.path.expanduser('~/work/S60_3rd_FP1/Epoc32/release/armv5/lib')
B = 0x80000000


def demangle(names):
    names = list(names)
    if not names:
        return []
    try:
        out = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True).stdout
        dm = out.split('\n')
        return dm[:len(names)] if len(dm) >= len(names) else names
    except OSError:
        return names


class Rom:
    def __init__(self):
        self.d = open(IMG, 'rb').read()
        if not os.path.exists(IDX) or os.path.getmtime(IDX) < os.path.getmtime(JSN):
            self.build()
        self.idx = json.load(open(IDX))
        self.mods = self.idx['modules']
        self.mods.sort(key=lambda m: m['code'])
        self.starts = [m['code'] for m in self.mods]
        self.byname = {m['name'].lower(): m for m in self.mods}
        ex = [(e[1] & ~1, m['name'], e[0], e[2], e[3] if len(e) > 3 else e[2]) for m in self.mods for e in m['exports']]
        ex.sort()
        self.ex, self.exaddr = ex, [e[0] for e in ex]
        self._gfuncs = {}
        self._ours()

    def _ours(self):
        """Our kernel-side files sit below the N95 user window in the hybrid ROM: resolve them from
        our own image (build/n95/ours.json) and ekern's ELF symbols, not from the N95 index."""
        self.ours_end, self.ours, self.ksyms = 0, [], []
        man = os.path.join(N95, 'hybrid-manifest.txt')
        oj = os.path.join(N95, 'ours.json')
        if not (os.path.exists(man) and os.path.exists(oj)):
            return
        n95 = [int(l.split()[0], 16) for l in open(man) if ' N95 ' in l]
        self.ours_end = min(n95) if n95 else 0
        for f in json.load(open(oj))['files']:
            im = f.get('image')
            if im and im['code'] < self.ours_end:
                self.ours.append((im['code'], im['code'] + im['codesize'], f['path'].split('/')[-1]))
        self.ours.sort()
        ek = [o for o in self.ours if o[2].lower() == 'ekern.exe']
        sym = os.path.join(ROOT, 'build', 'epocroot', 'epoc32', 'release', 'armv5', 'udeb', '_syborg_ekern.exe.sym')
        if ek and os.path.exists(sym):
            out = subprocess.run(['nm', '-n', '-C', '--defined-only', sym], capture_output=True, text=True).stdout
            for ln in out.splitlines():
                f = ln.split(' ', 2)
                if len(f) == 3 and f[1] in 'tT':
                    self.ksyms.append((int(f[0], 16) - 0x8000 + ek[0][0], f[2]))
            self.ksyms.sort()

    def describe_ours(self, a):
        i = bisect.bisect_right([o[0] for o in self.ours], a) - 1
        if i < 0 or a >= self.ours[i][1]:
            return 'our kernel side %08x' % a
        s = 'OURS %s+%x' % (self.ours[i][2], a - self.ours[i][0])
        if self.ksyms and self.ours[i][2].lower() == 'ekern.exe':
            k = bisect.bisect_right([x[0] for x in self.ksyms], a) - 1
            if k >= 0:
                s += '  %s+%x' % (self.ksyms[k][1], a - self.ksyms[k][0])
        return s

    # ---- index -------------------------------------------------------------------------------
    def build(self):
        d = open(IMG, 'rb').read()
        j = json.load(open(JSN))
        sdk = {n.lower(): n for n in os.listdir(SDK)} if os.path.isdir(SDK) else {}
        mods = []
        for f in j['files']:
            im = f.get('image')
            if not im or not im['codesize']:
                continue
            name = f['path'].split('/')[-1]
            dso = os.path.splitext(name)[0].lower() + '.dso'
            names = {}
            if dso in sdk:
                try:
                    names = dso_ordinals.load(os.path.join(SDK, sdk[dso]))
                except Exception:
                    names = {}
            exps = []
            if im['expcount'] and im['expdir'] >= B:
                for k, a in enumerate(struct.unpack_from('<%dI' % im['expcount'], d, im['expdir'] - B), 1):
                    if a:
                        exps.append([k, a, names.get(k, (None,))[0] or '%s_ord%d' % (os.path.splitext(name)[0], k)])
            code, size = im['code'], im['codesize']
            words = struct.unpack_from('<%dI' % (size // 4), d, code - B)
            thunks = {}
            for k in range(len(words) - 1):
                if words[k] == 0xE51FF004:
                    thunks[code + 4 * k] = words[k + 1]
            mods.append({'name': name, 'path': f['path'], 'code': code, 'end': code + size,
                         'text': code + im['textsize'], 'entry': im['entry'], 'data': im['data'],
                         'exports': exps, 'thunks': {str(k): v for k, v in thunks.items()}})
        # demangle all export names once
        allnames = sorted({e[2] for m in mods for e in m['exports'] if e[2].startswith('_Z')})
        dm = dict(zip(allnames, demangle(allnames)))
        for m in mods:
            for e in m['exports']:
                e.append(dm.get(e[2], e[2]))
        json.dump({'modules': mods}, open(IDX, 'w'))
        print('indexed %d modules' % len(mods), file=sys.stderr)

    # ---- lookups -----------------------------------------------------------------------------
    def module_at(self, a):
        i = bisect.bisect_right(self.starts, a) - 1
        if i >= 0 and self.mods[i]['code'] <= a < self.mods[i]['end']:
            return self.mods[i]
        return None

    def export_of(self, a):
        i = bisect.bisect_right(self.exaddr, a & ~1) - 1
        return self.ex[i] if i >= 0 else None

    def gfuncs(self, m):
        n = m['name']
        if n not in self._gfuncs:
            fs = []
            for cand in (n + '.c', os.path.splitext(n)[0] + '.c'):
                p = os.path.join(GHI, cand)
                if os.path.exists(p):
                    for mm in re.finditer(r'// ==== (\S+) @ ([0-9a-f]{8})', open(p, errors='replace').read()):
                        fs.append((int(mm.group(2), 16), mm.group(1)))
                    break
            fs.sort()
            self._gfuncs[n] = fs
        return self._gfuncs[n]

    def word(self, a):
        if B <= a < B + len(self.d) - 3:
            return struct.unpack_from('<I', self.d, a - B)[0]
        return None

    def literal(self, a):
        """Decode a TLitC16/TLitC8 at a (length word then data)."""
        n = self.word(a)
        if n is None or not 0 < n < 512:
            return None
        o = a - B + 4
        raw16 = self.d[o:o + 2 * n]
        if len(raw16) == 2 * n and all(32 <= raw16[i] < 127 and raw16[i + 1] == 0 for i in range(0, 2 * n, 2)):
            return 'L"%s"' % raw16.decode('utf-16le')
        raw8 = self.d[o:o + n]
        if len(raw8) == n and all(32 <= c < 127 for c in raw8):
            return '"%s"' % raw8.decode()
        return None

    def describe(self, a, short=False):
        if B <= a < self.ours_end:
            return self.describe_ours(a & ~1)
        m = self.module_at(a & ~1)
        if not m:
            if 0xc0000000 <= a:
                return 'kernel data/heap %08x' % a
            if a < 0x40000000:
                return 'user RAM %08x' % a
            return 'unknown %08x' % a
        off = (a & ~1) - m['code']
        parts = ['%s+%x' % (m['name'], off)]
        t = m['thunks'].get(str(a & ~3))
        if t is not None:
            parts.append('import thunk -> ' + self.describe(t, short=True))
        e = self.export_of(a)
        if e and e[1] == m['name'] and (a & ~1) - e[0] < 0x4000:
            nm = e[4] if len(e) > 4 else e[3]
            parts.append('%s%s' % (nm, '+%x' % ((a & ~1) - e[0]) if (a & ~1) != e[0] else ''))
        fs = self.gfuncs(m)
        if fs:
            i = bisect.bisect_right([f[0] for f in fs], a & ~1) - 1
            if i >= 0:
                parts.append('in %s+%x' % (fs[i][1], (a & ~1) - fs[i][0]))
        if not short and (a & ~1) >= m['text']:
            lit = self.literal(a) or (self.literal(self.word(a)) if self.word(a) else None)
            if lit:
                parts.append('literal ' + lit)
        return '  '.join(parts)


def parse_addr(s):
    return int(s, 16)


def cmd_addr(r, args):
    for s in args:
        print('%08x  %s' % (parse_addr(s), r.describe(parse_addr(s))))


def cmd_sym(r, args):
    rx = re.compile(args[0], re.I)
    n = 0
    for a, mod, o, mangled, *dm in r.ex:
        nm = dm[0] if dm else mangled
        if rx.search(nm) or rx.search(mangled):
            print('%08x  %-24s ord %-4d %s' % (a, mod, o, nm))
            n += 1
            if n > 200:
                print('... (more)'); break


def resolve_target(r, s):
    try:
        return parse_addr(s)
    except ValueError:
        rx = re.compile(re.escape(s) if not s.startswith('^') else s, re.I)
        hits = [e for e in r.ex if rx.search(e[4] if len(e) > 4 else e[3]) or rx.search(e[3])]
        if not hits:
            raise SystemExit('no export matches ' + s)
        if len(hits) > 1:
            print('# %d matches, using the first: %s!%s' % (len(hits), hits[0][1], hits[0][4] if len(hits[0]) > 4 else hits[0][3]))
        return hits[0][0]


def bl_targets(r, m):
    """(site, target) for Thumb BL/BLX pairs and ARM BL/BLX in a module's text."""
    d, code, text = r.d, m['code'], m['text']
    hw = struct.unpack_from('<%dH' % ((text - code) // 2), d, code - B)
    out = []
    for i in range(len(hw) - 1):
        h1, h2 = hw[i], hw[i + 1]
        if (h1 & 0xf800) == 0xf000 and (h2 & 0xe800) == 0xe800:
            off = ((h1 & 0x7ff) << 12) | ((h2 & 0x7ff) << 1)
            if off & 0x400000:
                off -= 0x800000
            pc = code + 2 * i + 4
            tgt = pc + off
            if (h2 & 0x1800) == 0x0800:   # BLX -> ARM, word aligned
                tgt &= ~3
            out.append((code + 2 * i, tgt))
    for i in range(0, (text - code) // 4):
        w = struct.unpack_from('<I', d, code - B + 4 * i)[0]
        if (w & 0x0f000000) == 0x0b000000 and (w >> 28) != 0xf:
            off = (w & 0xffffff) << 2
            if off & 0x2000000:
                off -= 0x4000000
            out.append((code + 4 * i, code + 4 * i + 8 + off))
    return out


def cmd_callers(r, args):
    sites = '--sites' in args
    only = None
    if '--module' in args:
        only = args[args.index('--module') + 1].lower()
    tgt = resolve_target(r, args[0])
    print('# target %08x %s' % (tgt, r.describe(tgt, short=True)))
    total = 0
    for m in r.mods:
        if only and m['name'].lower() != only:
            continue
        thunks = [int(a) for a, t in m['thunks'].items() if (t & ~1) == (tgt & ~1)]
        own = m['code'] <= tgt < m['end']
        if not thunks and not own:
            continue
        want = set(thunks) | ({tgt & ~1} if own else set())
        calls = [s for s, t in bl_targets(r, m) if t in want]
        total += len(calls)
        print('%-28s thunks %s  call sites %d' % (m['name'], ' '.join('%08x' % t for t in thunks) or '-', len(calls)))
        if sites:
            for s in calls:
                print('    %08x  %s' % (s, r.describe(s, short=True)))
    print('# %d call sites' % total)


def cmd_str(r, args):
    text = args[0]
    refs = '--refs' in args
    d = r.d
    for enc, pat in (('utf16', text.encode('utf-16le')), ('ascii', text.encode())):
        i = 0
        while True:
            i = d.find(pat, i)
            if i < 0:
                break
            a = i + B
            m = r.module_at(a)
            print('%08x  %-5s %s  %s' % (a, enc, r.describe(a, short=True), r.literal(a - 4) or ''))
            if refs and m:
                for ref in (a, a - 4):
                    pat2 = struct.pack('<I', ref)
                    j = m['code'] - B
                    while True:
                        j = d.find(pat2, j, m['end'] - B)
                        if j < 0:
                            break
                        print('      referenced at %08x  %s' % (j + B, r.describe(j + B, short=True)))
                        j += 4
            i += 2


def cmd_read(r, args):
    a = parse_addr(args[0])
    n = int(args[1]) if len(args) > 1 else 8
    for k in range(n):
        w = r.word(a + 4 * k)
        if w is None:
            break
        desc = r.describe(w, short=True) if (B <= w < B + len(r.d)) else ''
        lit = r.literal(w) if (B <= w < B + len(r.d)) else None
        print('%08x: %08x  %s %s' % (a + 4 * k, w, desc, lit or ''))


def cmd_lit(r, args):
    a = parse_addr(args[0])
    print(r.literal(a) or r.literal(r.word(a) or 0) or 'not a literal')


def cmd_fn(r, args):
    m = r.byname.get(args[0].lower())
    if not m:
        raise SystemExit('no module ' + args[0])
    c = None
    for cand in (m['name'] + '.c', os.path.splitext(m['name'])[0] + '.c'):
        if os.path.exists(os.path.join(GHI, cand)):
            c = os.path.join(GHI, cand)
            break
    if not c:
        print('# decompiling %s with Ghidra ...' % m['name'], file=sys.stderr)
        subprocess.run([os.path.join(ROOT, 'tools', 'ghidra', 'decompile.sh'), m['name']],
                       capture_output=True)
        c = os.path.join(GHI, m['name'] + '.c')
        r._gfuncs.pop(m['name'], None)
    src = open(c, errors='replace').read()
    key = args[1]
    try:
        a = parse_addr(key)
        fs = r.gfuncs(m)
        i = bisect.bisect_right([f[0] for f in fs], a & ~1) - 1
        key = fs[i][1] if i >= 0 else key
    except ValueError:
        pass
    mm = re.search(r'// ==== %s @ [0-9a-f]{8}.*?(?=\n// ==== |\Z)' % re.escape(key), src, re.S)
    print(mm.group(0) if mm else 'function %s not found in %s' % (key, c))


def cmd_mod(r, args):
    m = r.byname.get(args[0].lower())
    if not m:
        raise SystemExit('no module ' + args[0])
    print('%s  code %08x-%08x text-end %08x entry %08x data %08x' % (m['path'], m['code'], m['end'], m['text'], m['entry'], m['data']))
    print('exports %d, import thunks %d' % (len(m['exports']), len(m['thunks'])))
    deps = {}
    for a, t in m['thunks'].items():
        tm = r.module_at(t & ~1)
        deps[tm['name'] if tm else '?'] = deps.get(tm['name'] if tm else '?', 0) + 1
    print('imports from: ' + ', '.join('%s(%d)' % kv for kv in sorted(deps.items(), key=lambda kv: -kv[1])))


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ('-h', '--help'):
        print(__doc__); return
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == 'reindex':
        if os.path.exists(IDX):
            os.unlink(IDX)
        Rom(); return
    r = Rom()
    {'addr': cmd_addr, 'sym': cmd_sym, 'callers': cmd_callers, 'str': cmd_str, 'read': cmd_read,
     'lit': cmd_lit, 'fn': cmd_fn, 'mod': cmd_mod}[cmd](r, args)


if __name__ == '__main__':
    main()
