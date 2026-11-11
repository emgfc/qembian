#!/usr/bin/env python3
"""One N-Gage (EKA1) boot experiment on the ngage QEMU board, with a report.

Boots ROM.bin for SECS seconds with the catch-all log on, then asks the monitor for the CPU
state, stops QEMU and prints:
  - where the CPU is (PC, mode, CP15 control/TTB when the MMU is on);
  - mode switches / exceptions taken (count, first ones);
  - every physical 4 KB page the board does not model yet: reads/writes, address span,
    first values (the 'ngage:' lines of ngage.c's catch-all).
Each run's log stays in build/ngage/<name>.log for later questions (docs/eka1-ngage-questions.md).

usage: ngage_run.py [--secs 5] [--name runN] [--rom eka1/output/rom/ROM.bin] [-- extra qemu args]
"""
import argparse, collections, os, pathlib, re, socket, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parents[2]
QEMU = ROOT / 'build/qemu-ngage/build/qemu-system-arm'


def monitor(path, cmds):
    s = socket.socket(socket.AF_UNIX); s.connect(str(path)); s.settimeout(2)
    out = b''

    def drain():
        nonlocal out
        try:
            while True:
                b = s.recv(65536)
                if not b:
                    break
                out += b
                if out.rstrip().endswith(b'(qemu)'):
                    break
        except socket.timeout:
            pass
    drain()
    res = {}
    for c in cmds:
        out = b''
        s.sendall(c.encode() + b'\n'); drain()
        res[c] = out.decode('latin1')
    s.close()
    return res


# EKA1 kernel object containers (K:: globals, from the Find/FromHandle exports, Q16 era):
CONTAINERS = [('processes', 0x80000398), ('threads', 0x8000039c), ('chunks', 0x800003a0),
              ('semaphores', 0x800003a4), ('mutexes', 0x800003a8), ('ldds', 0x800003ac),
              ('pdds', 0x800003b0), ('channels', 0x800003b4), ('servers', 0x800003b8),
              ('sessions', 0x800003bc), ('timers', 0x800003c0), ('libraries', 0x800003c4)]


def read_words(mon, addr, n):
    out = monitor(mon, ['x/%dwx 0x%x' % (n, addr)])
    words = []
    for l in list(out.values())[0].splitlines():
        if re.match(r'^[0-9a-f]{8}:', l):
            words += [int(w, 16) for w in l.split(':', 1)[1].split()]
    return words[:n]


def read_name(mon, ptr):
    """An EKA1 object name: HBufC (length word, then UTF-16; descriptor type in the top bits)."""
    if not 0x80000000 <= ptr < 0x81000000:
        return '?'
    w = read_words(mon, ptr, 1)
    if not w:
        return '?'
    n = w[0] & 0x0fffffff
    if not 0 < n <= 64:
        return '?len %d' % n
    ws = read_words(mon, ptr + 4, (n * 2 + 3) // 4)
    b = b''.join(x.to_bytes(4, 'little') for x in ws)[:n * 2]
    return b.decode('utf-16-le', 'replace')


def print_objects(mon):
    """DObjectCon: +8 count, +16 array of DObject*; DObject: +8 owner, +16 name."""
    names = {}
    for kind, glob in CONTAINERS:
        con = read_words(mon, glob, 1)[0]
        hdr = read_words(mon, con, 6)
        count, arr = hdr[2], hdr[4]
        if not 0 <= count <= 512:
            print('%s: container %08x looks wrong (count %d)' % (kind, con, count)); continue
        objs = read_words(mon, arr, count) if count else []
        print('%s (%d):' % (kind, count))
        for o in objs:
            f = read_words(mon, o, 5)
            nm = read_name(mon, f[4]); names[o] = nm
            owner = names.get(f[2], '%08x' % f[2])
            print('   %08x  %-28s owner %s' % (o, nm, owner if kind != 'processes' else '-'))


def container(mon, glob):
    con = read_words(mon, glob, 1)[0]
    hdr = read_words(mon, con, 6)
    return read_words(mon, hdr[4], hdr[2]) if 0 < hdr[2] <= 512 else []


def rom_namer():
    import bisect, csv, json
    root = ROOT / 'eka1'
    man = json.loads((root / 'output/rom/manifest.json').read_text())
    files = sorted((f['rom_virtual_address'], f['size'], f['path'].split('/')[-1])
                   for f in man['files'] if f.get('rom_virtual_address'))
    tsv = root / 'analysis/user-exports.tsv'
    names = sorted((int(r['address'], 16), r['module'], r['name'])
                   for r in csv.DictReader(tsv.open(), delimiter='\t')) if tsv.exists() else []
    keys = [n[0] for n in names]

    def name(v):
        mod = next((p for va, sz, p in files if va <= v < va + sz), None)
        if not mod:
            return None
        j = bisect.bisect_right(keys, v) - 1
        if j >= 0 and names[j][1] == mod:
            return '%s %s+%x' % (mod, names[j][2][:60], v - names[j][0])
        return mod
    return name


def print_stack(mon, tname):
    """$STK chunk (DObject owner = the thread): +0x24 committed top, +0x28 base."""
    procs = {o: read_name(mon, read_words(mon, o, 5)[4]) for o in container(mon, 0x80000398)}
    threads = {}
    for o in container(mon, 0x8000039c):
        f = read_words(mon, o, 5)
        threads[o] = '%s::%s' % (procs.get(f[2], '?'), read_name(mon, f[4]))
    tid = next((o for o, n in threads.items() if n == tname or n.split('::')[1] == tname), None)
    if tid is None:
        print('stack %s: no such thread (%s)' % (tname, ', '.join(threads.values()))); return
    for c in container(mon, 0x800003a0):
        f = read_words(mon, c, 11)
        if (f[2] == tid and read_name(mon, f[4]) == '$STK') or (
                tname == 'Supervisor' and read_name(mon, f[4]) == 'SvStack'):
            top, base = f[9], f[10]
            break
    else:
        print('stack %s: no $STK chunk' % tname); return
    if base > top:
        # moving model: the process is not the running one, so its chunk is at its home
        # address (base); top is the run address of the committed end (run base 0x00400000)
        top = base + (top - 0x00400000)
    words = read_words(mon, base, (top - base) // 4)
    name = rom_namer()
    print('stack %s (%08x-%08x), ROM return-address candidates, deepest first:' % (tname, base, top))
    for i, v in enumerate(words):
        # ARM return addresses are word aligned; Thumb ones (EFile, EFSRV, ...) have bit 0 set
        if 0x5031af9c <= v < 0x51170000 and (v & 3 == 0 or v & 1):
            nm = name(v & ~1)
            if nm:
                print('   %08x: %08x %s%s' % (base + 4 * i, v, 'T ' if v & 1 else '', nm))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--secs', type=float, default=5)
    ap.add_argument('--name', default='run')
    ap.add_argument('--rom', default=str(ROOT / 'eka1/output/rom/ROM.bin'))
    ap.add_argument('--debug', default='unimp,guest_errors,int')
    ap.add_argument('--ppm', default=str(ROOT / 'eka1/extracted/RH2941.026'),
                    help='extracted PPM segments (the second flash file); "" = none (Q28)')
    ap.add_argument('--isi', default=str(ROOT / 'src/qemu-ngage/isi-replies.txt'),
                    help='ISI reply table for the NOSDRIVER channels (Q50); "" = none')
    ap.add_argument('--break', dest='brk', action='append', default=[],
                    help='hardware breakpoint (virtual address); gdb prints registers and stack '
                         'at the first hit, then the run ends')
    ap.add_argument('--watch', action='append', default=[],
                    help='write watchpoint, ADDR[:SKIP] (virtual; SKIP = hits to ignore first)')
    ap.add_argument('--mon', action='append', default=[],
                    help='QEMU monitor command at the end of the run (e.g. "xp/8wx 0x0a074000")')
    ap.add_argument('--objects', action='store_true',
                    help='at the end, list the kernel object containers (processes, threads, ...) '
                         'by name: the EKA1 "ps" (Q26)')
    ap.add_argument('--stack', action='append', default=[],
                    help='at the end, scan THREAD\'s stack for ROM return addresses (named)')
    ap.add_argument('--trace', action='append', default=[],
                    help='logging breakpoint VA=NAME: print r0-r3/lr at each hit and continue '
                         '(the run lasts --secs; output in <name>.trace)')
    ap.add_argument('--shot', help='save a PNG of the screen at the end of the run')
    ap.add_argument('--every', type=float, default=0,
                    help='with --shot X.png: also save X-<seconds>.png every this many seconds')
    ap.add_argument('--gdb-cmd', action='append', default=[], help='extra gdb command at the hit')
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()
    out = ROOT / 'build/ngage'; out.mkdir(parents=True, exist_ok=True)
    log, mon, ser = out / (a.name + '.log'), out / (a.name + '.mon'), out / (a.name + '.serial')
    for p in (log, mon, ser):
        p.unlink(missing_ok=True)
    mach = 'ngage' + (',ppm=%s' % a.ppm if a.ppm else '') + (',isi=%s' % a.isi if a.isi else '')
    cmd = [str(QEMU), '-M', mach, '-bios', a.rom, '-display', 'none', '-serial', 'file:%s' % ser,
           '-monitor', 'unix:%s,server,nowait' % mon, '-d', a.debug, '-D', str(log)] + a.extra
    if a.brk or a.watch or a.trace:
        s_ = socket.socket(); s_.bind(('127.0.0.1', 0)); port = s_.getsockname()[1]; s_.close()
        cmd += ['-gdb', 'tcp:127.0.0.1:%d' % port, '-S']
    q = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if a.trace:
        time.sleep(0.5)
        script = out / (a.name + '.gdb')
        lines = ['set architecture arm', 'set pagination off', 'set confirm off',
                 'target remote 127.0.0.1:%d' % port]
        for t in a.trace:
            # VA=NAME[;gdb command;...]: extra commands run at each hit (e.g. 'x/16sh $r1+4')
            va, _, rest = t.partition('=')
            nm, *extra = rest.split(';')
            lines += ['break *%s' % va, 'commands', 'silent',
                      'printf "%s r0=%%08x r1=%%08x r2=%%08x r3=%%08x lr=%%08x\\n", $r0, $r1, $r2, $r3, $lr' % (nm or va)]
            lines += extra + ['continue', 'end']
        lines += ['continue']
        script.write_text('\n'.join(lines) + '\n')
        tr = out / (a.name + '.trace')
        with tr.open('w') as fh:
            g = subprocess.Popen(['gdb', '-nx', '--batch', '-x', str(script)], stdout=fh, stderr=subprocess.STDOUT)
            time.sleep(a.secs)
            g.kill()
        hits = [l for l in tr.read_text(errors='replace').splitlines() if ' r0=' in l]
        print('trace: %d hits (%s)' % (len(hits), tr))
        for l in hits[-40:]:
            print('   ', l)
    elif a.brk or a.watch:
        time.sleep(0.5)
        g = ['gdb', '-nx', '--batch', '-ex', 'set architecture arm', '-ex', 'set pagination off',
             '-ex', 'target remote 127.0.0.1:%d' % port]
        for n, b in enumerate(a.brk, start=1):
            addr, _, cond = b.partition(':')      # VA[:gdb condition]
            g += ['-ex', 'hbreak *%s' % addr]
            if cond:
                g += ['-ex', 'condition %d %s' % (n, cond)]
        for n, wspec in enumerate(a.watch, start=len(a.brk) + 1):
            addr, _, skip = wspec.partition(':')
            g += ['-ex', 'watch *(unsigned short *)%s' % addr]
            if skip:
                g += ['-ex', 'ignore %d %s' % (n, skip)]
        g += ['-ex', 'continue', '-ex', 'info registers', '-ex', 'x/32wx $sp']
        for c in a.gdb_cmd:
            g += ['-ex', c]
        try:
            r = subprocess.run(g, capture_output=True, text=True, timeout=a.secs)
            print(r.stdout[-4000:])
        except subprocess.TimeoutExpired:
            print('gdb: no breakpoint hit within %s s' % a.secs)
    elif a.every and a.shot:
        t0 = time.time()
        while time.time() - t0 < a.secs:
            time.sleep(min(a.every, a.secs - (time.time() - t0)))
            n = int(round(time.time() - t0))
            try:
                monitor(mon, ['screendump %s -f png' % os.path.abspath(a.shot.replace('.png', '-%03d.png' % n))])
            except OSError:
                pass
    else:
        time.sleep(a.secs)
    state = {}
    if q.poll() is None:
        try:
            state = monitor(mon, ['info registers'])
            sp = re.search(r'R13=([0-9a-f]{8})', state.get('info registers', ''))
            if sp:
                state.update(monitor(mon, ['x/48wx 0x%s' % sp.group(1)]))
            if a.shot:
                monitor(mon, ['screendump %s -f png' % os.path.abspath(a.shot)])
                print('screen saved: %s' % a.shot)
            if a.objects:
                print_objects(mon)
            for t in a.stack:
                print_stack(mon, t)
            if a.mon:
                for k, v in monitor(mon, a.mon).items():
                    print('(qemu) %s\n%s' % (k, v.replace('(qemu)', '').strip()))
        except OSError as e:
            state = {'error': str(e)}
        q.terminate()
    try:
        q.wait(5)
    except subprocess.TimeoutExpired:
        q.kill()
    qout = q.stdout.read().decode('latin1').strip()

    print('== %s: %s' % (a.name, ' '.join(cmd[1:])))
    if qout:
        print('qemu:', qout[-600:])
    regs = state.get('info registers', '')
    m = re.findall(r'R(\d\d)=([0-9a-f]{8})', regs)
    if m:
        r = dict(m)
        print('PC=%s LR=%s SP=%s  %s' % (r.get('15'), r.get('14'), r.get('13'),
                                        ' '.join(re.findall(r'PSR=\S+ \S+ \S+', regs)) or regs.splitlines()[-2:]))
    elif state:
        print('monitor:', state)
    stack = [v for k, v in state.items() if k.startswith('x/')]
    if stack:
        words = re.findall(r'\b([0-9a-f]{8})\b', ' '.join(l.split(':', 1)[1] for l in stack[0].splitlines() if ':' in l))
        code = [w for w in words if 0x50000000 <= int(w, 16) < 0x51170000]
        print('stack words in ROM (return-address candidates, top first):', ' '.join(code[:16]))
    lines = ser.read_text('latin1').splitlines() if ser.exists() else []
    print('kernel debug output: %d lines (%s)' % (len(lines), ser))
    for l in lines[-25:]:
        print('   |', l)
    text = log.read_text('latin1') if log.exists() else ''
    ex = [l for l in text.splitlines() if l.startswith(('Taking exception', 'AArch32 mode switch', 'Exception return'))]
    print('exceptions/mode switches: %d' % len(ex))
    for l in ex[:8]:
        print('   ', l)
    pages = collections.OrderedDict()
    for l in text.splitlines():
        mm = re.match(r'ngage: ([RW]) ([0-9a-f]{8}) (\d) ([0-9a-f]+)', l)
        if not mm:
            continue
        k, ad, sz, v = mm.group(1), int(mm.group(2), 16), mm.group(3), mm.group(4)
        p = pages.setdefault(ad >> 12, {'R': 0, 'W': 0, 'lo': ad, 'hi': ad, 'first': []})
        p[k] += 1; p['lo'] = min(p['lo'], ad); p['hi'] = max(p['hi'], ad)
        if len(p['first']) < 4:
            p['first'].append('%s%x=%s' % (k, ad & 0xfff, v))
    print('unmodelled pages touched: %d' % len(pages))
    for pg, p in pages.items():
        print('  %08x  R%-5d W%-5d %08x-%08x  %s' % (pg << 12, p['R'], p['W'], p['lo'], p['hi'], ' '.join(p['first'])))


if __name__ == '__main__':
    main()
