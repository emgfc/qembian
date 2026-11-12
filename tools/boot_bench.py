#!/usr/bin/env python3
"""Matched boot timings for QEMU A/B comparisons (docs/perf.md): the same seeded boot, binaries alternated.

  python3 tools/boot_bench.py --device ID [--seed DIR] [--runs N] [--env K=V ...] BIN_A [BIN_B ...]

Each run starts a throw-away sydb session (build/sydb-bench) from the seed with SYBORG_QEMU=BIN, waits for
start-up done (`100058f4/1 = 3` in the serial log, KPSUidStartupApp = 3), and records:
  wall   seconds from QEMU's start to the milestone (serial log timestamps)
  cpu    the vCPU thread's user+system CPU seconds at the milestone (/proc/<pid>/task/<tid>/stat)
  busy   cpu / wall
Binaries are alternated (A B A B ...); the report gives median and spread per binary. No profiling: final
timing runs stay unprofiled (the 2026-11-11 review's rule). --env passes extra environment to QEMU
(e.g. SYBORG_TB_STATS=1).
"""
import argparse, os, re, statistics, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYDB = [sys.executable, os.path.join(ROOT, 'tools', 'sydb', 'sydb.py')]
MARK = re.compile(rb'^(\d+\.\d+) .*100058f4/1 = 3')


def qemu_pid(sess):
    for p in os.listdir('/proc'):
        if not p.isdigit():
            continue
        try:
            if open('/proc/%s/comm' % p).read().strip() == 'qemu-system-arm' and \
                    sess.encode() in open('/proc/%s/cmdline' % p, 'rb').read():
                return int(p)
        except OSError:
            pass
    return None


def vcpu_cpu(pid):
    hz = os.sysconf('SC_CLK_TCK')
    for t in os.listdir('/proc/%d/task' % pid):
        try:
            if open('/proc/%d/task/%s/comm' % (pid, t)).read().strip() == 'CPU 0/TCG':
                f = open('/proc/%d/task/%s/stat' % (pid, t)).read().rsplit(')', 1)[1].split()
                return (int(f[11]) + int(f[12])) / hz
        except OSError:
            pass
    return None


def run(binary, a):
    sess = os.path.join(ROOT, 'build', 'sydb-bench')
    env = dict(os.environ, SYBORG_DEVICE=a.device, SYBORG_QEMU=binary)
    for kv in a.env:
        k, _, v = kv.partition('=')
        env[k] = v
    subprocess.run(SYDB + ['--session', sess, 'stop'], env=env, capture_output=True)
    log = os.path.join(sess, 'serial.log')
    if os.path.exists(log):
        os.unlink(log)
    st = SYDB + ['--session', sess, 'start', '--timeout', '1'] + (['--seed', a.seed] if a.seed else ['--fresh'])
    subprocess.Popen(st, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end = time.time() + a.timeout
    wall = cpu = None
    pid = None
    while time.time() < end:
        pid = pid or qemu_pid(sess)
        try:
            for line in open(log, 'rb'):
                m = MARK.match(line)
                if m:
                    wall = float(m.group(1))
                    break
        except OSError:
            pass
        if wall is not None:
            cpu = vcpu_cpu(pid) if pid else None
            break
        time.sleep(0.1)
    stats = None
    if a.stats:                         # the last counter line QEMU printed by the milestone (patch 0007)
        try:
            for line in open(os.path.join(sess, 'qemu.err'), errors='replace'):
                if line.startswith('syborg-tb:'):
                    stats = line.strip()
        except OSError:
            pass
    subprocess.run(SYDB + ['--session', sess, 'stop'], env=env, capture_output=True)
    return wall, cpu, stats


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--device', required=True)
    ap.add_argument('--seed', help='seed drives (default build/seeds/DEVICE; none: --fresh)')
    ap.add_argument('--runs', type=int, default=5)
    ap.add_argument('--timeout', type=float, default=120)
    ap.add_argument('--env', action='append', default=[])
    ap.add_argument('--stats', action='store_true', help='SYBORG_TB_STATS=1; print each run\'s last TB counter line')
    ap.add_argument('binaries', nargs='+')
    a = ap.parse_args()
    if a.stats:
        a.env.append('SYBORG_TB_STATS=1')
    if a.seed is None:
        d = os.path.join(ROOT, 'build', 'seeds', a.device)
        a.seed = d if os.path.isdir(d) else None
    res = {b: [] for b in a.binaries}
    for i in range(a.runs):
        for b in a.binaries:
            wall, cpu, stats = run(b, a)
            res[b].append((wall, cpu))
            print('run %d %-40s wall %s cpu %s' % (i + 1, os.path.basename(os.path.dirname(b)) + '/' + os.path.basename(b),
                  '%.2f' % wall if wall else 'FAIL', '%.2f' % cpu if cpu else '-'), flush=True)
            if stats:
                print('      ' + stats, flush=True)
    print()
    for b, rs in res.items():
        ok = [r for r in rs if r[0]]
        if not ok:
            print('%-40s all runs failed' % b)
            continue
        w = [r[0] for r in ok]
        c = [r[1] for r in ok if r[1]]
        print('%-40s wall median %.2f s (min %.2f max %.2f)  cpu median %s  busy %s  failures %d' % (
            os.path.basename(os.path.dirname(b)) + '/' + os.path.basename(b), statistics.median(w), min(w), max(w),
            '%.2f s' % statistics.median(c) if c else '-',
            '%.0f %%' % (100 * statistics.median(c) / statistics.median(w)) if c else '-', len(rs) - len(ok)))


if __name__ == '__main__':
    main()
