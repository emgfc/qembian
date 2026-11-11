#!/usr/bin/env python3
"""Check a device's firmware against its personality (personalities/<p>/kabi.toml): the driver-facing
vtables of the device's own ekern vs ours, slot by slot (tools/personality/kabi.py).

  tools/personality/check.py [--device ID]      exit 0 = only allowed differences
"""
import argparse, json, os, subprocess, sys, tempfile, tomllib
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as devmodel


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--device')
    a = ap.parse_args()
    prof = devmodel.load(a.device)
    pers = devmodel.get(prof, 'platform.personality')
    # a personality that only changes a convention (sym92l91: the loader layout) names the one whose exec
    # map and driver ABI it uses: personalities/<p>/personality.toml base = "..."
    pt = os.path.join(ROOT, 'personalities', pers, 'personality.toml')
    if os.path.exists(pt):
        import tomllib
        base = tomllib.load(open(pt, 'rb')).get('base')
        if base:
            print('%s: personality %s uses %s\'s exec map and driver ABI' % (prof['_id'], pers, base))
            pers = base
    kf = os.path.join(ROOT, 'personalities', pers, 'kabi.toml')
    if not os.path.exists(kf):
        print('%s: personality %s has no kabi.toml (no driver-ABI spec written yet): nothing checked' % (prof['_id'], pers))
        return 0
    spec = tomllib.load(open(kf, 'rb'))
    with tempfile.NamedTemporaryFile(suffix='.json') as f:
        env = dict(os.environ, SYBORG_DEVICE=prof['_id'])
        r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'personality', 'kabi.py'), '--json', f.name],
                           capture_output=True, text=True, env=env)
        if r.returncode:
            print(r.stdout + r.stderr)
            return 2
        tables = json.load(open(f.name))
    bad = 0
    for cls in spec['classes']:
        t = tables.get(cls)
        if not t:
            print('%-22s not exported by this ekern' % cls); bad += 1; continue
        n95, ours = t['n95'], t['ours']
        extra = spec.get('allowed_extra_tail', {}).get(cls, [])
        ok = ours[:len(n95)] == n95 and ours[len(n95):] == extra
        print('%-22s %s (%d slots%s)' % (cls, 'ok' if ok else 'DIFFERENT', len(n95),
                                         ', +%d allowed' % len(extra) if extra and ok else ''))
        if not ok:
            bad += 1
            for i in range(max(len(n95), len(ours))):
                x = n95[i] if i < len(n95) else '-'
                y = ours[i] if i < len(ours) else '-'
                if x != y:
                    print('   %2d device %-50s | ours %s' % (i, x[:50], y))
    # exec numbering: every SWI the device's own EUSER uses has an entry in the personality's map
    import re
    swi = os.path.join(devmodel.paths(prof)['dir'], 'abi', 'n95-euser-swi.json')
    if os.path.exists(swi):
        keys = set()
        for l in open(os.path.join(ROOT, 'personalities', pers, 'exec.map')):
            m = re.match(r'^([FS])([0-9a-fA-F]+)\s+(\S+)', l)
            if m:
                keys.add((m.group(1), int(m.group(2), 16)))
        used = json.load(open(swi))['swis']
        miss = ['%s%02x' % k for k in (('F', s & 0x7fffff) if s & 0x800000 else ('S', s) for s in used) if k not in keys]
        print('%-22s %s (%d SWIs used by the device EUSER%s)' % ('exec numbering', 'ok' if not miss else 'MISSING',
                                                              len(used), ': ' + ' '.join(miss) if miss else ''))
        bad += 1 if miss else 0
    else:
        print('exec numbering         not checked (run tools/fw/inventory.py for the ABI reports)')
    print('%s / %s: %s' % (prof['_id'], pers, 'ok' if not bad else '%d problem(s)' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
