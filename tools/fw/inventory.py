#!/usr/bin/env python3
"""Firmware inventory of a device (docs/adding-a-device.md step 2), from its profile's [firmware]:
find the flash files, check them against the ROM store's record (rom.toml sha256), extract the partitions
(tools/fiasco.py), unpage the core image (tools/rom_unpage.py), list its files (tools/romimg.py ->
core.json), write the kernel-side driver list (input for [[standins]]), the ABI reports when the SDK
and our build are there (tools/swi_map.py, tools/xip_imports.py), and identify the result.
Nothing from the firmware is committed. The flash files come from the ROM store entry of the profile's
firmware (tools/fw/store.py; --fw-dir for files outside the store), the extraction goes to the shared
cache build/firmware/<RM>/<version>/<packs>/ (or --out) and its items are linked into the device's build
directory build/roms/<id>/<firmware key>/ (tools/device.py paths dir), where the device's ROM is composed.

  tools/fw/inventory.py [--device ID] [--fw-dir DIR] [--out DIR] [--no-abi] [--uda-only]
The user data layer ([[firmware.files]] role = "uda", drive = "C") becomes uda-<drive>.img, a sparse
image of its file system (tools/fw/uda.py); new session drives start as a copy (docs/devices.md).
--uda-only does only that step (a device inventoried before the layer was declared).
Without a store entry: --fw-dir DIR names the directory of the flash files (import them into the store
with tools/fw/store.py to share the extraction).
"""
import argparse, glob, hashlib, json, os, shutil, struct, subprocess, sys, zlib
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fw'))
import device as devmodel


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def sos_inflate(pdir, pname):
    """a compressed SOS* partition extracted by tools/fiasco.py (record payload .hdr + chunks .bin) ->
    (ROM image, the bytes after the deflate stream: the N73's ROFS1); tools/fw/identity.py inflate_sos"""
    import identity
    hdrs = sorted(glob.glob(os.path.join(pdir, glob.escape(pname) + '@*.hdr')))
    bins = sorted(glob.glob(os.path.join(pdir, glob.escape(pname) + '@*.bin')))
    if not hdrs or not bins:
        raise SystemExit('%s: no record payload/chunks for %s in %s' % ('inventory', pname, pdir))
    hloc, bloc = int(hdrs[0].rsplit('@', 1)[1][:-4], 16), int(bins[0].rsplit('@', 1)[1][:-4], 16)
    hdr, body = open(hdrs[0], 'rb').read(), open(bins[0], 'rb').read()
    if hloc + len(hdr) != bloc:
        raise SystemExit('%s: unexpected SOS header layout (hdr @%x+%x, data @%x)' % (pname, hloc, len(hdr), bloc))
    try:
        img, rest = identity.inflate_sos(hdr, body)
    except ValueError as e:
        raise SystemExit('%s: %s' % (pname, e))
    print('%s: ROM image 0x%x bytes (base 0x%08x)' % (pname, len(img), struct.unpack_from('<I', img, 0x8c)[0]))
    return img, rest


def rofs_slice(data, what):
    """the first ROFS/ROFx image in data (sector aligned), cut to its iImageSize"""
    import identity
    img = identity.rofs_slice(data)
    if img is None:
        raise SystemExit('%s: no ROFS image found' % what)
    return img


def run(cmd, out=None):
    print('>> ' + ' '.join(os.path.relpath(c, ROOT) if c.startswith(ROOT) else c for c in cmd), file=sys.stderr)
    r = subprocess.run(cmd, cwd=ROOT, stdout=open(out, 'w') if out else None)
    if r.returncode:
        raise SystemExit('failed: %s' % ' '.join(cmd))


def paths_dir(prof):
    return devmodel.paths(prof)['dir']


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--device'); ap.add_argument('--fw-dir'); ap.add_argument('--out')
    ap.add_argument('--no-abi', action='store_true', help='skip the SWI/import reports')
    ap.add_argument('--uda-only', action='store_true', help='only extract the user data layer')
    a = ap.parse_args()
    prof = devmodel.load(a.device)
    dev = prof['_id']
    fd = devmodel.firmware_dirs(prof)		# the ROM store entry and the shared extraction cache (device-model-v2)
    fw = a.fw_dir or (fd and fd['store'])
    if not fw:
        raise SystemExit('%s: firmware not in the ROM store (tools/fw/store.py import DIR, or --fw-dir DIR)' % dev)
    out = a.out or (fd and fd['cache']) or devmodel.paths(prof)['dir']
    os.makedirs(os.path.join(out, 'abi'), exist_ok=True)
    py = [sys.executable, '-I']
    # 1. the flash files, by role
    files = {}
    for f in devmodel.get(prof, 'firmware.files', []):
        if a.uda_only and f.get('role') != 'uda':
            continue
        hits = sorted(glob.glob(os.path.join(fw, f['glob'])))
        if not hits:
            if f.get('optional'):
                continue
            raise SystemExit('%s: no %s (%s) in %s' % (dev, f['role'], f['glob'], fw))
        files[f['role']] = (hits[0], f)
    # 2. checksums against the store's record of the files (written at import: a file changed since is not
    # the one identified); the profile's [firmware.identity] is checked on the extraction (step 6)
    sums = {os.path.basename(p): sha256(p) for p, _ in files.values()}
    want = {f['name']: f.get('sha256') for f in devmodel.store_entries().get(devmodel.firmware_id(prof) or '', {}).get('files', [])}
    for n, h in sums.items():
        if n in want:
            print('%-40s %s' % (n, 'sha256 ok' if want[n] == h else 'CHANGED since the store import (%s)' % h[:16]))
    # 3a. the user data layer -> uda-<drive>.img (a FAT drive, not ROM partitions)
    import uda as udamod
    for role, (p, f) in list(files.items()):
        if role != 'uda':
            continue
        del files[role]
        dst = os.path.join(out, 'uda-%s.img' % f.get('drive', 'C').lower())
        try:
            facts = udamod.write_image(p, dst)
        except udamod.UdaError as e:
            if 'no data chunks' in str(e):     # an erase-only UDA (e.g. N81 21.0.010): the factory C: is blank
                print('user data layer: %s erases the drive only (no data): C: starts blank' % os.path.basename(p))
                continue
            raise SystemExit('%s: %s' % (dev, e))
        print('user data layer: %s -> %s: %s %d MB, %d MB of data' % (os.path.basename(p), os.path.relpath(dst, ROOT),
              facts['type'], facts['size'] >> 20, facts['data'] >> 20))
    if a.uda_only:
        if not a.out and fd:
            devmodel.link_firmware(prof)
        return 0
    # 3. partitions -> core-flat.img, rofs1.img, rofs2.img, rofs3.img. Where each image is inside a file is read
    #    from the file (tools/fw/layout.py: partition names, compression, ROFS magic, the ROM header's paging);
    #    the profile's roles only select the files.
    import layout
    wrote = set()
    def part_file(pdir, pname):
        stem = pname.replace('+', '_')		# tools/fiasco.py writes "SOS+CORE" as SOS_CORE@<loc>.bin
        src = sorted(glob.glob(os.path.join(pdir, glob.escape(stem) + '@*.bin')))
        if not src:
            raise SystemExit('%s: partition %s not extracted in %s' % (dev, pname, pdir))
        return stem, src[0]
    def rofs_out(name, data, what):
        img = data if data[:4] in (b'ROFS', b'ROFx') else rofs_slice(data, what)	# RAPIDO NAND: 0xC00 prefix
        open(os.path.join(out, name), 'wb').write(img)
        wrote.add(name)
    plans = {}
    for role, (p, f) in files.items():
        pdir = os.path.join(out, role)
        run(py + [os.path.join(ROOT, 'tools', 'fiasco.py'), p, '--out', pdir], os.path.join(out, '%s-partitions.txt' % role))
        plans[role] = layout.plan(p)
        print('%s: %s' % (os.path.basename(p), plans[role]))
    langs = [r for r in files if 'lang' in plans[r] and 'core' not in plans[r]]
    for role, (p, f) in files.items():
        pl, pdir = plans[role], os.path.join(out, role)
        if 'core' in pl:
            pname, compressed = pl['core']
            stem, src = part_file(pdir, pname)
            rest = None
            paged = not compressed and layout.rom_is_paged(open(src, 'rb').read(0x10000))
            declared = devmodel.get(prof, 'firmware.rom.paged', None)
            if declared is not None and not compressed and bool(declared) != paged:
                print('WARNING: [firmware.rom] paged = %s, the core says %s' % (declared, paged))
            if compressed:
                img, rest = sos_inflate(pdir, stem)
                open(os.path.join(out, 'core-flat.img'), 'wb').write(img)
            elif paged:
                run(py + [os.path.join(ROOT, 'tools', 'rom_unpage.py'), src, os.path.join(out, 'core-flat.img')])
            else:
                shutil.copyfile(src, os.path.join(out, 'core-flat.img'))
            r1 = pl.get('rofs1')
            if r1 == 'after-core':
                rofs_out('rofs1.img', rest, pname)
            elif r1:
                rofs_out('rofs1.img', open(part_file(pdir, r1)[1], 'rb').read(), r1)
            else:
                raise SystemExit('%s: %s has a core but no ROFS1' % (dev, os.path.basename(p)))
            if 'lang' in pl and not langs:		# a language ROFS inside the core's file, no separate one
                rofs_out('rofs2.img', open(part_file(pdir, pl['lang'])[1], 'rb').read(), pl['lang'])
        elif 'lang' in pl:
            rofs_out('rofs2.img', open(part_file(pdir, pl['lang'])[1], 'rb').read(), pl['lang'])
        if 'cust' in pl:
            rofs_out('rofs3.img', open(part_file(pdir, pl['cust'])[1], 'rb').read(), pl['cust'])
        if not pl:
            print('%s: no core or ROFS image in it (role %s)' % (os.path.basename(p), role))
    if not os.path.exists(os.path.join(out, 'core-flat.img')):
        raise SystemExit('%s: no core image among the firmware files' % dev)
    core = os.path.join(out, 'core-flat.img')
    run(py + [os.path.join(ROOT, 'tools', 'romimg.py'), core, '--images', '--json', os.path.join(out, 'core.json')],
        os.path.join(out, 'core-files.txt'))
    # 4. kernel-side drivers: what [[standins]] has to answer for
    cj = json.load(open(os.path.join(out, 'core.json')))
    drv = sorted(f['path'].split('/')[-1] for f in cj['files'] if f['path'].lower().endswith(('.ldd', '.pdd')))
    have = {s['name'].lower() for s in prof.get('standins', [])}
    missing = {m.lower() for m in devmodel.get(prof, 'kernel.missing_drivers', [])}
    with open(os.path.join(out, 'kernel-drivers.txt'), 'w') as fo:
        for d in drv:
            base = d.lower().rsplit('.', 1)[0]
            tag = 'stand-in' if base in have else ('missing (declared)' if d.lower() in missing else '')
            fo.write('%-32s %s\n' % (d, tag))
    print('kernel drivers: %d (%d with a stand-in) -> %s' % (len(drv), sum(1 for d in drv if d.lower().rsplit('.', 1)[0] in have),
                                                          os.path.relpath(os.path.join(out, 'kernel-drivers.txt'), ROOT)))
    # 5. ABI reports (need the SDK's euser.dso and our build)
    k = os.path.join(ROOT, 'build', 'epocroot', 'sf', 'os', 'kernelhwsrv', 'kernel', 'eka')
    sdk = os.environ.get('SDK_EUSER', os.path.join(os.environ.get('S60_SDK_LIB', os.path.expanduser('~/work/S60_3rd_FP1/Epoc32/release/armv5/lib')), 'euser.dso'))
    ours = os.path.join(ROOT, 'build', 'epocroot', 'epoc32', 'release', 'armv5', 'udeb', '_syborg_euser.dll.sym')
    if not a.no_abi and os.path.exists(sdk) and os.path.exists(ours):
        eu = [l.split()[0] for l in open(os.path.join(out, 'core-files.txt')) if l.rstrip().lower().endswith('sys/bin/euser.dll')][0]   # case differs (701: euser.dll)
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
    if not a.out and fd:
        devmodel.link_firmware(prof)		# the cache's items linked into build/roms/<id>/<key>/
        print('linked into %s' % os.path.relpath(paths_dir(prof), ROOT))
    return 0


if __name__ == '__main__':
    sys.exit(main())
