#!/usr/bin/env python3
"""Firmware inventory of a device (docs/adding-a-device.md step 2), from its profile's [firmware]:
find the flash files, check them against devices/<id>/firmware.sha256, extract the partitions
(tools/fiasco.py), unpage the core image (tools/rom_unpage.py), list its files (tools/romimg.py ->
core.json), write the kernel-side driver list (input for [[standins]]), the ABI reports when the SDK
and our build are there (tools/swi_map.py, tools/xip_imports.py), and identify the result.
Nothing from the firmware is committed: everything goes to build/devices/<id>/ (or --out).

  tools/fw/inventory.py [--device ID] [--fw-dir DIR] [--out DIR] [--no-abi]
FW_DIR / N95_FW_DIR or firmware/<id>/ hold the flash files when --fw-dir is not given.
"""
import argparse, glob, hashlib, json, os, shutil, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as devmodel


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def run(cmd, out=None):
    print('>> ' + ' '.join(os.path.relpath(c, ROOT) if c.startswith(ROOT) else c for c in cmd), file=sys.stderr)
    r = subprocess.run(cmd, cwd=ROOT, stdout=open(out, 'w') if out else None)
    if r.returncode:
        raise SystemExit('failed: %s' % ' '.join(cmd))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--device'); ap.add_argument('--fw-dir'); ap.add_argument('--out')
    ap.add_argument('--no-abi', action='store_true', help='skip the SWI/import reports')
    a = ap.parse_args()
    prof = devmodel.load(a.device)
    dev = prof['_id']
    fw = a.fw_dir or os.environ.get('FW_DIR') or os.environ.get('N95_FW_DIR') or os.path.join(ROOT, 'firmware', dev)
    out = a.out or devmodel.paths(prof)['dir']
    os.makedirs(os.path.join(out, 'abi'), exist_ok=True)
    py = [sys.executable, '-I']
    # 1. the flash files, by role
    files = {}
    for f in devmodel.get(prof, 'firmware.files', []):
        hits = sorted(glob.glob(os.path.join(fw, f['glob'])))
        if not hits:
            if f.get('optional'):
                continue
            raise SystemExit('%s: no %s (%s) in %s' % (dev, f['role'], f['glob'], fw))
        files[f['role']] = (hits[0], f)
    # 2. checksums against the profile's record
    sums = {os.path.basename(p): sha256(p) for p, _ in files.values()}
    with open(os.path.join(out, 'firmware.sha256'), 'w') as fo:
        for n, h in sums.items():
            fo.write('%s  %s\n' % (h, n))
    rec = devmodel.device_file(prof, devmodel.get(prof, 'firmware.checksums', 'firmware.sha256'))
    if os.path.exists(rec):
        want = {l.split()[1].split('/')[-1]: l.split()[0] for l in open(rec) if l.strip()}
        for n, h in sums.items():
            print('%-40s %s' % (n, 'sha256 ok' if want.get(n) == h else 'NOT the profile\'s file (%s)' % h[:16]))
    # 3. partitions -> core-flat.img, rofs1.img, rofs2.img
    for role, (p, f) in files.items():
        pdir = os.path.join(out, role)
        run(py + [os.path.join(ROOT, 'tools', 'fiasco.py'), p, '--out', pdir], os.path.join(out, '%s-partitions.txt' % role))
        parts = f.get('partitions', {})
        for kind, pname in parts.items():
            src = sorted(glob.glob(os.path.join(pdir, pname + '@*.bin')))
            if not src:
                raise SystemExit('%s: partition %s not in %s' % (dev, pname, p))
            if kind == 'core':
                run(py + [os.path.join(ROOT, 'tools', 'rom_unpage.py'), src[0], os.path.join(out, 'core-flat.img')])
            elif kind == 'rofs':
                shutil.copyfile(src[0], os.path.join(out, 'rofs1.img' if role == 'core' else 'rofs2.img'))
    core = os.path.join(out, 'core-flat.img')
    run(py + [os.path.join(ROOT, 'tools', 'romimg.py'), core, '--images', '--json', os.path.join(out, 'core.json')],
        os.path.join(out, 'core-files.txt'))
    # 4. kernel-side drivers: what [[standins]] has to answer for
    cj = json.load(open(os.path.join(out, 'core.json')))
    drv = sorted(f['path'].split('/')[-1] for f in cj['files'] if f['path'].lower().endswith(('.ldd', '.pdd')))
    have = {s['name'].lower() for s in prof.get('standins', [])}
    missing = {m.lower() for m in devmodel.get(prof, 'standins.missing.drivers', [])}
    with open(os.path.join(out, 'kernel-drivers.txt'), 'w') as fo:
        for d in drv:
            base = d.lower().rsplit('.', 1)[0]
            tag = 'stand-in' if base in have else ('missing (declared)' if d.lower() in missing else '')
            fo.write('%-32s %s\n' % (d, tag))
    print('kernel drivers: %d (%d with a stand-in) -> %s' % (len(drv), sum(1 for d in drv if d.lower().rsplit('.', 1)[0] in have),
                                                          os.path.relpath(os.path.join(out, 'kernel-drivers.txt'), ROOT)))
    # 5. ABI reports (need the SDK's euser.dso and our build)
    k = os.path.join(ROOT, 'build', 'epocroot', 'sf', 'os', 'kernelhwsrv', 'kernel', 'eka')
    sdk = os.environ.get('SDK_EUSER', os.path.expanduser('~/work/S60_3rd_FP1/Epoc32/release/armv5/lib/euser.dso'))
    ours = os.path.join(ROOT, 'build', 'epocroot', 'epoc32', 'release', 'armv5', 'udeb', '_syborg_euser.dll.sym')
    if not a.no_abi and os.path.exists(sdk) and os.path.exists(ours):
        eu = [l.split()[0] for l in open(os.path.join(out, 'core-files.txt')) if l.rstrip().endswith('Sys/Bin/EUser.dll')][0]
        abi = os.path.join(out, 'abi')
        run(py + [os.path.join(ROOT, 'tools', 'swi_map.py'), '--rom-image', core, '--at', eu, '--names', sdk,
                  '--names', os.path.join(k, 'eabi', 'euseru.def'), '-o', os.path.join(abi, 'n95-euser-swi.json')])
        run(py + [os.path.join(ROOT, 'tools', 'swi_map.py'), '--elf', ours, '-o', os.path.join(abi, 'our-euser-swi.json')])
        run(py + [os.path.join(ROOT, 'tools', 'swi_map.py'), '--compare', os.path.join(abi, 'n95-euser-swi.json'),
                  os.path.join(abi, 'our-euser-swi.json'), '--execs', os.path.join(k, 'kernel', 'execs.txt')],
            os.path.join(abi, 'swi-n95-vs-ours.txt'))
        run(py + [os.path.join(ROOT, 'tools', 'xip_imports.py'), core, os.path.join(out, 'core.json'), '--ordinals',
                  os.path.join(abi, 'euser-ordinal-usage.tsv')], os.path.join(abi, 'xip-imports.txt'))
        print(open(os.path.join(abi, 'swi-n95-vs-ours.txt')).read().strip().splitlines()[-1])
    # 6. what the firmware says it is
    facts, matches = devmodel.identify(out)
    sw = facts.get('sw') or {}
    print('identity: sw.txt %s %s %s' % (sw.get('version'), sw.get('date'), sw.get('type_code')))
    for n, d, probs in matches:
        print('  %s %s%s' % ('MATCH' if n == 0 else '  -  ', d, '' if n == 0 else ': ' + '; '.join(probs[:2])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
