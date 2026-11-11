#!/usr/bin/env python3
"""Slow and unanswered IPC requests in a KIPC-traced serial log (sydb session serial.log or a trace-build boot).

  ipc_latency.py SERIAL.LOG [--since T] [--until T] [--min SECONDS] [--match TEXT] [--top N]

The kernel's KIPC trace (debug mask bit 11; `sydb strace` or a `device.py build --trace 0x80000800` ROM)
prints `MsgAc: M:<fn> <client>-><server>` when a client sends and `MsgCo: M:<fn> r:<result>
<server thread>-><client>` when the server completes it, plus `IPCRD <server> <- <client>: "<data>"` for
descriptor reads. Requests are paired per client thread and function, oldest first. Output:
  - completed requests that took at least --min seconds (default 0.2), slowest first, with the first
    descriptor the server read (usually a name: a file, a repository key, a resource);
  - requests still open at the end of the window (pending: a server that never answered, or a
    notification that is supposed to stay open).
"""
import argparse, collections, re, sys

AC = re.compile(r'^([0-9.]+) MsgAc: M:(-?\d+) (.+?)->(\S+)\s*$')
CO = re.compile(r'^([0-9.]+) MsgCo: M:(-?\d+) r:(-?\d+) (\S+)->(.+?)\s*$')
RD = re.compile(r'^([0-9.]+) IPCRD (\S+) <- (\S+(?: \S+)*?): "(.*)"\s*$')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--since', type=float, default=0)
    ap.add_argument('--until', type=float, default=1e12)
    ap.add_argument('--min', type=float, default=0.2)
    ap.add_argument('--match', default='', help='only requests whose client or server contains this')
    ap.add_argument('--top', type=int, default=40)
    a = ap.parse_args()
    pending = collections.defaultdict(list)      # (client, fn) -> [(t, server, first read)]
    done = []
    last_t = 0
    for ln in open(a.log, errors='replace'):
        m = AC.match(ln)
        if m:
            t = float(m.group(1))
            if t < a.since or t > a.until:
                continue
            last_t = t
            pending[(m.group(3), int(m.group(2)))].append([t, m.group(4), None])
            continue
        m = CO.match(ln)
        if m:
            t = float(m.group(1))
            if t < a.since or t > a.until:
                continue
            last_t = t
            key = (m.group(5), int(m.group(2)))
            if pending.get(key):
                t0, srv, rd = pending[key].pop(0)
                done.append((t - t0, t0, key[0], srv, key[1], int(m.group(3)), rd))
            continue
        m = RD.match(ln)
        if m:
            client = m.group(3)
            for (c, fn), lst in pending.items():
                if c == client and lst and lst[-1][2] is None:
                    lst[-1][2] = m.group(4)[:60]
    flt = lambda c, s: not a.match or a.match.lower() in c.lower() or a.match.lower() in s.lower()
    slow = sorted((d for d in done if d[0] >= a.min and flt(d[2], d[3])), reverse=True)
    print('# completed requests that took >= %.2f s (%d of %d)' % (a.min, len(slow), len(done)))
    for dt, t0, c, s, fn, r, rd in slow[:a.top]:
        print('%7.3f s  at %9.3f  %s -> %s  fn %d = %d%s' % (dt, t0, c, s, fn, r, '  "%s"' % rd if rd else ''))
    still = [(t0, c, s, fn, rd) for (c, fn), lst in pending.items() for t0, s, rd in lst if flt(c, s)]
    still.sort(key=lambda x: x[0])
    print('# still open at %.3f (%d)' % (last_t, len(still)))
    for t0, c, s, fn, rd in still[:a.top]:
        print('  since %9.3f (%6.2f s)  %s -> %s  fn %d%s' % (t0, last_t - t0, c, s, fn, '  "%s"' % rd if rd else ''))


if __name__ == '__main__':
    sys.exit(main())
