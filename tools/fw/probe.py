#!/usr/bin/env python3
"""Quick facts about an inventoried firmware (docs/adding-a-device.md steps 2-3), one subcommand each.
Works on build/devices/<id>/ as written by tools/fw/inventory.py (core-flat.img, core.json, rofs*.img);
prints only, nothing from the firmware leaves build/.

  probe.py [--device ID] header            ROM header, memory model, kernel limit, lowest user-side address
  probe.py [--device ID] stubs [--vs ID]   EUser.dll SWI stubs: count, shape (9.1 `mov ip, lr; swi` or 9.2
                                           `swi; bx lr`), ordered sequence compared with another device
  probe.py [--device ID] exec-sizes --vs ID   slow exec handlers whose copy-in sizes differ (structure changes)
  probe.py [--device ID] rofs              ROFS images: header, size, file count
  probe.py [--device ID] files [REGEX]     files of the core and ROFS images matching REGEX
  probe.py [--device ID] cat PATH          print one file (core or ROFS), e.g. Sys/Data/ESTART*.TXT
  probe.py [--device ID] display           wsini.ini screen lines (size, rotation, window mode)
  probe.py [--device ID] estart            ESTART*.TXT drive lines (letters, local drives, file systems)

Not covered (need a disassembler and judgement, see the doc): keypad matrix, display driver caps size."""
import argparse, json, os, re, struct, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'personality'))
import device as devmodel
import romimg, rofs


def paths(dev):
    p = devmodel.paths(devmodel.load(dev))
    if not os.path.exists(p['core_flat']):
        raise SystemExit('%s: no core-flat.img; run tools/fw/inventory.py --device %s first' % (p['dir'], dev or 'ID'))
    return p


def core(dev):
    p = paths(dev)
    data = open(p['core_flat'], 'rb').read()
    files = json.load(open(p['core_json']))['files'] if os.path.exists(p['core_json']) else None
    return p, romimg.Rom(data), files


def model(base):
    return {0xF8000000: 'moving (ARMv5; kernel.baseport "syborgmv")',
            0x80000000: 'multiple (kernel.baseport "syborg")'}.get(base, 'unknown')


def cmd_header(a):
    p, rom, files = core(a.device)
    print('base       0x%08x  -> memory model %s' % (rom.base, model(rom.base)))
    print('size       0x%08x' % rom.size)
    print('kernel data 0x%08x, kernel limit 0x%08x' % (rom.kerndata, rom.kernlimit))
    print('primary 0x%08x, secondary 0x%08x, variant 0x%08x, extensions 0x%08x' % (rom.primary, rom.secondary, rom.variant, rom.extension))
    if files:
        import romcompose
        romcompose.B = rom.base
        ks = romcompose.kernel_side(rom.d, files)
        user = [f for f in files if f['path'] not in ks and 'image' in f and rom.inrom(f['addr'])]
        kern = [f for f in files if f['path'] in ks and 'image' in f]
        if kern:
            k = max(kern, key=lambda f: f['addr'])
            print('kernel side: %d files, last %s at 0x%08x' % (len(kern), k['path'], k['addr']))
        if user:
            u = min(user, key=lambda f: f['addr'])
            print('lowest user-side file: %s at 0x%08x (firmware.rom.user_window_start candidate)' % (u['path'], u['addr']))
        fixed = [f['path'].split('/')[-1] for f in user if rom.kerndata <= f['image']['databsslin'] < rom.kernlimit]
        if fixed:
            print('fixed processes (data below the kernel limit): %s' % ', '.join(fixed))


def euser_stubs(dev):
    p, rom, files = core(dev)
    eu = [f for f in (files or []) if f['path'].lower().endswith('/euser.dll') and 'image' in f]
    if not eu:
        raise SystemExit('%s: no EUser.dll in core.json' % p['dir'])
    im = eu[0]['image']
    ws = struct.unpack_from('<%dI' % (im['codesize'] // 4), rom.d, im['code'] - rom.base)
    seq, shape = [], {'mov ip, lr; swi': 0, 'swi; bx lr': 0, 'other': 0}
    for i, w in enumerate(ws):
        if (w & 0xff000000) != 0xef000000:
            continue
        n = w & 0xffffff
        if 0x800000 <= n < 0x800100:
            n = 'F%02X' % (n - 0x800000)
        elif n < 0x200:
            n = 'S%02X' % n
        else:
            continue
        seq.append(n)
        if i and ws[i - 1] == 0xe1a0c00e:
            shape['mov ip, lr; swi'] += 1
        elif i + 1 < len(ws) and ws[i + 1] == 0xe12fff1e:
            shape['swi; bx lr'] += 1
        else:
            shape['other'] += 1
    return eu[0]['path'], im['code'], seq, shape


def cmd_stubs(a):
    path, code, seq, shape = euser_stubs(a.device)
    print('%s code 0x%08x: %d SWI sites (%d fast, %d slow)' % (path, code, len(seq), sum(1 for s in seq if s[0] == 'F'),
                                                              sum(1 for s in seq if s[0] == 'S')))
    print('shape: ' + ', '.join('%s %d' % kv for kv in shape.items()))
    print('  9.1 kernels return through ip (mov ip, lr); 9.2+ stubs return themselves (personalities/sym91/README.md)')
    if a.vs:
        _, _, other, oshape = euser_stubs(a.vs)
        n = next((i for i, (x, y) in enumerate(zip(seq, other)) if x != y), min(len(seq), len(other)))
        print('vs %s: %d SWI sites, shape %s' % (a.vs, len(other), ', '.join('%s %d' % kv for kv in oshape.items())))
        if n == len(seq) == len(other):
            print('ordered sequences identical: the exec map of %s fits' % a.vs)
        else:
            print('sequences agree for the first %d sites; then %s vs %s' % (n, seq[n:n + 6], other[n:n + 6]))
            extra = sorted(set(other) - set(seq))
            print('only in %s: %s' % (a.vs, ' '.join(extra[:40]) or '-'))
            mine = sorted(set(seq) - set(other))
            print('only in %s: %s' % (a.device or 'default', ' '.join(mine[:40]) or '-'))
            if [s for s in seq if s not in mine] == [s for s in other if s not in extra]:
                print('equal apart from those calls: the map of %s cut to the shared calls fits' % a.vs)


def cmd_exec_sizes(a):
    import exec_sizes
    pn, pk = paths(a.device), paths(a.vs)
    ta, na, ha = exec_sizes.kernel(pn['core_flat'], pn['core_json'])
    tb, nb, hb = exec_sizes.kernel(pk['core_flat'], pk['core_json'])
    print('%s: slow exec table 0x%08x, %d entries; %s: 0x%08x, %d entries' % (a.device or 'default', ta, na, a.vs, tb, nb))
    diff = [k for k in sorted(set(ha) & set(hb)) if ha[k] != hb[k]]
    for k in diff:
        print('S%02X  %-24s %s' % (k, [hex(x) for x in ha[k]], [hex(x) for x in hb[k]]))
    print('%d handlers differ (names: personalities/*/exec.map)' % len(diff))


def rofs_images(dev):
    for r in paths(dev)['rofs']:
        if os.path.exists(r):
            yield r, open(r, 'rb').read()


def cmd_rofs(a):
    for r, d in rofs_images(a.device):
        magic = d[:4].decode('latin1')
        hsz, ver = struct.unpack_from('<BB', d, 4)
        print('%-10s %s header %d bytes, version %d, %d bytes, %d files' % (
            os.path.basename(r), magic, hsz, ver, len(d), len(rofs.files(d)[1])))


def all_files(dev):
    p, rom, files = core(dev)
    for f in rom.walk():
        yield 'core', f['path'], lambda f=f: rom.bytes(f['addr'], f['size'])
    for r, d in rofs_images(dev):
        base, ents = rofs.files(d)
        for off, size, path in ents:
            o = off - base
            if 0 <= o < len(d):			# ROFx entries below base keep their data in ROFS1
                yield os.path.basename(r)[:-4], path, (lambda d=d, o=o, s=size: d[o:o + s])


def cmd_files(a):
    rx = re.compile(a.regex or '.', re.I)
    for src, path, _ in all_files(a.device):
        if rx.search(path):
            print('%-6s %s' % (src, path))


def find(dev, rx):
    rx = re.compile(rx, re.I)
    return [(s, p, get) for s, p, get in all_files(dev) if rx.search(p)]


def cmd_cat(a):
    hits = find(a.device, '^' + re.escape(a.path).replace('\\*', '.*') + '$')
    if not hits:
        raise SystemExit('no file %s' % a.path)
    for s, p, get in hits:
        print('== %s %s' % (s, p))
        sys.stdout.write(text(get()))


def text(b):
    if b[:2] in (b'\xff\xfe', b'\xfe\xff'):
        return b.decode('utf-16', 'replace')
    return b.decode('latin1')


def cmd_display(a):
    for s, p, get in find(a.device, r'(^|/)wsini\.ini$'):
        print('== %s %s' % (s, p))
        for l in text(get()).splitlines():
            if re.match(r'\s*(SCR_|SCREEN|WINDOWMODE|ROTATION|DEFAULT_ROTATION|FLICKERFREE|SIZE_MODE|SCREEN_ROTATION)', l, re.I):
                print('  ' + l.strip())


def cmd_estart(a):
    for s, p, get in find(a.device, r'estart[^/]*\.txt$'):
        print('== %s %s' % (s, p))
        for l in text(get()).splitlines():
            if re.match(r'\s*[A-Z]:\s', l):
                print('  ' + l.strip())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--device', help='device id or alias (default: $SYBORG_DEVICE / devices/default)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('header')
    s = sub.add_parser('stubs'); s.add_argument('--vs', help='compare with this device')
    s = sub.add_parser('exec-sizes'); s.add_argument('--vs', required=True, help='a device with a known personality')
    sub.add_parser('rofs')
    s = sub.add_parser('files'); s.add_argument('regex', nargs='?')
    s = sub.add_parser('cat'); s.add_argument('path')
    sub.add_parser('display')
    sub.add_parser('estart')
    a = ap.parse_args()
    globals()['cmd_' + a.cmd.replace('-', '_')](a)


if __name__ == '__main__':
    main()
