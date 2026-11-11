#!/usr/bin/env python3
"""Identify Nokia flash files: what they are, read from the files (docs/device-model-v2.md section 3).

  identify.py PATH... [--json]     PATH: a directory (searched recursively), flash files, or a .zip/.rar/.7z

Order of trust: the package's .vpl (type code, software version, product code, file roles), then the
version files inside the images (Resource/Versions/sw.txt in ROFS1, langsw.txt in ROFS2, customersw.txt
in ROFS3), then the file name (RM-xxx and a version). File roles without a .vpl come from the file's
layout (tools/fw/layout.py: core, language ROFS, customer ROFS), user data layers from the name.
Each firmware (type code + software version) is matched against the registry (models/registry.toml) and
the device profiles whose [firmware.identity] it equals. Nothing is extracted to disk except archives
(into a temporary directory, removed afterwards).
"""
import argparse, hashlib, json, os, re, shutil, struct, subprocess, sys, tempfile, zipfile
import xml.etree.ElementTree as ET

FW = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(FW)
sys.path.insert(0, FW)
sys.path.insert(0, TOOLS)
import layout, identity     # tools/fw
import device as devmodel   # tools/device.py

ARCHIVES = ('.zip', '.rar', '.7z')
UDA_NAME = re.compile(r'(\.uda\.|userarea|_uda\.)', re.I)


def expand(path, tmp):
    """an archive into a fresh directory under tmp (zip: zipfile; rar/7z: unar, else 7z)"""
    dst = tempfile.mkdtemp(dir=tmp)
    if path.lower().endswith('.zip'):
        with zipfile.ZipFile(path) as z:
            z.extractall(dst)
    elif shutil.which('unar'):
        subprocess.run(['unar', '-q', '-f', '-o', dst, path], check=True, stdout=subprocess.DEVNULL)
    else:
        subprocess.run(['7z', 'x', '-y', '-o' + dst, path], check=True, stdout=subprocess.DEVNULL)
    return dst


def collect(paths, tmp):
    files = []
    for p in paths:
        if os.path.isdir(p):
            for d, _, names in os.walk(p):
                files += [os.path.join(d, n) for n in sorted(names)]
        elif p.lower().endswith(ARCHIVES):
            files += collect([expand(p, tmp)], tmp)
        else:
            files.append(p)
    return files


def is_fiasco(p):
    try:
        with open(p, 'rb') as f:
            return f.read(1) == b'\xb2' and os.path.getsize(p) > 64
    except OSError:
        return False


def read_vpl(p):
    """a VariantPackingList: product identity and the file roles it names"""
    try:
        r = ET.parse(p).getroot()
    except ET.ParseError:
        return None
    if r.tag != 'VariantPackingList':
        return None
    vi = r.find('Variant/VariantIdentification')
    txt = lambda e, k: (e.findtext(k) or '').strip() if e is not None else ''
    files = {}
    for f in r.iterfind('Variant/FileList/File'):
        files[txt(f, 'Name')] = txt(f, 'FileSubType') or txt(f, 'FileType')
    return {'type_code': txt(vi, 'TypeDesignator'), 'product_code': txt(vi, 'ProductCode'),
            'description': txt(vi, 'Description'), 'group': txt(vi, 'VariantGroup'),
            'sw_version': txt(r, 'Variant/SwVersion'), 'variant_version': txt(r, 'Variant/VariantVersion'),
            'files': files, 'vpl': os.path.basename(p)}


def read_flash(p):
    """one flash file: its role, partitions and the version file of the image it carries"""
    rec = {'path': p, 'name': os.path.basename(p), 'size': os.path.getsize(p)}
    try:
        parts = {k: v for k, v in layout.partitions(p).items() if v}
        pl = layout.plan(p)
    except (ValueError, KeyError, IndexError) as e:
        rec.update(role='unreadable', problem=str(e))
        return rec
    rec['partitions'] = sorted(parts)
    try:
        if 'core' in pl:
            rec['role'] = 'core'
            pname, compressed = pl['core']
            rofs1 = None
            if pl.get('rofs1') == 'after-core':
                rofs1 = identity.rofs_slice(identity.inflate_sos(*identity.assemble(p, pname))[1])
            elif pl.get('rofs1'):
                rofs1 = identity.rofs_slice(identity.assemble(p, pl['rofs1'])[1])
            rec['versions'] = identity.rofs_versions(rofs1)
            rec['compressed'] = compressed
            if rofs1:
                rec['rofs1_sha256'] = hashlib.sha256(rofs1).hexdigest()	# tells custom firmwares of one version apart
        elif 'lang' in pl:
            rec['role'] = 'rofs2'
            rec['versions'] = identity.rofs_versions(identity.rofs_slice(identity.assemble(p, pl['lang'])[1]))
        elif 'cust' in pl:
            rec['role'] = 'rofs3'
            rec['versions'] = identity.rofs_versions(identity.rofs_slice(identity.assemble(p, pl['cust'])[1]))
        elif UDA_NAME.search(rec['name']) or re.search(r'\.(u\d\d|\d\du)$', rec['name'], re.I):	# .U01, E71 .01U
            rec['role'] = 'uda'
        else:
            rec['role'] = 'other'		# loaders, ENO/APE-only images, UDA erase files
    except (ValueError, KeyError, IndexError, OSError, struct.error) as e:
        rec['problem'] = 'version files: %s' % e
    if rec.get('role') in ('rofs2', 'rofs3'):		# the pack id from the name until the core is known
        m = re.search(r'_([A-Z]?\d\d\.\d\d)_', rec['name']) or re.search(r'\.[vV](\d\d)$', rec['name'])
        rec['id'] = m.group(1) if m else ''
    return rec


def norm_version(v):
    return re.sub(r'^V\s*', '', v or '').strip()


def from_name(name):
    tc = re.search(r'RM-?(\d{3})', name, re.I)
    ver = re.search(r'_(\d{2,3}\.\d{2,4}(?:\.\d+)*)', name)
    return ('RM-%s' % tc.group(1) if tc else ''), (ver.group(1) if ver else '')


def identify(paths):
    tmp = tempfile.mkdtemp(prefix='qembian-identify-')
    try:
        files = collect(paths, tmp)
        vpls = [v for v in (read_vpl(p) for p in files if p.lower().endswith('.vpl')) if v]
        flash = [read_flash(p) for p in files if is_fiasco(p)]
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    vroles = {}
    for v in vpls:
        for n, sub in v['files'].items():
            vroles.setdefault(n, sub)
    groups = {}
    for f in flash:
        f['dir'] = os.path.dirname(f['path'])
        f['vpl_role'] = vroles.get(f['name'], '')
        sw = (f.get('versions') or {}).get('sw') or (f.get('versions') or {}).get('langsw') \
            or (f.get('versions') or {}).get('customersw') or {}
        tc, ver = sw.get('type_code', ''), norm_version(sw.get('version', ''))
        conf = 'sw.txt' if tc else 'name'
        if not tc:
            tc, ver2 = from_name(f['name'])
            ver = ver or ver2
        v = next((x for x in vpls if f['name'] in x['files']), None)
        if v:
            conf = 'vpl' if conf == 'name' else conf
            tc, ver = tc or v['type_code'], ver or v['sw_version']
        f['confidence'] = conf
        f['key'] = (tc.upper(), ver)
    # a language, customer or user data file belongs to the core it came with: the only core of its
    # directory, else a core whose version its own version starts with (langsw "022.009.05.01", N73
    # "3.0717.1.4.1-16", N79 "30.019.005.1"); its pack id is the rest
    cores = [f for f in flash if f.get('role') == 'core']
    for f in flash:
        if f.get('role') != 'core':
            here = {c['key'] for c in cores if c['dir'] == f['dir']}
            pref = [c['key'] for c in cores if c['key'][1] and f['key'][1].startswith(c['key'][1])]
            # several cores in one directory and a pack of its own version scheme (N95 RM-159 .v17
            # langsw "SPP 31.31 49 BL" next to the 31.0.017 and 35.0.002 cores): the core its file name names
            named = [c['key'] for c in cores if c['dir'] == f['dir'] and c['key'][1] and c['key'][1] in f['name']]
            key = next(iter(here)) if len(here) == 1 else (pref[0] if pref else (named[0] if len(named) == 1
                                                                                  else f['key']))
            if key != f['key'] and f.get('role') in ('rofs2', 'rofs3') and f['key'][1].startswith(key[1]):
                f['id'] = f.get('id') or f['key'][1][len(key[1]):].lstrip('.-_ ')
            f['key'] = key
        groups.setdefault(f['key'], []).append(f)
    reg = devmodel.registry()
    out = []
    for (tc, ver), fl in sorted(groups.items()):
        core = next((f for f in fl if f.get('role') == 'core'), None)
        sw = ((core or {}).get('versions') or {}).get('sw') or {}
        prods = [v for v in vpls if v['type_code'].upper() == tc and v['sw_version'] == ver]
        rec = {'type_code': tc, 'sw_version': ver, 'date': sw.get('date', ''), 'product': sw.get('product', ''),
               'confidence': 'vpl' if prods else ('sw.txt' if sw else 'name'),
               'products': [{k: p[k] for k in ('product_code', 'description', 'group', 'variant_version', 'vpl')}
                            for p in prods],
               'files': [{k: f.get(k) for k in ('name', 'role', 'id', 'vpl_role', 'partitions', 'problem') if f.get(k)}
                         for f in fl],
               'registry': reg.get(tc), 'profiles': [], 'problems': []}
        for d in devmodel.device_ids():
            prof = devmodel.load(d)
            idn = devmodel.get(prof, 'firmware.identity.sw', {})
            want = devmodel.get(prof, 'firmware.identity.sha256', {}).get('rofs1.img')
            if idn.get('type_code', '').upper() == tc and norm_version(idn.get('version')) == ver \
                    and (not want or not core or core.get('rofs1_sha256') in (None, want)):
                rec['profiles'].append(d)
        if not core:
            rec['problems'].append('no core file (a language or customer pack alone cannot be placed)')
        for v in prods:
            if v['sw_version'] != ver:
                rec['problems'].append('%s says %s' % (v['vpl'], v['sw_version']))
        out.append(rec)
    return out


def show(rec):
    r = rec['registry'] or {}
    name = r.get('name', 'unknown type code')
    print('%s %s (%s), software %s %s  [%s]' % ('Nokia', name, rec['type_code'] or '?', rec['sw_version'] or '?',
                                                 rec['date'], rec['confidence']))
    if r:
        print('  registry: %s%s' % (r.get('status'), ', model ' + r['model'] if r.get('model') else ''))
    else:
        print('  registry: not listed (add a row to models/registry.toml)')
    print('  profiles with this firmware: %s' % (', '.join(rec['profiles']) or 'none'))
    for p in rec['products']:
        print('  product %s %s (%s)' % (p['product_code'], p['description'], p['vpl']))
    for f in rec['files']:
        print('    %-7s %-6s %s%s' % (f.get('role', '?'), f.get('id', ''), f['name'],
                                     '  -- ' + f['problem'] if f.get('problem') else ''))
    for p in rec['problems']:
        print('  PROBLEM: %s' % p)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    recs = identify(a.paths)
    if a.json:
        json.dump(recs, sys.stdout, indent=1)
        print()
    else:
        for r in recs:
            show(r)
    return 0 if recs else 1


if __name__ == '__main__':
    sys.exit(main())
