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
    spec = tomllib.load(open(os.path.join(ROOT, 'personalities', pers, 'kabi.toml'), 'rb'))
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
    print('%s / %s: %s' % (prof['_id'], pers, 'ok' if not bad else '%d class(es) differ' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
