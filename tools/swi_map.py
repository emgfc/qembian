#!/usr/bin/env python3
"""Recover a EUSER's executive-call (SWI) numbering, keyed by exported function names, and
compare two EUSERs (e.g. Nokia N95 XIP EUser.dll vs our GCC-built one).

genexec emits one ARM stub per exec call ("svc #n ; bx lr", possibly with a short prologue).
For every exported function we follow its body (ARM or Thumb, by export address bit 0) and
calls/tail-branches up to DEPTH levels and collect the SWI numbers reached. Matching export
names across two binaries gives SWI(old) -> SWI(new) pairs; the new numbers are named via our
execs.txt (tools/exec_table.py numbering).

  swi_map.py --rom-image ROM.img --at 0x8039d5f0 --names euser.dso [--names euseru.def] -o n95.json
  swi_map.py --elf _syborg_euser.dll.sym -o ours.json
  swi_map.py --compare n95.json ours.json --execs execs.txt
"""
import argparse, bisect, collections, json, os, re, struct, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dso_ordinals  # noqa: E402

OBJDUMP = os.environ.get('ARM_OBJDUMP', os.path.expanduser('~/gcc-builds/gcc-14.2.0/bin/arm-none-symbianelf-objdump'))
NM = OBJDUMP.replace('objdump', 'nm')
INSN = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{8})|(?:[0-9a-f]{4}(?: [0-9a-f]{4})?))\s+(\S+)\s*(.*)$')
DEPTH = 2
MAXBODY = 0x400


def valid_exec(n):
    # genexec: fast calls 0x00800000|n, slow calls n (both well below 0x200 in every release)
    return n < 0x200 or (n & 0xFFFF00) == 0x800000


def disasm(args):
    out = subprocess.run([OBJDUMP] + args, check=True, capture_output=True, text=True).stdout
    ins = {}
    for line in out.splitlines():
        m = INSN.match(line)
        if m:
            raw = m.group(2)
            # objdump prints Thumb encodings as 'xxxx' or 'xxxx xxxx', ARM as 'xxxxxxxx'
            mode_t = len(raw) != 8
            ins[int(m.group(1), 16)] = (raw.replace(' ', ''), m.group(3), m.group(4), mode_t)
    return ins


def target(op):
    m = re.match(r'(?:0x)?([0-9a-f]+)', op)
    return int(m.group(1), 16) if m else None


def is_return(mn, op):
    return (mn in ('bx',) and op.startswith('lr')) or \
        (mn.startswith(('pop', 'ldmfd', 'ldmia')) and 'pc' in op and mn in ('pop', 'ldmfd', 'ldmia', 'ldm'))


class Code:
    def __init__(self, arm, thumb):
        self.arm, self.thumb = arm, thumb
        self.arm_addrs = sorted(arm)
        self.thumb_addrs = sorted(thumb)
        # SWI stubs: entry range = [previous return + 4, swi]
        self.stub_entry = {}
        prev_ret = None
        for a in self.arm_addrs:
            raw, mn, op, mt = arm[a]
            if mt:
                continue
            w = int(raw, 16)
            if (w & 0xFF000000) == 0xEF000000 and valid_exec(w & 0xFFFFFF):
                lo = a
                k = bisect.bisect_left(self.arm_addrs, a) - 1
                while k >= 0 and a - self.arm_addrs[k] <= 16:
                    r2, m2, o2, _ = arm[self.arm_addrs[k]]
                    if is_return(m2, o2) or (int(r2, 16) & 0xFF000000) == 0xEF000000:
                        break
                    lo = self.arm_addrs[k]
                    k -= 1
                for e in range(lo, a + 1, 4):
                    self.stub_entry[e] = w & 0xFFFFFF
        self.swis = sorted(set(self.stub_entry.values()))

    def reach(self, addr, thumb, depth=DEPTH, seen=None):
        seen = seen if seen is not None else set()
        key = (addr, thumb)
        if key in seen:
            return set()
        seen.add(key)
        if not thumb and addr in self.stub_entry:
            return {self.stub_entry[addr]}
        tab, addrs = (self.thumb, self.thumb_addrs) if thumb else (self.arm, self.arm_addrs)
        out = set()
        i = bisect.bisect_left(addrs, addr)
        while i < len(addrs) and addrs[i] < addr + MAXBODY:
            a = addrs[i]
            raw, mn, op, mt = tab[a]
            i += 1
            if mn in ('bl', 'blx', 'b', 'b.w', 'bl.w') or (mn == 'b' and not thumb):
                t = target(op)
                if t is None:
                    continue
                tthumb = mt if mn != 'blx' else not mt
                if not tthumb and t in self.stub_entry:
                    out.add(self.stub_entry[t])
                elif depth > 0 and mn.startswith('bl'):
                    out |= self.reach(t & ~1, tthumb, depth - 1, seen)
                elif depth > 0 and mn.startswith('b') and not mn.startswith('bl'):
                    # unconditional tail branch outside the body = tail call
                    if t < addr or t >= addr + MAXBODY:
                        out |= self.reach(t & ~1, tthumb, depth - 1, seen)
                        break
            elif is_return(mn, op):
                break
        return out


def analyse(code, exports):
    res = {}
    for name, ea in exports.items():
        s = code.reach(ea & ~1, bool(ea & 1))
        if s:
            res[name] = sorted(s)
    return res


def rom_image(img, at, names):
    d = open(img, 'rb').read()
    rom_base = struct.unpack_from('<I', d, 140)[0]
    o = at - rom_base
    u1, u2, u3, uck, entry, code, data, codesize, textsize = struct.unpack_from('<9I', d, o)
    expcount, expdir = struct.unpack_from('<iI', d, o + 60)
    exps = struct.unpack_from('<%dI' % expcount, d, expdir - rom_base)
    tmp = os.path.join(os.path.dirname(os.path.abspath(img)), '.swi_map_text.bin')
    open(tmp, 'wb').write(d[code - rom_base:code - rom_base + textsize])
    arm = disasm(['-D', '-b', 'binary', '-m', 'arm', '--adjust-vma=0x%x' % code, tmp])
    thumb = disasm(['-D', '-b', 'binary', '-m', 'arm', '-M', 'force-thumb', '--adjust-vma=0x%x' % code, tmp])
    os.unlink(tmp)
    c = Code(arm, thumb)
    exports = {}
    for i, a in enumerate(exps, 1):
        nm = names.get(i, ('ord%d' % i, False))[0]
        if a and code <= (a & ~1) < code + textsize:
            exports[nm] = a
    res = analyse(c, exports)
    return dict(kind='rom', code=code, textsize=textsize, nexports=expcount,
                thumb_exports=sum(1 for a in exps if a & 1), swis=c.swis, byname=res)


def elf_image(path):
    syms = {}
    for line in subprocess.run([NM, '--defined-only', path], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in 'Tt' and not p[2].startswith('$'):
            syms.setdefault(p[2], int(p[0], 16))
    arm = disasm(['-d', path])
    c = Code(arm, arm)  # objdump uses $a/$t mapping symbols, so one listing covers both modes
    res = analyse(c, syms)
    return dict(kind='elf', swis=c.swis, byname=res)


def fmt(n):
    return ('F%02x' % (n & 0xFFFF)) if n & 0x800000 else ('S%02x' % n)


def compare(old, new, execs_path):
    import exec_table
    ours = exec_table.ours(execs_path)
    pairs = collections.defaultdict(collections.Counter)
    via = collections.defaultdict(list)
    for name, so in old['byname'].items():
        sn = new['byname'].get(name)
        if sn and len(so) == 1 and len(sn) == 1:
            pairs[so[0]][sn[0]] += 1
            via[so[0]].append(name)
    rows = []
    for o in old['swis']:
        c = pairs.get(o)
        if not c:
            rows.append(dict(old=o, new=None))
            continue
        (n, k), *rest = c.most_common()
        rows.append(dict(old=o, new=n, name=ours.get(n, '?'), votes=k, conflict=dict(rest) if rest else None,
                         example=via[o][0]))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom-image')
    ap.add_argument('--at', type=lambda s: int(s, 0))
    ap.add_argument('--names', action='append', default=[])
    ap.add_argument('--elf')
    ap.add_argument('--compare', nargs=2)
    ap.add_argument('--execs')
    ap.add_argument('-o', '--out')
    a = ap.parse_args()
    if a.compare:
        old, new = (json.load(open(p)) for p in a.compare)
        rows = compare(old, new, a.execs)
        print('# old(N95)  new(ours)  our exec name                       votes  example export')
        same = matched = 0
        for r in rows:
            if r['new'] is None:
                print('%-10s %-10s' % (fmt(r['old']), '-'))
                continue
            matched += 1
            same += r['old'] == r['new']
            print('%-10s %-10s %-35s %3d  %s%s%s' % (
                fmt(r['old']), fmt(r['new']), r['name'], r['votes'], 'SAME ' if r['old'] == r['new'] else '',
                ('CONFLICT %s ' % {fmt(k): v for k, v in r['conflict'].items()}) if r['conflict'] else '',
                r['example']))
        print('# N95 SWIs: %d; identified via shared exports: %d; same number as ours: %d'
              % (len(rows), matched, same))
        return
    if a.rom_image:
        names = {}
        for n in reversed(a.names):  # first --names wins
            names.update(dso_ordinals.load(n))
        r = rom_image(a.rom_image, a.at, names)
    else:
        r = elf_image(a.elf)
    if a.out:
        json.dump(r, open(a.out, 'w'), indent=1)
    print('%s: %d distinct SWIs (fast %d, slow %d), %d exports reach a SWI%s' % (
        a.rom_image or a.elf, len(r['swis']), sum(1 for s in r['swis'] if s & 0x800000),
        sum(1 for s in r['swis'] if not s & 0x800000), len(r['byname']),
        ('; %d/%d exports are Thumb' % (r['thumb_exports'], r['nexports'])) if 'nexports' in r else ''))


if __name__ == '__main__':
    main()
