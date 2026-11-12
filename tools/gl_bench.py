#!/usr/bin/env python3
"""A fixed UI workload on a running touch phone and the host GPU's counters over it (docs/gpu-boundary-roadmap.md:
before/after numbers for the speed phases).

  python3 tools/gl_bench.py --instance NAME | --session DIR  [--rounds N] [--json]

Workload: keep the phone awake and unlocked (agent), open the menu, N rounds of a drag up and a drag down in it, back to the home screen, N
home-screen page drags left and right. Reported: wall time, and the deltas of `sydb gl stats` (kicks, VG and
EGL calls, compositions, time in kicks and in composition) with VG calls per kick and kick time per frame.
"""
import argparse, json, os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYDB = [sys.executable, os.path.join(ROOT, 'tools/sydb/sydb.py')]


def sydb(where, *args):
    r = subprocess.run(SYDB + where + list(args), capture_output=True, text=True, timeout=60)
    if r.returncode:
        raise SystemExit('sydb %s: %s' % (' '.join(args), (r.stderr or r.stdout).strip()))
    return r.stdout


def stats(where):
    return json.loads(sydb(where, 'gl', 'stats', '--json'))['stats']


def drag(where, x0, y0, x1, y1, w, h, steps=12, ms=200):
    def ev(x, y, down=None):
        e = [{'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 0x7fff / (w - 1))}},
             {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 0x7fff / (h - 1))}}]
        if down is not None:
            e.append({'type': 'btn', 'data': {'down': down, 'button': 'left'}})
        sydb(where, 'qmp', 'input-send-event', json.dumps({'events': e}))
    ev(x0, y0, True)
    for i in range(1, steps + 1):
        ev(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        time.sleep(ms / 1000.0 / steps)
    ev(x1, y1, False)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--instance'); g.add_argument('--session')
    ap.add_argument('--rounds', type=int, default=4)
    ap.add_argument('--size', default='360x640', help='screen pixels (portrait)')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    where = ['--instance', a.instance] if a.instance else ['--session', a.session]
    w, h = (int(v) for v in a.size.split('x'))
    sydb(where, 'keepawake', 'unlocked')   # the agent keeps it unlocked and lit: no keyguard mid-run
    time.sleep(1)
    s0, t0 = stats(where), time.time()
    sydb(where, 'key', 'menu')
    time.sleep(1.5)
    for _ in range(a.rounds):
        drag(where, w // 2, h * 3 // 4, w // 2, h // 4, w, h)
        time.sleep(0.6)
        drag(where, w // 2, h // 4, w // 2, h * 3 // 4, w, h)
        time.sleep(0.6)
    sydb(where, 'key', 'menu')
    time.sleep(1.5)
    for _ in range(a.rounds):
        drag(where, w * 4 // 5, h // 2, w // 5, h // 2, w, h)
        time.sleep(0.6)
        drag(where, w // 5, h // 2, w * 4 // 5, h // 2, w, h)
        time.sleep(0.6)
    t1, s1 = time.time(), stats(where)
    d = {k: s1[k] - s0[k] for k in ('kicks', 'kick_ms', 'composes', 'compose_ms', 'vg_calls', 'egl_calls', 'fills')
         if k in s1 and k in s0}
    d['wall_s'] = round(t1 - t0, 1)
    d['fps'] = round(d['composes'] / d['wall_s'], 1) if d['wall_s'] else 0
    d['vg_per_kick'] = round(d.get('vg_calls', 0) / d['kicks'], 2) if d['kicks'] else 0
    d['kick_ms_per_frame'] = round(d['kick_ms'] / d['composes'], 2) if d['composes'] else 0
    d['kicks_per_frame'] = round(d['kicks'] / d['composes'], 1) if d['composes'] else 0
    for k in ('kick_ms', 'compose_ms'):
        d[k] = round(d[k], 1)
    print(json.dumps(d) if a.json else '\n'.join('%-18s %s' % kv for kv in d.items()))


if __name__ == '__main__':
    sys.exit(main())
