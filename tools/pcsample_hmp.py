#!/usr/bin/env python3
"""Guest PC samples through QEMU's monitor (`info registers`: the vCPU is not stopped by gdb),
attributed to ROM files (core.json) and RAM-loaded code (the session log's SYBCODE: lines).

  tools/pcsample_hmp.py SESSION SECONDS [--interval 0.005] [--top 25] [--json OUT]

Tells busy from waiting: the idle loop shows up as one kernel address (SyborgWFIIdle in
KASYBORG.DLL). docs/perf.md "How to measure". Device: $SYBORG_DEVICE / the registry default.
"""
import argparse, bisect, collections, json, os, re, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'sydb'))
import sydb as S                                    # noqa: E402
import device as devmodel                           # noqa: E402  (tools/device.py)

MODES = {0x10: 'usr', 0x11: 'fiq', 0x12: 'irq', 0x13: 'svc', 0x17: 'abt', 0x1b: 'und', 0x1f: 'sys'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('session')
    ap.add_argument('seconds', type=float)
    ap.add_argument('--interval', type=float, default=0.005)
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--json')
    a = ap.parse_args()
    c = S.Client(a.session)
    samples, t0 = [], time.time()
    while time.time() - t0 < a.seconds:
        r = c.call({'op': 'hmp', 'cmd': 'info registers'}, 5).get('out', '')
        pc, psr = re.search(r'R15=([0-9a-f]{8})', r), re.search(r'PSR=([0-9a-f]{8})', r)
        if pc:
            samples.append((int(pc.group(1), 16), int(psr.group(1), 16) if psr else 0))
        time.sleep(a.interval)
    if a.json:
        json.dump(samples, open(a.json, 'w'))
    prof = devmodel.load(os.environ.get('SYBORG_DEVICE'))
    core = json.load(open(devmodel.paths(prof)['core_json']))
    spans = sorted((f['image']['code'], f['image']['code'] + f['image']['codesize'], f['path'].split('/')[-1])
                   for f in core['files'] if 'image' in f)
    starts = [s[0] for s in spans]
    ram = []
    log = os.path.join(a.session, 'serial.log')
    for line in open(log, errors='replace') if os.path.exists(log) else []:
        m = re.search(r'SYBCODE: (\S+) code=([0-9a-f]+)\+([0-9a-f]+)', line)
        if m:
            b, n = int(m.group(2), 16), int(m.group(3), 16)
            ram.append((b, b + n, m.group(1).split('\\')[-1]))

    def where(pc):
        i = bisect.bisect_right(starts, pc) - 1
        if i >= 0 and spans[i][0] <= pc < spans[i][1]:
            return spans[i][2]
        for b, e, n in reversed(ram):
            if b <= pc < e:
                return n + ' (ram)'
        return 'kernel/variant/other' if pc >= 0x80000000 else 'user ram?'

    n = max(1, len(samples))
    mods, modes = collections.Counter(), collections.Counter()
    pcs = collections.Counter(pc for pc, _ in samples)
    for pc, psr in samples:
        mods[where(pc)] += 1
        modes[MODES.get(psr & 0x1f, hex(psr & 0x1f))] += 1
    print('%d samples; modes %s' % (len(samples), ', '.join('%s %d%%' % (k, 100 * v // n) for k, v in modes.most_common())))
    top = pcs.most_common(1)
    if top and top[0][1] > n // 2:
        print('one address holds %d%%: %08x (the idle loop if it is SyborgWFIIdle)' % (100 * top[0][1] // n, top[0][0]))
    for k, v in mods.most_common(a.top):
        print('%5.1f%%  %s' % (100.0 * v / n, k))


if __name__ == '__main__':
    main()
