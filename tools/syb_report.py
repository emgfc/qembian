#!/usr/bin/env python3
"""syb-report: summarise a hybrid-boot serial log (kernel debug port).

  syb_report.py SERIAL.LOG [--full] [--ipc-errors]

Sections: timeline milestones, processes (created / exited / panicked), SYBHACK and faults,
caught kernel exceptions per thread, startup properties (SYBPROP), stub driver activity
(SYBSTUB), ISI requests without reply (tools/isi.py names), and -- when the log has the IPC
trace (trace bits 0xC00) -- IPC requests still pending at the end of the log, i.e. who is
waiting on whom. Lines may carry a leading "<seconds> " timestamp (syb_run.py writes that).
"""
import collections, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))

TS = re.compile(r'^\s*(\d+\.\d+) (.*)$')
MSGAC = re.compile(r'^MsgAc: M:(-?\d+) (\S+)->(\S+)')
MSGCO = re.compile(r'^MsgCo: M:(-?\d+) r:(-?\d+) (\S+)->(\S+)')
CREATE = re.compile(r'^>DMemModelProcess::DoCreate (\S+)')
DIE = re.compile(r'^Process (\S+) Die: (\d+) (-?\d+) (\S*)')
PANIC = re.compile(r'^Thread (\S+) Panic (\S+ ?-?\d*)')
KILLED = re.compile(r'^Thread (\S+) Killed (-?\d+)')
STUB = re.compile(r'^SYBSTUB: (\S+) u-?\d+ (ctl \d+|request 0x[0-9a-f]+)\b.*?(ignored|returned|wrote int|wrote block|completed|pending|hook)')
STUBOPEN = re.compile(r'^SYBSTUB: (\S+) open unit .* by (\S+)')


def read(path):
    out = []
    for raw in open(path, errors='replace'):
        raw = raw.rstrip('\n').replace('\r', '')
        m = TS.match(raw)
        out.append((float(m.group(1)), m.group(2)) if m else (None, raw))
    return out


def fmt_t(t):
    return '%7.2fs' % t if t is not None else '      -'


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ('-h', '--help'):
        print(__doc__); return
    path = sys.argv[1]
    full = '--full' in sys.argv
    lines = read(path)
    last_t = max([t for t, _ in lines if t is not None] or [0])
    print('# %s: %d lines%s' % (path, len(lines), ', %.1f s' % last_t if last_t else ''))

    created, died, panics, hacks, faults, props = [], [], [], [], [], []
    exc = collections.Counter()
    stub = collections.Counter()
    stub_open = collections.Counter()
    pending = collections.OrderedDict()   # (client, fn) -> [t, server, line#]
    server_threads = {}
    ipc_err = collections.Counter()
    ipc_seen = False
    last_act = {}
    cur_exc = False
    for n, (t, l) in enumerate(lines):
        m = MSGAC.match(l)
        if m:
            ipc_seen = True
            fn, cl, sv = int(m.group(1)), m.group(2), m.group(3)
            pending.setdefault((cl, fn), []).append((t, sv, n))
            last_act[cl] = (t, n)
            continue
        m = MSGCO.match(l)
        if m:
            fn, r, st, cl = int(m.group(1)), int(m.group(2)), m.group(3), m.group(4)
            q = pending.get((cl, fn))
            if q:
                _, sv, _ = q.pop(0)
                server_threads[sv] = st
                if r < 0:
                    ipc_err[(cl, sv, fn, r)] += 1
            continue
        m = CREATE.match(l)
        if m:
            created.append((t, m.group(1))); continue
        m = DIE.match(l)
        if m:
            died.append((t, m.group(1), m.group(2), m.group(3), m.group(4))); continue
        m = PANIC.match(l)
        if m:
            panics.append((t, m.group(1), m.group(2))); continue
        m = KILLED.match(l)
        if m:
            panics.append((t, m.group(1), 'Killed ' + m.group(2))); continue
        if l.startswith('SYBHACK') or l.startswith('FAULT') or 'Fault(' in l:
            (faults if l.startswith('FAULT') else hacks).append((t, l)); continue
        if l.startswith('SYBPROP'):
            props.append((t, l[9:])); continue
        if l.startswith('Exc ') and 'Cpsr=' in l:
            cur_exc = True; continue
        if cur_exc and l.startswith('Thread full name='):
            exc[l.split('=', 1)[1]] += 1; cur_exc = False; continue
        m = STUBOPEN.match(l)
        if m:
            stub_open[(m.group(1), m.group(2))] += 1; continue
        m = STUB.match(l)
        if m:
            stub[(m.group(1), m.group(2), m.group(3))] += 1

    def section(title):
        print('\n## ' + title)

    section('Processes created (%d)' % len(created))
    print('  ' + ' '.join(n for _, n in created))
    section('Exits, panics, kills')
    for t, p, typ, r, cat in died:
        if typ != '0' or r != '0':
            print('  %s %s exit type %s reason %s %s' % (fmt_t(t), p, typ, r, cat))
    for t, th, why in panics:
        print('  %s %s %s' % (fmt_t(t), th, why))
    normal = [p for _, p, typ, r, _ in died if typ == '0' and r == '0']
    if normal:
        print('  normal exits: ' + ', '.join(normal))
    section('SYBHACK / faults')
    for t, l in hacks + faults:
        print('  %s %s' % (fmt_t(t), l))
    if exc:
        section('Caught kernel exceptions (bad user pointers in exec calls), per thread')
        for th, c in exc.most_common():
            print('  %4d  %s' % (c, th))
    section('Startup properties (KPSUidStartup)')
    for t, l in props:
        print('  %s %s' % (fmt_t(t), l))
    section('Stub drivers')
    for (dev, cl), c in sorted(stub_open.items()):
        print('  open %-16s by %s%s' % (dev, cl, ' x%d' % c if c > 1 else ''))
    for (dev, what, res), c in sorted(stub.items()):
        print('  %-16s %-14s %-10s x%d' % (dev, what, res, c))
    section('ISI')
    import subprocess
    out = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'isi.py'), 'log', path],
                         capture_output=True, text=True).stdout
    print('  ' + out.strip().replace('\n', '\n  '))
    if ipc_seen:
        section('IPC requests still pending at the end (who waits on whom)')
        blocked, other = [], collections.Counter()
        for (cl, fn), q in pending.items():
            for t, sv, n in q:
                if fn < 0:
                    continue
                if last_act.get(cl, (None, -1))[1] == n:
                    blocked.append((t if t is not None else n, cl, sv, fn))
                else:
                    other[(sv, fn)] += 1
        blocked.sort(key=lambda r: (r[0] if isinstance(r[0], float) else 0))
        print('  likely blocked (the client issued nothing after this request):')
        for t, cl, sv, fn in blocked:
            print('    %-9s %-44s -> %-26s M:%d' % (
                fmt_t(t) if isinstance(t, float) else '#%d' % t, cl, sv, fn))
        print('  other pending (async notifications etc.), by server and function:')
        line = []
        for (sv, fn), c in sorted(other.items(), key=lambda kv: -kv[1]):
            line.append('%s M:%d x%d' % (sv, fn, c))
        print('    ' + '; '.join(line[:40]) + (' ...' if len(line) > 40 else ''))
        if '--ipc-errors' in sys.argv or full:
            section('IPC requests completed with errors')
            for (cl, sv, fn, r), c in sorted(ipc_err.items(), key=lambda kv: -kv[1])[:60]:
                print('  %-44s -> %-26s M:%-5d r:%-4d x%d' % (cl, sv, fn, r, c))
    else:
        print('\n(no IPC trace in this log: run with --trace 0x80000C40 for the wait graph)')


if __name__ == '__main__':
    main()
