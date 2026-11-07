#!/usr/bin/env python3
"""Read the kernel debug port on stdin, timestamp it, report milestone times (see measure-boot.sh)."""
import argparse, os, signal, sys, time

MILESTONES = [
    ('first-output', None),
    ('wserv', 'SYBSTUB: install DisplayDriver'),
    ('rightsserver-drm', 'SecEnv mode 3: returned dummy cert'),
    ('hwrm-light', 'ISCPEER: ch 35 send (60): 00 00 00 3a'),
    ('startup-ui-phase', 'SYBPROP: Startup::startup sets 101f8766/4'),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--label', default='run'); ap.add_argument('--log'); ap.add_argument('--tsv')
    a = ap.parse_args()
    t0 = time.time(); seen = {}; log = open(a.log, 'w') if a.log else None
    for raw in sys.stdin.buffer:
        line = raw.decode('latin-1', 'replace')
        t = time.time() - t0
        if log:
            log.write('%8.2f %s' % (t, line))
        for name, pat in MILESTONES:
            if name not in seen and (pat is None or pat in line):
                seen[name] = t
                print('%-18s %7.1f s' % (name, t), flush=True)
        if len(seen) == len(MILESTONES):
            break
    if a.tsv:
        new = not os.path.exists(a.tsv)
        with open(a.tsv, 'a') as f:
            if new:
                f.write('label\tdate\t' + '\t'.join(n for n, _ in MILESTONES) + '\n')
            f.write('%s\t%s\t%s\n' % (a.label, time.strftime('%Y-%m-%d %H:%M'),
                                      '\t'.join('%.1f' % seen[n] if n in seen else '-' for n, _ in MILESTONES)))


if __name__ == '__main__':
    main()
