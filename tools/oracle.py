#!/usr/bin/env python3
"""Compare a real phone's oracle recording with a qembian boot of the same firmware
(docs/real-phone-tracing.md).

  oracle.py compare ORACLE_LOG SERIAL_LOG     # E:\\oracle\\boot.log from the phone vs <session>/serial.log
  oracle.py diff PHONE_LOG QEMBIAN_ORACLE_LOG  # two recordings (the same recorder run in qembian):
                                              # processes on one side only, abnormal exits, P&S
  oracle.py show ORACLE_LOG                   # the recording, deaths and P&S changes only
  oracle.py keys SERIAL_LOG > keys.txt        # the P&S keys a qembian boot sets: the recorder's key list
                                              # (E:\\oracle\\keys.txt on the phone; without it the
                                              # recorder watches its built-in N95 list)

Reported:
  - P&S keys whose sequence of values differs (values in order, first time of each), and keys only one
    side changed;
  - abnormal thread exits (panics, kills with a reason other than 0) on one side only, or different;
Known blind spots of the recording: P&S values that change twice within a millisecond show only the
last value (subscriptions coalesce); qembian's log has no thread list, so thread starts are not compared.
Times are seconds since boot on each side; compare orders and gaps, not absolute values.
"""
import re
import sys

PROP_O = re.compile(r'^([\d.]+) ORACLE: prop ([0-9a-f]+)/([0-9a-f]+) = (-?\d+)')
PROP_Q = re.compile(r'^([\d.]+) SYBPROP: .* sets ([0-9a-f]+)/([0-9a-f]+) = (-?\d+)')
TPLUS_O = re.compile(r'^([\d.]+) ORACLE: thread\+ (.+)$')
TMINUS_O = re.compile(r'^([\d.]+) ORACLE: thread- (.+?) (kill|terminate|panic) (-?\d+) ?(.*)$')
DEATH_Q = re.compile(r'^([\d.]+) Thread (.+?) (Panic|Killed|Terminated) (?:(.+) )?(-?\d+)\s*$')
CODE_Q = re.compile(r'^([\d.]+) SYBCODE: \S+\\(\S+?) code=')


def norm(name):
    """EStart.exe[10272c04]0001::Main -> EStart.exe::Main"""
    return re.sub(r'\[[0-9a-f]{8}\]\d{4}', '', name.strip()).lower()


def lines(path):
    with open(path, 'rb') as f:
        return f.read().decode('latin1').replace('\r', '').split('\n')


def oracle(path):
    props, deaths, threads = {}, {}, {}
    for l in lines(path):
        m = PROP_O.match(l)
        if m:
            props.setdefault((m.group(2), m.group(3)), []).append((float(m.group(1)), int(m.group(4))))
            continue
        m = TMINUS_O.match(l)
        if m:
            kind, reason, cat = m.group(3), int(m.group(4)), m.group(5).strip()
            if kind == 'panic' or reason != 0:
                deaths[norm(m.group(2))] = (float(m.group(1)), '%s %s %d' % (kind, cat, reason))
            continue
        m = TPLUS_O.match(l)
        if m:
            threads.setdefault(norm(m.group(2)), float(m.group(1)))
    return props, deaths, threads


def qembian(path):
    props, deaths, procs = {}, {}, set()
    for l in lines(path):
        m = PROP_Q.match(l)
        if m:
            props.setdefault((m.group(2), m.group(3)), []).append((float(m.group(1)), int(m.group(4))))
            continue
        m = DEATH_Q.match(l)
        if m:
            kind = {'Panic': 'panic', 'Killed': 'kill', 'Terminated': 'terminate'}[m.group(3)]
            reason = int(m.group(5))
            if kind == 'panic' or reason != 0:
                deaths[norm(m.group(2))] = (float(m.group(1)), '%s %s %d' % (kind, (m.group(4) or '').strip(), reason))
            continue
        m = CODE_Q.match(l)
        if m and m.group(2).lower().endswith('.exe'):
            procs.add(m.group(2).lower())
    return props, deaths, procs


def seq(vals):
    out, last = [], None
    for t, v in vals:
        if v != last:
            out.append((t, v))
            last = v
    return out


def compare(opath, qpath):
    op, od, _ = oracle(opath)
    qp, qd, qprocs = qembian(qpath)
    print('# P&S: keys whose value sequence differs (phone | qembian)')
    n = 0
    for k in sorted(set(op) | set(qp)):
        a, b = seq(op.get(k, [])), seq(qp.get(k, []))
        av, bv = [v for _, v in a], [v for _, v in b]
        # the oracle's first entry is the value it found at start (initial): qembian logs the set instead
        if av == bv or (len(av) and av[1:] == bv) or (len(av) and bv[-len(av):] == av):
            continue
        n += 1
        fa = ' '.join('%d@%.1f' % (v, t) for t, v in a) or '-'
        fb = ' '.join('%d@%.1f' % (v, t) for t, v in b) or '-'
        print('%s/%s\n  phone   %s\n  qembian %s' % (k[0], k[1], fa, fb))
    print('  (%d keys differ)\n' % n)
    print('# abnormal thread exits (phone | qembian)')
    for t in sorted(set(od) | set(qd)):
        if t.startswith('oracle.exe'):
            continue                                # the recorder's own second start (file exists: -11)
        a, b = od.get(t), qd.get(t)
        if a and b and a[1].split()[0] == b[1].split()[0]:
            continue
        print('%-60s %-30s | %s' % (t[:60], ('%s @%.1f' % (a[1], a[0])) if a else '-',
                                       ('%s @%.1f' % (b[1], b[0])) if b else '-'))


def procs(threads):
    """process name -> first time any of its user threads was seen (kernel threads left out)"""
    out = {}
    for t, at in threads.items():
        p = t.split('::')[0]
        if p != 'ekern.exe':
            out[p] = min(at, out.get(p, at))
    return out


def diff(ppath, qpath):
    pp, pd, pt = oracle(ppath)
    qp, qd, qt = oracle(qpath)
    a, b = procs(pt), procs(qt)
    print('# processes on the phone only (first seen)')
    for p in sorted(a, key=a.get):
        if p not in b:
            print('  %-40s %.1f' % (p, a[p]))
    print('# processes in qembian only (first seen)')
    for p in sorted(b, key=b.get):
        if p not in a:
            print('  %-40s %.1f' % (p, b[p]))
    print('# abnormal thread exits (phone | qembian)')
    for t in sorted(set(pd) | set(qd)):
        x, y = pd.get(t), qd.get(t)
        print('  %-55s %-28s | %s' % (t[:55], ('%s @%.1f' % (x[1], x[0])) if x else '-',
                                       ('%s @%.1f' % (y[1], y[0])) if y else '-'))
    print('# P&S keys set on one side only')
    for k in sorted(set(pp) ^ set(qp)):
        side = 'phone' if k in pp else 'qembian'
        vals = (pp if k in pp else qp)[k]
        print('  %s/%s %s: %s' % (k[0], k[1], side, ' '.join('%d@%.1f' % (v, t) for t, v in vals)))


def keys(qpath):
    qp, _, _ = qembian(qpath)
    print('# P&S keys set in a qembian boot (%s), for E:\\oracle\\keys.txt' % qpath)
    for k in sorted(qp):
        print('%s/%s' % k)


def show(opath):
    for l in lines(opath):
        if ' prop ' in l or ' thread- ' in l or l.endswith(('start', 'end')):
            print(l)


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'compare':
        compare(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == 'diff':
        diff(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 3 and sys.argv[1] == 'keys':
        keys(sys.argv[2])
    elif len(sys.argv) == 3 and sys.argv[1] == 'show':
        show(sys.argv[2])
    else:
        print(__doc__)
        sys.exit(2)
