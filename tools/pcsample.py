#!/usr/bin/env python3
"""Sample the guest PC through QEMU's gdbstub and map samples to ROM images.

  pcsample.py [--n 40] [--interval 2] [--port 1234] [--json build/n95/core.json] [--hybrid build/n95/hybrid-manifest.txt]

Each sample attaches gdb in batch mode, reads pc/cpsr/lr, and detaches (the guest resumes).
Addresses inside the N95 core ROM are mapped to "<image>+off" via core.json; addresses below
0x80000000 are user RAM (RAM-loaded code or data); CPU mode from cpsr (0x10 usr, 0x13 svc, 0x12 irq).
Run while a boot started with SYBORG_GDB=1 (scripts/hybrid-boot.sh) is going.
"""
import argparse, bisect, collections, json, re, subprocess, time

MODES = {0x10: 'usr', 0x11: 'fiq', 0x12: 'irq', 0x13: 'svc', 0x17: 'abt', 0x1b: 'und', 0x1f: 'sys'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--n', type=int, default=40)
    ap.add_argument('--interval', type=float, default=2)
    ap.add_argument('--port', type=int, default=1234)
    ap.add_argument('--json', default='build/n95/core.json')
    a = ap.parse_args()
    j = json.load(open(a.json))
    spans = sorted((f['image']['code'], f['image']['code'] + f['image']['codesize'], f['path'].split('/')[-1])
                   for f in j['files'] if 'image' in f)
    starts = [s[0] for s in spans]

    def where(x):
        i = bisect.bisect_right(starts, x) - 1
        if i >= 0 and spans[i][0] <= x < spans[i][1]:
            return '%s+%x' % (spans[i][2], x - spans[i][0])
        if x >= 0x80000000:
            return 'rom/kernel %08x' % x       # our kernel side (not in the N95 json)
        return 'ram %08x' % x

    hist = collections.Counter()
    for k in range(a.n):
        out = subprocess.run(['gdb', '-nx', '-batch', '-ex', 'set architecture arm',
                              '-ex', 'target remote 127.0.0.1:%d' % a.port,
                              '-ex', 'p/x $pc', '-ex', 'p/x $cpsr', '-ex', 'p/x $lr', '-ex', 'detach'],
                             capture_output=True, text=True, timeout=60).stdout
        v = re.findall(r'= (0x[0-9a-f]+)', out)
        if len(v) < 3:
            print('sample %d: no data' % k); time.sleep(a.interval); continue
        pc, cpsr, lr = (int(x, 16) for x in v[:3])
        mode = MODES.get(cpsr & 0x1f, hex(cpsr & 0x1f))
        line = '%s pc=%08x (%s) lr=%08x (%s)' % (mode, pc, where(pc), lr, where(lr))
        print(line, flush=True)
        hist['%s %s' % (mode, where(pc).split('+')[0])] += 1
        time.sleep(a.interval)
    print('--- histogram')
    for k, c in hist.most_common():
        print('%4d %s' % (c, k))


if __name__ == '__main__':
    main()
